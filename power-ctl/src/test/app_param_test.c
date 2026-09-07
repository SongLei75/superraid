/*
 * app_param_test.c - audit userspace app_param write and sync paths.
 *
 * Run the same command with and without LD_PRELOAD. Every case reports its
 * class, durability semantics and observed result for direct comparison.
 *
 * Usage:
 *   app_param_test [root] [all|case]
 *   app_param_test [root] [1|yes|true|on|off]
 *   app_param_test [root] delay SECONDS case
 *   app_param_test [root] [all|case] ROUNDS DELAY_SECONDS
 */

#define _GNU_SOURCE

#ifndef AT_FDCWD
#define AT_FDCWD (-100)
#endif

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <linux/openat2.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#ifndef RWF_DIRECT
/* Compatibility value used by the preload test; not a standard Linux flag. */
#define RWF_DIRECT 0x00000020
#endif

#define SYNC_FILE_RANGE_WAIT_BEFORE 1
#define SYNC_FILE_RANGE_WRITE 2
#define SYNC_FILE_RANGE_WAIT_AFTER 4
#define PAGE_BYTES 4096
#define PAYLOAD "app_param_test_payload\n"

static volatile sig_atomic_t running = 1;
static unsigned int observed_allowed;
static unsigned int observed_rejected;
static unsigned int observed_skipped;
static unsigned int harness_failed;

struct case_info;
typedef int (*case_fn)(const char* root, const struct case_info* info);

struct case_info {
  const char* name;
  const char* class_name;
  const char* durability;
  const char* api;
  case_fn run;
  int value;
};

static void on_signal(int sig) {
  (void)sig;
  running = 0;
}

static int is_true_word(const char* value) {
  return strcmp(value, "1") == 0 || strcmp(value, "yes") == 0 ||
         strcmp(value, "true") == 0 || strcmp(value, "on") == 0;
}

static int wait_seconds(unsigned int seconds) {
  unsigned int remaining = seconds;
  printf("READY pid=%ld delay=%u\n", (long)getpid(), seconds);
  fflush(stdout);
  while (remaining != 0 && running) {
    remaining = sleep(remaining);
  }
  return running ? 0 : -1;
}

static const char* run_mode(void) {
  const char* preload = getenv("LD_PRELOAD");
  return preload && *preload ? "preload" : "baseline";
}

static int make_path(char* path,
                     size_t path_size,
                     const char* root,
                     const char* name) {
  int written = snprintf(path, path_size, "%s/%s", root, name);
  return written < 0 || (size_t)written >= path_size ? -1 : 0;
}

static int open_file(const char* root, const char* name, int flags) {
  char path[512];

  if (make_path(path, sizeof(path), root, name) != 0) {
    errno = ENAMETOOLONG;
    return -1;
  }
  return open(path, flags | O_CLOEXEC, 0666);
}

static void remove_file(const char* root, const char* name) {
  char path[512];

  if (make_path(path, sizeof(path), root, name) == 0) unlink(path);
}

static const char* observed_status(long result, int error) {
  if (result >= 0) {
    observed_allowed++;
    return "ALLOWED";
  }
  if (error == ENOSYS) {
    observed_skipped++;
    return "SKIP";
  }
  observed_rejected++;
  return "REJECTED";
}

static void report_observation(const struct case_info* info,
                               long result,
                               int error,
                               const char* detail) {
  const char* status = observed_status(result, error);

  printf("CASE=%s mode=%s class=%s durability=%s api=%s observed=%s",
         info->name,
         run_mode(),
         info->class_name,
         info->durability,
         info->api,
         status);
  if (result < 0) printf(" errno=%d error=%s", error, strerror(error));
  printf(" detail=%s\n", detail);
}

static int report_setup_failure(const struct case_info* info,
                                int error,
                                const char* detail) {
  harness_failed++;
  report_observation(info, -1, error, detail);
  return -1;
}

