/*
 * drm_vblank.c: vblank counts, waits, events and sequences. A stub until 76-5b
 * writes it, whose ioctl handlers answer -EOPNOTSUPP, as Linux does for a
 * driver without the feature.
 */

#include "drm.h"

#include "../lib/kerrno.h"

int32_t drm_wait_vblank_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    (void)dev;
    (void)data;
    (void)file;
    return -EOPNOTSUPP;
}

int32_t drm_crtc_get_sequence_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    (void)dev;
    (void)data;
    (void)file;
    return -EOPNOTSUPP;
}

int32_t drm_crtc_queue_sequence_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    (void)dev;
    (void)data;
    (void)file;
    return -EOPNOTSUPP;
}
