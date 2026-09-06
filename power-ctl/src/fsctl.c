#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <openssl/evp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <systemd/sd-daemon.h>
#include <systemd/sd-bus.h>
#include <time.h>
#include <unistd.h>

#include "hash.h"
#include "storage.h"

#define PROGRAM_NAME "fsctl"
#define FOOTER_AREA_SIZE 4096UL
#define DESCRIPTOR_SIZE 128U

#define DESC_MAGIC_OFFSET 0U
#define DESC_VERSION_OFFSET 8U
#define DESC_DESCRIPTOR_SIZE_OFFSET 12U
#define DESC_HASH_ALGORITHM_OFFSET 16U
#define DESC_DIGEST_LEN_OFFSET 20U
#define DESC_HASHED_SIZE_OFFSET 24U
#define DESC_DIGEST_OFFSET 32U
#define DESC_CRC32C_OFFSET 64U
#define DESC_RESERVED_OFFSET 68U

#define PROTOCOL_VERSION 1U
#define HASH_ALGORITHM_SHA256 1U
#define SHA256_DIGEST_SIZE 32U

#define APP_PARAM_FS "vfat"
#define APP_PARAM_MOUNT_FLAGS (MS_NOATIME | MS_NOSUID | MS_NODEV)
#define APP_PARAM_MOUNT_OPTS NULL
#define PAGECACHE_WRITEBACK_DELAY 0
#define PAGECACHE_BACKGROUND_RATIO 50
#define PAGECACHE_DIRTY_RATIO 50
#define APP_READY_TIMEOUT_USEC (60ULL * 1000000ULL)

static const uint8_t descriptor_magic[8] = {
    'V', 'F', 'I', 'N', 'T', 'E', 'G', '\0'
};

enum exit_code {
    EXIT_OK = 0,
    EXIT_HASH_MISMATCH = 2,
    EXIT_METADATA_INVALID = 3,
    EXIT_IO_ERROR = 4,
    EXIT_DEVICE_BUSY = 5,
    EXIT_NOMEM = 6,
    EXIT_UNSUPPORTED_LAYOUT = 7
};

struct target {
    int fd;
    uint64_t size;
};

struct dirty_settings {
    int writeback_centisecs;
    int background_ratio;
    int dirty_ratio;
};

struct app_param_state {
    struct dirty_settings original;
    char *mounted_dev;
    char *other_dev;
    uint8_t mounted_digest[SHA256_DIGEST_SIZE];
};

static int verify_path(const char *path, int chunk_kib);
static int seal_path(const char *path, int chunk_kib);
static int verify_path_digest(const char *path, int chunk_kib,
                              uint8_t digest[SHA256_DIGEST_SIZE]);

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t get_le64(const uint8_t *p)
{
    return (uint64_t)get_le32(p) | ((uint64_t)get_le32(p + 4) << 32);
}

static int read_sysctl_int(const char *path, int *value)
{
    FILE *file = fopen(path, "r");
    int result;

    if (file == NULL) {
        return -1;
    }
    result = fscanf(file, "%d", value) == 1 ? 0 : -1;
    fclose(file);
    return result;
}

static int write_sysctl_int(const char *path, int value)
{
    FILE *file = fopen(path, "w");
    int result;

    if (file == NULL) {
        return -1;
    }
    result = fprintf(file, "%d\n", value) < 0 ? -1 : 0;
    if (fclose(file) != 0) {
        result = -1;
    }
    return result;
}

static int read_dirty_settings(struct dirty_settings *settings)
{
    return read_sysctl_int("/proc/sys/vm/dirty_writeback_centisecs",
                           &settings->writeback_centisecs) != 0 ||
           read_sysctl_int("/proc/sys/vm/dirty_background_ratio",
                           &settings->background_ratio) != 0 ||
           read_sysctl_int("/proc/sys/vm/dirty_ratio",
                           &settings->dirty_ratio) != 0 ? -1 : 0;
}

static int write_dirty_settings(const struct dirty_settings *settings)
{
    int first_errno = 0;

    if (write_sysctl_int("/proc/sys/vm/dirty_writeback_centisecs",
                         settings->writeback_centisecs) != 0)
        first_errno = errno != 0 ? errno : EIO;
    if (write_sysctl_int("/proc/sys/vm/dirty_background_ratio",
                         settings->background_ratio) != 0 &&
        first_errno == 0)
        first_errno = errno != 0 ? errno : EIO;
    if (write_sysctl_int("/proc/sys/vm/dirty_ratio",
                         settings->dirty_ratio) != 0 &&
        first_errno == 0)
        first_errno = errno != 0 ? errno : EIO;

    if (first_errno != 0) {
        errno = first_errno;
        return -1;
    }
    return 0;
}

