/*
 * intel_display.c: the Intel graphics driver's display on Haswell: the
 * firmware's picture read out and adopted without a mode set, the CRTCs,
 * the eDP connector, and the commit that pins each framebuffer it shows.
 */

#include "i915.h"
#include "i915_drm.h"
#include "i915_reg.h"

#include "../../klog.h"
#include "../../lib/kerrno.h"
#include "../../lib/kirq.h"
#include "../../lib/kstring.h"
#include "../../mm/kheap.h"

/* The alignment of a scanout's binding, Linux's for a display engine
 * behind VT-d (intel_scanout_needs_vtd_wa), which covers the rest. */
#define INTEL_SCANOUT_ALIGN (256u * 1024u)

/** A register of the device. */
static uint32_t rd(struct drm_i915_private *i915, uint32_t reg)
{
    return i915_read(&i915->hw, reg);
}

/* ------------------------------------------------------------------------
 * Readout
 */

/** The DRM format of a DSPCNTR pixel format, as Linux's
 * i9xx_format_to_fourcc, or 0 for one it does not scan out. */
static uint32_t dspcntr_format(uint32_t cntr)
{
    switch (cntr & DISP_FORMAT_MASK) {
    case DISP_FORMAT_8BPP:
        return DRM_FORMAT_C8;
    case DISP_FORMAT_BGRX565:
        return DRM_FORMAT_RGB565;
    case DISP_FORMAT_BGRX888:
        return DRM_FORMAT_XRGB8888;
    case DISP_FORMAT_RGBX888:
        return DRM_FORMAT_XBGR8888;
    case DISP_FORMAT_BGRX101010:
        return DRM_FORMAT_XRGB2101010;
    case DISP_FORMAT_RGBX101010:
        return DRM_FORMAT_XBGR2101010;
    case DISP_FORMAT_RGBX161616:
        return DRM_FORMAT_XBGR16161616F;
    default:
        return 0;
    }
}

/** The link's symbol clock in kHz of a DDI port's clock select, Linux's
 * hsw_ddi_lcpll_get_freq; 0 for a PLL whose readout 78-10 brings. */
static uint32_t port_clock(uint32_t sel)
{
    switch (sel & PORT_CLK_SEL_MASK) {
    case PORT_CLK_SEL_LCPLL_810:
        return 81000u * 2u;
    case PORT_CLK_SEL_LCPLL_1350:
        return 135000u * 2u;
    case PORT_CLK_SEL_LCPLL_2700:
        return 270000u * 2u;
    default:
        return 0;
    }
}

/** The pipe the eDP transcoder's input selects, or -1. */
static int edp_input_pipe(uint32_t ddi)
{
    switch (ddi & TRANS_DDI_EDP_INPUT_MASK) {
    case TRANS_DDI_EDP_INPUT_A_ON:
    case TRANS_DDI_EDP_INPUT_A_ONOFF:
        return I915_PIPE_A;
    case TRANS_DDI_EDP_INPUT_B_ONOFF:
        return I915_PIPE_B;
    case TRANS_DDI_EDP_INPUT_C_ONOFF:
        return I915_PIPE_C;
    default:
        return -1;
    }
}

/** Read transcoder `t`'s timings into `m`, each field plus one, as Linux's
 * intel_get_transcoder_timings. */
static void read_timings(struct drm_i915_private *i915, int t, struct drm_mode_modeinfo *m)
{
    uint32_t v;

    v = rd(i915, TRANS_HTOTAL(t));
    m->hdisplay = (uint16_t)((v & 0xffffu) + 1u);
    m->htotal = (uint16_t)((v >> 16) + 1u);
    v = rd(i915, TRANS_HSYNC(t));
    m->hsync_start = (uint16_t)((v & 0xffffu) + 1u);
    m->hsync_end = (uint16_t)((v >> 16) + 1u);
    v = rd(i915, TRANS_VTOTAL(t));
    m->vdisplay = (uint16_t)((v & 0xffffu) + 1u);
    m->vtotal = (uint16_t)((v >> 16) + 1u);
    v = rd(i915, TRANS_VSYNC(t));
    m->vsync_start = (uint16_t)((v & 0xffffu) + 1u);
    m->vsync_end = (uint16_t)((v >> 16) + 1u);
}

