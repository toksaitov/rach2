/*
 * drm_pc.c: the PC's display probes, which drm_start() calls through
 * arch_drm_probe: the native drivers of the PC's display devices, and
 * ACPI's backlight where they registered none, then the driver of the
 * framebuffer the firmware left.
 */

#include "drm.h"

#include "../acpi/devices.h"

#include "../arch/arch.h"
#include "../klog.h"
#include "../lib/kerrno.h"

/** Log a probe's error by its driver's name; ENODEV, no device, is not
 * one. */
static void drm_pc_report(const char *name, int32_t n)
{
    if (n < 0 && n != -ENODEV) {
        klog(KLOG_WARN, "drm", "%s: error %d", name, (int)n);
    }
}

void arch_drm_probe(int phase)
{
    if (phase == ARCH_DRM_NATIVE) {
        drm_pc_report("bochs", bochs_probe());
        drm_pc_report("i915", i915_probe());
        /* Linux's acpi_video_register_backlight: the firmware's backlight
         * only where no native driver registered one. */
        acpi_video_register_backlight();
    } else {
        drm_pc_report("fbdrm", fbdrm_probe());
    }
}
