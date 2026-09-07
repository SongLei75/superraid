#ifndef POWERCTL_STORAGE_H
#define POWERCTL_STORAGE_H

#include <stddef.h>
#include <stdint.h>
#include <sys/ioctl.h>

#include "hash.h"

#define FAT_IOCTL_DISABLE_WRITE _IO('r', 0x14)

struct app_param_state {
    char *mounted_dev;
    char *other_dev;
    uint8_t mounted_digest[SHA256_DIGEST_SIZE];
};

struct target {
    int fd;
    uint64_t size;
    unsigned int logical_size;
};

int powerctl_disable_vfat_write(const char *mountpoint);
int powerctl_seal_block_device(const char *path, int chunk_kib,
                               uint8_t digest[SHA256_DIGEST_SIZE],
                               uint64_t *device_size);
int powerctl_copy_block_device(const char *source, const char *destination);
int powerctl_resolve_active_peer(const char *mountpoint, const char *device_a,
                                 const char *device_b, const char **active,
                                 const char **peer);
int powerctl_resolve_partition_source(const char *partition, char *parent,
                                      size_t parent_size,
                                      uint64_t *source_offset,
                                      uint64_t *partition_size);
int powerctl_copy_block_range(const char *source, uint64_t source_offset,
                              uint64_t length, const char *destination);
int powerctl_verify_path_digest(const char *path, int chunk_kib,
                                uint8_t digest[SHA256_DIGEST_SIZE]);
int mount_partition(struct app_param_state *state, const char *devices,
                    const char *mountpoint, int chunk_kib);
int umount_partition(const char *path);
int recover_app_param(struct app_param_state *state, const char *mountpoint,
                      int chunk_kib);
int open_target(const char *path, int flags, struct target *target);
void close_target(struct target *target);
#endif