void intel_pipe_readout(struct drm_i915_private *i915, int pipe, struct intel_pipe_readout *out)
{
    struct drm_mode_modeinfo *m = &out->mode;
    uint32_t edp, ddi, conf, cntr, v;
    int t = -1;

    k_memset(out, 0, sizeof(*out));
    edp = rd(i915, TRANS_DDI_FUNC_CTL(I915_TRANS_EDP));
    if ((edp & TRANS_DDI_FUNC_ENABLE) && edp_input_pipe(edp) == pipe) {
        t = I915_TRANS_EDP;
    } else if (rd(i915, TRANS_DDI_FUNC_CTL(pipe)) & TRANS_DDI_FUNC_ENABLE) {
        t = pipe;
    }
    if (t < 0 || !((conf = rd(i915, TRANSCONF(t))) & TRANSCONF_ENABLE)) {
        return;
    }
    ddi = rd(i915, TRANS_DDI_FUNC_CTL(t));
    out->active = 1;
    out->trans = t;
    out->port = (int)((ddi & TRANS_DDI_PORT_MASK) >> TRANS_DDI_PORT_SHIFT);
    out->dither = (conf & TRANSCONF_DITHER_EN) != 0;
    out->lanes = ((ddi & TRANS_DDI_PORT_WIDTH_MASK) >> 1) + 1u;
    switch (ddi & TRANS_DDI_BPC_MASK) {
    case TRANS_DDI_BPC_6:
        out->bpp = 18;
        break;
    case TRANS_DDI_BPC_10:
        out->bpp = 30;
        break;
    case TRANS_DDI_BPC_12:
        out->bpp = 36;
        break;
    default:
        out->bpp = 24;
        break;
    }
    read_timings(i915, t, m);
    m->flags = (ddi & TRANS_DDI_PHSYNC ? DRM_MODE_FLAG_PHSYNC : DRM_MODE_FLAG_NHSYNC) |
               (ddi & TRANS_DDI_PVSYNC ? DRM_MODE_FLAG_PVSYNC : DRM_MODE_FLAG_NVSYNC);
    if ((conf & TRANSCONF_INTERLACE_MASK) != 0) {
        m->flags |= DRM_MODE_FLAG_INTERLACE;
    }
    out->port_clock = port_clock(rd(i915, PORT_CLK_SEL(out->port)));
    v = rd(i915, PIPE_DATA_M1(t));
    out->tu = ((v >> TU_SIZE_SHIFT) & 0x3fu) + 1u;
    out->data_m = v & DATA_LINK_M_N_MASK;
    out->data_n = rd(i915, PIPE_DATA_N1(t)) & DATA_LINK_M_N_MASK;
    out->link_m = rd(i915, PIPE_LINK_M1(t)) & DATA_LINK_M_N_MASK;
    out->link_n = rd(i915, PIPE_LINK_N1(t)) & DATA_LINK_M_N_MASK;
    /* Linux's intel_dotclock_calculate: M/N of the link's symbol clock for
     * DisplayPort, the port's clock itself for HDMI and DVI. */
    if ((ddi & TRANS_DDI_MODE_SELECT_MASK) == TRANS_DDI_MODE_SELECT_DP_SST) {
        if (out->link_n != 0) {
            m->clock = (uint32_t)drm_div64(
                (uint64_t)out->link_m * out->port_clock + out->link_n - 1u, out->link_n);
        }
    } else {
        m->clock = out->port_clock;
    }
    m->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
    m->vrefresh = drm_mode_vrefresh(m);
    drm_mode_set_name(m);
    v = rd(i915, PIPESRC(pipe));
    out->src_w = (v >> 16) + 1u;
    out->src_h = (v & 0xffffu) + 1u;
    out->pf_ctl = rd(i915, PF_CTL(pipe));
    if (out->pf_ctl & PF_ENABLE) {
        out->pf_win_pos = rd(i915, PF_WIN_POS(pipe));
        out->pf_win_sz = rd(i915, PF_WIN_SZ(pipe));
    } else {
        out->pf_ctl = 0;
    }
    cntr = rd(i915, DSPCNTR(pipe));
    if (!(cntr & DISP_ENABLE)) {
        return;
    }
    out->plane_on = 1;
    out->format = dspcntr_format(cntr);
    out->modifier = (cntr & DISP_TILED) ? I915_FORMAT_MOD_X_TILED : DRM_FORMAT_MOD_LINEAR;
    out->surf = rd(i915, DSPSURF(pipe)) & ~0xfffu;
    out->stride = rd(i915, DSPSTRIDE(pipe));
    v = rd(i915, DSPOFFSET(pipe));
    out->x = v & 0xffffu;
    out->y = v >> 16;
}