static int write_buffered_case(const char* root,
                               const struct case_info* info,
                               int use_pwrite) {
  const char payload[] = PAYLOAD;
  const ssize_t expected = (ssize_t)sizeof(payload) - 1;
  int fd = open_file(root, info->name, O_CREAT | O_RDWR | O_TRUNC);
  ssize_t written;
  int error;

  if (fd < 0) return report_setup_failure(info, errno, "buffered open");
  written = use_pwrite ? pwrite(fd, payload, (size_t)expected, 0)
                       : write(fd, payload, (size_t)expected);
  error = errno;
  close(fd);
  remove_file(root, info->name);
  report_observation(info,
                     written == expected ? 0 : -1,
                     written == expected ? 0 : error,
                     use_pwrite ? "buffered pwrite" : "buffered write");
  return 0;
}

static int case_write(const char* root, const struct case_info* info) {
  return write_buffered_case(root, info, 0);
}

static int case_pwrite(const char* root, const struct case_info* info) {
  return write_buffered_case(root, info, 1);
}

static int case_writev(const char* root, const struct case_info* info) {
  const char first[] = "writev_part_a\n";
  const char second[] = "writev_part_b\n";
  struct iovec iov[2] = {
      {(void*)first, sizeof(first) - 1},
      {(void*)second, sizeof(second) - 1},
  };
  const ssize_t expected = (ssize_t)(sizeof(first) + sizeof(second) - 2);
  int fd = open_file(root, info->name, O_CREAT | O_RDWR | O_TRUNC);
  ssize_t written;
  int error;

  if (fd < 0) return report_setup_failure(info, errno, "buffered open");
  written = writev(fd, iov, 2);
  error = errno;
  close(fd);
  remove_file(root, info->name);
  report_observation(info,
                     written == expected ? 0 : -1,
                     written == expected ? 0 : error,
                     "buffered writev");
  return 0;
}

static int case_pwritev(const char* root, const struct case_info* info) {
  const char first[] = "pwritev_part_a\n";
  const char second[] = "pwritev_part_b\n";
  struct iovec iov[2] = {
      {(void*)first, sizeof(first) - 1},
      {(void*)second, sizeof(second) - 1},
  };
  const ssize_t expected = (ssize_t)(sizeof(first) + sizeof(second) - 2);
  int fd = open_file(root, info->name, O_CREAT | O_RDWR | O_TRUNC);
  ssize_t written;
  int error;

  if (fd < 0) return report_setup_failure(info, errno, "buffered open");
  written = pwritev(fd, iov, 2, 0);
  error = errno;
  close(fd);
  remove_file(root, info->name);
  report_observation(info,
                     written == expected ? 0 : -1,
                     written == expected ? 0 : error,
                     "buffered pwritev");
  return 0;
}

#if 0 /* Audit-only: direct syscall bypasses LD_PRELOAD. */
static int case_raw_write(const char* root, const struct case_info* info) {
#ifdef SYS_write
  const char payload[] = PAYLOAD;
  int fd = open_file(root, info->name, O_CREAT | O_RDWR | O_TRUNC);
  ssize_t written;
  int error;

  if (fd < 0) return report_setup_failure(info, errno, "buffered open");
  written = syscall(SYS_write, fd, payload, sizeof(payload) - 1);
  error = errno;
  close(fd);
  remove_file(root, info->name);
  report_observation(info,
                     written == (ssize_t)sizeof(payload) - 1 ? 0 : -1,
                     written == (ssize_t)sizeof(payload) - 1 ? 0 : error,
                     "raw syscall(SYS_write) bypasses LD_PRELOAD");
  return 0;
#else
  report_observation(info, -1, ENOSYS, "SYS_write unavailable");
  return 0;
#endif
}

static int case_raw_pwrite(const char* root, const struct case_info* info) {
#ifdef SYS_pwrite64
  const char payload[] = PAYLOAD;
  int fd = open_file(root, info->name, O_CREAT | O_RDWR | O_TRUNC);
  ssize_t written;
  int error;

  if (fd < 0) return report_setup_failure(info, errno, "buffered open");
  written = syscall(SYS_pwrite64, fd, payload, sizeof(payload) - 1, 0);
  error = errno;
  close(fd);
  remove_file(root, info->name);
  report_observation(info,
                     written == (ssize_t)sizeof(payload) - 1 ? 0 : -1,
                     written == (ssize_t)sizeof(payload) - 1 ? 0 : error,
                     "raw syscall(SYS_pwrite64) bypasses LD_PRELOAD");
  return 0;
#else
  report_observation(info, -1, ENOSYS, "SYS_pwrite64 unavailable");
  return 0;
#endif
}
#endif

