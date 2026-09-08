#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#include <linux/fs.h>
#include <systemd/sd-bus.h>
#include <systemd/sd-daemon.h>

#include "common.h"
#include "footer.h"
#include "hash.h"
#include "storage.h"

#define APP_PARAM_MOUNT_FLAGS (MS_NOATIME | MS_NOSUID | MS_NODEV)
#define APP_PARAM_FS "vfat"
#define APP_PARAM_MOUNT_OPTS NULL
#define APP_READY_TIMEOUT_USEC (60ULL * 1000000ULL)
#define FAT_IOCTL_DISABLE_WRITE _IO('r', 0x14)

struct dirty_settings {
    uint32_t writeback_centisecs;
    uint32_t background_ratio;
    uint32_t dirty_ratio;
};

struct target {
    int32_t fd;
    uint64_t size;
    uint32_t logical_size;
};

static struct dirty_settings original;

static int32_t open_target(const char *path, int32_t flags,
                           struct target *target) {
    struct stat st;
    int32_t logical_size;

    if (path == NULL || target == NULL) {
        errno = EINVAL;
        FSCTL_ERROR("invalid block target arguments");
        return -1;
    }

    target->fd = -1;
    target->size = 0U;
    target->logical_size = 0U;

    if (stat(path, &st) != 0) {
        FSCTL_ERROR("stat failed path=%s: %s", path, strerror(errno));
        return -1;
    }
    if (!S_ISBLK(st.st_mode)) {
        errno = ENOTBLK;
        FSCTL_ERROR("target is not a block device path=%s", path);
        return -1;
    }

    target->fd = (int32_t)open(path, flags);
    if (target->fd < 0) {
        FSCTL_ERROR("open block device failed path=%s flags=0x%" PRIx32 ": %s",
                    path, (uint32_t)flags, strerror(errno));
        return -1;
    }

    if (get_fd_size(target->fd, &target->size) != 0 ||
        ioctl(target->fd, BLKSSZGET, &logical_size) != 0) {
        int32_t saved_errno = errno;
        FSCTL_ERROR("query block geometry failed path=%s: %s", path,
                    strerror(saved_errno));
        (void)close(target->fd);
        target->fd = -1;
        errno = saved_errno;
        return -1;
    }

    if (target->size <= FOOTER_AREA_SIZE || target->size > INT64_MAX ||
        logical_size <= 0 ||
        FOOTER_AREA_SIZE % (uint32_t)logical_size != 0U ||
        target->size % (uint32_t)logical_size != 0U) {
        FSCTL_ERROR("unsupported block layout path=%s size=%" PRIu64
                    " logical=%" PRId32,
                    path, target->size, logical_size);
        (void)close(target->fd);
        target->fd = -1;
        errno = EINVAL;
        return -1;
    }

    target->logical_size = (uint32_t)logical_size;
    FSCTL_DEBUG("opened block target path=%s fd=%" PRId32 " size=%" PRIu64
                " logical=%" PRIu32,
                path, target->fd, target->size, target->logical_size);
    return 0;
}

static void close_target(struct target *target) {
    if (target == NULL || target->fd < 0)
        return;
    if (close(target->fd) != 0)
        FSCTL_WARN("close block target failed fd=%" PRId32 ": %s", target->fd,
                   strerror(errno));
    target->fd = -1;
}

