/*
 * drm_atomic.c: the atomic commit: its check, its commit and its events and
 * fences. A stub until 76-5a, then 76-5b, writes it, whose ioctl handlers
 * answer -EOPNOTSUPP, as Linux does for a driver without the feature.
 */

#include "drm.h"

#include "../lib/kerrno.h"

int32_t drm_mode_atomic_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    (void)dev;
    (void)data;
    (void)file;
    return -EOPNOTSUPP;
}
