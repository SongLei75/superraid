#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <openssl/evp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <systemd/sd-daemon.h>
#include <systemd/sd-bus.h>
#include <time.h>
#include <unistd.h>

#include "hash.h"
#include "storage.h"
#include "footer.h"
#include "common.h"

static void print_usage(FILE *stream) {
    fprintf(stream,
            "Usage:\n"
            "  %s [--algo sha256] [--chunk KiB] seal DEVICE\n"
            "  %s [--algo sha256] [--chunk KiB] verify DEVICE\n"
            "  %s [--chunk KiB] mount A:B MOUNTPOINT\n"
            "  %s mount MOUNTPOINT\n"
            "  %s umount MOUNTPOINT\n"
            "\n"
            "seal/verify DEVICE must be an unmounted Linux block device.\n"
            "mount A:B MOUNTPOINT mounts a valid app_param device and\n"
            "recovers the peer device when its footer hash differs.\n"
            "mount MOUNTPOINT flushes and unmounts the filesystem.\n"
            "--algo: compatibility option; only sha256 is supported.\n"
            "--chunk: read buffer size in KiB per ring slot (default 8192).\n"
            "umount flushes the mounted filesystem with syncfs() and performs "
            "a normal umount.\n",
            PROGRAM_NAME, PROGRAM_NAME, PROGRAM_NAME, PROGRAM_NAME,
            PROGRAM_NAME);
}

int main(int argc, char **argv) {
    int cmd_idx = 1; /* index of subcommand in argv */

    if (argc < cmd_idx + 1) {
        print_usage(stderr);
        return EXIT_UNSUPPORTED_LAYOUT;
    }
    if (strcmp(argv[cmd_idx], "--help") == 0 ||
        strcmp(argv[cmd_idx], "-h") == 0) {
        print_usage(stdout);
        return EXIT_OK;
    }
    if (strcmp(argv[cmd_idx], "seal") == 0 && argc == cmd_idx + 2) {
        uint8_t digest[32];
        uint64_t device_size;
        char digest_hex[(32 * 2U) + 1U];

        if (powerctl_seal_block_device(argv[cmd_idx + 1], DEFAULT_CHUNK_KIB,
                                       digest, &device_size) != 0)
            return EXIT_IO_ERROR;
        hash_to_hex(digest, digest_hex);
        printf("seal ok, device_size=%" PRIu64 " protected_size=%" PRIu64
               " digest=%s\n",
               device_size, device_size - FOOTER_AREA_SIZE, digest_hex);
        return EXIT_OK;
    }
    if (strcmp(argv[cmd_idx], "verify") == 0 && argc == cmd_idx + 2) {
        uint8_t digest[32];
        return powerctl_verify_path_digest(argv[cmd_idx + 1], DEFAULT_CHUNK_KIB,
                                           digest);
    }
    if (strcmp(argv[cmd_idx], "mount") == 0 && argc == cmd_idx + 3) {
        printf("start mounting %s to %s\n", argv[cmd_idx + 1],
               argv[cmd_idx + 2]);
        struct app_param_state state = {0};
        int result = mount_partition(&state, argv[cmd_idx + 1],
                                     argv[cmd_idx + 2], DEFAULT_CHUNK_KIB);
        if (result != EXIT_OK) {
            return result;
        }
        result =
            recover_app_param(&state, argv[cmd_idx + 2], DEFAULT_CHUNK_KIB);
        free(state.mounted_dev);
        free(state.other_dev);
        return result;
    }
    if (strcmp(argv[cmd_idx], "umount") == 0 && argc == cmd_idx + 2) {
        return umount_partition(argv[cmd_idx + 1]);
    }

    print_usage(stderr);
    return EXIT_UNSUPPORTED_LAYOUT;
}
