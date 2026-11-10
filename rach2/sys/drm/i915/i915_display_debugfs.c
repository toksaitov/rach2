/*
 * i915_display_debugfs.c: the pipe CRC of Haswell, which debugfs's
 * crc/control and crc/data reach through the DRM core (76-18b), with the
 * fitter forced through on the eDP pipe, after Linux's
 * display/intel_pipe_crc.c.
 */

#include "i915.h"
#include "i915_reg.h"

#include "../../klog.h"
#include "../../lib/kerrno.h"
#include "../../lib/kirq.h"

/* The sources Linux names, in its order (pipe_crc_sources); a pipe takes
 * those `crc_ctl_of` knows. */
static const char *const crc_sources[] = {"none", "plane1", "plane2", "pf",   "pipe",
                                          "TV",   "DP-B",   "DP-C",   "DP-D", "auto"};

#define NUM_CRC_SOURCES (sizeof(crc_sources) / sizeof(crc_sources[0]))

/** Whether the NUL-terminated `a` and `b` are the same. */
static int same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/**
 * PIPE_CRC_CTL's value for the source `name`, Linux's
 * ivb_pipe_crc_ctl_reg, in `*ctl`, 0 for "none": the primary plane, the
 * sprite, or the pipe's output after its panel fitter for "pipe", "pf"
 * and "auto". 0, or -EINVAL for a source Haswell does not have.
 */
static int32_t crc_ctl_of(const char *name, uint32_t *ctl)
{
    if (!name || same(name, "none")) {
        *ctl = 0;
    } else if (same(name, "plane1")) {
        *ctl = PIPE_CRC_ENABLE | PIPE_CRC_SOURCE_PRIMARY_IVB;
    } else if (same(name, "plane2")) {
        *ctl = PIPE_CRC_ENABLE | PIPE_CRC_SOURCE_SPRITE_IVB;
    } else if (same(name, "pipe") || same(name, "pf") || same(name, "auto")) {
        *ctl = PIPE_CRC_ENABLE | PIPE_CRC_SOURCE_PF_IVB;
    } else {
        return -EINVAL;
    }
    return 0;
}

/**
 * Linux's intel_crtc_crc_setup_workarounds for Haswell: pipe A feeding the
 * eDP transcoder gives no CRC at its fitter's output while the transcoder
 * takes the pipe past the fitter's power well, so while a source runs the
 * pipe is set again with its input through the well (force_thru), and
 * back after. A commit of its own under the commit lock, a mode set only
 * when the input changes, as Linux's fastset leaves it otherwise.
 */
static void crc_workaround(struct intel_crtc *c, int enable)
{
    struct drm_device *dev = &c->i915->drm;
    struct drm_atomic_state *state;
    struct drm_crtc_state *cs;
    int32_t err = -ENOMEM;

    c->crc_wa = (uint8_t)enable;
    if (c->pipe != I915_PIPE_A || !c->cfg.active ||
        c->cfg.force_thru == (c->cfg.pfit || c->crc_wa)) {
        return;
    }
    if (drm_modeset_lock(dev) != 0) {
        return;
    }
    state = drm_atomic_state_alloc(dev);
    cs = state ? drm_atomic_get_crtc_state(state, &c->base) : NULL;
    if (cs) {
        cs->mode_changed = 1;
        err = drm_atomic_commit(state);
    }
    drm_atomic_state_free(state);
    drm_modeset_unlock(dev);
    if (err != 0) {
        klog(KLOG_WARN, "i915", "crc: the fitter's force-through %s: error %d",
             enable ? "not set" : "not cleared", (int)err);
    }
}

int32_t intel_crtc_verify_crc_source(struct drm_crtc *crtc, const char *name, uint32_t *values_cnt)
{
    uint32_t ctl;

    (void)crtc;
    if (crc_ctl_of(name, &ctl) != 0) {
        return -EINVAL;
    }
    /* Linux gives five values a frame on every i915 (i915_crc_source_valid
     * and intel_crtc_verify_crc_source); Haswell fills the first. */
    *values_cnt = 5;
    return 0;
}

const char *const *intel_crtc_get_crc_sources(struct drm_crtc *crtc, uint32_t *count)
{
    (void)crtc;
    *count = NUM_CRC_SOURCES;
    return crc_sources;
}

/**
 * Start the source `name` on `crtc`, or stop it for NULL or "none", as
 * Linux's intel_crtc_set_crc_source: the fitter forced through for a
 * source (`crc_workaround`), the control written and the first frame's CRC
 * dropped, which Haswell computes over a frame it did not see whole, and
 * the fitter let go after the source stops. A running source holds the
 * pipe's vblanks; the error interrupt reads each frame's CRC at its CRC
 * done and the deferred work enters it. 0, -EINVAL
 * for a source the pipe does not have, or -EIO on a pipe that is off.
 * Takes the commit lock.
 */
int32_t intel_crtc_set_crc_source(struct drm_crtc *crtc, const char *name)
{
    struct intel_crtc *c = to_intel_crtc(crtc);
    struct i915_hw *hw = &c->i915->hw;
    uint32_t ctl, flags;
    int32_t err;

    err = crc_ctl_of(name, &ctl);
    if (err != 0) {
        return err;
    }
    if (ctl != 0) {
        crc_workaround(c, 1);
    }
    flags = kirq_save();
    if (ctl != 0 && c->crc_ctl == 0 && drm_crtc_vblank_get(crtc) != 0) {
        kirq_restore(flags);
        crc_workaround(c, 0);
        return -EIO;
    }
    if (ctl == 0 && c->crc_ctl != 0) {
        drm_crtc_vblank_put(crtc);
    }
    i915_write(hw, PIPE_CRC_CTL(c->pipe), ctl);
    i915_posting_read(hw, PIPE_CRC_CTL(c->pipe));
    c->crc_ctl = ctl;
    c->crc_skip = ctl != 0 ? 1u : 0u;
    kirq_restore(flags);
    if (ctl == 0) {
        crc_workaround(c, 0);
    }
    return 0;
}

void intel_crtc_crc_frame(struct intel_crtc *c, uint32_t crc0)
{
    uint32_t crc[5] = {0, 0, 0, 0, 0};

    if (c->crc_ctl == 0) {
        return;
    }
    if (c->crc_skip > 0) {
        c->crc_skip--;
        return;
    }
    crc[0] = crc0;
    (void)drm_crtc_add_crc_entry(&c->base, 1, (uint32_t)c->base.vblank.count, crc);
}