static int get_unit_active_state(sd_bus *bus, const char *unit,
                                 char **active_state)
{
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    const char *unit_path;
    int result;

    *active_state = NULL;
    result = sd_bus_call_method(bus,
                                "org.freedesktop.systemd1",
                                "/org/freedesktop/systemd1",
                                "org.freedesktop.systemd1.Manager",
                                "GetUnit",
                                &error,
                                &reply,
                                "s",
                                unit);
    if (result >= 0) {
        result = sd_bus_message_read(reply, "o", &unit_path);
    }
    if (result >= 0) {
        result = sd_bus_get_property_string(
            bus,
            "org.freedesktop.systemd1",
            unit_path,
            "org.freedesktop.systemd1.Unit",
            "ActiveState",
            &error,
            active_state);
    }
    if (result < 0) {
        fprintf(stderr, "%s: cannot query %s state: %s\n",
                PROGRAM_NAME, unit, error.message != NULL ?
                error.message : strerror(-result));
    }
    sd_bus_message_unref(reply);
    sd_bus_error_free(&error);
    return result;
}

static int release_app_services(void)
{
    static const char *const app_services[] = {
        "sensor_center.service",
        "video_functions.service",
    };
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus *bus = NULL;
    bool completed[sizeof(app_services) / sizeof(app_services[0])] = {false};
    size_t i;
    struct timespec start_time;
    struct timespec now;
    bool timed_out = false;
    int result;

    result = sd_bus_open_system(&bus);
    if (result < 0) {
        fprintf(stderr, "%s: cannot connect to system bus: %s\n",
                PROGRAM_NAME, strerror(-result));
        return -1;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &start_time) != 0) {
        fprintf(stderr, "%s: cannot read monotonic clock: %s\n",
                PROGRAM_NAME, strerror(errno));
        sd_bus_error_free(&error);
        sd_bus_unref(bus);
        return -1;
    }

    for (;;) {
        bool all_finished = true;

        for (i = 0; i < sizeof(app_services) / sizeof(app_services[0]); ++i) {
            char *active_state = NULL;

            if (completed[i]) {
                continue;
            }
            result = get_unit_active_state(bus, app_services[i],
                                           &active_state);
            if (result < 0) {
                all_finished = false;
                continue;
            }
            if (strcmp(active_state, "failed") == 0) {
                fprintf(stderr, "%s: app service failed, skip SIGUSR2: %s\n",
                        PROGRAM_NAME, app_services[i]);
                completed[i] = true;
                free(active_state);
                continue;
            }
            if (strcmp(active_state, "active") == 0) {
                result = sd_bus_call_method(
                    bus,
                    "org.freedesktop.systemd1",
                    "/org/freedesktop/systemd1",
                    "org.freedesktop.systemd1.Manager",
                    "KillUnit",
                    &error,
                    NULL,
                    "ssi",
                    app_services[i],
                    "all",
                    SIGUSR2);
                if (result < 0) {
                    fprintf(stderr, "%s: cannot send SIGUSR2 to %s: %s\n",
                            PROGRAM_NAME, app_services[i],
                            error.message != NULL ? error.message :
                            strerror(-result));
                    sd_bus_error_free(&error);
                    free(active_state);
                    sd_bus_unref(bus);
                    return -1;
                }
                printf("APP_PARAM_RELEASE_OK service=%s signal=SIGUSR2\n",
                       app_services[i]);
                  completed[i] = true;
                free(active_state);
                continue;
            }
            all_finished = false;
            free(active_state);
        }

        if (all_finished) {
            break;
        }
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
            fprintf(stderr, "%s: cannot read monotonic clock: %s\n",
                    PROGRAM_NAME, strerror(errno));
            sd_bus_error_free(&error);
            sd_bus_unref(bus);
            return -1;
        }
        {
            uint64_t elapsed =
                (uint64_t)(now.tv_sec - start_time.tv_sec) * 1000000ULL;
            if (now.tv_nsec >= start_time.tv_nsec) {
                elapsed += (uint64_t)(now.tv_nsec - start_time.tv_nsec) /
                           1000ULL;
            } else {
                elapsed -= 1000000ULL;
                elapsed += (uint64_t)(1000000000L + now.tv_nsec -
                                      start_time.tv_nsec) /
                           1000ULL;
            }
            if (elapsed >= APP_READY_TIMEOUT_USEC) {
                timed_out = true;
                break;
            }
        }
        usleep(100000);
    }

    if (timed_out) {
        fprintf(stderr,
                "%s: app services did not finish startup within %llu seconds; "
                "skip remaining SIGUSR2\n",
                PROGRAM_NAME,
                (unsigned long long)(APP_READY_TIMEOUT_USEC / 1000000ULL));
    }

    sd_bus_error_free(&error);
    sd_bus_unref(bus);
    return 0;
}