static int32_t resolve_active_peer(const char *mountpoint, const char *device_a,
                                   const char *device_b, const char **active,
                                   const char **peer) {
    struct stat mount_stat;
    struct stat a_stat;
    struct stat b_stat;

    if (mountpoint == NULL || device_a == NULL || device_b == NULL ||
        active == NULL || peer == NULL) {
        errno = EINVAL;
        FSCTL_ERROR("invalid active/peer arguments");
        return -1;
    }
    if (stat(mountpoint, &mount_stat) != 0 || stat(device_a, &a_stat) != 0 ||
        stat(device_b, &b_stat) != 0) {
        FSCTL_ERROR("cannot resolve active/peer mountpoint=%s: %s", mountpoint,
                    strerror(errno));
        return -1;
    }
    if (!S_ISBLK(a_stat.st_mode) || !S_ISBLK(b_stat.st_mode) ||
        a_stat.st_rdev == b_stat.st_rdev) {
        errno = EINVAL;
        FSCTL_ERROR("invalid A/B block devices a=%s b=%s", device_a, device_b);
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
    FSCTL_ERROR("mountpoint is not backed by A/B mountpoint=%s", mountpoint);
    return -1;
}

static int32_t disable_vfat_write(const char *mountpoint) {
    int32_t fd;
    int32_t result;

    fd = (int32_t)open(mountpoint, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        FSCTL_ERROR("open mountpoint for freeze failed path=%s: %s", mountpoint,
                    strerror(errno));
        return -1;
    }

    FSCTL_INFO("freeze app_param mountpoint=%s", mountpoint);
    result = (int32_t)ioctl(fd, FAT_IOCTL_DISABLE_WRITE);
    if (result != 0) {
        int32_t saved_errno = errno;
        FSCTL_ERROR("FAT write disable failed mountpoint=%s: %s", mountpoint,
                    strerror(saved_errno));
        (void)close(fd);
        errno = saved_errno;
        return -1;
    }
    if (close(fd) != 0) {
        FSCTL_ERROR("close frozen mountpoint fd failed path=%s: %s", mountpoint,
                    strerror(errno));
        return -1;
    }

    FSCTL_INFO("freeze complete mountpoint=%s", mountpoint);
    return 0;
}

int32_t powerctl_seal_block_device(const char *path,
                                   uint8_t digest[SHA256_DIGEST_SIZE],
                                   uint64_t *device_size) {
    struct target target;
    uint8_t computed_digest[SHA256_DIGEST_SIZE];
    uint64_t protected_size;
    int32_t result = -1;

    FSCTL_INFO("seal start device=%s", path != NULL ? path : "(null)");
    if (open_target(path, O_RDWR | O_CLOEXEC | O_DIRECT, &target) != 0)
        return -1;

    protected_size = target.size - FOOTER_AREA_SIZE;
    if (hash_sha256_fd(target.fd, protected_size, computed_digest) != 0) {
        FSCTL_ERROR("seal hash failed device=%s", path);
        goto out;
    }
    if (write_descriptor(target.fd, computed_digest) != EXIT_OK) {
        FSCTL_ERROR("seal descriptor update failed device=%s", path);
        goto out;
    }

    if (digest != NULL)
        memcpy(digest, computed_digest, sizeof(computed_digest));
    if (device_size != NULL)
        *device_size = target.size;
    result = 0;
    FSCTL_INFO("seal complete device=%s protected_size=%" PRIu64, path,
               protected_size);

out:
    close_target(&target);
    return result;
}

static int32_t resolve_partition_source(const char *partition, char *parent,
                                        size_t parent_size,
                                        uint64_t *source_offset,
                                        uint64_t *partition_size) {
    struct stat st;
    char sys_path[PATH_MAX];
    char resolved[PATH_MAX];
    char start_path[PATH_MAX];
    char *slash;
    char *parent_name;
    uint64_t start_sector;
    FILE *file;
    struct target target;
    int32_t length;

    if (partition == NULL || parent == NULL || parent_size == 0U ||
        source_offset == NULL || partition_size == NULL) {
        errno = EINVAL;
        FSCTL_ERROR("invalid recovery source arguments");
        return -1;
    }
    if (stat(partition, &st) != 0 || !S_ISBLK(st.st_mode)) {
        if (errno == 0)
            errno = ENOTBLK;
        FSCTL_ERROR("invalid recovery partition=%s: %s", partition,
                    strerror(errno));
        return -1;
    }

    length = (int32_t)snprintf(sys_path, sizeof(sys_path), "/sys/dev/block/%u:%u",
                               major(st.st_rdev), minor(st.st_rdev));
    if (length < 0 || (size_t)length >= sizeof(sys_path) ||
        realpath(sys_path, resolved) == NULL) {
        FSCTL_ERROR("resolve sysfs parent failed partition=%s: %s", partition,
                    strerror(errno));
        return -1;
    }

    slash = strrchr(resolved, '/');
    if (slash == NULL || slash == resolved) {
        errno = EINVAL;
        FSCTL_ERROR("invalid sysfs partition path=%s", resolved);
        return -1;
    }
    *slash = '\0';
    parent_name = strrchr(resolved, '/');
    if (parent_name == NULL || parent_name[1] == '\0') {
        errno = EINVAL;
        FSCTL_ERROR("cannot derive parent block device partition=%s", partition);
        return -1;
    }
    ++parent_name;

    length = (int32_t)snprintf(parent, parent_size, "/dev/%s", parent_name);
    if (length < 0 || (size_t)length >= parent_size) {
        errno = ENAMETOOLONG;
        FSCTL_ERROR("parent block path too long partition=%s", partition);
        return -1;
    }
    length = (int32_t)snprintf(start_path, sizeof(start_path), "%s/start",
                               sys_path);
    if (length < 0 || (size_t)length >= sizeof(start_path)) {
        errno = ENAMETOOLONG;
        FSCTL_ERROR("partition start path too long partition=%s", partition);
        return -1;
    }

    file = fopen(start_path, "r");
    if (file == NULL) {
        FSCTL_ERROR("open partition start failed path=%s: %s", start_path,
                    strerror(errno));
        return -1;
    }
    if (fscanf(file, "%" SCNu64, &start_sector) != 1) {
        FSCTL_ERROR("read partition start failed path=%s", start_path);
        (void)fclose(file);
        errno = EINVAL;
        return -1;
    }
    if (fclose(file) != 0)
        FSCTL_WARN("close partition start file failed path=%s: %s", start_path,
                   strerror(errno));

    if (open_target(partition, O_RDONLY | O_CLOEXEC, &target) != 0)
        return -1;
    *partition_size = target.size;
    close_target(&target);

    if (start_sector > UINT64_MAX / 512ULL) {
        errno = EOVERFLOW;
        FSCTL_ERROR("partition start overflow partition=%s sector=%" PRIu64,
                    partition, start_sector);
        return -1;
    }
    *source_offset = start_sector * 512ULL;
    FSCTL_DEBUG("resolved recovery source partition=%s parent=%s offset=%" PRIu64
                " size=%" PRIu64,
                partition, parent, *source_offset, *partition_size);
    return 0;
}

static int32_t copy_block_range_from_target(struct target *source_target,
                                            uint64_t source_offset,
                                            uint64_t length,
                                            const char *destination) {
    struct target destination_target;
    void *buffer = NULL;
    const size_t buffer_size = 1024U * 1024U;
    uint64_t offset = 0U;
    size_t alignment;
    int32_t result = -1;

    if (source_target == NULL || destination == NULL || length == 0U ||
        source_offset > source_target->size ||
        length > source_target->size - source_offset) {
        errno = EINVAL;
        FSCTL_ERROR("invalid block copy range offset=%" PRIu64 " length=%" PRIu64,
                    source_offset, length);
        return -1;
    }

    if (open_target(destination, O_RDWR | O_CLOEXEC | O_DIRECT | O_EXCL,
                    &destination_target) != 0)
        return -1;

    if (destination_target.size != length ||
        source_offset % source_target->logical_size != 0U ||
        length % source_target->logical_size != 0U ||
        length % destination_target.logical_size != 0U) {
        errno = EINVAL;
        FSCTL_ERROR("block copy layout mismatch destination=%s src_offset=%" PRIu64
                    " length=%" PRIu64,
                    destination, source_offset, length);
        goto out;
    }

    alignment = source_target->logical_size > destination_target.logical_size
                    ? source_target->logical_size
                    : destination_target.logical_size;
    if (alignment < 4096U)
        alignment = 4096U;
    if (posix_memalign(&buffer, alignment, buffer_size) != 0) {
        errno = ENOMEM;
        FSCTL_ERROR("block copy buffer allocation failed alignment=%zu", alignment);
        goto out;
    }

    FSCTL_INFO("block copy start destination=%s offset=%" PRIu64
               " length=%" PRIu64,
               destination, source_offset, length);
    while (offset < length) {
        size_t count = buffer_size;

        if (length - offset < count)
            count = (size_t)(length - offset);
        if (pread_full(source_target->fd, buffer, count, source_offset + offset) !=
            0) {
            FSCTL_ERROR("block copy source read failed offset=%" PRIu64,
                        source_offset + offset);
            goto out;
        }
        if (pwrite_full(destination_target.fd, buffer, count, offset) != 0) {
            FSCTL_ERROR("block copy destination write failed path=%s offset=%" PRIu64,
                        destination, offset);
            goto out;
        }
        offset += count;
    }

    if (fsync(destination_target.fd) != 0) {
        FSCTL_ERROR("block copy fsync failed destination=%s: %s", destination,
                    strerror(errno));
        goto out;
    }
    result = 0;
    FSCTL_INFO("block copy complete destination=%s bytes=%" PRIu64, destination,
               length);

out:
    free(buffer);
    close_target(&destination_target);
    return result;
}

static int32_t copy_block_range(const char *source, uint64_t source_offset,
                                uint64_t length, const char *destination) {
    struct target source_target;
    int32_t result;

    if (source == NULL || destination == NULL || strcmp(source, destination) == 0) {
        errno = EINVAL;
        FSCTL_ERROR("invalid block copy source/destination");
        return -1;
    }
    if (open_target(source, O_RDONLY | O_CLOEXEC | O_DIRECT, &source_target) != 0)
        return -1;

    result = copy_block_range_from_target(&source_target, source_offset, length,
                                          destination);
    close_target(&source_target);
    return result;
}

static int32_t copy_block_device(const char *source, const char *destination) {
    struct target source_target;
    int32_t result;

    if (source == NULL || destination == NULL || strcmp(source, destination) == 0) {
        errno = EINVAL;
        FSCTL_ERROR("invalid full block copy source/destination");
        return -1;
    }
    if (open_target(source, O_RDONLY | O_CLOEXEC | O_DIRECT, &source_target) != 0)
        return -1;

    result = copy_block_range_from_target(&source_target, 0U, source_target.size,
                                          destination);
    close_target(&source_target);
    return result;
}

static int32_t read_sysctl_u32(const char *path, uint32_t *value) {
    FILE *file;
    int32_t result = 0;

    file = fopen(path, "r");
    if (file == NULL) {
        FSCTL_ERROR("open sysctl for read failed path=%s: %s", path,
                    strerror(errno));
        return -1;
    }
    if (fscanf(file, "%" SCNu32, value) != 1) {
        FSCTL_ERROR("read sysctl failed path=%s", path);
        errno = EIO;
        result = -1;
    }
    if (fclose(file) != 0) {
        FSCTL_WARN("close sysctl after read failed path=%s: %s", path,
                   strerror(errno));
        if (result == 0)
            result = -1;
    }
    return result;
}

static int32_t write_sysctl_u32(const char *path, uint32_t value) {
    FILE *file;
    int32_t result = 0;

    file = fopen(path, "w");
    if (file == NULL) {
        FSCTL_ERROR("open sysctl for write failed path=%s: %s", path,
                    strerror(errno));
        return -1;
    }
    if (fprintf(file, "%" PRIu32 "\n", value) < 0) {
        FSCTL_ERROR("write sysctl failed path=%s value=%" PRIu32, path, value);
        result = -1;
    }
    if (fclose(file) != 0) {
        FSCTL_ERROR("close sysctl after write failed path=%s: %s", path,
                    strerror(errno));
        result = -1;
    }
    return result;
}

static int32_t read_dirty_settings(struct dirty_settings *settings) {
    if (settings == NULL) {
        errno = EINVAL;
        FSCTL_ERROR("dirty settings output is null");
        return -1;
    }
    if (read_sysctl_u32("/proc/sys/vm/dirty_writeback_centisecs",
                        &settings->writeback_centisecs) != 0 ||
        read_sysctl_u32("/proc/sys/vm/dirty_background_ratio",
                        &settings->background_ratio) != 0 ||
        read_sysctl_u32("/proc/sys/vm/dirty_ratio", &settings->dirty_ratio) != 0) {
        FSCTL_ERROR("read dirty settings failed");
        return -1;
    }
    FSCTL_DEBUG("dirty settings read writeback=%" PRIu32 " background=%" PRIu32
                " ratio=%" PRIu32,
                settings->writeback_centisecs, settings->background_ratio,
                settings->dirty_ratio);
    return 0;
}

static int32_t write_dirty_settings(const struct dirty_settings *settings) {
    int32_t first_errno = 0;

    if (settings == NULL) {
        errno = EINVAL;
        FSCTL_ERROR("dirty settings input is null");
        return -1;
    }

    if (write_sysctl_u32("/proc/sys/vm/dirty_writeback_centisecs",
                         settings->writeback_centisecs) != 0)
        first_errno = errno != 0 ? errno : EIO;
    if (write_sysctl_u32("/proc/sys/vm/dirty_background_ratio",
                         settings->background_ratio) != 0 &&
        first_errno == 0)
        first_errno = errno != 0 ? errno : EIO;
    if (write_sysctl_u32("/proc/sys/vm/dirty_ratio", settings->dirty_ratio) != 0 &&
        first_errno == 0)
        first_errno = errno != 0 ? errno : EIO;

    if (first_errno != 0) {
        errno = first_errno;
        FSCTL_ERROR("write dirty settings failed: %s", strerror(errno));
        return -1;
    }
    FSCTL_DEBUG("dirty settings applied writeback=%" PRIu32
                " background=%" PRIu32 " ratio=%" PRIu32,
                settings->writeback_centisecs, settings->background_ratio,
                settings->dirty_ratio);
    return 0;
}

static int32_t enable_pagecache_delay(void) {
    const struct dirty_settings delayed = {
        .writeback_centisecs = 6000U,
        .background_ratio = 80U,
        .dirty_ratio = 90U,
    };

    FSCTL_INFO("enable recovery dirty profile writeback=%" PRIu32
               " background=%" PRIu32 " ratio=%" PRIu32,
               delayed.writeback_centisecs, delayed.background_ratio,
               delayed.dirty_ratio);
    return write_dirty_settings(&delayed);
}

static int32_t split_devices(const char *spec, char **first, char **second) {
    const char *separator;
    size_t first_len;

    if (spec == NULL || first == NULL || second == NULL) {
        errno = EINVAL;
        FSCTL_ERROR("invalid A/B device specification arguments");
        return -1;
    }
    separator = strchr(spec, ':');
    if (separator == NULL || separator == spec || separator[1] == '\0') {
        errno = EINVAL;
        FSCTL_ERROR("invalid device pair spec=%s expected=A:B", spec);
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
        FSCTL_ERROR("allocate A/B device names failed");
        return -1;
    }
    return 0;
}

int32_t powerctl_verify_path_digest(const char *path,
                                    uint8_t digest[SHA256_DIGEST_SIZE]) {
    struct target target;
    uint8_t expected_digest[SHA256_DIGEST_SIZE];
    uint8_t actual_digest[SHA256_DIGEST_SIZE];
    uint64_t protected_size;
    char digest_hex[(SHA256_DIGEST_SIZE * 2U) + 1U];
    int32_t result;

    if (path == NULL || digest == NULL) {
        errno = EINVAL;
        FSCTL_ERROR("invalid verify arguments");
        return EXIT_IO_ERROR;
    }

    FSCTL_DEBUG("verify start device=%s", path);
    if (open_target(path, O_RDONLY | O_CLOEXEC | O_DIRECT, &target) != 0)
        return EXIT_IO_ERROR;

    result = read_descriptor(target.fd, expected_digest);
    if (result != EXIT_OK) {
        FSCTL_WARN("descriptor verify failed device=%s result=%" PRId32, path,
                   result);
        close_target(&target);
        return result;
    }

    protected_size = target.size - FOOTER_AREA_SIZE;
    if (hash_sha256_fd(target.fd, protected_size, actual_digest) != 0) {
        FSCTL_ERROR("device hash failed path=%s", path);
        close_target(&target);
        return EXIT_IO_ERROR;
    }
    close_target(&target);

    if (memcmp(actual_digest, expected_digest, SHA256_DIGEST_SIZE) != 0) {
        char expected_hex[(SHA256_DIGEST_SIZE * 2U) + 1U];
        char actual_hex[(SHA256_DIGEST_SIZE * 2U) + 1U];

        hash_to_hex(expected_digest, expected_hex);
        hash_to_hex(actual_digest, actual_hex);
        FSCTL_WARN("hash mismatch device=%s expected=%s actual=%s", path,
                   expected_hex, actual_hex);
        return EXIT_HASH_MISMATCH;
    }

    memcpy(digest, actual_digest, SHA256_DIGEST_SIZE);
    hash_to_hex(actual_digest, digest_hex);
    FSCTL_INFO("verify pass device=%s size=%" PRIu64 " protected=%" PRIu64
               " digest=%s",
               path, target.size, protected_size, digest_hex);
    return EXIT_OK;
}

int32_t mount_partition(struct app_param_state *state, const char *devices,
                        const char *mountpoint) {
    char *device_a = NULL;
    char *device_b = NULL;
    const char *candidates[2];
    uint32_t index;
    int32_t result;

    if (state == NULL || mountpoint == NULL) {
        errno = EINVAL;
        FSCTL_ERROR("invalid mount arguments");
        return EXIT_UNSUPPORTED_LAYOUT;
    }
    if (split_devices(devices, &device_a, &device_b) != 0)
        return EXIT_UNSUPPORTED_LAYOUT;

    candidates[0] = device_a;
    candidates[1] = device_b;
    FSCTL_INFO("mount start devices=%s mountpoint=%s", devices, mountpoint);

    if (read_dirty_settings(&original) != 0) {
        free(device_a);
        free(device_b);
        return EXIT_IO_ERROR;
    }
    if (enable_pagecache_delay() != 0) {
        int32_t saved_errno = errno;

        FSCTL_ERROR("configure recovery dirty profile failed: %s",
                    strerror(saved_errno));
        if (write_dirty_settings(&original) != 0)
            FSCTL_ERROR("restore dirty settings after setup failure failed: %s",
                        strerror(errno));
        free(device_a);
        free(device_b);
        errno = saved_errno;
        return EXIT_IO_ERROR;
    }

    if (mkdir(mountpoint, 0755) != 0 && errno != EEXIST) {
        FSCTL_ERROR("create mountpoint failed path=%s: %s", mountpoint,
                    strerror(errno));
        result = EXIT_IO_ERROR;
        goto fail_restore_dirty;
    }

    for (index = 0U; index < 2U; ++index) {
        result = powerctl_verify_path_digest(candidates[index],
                                             state->mounted_digest);
        if (result != EXIT_OK) {
            FSCTL_WARN("candidate invalid device=%s result=%" PRId32,
                       candidates[index], result);
            continue;
        }

        if (mount(candidates[index], mountpoint, APP_PARAM_FS,
                  APP_PARAM_MOUNT_FLAGS, APP_PARAM_MOUNT_OPTS) != 0) {
            FSCTL_WARN("mount candidate failed device=%s mountpoint=%s: %s",
                       candidates[index], mountpoint, strerror(errno));
            continue;
        }

        state->mounted_dev = strdup(candidates[index]);
        state->other_dev = strdup(index == 0U ? candidates[1] : candidates[0]);
        if (state->mounted_dev == NULL || state->other_dev == NULL) {
            FSCTL_ERROR("allocate mount state failed");
            result = EXIT_NOMEM;
            goto fail_unmount;
        }

        FSCTL_INFO("mount ready active=%s peer=%s mountpoint=%s",
                   state->mounted_dev, state->other_dev, mountpoint);
        if (sd_notify(0, "READY=1\nSTATUS=app_param mounted") < 0) {
            FSCTL_ERROR("sd_notify READY failed mountpoint=%s", mountpoint);
            result = EXIT_IO_ERROR;
            goto fail_unmount;
        }

        free(device_a);
        free(device_b);
        return EXIT_OK;
    }

    FSCTL_ERROR("no valid app_param device devices=%s", devices);
    result = EXIT_METADATA_INVALID;
    goto fail_restore_dirty;

fail_unmount:
    free(state->mounted_dev);
    free(state->other_dev);
    state->mounted_dev = NULL;
    state->other_dev = NULL;
    if (umount2(mountpoint, 0) != 0)
        FSCTL_ERROR("cleanup unmount failed mountpoint=%s: %s", mountpoint,
                    strerror(errno));

fail_restore_dirty:
    if (write_dirty_settings(&original) != 0)
        FSCTL_ERROR("restore dirty settings after mount failure failed: %s",
                    strerror(errno));
    free(device_a);
    free(device_b);
    return result;
}

int32_t umount_partition(const char *devices, const char *mountpoint) {
    char *device_a = NULL;
    char *device_b = NULL;
    const char *active = NULL;
    const char *peer = NULL;
    int32_t result = EXIT_IO_ERROR;

    if (split_devices(devices, &device_a, &device_b) != 0)
        return EXIT_UNSUPPORTED_LAYOUT;
    if (resolve_active_peer(mountpoint, device_a, device_b, &active, &peer) != 0)
        goto out;

    FSCTL_INFO("power prepare start active=%s peer=%s mountpoint=%s", active,
               peer, mountpoint);
    if (disable_vfat_write(mountpoint) != 0)
        goto out;
    if (powerctl_seal_block_device(active, NULL, NULL) != 0)
        goto out;
    if (copy_block_device(active, peer) != 0)
        goto out;

    FSCTL_INFO("power prepare complete active=%s peer=%s", active, peer);
    result = EXIT_OK;

out:
    if (result != EXIT_OK)
        FSCTL_ERROR("power prepare failed mountpoint=%s", mountpoint);
    free(device_a);
    free(device_b);
    return result;
}

static int32_t get_unit_active_state(sd_bus *bus, const char *unit,
                                     char **active_state) {
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    const char *unit_path;
    int32_t result;

    if (bus == NULL || unit == NULL || active_state == NULL) {
        errno = EINVAL;
        FSCTL_ERROR("invalid unit state query arguments");
        return -1;
    }

    *active_state = NULL;
    result = (int32_t)sd_bus_call_method(
        bus, "org.freedesktop.systemd1", "/org/freedesktop/systemd1",
        "org.freedesktop.systemd1.Manager", "GetUnit", &error, &reply, "s",
        unit);
    if (result >= 0)
        result = (int32_t)sd_bus_message_read(reply, "o", &unit_path);
    if (result >= 0)
        result = (int32_t)sd_bus_get_property_string(
            bus, "org.freedesktop.systemd1", unit_path,
            "org.freedesktop.systemd1.Unit", "ActiveState", &error,
            active_state);
    if (result < 0)
        FSCTL_ERROR("query unit state failed unit=%s: %s", unit,
                    error.message != NULL ? error.message : strerror(-result));

    sd_bus_message_unref(reply);
    sd_bus_error_free(&error);
    return result;
}

static int32_t release_app_services(void) {
    static const char *const app_services[] = {
        "sensor_center.service",
        "video_functions.service",
    };
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus *bus = NULL;
    bool completed[sizeof(app_services) / sizeof(app_services[0])] = {false};
    struct timespec start_time;
    int32_t result;

    result = (int32_t)sd_bus_open_system(&bus);
    if (result < 0) {
        FSCTL_ERROR("connect system bus failed: %s", strerror(-result));
        return -1;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &start_time) != 0) {
        FSCTL_ERROR("read monotonic clock failed: %s", strerror(errno));
        result = -1;
        goto out;
    }

    for (;;) {
        bool all_finished = true;
        uint32_t index;

        for (index = 0U;
             index < (uint32_t)(sizeof(app_services) / sizeof(app_services[0]));
             ++index) {
            char *active_state = NULL;

            if (completed[index])
                continue;

            result = get_unit_active_state(bus, app_services[index],
                                           &active_state);
            if (result < 0) {
                all_finished = false;
                continue;
            }
            if (strcmp(active_state, "failed") == 0 ||
                strcmp(active_state, "inactive") == 0) {
                FSCTL_WARN("skip app release service=%s state=%s",
                           app_services[index], active_state);
                completed[index] = true;
                free(active_state);
                continue;
            }
            if (strcmp(active_state, "active") == 0) {
                result = (int32_t)sd_bus_call_method(
                    bus, "org.freedesktop.systemd1",
                    "/org/freedesktop/systemd1",
                    "org.freedesktop.systemd1.Manager", "KillUnit", &error,
                    NULL, "ssi", app_services[index], "all", SIGUSR2);
                if (result < 0) {
                    FSCTL_ERROR("send SIGUSR2 failed service=%s: %s",
                                app_services[index],
                                error.message != NULL ? error.message
                                                      : strerror(-result));
                    free(active_state);
                    result = -1;
                    goto out;
                }
                FSCTL_INFO("app cache gate released service=%s",
                           app_services[index]);
                completed[index] = true;
                free(active_state);
                continue;
            }

            FSCTL_DEBUG("wait app service=%s state=%s", app_services[index],
                        active_state);
            all_finished = false;
            free(active_state);
        }

        if (all_finished)
            break;

        {
            struct timespec now;
            uint64_t elapsed;

            if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
                FSCTL_ERROR("read monotonic clock failed: %s", strerror(errno));
                result = -1;
                goto out;
            }
            elapsed = (uint64_t)(now.tv_sec - start_time.tv_sec) * 1000000ULL;
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
                FSCTL_WARN("app release wait timeout seconds=%" PRIu64,
                           (uint64_t)(APP_READY_TIMEOUT_USEC / 1000000ULL));
                break;
            }
        }
        usleep(100000U);
    }

    result = 0;
