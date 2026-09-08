#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "common.h"
#include "footer.h"
#include "hash.h"

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

static const uint8_t descriptor_magic[8] = {'V', 'F', 'I', 'N',
                                            'T', 'E', 'G', '\0'};

static int32_t set_buffered_io(int32_t fd, int32_t *saved_flags) {
    *saved_flags = (int32_t)fcntl(fd, F_GETFL);
    if (*saved_flags < 0) {
        FSCTL_ERROR("F_GETFL failed fd=%" PRId32 ": %s", fd, strerror(errno));
        return -1;
    }
    if ((*saved_flags & O_DIRECT) != 0 &&
        fcntl(fd, F_SETFL, *saved_flags & ~O_DIRECT) != 0) {
        FSCTL_ERROR("clear O_DIRECT failed fd=%" PRId32 ": %s", fd,
                    strerror(errno));
        return -1;
    }
    return 0;
}

static int32_t restore_io_flags(int32_t fd, int32_t saved_flags) {
    if ((saved_flags & O_DIRECT) == 0)
        return 0;
    if (fcntl(fd, F_SETFL, saved_flags) != 0) {
        FSCTL_ERROR("restore fd flags failed fd=%" PRId32 ": %s", fd,
                    strerror(errno));
        return -1;
    }
    return 0;
}

static int32_t validate_descriptor(const uint8_t raw[DESCRIPTOR_SIZE],
                                   uint64_t protected_size,
                                   uint8_t digest[SHA256_DIGEST_SIZE]) {
    uint8_t copy[DESCRIPTOR_SIZE];
    uint8_t crc_digest[4];
    uint32_t stored_crc;
    uint32_t computed_crc;
    size_t i;

    stored_crc = get_le32(raw + DESC_CRC32C_OFFSET);
    memcpy(copy, raw, sizeof(copy));
    memset(copy + DESC_CRC32C_OFFSET, 0, sizeof(uint32_t));
    if (hash_crc32(copy, sizeof(copy), crc_digest) != 0) {
        FSCTL_ERROR("descriptor CRC32C calculation failed");
        return EXIT_METADATA_INVALID;
    }
    computed_crc = get_le32(crc_digest);
    if (computed_crc != stored_crc) {
        FSCTL_WARN("descriptor CRC32C mismatch");
        return EXIT_METADATA_INVALID;
    }
    if (memcmp(raw + DESC_MAGIC_OFFSET, descriptor_magic,
               sizeof(descriptor_magic)) != 0) {
        FSCTL_WARN("descriptor magic is invalid");
        return EXIT_METADATA_INVALID;
    }
    if (get_le32(raw + DESC_VERSION_OFFSET) != PROTOCOL_VERSION) {
        FSCTL_WARN("descriptor version is unsupported");
        return EXIT_METADATA_INVALID;
    }
    if (get_le32(raw + DESC_DESCRIPTOR_SIZE_OFFSET) != DESCRIPTOR_SIZE) {
        FSCTL_WARN("descriptor size is invalid");
        return EXIT_METADATA_INVALID;
    }
    if (get_le32(raw + DESC_HASH_ALGORITHM_OFFSET) != HASH_ALGORITHM_SHA256) {
        FSCTL_WARN("descriptor hash algorithm is unsupported");
        return EXIT_METADATA_INVALID;
    }
    if (get_le32(raw + DESC_DIGEST_LEN_OFFSET) != SHA256_DIGEST_SIZE) {
        FSCTL_WARN("descriptor digest length is invalid");
        return EXIT_METADATA_INVALID;
    }
    if (get_le64(raw + DESC_HASHED_SIZE_OFFSET) != protected_size) {
        FSCTL_WARN("descriptor hashed size does not match target");
        return EXIT_METADATA_INVALID;
    }
    for (i = DESC_RESERVED_OFFSET; i < DESCRIPTOR_SIZE; ++i) {
        if (raw[i] != 0U) {
            FSCTL_WARN("descriptor reserved bytes are nonzero");
            return EXIT_METADATA_INVALID;
        }
    }
    memcpy(digest, raw + DESC_DIGEST_OFFSET, SHA256_DIGEST_SIZE);
    return EXIT_OK;
}

