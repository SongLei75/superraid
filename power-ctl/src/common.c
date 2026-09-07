#include <errno.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/types.h>

uint32_t get_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

uint64_t get_le64(const uint8_t *p) {
    return (uint64_t)get_le32(p) | ((uint64_t)get_le32(p + 4) << 32);
}

void put_le32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

void put_le64(uint8_t *p, uint64_t value) {
    put_le32(p, (uint32_t)value);
    put_le32(p + 4, (uint32_t)(value >> 32));
}

int pread_full(int fd, void *buffer, size_t length, uint64_t offset) {
    uint8_t *p = buffer;
    size_t done = 0;

    while (done < length) {
        ssize_t count =
            pread(fd, p + done, length - done, (off_t)(offset + done));
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
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
