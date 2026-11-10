/*
 * intel_panel.c: the eDP panel's fixed modes, after Linux's
 * display/intel_panel.c: chosen from its EDID or the VBT, the modes its
 * connector offers, and the fixed mode a pipe drives for a user's mode.
 */

#include "i915.h"

#include "../../klog.h"
#include "../../lib/kerrno.h"

/* The sync polarities, which an alternate fixed mode may change. */
#define SYNC_FLAGS \
    (DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC | DRM_MODE_FLAG_NVSYNC)

/*
 * A walk over the EDID's modes, in the order the parse adds them: the
 * first pass finds the preferred mode, the first one marked so or else the
 * first one, at index `preferred`; the second adds the alternate fixed
 * modes after it.
 */
struct edid_walk {
    struct intel_dp *dp;
    int index;
    int preferred;
    struct drm_mode_modeinfo mode;
};

/** The first pass: note the preferred mode. */
static int find_preferred(const struct drm_mode_modeinfo *mode, void *arg)
{
    struct edid_walk *w = (struct edid_walk *)arg;

    if (w->index == 0) {
        w->mode = *mode;
    }
    if (w->preferred < 0 && (mode->type & DRM_MODE_TYPE_PREFERRED)) {
        w->mode = *mode;
        w->preferred = w->index;
    }
    w->index++;
    return 0;
}

/** Whether `mode` may stand in for the preferred mode `pref`, Linux's
 * is_alt_fixed_mode: the same size and flags but for the syncs. */
static int is_alt_fixed_mode(const struct drm_mode_modeinfo *mode,
                             const struct drm_mode_modeinfo *pref)
{
    return (mode->flags & ~SYNC_FLAGS) == (pref->flags & ~SYNC_FLAGS) &&
           mode->hdisplay == pref->hdisplay && mode->vdisplay == pref->vdisplay;
}

/** The second pass: add each alternate fixed mode but the preferred. */
static int add_alt(const struct drm_mode_modeinfo *mode, void *arg)
{
    struct edid_walk *w = (struct edid_walk *)arg;
    struct intel_dp *dp = w->dp;

    if (w->index++ != w->preferred && dp->nfixed < INTEL_PANEL_FIXED_MODES &&
        is_alt_fixed_mode(mode, &dp->fixed[0])) {
        dp->fixed[dp->nfixed++] = *mode;
    }
    return 0;
}

/** Add `mode` as a fixed mode, preferred, Linux's
 * intel_panel_add_fixed_mode. */
static void add_fixed(struct intel_dp *dp, const struct drm_mode_modeinfo *mode, const char *what)
{
    if (dp->nfixed == INTEL_PANEL_FIXED_MODES) {
        return;
    }
    dp->fixed[dp->nfixed] = *mode;
    dp->fixed[dp->nfixed].type |= DRM_MODE_TYPE_PREFERRED;
    klog(KLOG_INFO, "i915", "panel: %s fixed mode %ux%u@%u, %u kHz", what, (unsigned)mode->hdisplay,
         (unsigned)mode->vdisplay, (unsigned)drm_mode_vrefresh(mode), (unsigned)mode->clock);
    dp->nfixed++;
}

void intel_panel_init_fixed_modes(struct drm_i915_private *i915)
{
    struct intel_dp *dp = &i915->display.dp;
    struct edid_walk w;

    dp->nfixed = 0;
    w.dp = dp;
    w.index = 0;
    w.preferred = -1;
    if (dp->edid_len != 0 && drm_edid_parse_modes(dp->edid, dp->edid_len, find_preferred, &w) > 0) {
        if (w.preferred < 0) {
            w.preferred = 0;
        }
        add_fixed(dp, &w.mode,
                  (w.mode.type & DRM_MODE_TYPE_PREFERRED) ? "the EDID's preferred"
                                                          : "the EDID's first");
        /* The EDID's other modes of the panel's size, as Linux's eDP
         * init adds them (use_alt_fixed_modes). */
        w.index = 0;
        (void)drm_edid_parse_modes(dp->edid, dp->edid_len, add_alt, &w);
    }
    if (dp->nfixed == 0 && dp->vbt.has_lfp_mode) {
        add_fixed(dp, &dp->vbt.lfp_mode, "the VBT's");
    }
}

const struct drm_mode_modeinfo *intel_panel_fixed_mode(struct drm_i915_private *i915,
                                                       const struct drm_mode_modeinfo *mode)
{
    struct intel_dp *dp = &i915->display.dp;
    const struct drm_mode_modeinfo *best = NULL;
    int32_t vrefresh = (int32_t)drm_mode_vrefresh(mode), d, best_d = 0;
    int i;

    /* The closest refresh rate, the first of equals. */
    for (i = 0; i < dp->nfixed; i++) {
        d = (int32_t)drm_mode_vrefresh(&dp->fixed[i]) - vrefresh;
        d = d < 0 ? -d : d;
        if (!best || d < best_d) {
            best = &dp->fixed[i];
            best_d = d;
        }
    }
    return best;
}

int32_t intel_panel_mode_valid(struct drm_i915_private *i915, const struct drm_mode_modeinfo *mode)
{
    const struct drm_mode_modeinfo *fixed = intel_panel_fixed_mode(i915, mode);

    if (!fixed) {
        return 0;
    }
    if (mode->hdisplay != fixed->hdisplay || mode->vdisplay != fixed->vdisplay ||
        drm_mode_vrefresh(mode) != drm_mode_vrefresh(fixed)) {
        return -EINVAL;
    }
    return 0;
}

int32_t intel_panel_compute_config(struct drm_i915_private *i915, struct drm_mode_modeinfo *mode)
{
    const struct drm_mode_modeinfo *fixed = intel_panel_fixed_mode(i915, mode);
    int32_t d;

    if (!fixed) {
        return 0;
    }
    /* Linux's latitude of a hertz, for modes a program made up near the
     * panel's rate. */
    d = (int32_t)drm_mode_vrefresh(mode) - (int32_t)drm_mode_vrefresh(fixed);
    if (d > 1 || d < -1) {
        return -EINVAL;
    }
    *mode = *fixed;
    return 0;
}

int intel_panel_get_modes(struct drm_i915_private *i915, struct drm_connector *connector)
{
    struct intel_dp *dp = &i915->display.dp;
    int i, n = 0;

    for (i = 0; i < dp->nfixed; i++) {
        if (drm_mode_probed_add(connector, &dp->fixed[i]) == 0) {
            n++;
        }
    }
    return n;
}