void intel_display_readout(struct drm_i915_private *i915)
{
    struct intel_display *d = &i915->display;
    struct intel_pps_readout *p = &d->pps;
    int pipe;

    for (pipe = 0; pipe < I915_NUM_PIPES; pipe++) {
        intel_pipe_readout(i915, pipe, &d->crtc[pipe].hw);
    }
    p->status = rd(i915, PP_STATUS);
    p->control = rd(i915, PP_CONTROL);
    p->on_delays = rd(i915, PP_ON_DELAYS);
    p->off_delays = rd(i915, PP_OFF_DELAYS);
    p->divisor = rd(i915, PP_DIVISOR);
    p->panel_on = (p->control & PANEL_POWER_ON) && (p->status & PP_ON);
    p->backlight_on = (p->control & EDP_BLC_ENABLE) != 0;
}

/* ------------------------------------------------------------------------
 * Framebuffers and their pins
 */

/** The firmware's framebuffer object's `free`: the object alone; its GTT
 * range stays kept, since the console may draw there again through the
 * aperture. */
static void fw_obj_free(struct drm_gem_object *obj)
{
    kfree(obj);
}

static const struct drm_gem_object_funcs fw_obj_funcs = {fw_obj_free, NULL, NULL, NULL,
                                                         NULL,        NULL, NULL};

/** Whether `fb` shows the firmware's framebuffer. */
static int fb_is_firmware(const struct drm_framebuffer *fb)
{
    return fb->obj[0] && fb->obj[0]->funcs == &fw_obj_funcs;
}

uint64_t intel_fb_modifier(const struct drm_framebuffer *fb)
{
    const struct drm_gem_object *base = fb->obj[0];

    if (!(fb->flags & DRM_MODE_FB_MODIFIERS) && base && base->funcs == &i915_gem_object_funcs &&
        ((const struct drm_i915_gem_object *)base)->tiling == I915_TILING_X) {
        return I915_FORMAT_MOD_X_TILED;
    }
    return fb->modifier;
}

uint32_t intel_fb_ggtt(const struct drm_framebuffer *fb)
{
    struct drm_i915_private *i915 = to_i915(fb->dev);
    const struct i915_vma *vma;

    if (fb_is_firmware(fb)) {
        return i915->display.fw_ggtt;
    }
    if (fb->obj[0]->funcs != &i915_gem_object_funcs) {
        return 0;
    }
    for (vma = ((const struct drm_i915_gem_object *)fb->obj[0])->vmas; vma; vma = vma->obj_next) {
        if (vma->view_pages == 0 && vma->mappable && vma->pin_count > 0) {
            return (uint32_t)vma->start;
        }
    }
    return 0;
}

/** Pin `fb` for scanout: its binding in `*vma`, NULL for the firmware's.
 * 0, -EINVAL for an object of another device, or the binding's error. */
static int32_t fb_pin(struct drm_framebuffer *fb, struct i915_vma **vma)
{
    *vma = NULL;
    if (fb_is_firmware(fb)) {
        return 0;
    }
    if (fb->obj[0]->funcs != &i915_gem_object_funcs) {
        return -EINVAL;
    }
    return i915_gem_object_pin_to_display((struct drm_i915_gem_object *)fb->obj[0],
                                          INTEL_SCANOUT_ALIGN, vma);
}

/** Let a scanout go: its pin and its hold on the framebuffer. */
static void scanout_put(struct intel_scanout *s)
{
    if (s->vma) {
        i915_vma_unpin(s->vma);
    }
    drm_framebuffer_put(s->fb);
    s->fb = NULL;
    s->vma = NULL;
}

/** The pipe and plane index of `plane`, or NULL when the driver did not
 * make it. */
