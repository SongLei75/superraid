/*
 * hash.h - 双线程哈希计算库接口
 *
 * 供 footer_verity 等工具调用的哈希 API。
 * 实现: hash.c (双线程无锁 SPSC ring buffer)
 */
#ifndef HASH_H
#define HASH_H

#include <stddef.h>
#include <stdint.h>

#define SHA256_DIGEST_SIZE 32
#define DEFAULT_CHUNK_KIB 8192

int32_t hash_sha256(const uint8_t *data, size_t len,
                uint8_t digest[SHA256_DIGEST_SIZE]);

int32_t hash_crc32(const uint8_t *data, size_t len, uint8_t digest[4]);

int32_t hash_sha256_fd(int32_t fd, uint64_t len,
                   uint8_t digest[SHA256_DIGEST_SIZE]);

void hash_to_hex(const uint8_t digest[32], char output[(32 * 2U) + 1U]);

#endif /* HASH_H */
