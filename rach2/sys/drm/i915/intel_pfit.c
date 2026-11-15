/*
 * intel_pfit.c: a Haswell pipe's panel fitter, after Linux 6.12's
 * pch_panel_fitting (display/intel_panel.c) and ilk_pfit_enable and
 * ilk_pfit_disable (intel_display.c) for a PCH split display: the window a
 * user's mode is scaled into on the panel's fixed mode, its limits, and
 * its registers.
 */

#include "i915.h"
#include "i915_reg.h"

#include "../../lib/kerrno.h"

/* A fitter's window on the pipe, in pixels. */
struct pf_win {
    int32_t x, y, w, h;
};

/** Linux 6.18's checks of a fitter's window `win`
 * (display/intel_pfit.c, intel_pch_pfit_check_*): centred on the pipe, not
 * at x 1, a source the fitter of `pipe` takes and at least 7 lines.
 * 0 or -EINVAL. The scale down has no ratio limit, as Linux 6.12's
 * pch_panel_fitting lets the Lenovo show 1920x1080 and 2560x1440 on its
 * 1366x768 panel; the pipe's pixel rate still has to fit the CD clock. */
static int32_t pfit_check(int pipe, const struct drm_mode_modeinfo *m, int32_t src_w, int32_t src_h,
                          const struct pf_win *win)
{
    int32_t x = win->x, y = win->y, w = win->w, h = win->h;

    if ((int32_t)m->hdisplay != 2 * x + w || (int32_t)m->vdisplay != 2 * y + h || x == 1) {
        return -EINVAL;
    }
    /* Pipe A's fitter is 7x5 and takes 4096 pixels; B's and C's 2048. */
    if (src_w > (pipe == I915_PIPE_A ? 4096 : 2048) || src_h > 4096) {
        return -EINVAL;
    }
    if (m->vdisplay < 7) {
        return -EINVAL;
    }
    return 0;
}

int32_t intel_pfit_compute_config(int pipe, uint32_t scaling_mode, struct intel_crtc_config *cfg)
{
    const struct drm_mode_modeinfo *m = &cfg->mode;
    int32_t sw = (int32_t)cfg->src_w, sh = (int32_t)cfg->src_h;
    int32_t hd = (int32_t)m->hdisplay, vd = (int32_t)m->vdisplay;
    int32_t x, y, w, h, scaled_w, scaled_h;
    struct pf_win win;

    cfg->pfit = 0;
    if (hd == sw && vd == sh) {
        return 0;
    }
    switch (scaling_mode) {
    case DRM_MODE_SCALE_CENTER:
        w = sw;
        h = sh;
        x = (hd - w + 1) / 2;
        y = (vd - h + 1) / 2;
        break;
    case DRM_MODE_SCALE_ASPECT:
        scaled_w = hd * sh;
        scaled_h = sw * vd;
        x = y = 0;
        w = hd;
        h = vd;
        if (scaled_w > scaled_h) {
            /* Pillars: the width the source's aspect gives, even. */
            w = scaled_h / sh;
            w += w & 1;
            x = (hd - w + 1) / 2;
        } else if (scaled_w < scaled_h) {
            /* Letterbox: the height, even. */
            h = scaled_w / sw;
            h += h & 1;
            y = (vd - h + 1) / 2;
        }
        break;
    case DRM_MODE_SCALE_NONE:
    case DRM_MODE_SCALE_FULLSCREEN:
        x = y = 0;
        w = hd;
        h = vd;
        break;
    default:
        return -EINVAL;
    }
    cfg->pfit = 1;
    cfg->pf_pos = PF_WIN_XPOS((uint32_t)x & 0x1fffu) | PF_WIN_YPOS((uint32_t)y & 0x1fffu);
    cfg->pf_size = PF_WIN_XSIZE((uint32_t)w & 0x1fffu) | PF_WIN_YSIZE((uint32_t)h & 0x1fffu);
    win.x = x;
    win.y = y;
    win.w = w;
    win.h = h;
    return pfit_check(pipe, m, sw, sh, &win);
}

uint32_t intel_pfit_pixel_rate(const struct intel_crtc_config *cfg)
{
    uint32_t pw = cfg->pf_size >> 16, ph = cfg->pf_size & 0xffffu;
    uint32_t w = cfg->src_w > pw ? cfg->src_w : pw, h = cfg->src_h > ph ? cfg->src_h : ph;

    /* Linux's ilk_pipe_pixel_rate: the fitter reads its source at the
     * rate its window is shown, more when it scales down. */
    if (!cfg->pfit || pw == 0 || ph == 0) {
        return cfg->mode.clock;
    }
    return (uint32_t)drm_div64((uint64_t)cfg->mode.clock * w * h, (uint64_t)pw * ph);
}

void ilk_pfit_enable(struct drm_i915_private *i915, int pipe, const struct intel_crtc_config *cfg)
{
    if (!cfg->pfit) {
        return;
    }
    /* Linux forces the filter's coefficients on Haswell, since some
     * firmware leaves broken ones (the x201's). */
    i915_write(&i915->hw, PF_CTL(pipe), PF_ENABLE | PF_FILTER_MED_3x3 | PF_PIPE_SEL_IVB(pipe));
    i915_write(&i915->hw, PF_WIN_POS(pipe), cfg->pf_pos);
    i915_write(&i915->hw, PF_WIN_SZ(pipe), cfg->pf_size);
}

void ilk_pfit_disable(struct drm_i915_private *i915, int pipe, const struct intel_crtc_config *cfg)
{
    /* Only a fitter in use, lest Haswell's power well be upset. */
    if (!cfg->pfit) {
        return;
    }
    i915_write(&i915->hw, PF_CTL(pipe), 0);
    i915_write(&i915->hw, PF_WIN_POS(pipe), 0);
    i915_write(&i915->hw, PF_WIN_SZ(pipe), 0);
}
