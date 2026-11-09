/*
 * drm_pc.c: the PC's display probes, which drm_start() calls through
 * arch_drm_probe: the native drivers of the PC's display devices, then the
 * driver of the framebuffer the firmware left.
 */

#include "drm.h"

#include "../arch/arch.h"
#include "../klog.h"
#include "../lib/kerrno.h"

void arch_drm_probe(int phase)
{
    int32_t n;
    const char *name;

    if (phase == ARCH_DRM_NATIVE) {
        n = bochs_probe();
        name = "bochs";
    } else {
        n = fbdrm_probe();
        name = "fbdrm";
    }
    if (n < 0 && n != -ENODEV) {
        klog(KLOG_WARN, "drm", "%s: error %d", name, (int)n);
    }
}