static int case_append(const char* root, const struct case_info* info) {
  const char payload[] = PAYLOAD;
  int fd = open_file(root, info->name, O_CREAT | O_WRONLY | O_APPEND);
  ssize_t written;
  int error;

  if (fd < 0) return report_setup_failure(info, errno, "append open");
  written = write(fd, payload, sizeof(payload) - 1);
  error = errno;
  close(fd);
  remove_file(root, info->name);
  report_observation(info,
                     written == (ssize_t)sizeof(payload) - 1 ? 0 : -1,
                     written == (ssize_t)sizeof(payload) - 1 ? 0 : error,
                     "buffered O_APPEND write");
  return 0;
}

static int case_stdio(const char* root, const struct case_info* info) {
  const char payload[] = PAYLOAD;
  int fd = open_file(root, info->name, O_CREAT | O_WRONLY | O_TRUNC);
  FILE* stream;
  size_t written;
  int result;
  int error;

  if (fd < 0) return report_setup_failure(info, errno, "stdio open");
  stream = fdopen(fd, "w");
  if (!stream) {
    error = errno;
    close(fd);
    remove_file(root, info->name);
    return report_setup_failure(info, error, "fdopen");
  }
  written = fwrite(payload, 1, sizeof(payload) - 1, stream);
  result =
      info->value ? fflush(stream) : (written == sizeof(payload) - 1 ? 0 : -1);
  error = errno;
  fclose(stream);
  remove_file(root, info->name);
  report_observation(info,
                     result == 0 ? 0 : -1,
                     result == 0 ? 0 : error,
                     info->value ? "fwrite + fflush; fflush is not durable"
                                 : "fwrite user-space buffer + fclose");
  return 0;
}

static int case_sync_open(const char* root, const struct case_info* info) {
  const char payload[] = PAYLOAD;
  int fd =
      open_file(root, info->name, O_CREAT | O_RDWR | O_TRUNC | info->value);
  ssize_t written;
  int error;

  if (fd < 0) {
    report_observation(info, -1, errno, "synchronous open rejected");
    remove_file(root, info->name);
    return 0;
  }
  written = write(fd, payload, sizeof(payload) - 1);
  error = errno;
  close(fd);
  remove_file(root, info->name);
  report_observation(info,
                     written == (ssize_t)sizeof(payload) - 1 ? 0 : -1,
                     written == (ssize_t)sizeof(payload) - 1 ? 0 : error,
                     info->value == O_SYNC ? "O_SYNC write" : "O_DSYNC write");
  return 0;
}

static ssize_t direct_write(int fd) {
  void* buffer = NULL;
  ssize_t written;
  int error;

  if (posix_memalign(&buffer, PAGE_BYTES, PAGE_BYTES) != 0) {
    errno = ENOMEM;
    return -1;
  }
  memset(buffer, 0x5a, PAGE_BYTES);
  written = pwrite(fd, buffer, PAGE_BYTES, 0);
  error = errno;
  free(buffer);
  errno = error;
  return written;
}

static int case_direct_open(const char* root, const struct case_info* info) {
  int fd = open_file(root, info->name, O_CREAT | O_RDWR | O_TRUNC | O_DIRECT);
  ssize_t written;
  int error;

  if (fd < 0) {
    report_observation(info, -1, errno, "O_DIRECT open");
    remove_file(root, info->name);
    return 0;
  }
  written = direct_write(fd);
  error = errno;
  close(fd);
  remove_file(root, info->name);
  report_observation(info,
                     written == PAGE_BYTES ? 0 : -1,
                     written == PAGE_BYTES ? 0 : error,
                     "O_DIRECT open + aligned pwrite");
  return 0;
}

static int case_direct_fcntl(const char* root, const struct case_info* info) {
  int fd = open_file(root, info->name, O_CREAT | O_RDWR | O_TRUNC);
  int ret;
  int error;
  ssize_t written = -1;

  if (fd < 0) return report_setup_failure(info, errno, "buffered open");
  ret = fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_DIRECT);
  error = errno;
  if (ret == 0) {
    written = direct_write(fd);
    error = errno;
  }
  close(fd);
  remove_file(root, info->name);
  report_observation(info,
                     written == PAGE_BYTES ? 0 : -1,
                     written == PAGE_BYTES ? 0 : error,
                     ret == 0 ? "F_SETFL O_DIRECT + aligned pwrite"
                              : "F_SETFL O_DIRECT rejected");
  return 0;
}