static struct intel_crtc *plane_slot(struct drm_i915_private *i915, const struct drm_plane *plane,
                                     int *slot)
{
    int pipe;

    for (pipe = 0; pipe < I915_NUM_PIPES; pipe++) {
        struct intel_crtc *c = &i915->display.crtc[pipe];

        *slot = plane == &c->primary  ? INTEL_PLANE_PRIMARY
                : plane == &c->sprite ? INTEL_PLANE_SPRITE
                : plane == &c->cursor ? INTEL_PLANE_CURSOR
                                      : -1;
        if (*slot >= 0) {
            return c;
        }
    }
    return NULL;
}

int32_t intel_atomic_commit(struct drm_device *dev, struct drm_atomic_state *state, int nonblock)
{
    struct drm_i915_private *i915 = to_i915(dev);
    struct i915_vma *pinned[DRM_MAX_PLANES];
    struct drm_framebuffer *fbs[DRM_MAX_PLANES];
    struct drm_plane *planes[DRM_MAX_PLANES];
    int32_t err = 0;
    int i, slot;

    for (i = 0; i < DRM_MAX_PLANES; i++) {
        struct drm_plane_state *ps = state->planes_new[i];

        pinned[i] = NULL;
        fbs[i] = NULL;
        planes[i] = ps ? ps->plane : NULL;
        if (ps && ps->fb && err == 0) {
            err = fb_pin(ps->fb, &pinned[i]);
            if (err == 0) {
                fbs[i] = ps->fb;
                drm_framebuffer_get(fbs[i]);
            }
        }
    }
    if (err == 0) {
        err = drm_atomic_helper_commit(dev, state, nonblock);
    }
    for (i = 0; i < DRM_MAX_PLANES; i++) {
        struct intel_crtc *c = planes[i] ? plane_slot(i915, planes[i], &slot) : NULL;
        struct intel_scanout next;

        next.fb = fbs[i];
        next.vma = pinned[i];
        if (err != 0 || !c) {
            scanout_put(&next);
            continue;
        }
        /* The commit's stall let the flip before this one latch, so what
         * the plane showed then is read no more. */
        scanout_put(&c->parked[slot]);
        c->parked[slot] = c->shown[slot];
        c->shown[slot] = next;
    }
    return err;
}

/* ------------------------------------------------------------------------
 * Watermarks
 */

/* The level 0 latency of each plane in tenths of a microsecond, which
 * Haswell's pcode gives for all three (i915_pri_wm_latency), and the
 * largest level 0 value of each field of WM0_PIPE. */
#define WM0_LATENCY  20u
#define WM0_PRI_MAX  127u
#define WM0_SPR_MAX  127u
#define WM0_CUR_MAX  63u
#define WM_LP_ENABLE (1u << 31)

/** Linux's ilk_wm_method1: the FIFO's cachelines the latency drains at
 * `rate` kHz and `cpp` bytes a pixel, plus two. */
static uint32_t wm_method1(uint32_t rate, uint32_t cpp, uint32_t latency)
{
    uint64_t bytes = drm_div64((uint64_t)rate * cpp * latency + 9999u, 10000u);

    return (uint32_t)((bytes + 63u) / 64u) + 2u;
}

/** Linux's ilk_wm_method2: the lines the latency spans, at `width` pixels
 * of `cpp` bytes, in cachelines plus two. */
static uint32_t wm_method2(uint32_t rate, uint32_t htotal, uint32_t width, uint32_t cpp,
                           uint32_t latency)
{
    uint32_t lines;

    if (htotal == 0) {
        return 0;
    }
    lines = (uint32_t)drm_div64((uint64_t)rate * latency + (uint64_t)htotal * 10000u - 1u,
                                (uint64_t)htotal * 10000u);
    return (lines * width * cpp + 63u) / 64u + 2u;
}

/** The smaller of `a` and `b`. */
static uint32_t wm_min(uint32_t a, uint32_t b)
{
    return a < b ? a : b;
}

