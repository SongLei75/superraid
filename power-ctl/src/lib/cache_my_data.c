#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/openat2.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/mman.h>
#include <signal.h>
#include <stdbool.h>

typedef int (*libc_open_t)(const char *, int, ...);
typedef int (*libc_openat_t)(int, const char *, int, ...);
typedef int (*libc_creat_t)(const char *, mode_t);
typedef int (*libc_fcntl_t)(int, int, ...);
typedef int (*libc_fsync_t)(int);
typedef void (*libc_sync_t)(void);
typedef int (*libc_fdatasync_t)(int);
typedef int (*libc_msync_t)(void *, size_t, int);
typedef int (*libc_sync_file_range_t)(int, off64_t, off64_t, unsigned int);
typedef int (*libc_syncfs_t)(int);
typedef ssize_t (*libc_pwritev2_t)(int, const struct iovec *, int, off64_t,
                                   int);
static libc_open_t libc_open = NULL;
static libc_openat_t libc_openat = NULL;
static libc_creat_t libc_creat = NULL;
static libc_fcntl_t libc_fcntl = NULL;
static libc_fsync_t libc_fsync = NULL;
static libc_sync_t libc_sync = NULL;
static libc_fdatasync_t libc_fdatasync = NULL;
static libc_msync_t libc_msync = NULL;
static libc_sync_file_range_t libc_sync_file_range = NULL;
static libc_syncfs_t libc_syncfs = NULL;
static libc_pwritev2_t libc_pwritev64v2 = NULL;

static volatile sig_atomic_t force_pagecache = 1;

#define ASSIGN_DLSYM(target, type, symbol)                              \
    target = (type)(intptr_t)dlsym(RTLD_NEXT, symbol);                  \
    if (!target) {                                                      \
        const char *dlerror_str = dlerror();                            \
        fprintf(stderr, "libeatmydata init error for %s: %s\n",         \
                symbol, dlerror_str ? dlerror_str : "(null)");          \
        _exit(1);                                                       \
    }

static void eatmydata_init(void)
{
    ASSIGN_DLSYM(libc_open, libc_open_t, "open");
    ASSIGN_DLSYM(libc_openat, libc_openat_t, "openat");
    ASSIGN_DLSYM(libc_creat, libc_creat_t, "creat");
    ASSIGN_DLSYM(libc_fcntl, libc_fcntl_t, "fcntl");
    ASSIGN_DLSYM(libc_fsync, libc_fsync_t, "fsync");
    ASSIGN_DLSYM(libc_sync, libc_sync_t, "sync");
    ASSIGN_DLSYM(libc_fdatasync, libc_fdatasync_t, "fdatasync");
    ASSIGN_DLSYM(libc_msync, libc_msync_t, "msync");
    ASSIGN_DLSYM(libc_sync_file_range, libc_sync_file_range_t,
                 "sync_file_range");
    ASSIGN_DLSYM(libc_syncfs, libc_syncfs_t, "syncfs");
    ASSIGN_DLSYM(libc_pwritev64v2, libc_pwritev2_t, "pwritev64v2");
}

static void disable_force_pagecache(int signo)
{
    (void)signo;
    force_pagecache = 0;
}

static void eatmydata_init_once(void) __attribute__((constructor));

static void eatmydata_init_once(void)
{
    struct sigaction action = {
        .sa_handler = disable_force_pagecache,
    };

    sigemptyset(&action.sa_mask);
    eatmydata_init();
    if (sigaction(SIGUSR2, &action, NULL) != 0)
        _exit(1);
}

int fsync(int fd)
{
    if (!force_pagecache)
        return libc_fsync(fd);
    (void)fd;
    errno = EOPNOTSUPP;
    return -1;
}

void sync(void)
{
    if (!force_pagecache) {
        libc_sync();
        return;
    }
    errno = EOPNOTSUPP;
}

