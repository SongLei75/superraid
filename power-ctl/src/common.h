#ifndef COMMON_H
#define COMMON_H

#define PROGRAM_NAME "fsctl"

enum {
    EXIT_OK = 0,
    EXIT_HASH_MISMATCH = 2,
    EXIT_METADATA_INVALID = 3,
    EXIT_IO_ERROR = 4,
    EXIT_DEVICE_BUSY = 5,
    EXIT_NOMEM = 6,
    EXIT_UNSUPPORTED_LAYOUT = 7
};

uint32_t get_le32(const uint8_t *p);
uint64_t get_le64(const uint8_t *p);
void put_le32(uint8_t *p, uint32_t value);
void put_le64(uint8_t *p, uint64_t value);

int pread_full(int fd, void *buffer, size_t length, uint64_t offset);

#endif  // COMMON_H
