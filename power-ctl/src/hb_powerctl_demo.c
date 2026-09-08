#define _GNU_SOURCE

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <systemd/sd-bus.h>

#include "common.h"
#include "storage.h"

#define APP_PARAM_MOUNTPOINT "/app_param"
#define APP_PARAM_DEVICES "/dev/mmcblk0p26:/dev/mmcblk0p27"
#define APP_PARAM_MOUNT_SERVICE "app_param-mount.service"
#define APP_PARAM_READY_TIMEOUT_USEC (60ULL * 1000000ULL)

static int32_t get_unit_substate(sd_bus *bus, const char *unit,
                                 char **substate) {
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    const char *unit_path;
    int32_t result;

    if (bus == NULL || unit == NULL || substate == NULL) {
        errno = EINVAL;
        FSCTL_ERROR("invalid mount service query arguments");
        return -1;
    }

    *substate = NULL;
    result = (int32_t)sd_bus_call_method(
        bus, "org.freedesktop.systemd1", "/org/freedesktop/systemd1",
        "org.freedesktop.systemd1.Manager", "GetUnit", &error, &reply, "s",
        unit);
    if (result >= 0)
        result = (int32_t)sd_bus_message_read(reply, "o", &unit_path);
    if (result >= 0)
        result = (int32_t)sd_bus_get_property_string(
            bus, "org.freedesktop.systemd1", unit_path,
            "org.freedesktop.systemd1.Unit", "SubState", &error, substate);
    if (result < 0)
        FSCTL_ERROR("query mount service SubState failed unit=%s: %s", unit,
                    error.message != NULL ? error.message : strerror(-result));

    sd_bus_message_unref(reply);
    sd_bus_error_free(&error);
    return result;
}

static int32_t wait_mount_recovery_complete(void) {
    sd_bus *bus = NULL;
    struct timespec start_time;
    int32_t result;

    result = (int32_t)sd_bus_open_system(&bus);
    if (result < 0) {
        FSCTL_ERROR("connect system bus failed: %s", strerror(-result));
        return result;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &start_time) != 0) {
        FSCTL_ERROR("read monotonic clock failed: %s", strerror(errno));
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
            FSCTL_INFO("mount recovery complete service=%s", APP_PARAM_MOUNT_SERVICE);
            free(substate);
            sd_bus_unref(bus);
            return 0;
        }
        if (strcmp(substate, "start-pre") != 0 &&
            strcmp(substate, "start") != 0 &&
            strcmp(substate, "start-post") != 0 &&
            strcmp(substate, "running") != 0) {
            FSCTL_ERROR("mount service unusable state=%s", substate);
            free(substate);
            sd_bus_unref(bus);
            errno = EIO;
            return -1;
        }
        FSCTL_DEBUG("wait mount recovery service=%s state=%s",
                    APP_PARAM_MOUNT_SERVICE, substate);
        free(substate);

        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
            FSCTL_ERROR("read monotonic clock failed: %s", strerror(errno));
            sd_bus_unref(bus);
            return -1;
        }
        elapsed = (uint64_t)(now.tv_sec - start_time.tv_sec) * 1000000ULL;
        if (now.tv_nsec >= start_time.tv_nsec) {
            elapsed += (uint64_t)(now.tv_nsec - start_time.tv_nsec) / 1000ULL;
        } else {
            elapsed -= 1000000ULL;
            elapsed +=
                (uint64_t)(1000000000L + now.tv_nsec - start_time.tv_nsec) /
                1000ULL;
        }
        if (elapsed >= APP_PARAM_READY_TIMEOUT_USEC) {
            FSCTL_ERROR("mount recovery wait timeout seconds=%" PRIu64,
                        (uint64_t)(APP_PARAM_READY_TIMEOUT_USEC / 1000000ULL));
            sd_bus_unref(bus);
            errno = ETIMEDOUT;
            return -1;
        }
        usleep(100000U);
    }
}

static int32_t prepare_app_param(void) {
    if (wait_mount_recovery_complete() != 0)
        return -1;
    return umount_partition(APP_PARAM_DEVICES, APP_PARAM_MOUNTPOINT) == EXIT_OK
               ? 0
               : -1;
}

int main(int argc, char **argv) {
    uint32_t prepare_only = 0U;
    int32_t result;

    if (argc == 2 && strcmp(argv[1], "--prepare-only") == 0) {
        prepare_only = 1U;
    } else if (argc != 1) {
        FSCTL_WARN("invalid powerctl demo arguments");
        fprintf(stderr, "Usage: %s [--prepare-only]\n", argv[0]);
        return 2;
    }

    result = prepare_app_param();
    if (prepare_only != 0U)
        return result == 0 ? 0 : 1;

    if (result != 0)
        FSCTL_ERROR("power prepare failed; original power transition continues");
    else
        FSCTL_INFO("power prepare succeeded; original power transition continues");
    return 0;
}
