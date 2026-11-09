/*
 * drm_legacy.c: the mode-setting calls that came before atomic, each made an
 * atomic commit. A stub until 76-5a writes it, whose ioctl handlers answer
 * -EOPNOTSUPP, as Linux does for a driver without the feature.
 */

#include "drm.h"

#include "../lib/kerrno.h"

int32_t drm_mode_setcrtc(struct drm_device *dev, void *data, struct drm_file *file)
{
    (void)dev;
    (void)data;
    (void)file;
    return -EOPNOTSUPP;
}

int32_t drm_mode_setplane(struct drm_device *dev, void *data, struct drm_file *file)
{
    (void)dev;
    (void)data;
    (void)file;
    return -EOPNOTSUPP;
}

int32_t drm_mode_cursor_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    (void)dev;
    (void)data;
    (void)file;
    return -EOPNOTSUPP;
}

int32_t drm_mode_cursor2_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    (void)dev;
    (void)data;
    (void)file;
    return -EOPNOTSUPP;
}

int32_t drm_mode_gamma_set_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    (void)dev;
    (void)data;
    (void)file;
    return -EOPNOTSUPP;
}

int32_t drm_connector_property_set_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    (void)dev;
    (void)data;
    (void)file;
    return -EOPNOTSUPP;
}

int32_t drm_mode_obj_set_property_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    (void)dev;
    (void)data;
    (void)file;
    return -EOPNOTSUPP;
}

int32_t drm_mode_page_flip_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    (void)dev;
    (void)data;
    (void)file;
    return -EOPNOTSUPP;
}

int32_t drm_mode_dirtyfb_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    (void)dev;
    (void)data;
    (void)file;
    return -EOPNOTSUPP;
}
