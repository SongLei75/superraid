#ifndef POWERCTL_STORAGE_H
#define POWERCTL_STORAGE_H

#include <sys/ioctl.h>
#include <stdint.h>

#define FAT_IOCTL_DISABLE_WRITE _IO('r', 0x14)

int powerctl_disable_vfat_write(const char *mountpoint);
int powerctl_seal_block_device(const char *path, int chunk_kib,
							   uint8_t digest[32], uint64_t *device_size);
int powerctl_copy_block_device(const char *source, const char *destination);

#endif