void intel_update_wm(struct intel_crtc *crtc)
{
    struct drm_i915_private *i915 = crtc->i915;
    const struct drm_mode_modeinfo *m = &crtc->base.state->mode;
    uint32_t rate = m->clock, pri = 0, spr = 0, cur = 0, wm;
    struct drm_plane_state *ps;

    if (crtc->base.state->active && rate != 0) {
        ps = crtc->primary.state;
        if (ps->visible && ps->fb) {
            pri = wm_min(wm_method1(rate, ps->fb->info->cpp[0], WM0_LATENCY), WM0_PRI_MAX);
        }
        ps = crtc->sprite.state;
        if (ps->visible && ps->fb) {
            spr = wm_min(wm_min(wm_method1(rate, ps->fb->info->cpp[0], WM0_LATENCY),
                                wm_method2(rate, m->htotal, (uint32_t)(ps->dst.x2 - ps->dst.x1),
                                           ps->fb->info->cpp[0], WM0_LATENCY)),
                         WM0_SPR_MAX);
        }
        ps = crtc->cursor.state;
        if (ps->visible && ps->fb) {
            cur = wm_min(wm_method2(rate, m->htotal, ps->fb->width, 4u, WM0_LATENCY), WM0_CUR_MAX);
        }
    }
    if ((spr || cur) && !i915->display.lp_wm_off) {
        /* Linux's ilk LP watermarks would take the new planes in; with them
         * off the FIFO is filled at level 0 alone, which costs power only. */
        i915_write(&i915->hw, WM3_LP_ILK, rd(i915, WM3_LP_ILK) & ~WM_LP_ENABLE);
        i915_write(&i915->hw, WM2_LP_ILK, rd(i915, WM2_LP_ILK) & ~WM_LP_ENABLE);
        i915_write(&i915->hw, WM1_LP_ILK, rd(i915, WM1_LP_ILK) & ~WM_LP_ENABLE);
        i915->display.lp_wm_off = 1;
    }
    wm = pri << 16 | spr << 8 | cur;
    if (!pri) {
        /* The firmware's primary watermark stands while nothing replaces
         * it, as on a pipe off. */
        wm |= rd(i915, WM0_PIPE_ILK(crtc->pipe)) & (0x7fu << 16);
    }
    if (wm != crtc->wm) {
        i915_write(&i915->hw, WM0_PIPE_ILK(crtc->pipe), wm);
        crtc->wm = wm;
    }
}

/* ------------------------------------------------------------------------
 * CRTCs
 */

/** Whether pipe `crtc` can be lit without a mode set: the firmware left it
 * on with a mode the readout knows. */
static int crtc_adoptable(const struct intel_crtc *crtc)
{
    return crtc->hw.active && crtc->hw.mode.clock != 0;
}

/**
 * The CRTC's check: a pipe lights only with the mode the firmware left on
 * it, since the mode set comes with 78-10; turning it off, and on again
 * with that mode, is a fastset of the planes and the backlight. -EINVAL
 * for anything else.
 */
static int32_t intel_crtc_check(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
    struct intel_crtc *c = to_intel_crtc(crtc);
    struct drm_crtc_state *cs = state->crtcs_new[crtc->index];

    if (!cs || !cs->enable) {
        return 0;
    }
    if (!crtc_adoptable(c) || !drm_mode_equal(&cs->mode, &c->hw.mode)) {
        return -EINVAL;
    }
    if (cs->async_flip && drm_atomic_crtc_needs_modeset(cs)) {
        return -EINVAL;
    }
    return 0;
}

/** The modes a pipe drives: the firmware's. */
static int32_t intel_crtc_mode_valid(struct drm_crtc *crtc, const struct drm_mode_modeinfo *mode)
{
    struct intel_crtc *c = to_intel_crtc(crtc);

    return crtc_adoptable(c) && drm_mode_equal(mode, &c->hw.mode) ? 0 : -EINVAL;
}

/** The pipe comes on again: the planes are written by the commit's plane
 * updates, so only the backlight follows. */
static void intel_crtc_enable(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
    struct intel_crtc *c = to_intel_crtc(crtc);

    (void)state;
    if (c->hw.trans == I915_TRANS_EDP) {
        intel_backlight_pipe(c->i915, 1);
    }
}

/** Turn a plane's registers off and arm the change for the next vblank. */
static void plane_off(struct intel_crtc *c, int slot)
{
    struct i915_hw *hw = &c->i915->hw;

    switch (slot) {
    case INTEL_PLANE_PRIMARY:
        i915_write(hw, DSPCNTR(c->pipe), 0);
        i915_write(hw, DSPSURF(c->pipe), 0);
        break;
    case INTEL_PLANE_SPRITE:
        i915_write(hw, SPRCTL(c->pipe), 0);
        i915_write(hw, SPRSCALE(c->pipe), 0);
        i915_write(hw, SPRSURF(c->pipe), 0);
        break;
    default:
        i915_write(hw, CURCNTR(c->pipe), 0);
        i915_write(hw, CURBASE(c->pipe), 0);
        break;
    }
    c->planes_on &= ~(1u << slot);
}

