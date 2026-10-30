/*
 * evdev.c: the input devices of /dev/input/eventN (evdev.h). No device is
 * registered yet, so every node is absent and every operation fails.
 */

#include "evdev.h"

#include "lib/kdef.h"
#include "lib/kerrno.h"

struct evdev *evdev_register(const char *name, uint16_t bustype, uint16_t vendor, uint16_t product,
                             const uint8_t *keys)
{
    (void)name;
    (void)bustype;
    (void)vendor;
    (void)product;
    (void)keys;
    return NULL;
}

void evdev_report(struct evdev *dev, uint16_t type, uint16_t code, int32_t value)
{
    (void)dev;
    (void)type;
    (void)code;
    (void)value;
}

int evdev_present(int n)
{
    (void)n;
    return 0;
}

int32_t evdev_open(fd_entry_t *e, int n)
{
    (void)e;
    (void)n;
    return -ENODEV;
}

int32_t evdev_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    (void)e;
    (void)buf;
    (void)count;
    return -ENODEV;
}

short evdev_poll(fd_entry_t *e, short events, struct wait_queue **wq)
{
    (void)e;
    (void)events;
    *wq = NULL;
    return 0;
}

int32_t evdev_ioctl(fd_entry_t *e, uint32_t request, uintptr_t arg)
{
    (void)e;
    (void)request;
    (void)arg;
    return -ENODEV;
}

void evdev_close(fd_entry_t *e)
{
    (void)e;
}
