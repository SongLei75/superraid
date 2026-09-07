#define _GNU_SOURCE

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <systemd/sd-bus.h>

#include "storage.h"

#define APP_PARAM_MOUNTPOINT "/app_param"
#define APP_PARAM_DEVICE "/dev/disk/by-partlabel/app_param"
#define APP_PARAM_BACKUP_DEVICE "/dev/disk/by-partlabel/app_param_bak"
#define APP_PARAM_HASH_CHUNK_KIB 0
#define APP_PARAM_MOUNT_SERVICE "app_param-mount.service"
#define APP_PARAM_READY_TIMEOUT_USEC (60ULL * 1000000ULL)

static int get_unit_substate(sd_bus *bus, const char *unit, char **substate)
{
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    const char *unit_path;
    int result;

    *substate = NULL;
    result = sd_bus_call_method(bus,
                                "org.freedesktop.systemd1",
                                "/org/freedesktop/systemd1",
                                "org.freedesktop.systemd1.Manager",
                                "GetUnit", &error, &reply, "s", unit);
    if (result >= 0)
        result = sd_bus_message_read(reply, "o", &unit_path);
    if (result >= 0)
        result = sd_bus_get_property_string(bus,
                                            "org.freedesktop.systemd1",
                                            unit_path,
                                            "org.freedesktop.systemd1.Unit",
                                            "SubState", &error, substate);
    if (result < 0)
        fprintf(stderr, "hb_powerctl_demo: cannot query %s SubState: %s\n",
                unit, error.message != NULL ? error.message : strerror(-result));
    sd_bus_message_unref(reply);
    sd_bus_error_free(&error);
    return result;
}

static int wait_mount_recovery_complete(void)
{
    sd_bus *bus = NULL;
    struct timespec start_time;
    int result;

    result = sd_bus_open_system(&bus);
    if (result < 0)
        return result;
    if (clock_gettime(CLOCK_MONOTONIC, &start_time) != 0) {
        sd_bus_unref(bus);
        return -1;
    }

    for (;;) {
        struct timespec now;
        char *substate = NULL;
        uint64_t elapsed;

        result = get_unit_substate(bus, APP_PARAM_MOUNT_SERVICE, &substate);
        if (result < 0) {
            sd_bus_unref(bus);
            return result;
        }
        if (strcmp(substate, "exited") == 0) {
            printf("POWERCTL_MOUNT_READY service=%s substate=%s\n",
                   APP_PARAM_MOUNT_SERVICE, substate);
            free(substate);
            sd_bus_unref(bus);
            return 0;
        }
        if (strcmp(substate, "start-pre") != 0 &&
            strcmp(substate, "start") != 0 &&
            strcmp(substate, "start-post") != 0 &&
            strcmp(substate, "running") != 0) {
            fprintf(stderr, "hb_powerctl_demo: unusable SubState=%s\n", substate);
            free(substate);
            sd_bus_unref(bus);
            errno = EIO;
            return -1;
        }
        printf("POWERCTL_WAIT service=%s substate=%s\n",
               APP_PARAM_MOUNT_SERVICE, substate);
        free(substate);

        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
            sd_bus_unref(bus);
            return -1;
        }
        elapsed = (uint64_t)(now.tv_sec - start_time.tv_sec) * 1000000ULL;
        if (now.tv_nsec >= start_time.tv_nsec)
            elapsed += (uint64_t)(now.tv_nsec - start_time.tv_nsec) / 1000ULL;
        else {
            elapsed -= 1000000ULL;
            elapsed += (uint64_t)(1000000000L + now.tv_nsec - start_time.tv_nsec) / 1000ULL;
        }
        if (elapsed >= APP_PARAM_READY_TIMEOUT_USEC) {
            sd_bus_unref(bus);
            errno = ETIMEDOUT;
            return -1;
        }
        usleep(100000);
    }
}

static int prepare_app_param(void)
{
    const char *active;
    const char *peer;

    if (wait_mount_recovery_complete() != 0)
        return -1;
    if (powerctl_resolve_active_peer(APP_PARAM_MOUNTPOINT, APP_PARAM_DEVICE,
                                     APP_PARAM_BACKUP_DEVICE,
                                     &active, &peer) != 0)
        return -1;
    printf("POWERCTL_DIRECTION active=%s peer=%s\n", active, peer);
    if (powerctl_disable_vfat_write(APP_PARAM_MOUNTPOINT) != 0)
        return -1;
    if (powerctl_seal_block_device(active, APP_PARAM_HASH_CHUNK_KIB,
                                   NULL, NULL) != 0)
        return -1;
    if (powerctl_copy_block_device(active, peer) != 0)
        return -1;
    printf("POWERCTL_PREPARE_OK active=%s peer=%s\n", active, peer);
    return 0;
}

int main(int argc, char **argv)
{
    int prepare_only = 0;
    int result;

    if (argc == 2 && strcmp(argv[1], "--prepare-only") == 0)
        prepare_only = 1;
    else if (argc != 1) {
        fprintf(stderr, "Usage: %s [--prepare-only]\n", argv[0]);
        return 2;
    }

    result = prepare_app_param();
    if (prepare_only)
        return result == 0 ? 0 : 1;
    if (result != 0)
        fprintf(stderr, "hb_powerctl_demo: prepare failed; original power transition still continues\n");
    else
        printf("hb_powerctl_demo: prepare succeeded; original power transition continues\n");
    return 0;
}