int32_t read_descriptor(int32_t fd,
                        uint8_t digest[SHA256_DIGEST_SIZE]) {
    uint8_t raw[DESCRIPTOR_SIZE];
    uint64_t device_size;
    uint64_t protected_size;
    int32_t saved_flags;
    int32_t result;

    if (digest == NULL || get_fd_size(fd, &device_size) != 0 ||
        device_size <= FOOTER_AREA_SIZE) {
        if (digest == NULL)
            errno = EINVAL;
        FSCTL_ERROR("cannot determine descriptor device size fd=%" PRId32
                    ": %s", fd, strerror(errno));
        return EXIT_IO_ERROR;
    }
    protected_size = device_size - FOOTER_AREA_SIZE;
    FSCTL_DEBUG("read descriptor fd=%" PRId32 " device_size=%" PRIu64
                " protected_size=%" PRIu64, fd, device_size, protected_size);

    if (set_buffered_io(fd, &saved_flags) != 0)
        return EXIT_IO_ERROR;
    result = pread_full(fd, raw, sizeof(raw), device_size - DESCRIPTOR_SIZE);
    if (restore_io_flags(fd, saved_flags) != 0 && result == 0)
        result = -1;
    if (result != 0) {
        FSCTL_ERROR("descriptor read failed fd=%" PRId32, fd);
        return EXIT_IO_ERROR;
    }

    return validate_descriptor(raw, protected_size, digest);
}

int32_t write_descriptor(int32_t fd,
                         const uint8_t digest[SHA256_DIGEST_SIZE]) {
    uint8_t raw[DESCRIPTOR_SIZE];
    uint8_t crc_digest[4];
    uint64_t device_size;
    uint64_t protected_size;
    int32_t saved_flags;
    int32_t result;

    if (digest == NULL || get_fd_size(fd, &device_size) != 0 ||
        device_size <= FOOTER_AREA_SIZE) {
        if (digest == NULL)
            errno = EINVAL;
        FSCTL_ERROR("cannot determine descriptor device size fd=%" PRId32
                    ": %s", fd, strerror(errno));
        return EXIT_IO_ERROR;
    }
    protected_size = device_size - FOOTER_AREA_SIZE;
    FSCTL_DEBUG("write descriptor fd=%" PRId32 " device_size=%" PRIu64
                " protected_size=%" PRIu64, fd, device_size, protected_size);

    memset(raw, 0, sizeof(raw));
    memcpy(raw + DESC_MAGIC_OFFSET, descriptor_magic, sizeof(descriptor_magic));
    put_le32(raw + DESC_VERSION_OFFSET, PROTOCOL_VERSION);
    put_le32(raw + DESC_DESCRIPTOR_SIZE_OFFSET, DESCRIPTOR_SIZE);
    put_le32(raw + DESC_HASH_ALGORITHM_OFFSET, HASH_ALGORITHM_SHA256);
    put_le32(raw + DESC_DIGEST_LEN_OFFSET, SHA256_DIGEST_SIZE);
    put_le64(raw + DESC_HASHED_SIZE_OFFSET, protected_size);
    memcpy(raw + DESC_DIGEST_OFFSET, digest, SHA256_DIGEST_SIZE);
    if (hash_crc32(raw, sizeof(raw), crc_digest) != 0) {
        FSCTL_ERROR("descriptor CRC32C calculation failed");
        return EXIT_IO_ERROR;
    }
    put_le32(raw + DESC_CRC32C_OFFSET, get_le32(crc_digest));

    if (set_buffered_io(fd, &saved_flags) != 0)
        return EXIT_IO_ERROR;
    result = pwrite_full(fd, raw, sizeof(raw), device_size - DESCRIPTOR_SIZE);
    if (restore_io_flags(fd, saved_flags) != 0 && result == 0)
        result = -1;
    if (result != 0) {
        FSCTL_ERROR("descriptor write failed fd=%" PRId32, fd);
        return EXIT_IO_ERROR;
    }
    if (fsync(fd) != 0) {
        FSCTL_ERROR("descriptor fsync failed fd=%" PRId32 ": %s", fd,
                    strerror(errno));
        return EXIT_IO_ERROR;
    }
    FSCTL_INFO("descriptor updated fd=%" PRId32 " protected_size=%" PRIu64,
               fd, protected_size);
    return EXIT_OK;
}
