/*
 * app_param_test_loop.c - continuous buffered write test.
 *
 * Each iteration opens the same file, writes one 4 KiB block, closes it,
 * then waits one second.  The write offset advances to 1 MiB and wraps to
 * zero without truncating the file on subsequent wraps.
 *
 * Usage:
 *   app_param_test_loop [root] [file]
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_ROOT "/mnt"
#define DEFAULT_FILE "app_param_test_loop.bin"
#define WRITE_BYTES 4096U
#define MAX_BYTES (1024U * 1024U)
#define FILE_MODE 0666

static volatile sig_atomic_t running = 1;

static void stop_loop(int signal_number)
{
  (void)signal_number;
  running = 0;
}

static int build_path(char* path, size_t path_size, const char* root,
                      const char* file_name)
{
  int written = snprintf(path, path_size, "%s/%s", root, file_name);

  return written < 0 || (size_t)written >= path_size ? -1 : 0;
}

static int wait_one_second(void)
{
  struct timespec delay = {.tv_sec = 1, .tv_nsec = 0};

  while (running && nanosleep(&delay, &delay) != 0) {
    if (errno != EINTR)
      return -1;
  }
  return 0;
}

int main(int argc, char** argv)
{
  const char* root = argc > 1 ? argv[1] : DEFAULT_ROOT;
  const char* file_name = argc > 2 ? argv[2] : DEFAULT_FILE;
  char path[512];
  unsigned char buffer[WRITE_BYTES];
  unsigned int offset = 0;
  uint64_t iteration = 0;
  struct sigaction action = {
      .sa_handler = stop_loop,
  };

  if (argc > 3) {
    fprintf(stderr, "usage: %s [root] [file]\n", argv[0]);
    return 2;
  }
  if (build_path(path, sizeof(path), root, file_name) != 0) {
    fprintf(stderr, "path is too long\n");
    return 2;
  }

  sigemptyset(&action.sa_mask);
  if (sigaction(SIGINT, &action, NULL) != 0 ||
      sigaction(SIGTERM, &action, NULL) != 0) {
    fprintf(stderr, "sigaction: errno=%d error=%s\n", errno,
            strerror(errno));
    return 1;
  }

  memset(buffer, 0, sizeof(buffer));
  printf("LOOP_START path=%s chunk=%u max=%u interval=1s\n", path,
         WRITE_BYTES, MAX_BYTES);
  fflush(stdout);

  while (running) {
    int flags = O_CREAT | O_WRONLY | O_CLOEXEC;
    int fd;
    ssize_t written;

    if (iteration == 0)
      flags |= O_TRUNC;

    fd = open(path, flags, FILE_MODE);
    if (fd < 0) {
      fprintf(stderr, "open(%s): errno=%d error=%s\n", path, errno,
              strerror(errno));
      return 1;
    }
    if (lseek(fd, (off_t)offset, SEEK_SET) < 0) {
      fprintf(stderr, "lseek(%s, %u): errno=%d error=%s\n", path, offset,
              errno, strerror(errno));
      close(fd);
      return 1;
    }

    memset(buffer, (unsigned char)(iteration & 0xffU), sizeof(buffer));
    written = write(fd, buffer, sizeof(buffer));
    if (written != (ssize_t)sizeof(buffer)) {
      int error = errno;

      fprintf(stderr, "write(%s, %u): result=%zd errno=%d error=%s\n", path,
              offset, written, error, strerror(error));
      close(fd);
      return 1;
    }
    if (close(fd) != 0) {
      fprintf(stderr, "close(%s): errno=%d error=%s\n", path, errno,
              strerror(errno));
      return 1;
    }

    printf("LOOP_WRITE iteration=%llu offset=%u bytes=%u\n",
           (unsigned long long)iteration, offset, WRITE_BYTES);
    fflush(stdout);
    offset += WRITE_BYTES;
    if (offset == MAX_BYTES)
      offset = 0;
    iteration++;

    if (wait_one_second() != 0) {
      fprintf(stderr, "nanosleep: errno=%d error=%s\n", errno,
              strerror(errno));
      return 1;
    }
  }

  printf("LOOP_STOP iterations=%llu next_offset=%u\n",
         (unsigned long long)iteration, offset);
  return 0;
}
