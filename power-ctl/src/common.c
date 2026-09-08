#define _GNU_SOURCE

#include <errno.h>
#include <stdarg.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include <linux/fs.h>

#define SD_JOURNAL_SUPPRESS_LOCATION
#include <systemd/sd-journal.h>

#include "common.h"

int32_t fsctl_log(uint32_t priority, const char *file, const char *line,
                  const char *func, const char *format, ...) {
    char message[1024];
    char message_field[sizeof(message) + sizeof("MESSAGE=")];
    char priority_field[24];
    char file_field[PATH_MAX + sizeof("CODE_FILE=")];
    char line_field[64];
    char func_field[256];
    struct iovec fields[6];
    va_list args;
    int32_t length;

    if (file == NULL || line == NULL || func == NULL || format == NULL ||
        priority > FSCTL_LOG_DEBUG) {
        return -EINVAL;
    }

    va_start(args, format);
    length = (int32_t)vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    if (length < 0)
        return -EINVAL;

    (void)snprintf(message_field, sizeof(message_field), "MESSAGE=%s", message);
    (void)snprintf(priority_field, sizeof(priority_field), "PRIORITY=%" PRIu32,
                   priority);

    (void)snprintf(file_field, sizeof(file_field), "CODE_FILE=%s", file);
    (void)snprintf(line_field, sizeof(line_field), "CODE_LINE=%s", line);
    (void)snprintf(func_field, sizeof(func_field), "CODE_FUNC=%s", func);

    fields[0].iov_base = message_field;
    fields[0].iov_len = strlen(message_field);
    fields[1].iov_base = priority_field;
    fields[1].iov_len = strlen(priority_field);
    fields[2].iov_base = file_field;
    fields[2].iov_len = strlen(file_field);
    fields[3].iov_base = line_field;
    fields[3].iov_len = strlen(line_field);
    fields[4].iov_base = func_field;
    fields[4].iov_len = strlen(func_field);
    fields[5].iov_base = (void *)"SYSLOG_IDENTIFIER=fsctl";
    fields[5].iov_len = strlen((const char *)fields[5].iov_base);

    return (int32_t)sd_journal_sendv(
        fields, (int)(sizeof(fields) / sizeof(fields[0])));
}

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

int32_t get_fd_size(int32_t fd, uint64_t *size) {
    struct stat st;

    if (fd < 0 || size == NULL) {
        errno = EINVAL;
        FSCTL_ERROR("invalid fd size request fd=%" PRId32, fd);
        return -1;
    }
    if (fstat(fd, &st) != 0) {
        FSCTL_ERROR("fstat failed fd=%" PRId32 ": %s", fd, strerror(errno));
        return -1;
    }

    if (S_ISBLK(st.st_mode)) {
        uint64_t bytes;

        if (ioctl(fd, BLKGETSIZE64, &bytes) != 0) {
            FSCTL_ERROR("BLKGETSIZE64 failed fd=%" PRId32 ": %s", fd,
                        strerror(errno));
            return -1;
        }
        *size = bytes;
        return 0;
    }
    if (S_ISREG(st.st_mode) && st.st_size >= 0) {
        *size = (uint64_t)st.st_size;
        return 0;
    }

    errno = EINVAL;
    FSCTL_ERROR("unsupported fd type fd=%" PRId32, fd);
    return -1;
}

int32_t pread_full(int32_t fd, void *buffer, size_t length, uint64_t offset) {
    uint8_t *p = buffer;
    size_t done = 0;

    while (done < length) {
        ssize_t count =
            pread(fd, p + done, length - done, (off_t)(offset + done));
        if (count < 0) {
            if (errno == EINTR)
                continue;
            FSCTL_ERROR("pread failed fd=%" PRId32 " offset=%" PRIu64
                        " length=%zu: %s",
                        fd, offset + done, length - done, strerror(errno));
            return -1;
        }
        if (count == 0) {
            errno = EIO;
            FSCTL_ERROR("unexpected EOF fd=%" PRId32 " offset=%" PRIu64,
                        fd, offset + done);
            return -1;
        }
        done += (size_t)count;
    }
    return 0;
}

int32_t pwrite_full(int32_t fd, const void *buffer, size_t length,
                    uint64_t offset) {
    const uint8_t *p = buffer;
    size_t done = 0;

    while (done < length) {
        ssize_t count =
            pwrite(fd, p + done, length - done, (off_t)(offset + done));
        if (count < 0) {
            if (errno == EINTR)
                continue;
            FSCTL_ERROR("pwrite failed fd=%" PRId32 " offset=%" PRIu64
                        " length=%zu: %s",
                        fd, offset + done, length - done, strerror(errno));
            return -1;
        }
        if (count == 0) {
            errno = EIO;
            FSCTL_ERROR("short pwrite fd=%" PRId32 " offset=%" PRIu64, fd,
                        offset + done);
            return -1;
        }
        done += (size_t)count;
    }
    return 0;
}