int open(const char *pathname, int flags, ...)
{
    int needs_mode;
    mode_t mode = 0;

    if (force_pagecache && (flags & (O_DIRECT | O_SYNC | O_DSYNC))) {
        errno = EOPNOTSUPP;
        return -1;
    }

    needs_mode = (flags & O_CREAT) != 0 || (flags & O_TMPFILE) == O_TMPFILE;
    if (needs_mode) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
        return libc_open(pathname, flags, mode);
    }
    return libc_open(pathname, flags);
}


int openat(int dirfd, const char *pathname, int flags, ...)
{
    int needs_mode;
    mode_t mode = 0;

    if (force_pagecache && (flags & (O_DIRECT | O_SYNC | O_DSYNC))) {
        errno = EOPNOTSUPP;
        return -1;
    }

    needs_mode = (flags & O_CREAT) != 0 || (flags & O_TMPFILE) == O_TMPFILE;
    if (needs_mode) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, mode_t);
        va_end(ap);
        return libc_openat(dirfd, pathname, flags, mode);
    }
    return libc_openat(dirfd, pathname, flags);
}

int openat2(int dirfd, const char *pathname, const struct open_how *how,
            size_t size)
{
#ifdef SYS_openat2
    struct open_how sanitized;

    if (!how || size != sizeof(sanitized)) {
        errno = EINVAL;
        return -1;
    }
    sanitized = *how;
    if (force_pagecache &&
        (sanitized.flags & ((uint64_t)O_DIRECT | (uint64_t)O_SYNC |
                            (uint64_t)O_DSYNC))) {
        errno = EOPNOTSUPP;
        return -1;
    }
    return (int)syscall(SYS_openat2, dirfd, pathname, &sanitized, size);
#else
    (void)dirfd;
    (void)pathname;
    (void)how;
    (void)size;
    errno = ENOSYS;
    return -1;
#endif
}

int creat(const char *pathname, mode_t mode)
{
    /* creat 固定 O_WRONLY|O_CREAT|O_TRUNC, 天然无 direct/sync flag, 直接透传 */
    return libc_creat(pathname, mode);
}


int fcntl(int fd, int cmd, ...)
{
    va_list ap;
    unsigned long arg = 0;

    va_start(ap, cmd);
    if (cmd == F_GETFD || cmd == F_GETFL) {
        va_end(ap);
        return libc_fcntl(fd, cmd);
    }
    if (cmd == F_SETFL) {
        int fl = va_arg(ap, int);
        va_end(ap);
        if (force_pagecache && (fl & (O_DIRECT | O_SYNC | O_DSYNC))) {
            errno = EOPNOTSUPP;
            return -1;
        }
        return libc_fcntl(fd, cmd, fl);
    }
    arg = va_arg(ap, unsigned long);
    va_end(ap);
    return libc_fcntl(fd, cmd, arg);
}

ssize_t pwritev64v2(int fd, const struct iovec *iov, int iovcnt,
                    off64_t offset, int flags)
{
    if (force_pagecache && (flags & (RWF_SYNC | RWF_DSYNC))) {
        errno = EOPNOTSUPP;
        return -1;
    }
    return libc_pwritev64v2(fd, iov, iovcnt, offset, flags);
}


int fdatasync(int fd)
{
    if (!force_pagecache)
        return libc_fdatasync(fd);
    (void)fd;
    errno = EOPNOTSUPP;
    return -1;
}

int msync(void *addr, size_t length, int flags)
{
    if (!force_pagecache)
        return libc_msync(addr, length, flags);
    (void)addr;
    (void)length;
    (void)flags;
    errno = EOPNOTSUPP;
    return -1;
}

int sync_file_range(int fd, off64_t offset, off64_t nbytes, unsigned int flags)
{
    if (!force_pagecache)
        return libc_sync_file_range(fd, offset, nbytes, flags);
    (void)fd;
    (void)offset;
    (void)nbytes;
    (void)flags;
    errno = EOPNOTSUPP;
    return -1;
}

int syncfs(int fd)
{
    if (!force_pagecache)
        return libc_syncfs(fd);
    (void)fd;
    errno = EOPNOTSUPP;
    return -1;
}
