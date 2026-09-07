#include <stdint.h>
#include <stdio.h>

#include "../../include/hb_powerctl.h"

int main(void)
{
    int32_t result = hb_powerctl_reboot(POWEROFF, NULL);

    if (result != 0) {
        fprintf(stderr, "freeze: hb_powerctl_reboot(POWEROFF) failed: %d\n",
                result);
        return 1;
    }

    puts("freeze: hb_powerctl_reboot(POWEROFF) succeeded");
    return 0;
}