/**
 * The pipe goes off, as far as 78-9 drives it: its planes turned off, the
 * backlight of its panel off and its held event sent; the pipe and its
 * transcoder keep running the firmware's mode until 78-10's mode set.
 */
static void intel_crtc_disable(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
    struct intel_crtc *c = to_intel_crtc(crtc);
    uint32_t flags;
    int slot;

    (void)state;
    for (slot = 0; slot < INTEL_NUM_PLANES; slot++) {
        plane_off(c, slot);
    }
    i915_posting_read(&c->i915->hw, DSPSURF(c->pipe));
    if (c->hw.trans == I915_TRANS_EDP) {
        intel_backlight_pipe(c->i915, 0);
    }
    flags = kirq_save();
    intel_crtc_send_event(c);
    kirq_restore(flags);
}

/** Before the planes: the watermarks raised to cover the planes the
 * commit shows, and an async flip's flip done unmasked, since the chip
 * latches only an unmasked event and the flip may be done as soon as the
 * surface is written. */
static void intel_crtc_begin(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
    intel_update_wm(to_intel_crtc(crtc));
    intel_crtc_arm_flip_done(to_intel_crtc(crtc), state);
}

/** After the planes: the commit's event handed to the vblank path. */
static void intel_crtc_flush(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
    intel_crtc_arm_event(to_intel_crtc(crtc), state);
}

static const struct drm_crtc_funcs intel_crtc_funcs = {
    intel_crtc_check,           intel_crtc_enable,
    intel_crtc_disable,         intel_crtc_begin,
    intel_crtc_flush,           intel_crtc_mode_valid,
    intel_enable_vblank,        intel_disable_vblank,
    intel_crtc_set_crc_source,  intel_crtc_verify_crc_source,
    intel_crtc_get_crc_sources, intel_get_vblank_counter};

/* ------------------------------------------------------------------------
 * The eDP connector
 */

/** The pipe the eDP transcoder drives, which the readout found, or NULL. */
static struct intel_crtc *edp_crtc(struct drm_i915_private *i915)
{
    int pipe;

    for (pipe = 0; pipe < I915_NUM_PIPES; pipe++) {
        struct intel_crtc *c = &i915->display.crtc[pipe];

        if (crtc_adoptable(c) && c->hw.trans == I915_TRANS_EDP) {
            return c;
        }
    }
    return NULL;
}

/** The panel is there when the firmware lit it; until 78-10 reads its
 * EDID over AUX, a panel the firmware left dark is not known. */
static int intel_edp_detect(struct drm_connector *connector, int force_probe)
{
    (void)force_probe;
    return edp_crtc(to_i915(connector->dev)) ? DRM_CONNECTOR_CONNECTED : DRM_CONNECTOR_DISCONNECTED;
}

/** The panel's fixed mode, the firmware's, as Linux's
 * intel_panel_get_modes gives it. */
static int intel_edp_get_modes(struct drm_connector *connector)
{
    struct intel_crtc *c = edp_crtc(to_i915(connector->dev));

    if (!c || drm_mode_probed_add(connector, &c->hw.mode) != 0) {
        return 0;
    }
    return 1;
}

static const struct drm_connector_funcs intel_edp_funcs = {intel_edp_detect, intel_edp_get_modes,
                                                           NULL, NULL};

/* ------------------------------------------------------------------------
 * Adoption
 */

/**
 * Make the framebuffer over the firmware's surface on `c`, Linux's
 * intel_alloc_initial_plane_obj: an object of the device over the kept GTT
 * range the plane scans out, and a framebuffer of the readout's format,
 * stride and size made from it through the kernel's own file. NULL when
 * the surface lies outside the range the bind kept, or on no memory.
 */
