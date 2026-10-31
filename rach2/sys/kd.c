/*
 * kd.c: the console's keyboard as Linux's console keeps it: its key events,
 * its KD* requests and the state saved for the screen's owners (kd.h).
 */
#include "kd.h"

#include "lib/kerrno.h"

void kd_key_event(const void *device, unsigned int keycode, int value)
{
    (void)device;
    (void)keycode;
    (void)value;
}

int32_t kd_ioctl(uint32_t request, uintptr_t arg)
{
    (void)request;
    (void)arg;
    return -ENOTTY;
}

void kd_state_save(struct kd_state *st)
{
    (void)st;
}

void kd_state_restore(const struct kd_state *st)
{
    (void)st;
}
