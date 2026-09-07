#define _GNU_SOURCE

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include "hash.h"
#include "footer.h"
#include "common.h"

static const uint8_t descriptor_magic[8] = {'V', 'F', 'I', 'N',
                                            'T', 'E', 'G', '\0'};

int validate_descriptor(const uint8_t descriptor[DESCRIPTOR_SIZE],
                        uint64_t expected_hashed_size,
                        uint8_t digest[SHA256_DIGEST_SIZE]) {
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
        fprintf(stderr, "%s: descriptor version is unsupported\n",
                PROGRAM_NAME);
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

int read_descriptor(int fd, struct footer_descriptor *descriptor) {
    uint8_t raw[DESCRIPTOR_SIZE];

    if (pread_full(fd, raw, sizeof(raw),
                   FOOTER_AREA_SIZE - DESCRIPTOR_SIZE) != 0)
        return -1;

    memcpy(descriptor->magic, raw + DESC_MAGIC_OFFSET,
           sizeof(descriptor->magic));
    descriptor->version = get_le32(raw + DESC_VERSION_OFFSET);
    descriptor->descriptor_size =
        get_le32(raw + DESC_DESCRIPTOR_SIZE_OFFSET);
    descriptor->hash_algorithm = get_le32(raw + DESC_HASH_ALGORITHM_OFFSET);
    descriptor->digest_len = get_le32(raw + DESC_DIGEST_LEN_OFFSET);
    descriptor->hashed_size = get_le64(raw + DESC_HASHED_SIZE_OFFSET);
    memcpy(descriptor->digest, raw + DESC_DIGEST_OFFSET,
           sizeof(descriptor->digest));
    descriptor->crc32c = get_le32(raw + DESC_CRC32C_OFFSET);
    memcpy(descriptor->reserved, raw + DESC_RESERVED_OFFSET,
           sizeof(descriptor->reserved));
    return 0;
}

int write_descriptor(int fd, struct footer_descriptor *descriptor) {
    uint8_t raw[DESCRIPTOR_SIZE];
    size_t done = 0;

    memset(raw, 0, sizeof(raw));
    memcpy(raw + DESC_MAGIC_OFFSET, descriptor->magic,
           sizeof(descriptor->magic));
    put_le32(raw + DESC_VERSION_OFFSET, descriptor->version);
    put_le32(raw + DESC_DESCRIPTOR_SIZE_OFFSET, descriptor->descriptor_size);
    put_le32(raw + DESC_HASH_ALGORITHM_OFFSET, descriptor->hash_algorithm);
    put_le32(raw + DESC_DIGEST_LEN_OFFSET, descriptor->digest_len);
    put_le64(raw + DESC_HASHED_SIZE_OFFSET, descriptor->hashed_size);
    memcpy(raw + DESC_DIGEST_OFFSET, descriptor->digest,
           sizeof(descriptor->digest));
    put_le32(raw + DESC_CRC32C_OFFSET, descriptor->crc32c);
    memcpy(raw + DESC_RESERVED_OFFSET, descriptor->reserved,
           sizeof(descriptor->reserved));

    while (done < sizeof(raw)) {
        ssize_t count = pwrite(fd, raw + done, sizeof(raw) - done,
                               (off_t)(FOOTER_AREA_SIZE - DESCRIPTOR_SIZE +
                                       done));
        if (count < 0) {
            if (errno == EINTR)
                continue;
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

int write_footer(int fd, uint64_t protected_size,
                 const uint8_t digest[SHA256_DIGEST_SIZE]) {
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
    if (hash_crc32(descriptor, DESCRIPTOR_SIZE, crc_digest) != 0) return -1;
    put_le32(descriptor + DESC_CRC32C_OFFSET, get_le32(crc_digest));

    saved_flags = fcntl(fd, F_GETFL);
    if (saved_flags < 0) return -1;
    if (saved_flags & O_DIRECT &&
        fcntl(fd, F_SETFL, saved_flags & ~O_DIRECT) < 0)
        return -1;
    if (pwrite(fd, footer, sizeof(footer), (off_t)protected_size) !=
        (ssize_t)sizeof(footer))
        return -1;
    return fsync(fd);
}
