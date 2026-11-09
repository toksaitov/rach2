/*
 * drm.c: the DRM core: the start of the display devices, the firmware
 * framebuffer's handoff and the core's own ioctls. Its devices, nodes and
 * dispatcher are 76-4a's; until then its identity handlers answer
 * -EOPNOTSUPP, as Linux does for a driver without the feature.
 */

#include "drm.h"

#include "../arch/arch.h"
#include "../klog.h"
#include "../lib/kerrno.h"

/* A firmware device's range of the framebuffer, while it drives it. */
struct drm_aperture {
    struct drm_device *dev; /* NULL: the slot is free */
    uint64_t base, size;
    void (*remove)(struct drm_device *dev);
};

static struct drm_aperture apertures[DRM_APERTURE_MAX];

void drm_start(void)
{
    int32_t n;

    klog(KLOG_INFO, "drm", "start");
    n = udmabuf_start();
    if (n < 0) {
        klog(KLOG_WARN, "drm", "udmabuf: error %d", (int)n);
    }
    arch_drm_probe(ARCH_DRM_NATIVE);
    n = virtio_gpu_probe();
    if (n < 0 && n != -ENODEV) {
        klog(KLOG_WARN, "drm", "virtio_gpu: error %d", (int)n);
    }
    arch_drm_probe(ARCH_DRM_FIRMWARE);
    n = vkms_probe();
    if (n < 0 && n != -ENODEV) {
        klog(KLOG_WARN, "drm", "vkms: error %d", (int)n);
    }
}

int32_t drm_aperture_acquire_firmware(struct drm_device *dev, uint64_t base, uint64_t size,
                                      void (*remove)(struct drm_device *dev))
{
    int i, free = -1;

    for (i = 0; i < DRM_APERTURE_MAX; i++) {
        if (!apertures[i].dev) {
            if (free < 0) {
                free = i;
            }
        } else if (base < apertures[i].base + apertures[i].size &&
                   apertures[i].base < base + size) {
            return -EBUSY;
        }
    }
    if (free < 0) {
        return -EBUSY;
    }
    apertures[free].dev = dev;
    apertures[free].base = base;
    apertures[free].size = size;
    apertures[free].remove = remove;
    return 0;
}

int32_t drm_aperture_remove_conflicting(uint64_t base, uint64_t size, const char *name)
{
    int i;

    for (i = 0; i < DRM_APERTURE_MAX; i++) {
        struct drm_aperture a = apertures[i];

        if (!a.dev || base >= a.base + a.size || a.base >= base + size) {
            continue;
        }
        /* The slot is freed first, so that a remove hook that registers
         * again, or removes another, sees a consistent table. */
        apertures[i].dev = NULL;
        klog(KLOG_INFO, "drm", "aperture: %s takes the framebuffer of %s", name,
             a.dev->driver ? a.dev->driver->ident.name : "?");
        drm_client_dev_unregister(a.dev);
        if (a.remove) {
            a.remove(a.dev);
        }
    }
    return 0;
}

int32_t drm_noop(struct drm_device *dev, void *data, struct drm_file *file)
{
    (void)dev;
    (void)data;
    (void)file;
    return 0;
}

int32_t drm_invalid_op(struct drm_device *dev, void *data, struct drm_file *file)
{
    (void)dev;
    (void)data;
    (void)file;
    return -EINVAL;
}

int32_t drm_version(struct drm_device *dev, void *data, struct drm_file *file)
{
    (void)dev;
    (void)data;
    (void)file;
    return -EOPNOTSUPP;
}

int32_t drm_getunique(struct drm_device *dev, void *data, struct drm_file *file)
{
    (void)dev;
    (void)data;
    (void)file;
    return -EOPNOTSUPP;
}

int32_t drm_getclient(struct drm_device *dev, void *data, struct drm_file *file)
{
    (void)dev;
    (void)data;
    (void)file;
    return -EOPNOTSUPP;
}

int32_t drm_getstats(struct drm_device *dev, void *data, struct drm_file *file)
{
    (void)dev;
    (void)data;
    (void)file;
    return -EOPNOTSUPP;
}

int32_t drm_getcap(struct drm_device *dev, void *data, struct drm_file *file)
{
    (void)dev;
    (void)data;
    (void)file;
    return -EOPNOTSUPP;
}

int32_t drm_setclientcap(struct drm_device *dev, void *data, struct drm_file *file)
{
    (void)dev;
    (void)data;
    (void)file;
    return -EOPNOTSUPP;
}

int32_t drm_setversion(struct drm_device *dev, void *data, struct drm_file *file)
{
    (void)dev;
    (void)data;
    (void)file;
    return -EOPNOTSUPP;
}
