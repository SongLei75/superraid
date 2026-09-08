#ifndef POWERCTL_STORAGE_H
#define POWERCTL_STORAGE_H

#include <stdint.h>

#include "hash.h"

struct app_param_state {
    char *mounted_dev;
    char *other_dev;
    uint8_t mounted_digest[SHA256_DIGEST_SIZE];
};

int32_t powerctl_seal_block_device(const char *path,
                                   uint8_t digest[SHA256_DIGEST_SIZE],
                                   uint64_t *device_size);
int32_t powerctl_verify_path_digest(const char *path,
                                    uint8_t digest[SHA256_DIGEST_SIZE]);
int32_t mount_partition(struct app_param_state *state, const char *devices,
                        const char *mountpoint);
int32_t umount_partition(const char *devices, const char *mountpoint);
int32_t recover_app_param(struct app_param_state *state);

#endif  // POWERCTL_STORAGE_H
