/* virtio_gpu_blob.c: blob and context-init ioctls, without their features. */
#include "virtio_gpu.h"
#include "../lib/kerrno.h"

/**
 * Refuse blob and context-init calls with EINVAL while their features are
 * absent.
 */
int32_t vgpu_blob_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    (void)dev;
    (void)data;
    (void)file;
    return -EINVAL;
}
