/*
 * drm_prime.c: PRIME, the export of buffer objects as dma-bufs and their
 * import. A stub until 76-4b writes it, whose ioctl handlers answer
 * -EOPNOTSUPP, as Linux does for a driver without the feature.
 */

#include "drm.h"

#include "../lib/kerrno.h"

int32_t drm_prime_handle_to_fd_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    (void)dev;
    (void)data;
    (void)file;
    return -EOPNOTSUPP;
}

int32_t drm_prime_fd_to_handle_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    (void)dev;
    (void)data;
    (void)file;
    return -EOPNOTSUPP;
}
