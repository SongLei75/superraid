#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <openssl/evp.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#if defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#endif

#include "hash.h"
#include "common.h"

#define RING_SLOTS 8

struct ring {
    struct {
        uint8_t *data;
        size_t len;
    } slots[RING_SLOTS];
    _Atomic uint32_t head;
    _Atomic uint32_t tail;
    _Atomic uint32_t done;
    _Atomic uint32_t error;
};

static double now_sec(void) {
    struct timespec ts = {0};

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        FSCTL_WARN("read monotonic clock for hash timing failed: %s",
                   strerror(errno));
        return 0.0;
    }
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static uint32_t crc32c_update(uint32_t crc, const uint8_t *data, size_t len) {
#if defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
    const uint8_t *p = data;

    while (len >= 8) {
        uint64_t value;
        memcpy(&value, p, sizeof(value));
        crc = __crc32cd(crc, value);
        p += 8;
        len -= 8;
    }
    if (len >= 4) {
        uint32_t value;
        memcpy(&value, p, sizeof(value));
        crc = __crc32cw(crc, value);
        p += 4;
        len -= 4;
    }
    if (len >= 2) {
        uint16_t value;
        memcpy(&value, p, sizeof(value));
        crc = __crc32ch(crc, value);
        p += 2;
        len -= 2;
    }
    if (len != 0) crc = __crc32cb(crc, *p);
    return crc;
#else
    const uint32_t polynomial = 0x82f63b78U;

    while (len-- != 0U) {
        uint32_t bit;

        crc ^= *data++;
        for (bit = 0; bit < 8U; ++bit)
            crc = (crc >> 1) ^ ((crc & 1U) ? polynomial : 0U);
    }
    return crc;
#endif
}

int32_t hash_crc32(const uint8_t *data, size_t len, uint8_t digest[4]) {
    uint32_t crc = ~crc32c_update(UINT32_MAX, data, len);

    put_le32(digest, crc);
    return 0;
}

int32_t hash_sha256(const uint8_t *data, size_t len, uint8_t digest[32]) {
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    unsigned int digest_len = 0;
    int32_t result = -1;

    if (ctx == NULL) {
        FSCTL_ERROR("EVP_MD_CTX_new failed");
        return -1;
    }
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) == 1 &&
        EVP_DigestUpdate(ctx, data, len) == 1 &&
        EVP_DigestFinal_ex(ctx, digest, &digest_len) == 1 &&
        digest_len == 32U) {
        result = 0;
    }
    EVP_MD_CTX_free(ctx);
    if (result != 0)
        FSCTL_ERROR("SHA-256 memory digest failed");
    return result;
}

struct producer_args {
    int32_t fd;
    uint64_t total_size;
    size_t chunk_size;
    struct ring *ring;
};

static void *producer_thread(void *arg) {
    struct producer_args *args = arg;
    uint64_t offset = 0;
    uint32_t tail = 0;

    while (offset < args->total_size) {
        size_t want = (args->total_size - offset < args->chunk_size)
                          ? (size_t)(args->total_size - offset)
                          : args->chunk_size;
        uint32_t head;
        uint32_t slot;
        size_t aligned_want;
        ssize_t bytes_read;

        do {
            head =
                atomic_load_explicit(&args->ring->head, memory_order_acquire);
            if (atomic_load(&args->ring->error)) return NULL;
            if (tail - head < RING_SLOTS) break;
            sched_yield();
        } while (1);

        slot = tail % RING_SLOTS;
        aligned_want = (want + 511U) & ~511U;
        bytes_read = pread(args->fd, args->ring->slots[slot].data, aligned_want,
                           (off_t)offset);
        if (bytes_read <= 0 || (uint64_t)bytes_read < want) {
            if (bytes_read == 0) errno = EIO;
            FSCTL_ERROR("hash pread failed offset=%" PRIu64 " want=%zu: %s",
                        offset, want, strerror(errno));
            atomic_store(&args->ring->error, 1);
            atomic_store(&args->ring->done, 1);
            return NULL;
        }

        args->ring->slots[slot].len = want;
        atomic_store_explicit(&args->ring->tail, tail + 1,
                              memory_order_release);
        offset += want;
        tail++;
    }

    atomic_store(&args->ring->done, 1);
    return NULL;
}

struct consumer_args {
    struct ring *ring;
    uint8_t digest[32];
    int32_t error;
};

