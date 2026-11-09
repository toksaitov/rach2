/*
 * fbdrm.c: the framebuffer the firmware or the loader set up, as one fixed
 * mode, Linux's simpledrm. A stub until 76-6 writes it, which finds none.
 */

#include "drm.h"

#include "../lib/kerrno.h"

int32_t fbdrm_probe(void)
{
    return -ENODEV;
}
