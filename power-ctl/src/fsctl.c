#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "footer.h"
#include "hash.h"
#include "storage.h"

static void print_usage(FILE *stream) {
    fprintf(stream,
            "Usage:\n"
            "  %s seal DEVICE\n"
            "  %s verify DEVICE\n"
            "  %s mount A:B MOUNTPOINT\n"
            "  %s umount A:B MOUNTPOINT\n"
            "\n"
            "seal/verify DEVICE must be an unmounted Linux block device.\n"
            "mount verifies A/B, mounts a valid device, and recovers its peer.\n"
            "umount freezes the mounted filesystem, seals the active device,\n"
            "and copies the active device to its peer for power transition.\n",
            PROGRAM_NAME, PROGRAM_NAME, PROGRAM_NAME, PROGRAM_NAME);
}

int main(int argc, char **argv) {
    const int32_t cmd_idx = 1;

    if (argc <= cmd_idx) {
        FSCTL_WARN("missing command argument");
        print_usage(stderr);
        return EXIT_UNSUPPORTED_LAYOUT;
    }
    if (strcmp(argv[cmd_idx], "--help") == 0 ||
        strcmp(argv[cmd_idx], "-h") == 0) {
        print_usage(stdout);
        return EXIT_OK;
    }
    if (strcmp(argv[cmd_idx], "seal") == 0 && argc == cmd_idx + 2) {
        uint8_t digest[SHA256_DIGEST_SIZE];
        uint64_t device_size;
        char digest_hex[(SHA256_DIGEST_SIZE * 2U) + 1U];

        if (powerctl_seal_block_device(argv[cmd_idx + 1], digest,
                                       &device_size) != 0) {
            FSCTL_ERROR("seal command failed device=%s", argv[cmd_idx + 1]);
            return EXIT_IO_ERROR;
        }
        hash_to_hex(digest, digest_hex);
        FSCTL_INFO("seal command complete device=%s size=%" PRIu64
                   " protected=%" PRIu64 " digest=%s",
                   argv[cmd_idx + 1], device_size,
                   device_size - FOOTER_AREA_SIZE, digest_hex);
        return EXIT_OK;
    }
    if (strcmp(argv[cmd_idx], "verify") == 0 && argc == cmd_idx + 2) {
        uint8_t digest[SHA256_DIGEST_SIZE];
        return powerctl_verify_path_digest(argv[cmd_idx + 1], digest);
    }
    if (strcmp(argv[cmd_idx], "mount") == 0 && argc == cmd_idx + 3) {
        struct app_param_state state = {0};
        int32_t result;

        result = mount_partition(&state, argv[cmd_idx + 1],
                                 argv[cmd_idx + 2]);
        if (result != EXIT_OK)
            return result;
        result = recover_app_param(&state);
        free(state.mounted_dev);
        free(state.other_dev);
        return result;
    }
    if (strcmp(argv[cmd_idx], "umount") == 0 && argc == cmd_idx + 3)
        return umount_partition(argv[cmd_idx + 1], argv[cmd_idx + 2]);

    FSCTL_WARN("invalid command or argument count command=%s", argv[cmd_idx]);
    print_usage(stderr);
    return EXIT_UNSUPPORTED_LAYOUT;
}
