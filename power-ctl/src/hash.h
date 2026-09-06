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

int hash_sha256(const uint8_t *data, size_t len, uint8_t digest[32]);

/* CRC32C (Castagnoli), result is written as little-endian digest[4]. */
int hash_crc32(const uint8_t *data, size_t len, uint8_t digest[4]);

/* SHA-256 over fd [0, len); chunk_kib <= 0 selects the 8 MiB default. */
int hash_sha256_fd(int fd, uint64_t len, uint8_t digest[32], int chunk_kib);

#endif /* HASH_H */