static int case_openat_direct(const char* root, const struct case_info* info) {
  char path[512];
  int fd;
  ssize_t written;
  int error;

  if (make_path(path, sizeof(path), root, info->name) != 0)
    return report_setup_failure(info, ENAMETOOLONG, "path");
  fd = openat(AT_FDCWD, path, O_CREAT | O_RDWR | O_TRUNC | O_DIRECT, 0666);
  if (fd < 0) {
    report_observation(info, -1, errno, "openat O_DIRECT");
    unlink(path);
    return 0;
  }
  written = direct_write(fd);
  error = errno;
  close(fd);
  unlink(path);
  report_observation(info,
                     written == PAGE_BYTES ? 0 : -1,
                     written == PAGE_BYTES ? 0 : error,
                     "openat O_DIRECT + aligned pwrite");
  return 0;
}

static int case_creat(const char* root, const struct case_info* info) {
  char path[512];
  const char payload[] = PAYLOAD;
  int fd;
  ssize_t written;
  int error;

  if (make_path(path, sizeof(path), root, info->name) != 0)
    return report_setup_failure(info, ENAMETOOLONG, "path");
  fd = creat(path, 0666);
  if (fd < 0) return report_setup_failure(info, errno, "creat");
  written = write(fd, payload, sizeof(payload) - 1);
  error = errno;
  close(fd);
  unlink(path);
  report_observation(info,
                     written == (ssize_t)sizeof(payload) - 1 ? 0 : -1,
                     written == (ssize_t)sizeof(payload) - 1 ? 0 : error,
                     "creat + buffered write");
  return 0;
}

static int case_sync(const char* root, const struct case_info* info) {
  int error;

  (void)root;
  errno = 0;
  sync();
  error = errno;
  report_observation(info,
                     error == EOPNOTSUPP ? -1 : 0,
                     error == EOPNOTSUPP ? error : 0,
                     "global sync; void ABI, errno is observed only");
  return 0;
}

#if 0 /* Audit-only: raw syscall openat2 bypasses LD_PRELOAD. */
static int case_openat2_direct(const char* root, const struct case_info* info) {
#ifdef SYS_openat2
  struct open_how how = {
      .flags = O_CREAT | O_RDWR | O_TRUNC | O_DIRECT,
      .mode = 0666,
      .resolve = 0,
  };
  char path[512];
  int fd;
  ssize_t written;
  int error;

  if (make_path(path, sizeof(path), root, info->name) != 0)
    return report_setup_failure(info, ENAMETOOLONG, "path");
  fd = (int)syscall(SYS_openat2, AT_FDCWD, path, &how, sizeof(how));
  if (fd < 0) {
    report_observation(
        info, -1, errno, "raw openat2 O_DIRECT bypasses LD_PRELOAD");
    unlink(path);
    return 0;
  }
  written = direct_write(fd);
  error = errno;
  close(fd);
  unlink(path);
  report_observation(info,
                     written == PAGE_BYTES ? 0 : -1,
                     written == PAGE_BYTES ? 0 : error,
                     "raw openat2 + aligned pwrite");
  return 0;
#else
  report_observation(info, -1, ENOSYS, "openat2 unavailable");
  return 0;
#endif
}
#endif

static int case_pwritev2(const char* root, const struct case_info* info) {
  struct iovec iov = {(void*)PAYLOAD, sizeof(PAYLOAD) - 1};
  int fd = open_file(root, info->name, O_CREAT | O_RDWR | O_TRUNC);
  ssize_t written;
  int error;

  if (fd < 0) return report_setup_failure(info, errno, "buffered open");
  written = pwritev2(fd, &iov, 1, 0, info->value);
  error = errno;
  close(fd);
  remove_file(root, info->name);
  report_observation(
      info,
      written == (ssize_t)sizeof(PAYLOAD) - 1 ? 0 : -1,
      written == (ssize_t)sizeof(PAYLOAD) - 1 ? 0 : error,
      info->value == 0 ? "pwritev2 buffered write" : "pwritev2 flagged write");
  return 0;
}

