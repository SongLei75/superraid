#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/fs.h>

#include "hash.h"
#include "storage.h"

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
#define PROTOCOL_VERSION 1U
#define HASH_ALGORITHM_SHA256 1U
#define SHA256_DIGEST_SIZE 32U

static const uint8_t descriptor_magic[8] = {
    'V', 'F', 'I', 'N', 'T', 'E', 'G', '\0'
};

struct target {
    int fd;
    uint64_t size;
};

static uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void put_le32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static void put_le64(uint8_t *p, uint64_t value)
{
    put_le32(p, (uint32_t)value);
    put_le32(p + 4, (uint32_t)(value >> 32));
}

static int open_target(const char *path, bool writable, struct target *target)
{
    struct stat st;
    unsigned long long bytes;
    int logical_size;
    int flags = (writable ? O_RDWR : O_RDONLY) | O_CLOEXEC | O_DIRECT;

    target->fd = -1;
    target->size = 0;
    if (stat(path, &st) != 0 || !S_ISBLK(st.st_mode)) {
        errno = ENOTBLK;
        return -1;
    }
    target->fd = open(path, flags);
    if (target->fd < 0)
        return -1;
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
    return 0;
}

static void close_target(struct target *target)
{
    if (target->fd >= 0) {
        close(target->fd);
        target->fd = -1;
    }
}

static int write_footer(int fd, uint64_t protected_size,
                        const uint8_t digest[SHA256_DIGEST_SIZE])
{
    uint8_t footer[FOOTER_AREA_SIZE];
    uint8_t *descriptor = footer + FOOTER_AREA_SIZE - DESCRIPTOR_SIZE;
    uint8_t crc_digest[4];
    int saved_flags;

    memset(footer, 0, sizeof(footer));
    memcpy(descriptor + DESC_MAGIC_OFFSET, descriptor_magic,
           sizeof(descriptor_magic));
    put_le32(descriptor + DESC_VERSION_OFFSET, PROTOCOL_VERSION);
    put_le32(descriptor + DESC_DESCRIPTOR_SIZE_OFFSET, DESCRIPTOR_SIZE);
    put_le32(descriptor + DESC_HASH_ALGORITHM_OFFSET, HASH_ALGORITHM_SHA256);
    put_le32(descriptor + DESC_DIGEST_LEN_OFFSET, SHA256_DIGEST_SIZE);
    put_le64(descriptor + DESC_HASHED_SIZE_OFFSET, protected_size);
    memcpy(descriptor + DESC_DIGEST_OFFSET, digest, SHA256_DIGEST_SIZE);
    if (hash_crc32(descriptor, DESCRIPTOR_SIZE, crc_digest) != 0)
        return -1;
    put_le32(descriptor + DESC_CRC32C_OFFSET, get_le32(crc_digest));

    saved_flags = fcntl(fd, F_GETFL);
    if (saved_flags < 0)
        return -1;
    if (saved_flags & O_DIRECT &&
        fcntl(fd, F_SETFL, saved_flags & ~O_DIRECT) < 0)
        return -1;
    if (pwrite(fd, footer, sizeof(footer), (off_t)protected_size) !=
        (ssize_t)sizeof(footer))
        return -1;
    return fsync(fd);
}

int powerctl_disable_vfat_write(const char *mountpoint)
{
    int fd;
    int result;

    fd = open(mountpoint, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return -1;
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
                               uint64_t *device_size)
{
    struct target target;
    uint8_t computed_digest[SHA256_DIGEST_SIZE];
    uint64_t protected_size;
    int result = -1;

    if (open_target(path, true, &target) != 0)
        return -1;
    protected_size = target.size - FOOTER_AREA_SIZE;
    if (hash_sha256_fd(target.fd, protected_size, computed_digest,
                       chunk_kib) == 0) {
        result = write_footer(target.fd, protected_size, computed_digest);
        if (result == 0) {
            if (digest != NULL)
                memcpy(digest, computed_digest, sizeof(computed_digest));
            if (device_size != NULL)
            *device_size = target.size;
        }
    }
    close_target(&target);
    return result;
}

int powerctl_copy_block_device(const char *source, const char *destination)
{
    struct target source_target;
    struct target destination_target;
    void *buffer = NULL;
    const size_t buffer_size = 1024U * 1024U;
    uint64_t offset = 0;
    int result = -1;

    if (open_target(source, false, &source_target) != 0)
        return -1;
    if (open_target(destination, true, &destination_target) != 0)
        goto out_source;
    if (posix_memalign(&buffer, 4096U, buffer_size) != 0) {
        errno = ENOMEM;
        goto out;
    }
    while (offset < source_target.size) {
        size_t count = buffer_size;
        ssize_t read_count;
        ssize_t written;

        if (source_target.size - offset < count)
            count = (size_t)(source_target.size - offset);
        read_count = pread(source_target.fd, buffer, count, (off_t)offset);
        if (read_count != (ssize_t)count)
            goto out;
        written = pwrite(destination_target.fd, buffer, count,
                         (off_t)offset);
        if (written != (ssize_t)count)
            goto out;
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