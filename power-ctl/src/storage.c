#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/uio.h>
#include <unistd.h>

/* glibc and the kernel UAPI use the same RWF_* names with different types. */
#undef RWF_HIPRI
#undef RWF_DSYNC
#undef RWF_SYNC
#undef RWF_NOWAIT
#undef RWF_APPEND

#undef BLKROSET
#undef BLKROGET
#undef BLKRRPART
#undef BLKGETSIZE
#undef BLKFLSBUF
#undef BLKRASET
#undef BLKRAGET
#undef BLKFRASET
#undef BLKFRAGET
#undef BLKSECTSET
#undef BLKSECTGET
#undef BLKSSZGET
#undef BLKBSZGET
#undef BLKBSZSET
#undef BLKGETSIZE64

#include <linux/fs.h>
#include <systemd/sd-daemon.h>
#include <systemd/sd-bus.h>
#include "hash.h"
#include "storage.h"
#include "footer.h"
#include "common.h"

#define APP_PARAM_MOUNT_FLAGS (MS_NOATIME | MS_NOSUID | MS_NODEV)
#define APP_PARAM_FS "vfat"
#define APP_PARAM_MOUNT_OPTS NULL
#define APP_READY_TIMEOUT_USEC (60ULL * 1000000ULL)

struct dirty_settings {
    int writeback_centisecs;
    int background_ratio;
    int dirty_ratio;
};

struct dirty_settings original;

int open_target(const char *path, int flags, struct target *target) {
    struct stat st;
    unsigned long long bytes;
    int logical_size;

    target->fd = -1;
    target->size = 0;
    target->logical_size = 0;
    if (stat(path, &st) != 0 || !S_ISBLK(st.st_mode)) {
        errno = ENOTBLK;
        return -1;
    }
    target->fd = open(path, flags);
    if (target->fd < 0) return -1;
    if (ioctl(target->fd, BLKGETSIZE64, &bytes) != 0 ||
        ioctl(target->fd, BLKSSZGET, &logical_size) != 0 ||
        bytes <= FOOTER_AREA_SIZE || bytes > (unsigned long long)INT64_MAX ||
        logical_size <= 0 || FOOTER_AREA_SIZE % (unsigned)logical_size != 0 ||
        bytes % (unsigned)logical_size != 0) {
        close(target->fd);
        target->fd = -1;
        errno = EINVAL;
        return -1;
    }
    target->size = (uint64_t)bytes;
    target->logical_size = (unsigned int)logical_size;
    return 0;
}

void close_target(struct target *target) {
    if (target->fd >= 0) {
        close(target->fd);
        target->fd = -1;
    }
}