static struct drm_framebuffer *fw_fb_make(struct drm_i915_private *i915, struct intel_crtc *c)
{
    struct intel_display *d = &i915->display;
    const struct intel_pipe_readout *hw = &c->hw;
    struct drm_mode_fb_cmd2 r;
    struct drm_framebuffer *fb = NULL;
    struct drm_gem_object *obj;
    struct drm_file *file;
    uint64_t bytes = (uint64_t)hw->stride * (hw->y + hw->src_h);
    int32_t err = 0;
    int i, kept = 0;

    bytes = (bytes + I915_GTT_PAGE_SIZE - 1u) & ~(uint64_t)(I915_GTT_PAGE_SIZE - 1u);
    for (i = 0; i < i915->ggtt.nkept; i++) {
        if (i915->ggtt.kept[i].start <= hw->surf && hw->surf + bytes <= i915->ggtt.kept[i].end) {
            kept = 1;
        }
    }
    if (!kept || hw->format == 0 || d->fw_taken) {
        return NULL;
    }
    obj = (struct drm_gem_object *)kzalloc(sizeof(*obj));
    file = (struct drm_file *)kzalloc(sizeof(*file));
    if (!obj || !file) {
        kfree(obj);
        kfree(file);
        return NULL;
    }
    drm_gem_object_init(&i915->drm, obj, bytes, &fw_obj_funcs);
    file->dev = &i915->drm;
    file->minor_type = DRM_MINOR_PRIMARY;
    k_memset(&r, 0, sizeof(r));
    r.width = hw->x + hw->src_w;
    r.height = hw->y + hw->src_h;
    r.pixel_format = hw->format;
    r.flags = DRM_MODE_FB_MODIFIERS;
    r.modifier[0] = hw->modifier;
    r.pitches[0] = hw->stride;
    if (drm_gem_handle_create(file, obj, &r.handles[0]) == 0) {
        fb = drm_internal_framebuffer_create(&i915->drm, &r, file, &err);
        (void)drm_gem_handle_delete(file, r.handles[0]);
    }
    /* The framebuffer holds the object now. */
    drm_gem_object_put(obj);
    kfree(file);
    if (!fb) {
        klog(KLOG_WARN, "i915", "display: the firmware's framebuffer not taken: error %d",
             (int)err);
        return NULL;
    }
    d->fw_taken = 1;
    d->fw_ggtt = hw->surf;
    return fb;
}

/**
 * Adopt the picture the firmware left on `c` into the atomic state, as
 * Linux's intel_modeset_readout_hw_state and intel_find_initial_plane_obj
 * do: the CRTC on with the readout's mode, the primary plane showing the
 * firmware's framebuffer, and the eDP connector routed to it, so that a
 * commit that keeps the mode programs the planes alone.
 */
static void adopt(struct drm_i915_private *i915, struct intel_crtc *c)
{
    struct intel_display *d = &i915->display;
    struct drm_crtc_state *cs = c->base.state;
    struct drm_plane_state *ps = c->primary.state;
    struct drm_connector_state *conn = d->connector.state;
    struct drm_framebuffer *fb;

    if (drm_atomic_set_mode_for_crtc(cs, &c->hw.mode) != 0) {
        return;
    }
    cs->active = 1;
    cs->connector_mask |= drm_connector_mask(&d->connector);
    conn->crtc = &c->base;
    conn->best_encoder = &d->encoder;
    d->connector.encoder = &d->encoder;
    d->connector.dpms = DRM_MODE_DPMS_ON;
    d->encoder.crtc = &c->base;
    drm_crtc_vblank_on(&c->base);
    if (!c->hw.plane_on || (fb = fw_fb_make(i915, c)) == NULL) {
        return;
    }
    cs->plane_mask |= drm_plane_mask(&c->primary);
    ps->crtc = &c->base;
    ps->fb = fb; /* the framebuffer's one reference becomes the state's */
    ps->crtc_x = 0;
    ps->crtc_y = 0;
    ps->crtc_w = c->hw.mode.hdisplay;
    ps->crtc_h = c->hw.mode.vdisplay;
    ps->src_x = c->hw.x << 16;
    ps->src_y = c->hw.y << 16;
    ps->src_w = c->hw.src_w << 16;
    ps->src_h = c->hw.src_h << 16;
    ps->src.x1 = (int32_t)ps->src_x;
    ps->src.y1 = (int32_t)ps->src_y;
    ps->src.x2 = (int32_t)(ps->src_x + ps->src_w);
    ps->src.y2 = (int32_t)(ps->src_y + ps->src_h);
    ps->dst.x2 = (int32_t)ps->crtc_w;
    ps->dst.y2 = (int32_t)ps->crtc_h;
    ps->visible = 1;
    drm_framebuffer_get(fb);
    c->shown[INTEL_PLANE_PRIMARY].fb = fb;
    c->planes_on |= 1u << INTEL_PLANE_PRIMARY;
    c->wm = rd(i915, WM0_PIPE_ILK(c->pipe));
    klog(KLOG_INFO, "i915",
         "display: pipe %c adopted: %ux%u@%u, %u kHz, %u bpp, %u lane%s at %u kHz, "
         "the firmware's framebuffer at GTT 0x%x, stride %u",
         'A' + c->pipe, (unsigned)c->hw.mode.hdisplay, (unsigned)c->hw.mode.vdisplay,
         (unsigned)c->hw.mode.vrefresh, (unsigned)c->hw.mode.clock, (unsigned)c->hw.bpp,
         (unsigned)c->hw.lanes, c->hw.lanes == 1 ? "" : "s", (unsigned)c->hw.port_clock,
         (unsigned)c->hw.surf, (unsigned)c->hw.stride);
}