static void *consumer_thread(void *arg) {
    struct consumer_args *args = arg;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    uint32_t head = 0;
    unsigned int digest_len = 0;

    if (ctx == NULL || EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1) {
        FSCTL_ERROR("SHA-256 consumer init failed");
        args->error = -1;
        EVP_MD_CTX_free(ctx);
        return NULL;
    }

    for (;;) {
        uint32_t tail;
        uint32_t slot;

        do {
            tail =
                atomic_load_explicit(&args->ring->tail, memory_order_acquire);
            if (head < tail) break;
            if (atomic_load(&args->ring->done) && head >= tail) goto finish;
            sched_yield();
        } while (1);

        slot = head % RING_SLOTS;
        if (EVP_DigestUpdate(ctx, args->ring->slots[slot].data,
                             args->ring->slots[slot].len) != 1) {
            FSCTL_ERROR("SHA-256 consumer update failed");
            args->error = -1;
            atomic_store(&args->ring->error, 1);
            atomic_store(&args->ring->done, 1);
            EVP_MD_CTX_free(ctx);
            return NULL;
        }
        head++;
        atomic_store_explicit(&args->ring->head, head, memory_order_release);
    }

finish:
    if (!atomic_load(&args->ring->error) &&
        (EVP_DigestFinal_ex(ctx, args->digest, &digest_len) != 1 ||
         digest_len != 32U)) {
        FSCTL_ERROR("SHA-256 consumer final failed");
        args->error = -1;
    }
    EVP_MD_CTX_free(ctx);
    return NULL;
}

int32_t hash_sha256_fd(int32_t fd, uint64_t len, uint8_t digest[32]) {
    size_t chunk_size;
    size_t slot_stride;
    size_t buffer_size;
    uint8_t *buffer;
    pthread_t producer;
    pthread_t consumer;
    struct ring ring = {0};
    struct producer_args producer_args;
    struct consumer_args consumer_args = {0};
    double start;
    double elapsed;
    uint32_t producer_created = 0U;
    uint32_t consumer_created = 0U;

    if (len == 0)
        return hash_sha256((const uint8_t *)"", 0, digest);
    FSCTL_DEBUG("hash start fd=%" PRId32 " bytes=%" PRIu64, fd, len);
    chunk_size = (size_t)DEFAULT_CHUNK_KIB * 1024U;
    slot_stride = (chunk_size + 511U) & ~511U;
    if (slot_stride < chunk_size || slot_stride > SIZE_MAX / RING_SLOTS) {
        FSCTL_ERROR("hash buffer size overflow chunk=%zu", chunk_size);
        return -1;
    }
    buffer_size = slot_stride * RING_SLOTS;

    if (posix_memalign((void **)&buffer, 512, buffer_size) != 0) {
        FSCTL_ERROR("hash buffer allocation failed size=%zu", buffer_size);
        return -1;
    }
    memset(buffer, 0, buffer_size);

    atomic_init(&ring.head, 0);
    atomic_init(&ring.tail, 0);
    atomic_init(&ring.done, 0);
    atomic_init(&ring.error, 0);
    for (uint32_t i = 0; i < RING_SLOTS; ++i)
        ring.slots[i].data = buffer + (size_t)i * slot_stride;

    producer_args.fd = fd;
    producer_args.total_size = len;
    producer_args.chunk_size = chunk_size;
    producer_args.ring = &ring;
    consumer_args.ring = &ring;

    start = now_sec();
    if (pthread_create(&consumer, NULL, consumer_thread, &consumer_args) == 0) {
        consumer_created = 1U;
    } else {
        FSCTL_ERROR("hash consumer thread create failed");
        atomic_store(&ring.error, 1);
        atomic_store(&ring.done, 1);
    }
    if (consumer_created &&
        pthread_create(&producer, NULL, producer_thread, &producer_args) == 0) {
        producer_created = 1U;
    } else {
        FSCTL_ERROR("hash producer thread create failed");
        atomic_store(&ring.error, 1);
        atomic_store(&ring.done, 1);
    }
    if (producer_created != 0U && pthread_join(producer, NULL) != 0) {
        FSCTL_ERROR("hash producer thread join failed");
        atomic_store(&ring.error, 1);
    }
    if (consumer_created != 0U && pthread_join(consumer, NULL) != 0) {
        FSCTL_ERROR("hash consumer thread join failed");
        atomic_store(&ring.error, 1);
    }
    elapsed = now_sec() - start;

    if (atomic_load(&ring.error) || consumer_args.error) {
        FSCTL_ERROR("hash calculation failed fd=%" PRId32, fd);
        free(buffer);
        return -1;
    }
    memcpy(digest, consumer_args.digest, 32);
    FSCTL_INFO("hash complete bytes=%" PRIu64 " chunk=%uKiB ring=%u "
               "time=%.3fs speed=%.1fMiB/s",
               len, DEFAULT_CHUNK_KIB, RING_SLOTS, elapsed,
               (double)(len / (1024U * 1024U)) / (elapsed + 1e-9));
    free(buffer);
    return 0;
}

void hash_to_hex(const uint8_t digest[32], char output[(32 * 2U) + 1U]) {
    const char digits[] = "0123456789abcdef";
    size_t i;
    for (i = 0; i < 32; ++i) {
        output[i * 2U] = digits[digest[i] >> 4];
        output[i * 2U + 1U] = digits[digest[i] & 0x0fU];
    }
    output[32 * 2U] = '\0';
}