static int enable_pagecache_delay(void)
{
    const struct dirty_settings delayed = {
        .writeback_centisecs = PAGECACHE_WRITEBACK_DELAY,
        .background_ratio = PAGECACHE_BACKGROUND_RATIO,
        .dirty_ratio = PAGECACHE_DIRTY_RATIO,
    };
    return write_dirty_settings(&delayed);
}

static int pread_full(int fd, void *buffer, size_t length, uint64_t offset)
{
    uint8_t *p = buffer;
    size_t done = 0;

    while (done < length) {
        ssize_t count = pread(fd, p + done, length - done,
                              (off_t)(offset + done));
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (count == 0) {
            errno = EIO;
            return -1;
        }
        done += (size_t)count;
    }
    return 0;
}

static void decode_mountinfo_field(char *text)
{
    char *src = text;
    char *dst = text;

    while (*src != '\0') {
        if (src[0] == '\\' &&
            src[1] >= '0' && src[1] <= '7' &&
            src[2] >= '0' && src[2] <= '7' &&
            src[3] >= '0' && src[3] <= '7') {
            *dst++ = (char)(((src[1] - '0') << 6) |
                           ((src[2] - '0') << 3) |
                           (src[3] - '0'));
            src += 4;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

/* Returns 1 for an exact mountpoint, 0 for not mounted, -1 on error.
 * When resolved_path is non-NULL and the path resolves successfully, ownership
 * of the allocated canonical path is returned to the caller. */
static int mountpoint_status(const char *path, char **resolved_path)
{
    char *resolved;
    FILE *mountinfo;
    char *line = NULL;
    size_t capacity = 0;
    int found = 0;
    int saved_errno = 0;

    resolved = realpath(path, NULL);
    if (resolved == NULL) {
        return -1;
    }

    mountinfo = fopen("/proc/self/mountinfo", "r");
    if (mountinfo == NULL) {
        saved_errno = errno;
        free(resolved);
        errno = saved_errno;
        return -1;
    }

    while (getline(&line, &capacity, mountinfo) >= 0) {
        char *save = NULL;
        char *field;
        unsigned int index = 0;

        for (field = strtok_r(line, " ", &save);
             field != NULL && index < 5U;
             field = strtok_r(NULL, " ", &save), ++index) {
            if (index == 4U) {
                decode_mountinfo_field(field);
                if (strcmp(field, resolved) == 0) {
                    found = 1;
                }
                break;
            }
        }
        if (found) {
            break;
        }
    }

    if (ferror(mountinfo)) {
        saved_errno = errno != 0 ? errno : EIO;
        found = -1;
    }

    free(line);
    fclose(mountinfo);

    if (found >= 0 && resolved_path != NULL) {
        *resolved_path = resolved;
    } else {
        free(resolved);
    }
    if (saved_errno != 0) {
        errno = saved_errno;
    }
    return found;
}

static int umount_path(const char *path)
{
    char *mountpoint = NULL;
    int mount_status;
    int fd;

    mount_status = mountpoint_status(path, &mountpoint);
    if (mount_status < 0) {
        fprintf(stderr, "%s: cannot inspect mountpoint '%s': %s\n",
                PROGRAM_NAME, path, strerror(errno));
        return EXIT_IO_ERROR;
    }
    if (mount_status == 0) {
        printf("UMOUNT_OK mountpoint=%s already_unmounted=1\n", mountpoint);
        free(mountpoint);
        return EXIT_OK;
    }

    fd = open(mountpoint, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "%s: cannot open mountpoint '%s': %s\n",
                PROGRAM_NAME, mountpoint, strerror(errno));
        free(mountpoint);
        return EXIT_IO_ERROR;
    }

    if (syncfs(fd) != 0) {
        fprintf(stderr, "%s: syncfs failed for '%s': %s\n",
                PROGRAM_NAME, mountpoint, strerror(errno));
        close(fd);
        free(mountpoint);
        return EXIT_IO_ERROR;
    }
    if (close(fd) != 0) {
        fprintf(stderr, "%s: close failed for mountpoint '%s': %s\n",
                PROGRAM_NAME, mountpoint, strerror(errno));
        free(mountpoint);
        return EXIT_IO_ERROR;
    }

    if (umount2(mountpoint, 0) != 0) {
        int code = errno == EBUSY ? EXIT_DEVICE_BUSY : EXIT_IO_ERROR;
        fprintf(stderr, "%s: cannot unmount '%s': %s\n",
                PROGRAM_NAME, mountpoint, strerror(errno));
        free(mountpoint);
        return code;
    }

    mount_status = mountpoint_status(mountpoint, NULL);
    if (mount_status < 0) {
        fprintf(stderr, "%s: cannot confirm unmount of '%s': %s\n",
                PROGRAM_NAME, mountpoint, strerror(errno));
        free(mountpoint);
        return EXIT_IO_ERROR;
    }
    if (mount_status != 0) {
        fprintf(stderr, "%s: mountpoint '%s' is still mounted\n",
                PROGRAM_NAME, mountpoint);
        free(mountpoint);
        return EXIT_DEVICE_BUSY;
    }

    printf("UMOUNT_OK mountpoint=%s already_unmounted=0\n", mountpoint);
    free(mountpoint);
    return EXIT_OK;
}

static int split_devices(const char *spec, char **first, char **second)
{
    const char *separator = strchr(spec, ':');
    size_t first_len;

    if (separator == NULL || separator == spec || separator[1] == '\0') {
        errno = EINVAL;
        return -1;
    }
    first_len = (size_t)(separator - spec);
    *first = strndup(spec, first_len);
    *second = strdup(separator + 1);
    if (*first == NULL || *second == NULL) {
        free(*first);
        free(*second);
        *first = NULL;
        *second = NULL;
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

static int mount_app_param(struct app_param_state *state,
                           const char *devices,
                           const char *mountpoint,
                           int chunk_kib)
{
    char *device_a = NULL;
    char *device_b = NULL;
    const char *candidates[2];
    size_t i;
    int result;

    if (split_devices(devices, &device_a, &device_b) != 0) {
        fprintf(stderr, "%s: invalid device pair '%s', expected A:B\n",
                PROGRAM_NAME, devices);
        return EXIT_UNSUPPORTED_LAYOUT;
    }
    candidates[0] = device_a;
    candidates[1] = device_b;
    if (read_dirty_settings(&state->original) != 0) {
        fprintf(stderr, "%s: cannot read dirty settings: %s\n",
                PROGRAM_NAME, strerror(errno));
        free(device_a);
        free(device_b);
        return EXIT_IO_ERROR;
    }
    if (enable_pagecache_delay() != 0) {
        int saved_errno = errno;

        fprintf(stderr, "%s: cannot configure page-cache delay: %s\n",
                PROGRAM_NAME, strerror(saved_errno));
        if (write_dirty_settings(&state->original) != 0)
            fprintf(stderr,
                    "%s: cannot restore dirty settings after setup failure: %s\n",
                    PROGRAM_NAME, strerror(errno));
        free(device_a);
        free(device_b);
        errno = saved_errno;
        return EXIT_IO_ERROR;
    }

    if (mkdir(mountpoint, 0755) != 0 && errno != EEXIST) {
        result = EXIT_IO_ERROR;
        goto fail_restore_dirty;
    }

    for (i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        result = verify_path_digest(candidates[i], chunk_kib,
                        state->mounted_digest);
        if (result != EXIT_OK) {
            fprintf(stderr, "%s: %s hash check invalid\n",
                    PROGRAM_NAME, candidates[i]);
            continue;
        }

        if (mount(candidates[i], mountpoint, APP_PARAM_FS,
                APP_PARAM_MOUNT_FLAGS, APP_PARAM_MOUNT_OPTS) != 0) {
            fprintf(stderr, "%s: cannot mount %s: %s\n",
                    PROGRAM_NAME, candidates[i], strerror(errno));
            continue;
        }

        state->mounted_dev = strdup(candidates[i]);
        state->other_dev = strdup(i == 0 ? candidates[1] : candidates[0]);
        if (state->mounted_dev == NULL || state->other_dev == NULL) {
            result = EXIT_NOMEM;
            goto fail_restore_dirty;
        }
        printf("%s is mounted, backup is %s\n",
               state->mounted_dev, state->other_dev);
        if (sd_notify(0, "READY=1\nSTATUS=app_param mounted") < 0) {
            fprintf(stderr, "%s: sd_notify READY failed\n", PROGRAM_NAME);
            result = EXIT_IO_ERROR;
            goto fail_restore_dirty;
        }
        free(device_a);
        free(device_b);
        return EXIT_OK;
    }
    result = EXIT_METADATA_INVALID;

fail_restore_dirty:
    if (write_dirty_settings(&state->original) != 0)
        fprintf(stderr,
                "%s: cannot restore dirty settings after mount failure: %s\n",
                PROGRAM_NAME, strerror(errno));
    free(device_a);
    free(device_b);
    return result;
}

static int recover_app_param(struct app_param_state *state,
                             const char *mountpoint,
                             int chunk_kib)
{
    uint8_t other_digest[SHA256_DIGEST_SIZE];
    int result = EXIT_OK;
    int other_result;

    (void)mountpoint;
    other_result = verify_path_digest(state->other_dev, chunk_kib,
                                      other_digest);

    if (other_result != EXIT_OK ||
        memcmp(state->mounted_digest, other_digest,
               SHA256_DIGEST_SIZE) != 0) {
        char parent[PATH_MAX];
        uint64_t source_offset;
        uint64_t partition_size;

        if (powerctl_resolve_partition_source(state->mounted_dev, parent,
                                              sizeof(parent), &source_offset,
                                              &partition_size) != 0) {
            fprintf(stderr, "%s: cannot resolve recovery source %s: %s\n",
                    PROGRAM_NAME, state->mounted_dev, strerror(errno));
            result = EXIT_IO_ERROR;
            goto out_restore;
        }
        printf("start recover backup %s from %s offset=%" PRIu64
               " size=%" PRIu64 "\n",
               state->other_dev, parent, source_offset, partition_size);
        if (powerctl_copy_block_range(parent, source_offset, partition_size,
                                      state->other_dev) != 0) {
            fprintf(stderr, "%s: cannot copy app_param backup: %s\n",
                    PROGRAM_NAME, strerror(errno));
            result = EXIT_IO_ERROR;
            goto out_restore;
        }
        other_result = verify_path_digest(state->other_dev, chunk_kib,
                                          other_digest);
        if (other_result != EXIT_OK ||
            memcmp(state->mounted_digest, other_digest,
                   SHA256_DIGEST_SIZE) != 0) {
            fprintf(stderr, "%s: recovered peer verify failed: %s\n",
                    PROGRAM_NAME, state->other_dev);
            result = other_result == EXIT_OK ? EXIT_HASH_MISMATCH : other_result;
            goto out_restore;
        }
        printf("backup %s is recovered and verified\n", state->other_dev);
    }

    if (release_app_services() != 0) {
        result = EXIT_IO_ERROR;
        goto out_restore;
    }

out_restore:
    if (write_dirty_settings(&state->original) != 0) {
        fprintf(stderr, "%s: cannot restore dirty settings: %s\n",
                PROGRAM_NAME, strerror(errno));
        if (result == EXIT_OK)
            result = EXIT_IO_ERROR;
    }
    return result;
}

static int open_target(const char *path, bool writable, struct target *target)
{
    struct stat st;
    int flags = (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC | O_DIRECT;
    int fd;

    memset(target, 0, sizeof(*target));
    target->fd = -1;

    /* O_EXCL has block-device semantics only, so reject other target types
     * before opening and confirm the opened object with fstat(). */
    if (stat(path, &st) != 0) {
        fprintf(stderr, "%s: cannot stat '%s': %s\n", PROGRAM_NAME, path,
                strerror(errno));
        return EXIT_IO_ERROR;
    }
    if (!S_ISBLK(st.st_mode)) {
        fprintf(stderr, "%s: '%s' is not a block device\n", PROGRAM_NAME,
                path);
        return EXIT_UNSUPPORTED_LAYOUT;
    }

    fd = open(path, flags);
    if (fd < 0) {
        int code = errno == EBUSY ? EXIT_DEVICE_BUSY : EXIT_IO_ERROR;
        fprintf(stderr, "%s: cannot open '%s': %s\n", PROGRAM_NAME, path,
                strerror(errno));
        return code;
    }

    if (fstat(fd, &st) != 0) {
        fprintf(stderr, "%s: cannot inspect '%s': %s\n", PROGRAM_NAME, path,
                strerror(errno));
        close(fd);
        return EXIT_IO_ERROR;
    }
    if (!S_ISBLK(st.st_mode)) {
        fprintf(stderr, "%s: target type changed while opening '%s'\n",
                PROGRAM_NAME, path);
        close(fd);
        return EXIT_IO_ERROR;
    }

    target->fd = fd;
    {
        unsigned long long bytes = 0;
        int logical_size = 0;
        uint32_t logical;

        if (ioctl(fd, BLKGETSIZE64, &bytes) != 0) {
            fprintf(stderr, "%s: BLKGETSIZE64 failed for '%s': %s\n",
                    PROGRAM_NAME, path, strerror(errno));
            close(fd);
            target->fd = -1;
            return EXIT_IO_ERROR;
        }
        if (ioctl(fd, BLKSSZGET, &logical_size) != 0 || logical_size <= 0) {
            fprintf(stderr, "%s: BLKSSZGET failed for '%s': %s\n",
                    PROGRAM_NAME, path, strerror(errno));
            close(fd);
            target->fd = -1;
            return EXIT_IO_ERROR;
        }
        target->size = (uint64_t)bytes;
        logical = (uint32_t)logical_size;

        if (target->size <= FOOTER_AREA_SIZE ||
            target->size > (uint64_t)INT64_MAX) {
            fprintf(stderr,
                    "%s: unsupported target size for '%s': %" PRIu64 "\n",
                    PROGRAM_NAME, path, target->size);
            close(fd);
            target->fd = -1;
            return EXIT_UNSUPPORTED_LAYOUT;
        }

        if ((FOOTER_AREA_SIZE % logical) != 0U ||
            (target->size % logical) != 0U) {
            fprintf(stderr,
                    "%s: 4 KiB footer/target is not aligned to logical block size %u\n",
                    PROGRAM_NAME, logical);
            close(fd);
            target->fd = -1;
            return EXIT_UNSUPPORTED_LAYOUT;
        }
    }

    return EXIT_OK;
}

static void close_target(struct target *target)
{
    if (target->fd >= 0) {
        close(target->fd);
        target->fd = -1;
    }
}

/* Footer payload hashing is fixed to SHA-256 by the descriptor protocol. */
static int hash_region(int fd, uint64_t length, int chunk_kib,
                       uint8_t digest[SHA256_DIGEST_SIZE])
{
    return hash_sha256_fd(fd, length, digest, chunk_kib) == 0
           ? EXIT_OK : EXIT_IO_ERROR;
}

static void digest_to_hex(const uint8_t digest[SHA256_DIGEST_SIZE],
                          char output[(SHA256_DIGEST_SIZE * 2U) + 1U])
{
    static const char digits[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < SHA256_DIGEST_SIZE; ++i) {
        output[i * 2U] = digits[digest[i] >> 4];
        output[i * 2U + 1U] = digits[digest[i] & 0x0fU];
    }
    output[SHA256_DIGEST_SIZE * 2U] = '\0';
}

static int validate_descriptor(const uint8_t descriptor[DESCRIPTOR_SIZE],
                               uint64_t expected_hashed_size,
                               uint8_t digest[SHA256_DIGEST_SIZE])
{
    uint8_t copy[DESCRIPTOR_SIZE];
    uint32_t stored_crc;
    uint32_t computed_crc;
    size_t i;

    stored_crc = get_le32(descriptor + DESC_CRC32C_OFFSET);
    memcpy(copy, descriptor, sizeof(copy));
    memset(copy + DESC_CRC32C_OFFSET, 0, sizeof(uint32_t));
    {
        uint8_t crc_digest[4];
        if (hash_crc32(copy, sizeof(copy), crc_digest) != 0)
            return EXIT_METADATA_INVALID;
        computed_crc = get_le32(crc_digest);
    }
    if (computed_crc != stored_crc) {
        fprintf(stderr, "%s: descriptor CRC32C mismatch\n", PROGRAM_NAME);
        return EXIT_METADATA_INVALID;
    }
    if (memcmp(descriptor + DESC_MAGIC_OFFSET, descriptor_magic,
               sizeof(descriptor_magic)) != 0) {
        fprintf(stderr, "%s: descriptor magic is invalid\n", PROGRAM_NAME);
        return EXIT_METADATA_INVALID;
    }
    if (get_le32(descriptor + DESC_VERSION_OFFSET) != PROTOCOL_VERSION) {
        fprintf(stderr, "%s: descriptor version is unsupported\n", PROGRAM_NAME);
        return EXIT_METADATA_INVALID;
    }
    if (get_le32(descriptor + DESC_DESCRIPTOR_SIZE_OFFSET) != DESCRIPTOR_SIZE) {
        fprintf(stderr, "%s: descriptor size is invalid\n", PROGRAM_NAME);
        return EXIT_METADATA_INVALID;
    }
    if (get_le32(descriptor + DESC_HASH_ALGORITHM_OFFSET) !=
        HASH_ALGORITHM_SHA256) {
        fprintf(stderr, "%s: descriptor hash algorithm is unsupported\n",
                PROGRAM_NAME);
        return EXIT_METADATA_INVALID;
    }
    if (get_le32(descriptor + DESC_DIGEST_LEN_OFFSET) != SHA256_DIGEST_SIZE) {
        fprintf(stderr, "%s: descriptor digest length is invalid\n",
                PROGRAM_NAME);
        return EXIT_METADATA_INVALID;
    }
    if (get_le64(descriptor + DESC_HASHED_SIZE_OFFSET) !=
        expected_hashed_size) {
        fprintf(stderr, "%s: descriptor hashed size does not match target\n",
                PROGRAM_NAME);
        return EXIT_METADATA_INVALID;
    }
    for (i = DESC_RESERVED_OFFSET; i < DESCRIPTOR_SIZE; ++i) {
        if (descriptor[i] != 0U) {
            fprintf(stderr, "%s: descriptor reserved bytes are nonzero\n",
                    PROGRAM_NAME);
            return EXIT_METADATA_INVALID;
        }
    }
    memcpy(digest, descriptor + DESC_DIGEST_OFFSET, SHA256_DIGEST_SIZE);
    return EXIT_OK;
}

static int verify_path_digest(const char *path, int chunk_kib,
                              uint8_t digest[SHA256_DIGEST_SIZE])
{
    struct target target;
    uint8_t descriptor[DESCRIPTOR_SIZE];
    uint8_t expected_digest[SHA256_DIGEST_SIZE];
    uint8_t actual_digest[SHA256_DIGEST_SIZE];
    uint64_t protected_size;
    uint64_t descriptor_offset;
    char digest_hex[(SHA256_DIGEST_SIZE * 2U) + 1U];
    int result;

    result = open_target(path, false, &target);
    if (result != EXIT_OK) {
        return result;
    }
    protected_size = target.size - FOOTER_AREA_SIZE;
    descriptor_offset = target.size - DESCRIPTOR_SIZE;

    /* Read descriptor with buffered I/O (128 bytes, not O_DIRECT aligned) */
    int saved_flags = fcntl(target.fd, F_GETFL);
    if (saved_flags >= 0 && (saved_flags & O_DIRECT))
        fcntl(target.fd, F_SETFL, saved_flags & ~O_DIRECT);
    if (pread_full(target.fd, descriptor, sizeof(descriptor),
                   descriptor_offset) != 0) {
        fprintf(stderr, "%s: cannot read descriptor: %s\n", PROGRAM_NAME,
                strerror(errno));
        close_target(&target);
        return EXIT_IO_ERROR;
    }
    if (saved_flags >= 0 && (saved_flags & O_DIRECT))
        fcntl(target.fd, F_SETFL, saved_flags);

    result = validate_descriptor(descriptor, protected_size, expected_digest);
    if (result != EXIT_OK) {
        close_target(&target);
        return result;
    }
    result = hash_region(target.fd, protected_size, chunk_kib, actual_digest);
    close_target(&target);
    if (result != EXIT_OK) {
        return result;
    }
    if (memcmp(actual_digest, expected_digest, SHA256_DIGEST_SIZE) != 0) {
        char expected_hex[(SHA256_DIGEST_SIZE * 2U) + 1U];
        char actual_hex[(SHA256_DIGEST_SIZE * 2U) + 1U];
        digest_to_hex(expected_digest, expected_hex);
        digest_to_hex(actual_digest, actual_hex);
        fprintf(stderr, "hash mismatch, expected=%s actual=%s\n", expected_hex,
                actual_hex);
        return EXIT_HASH_MISMATCH;
    }
    memcpy(digest, actual_digest, SHA256_DIGEST_SIZE);
    digest_to_hex(actual_digest, digest_hex);
    printf("verify pass, device_size=%" PRIu64 " protected_size=%" PRIu64
           " digest=%s\n", protected_size + FOOTER_AREA_SIZE, protected_size,
           digest_hex);
    return EXIT_OK;
}

static int verify_path(const char *path, int chunk_kib)
{
    uint8_t digest[SHA256_DIGEST_SIZE];
    return verify_path_digest(path, chunk_kib, digest);
}

static int seal_path(const char *path, int chunk_kib)
{
    uint8_t digest[SHA256_DIGEST_SIZE];
    uint64_t device_size;
    char digest_hex[(SHA256_DIGEST_SIZE * 2U) + 1U];

    if (powerctl_seal_block_device(path, chunk_kib, digest,
                                   &device_size) != 0)
        return EXIT_IO_ERROR;
    digest_to_hex(digest, digest_hex);
    printf("seal ok, device_size=%" PRIu64 " protected_size=%" PRIu64
           " digest=%s\n", device_size, device_size - FOOTER_AREA_SIZE,
           digest_hex);
    return EXIT_OK;
}

static void print_usage(FILE *stream)
{
    fprintf(stream,
            "Usage:\n"
            "  %s [--algo sha256] [--chunk KiB] seal TARGET\n"
            "  %s [--algo sha256] [--chunk KiB] verify TARGET\n"
            "  %s [--chunk KiB] mount A:B MOUNTPOINT\n"
            "  %s umount MOUNTPOINT\n"
            "\n"
            "seal/verify TARGET must be an unmounted Linux block device.\n"
            "--algo: compatibility option; only sha256 is supported.\n"
            "--chunk: read buffer size in KiB per ring slot (default 8192).\n"
            "umount flushes the mounted filesystem with syncfs() and performs "
            "a normal umount.\n",
            PROGRAM_NAME, PROGRAM_NAME, PROGRAM_NAME, PROGRAM_NAME);
}

int main(int argc, char **argv)
{
    int chunk_kib = 0;  /* 0 = use hash.c default (8192 KiB) */
    int cmd_idx = 1;  /* index of subcommand in argv */

    /* Parse --algo and --chunk before subcommand */
    while (cmd_idx < argc && argv[cmd_idx][0] == '-') {
        if (strcmp(argv[cmd_idx], "--algo") == 0 && cmd_idx + 1 < argc) {
            if (strcmp(argv[cmd_idx + 1], "sha256") != 0) {
                fprintf(stderr, "%s: unsupported algorithm '%s' (only sha256 is supported)\n",
                        PROGRAM_NAME, argv[cmd_idx + 1]);
                return EXIT_UNSUPPORTED_LAYOUT;
            }
            cmd_idx += 2;
        } else if (strcmp(argv[cmd_idx], "--chunk") == 0 && cmd_idx + 1 < argc) {
            chunk_kib = atoi(argv[cmd_idx + 1]);
            if (chunk_kib < 1) {
                fprintf(stderr, "%s: invalid chunk size '%s'\n",
                        PROGRAM_NAME, argv[cmd_idx + 1]);
                return EXIT_UNSUPPORTED_LAYOUT;
            }
            cmd_idx += 2;
        } else {
            break;
        }
    }

    if (argc < cmd_idx + 1) {
        print_usage(stderr);
        return EXIT_UNSUPPORTED_LAYOUT;
    }
    if (strcmp(argv[cmd_idx], "--help") == 0 || strcmp(argv[cmd_idx], "-h") == 0) {
        print_usage(stdout);
        return EXIT_OK;
    }
    if (strcmp(argv[cmd_idx], "seal") == 0 && argc == cmd_idx + 2) {
        return seal_path(argv[cmd_idx + 1], chunk_kib);
    }
    if (strcmp(argv[cmd_idx], "verify") == 0 && argc == cmd_idx + 2) {
        return verify_path(argv[cmd_idx + 1], chunk_kib);
    }
    if (strcmp(argv[cmd_idx], "mount") == 0 && argc == cmd_idx + 3) {
        printf("start mounting %s to %s\n", argv[cmd_idx + 1], argv[cmd_idx + 2]);
        struct app_param_state state = {0};
        int result = mount_app_param(&state, argv[cmd_idx + 1],
                                     argv[cmd_idx + 2], chunk_kib);
        if (result != EXIT_OK) {
            return result;
        }
        result = recover_app_param(&state, argv[cmd_idx + 2], chunk_kib);
        free(state.mounted_dev);
        free(state.other_dev);
        return result;
    }
    if (strcmp(argv[cmd_idx], "umount") == 0 && argc == cmd_idx + 2) {
        return umount_path(argv[cmd_idx + 1]);
    }

    print_usage(stderr);
    return EXIT_UNSUPPORTED_LAYOUT;
}
