/*
 * ia32_ioctl_gpu.c: the i386 forms of the GPU drivers' ioctls, i915's and
 * virtio-gpu's, for the compat ioctl path (ia32_ioctl.c).
 */

#include "ia32.h"

#include "../../drm/i915/i915_drm.h"
#include "../../lib/kstring.h"
#include "../../mm/usercopy.h"
#include "../../syscall.h"

#if RACH2_IA32

/** Whether `fd` belongs to i915, using VERSION before interpreting a
 * driver request whose number other drivers also use. The argument and
 * name share the kernel usercopy window; a failed query leaves the
 * original request to its native dispatcher. */
static bool is_i915(unsigned int fd)
{
    struct {
        struct drm_version version;
        char name[4];
    } v;

    k_memset(&v, 0, sizeof(v));
    v.version.name = v.name;
    v.version.name_len = sizeof(v.name);
    return ia32_ioctl_native(fd, DRM_IOCTL_VERSION, &v, sizeof(v)) == 0 &&
           v.version.name_len == sizeof(v.name) && v.name[0] == 'i' && v.name[1] == '9' &&
           v.name[2] == '1' && v.name[3] == '5';
}

/** GETPARAM's signed parameter and zero-extended 32-bit int pointer, as
 * Linux's compat_i915_getparam. Only the pointed-to int is written back;
 * neither the request's size and direction nor native tail padding may
 * cause a copy back to the eight-byte compat request. Returns the native
 * result, or -EFAULT if the request cannot be read. */
static long getparam(unsigned int fd, unsigned long arg)
{
    struct {
        int32_t param;
        uint32_t value;
    } p32;
    struct drm_i915_getparam p;

    if (copy_from_user(&p32, (const void __user *)arg, sizeof(p32)) != 0) {
        return -EFAULT;
    }
    k_memset(&p, 0, sizeof(p));
    p.param = p32.param;
    p.value = (int __user *)(uintptr_t)p32.value;
    return ia32_ioctl_native(fd, DRM_IOCTL_I915_GETPARAM, &p, sizeof(p));
}

/** Translate i915 GETPARAM, keyed by number as Linux's i915_ioc32.c
 * does. Every other GPU request passes unchanged: GEM, virtio-gpu,
 * dma-buf, syncobj, sync_file and udmabuf have fixed-width layouts (or
 * only tail padding the DRM dispatcher handles). Returns the native
 * result or the translation's -EFAULT. */
long ia32_ioctl_gpu(unsigned int fd, unsigned int cmd, unsigned long arg)
{
    if (UAPI_IOC_TYPE(cmd) == DRM_IOCTL_BASE &&
        UAPI_IOC_NR(cmd) == DRM_COMMAND_BASE + DRM_I915_GETPARAM && is_i915(fd)) {
        return getparam(fd, arg);
    }
    return ioctl_fd((int)fd, cmd, (uintptr_t)arg);
}

#endif /* RACH2_IA32 */
