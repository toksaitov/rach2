/*
 * drm_auth.c: authentication of a card's clients and the master role. A stub
 * until 76-4a writes it, whose ioctl handlers answer -EOPNOTSUPP, as Linux
 * does for a driver without the feature.
 */

#include "drm.h"

#include "../lib/kerrno.h"

int32_t drm_getmagic(struct drm_device *dev, void *data, struct drm_file *file)
{
    (void)dev;
    (void)data;
    (void)file;
    return -EOPNOTSUPP;
}

int32_t drm_authmagic(struct drm_device *dev, void *data, struct drm_file *file)
{
    (void)dev;
    (void)data;
    (void)file;
    return -EOPNOTSUPP;
}

int32_t drm_setmaster_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    (void)dev;
    (void)data;
    (void)file;
    return -EOPNOTSUPP;
}

int32_t drm_dropmaster_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    (void)dev;
    (void)data;
    (void)file;
    return -EOPNOTSUPP;
}
