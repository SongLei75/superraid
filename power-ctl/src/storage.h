#ifndef POWERCTL_STORAGE_H
#define POWERCTL_STORAGE_H

#include <stddef.h>
#include <stdint.h>
#include <sys/ioctl.h>

#define FAT_IOCTL_DISABLE_WRITE _IO('r', 0x14)

int powerctl_disable_vfat_write(const char *mountpoint);
int powerctl_seal_block_device(const char *path, int chunk_kib,
                               uint8_t digest[32], uint64_t *device_size);
int powerctl_copy_block_device(const char *source, const char *destination);
int powerctl_resolve_active_peer(const char *mountpoint,
                                 const char *device_a,
                                 const char *device_b,
                                 const char **active,
                                 const char **peer);
int powerctl_resolve_partition_source(const char *partition,
                                      char *parent, size_t parent_size,
                                      uint64_t *source_offset,
                                      uint64_t *partition_size);
int powerctl_copy_block_range(const char *source, uint64_t source_offset,
                              uint64_t length, const char *destination);

#endif
