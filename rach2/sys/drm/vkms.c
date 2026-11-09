/*
 * vkms.c: a display in software, with vblanks from a timer and CRCs of
 * what it composes, which the option rach2.vkms starts. A stub until 76-12
 * writes it, which starts none.
 */

#include "drm.h"

#include "../lib/kerrno.h"

int32_t vkms_probe(void)
{
    return -ENODEV;
}