int powerctl_resolve_active_peer(const char *mountpoint, const char *device_a,
                                 const char *device_b, const char **active,
                                 const char **peer) {
    struct stat mount_stat;
    struct stat a_stat;
    struct stat b_stat;

    if (mountpoint == NULL || device_a == NULL || device_b == NULL ||
        active == NULL || peer == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (stat(mountpoint, &mount_stat) != 0 || stat(device_a, &a_stat) != 0 ||
        stat(device_b, &b_stat) != 0)
        return -1;
    if (!S_ISBLK(a_stat.st_mode) || !S_ISBLK(b_stat.st_mode) ||
        a_stat.st_rdev == b_stat.st_rdev) {
        errno = EINVAL;
        return -1;
    }

    if (mount_stat.st_dev == a_stat.st_rdev) {
        *active = device_a;
        *peer = device_b;
        return 0;
    }
    if (mount_stat.st_dev == b_stat.st_rdev) {
        *active = device_b;
        *peer = device_a;
        return 0;
    }

    errno = ENODEV;
    return -1;
}

int powerctl_disable_vfat_write(const char *mountpoint) {
    int fd;
    int result;

    fd = open(mountpoint, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;
    result = ioctl(fd, FAT_IOCTL_DISABLE_WRITE);
    if (result == 0)
        result = close(fd);
    else {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
    }
    return result;
}

int powerctl_seal_block_device(const char *path, int chunk_kib,
                               uint8_t digest[SHA256_DIGEST_SIZE],
                               uint64_t *device_size) {
    struct target target;
    uint8_t computed_digest[SHA256_DIGEST_SIZE];
    uint64_t protected_size;
    int result = -1;

    if (open_target(path, O_RDWR | O_CLOEXEC | O_DIRECT, &target) != 0)
        return -1;
    protected_size = target.size - FOOTER_AREA_SIZE;
    if (hash_sha256_fd(target.fd, protected_size, computed_digest, chunk_kib) ==
        0) {
        result = write_footer(target.fd, protected_size, computed_digest);
        if (result == 0) {
            if (digest != NULL)
                memcpy(digest, computed_digest, sizeof(computed_digest));
            if (device_size != NULL) *device_size = target.size;
        }
    }
    close_target(&target);
    return result;
}

int powerctl_resolve_partition_source(const char *partition, char *parent,
                                      size_t parent_size,
                                      uint64_t *source_offset,
                                      uint64_t *partition_size) {
    struct stat st;
    char sys_path[PATH_MAX];
    char resolved[PATH_MAX];
    char start_path[PATH_MAX];
    char *slash;
    char *parent_name;
    unsigned long long start_sector;
    unsigned long long bytes;
    FILE *file;
    int fd;

    if (partition == NULL || parent == NULL || parent_size == 0 ||
        source_offset == NULL || partition_size == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (stat(partition, &st) != 0 || !S_ISBLK(st.st_mode)) {
        errno = ENOTBLK;
        return -1;
    }
    if (snprintf(sys_path, sizeof(sys_path), "/sys/dev/block/%u:%u",
                 major(st.st_rdev),
                 minor(st.st_rdev)) >= (int)sizeof(sys_path) ||
        realpath(sys_path, resolved) == NULL)
        return -1;

    slash = strrchr(resolved, '/');
    if (slash == NULL || slash == resolved) {
        errno = EINVAL;
        return -1;
    }
    *slash = '\0';
    parent_name = strrchr(resolved, '/');
    if (parent_name == NULL || parent_name[1] == '\0') {
        errno = EINVAL;
        return -1;
    }
    ++parent_name;
    if (snprintf(parent, parent_size, "/dev/%s", parent_name) >=
        (int)parent_size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (snprintf(start_path, sizeof(start_path), "%s/start", sys_path) >=
        (int)sizeof(start_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    file = fopen(start_path, "r");
    if (file == NULL) return -1;
    if (fscanf(file, "%llu", &start_sector) != 1) {
        fclose(file);
        errno = EINVAL;
        return -1;
    }
    fclose(file);

    fd = open(partition, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    if (ioctl(fd, BLKGETSIZE64, &bytes) != 0) {
        int saved_errno = errno;
        close(fd);
        errno = saved_errno;
        return -1;
    }
    close(fd);

    if (start_sector > UINT64_MAX / 512ULL) {
        errno = EOVERFLOW;
        return -1;
    }
    *source_offset = (uint64_t)start_sector * 512ULL;
    *partition_size = (uint64_t)bytes;
    return 0;
}

int powerctl_copy_block_range(const char *source, uint64_t source_offset,
                              uint64_t length, const char *destination) {
    struct target source_target;
    struct target destination_target;
    void *buffer = NULL;
    const size_t buffer_size = 1024U * 1024U;
    uint64_t offset = 0;
    size_t alignment;
    int result = -1;

    if (source == NULL || destination == NULL ||
        strcmp(source, destination) == 0 || length == 0) {
        errno = EINVAL;
        return -1;
    }
    if (open_target(source, O_RDONLY | O_CLOEXEC | O_DIRECT, &source_target) !=
        0)
        return -1;
    if (open_target(destination, O_RDWR | O_CLOEXEC | O_DIRECT,
                    &destination_target) != 0)
        goto out_source;
    if (destination_target.size != length ||
        source_offset > source_target.size ||
        length > source_target.size - source_offset ||
        source_offset % source_target.logical_size != 0 ||
        length % source_target.logical_size != 0 ||
        length % destination_target.logical_size != 0) {
        errno = EINVAL;
        goto out;
    }
    alignment = source_target.logical_size > destination_target.logical_size
                    ? source_target.logical_size
                    : destination_target.logical_size;
    if (alignment < 4096U) alignment = 4096U;
    if (posix_memalign(&buffer, alignment, buffer_size) != 0) {
        errno = ENOMEM;
        goto out;
    }
    while (offset < length) {
        size_t count = buffer_size;
        ssize_t read_count;
        ssize_t written;

        if (length - offset < count) count = (size_t)(length - offset);
        read_count = pread(source_target.fd, buffer, count,
                           (off_t)(source_offset + offset));
        if (read_count != (ssize_t)count) goto out;
        written = pwrite(destination_target.fd, buffer, count, (off_t)offset);
        if (written != (ssize_t)count) goto out;
        offset += count;
    }
    result = fsync(destination_target.fd);
out:
    free(buffer);
    close_target(&destination_target);
out_source:
    close_target(&source_target);
    return result;
}

int powerctl_copy_block_device(const char *source, const char *destination) {
    struct target source_target;
    uint64_t source_size;

    if (open_target(source, O_RDONLY | O_CLOEXEC | O_DIRECT, &source_target) !=
        0)
        return -1;
    source_size = source_target.size;
    close_target(&source_target);
    return powerctl_copy_block_range(source, 0, source_size, destination);
}

static void decode_mountinfo_field(char *text) {
    char *src = text;
    char *dst = text;

    while (*src != '\0') {
        if (src[0] == '\\' && src[1] >= '0' && src[1] <= '7' && src[2] >= '0' &&
            src[2] <= '7' && src[3] >= '0' && src[3] <= '7') {
            *dst++ = (char)(((src[1] - '0') << 6) | ((src[2] - '0') << 3) |
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
static int mountpoint_status(const char *path, char **resolved_path) {
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

        for (field = strtok_r(line, " ", &save); field != NULL && index < 5U;
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

int umount_partition(const char *path) {
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
        fprintf(stderr, "%s: cannot open mountpoint '%s': %s\n", PROGRAM_NAME,
                mountpoint, strerror(errno));
        free(mountpoint);
        return EXIT_IO_ERROR;
    }

    if (syncfs(fd) != 0) {
        fprintf(stderr, "%s: syncfs failed for '%s': %s\n", PROGRAM_NAME,
                mountpoint, strerror(errno));
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
        fprintf(stderr, "%s: cannot unmount '%s': %s\n", PROGRAM_NAME,
                mountpoint, strerror(errno));
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
        fprintf(stderr, "%s: mountpoint '%s' is still mounted\n", PROGRAM_NAME,
                mountpoint);
        free(mountpoint);
        return EXIT_DEVICE_BUSY;
    }

    printf("UMOUNT_OK mountpoint=%s already_unmounted=0\n", mountpoint);
    free(mountpoint);
    return EXIT_OK;
}
static int read_sysctl_int(const char *path, int *value) {
    FILE *file = fopen(path, "r");
    int result;

    if (file == NULL) {
        return -1;
    }
    result = fscanf(file, "%d", value) == 1 ? 0 : -1;
    fclose(file);
    return result;
}

static int write_sysctl_int(const char *path, int value) {
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

static int read_dirty_settings(struct dirty_settings *settings) {
    return read_sysctl_int("/proc/sys/vm/dirty_writeback_centisecs",
                           &settings->writeback_centisecs) != 0 ||
                   read_sysctl_int("/proc/sys/vm/dirty_background_ratio",
                                   &settings->background_ratio) != 0 ||
                   read_sysctl_int("/proc/sys/vm/dirty_ratio",
                                   &settings->dirty_ratio) != 0
               ? -1
               : 0;
}

static int write_dirty_settings(const struct dirty_settings *settings) {
    int first_errno = 0;

    if (write_sysctl_int("/proc/sys/vm/dirty_writeback_centisecs",
                         settings->writeback_centisecs) != 0)
        first_errno = errno != 0 ? errno : EIO;
    if (write_sysctl_int("/proc/sys/vm/dirty_background_ratio",
                         settings->background_ratio) != 0 &&
        first_errno == 0)
        first_errno = errno != 0 ? errno : EIO;
    if (write_sysctl_int("/proc/sys/vm/dirty_ratio", settings->dirty_ratio) !=
            0 &&
        first_errno == 0)
        first_errno = errno != 0 ? errno : EIO;

    if (first_errno != 0) {
        errno = first_errno;
        return -1;
    }
    return 0;
}

static int enable_pagecache_delay(void) {
    const struct dirty_settings delayed = {
        .writeback_centisecs = 6000,  // 60 seconds
        .background_ratio = 80,
        .dirty_ratio = 90,
    };
    return write_dirty_settings(&delayed);
}

static int split_devices(const char *spec, char **first, char **second) {
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

int powerctl_verify_path_digest(const char *path, int chunk_kib,
                                uint8_t digest[32]) {
    struct target target;
    uint8_t descriptor[DESCRIPTOR_SIZE];
    uint8_t expected_digest[32];
    uint8_t actual_digest[32];
    uint64_t protected_size;
    uint64_t descriptor_offset;
    char digest_hex[(32 * 2U) + 1U];
    int result;

    result = open_target(path, O_RDONLY | O_CLOEXEC | O_DIRECT, &target);
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
    result =
        hash_sha256_fd(target.fd, protected_size, actual_digest, chunk_kib);
    close_target(&target);
    if (result != 0) {
        return result;
    }
    if (memcmp(actual_digest, expected_digest, 32) != 0) {
        char expected_hex[(32 * 2U) + 1U];
        char actual_hex[(32 * 2U) + 1U];
        hash_to_hex(expected_digest, expected_hex);
        hash_to_hex(actual_digest, actual_hex);
        fprintf(stderr, "hash mismatch, expected=%s actual=%s\n", expected_hex,
                actual_hex);
        return EXIT_HASH_MISMATCH;
    }
    memcpy(digest, actual_digest, 32);
    hash_to_hex(actual_digest, digest_hex);
    printf("verify pass, device_size=%" PRIu64 " protected_size=%" PRIu64
           " digest=%s\n",
           protected_size + FOOTER_AREA_SIZE, protected_size, digest_hex);
    return EXIT_OK;
}

int mount_partition(struct app_param_state *state, const char *devices,
                    const char *mountpoint, int chunk_kib) {
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
    if (read_dirty_settings(&original) != 0) {
        fprintf(stderr, "%s: cannot read dirty settings: %s\n", PROGRAM_NAME,
                strerror(errno));
        free(device_a);
        free(device_b);
        return EXIT_IO_ERROR;
    }

    if (enable_pagecache_delay() != 0) {
        int saved_errno = errno;

        fprintf(stderr, "%s: cannot configure page-cache delay: %s\n",
                PROGRAM_NAME, strerror(saved_errno));
        if (write_dirty_settings(&original) != 0)
            fprintf(
                stderr,
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
        result = powerctl_verify_path_digest(candidates[i], chunk_kib,
                                             state->mounted_digest);
        if (result != EXIT_OK) {
            fprintf(stderr, "%s: %s hash check invalid\n", PROGRAM_NAME,
                    candidates[i]);
            continue;
        }

        if (mount(candidates[i], mountpoint, APP_PARAM_FS,
                  APP_PARAM_MOUNT_FLAGS, APP_PARAM_MOUNT_OPTS) != 0) {
            fprintf(stderr, "%s: cannot mount %s: %s\n", PROGRAM_NAME,
                    candidates[i], strerror(errno));
            continue;
        }

        state->mounted_dev = strdup(candidates[i]);
        state->other_dev = strdup(i == 0 ? candidates[1] : candidates[0]);
        if (state->mounted_dev == NULL || state->other_dev == NULL) {
            result = EXIT_NOMEM;
            goto fail_restore_dirty;
        }
        printf("%s is mounted, backup is %s\n", state->mounted_dev,
               state->other_dev);
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
    if (write_dirty_settings(&original) != 0)
        fprintf(stderr,
                "%s: cannot restore dirty settings after mount failure: %s\n",
                PROGRAM_NAME, strerror(errno));
    free(device_a);
    free(device_b);
    return result;
}

static int get_unit_active_state(sd_bus *bus, const char *unit,
                                 char **active_state) {
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    const char *unit_path;
    int result;

    *active_state = NULL;
    result = sd_bus_call_method(bus, "org.freedesktop.systemd1",
                                "/org/freedesktop/systemd1",
                                "org.freedesktop.systemd1.Manager", "GetUnit",
                                &error, &reply, "s", unit);
    if (result >= 0) {
        result = sd_bus_message_read(reply, "o", &unit_path);
    }
    if (result >= 0) {
        result = sd_bus_get_property_string(
            bus, "org.freedesktop.systemd1", unit_path,
            "org.freedesktop.systemd1.Unit", "ActiveState", &error,
            active_state);
    }
    if (result < 0) {
        fprintf(stderr, "%s: cannot query %s state: %s\n", PROGRAM_NAME, unit,
                error.message != NULL ? error.message : strerror(-result));
    }
    sd_bus_message_unref(reply);
    sd_bus_error_free(&error);
    return result;
}

static int release_app_services(void) {
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
        fprintf(stderr, "%s: cannot connect to system bus: %s\n", PROGRAM_NAME,
                strerror(-result));
        return -1;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &start_time) != 0) {
        fprintf(stderr, "%s: cannot read monotonic clock: %s\n", PROGRAM_NAME,
                strerror(errno));
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
            result = get_unit_active_state(bus, app_services[i], &active_state);
            if (result < 0) {
                sd_bus_error_free(&error);
                sd_bus_unref(bus);
                return -1;
            }
            if (strcmp(active_state, "failed") == 0) {
                fprintf(stderr, "%s: app service failed, skip SIGUSR2: %s\n",
                        PROGRAM_NAME, app_services[i]);
                completed[i] = true;
                free(active_state);
                continue;
            }
            if (strcmp(active_state, "active") == 0) {
                result = sd_bus_call_method(bus, "org.freedesktop.systemd1",
                                            "/org/freedesktop/systemd1",
                                            "org.freedesktop.systemd1.Manager",
                                            "KillUnit", &error, NULL, "ssi",
                                            app_services[i], "all", SIGUSR2);
                if (result < 0) {
                    fprintf(stderr, "%s: cannot send SIGUSR2 to %s: %s\n",
                            PROGRAM_NAME, app_services[i],
                            error.message != NULL ? error.message
                                                  : strerror(-result));
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
                elapsed +=
                    (uint64_t)(now.tv_nsec - start_time.tv_nsec) / 1000ULL;
            } else {
                elapsed -= 1000000ULL;
                elapsed +=
                    (uint64_t)(1000000000L + now.tv_nsec - start_time.tv_nsec) /
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
        sd_bus_error_free(&error);
        sd_bus_unref(bus);
        errno = ETIMEDOUT;
        return -1;
    }

    sd_bus_error_free(&error);
    sd_bus_unref(bus);
    return 0;
}

int recover_app_param(struct app_param_state *state, const char *mountpoint,
                      int chunk_kib) {
    uint8_t other_digest[32];
    int result = EXIT_OK;
    int other_result;

    (void)mountpoint;
    other_result =
        powerctl_verify_path_digest(state->other_dev, chunk_kib, other_digest);

    if (other_result != EXIT_OK ||
        memcmp(state->mounted_digest, other_digest, 32) != 0) {
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

        other_result = powerctl_verify_path_digest(state->other_dev, chunk_kib,
                                                   other_digest);
        if (other_result != EXIT_OK ||
            memcmp(state->mounted_digest, other_digest, 32) != 0) {
            fprintf(stderr, "%s: recovered peer verify failed: %s\n",
                    PROGRAM_NAME, state->other_dev);
            result =
                other_result == EXIT_OK ? EXIT_HASH_MISMATCH : other_result;
            goto out_restore;
        }
        printf("backup %s is recovered and verified\n", state->other_dev);
    }

    if (release_app_services() != 0) {
        result = EXIT_IO_ERROR;
        goto out_restore;
    }

out_restore:
    if (write_dirty_settings(&original) != 0) {
        fprintf(stderr, "%s: cannot restore dirty settings: %s\n", PROGRAM_NAME,
                strerror(errno));
        if (result == EXIT_OK) result = EXIT_IO_ERROR;
    }
    return result;
}
