/*
 * ia32_ioctl_drm.c: the i386 forms of the DRM core's ioctls, as Linux's
 * drm_ioc32.c translates them, for the compat ioctl path (ia32_ioctl.c).
 */

#include "ia32.h"

/** No command has a translation yet. */
long ia32_ioctl_drm(unsigned int fd, unsigned int cmd, unsigned long arg)
{
    (void)fd;
    (void)cmd;
    (void)arg;
    return -ENOTTY;
}
