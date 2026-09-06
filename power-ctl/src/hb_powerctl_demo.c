#define _GNU_SOURCE

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <systemd/sd-bus.h>

#include "storage.h"

#define APP_PARAM_SERVICE "app_param-mount.service"
#define APP_PARAM_MOUNTPOINT "/app_param"
#define APP_PARAM_DEVICE_A "/dev/mmcblk0p26"
#define APP_PARAM_DEVICE_B "/dev/mmcblk0p27"
#define APP_PARAM_HASH_CHUNK_KIB 0

static int get_unit_substate(sd_bus *bus, const char *unit, char **substate)
{
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    const char *unit_path;
    int r;

    *substate = NULL;
    r = sd_bus_call_method(bus,
                           "org.freedesktop.systemd1",
                           "/org/freedesktop/systemd1",
                           "org.freedesktop.systemd1.Manager",
                           "GetUnit",
                           &error,
                           &reply,
                           "s",
                           unit);
    if (r >= 0)
        r = sd_bus_message_read(reply, "o", &unit_path);
    if (r >= 0)
        r = sd_bus_get_property_string(bus,
                                       "org.freedesktop.systemd1",
                                       unit_path,
                                       "org.freedesktop.systemd1.Unit",
                                       "SubState",
                                       &error,
                                       substate);
    if (r < 0)
        fprintf(stderr, "hb_powerctl_demo: cannot query %s SubState: %s\n",
                unit, error.message != NULL ? error.message : strerror(-r));

    sd_bus_message_unref(reply);
    sd_bus_error_free(&error);
    return r;
}

static int wait_mount_recovery_complete(void)
{
    sd_bus *bus = NULL;
    int r;

    r = sd_bus_open_system(&bus);
    if (r < 0) {
        fprintf(stderr, "hb_powerctl_demo: cannot connect to system bus: %s\n",
                strerror(-r));
        return -1;
    }

    for (;;) {
        char *substate = NULL;

        r = get_unit_substate(bus, APP_PARAM_SERVICE, &substate);
        if (r < 0) {
            sd_bus_unref(bus);
            return -1;
        }

        if (strcmp(substate, "exited") == 0) {
            printf("POWERCTL_MOUNT_READY service=%s substate=%s\n",
                   APP_PARAM_SERVICE, substate);
            free(substate);
            sd_bus_unref(bus);
            return 0;
        }

        if (strcmp(substate, "start-pre") == 0 ||
            strcmp(substate, "start") == 0 ||
            strcmp(substate, "start-post") == 0 ||
            strcmp(substate, "running") == 0) {
            printf("POWERCTL_WAIT service=%s substate=%s\n",
                   APP_PARAM_SERVICE, substate);
            free(substate);
            usleep(100000);
            continue;
        }

        fprintf(stderr,
                "hb_powerctl_demo: %s is not usable for power prepare, SubState=%s\n",
                APP_PARAM_SERVICE, substate);
        free(substate);
        sd_bus_unref(bus);
        errno = EIO;
        return -1;
    }
}

static int hb_powerctl_prepare(void)
{
    const char *active;
    const char *peer;

    if (wait_mount_recovery_complete() != 0)
        return -1;

    if (powerctl_resolve_active_peer(APP_PARAM_MOUNTPOINT,
                                     APP_PARAM_DEVICE_A,
                                     APP_PARAM_DEVICE_B,
                                     &active, &peer) != 0) {
        fprintf(stderr, "hb_powerctl_demo: cannot resolve active/peer: %s\n",
                strerror(errno));
        return -1;
    }
    printf("POWERCTL_DIRECTION active=%s peer=%s\n", active, peer);

    if (powerctl_disable_vfat_write(APP_PARAM_MOUNTPOINT) != 0) {
        fprintf(stderr, "hb_powerctl_demo: disable-write failed: %s\n",
                strerror(errno));
        return -1;
    }

    if (powerctl_seal_block_device(active, APP_PARAM_HASH_CHUNK_KIB,
                                   NULL, NULL) != 0) {
        fprintf(stderr, "hb_powerctl_demo: seal %s failed: %s\n",
                active, strerror(errno));
        return -1;
    }

    if (powerctl_copy_block_device(active, peer) != 0) {
        fprintf(stderr, "hb_powerctl_demo: copy %s -> %s failed: %s\n",
                active, peer, strerror(errno));
        return -1;
    }

    printf("POWERCTL_PREPARE_OK active=%s peer=%s\n", active, peer);
    return 0;
}

int main(int argc, char **argv)
{
    int prepare_result;
    int prepare_only = 0;

    if (argc == 2 && strcmp(argv[1], "--prepare-only") == 0)
        prepare_only = 1;
    else if (argc != 1) {
        fprintf(stderr, "Usage: %s [--prepare-only]\n", argv[0]);
        return 2;
    }

    prepare_result = hb_powerctl_prepare();
    if (prepare_only)
        return prepare_result == 0 ? 0 : 1;

    if (prepare_result != 0)
        fprintf(stderr,
                "hb_powerctl_demo: prepare failed; original power transition still continues\n");
    else
        printf("hb_powerctl_demo: prepare succeeded; original power transition continues\n");

    /* Demo stops here. In the product this is the existing, unchanged
     * systemd D-Bus shutdown/reboot call. */
    return 0;
}
