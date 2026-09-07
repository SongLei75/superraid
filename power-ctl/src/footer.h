#ifndef FOOTER_H
#define FOOTER_H

#include <stdint.h>

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

struct footer_descriptor {
    uint8_t magic[8];
    uint32_t version;
    uint32_t descriptor_size;
    uint32_t hash_algorithm;
    uint32_t digest_len;
    uint64_t hashed_size;
    uint8_t digest[32];
    uint32_t crc32c;
    uint8_t reserved[60];
};

int validate_descriptor(const uint8_t descriptor[DESCRIPTOR_SIZE],
                        uint64_t expected_hashed_size, uint8_t digest[32]);
int read_descriptor(int fd, struct footer_descriptor *descriptor);
int write_descriptor(int fd, struct footer_descriptor *descriptor);
int write_footer(int fd, uint64_t protected_size, const uint8_t digest[32]);
#endif  // FOOTER_H
