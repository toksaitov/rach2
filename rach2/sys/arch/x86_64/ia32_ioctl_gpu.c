/*
 * ia32_ioctl_gpu.c: the i386 forms of the GPU drivers' ioctls, i915's and
 * virtio-gpu's, for the compat ioctl path (ia32_ioctl.c).
 */

#include "ia32.h"

#if RACH2_IA32

/** No command has a translation yet. */
long ia32_ioctl_gpu(unsigned int fd, unsigned int cmd, unsigned long arg)
{
    (void)fd;
    (void)cmd;
    (void)arg;
    return -ENOTTY;
}

#endif /* RACH2_IA32 */
