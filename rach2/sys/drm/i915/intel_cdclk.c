/*
 * intel_cdclk.c: the display's CD clock on Haswell, after Linux's
 * display/intel_cdclk.c: read from LCPLL and the fuses at init, fixed
 * from then on, and the bound it puts on the pipes' pixel rates.
 */

#include "i915.h"
#include "i915_reg.h"

#include "../../klog.h"
#include "../../lib/kerrno.h"

void intel_cdclk_init(struct drm_i915_private *i915)
{
    struct intel_display *d = &i915->display;
    uint32_t lcpll = i915_read(&i915->hw, LCPLL_CTL);

    /* Linux's hsw_get_cdclk. */
    if (lcpll & LCPLL_CD_SOURCE_FCLK) {
        d->cdclk = 800000;
    } else if (i915_read(&i915->hw, FUSE_STRAP) & HSW_CDCLK_LIMIT) {
        d->cdclk = 450000;
    } else if ((lcpll & LCPLL_CLK_FREQ_MASK) == LCPLL_CLK_FREQ_450) {
        d->cdclk = 450000;
    } else if (i915->id && i915->id->ult) {
        d->cdclk = 337500;
    } else {
        d->cdclk = 540000;
    }
    /* Haswell cannot change it: what the firmware set is the most a mode
     * set may use (intel_update_max_cdclk's fixed case). */
    d->max_cdclk = d->cdclk;
    klog(KLOG_INFO, "i915", "cdclk: %u kHz, the most %u kHz, pixel clocks to %u kHz",
         (unsigned)d->cdclk, (unsigned)d->max_cdclk, (unsigned)intel_max_dotclock(i915));
}

uint32_t intel_max_dotclock(struct drm_i915_private *i915)
{
    /* Linux's intel_compute_max_dotclk: one pixel a CD clock on Haswell. */
    return i915->display.max_cdclk;
}

int32_t intel_cdclk_check(struct drm_i915_private *i915, uint32_t pixel_rate)
{
    /* Linux's intel_pixel_rate_to_cdclk on Haswell is the pixel rate
     * itself, and fixed_modeset_calc_cdclk refuses more than the clock. */
    if (pixel_rate > i915->display.cdclk) {
        return -EINVAL;
    }
    return 0;
}
