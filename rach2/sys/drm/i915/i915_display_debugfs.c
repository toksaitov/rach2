/*
 * i915_display_debugfs.c: the pipe CRC of Haswell, which debugfs's
 * crc/control and crc/data reach through the DRM core (76-18b), after
 * Linux's display/intel_pipe_crc.c.
 */

#include "i915.h"
#include "i915_reg.h"

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
 * PIPE_CRC_CTL's value for the source `name` on `c`, Linux's
 * ivb_pipe_crc_ctl_reg, in `*ctl`, 0 for "none": the primary plane, the
 * sprite, or the pipe's output after its panel fitter. "auto" is the pipe
 * as on Linux, but the primary plane on a pipe whose fitter is off and
 * that feeds the eDP transcoder, where Haswell's fitter output carries no
 * CRC until Linux's force-through of the fitter (hsw_pipe_A_crc_wa), which
 * needs a mode set, comes with 78-10; "pipe" and "pf" are refused there.
 * 0, or -EINVAL for a source Haswell does not have.
 */
static int32_t crc_ctl_of(const struct intel_crtc *c, const char *name, uint32_t *ctl)
{
    int bypass = c->hw.trans == I915_TRANS_EDP && c->hw.pf_ctl == 0;

    if (!name || same(name, "none")) {
        *ctl = 0;
    } else if (same(name, "plane1") || (same(name, "auto") && bypass)) {
        *ctl = PIPE_CRC_ENABLE | PIPE_CRC_SOURCE_PRIMARY_IVB;
    } else if (same(name, "plane2")) {
        *ctl = PIPE_CRC_ENABLE | PIPE_CRC_SOURCE_SPRITE_IVB;
    } else if (!bypass && (same(name, "pipe") || same(name, "pf") || same(name, "auto"))) {
        *ctl = PIPE_CRC_ENABLE | PIPE_CRC_SOURCE_PF_IVB;
    } else {
        return -EINVAL;
    }
    return 0;
}

int32_t intel_crtc_verify_crc_source(struct drm_crtc *crtc, const char *name, uint32_t *values_cnt)
{
    uint32_t ctl;

    if (crc_ctl_of(to_intel_crtc(crtc), name, &ctl) != 0) {
        return -EINVAL;
    }
    *values_cnt = 1;
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
 * Linux's intel_crtc_set_crc_source: the control written and the first
 * frame's CRC dropped, which Haswell computes over a frame it did not see
 * whole. A running source holds the pipe's vblanks, whose deferred work
 * enters each frame's CRC. 0, -EINVAL for a source the pipe does not have,
 * or -EIO on a pipe that is off.
 */
int32_t intel_crtc_set_crc_source(struct drm_crtc *crtc, const char *name)
{
    struct intel_crtc *c = to_intel_crtc(crtc);
    struct i915_hw *hw = &c->i915->hw;
    uint32_t ctl, flags;
    int32_t err;

    err = crc_ctl_of(c, name, &ctl);
    if (err != 0) {
        return err;
    }
    flags = kirq_save();
    if (ctl != 0 && c->crc_ctl == 0 && drm_crtc_vblank_get(crtc) != 0) {
        kirq_restore(flags);
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
    return 0;
}

void intel_crtc_crc_frame(struct intel_crtc *c)
{
    uint32_t crc;

    if (c->crc_ctl == 0) {
        return;
    }
    if (c->crc_skip > 0) {
        c->crc_skip--;
        return;
    }
    crc = i915_read(&c->i915->hw, PIPE_CRC_RES_HSW(c->pipe));
    (void)drm_crtc_add_crc_entry(&c->base, 1, (uint32_t)c->base.vblank.count, &crc);
}