out:
    sd_bus_error_free(&error);
    sd_bus_unref(bus);
    return result;
}

int32_t recover_app_param(struct app_param_state *state) {
    uint8_t other_digest[SHA256_DIGEST_SIZE];
    int32_t result = EXIT_OK;
    int32_t other_result;

    if (state == NULL || state->mounted_dev == NULL || state->other_dev == NULL) {
        errno = EINVAL;
        FSCTL_ERROR("invalid recovery state");
        return EXIT_IO_ERROR;
    }

    other_result = powerctl_verify_path_digest(state->other_dev, other_digest);
    if (other_result != EXIT_OK ||
        memcmp(state->mounted_digest, other_digest, SHA256_DIGEST_SIZE) != 0) {
        char parent[PATH_MAX];
        uint64_t source_offset;
        uint64_t partition_size;

        FSCTL_INFO("recovery required active=%s peer=%s peer_result=%" PRId32,
                   state->mounted_dev, state->other_dev, other_result);
        if (resolve_partition_source(state->mounted_dev, parent, sizeof(parent),
                                     &source_offset, &partition_size) != 0) {
            FSCTL_ERROR("resolve recovery source failed active=%s: %s",
                        state->mounted_dev, strerror(errno));
            result = EXIT_IO_ERROR;
            goto out_restore;
        }

        FSCTL_INFO("recovery copy start peer=%s parent=%s offset=%" PRIu64
                   " size=%" PRIu64,
                   state->other_dev, parent, source_offset, partition_size);
        if (copy_block_range(parent, source_offset, partition_size,
                             state->other_dev) != 0) {
            FSCTL_ERROR("recovery copy failed peer=%s: %s", state->other_dev,
                        strerror(errno));
            result = EXIT_IO_ERROR;
            goto out_restore;
        }

        other_result = powerctl_verify_path_digest(state->other_dev,
                                                   other_digest);
        if (other_result != EXIT_OK ||
            memcmp(state->mounted_digest, other_digest, SHA256_DIGEST_SIZE) != 0) {
            FSCTL_ERROR("recovered peer verify failed peer=%s result=%" PRId32,
                        state->other_dev, other_result);
            result =
                other_result == EXIT_OK ? EXIT_HASH_MISMATCH : other_result;
            goto out_restore;
        }
        FSCTL_INFO("recovery complete peer=%s", state->other_dev);
    } else {
        FSCTL_DEBUG("recovery not required active=%s peer=%s", state->mounted_dev,
                    state->other_dev);
    }

    if (release_app_services() != 0) {
        FSCTL_ERROR("release app cache gate failed");
        result = EXIT_IO_ERROR;
        goto out_restore;
    }

out_restore:
    if (write_dirty_settings(&original) != 0) {
        FSCTL_ERROR("restore dirty settings failed: %s", strerror(errno));
        if (result == EXIT_OK)
            result = EXIT_IO_ERROR;
    } else {
        FSCTL_INFO("recovery dirty profile restored");
    }
    return result;
}
