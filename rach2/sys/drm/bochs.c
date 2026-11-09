/*
 * bochs.c: QEMU's std-vga through its DISPI registers, Linux's bochs-drm.
 * A stub until 76-6 writes it, which finds no device.
 */

#include "drm.h"

#include "../lib/kerrno.h"

int32_t bochs_probe(void)
{
    return -ENODEV;
}
