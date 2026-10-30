/*
 * mixer.c: /dev/mixer (mixer.h). No driver offers its volume here yet, so
 * the node is absent and its ioctls fail.
 */

#include "mixer.h"

#include "../lib/kerrno.h"

int mixer_present(void)
{
    return 0;
}

int32_t mixer_ioctl(fd_entry_t *e, uint32_t request, uintptr_t arg)
{
    (void)e;
    (void)request;
    (void)arg;
    return -ENODEV;
}
