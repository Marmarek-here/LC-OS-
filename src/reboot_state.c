#include "reboot_state.h"

static volatile int reboot_requested = 0;

void reboot_request(void)
{
    reboot_requested = 1;
}

int reboot_take_requested(void)
{
    if (reboot_requested) {
        reboot_requested = 0;
        return 1;
    }

    return 0;
}
