/*
 * virtio_gpu.c: the virtio GPU, its 2D resources and scanouts, and later
 * its 3D contexts. A stub until 76-8 writes it, which finds no device.
 */

#include "drm.h"

#include "../lib/kerrno.h"

int32_t virtio_gpu_probe(void)
{
    return -ENODEV;
}