int32_t intel_display_init(struct drm_i915_private *i915)
{
    struct intel_display *d = &i915->display;
    struct drm_device *dev = &i915->drm;
    struct drm_mode_config *mc = &dev->mode_config;
    struct intel_crtc *edp;
    int32_t err;
    int pipe;

    d->bl_index = -1;
    intel_display_readout(i915);
    err = drm_mode_config_init(dev);
    if (err != 0) {
        return err;
    }
    d->ready = 1;
    /* Linux's intel_mode_config_init for display 7. */
    mc->min_width = 0;
    mc->min_height = 0;
    mc->max_width = 8192;
    mc->max_height = 8192;
    mc->cursor_width = 256;
    mc->cursor_height = 256;
    mc->preferred_depth = 24;
    mc->prefer_shadow = 1;
    mc->async_page_flip = 1;
    mc->fb_dirty = 1;
    mc->atomic_commit = intel_atomic_commit;
    for (pipe = 0; pipe < I915_NUM_PIPES; pipe++) {
        struct intel_crtc *c = &d->crtc[pipe];

        c->i915 = i915;
        c->pipe = pipe;
        if ((err = intel_planes_init(c)) != 0 ||
            (err = drm_crtc_init_with_planes(dev, &c->base, &c->primary, &c->cursor,
                                             &intel_crtc_funcs)) != 0) {
            return err;
        }
    }
    edp = edp_crtc(i915);
    if ((err = drm_encoder_init(dev, &d->encoder, DRM_MODE_ENCODER_TMDS)) != 0 ||
        (err = drm_connector_init(dev, &d->connector, &intel_edp_funcs, DRM_MODE_CONNECTOR_eDP)) !=
            0) {
        return err;
    }
    /* Until 78-10 sets modes, the panel stays on the pipe that feeds it. */
    d->encoder.possible_crtcs = edp ? drm_crtc_mask(&edp->base) : 0x7u;
    d->connector.status = edp ? DRM_CONNECTOR_CONNECTED : DRM_CONNECTOR_DISCONNECTED;
    err = drm_connector_attach_encoder(&d->connector, &d->encoder);
    if (err != 0) {
        return err;
    }
    err = intel_vblank_init(i915);
    if (err != 0) {
        return err;
    }
    if (edp) {
        adopt(i915, edp);
        intel_backlight_setup(i915);
    } else {
        klog(KLOG_INFO, "i915", "display: no pipe lit by the firmware; the eDP panel is dark");
    }
    return 0;
}

void intel_display_fini(struct drm_i915_private *i915)
{
    struct intel_display *d = &i915->display;
    int pipe, slot;

    if (!d->ready) {
        return;
    }
    intel_vblank_fini(i915);
    intel_backlight_fini(i915);
    for (pipe = 0; pipe < I915_NUM_PIPES; pipe++) {
        for (slot = 0; slot < INTEL_NUM_PLANES; slot++) {
            scanout_put(&d->crtc[pipe].parked[slot]);
            scanout_put(&d->crtc[pipe].shown[slot]);
        }
    }
}
