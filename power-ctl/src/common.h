#ifndef COMMON_H
#define COMMON_H

#include <stddef.h>
#include <stdint.h>

#define PROGRAM_NAME "fsctl"

#define FSCTL_LOG_ERROR 3U
#define FSCTL_LOG_WARN 4U
#define FSCTL_LOG_NOTICE 5U
#define FSCTL_LOG_DEBUG 7U

#define FSCTL_STRINGIFY_(x) #x
#define FSCTL_STRINGIFY(x) FSCTL_STRINGIFY_(x)

#define FSCTL_LOG(level, ...)                                                \
    fsctl_log((level), __FILE__, FSCTL_STRINGIFY(__LINE__), __func__,         \
              __VA_ARGS__)
#define FSCTL_ERROR(...) FSCTL_LOG(FSCTL_LOG_ERROR, __VA_ARGS__)
#define FSCTL_WARN(...) FSCTL_LOG(FSCTL_LOG_WARN, __VA_ARGS__)
#define FSCTL_INFO(...) FSCTL_LOG(FSCTL_LOG_NOTICE, __VA_ARGS__)
#define FSCTL_DEBUG(...) FSCTL_LOG(FSCTL_LOG_DEBUG, __VA_ARGS__)

#define EXIT_OK 0
#define EXIT_HASH_MISMATCH 2
#define EXIT_METADATA_INVALID 3
#define EXIT_IO_ERROR 4
#define EXIT_DEVICE_BUSY 5
#define EXIT_NOMEM 6
#define EXIT_UNSUPPORTED_LAYOUT 7

int32_t fsctl_log(uint32_t priority, const char *file, const char *line,
                  const char *func, const char *format, ...)
    __attribute__((format(printf, 5, 6)));

uint32_t get_le32(const uint8_t *p);
uint64_t get_le64(const uint8_t *p);
void put_le32(uint8_t *p, uint32_t value);
void put_le64(uint8_t *p, uint64_t value);

int32_t get_fd_size(int32_t fd, uint64_t *size);
int32_t pread_full(int32_t fd, void *buffer, size_t length, uint64_t offset);
int32_t pwrite_full(int32_t fd, const void *buffer, size_t length,
                    uint64_t offset);

#endif  // COMMON_H