static int case_mmap(const char* root, const struct case_info* info) {
  int fd = open_file(root, info->name, O_CREAT | O_RDWR | O_TRUNC);
  void* mapping;
  int error;

  if (fd < 0) return report_setup_failure(info, errno, "mmap open");
  if (ftruncate(fd, PAGE_BYTES) != 0) {
    error = errno;
    close(fd);
    remove_file(root, info->name);
    return report_setup_failure(info, error, "ftruncate");
  }
  mapping = mmap(NULL, PAGE_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (mapping == MAP_FAILED) {
    error = errno;
    close(fd);
    remove_file(root, info->name);
    return report_setup_failure(info, error, "mmap");
  }
  memcpy(mapping, PAYLOAD, sizeof(PAYLOAD) - 1);
  munmap(mapping, PAGE_BYTES);
  close(fd);
  remove_file(root, info->name);
  report_observation(info, 0, 0, "MAP_SHARED dirty pages; no explicit sync");
  return 0;
}

static int case_msync(const char* root, const struct case_info* info) {
  int fd = open_file(root, info->name, O_CREAT | O_RDWR | O_TRUNC);
  void* mapping;
  int ret;
  int error;

  if (fd < 0) return report_setup_failure(info, errno, "mmap open");
  if (ftruncate(fd, PAGE_BYTES) != 0) {
    error = errno;
    close(fd);
    remove_file(root, info->name);
    return report_setup_failure(info, error, "ftruncate");
  }
  mapping = mmap(NULL, PAGE_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (mapping == MAP_FAILED) {
    error = errno;
    close(fd);
    remove_file(root, info->name);
    return report_setup_failure(info, error, "mmap");
  }
  memcpy(mapping, PAYLOAD, sizeof(PAYLOAD) - 1);
  ret = msync(mapping, PAGE_BYTES, MS_SYNC);
  error = errno;
  munmap(mapping, PAGE_BYTES);
  close(fd);
  remove_file(root, info->name);
  report_observation(
      info, ret == 0 ? 0 : -1, ret == 0 ? 0 : error, "MAP_SHARED + MS_SYNC");
  return 0;
}

static int case_sync_range(const char* root, const struct case_info* info) {
  const char payload[] = PAYLOAD;
  const ssize_t expected = (ssize_t)sizeof(payload) - 1;
  int fd = open_file(root, info->name, O_CREAT | O_RDWR | O_TRUNC);
  ssize_t written;
  int ret;
  int error;

  if (fd < 0) return report_setup_failure(info, errno, "buffered open");
  written = write(fd, payload, (size_t)expected);
  ret = written == expected ? sync_file_range(fd,
                                              0,
                                              PAGE_BYTES,
                                              SYNC_FILE_RANGE_WAIT_BEFORE |
                                                  SYNC_FILE_RANGE_WRITE |
                                                  SYNC_FILE_RANGE_WAIT_AFTER)
                            : -1;
  error = errno;
  close(fd);
  remove_file(root, info->name);
  report_observation(info,
                     ret == 0 ? 0 : -1,
                     ret == 0 ? 0 : error,
                     "buffered write + sync_file_range");
  return 0;
}

static int case_file_sync(const char* root, const struct case_info* info) {
  int fd = open_file(root, info->name, O_CREAT | O_RDWR | O_TRUNC);
  int ret;
  int error;

  if (fd < 0) return report_setup_failure(info, errno, "buffered open");
  if (pwrite(fd, PAYLOAD, sizeof(PAYLOAD) - 1, 0) < 0) {
    error = errno;
    close(fd);
    remove_file(root, info->name);
    return report_setup_failure(info, error, "buffered pwrite");
  }
  if (info->value == 1)
    ret = fsync(fd);
  else if (info->value == 2)
    ret = fdatasync(fd);
  else
    ret = syncfs(fd);
  error = errno;
  close(fd);
  remove_file(root, info->name);
  report_observation(info,
                     ret == 0 ? 0 : -1,
                     ret == 0 ? 0 : error,
                     info->value == 1   ? "pwrite + fsync"
                     : info->value == 2 ? "pwrite + fdatasync"
                                        : "pwrite + syncfs");
  return 0;
}

#if 0 /* Audit-only: direct syscall fsync bypasses LD_PRELOAD. */
static int case_raw_fsync(const char* root, const struct case_info* info) {
#ifdef SYS_fsync
  int fd = open_file(root, info->name, O_CREAT | O_RDWR | O_TRUNC);
  int ret;
  int error;

  if (fd < 0) return report_setup_failure(info, errno, "buffered open");
  if (pwrite(fd, PAYLOAD, sizeof(PAYLOAD) - 1, 0) < 0) {
    error = errno;
    close(fd);
    remove_file(root, info->name);
    return report_setup_failure(info, error, "buffered pwrite");
  }
  ret = (int)syscall(SYS_fsync, fd);
  error = errno;
  close(fd);
  remove_file(root, info->name);
  report_observation(info,
                     ret == 0 ? 0 : -1,
                     ret == 0 ? 0 : error,
                     "raw syscall(SYS_fsync) bypasses LD_PRELOAD");
  return 0;
#else
  report_observation(info, -1, ENOSYS, "SYS_fsync unavailable");
  return 0;
#endif
}
#endif

static int case_ftruncate(const char* root, const struct case_info* info) {
  int fd = open_file(root, info->name, O_CREAT | O_RDWR | O_TRUNC);
  int ret;
  int error;

  if (fd < 0) return report_setup_failure(info, errno, "metadata open");
  ret = ftruncate(fd, PAGE_BYTES);
  error = errno;
  close(fd);
  remove_file(root, info->name);
  report_observation(info,
                     ret == 0 ? 0 : -1,
                     ret == 0 ? 0 : error,
                     "ftruncate changes persistent file metadata");
  return 0;
}

static int case_fallocate(const char* root, const struct case_info* info) {
  int fd = open_file(root, info->name, O_CREAT | O_RDWR | O_TRUNC);
  int ret;
  int error;

  if (fd < 0) return report_setup_failure(info, errno, "allocation open");
  ret = info->value ? posix_fallocate(fd, 0, PAGE_BYTES)
                    : fallocate(fd, 0, 0, PAGE_BYTES);
  error = errno;
  close(fd);
  remove_file(root, info->name);
  report_observation(
      info,
      ret == 0 ? 0 : -1,
      ret == 0 ? 0 : (ret ? ret : error),
      info->value ? "posix_fallocate allocation" : "fallocate allocation");
  return 0;
}

static const struct case_info all_cases[] = {
    {"write", "BUFFERED", "MAY_WRITEBACK", "write", case_write, 0},
    {"pwrite", "BUFFERED", "MAY_WRITEBACK", "pwrite", case_pwrite, 0},
    {"writev", "BUFFERED", "MAY_WRITEBACK", "writev", case_writev, 0},
    {"pwritev", "BUFFERED", "MAY_WRITEBACK", "pwritev", case_pwritev, 0},
    /* Raw syscall write/pwrite bypass LD_PRELOAD and are not app APIs. */
    {"append", "BUFFERED", "MAY_WRITEBACK", "O_APPEND+write", case_append, 0},
    {"stdio-fwrite", "BUFFERED", "MAY_WRITEBACK", "fwrite", case_stdio, 0},
    {"stdio-fflush", "BUFFERED", "NOT_DURABLE", "fwrite+fflush", case_stdio, 1},
    {"mmap", "MMAP", "MAY_WRITEBACK", "MAP_SHARED", case_mmap, 0},
    {"sync-open",
     "SYNC_ON_WRITE",
     "WRITE_SYNC",
     "O_SYNC+write",
     case_sync_open,
     O_SYNC},
    {"dsync-open",
     "SYNC_ON_WRITE",
     "WRITE_SYNC",
     "O_DSYNC+write",
     case_sync_open,
     O_DSYNC},
    {"direct-open",
     "DIRECT",
     "DIRECT_IO",
     "O_DIRECT+pwrite",
     case_direct_open,
     0},
    {"direct-fcntl",
     "DIRECT",
     "DIRECT_IO",
     "F_SETFL(O_DIRECT)+pwrite",
     case_direct_fcntl,
     0},
    {"openat-direct",
     "DIRECT",
     "DIRECT_IO",
     "openat(O_DIRECT)+pwrite",
     case_openat_direct,
     0},
    {"creat", "BUFFERED", "MAY_WRITEBACK", "creat+write", case_creat, 0},
    /* raw openat2(O_DIRECT) bypasses LD_PRELOAD and is not an app API. */
    {"pwritev2-buffered",
     "BUFFERED",
     "MAY_WRITEBACK",
     "pwritev2",
     case_pwritev2,
     0},
    {"pwritev2-sync",
     "SYNC_ON_WRITE",
     "WRITE_SYNC",
     "pwritev2(RWF_SYNC)",
     case_pwritev2,
     RWF_SYNC},
    {"pwritev2-dsync",
     "SYNC_ON_WRITE",
     "WRITE_SYNC",
     "pwritev2(RWF_DSYNC)",
     case_pwritev2,
     RWF_DSYNC},
    {"pwritev2-direct",
     "DIRECT_OR_UNKNOWN",
     "FLAG_DEPENDENT",
     "pwritev2(0x20)",
     case_pwritev2,
     RWF_DIRECT},
    {"msync", "MMAP_SYNC", "EXPLICIT_SYNC", "msync(MS_SYNC)", case_msync, 0},
    {"sync-file-range",
     "RANGE_SYNC",
     "EXPLICIT_WRITEBACK",
     "sync_file_range",
     case_sync_range,
     0},
    {"fsync", "FILE_SYNC", "EXPLICIT_SYNC", "fsync", case_file_sync, 1},
    {"fdatasync", "FILE_SYNC", "EXPLICIT_SYNC", "fdatasync", case_file_sync, 2},
    {"syncfs", "FILESYSTEM_SYNC", "EXPLICIT_SYNC", "syncfs", case_file_sync, 3},
    {"sync", "GLOBAL_SYNC", "EXPLICIT_SYNC", "sync", case_sync, 0},
    {"ftruncate", "METADATA", "MAY_WRITEBACK", "ftruncate", case_ftruncate, 0},
    {"fallocate",
     "ALLOCATION",
     "MAY_WRITEBACK",
     "fallocate",
     case_fallocate,
     0},
    {"posix-fallocate",
     "ALLOCATION",
     "MAY_WRITEBACK",
     "posix_fallocate",
     case_fallocate,
     1},
};

static const struct case_info* find_case(const char* name) {
  size_t i;

  for (i = 0; i < sizeof(all_cases) / sizeof(all_cases[0]); ++i)
    if (strcmp(name, all_cases[i].name) == 0) return &all_cases[i];
  return NULL;
}

static int run_all_cases(const char* root, unsigned int round) {
  size_t i;

  observed_allowed = 0;
  observed_rejected = 0;
  observed_skipped = 0;
  harness_failed = 0;
  printf("ROUND_BEGIN round=%u total=%zu\n",
         round,
         sizeof(all_cases) / sizeof(all_cases[0]));
  for (i = 0; i < sizeof(all_cases) / sizeof(all_cases[0]); ++i)
    all_cases[i].run(root, &all_cases[i]);
  printf(
      "ROUND_SUMMARY round=%u total=%zu allowed=%u rejected=%u skipped=%u "
      "failed=%u\n",
      round,
      sizeof(all_cases) / sizeof(all_cases[0]),
      observed_allowed,
      observed_rejected,
      observed_skipped,
      harness_failed);
  return harness_failed == 0 ? 0 : 1;
}

static int run_rounds(const char* root,
                      const char* selected,
                      unsigned int rounds,
                      unsigned int delay) {
  const struct case_info* info;
  unsigned int round;

  if (rounds == 0) {
    fprintf(stderr, "rounds must be greater than zero\n");
    return 1;
  }
  for (round = 1; round <= rounds; ++round) {
    if (round > 1 && wait_seconds(delay) != 0) return 1;
    if (strcmp(selected, "all") == 0) {
      if (run_all_cases(root, round) != 0) return 1;
    } else {
      info = find_case(selected);
      if (!info) {
        fprintf(stderr, "unknown case: %s\n", selected);
        return 1;
      }
      observed_allowed = 0;
      observed_rejected = 0;
      observed_skipped = 0;
      harness_failed = 0;
      printf("ROUND_BEGIN round=%u total=1\n", round);
      info->run(root, info);
      printf(
          "ROUND_SUMMARY round=%u total=1 allowed=%u rejected=%u "
          "skipped=%u failed=%u\n",
          round,
          observed_allowed,
          observed_rejected,
          observed_skipped,
          harness_failed);
      if (harness_failed != 0) return 1;
    }
  }
  return 0;
}

static int run_continuous(const char* root, int do_fsync) {
  char held_path[512];
  char write_path[512];
  int held_fd;
  int fd;
  off_t size = 0;
  unsigned long long seq = 0;

  if (make_path(held_path, sizeof(held_path), root, "held_file.txt") != 0 ||
      make_path(write_path, sizeof(write_path), root, "write_file.txt") != 0)
    return 1;
  held_fd = open(held_path, O_CREAT | O_RDWR | O_CLOEXEC, 0666);
  fd = open(write_path, O_CREAT | O_RDWR | O_TRUNC | O_CLOEXEC, 0666);
  if (fd < 0) {
    perror("open write_file");
    if (held_fd >= 0) close(held_fd);
    return 1;
  }
  printf("RUN mode=%s class=CONTINUOUS api=pwrite%s\n",
         run_mode(),
         do_fsync ? "+fsync" : "");
  while (running) {
    char buffer[256];
    int count = snprintf(
        buffer, sizeof(buffer), "seq=%llu ts=%ld\n", seq, (long)time(NULL));

    if (size + count > (off_t)(64 * 1024 * 1024)) {
      if (ftruncate(fd, 0) != 0) perror("ftruncate");
      size = 0;
    }
    if (pwrite(fd, buffer, (size_t)count, size) != count) break;
    size += count;
    if (do_fsync && fsync(fd) != 0) break;
    ++seq;
  }
  close(fd);
  if (held_fd >= 0) close(held_fd);
  printf("CONTINUOUS stopped seq=%llu final_size=%lld\n", seq, (long long)size);
  return 0;
}

int main(int argc, char** argv) {
  const char* root = argc > 1 ? argv[1] : "/app_param";
  const char* selected = argc > 2 ? argv[2] : "all";
  const struct case_info* info;
  char* end = NULL;
  unsigned long delay;
  unsigned long rounds;

  signal(SIGTERM, on_signal);
  signal(SIGINT, on_signal);
  printf("RUN mode=%s root=%s selected=%s\n", run_mode(), root, selected);
  if (argc > 2 && is_true_word(selected)) return run_continuous(root, 1);
  if (argc > 2 && strcmp(selected, "off") == 0) return run_continuous(root, 0);
  if (argc > 3 && strcmp(selected, "delay") == 0) {
    errno = 0;
    delay = strtoul(argv[3], &end, 10);
    if (errno != 0 || end == argv[3] || *end != '\0' || delay > 86400 ||
        argc < 5 || wait_seconds((unsigned int)delay) != 0)
      return 1;
    selected = argv[4];
  }
  if (argc > 4 && strcmp(selected, "delay") != 0) {
    errno = 0;
    rounds = strtoul(argv[3], &end, 10);
    if (errno != 0 || end == argv[3] || *end != '\0' || rounds > 100000 ||
        strcmp(argv[4], "") == 0)
      return 1;
    errno = 0;
    delay = strtoul(argv[4], &end, 10);
    if (errno != 0 || end == argv[4] || *end != '\0' || delay > 86400) return 1;
    return run_rounds(
        root, selected, (unsigned int)rounds, (unsigned int)delay);
  }
  if (strcmp(selected, "all") == 0) return run_all_cases(root, 1);
  info = find_case(selected);
  if (!info) {
    fprintf(stderr, "unknown case: %s\n", selected);
    return 1;
  }
  info->run(root, info);
  printf("CASE_SUMMARY total=1 allowed=%u rejected=%u skipped=%u failed=%u\n",
         observed_allowed,
         observed_rejected,
         observed_skipped,
         harness_failed);
  return harness_failed == 0 ? 0 : 1;
}
