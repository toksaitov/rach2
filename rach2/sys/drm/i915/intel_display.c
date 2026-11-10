/*
 * intel_display.c: the Intel graphics driver's display on Haswell: the
 * firmware's picture read out and adopted without a mode set, the CRTCs,
 * the eDP connector and the outputs of DDI B to D, and the commit that
 * pins each framebuffer it shows.
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

/** The link's clock in kHz of a DDI port's clock select, Linux's
 * hsw_ddi_lcpll_get_freq, hsw_ddi_wrpll_get_freq and
 * hsw_ddi_spll_get_freq; 0 for none. */
static uint32_t port_clock(struct drm_i915_private *i915, uint32_t sel)
{
    switch (sel & PORT_CLK_SEL_MASK) {
    case PORT_CLK_SEL_WRPLL1:
        return intel_dpll_wrpll_freq(rd(i915, WRPLL_CTL(0)));
    case PORT_CLK_SEL_WRPLL2:
        return intel_dpll_wrpll_freq(rd(i915, WRPLL_CTL(1)));
    case PORT_CLK_SEL_SPLL:
        return intel_dpll_spll_freq(rd(i915, SPLL_CTL));
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
    uint32_t edp, ddi, conf, cntr, v, mode_sel;
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
    mode_sel = ddi & TRANS_DDI_MODE_SELECT_MASK;
    out->output = mode_sel == TRANS_DDI_MODE_SELECT_DP_SST
                      ? (t == I915_TRANS_EDP ? INTEL_OUTPUT_EDP : INTEL_OUTPUT_DP)
                  : mode_sel == TRANS_DDI_MODE_SELECT_HDMI || mode_sel == TRANS_DDI_MODE_SELECT_DVI
                      ? INTEL_OUTPUT_HDMI
                      : INTEL_OUTPUT_NONE;
    out->has_hdmi_sink = mode_sel == TRANS_DDI_MODE_SELECT_HDMI;
    out->pixel_multiplier = t == I915_TRANS_EDP ? 1u : (rd(i915, PIPE_MULT(t)) & 7u) + 1u;
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
    m->flags = ((ddi & TRANS_DDI_PHSYNC) != 0 ? DRM_MODE_FLAG_PHSYNC : DRM_MODE_FLAG_NHSYNC) |
               ((ddi & TRANS_DDI_PVSYNC) != 0 ? DRM_MODE_FLAG_PVSYNC : DRM_MODE_FLAG_NVSYNC);
    /* Linux's intel_get_transcoder_timings: the chip adds the two half
     * lines of an interlaced frame. */
    if ((conf & TRANSCONF_INTERLACE_MASK) != 0) {
        m->flags |= DRM_MODE_FLAG_INTERLACE;
        m->vtotal = (uint16_t)(m->vtotal + 1u);
    }
    if (out->pixel_multiplier == 2u) {
        m->flags |= DRM_MODE_FLAG_DBLCLK;
    }
    out->port_clock = port_clock(i915, rd(i915, PORT_CLK_SEL(out->port)));
    v = rd(i915, PIPE_DATA_M1(t));
    out->tu = ((v >> TU_SIZE_SHIFT) & 0x3fu) + 1u;
    out->data_m = v & DATA_LINK_M_N_MASK;
    out->data_n = rd(i915, PIPE_DATA_N1(t)) & DATA_LINK_M_N_MASK;
    out->link_m = rd(i915, PIPE_LINK_M1(t)) & DATA_LINK_M_N_MASK;
    out->link_n = rd(i915, PIPE_LINK_N1(t)) & DATA_LINK_M_N_MASK;
    /* Linux's ddi_dotclock_get: M/N of the link's symbol clock for
     * DisplayPort; for HDMI and DVI the TMDS clock at 8 bits a colour,
     * divided by the pixel multiplier. */
    if (mode_sel == TRANS_DDI_MODE_SELECT_DP_SST) {
        if (out->link_n != 0) {
            m->clock = (uint32_t)drm_div64(
                (uint64_t)out->link_m * out->port_clock + out->link_n - 1u, out->link_n);
        }
    } else {
        m->clock = out->bpp != 24 && out->has_hdmi_sink ? out->port_clock * 24u / out->bpp
                                                        : out->port_clock;
        m->clock /= out->pixel_multiplier;
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
                                                         NULL,        NULL, NULL, NULL};

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

static void wait_next_vblank(struct drm_i915_private *i915, int pipe);

/** Whether the frame count `a` is past `b`, across the counter's wrap. */
static int frame_past(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) > 0;
}

/**
 * Make `next` plane `slot`'s scanout on `c` after a commit that wrote it
 * between the frame counts `from` and `to`, and let go what the chip reads
 * no more. After a stall the flip before latched, so what the plane showed
 * before it goes. A legacy cursor update has no stall, and its write only
 * arms the plane for the next vblank, as Linux unpins the old cursor from
 * a vblank work: with no vblank since the plane's last write began, what
 * it shows was never latched and goes, while what it showed before is
 * still read and stays; after a vblank during that write, the next one is
 * waited for. May sleep.
 */
static void scanout_rotate(struct intel_crtc *c, int slot, struct intel_scanout next, uint32_t from,
                           uint32_t to, int nostall)
{
    if (nostall && c->cfg.active) {
        if (from == c->shown_from[slot]) {
            scanout_put(&c->shown[slot]);
            c->shown[slot] = next;
            c->shown_to[slot] = to;
            return;
        }
        if (!frame_past(from, c->shown_to[slot])) {
            wait_next_vblank(c->i915, c->pipe);
        }
    }
    scanout_put(&c->parked[slot]);
    c->parked[slot] = c->shown[slot];
    c->shown[slot] = next;
    c->shown_from[slot] = from;
    c->shown_to[slot] = to;
}

int32_t intel_atomic_commit(struct drm_device *dev, struct drm_atomic_state *state, int nonblock)
{
    struct drm_i915_private *i915 = to_i915(dev);
    struct i915_vma *pinned[DRM_MAX_PLANES];
    struct drm_framebuffer *fbs[DRM_MAX_PLANES];
    struct drm_plane *planes[DRM_MAX_PLANES];
    uint32_t from[DRM_MAX_PLANES];
    int32_t err = 0;
    int i, slot = 0;

    for (i = 0; i < DRM_MAX_PLANES; i++) {
        struct drm_plane_state *ps = state->planes_new[i];
        struct intel_crtc *c = ps ? plane_slot(i915, ps->plane, &slot) : NULL;

        pinned[i] = NULL;
        fbs[i] = NULL;
        planes[i] = ps ? ps->plane : NULL;
        from[i] = c ? intel_get_vblank_counter(&c->base) : 0;
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
        scanout_rotate(c, slot, next, from[i], intel_get_vblank_counter(&c->base),
                       state->legacy_cursor_update);
    }
    return err;
}

/* ------------------------------------------------------------------------
 * CRTCs
 */

/**
 * The connector `state` routes to pipe `c`, with its state in `state` or
 * its state now in `*conn`; NULL, with `*conn` NULL, when none is.
 */
static struct drm_connector *crtc_connector(struct intel_crtc *c, struct drm_atomic_state *state,
                                            const struct drm_connector_state **conn)
{
    struct drm_mode_config *mc = &c->i915->drm.mode_config;
    uint32_t i;

    for (i = 0; i < mc->num_connector; i++) {
        struct drm_connector *connector = mc->connectors[i];
        const struct drm_connector_state *cs = state->connectors_new[connector->index];

        if (!cs) {
            cs = connector->state;
        }
        if (cs && cs->crtc == &c->base) {
            *conn = cs;
            return connector;
        }
    }
    *conn = NULL;
    return NULL;
}

/** The DDI port of B to D whose connector `connector` is, or NULL. */
static struct intel_ddi_port *connector_port(struct drm_i915_private *i915,
                                             const struct drm_connector *connector)
{
    int i;

    for (i = 0; i < 3; i++) {
        struct intel_ddi_port *p = &i915->display.ddi[i];

        if (p->present && ((p->has_dp && connector == &p->dp_connector) ||
                           (p->has_hdmi && connector == &p->hdmi_connector))) {
            return p;
        }
    }
    return NULL;
}

/** Linux's DIV_ROUND_CLOSEST for line times, at most HSW_LINETIME_MASK. */
static uint32_t linetime(uint32_t htotal, uint32_t clock)
{
    uint32_t t = clock ? (htotal * 8000u + clock / 2u) / clock : 0;

    return t > HSW_LINETIME_MASK ? HSW_LINETIME_MASK : t;
}

/**
 * Compute what the new state `cs` of `c` sets the pipe to into `cfg`,
 * Linux's intel_modeset_pipe_config and intel_crtc_compute_config: the
 * source the user's mode's size, the configuration of the output `state`
 * routes to the pipe (the eDP panel's, a monitor's on DisplayPort or on
 * HDMI), the dot clock and the pixel rate within the CD clock, and the
 * line times. A disabled CRTC gives a zeroed `cfg`. The PLL is chosen
 * apart (intel_dpll_compute). 0, or -EINVAL or -ERANGE for a state the
 * pipe cannot take.
 */
static int32_t compute_config(struct intel_crtc *c, const struct drm_crtc_state *cs,
                              struct drm_atomic_state *state, struct intel_crtc_config *cfg)
{
    struct drm_i915_private *i915 = c->i915;
    struct intel_display *d = &i915->display;
    const struct drm_connector_state *conn;
    struct drm_connector *connector;
    struct intel_ddi_port *p;
    int32_t err;

    k_memset(cfg, 0, sizeof(*cfg));
    cfg->pll = DPLL_ID_NONE;
    if (!cs->enable) {
        return 0;
    }
    connector = crtc_connector(c, state, &conn);
    if (!connector) {
        return -EINVAL;
    }
    cfg->active = cs->active != 0;
    cfg->mode = cs->mode;
    cfg->src_w = cs->mode.hdisplay;
    cfg->src_h = cs->mode.vdisplay;
    cfg->pixel_multiplier = 1;
    if (connector == &d->connector && d->dp.present) {
        err = intel_dp_compute_config(&d->dp, c->pipe, conn, c->crc_wa, cfg);
    } else if ((p = connector_port(i915, connector)) == NULL) {
        err = -EINVAL;
    } else if (connector == &p->dp_connector) {
        err = intel_dp_compute_config(&p->dp, c->pipe, conn, c->crc_wa, cfg);
    } else {
        err = intel_hdmi_compute_config(p, c->pipe, conn, cfg);
    }
    if (err != 0) {
        return err;
    }
    if (cfg->mode.clock > intel_max_dotclock(i915)) {
        return -EINVAL;
    }
    cfg->pixel_rate = intel_pfit_pixel_rate(cfg);
    err = intel_cdclk_check(i915, cfg->pixel_rate);
    if (err != 0) {
        return err;
    }
    /* Linux's hsw_linetime_wm and hsw_ips_linetime_wm. */
    cfg->linetime = linetime(cfg->mode.htotal, cfg->mode.clock);
    cfg->ips_linetime = linetime(cfg->mode.htotal, i915->display.cdclk);
    return 0;
}

/** The CRTC's check, Linux's intel_crtc_atomic_check: a mode set must
 * configure, without an async flip, and the planes' watermarks must fit,
 * the new alone and with the old. 0, -EINVAL or -ERANGE. */
static int32_t intel_crtc_check(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
    struct intel_crtc *c = to_intel_crtc(crtc);
    struct drm_crtc_state *cs = state->crtcs_new[crtc->index];
    struct intel_crtc_config cfg;
    struct intel_pipe_wm wm;
    int modeset;
    int32_t err;

    if (!cs) {
        return 0;
    }
    modeset = drm_atomic_crtc_needs_modeset(cs);
    if (modeset && cs->async_flip) {
        return -EINVAL;
    }
    cfg = c->cfg;
    if (modeset && (err = compute_config(c, cs, state, &cfg)) != 0) {
        return err;
    }
    if ((err = intel_wm_compute(c, state, &cfg, &wm)) != 0) {
        return err;
    }
    return modeset || !cfg.active ? 0 : intel_wm_intermediate(&c->wm_optimal, &wm);
}

/** The modes a pipe drives, Linux's intel_mode_valid for Haswell: no
 * vertical scan, skew, composite sync or the like, a clock the CD clock
 * takes, and the transcoder's sizes. */
static int32_t intel_crtc_mode_valid(struct drm_crtc *crtc, const struct drm_mode_modeinfo *m)
{
    struct drm_i915_private *i915 = to_intel_crtc(crtc)->i915;

    if (m->vscan > 1 ||
        (m->flags &
         (DRM_MODE_FLAG_HSKEW | DRM_MODE_FLAG_CSYNC | DRM_MODE_FLAG_NCSYNC | DRM_MODE_FLAG_PCSYNC |
          DRM_MODE_FLAG_BCAST | DRM_MODE_FLAG_PIXMUX | DRM_MODE_FLAG_CLKDIV2)) ||
        m->clock > intel_max_dotclock(i915)) {
        return -EINVAL;
    }
    if (m->hdisplay > 8192 || m->hsync_start > 8192 || m->hsync_end > 8192 || m->htotal > 8192 ||
        m->vdisplay > 4096 || m->vsync_start > 8192 || m->vsync_end > 8192 || m->vtotal > 8192) {
        return -EINVAL;
    }
    return 0;
}

/** Nothing more: `intel_crtc_begin` turned the pipe on before the planes,
 * as Linux's commit tail does, and the core turns its vblanks on. */
static void intel_crtc_enable(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
    (void)crtc;
    (void)state;
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
 * Write the transcoder's M/N values and timings, Linux's
 * hsw_configure_cpu_transcoder: the M/N values of DisplayPort, the
 * timings with an interlaced frame's two half lines left to the chip, the
 * pixel multiplier of a transcoder other than eDP's, the frame start delay
 * and TRANSCONF with the transcoder still off.
 */
static void configure_transcoder(struct intel_crtc *c, const struct intel_crtc_config *cfg)
{
    struct i915_hw *hw = &c->i915->hw;
    const struct drm_mode_modeinfo *m = &cfg->mode;
    int t = cfg->trans;
    uint32_t vtotal = m->vtotal, vblank_start, vsyncshift = 0, conf;
    int32_t shift;

    if (cfg->output != INTEL_OUTPUT_HDMI) {
        i915_write(hw, PIPE_DATA_M1(t), TU_SIZE(cfg->tu) | cfg->data_m);
        i915_write(hw, PIPE_DATA_N1(t), cfg->data_n);
        i915_write(hw, PIPE_LINK_M1(t), cfg->link_m);
        i915_write(hw, PIPE_LINK_N1(t), cfg->link_n);
    }
    /* The second set, for a refresh rate switch Rach2 does not make:
     * zeroes, with the transfer unit's field of 0 less one. Haswell has it
     * on the eDP transcoder alone (intel_cpu_transcoder_has_m2_n2). */
    if (cfg->output != INTEL_OUTPUT_HDMI && t == I915_TRANS_EDP) {
        i915_write(hw, PIPE_DATA_M2(t), 0x3fu << TU_SIZE_SHIFT);
        i915_write(hw, PIPE_DATA_N2(t), 0);
        i915_write(hw, PIPE_LINK_M2(t), 0);
        i915_write(hw, PIPE_LINK_N2(t), 0);
    }
    /* Linux's intel_set_transcoder_timings: blanking spans the borders,
     * none here; the chip adds an interlaced frame's two half lines. */
    if (m->flags & DRM_MODE_FLAG_INTERLACE) {
        vtotal -= 1u;
        shift = (int32_t)m->hsync_start - (int32_t)(m->htotal / 2u);
        if (shift < 0) {
            shift += m->htotal;
        }
        vsyncshift = (uint32_t)shift;
    }
    vblank_start = m->vsync_start < m->vdisplay ? m->vsync_start : m->vdisplay;
    i915_write(hw, TRANS_VSYNCSHIFT(t), vsyncshift);
    i915_write(hw, TRANS_HTOTAL(t), (uint32_t)(m->hdisplay - 1) | (uint32_t)(m->htotal - 1) << 16);
    i915_write(hw, TRANS_HBLANK(t), (uint32_t)(m->hdisplay - 1) | (uint32_t)(m->htotal - 1) << 16);
    i915_write(hw, TRANS_HSYNC(t),
               (uint32_t)(m->hsync_start - 1) | (uint32_t)(m->hsync_end - 1) << 16);
    i915_write(hw, TRANS_VTOTAL(t), (uint32_t)(m->vdisplay - 1) | (vtotal - 1u) << 16);
    i915_write(hw, TRANS_VBLANK(t), (vblank_start - 1u) | (vtotal - 1u) << 16);
    i915_write(hw, TRANS_VSYNC(t),
               (uint32_t)(m->vsync_start - 1) | (uint32_t)(m->vsync_end - 1) << 16);
    /* Pipe B or C feeding eDP takes the eDP transcoder's VTOTAL in its own
     * (TRANS_DDI_FUNC_CTL's eDP input select). */
    if (t == I915_TRANS_EDP && c->pipe != I915_PIPE_A) {
        i915_write(hw, TRANS_VTOTAL(c->pipe), (uint32_t)(m->vdisplay - 1) | (vtotal - 1u) << 16);
    }
    if (t != I915_TRANS_EDP) {
        i915_write(hw, PIPE_MULT(t), cfg->pixel_multiplier - 1u);
    }
    /* Linux's hsw_set_frame_start_delay, a delay of one. */
    i915_rmw(hw, CHICKEN_TRANS(t), HSW_FRAME_START_DELAY_MASK, HSW_FRAME_START_DELAY(0));
    /* Linux's hsw_set_transconf for a mode set: the pipe still off. */
    conf = (m->flags & DRM_MODE_FLAG_INTERLACE) ? TRANSCONF_INTERLACE_IF_ID_ILK
                                                : TRANSCONF_INTERLACE_PF_PD_ILK;
    if (cfg->dither) {
        conf |= TRANSCONF_DITHER_EN | TRANSCONF_DITHER_TYPE_SP;
    }
    i915_write(hw, TRANSCONF(t), conf);
    i915_posting_read(hw, TRANSCONF(t));
}

/** Pipe `pipe`'s matrix for limited range RGB, Linux's
 * ilk_csc_matrix_limited_range through ilk_update_pipe_csc. */
static void write_csc_limited(struct i915_hw *hw, int pipe)
{
    /* (235 - 16) / 255 in 1.12, and 16 / 255 of full scale. */
    const uint32_t coeff = (235u - 16u) << 4, postoff = 16u << 4;

    i915_write(hw, PIPE_CSC_PREOFF_HI(pipe), 0);
    i915_write(hw, PIPE_CSC_PREOFF_ME(pipe), 0);
    i915_write(hw, PIPE_CSC_PREOFF_LO(pipe), 0);
    i915_write(hw, PIPE_CSC_COEFF_RY_GY(pipe), coeff << 16);
    i915_write(hw, PIPE_CSC_COEFF_BY(pipe), 0);
    i915_write(hw, PIPE_CSC_COEFF_RU_GU(pipe), coeff);
    i915_write(hw, PIPE_CSC_COEFF_BU(pipe), 0);
    i915_write(hw, PIPE_CSC_COEFF_RV_GV(pipe), 0);
    i915_write(hw, PIPE_CSC_COEFF_BV(pipe), coeff << 16);
    i915_write(hw, PIPE_CSC_POSTOFF_HI(pipe), postoff);
    i915_write(hw, PIPE_CSC_POSTOFF_ME(pipe), postoff);
    i915_write(hw, PIPE_CSC_POSTOFF_LO(pipe), postoff);
}

/**
 * The pipe's colour, Linux's color commit for a pipe without tables or
 * user matrix: the legacy 8-bit gamma, and for limited range RGB the
 * matrix that scales to 16-235, which the planes then pass through.
 */
static void color_commit(struct intel_crtc *c, const struct intel_crtc_config *cfg)
{
    struct i915_hw *hw = &c->i915->hw;
    int pipe = c->pipe;

    /* Linux's ivb_csc_mode: the matrix after the gamma when it makes
     * limited range, before it otherwise; hsw_color_commit_arm writes the
     * matrix first (ilk_color_commit_noarm) and the modes after. */
    if (cfg->limited_range) {
        write_csc_limited(hw, pipe);
    }
    i915_write(hw, GAMMA_MODE(pipe), GAMMA_MODE_MODE_8BIT);
    i915_write(hw, PIPE_CSC_MODE(pipe), cfg->limited_range ? 0 : CSC_POSITION_BEFORE_GAMMA);
}

/** The link failed to train and a new mode set should try the link the
 * fallback chose: `connector`'s link status bad and a hotplug event for
 * the console and the compositor, Linux's
 * intel_dp_queue_modeset_retry_work. */
static void link_retry(struct drm_connector *connector)
{
    connector->state->link_status = DRM_MODE_LINK_STATUS_BAD;
    drm_connector_hotplug_later(connector);
}

int intel_crtc_needs_well(int pipe, const struct intel_crtc_config *cfg)
{
    return pipe != I915_PIPE_A || cfg->trans != I915_TRANS_EDP || cfg->pfit || cfg->force_thru ||
           cfg->port != INTEL_PORT_A;
}

/** Wait for pipe `pipe`'s next vblank by its frame counter, 100 ms at
 * most, as Linux's intel_crtc_wait_for_next_vblank; a pipe whose counter
 * does not move is logged. */
static void wait_next_vblank(struct drm_i915_private *i915, int pipe)
{
    uint32_t start = i915_read(&i915->hw, PIPE_FRMCOUNT_G4X(pipe));
    int ms;

    for (ms = 0; ms < 100; ms++) {
        intel_display_msleep(i915, 1);
        if (i915_read(&i915->hw, PIPE_FRMCOUNT_G4X(pipe)) != start) {
            return;
        }
    }
    klog(KLOG_WARN, "i915", "display: pipe %c: no vblank in 100 ms", 'A' + pipe);
}

/**
 * The pipe whose vblanks pipe `c` waits for once it is on, Linux's
 * hsw_mode_set_planes_workaround on Haswell: the first pipe `state` turns
 * on waits for the one pipe already on and kept, and with none kept on,
 * the second pipe it turns on waits for the first. -1 for none, as when
 * two pipes stay on.
 */
static int workaround_pipe(struct intel_crtc *c, struct drm_atomic_state *state)
{
    struct drm_i915_private *i915 = c->i915;
    int pipe, first = -1, other = -1, enabled = -1;

    for (pipe = 0; pipe < I915_NUM_PIPES; pipe++) {
        const struct drm_crtc_state *cs = state->crtcs_new[i915->display.crtc[pipe].base.index];

        if (cs && cs->active && drm_atomic_crtc_needs_modeset(cs)) {
            if (first < 0) {
                first = pipe;
            } else if (other < 0) {
                other = pipe;
            }
        }
    }
    if (first < 0) {
        return -1;
    }
    for (pipe = 0; pipe < I915_NUM_PIPES; pipe++) {
        struct drm_crtc *crtc = &i915->display.crtc[pipe].base;
        const struct drm_crtc_state *cs =
            state->crtcs_new[crtc->index] ? state->crtcs_new[crtc->index] : crtc->state;

        if (!cs || !cs->active || drm_atomic_crtc_needs_modeset(cs)) {
            continue;
        }
        if (enabled >= 0) {
            return -1;
        }
        enabled = pipe;
    }
    if (enabled >= 0) {
        return c->pipe == first ? enabled : -1;
    }
    return c->pipe == other ? first : -1;
}

/**
 * Turn pipe `c` on for its new state, Linux's hsw_crtc_enable with the
 * DDI encoder's hooks: the display well and the PLL, the link up (and
 * for DisplayPort trained), the pipe's source, the transcoder's M/N and
 * timings, the fitter, the colour, the primary plane off, the line times
 * and the watermarks, then the transcoder on and the output's last step,
 * and Haswell's two vblanks of another pipe when one comes on beside it.
 */
static void crtc_enable(struct intel_crtc *c, struct drm_atomic_state *state)
{
    struct drm_i915_private *i915 = c->i915;
    struct intel_crtc_config *cfg = &c->cfg;
    struct i915_hw *hw = &i915->hw;
    const struct drm_connector_state *conn;
    struct drm_connector *connector;
    struct intel_dpll_plan plan;
    int32_t err;
    int wa;

    err = compute_config(c, state->crtcs_new[c->base.index], state, cfg);
    if (err == 0 && cfg->active) {
        intel_dpll_plan_init(i915, 0, &plan);
        err = intel_dpll_compute(i915, &plan, c->pipe, cfg);
    }
    if (err != 0 || !cfg->active) {
        klog(KLOG_ERR, "i915", "display: pipe %c not set: error %d", 'A' + c->pipe, (int)err);
        k_memset(cfg, 0, sizeof(*cfg));
        cfg->pll = DPLL_ID_NONE;
        return;
    }
    connector = crtc_connector(c, state, &conn);
    if (intel_crtc_needs_well(c->pipe, cfg)) {
        cfg->well = i915_power_get(i915) == 0;
    }
    intel_dpll_enable(i915, c->pipe, cfg);
    /* Linux's intel_ddi_pre_enable reports the pipe's underruns first. */
    c->inherited = 0;
    intel_fifo_underrun_reporting(c, 1);
    err = intel_ddi_pre_enable(i915, cfg);
    i915_write(hw, PIPESRC(c->pipe), (cfg->src_w - 1u) << 16 | (cfg->src_h - 1u));
    configure_transcoder(c, cfg);
    ilk_pfit_enable(i915, c->pipe, cfg);
    color_commit(c, cfg);
    plane_off(c, INTEL_PLANE_PRIMARY);
    i915_write(hw, WM_LINETIME(c->pipe),
               cfg->linetime | cfg->ips_linetime << HSW_IPS_LINETIME_SHIFT);
    intel_wm_commit(c, state, 1);
    intel_ddi_enable(i915, c->pipe, cfg);
    if (err == -EAGAIN && connector) {
        link_retry(connector);
    }
    wa = workaround_pipe(c, state);
    if (wa >= 0) {
        wait_next_vblank(i915, wa);
        wait_next_vblank(i915, wa);
    }
    /* Linux's intel_crtc_enable_pipe_crc: a running source again, its
     * first frame dropped. */
    if (c->crc_ctl != 0) {
        c->crc_skip = 1;
        i915_write(hw, PIPE_CRC_CTL(c->pipe), c->crc_ctl);
    }
    if (cfg->output == INTEL_OUTPUT_HDMI) {
        klog(KLOG_INFO, "i915",
             "display: pipe %c set: %ux%u%s@%u on %s %c, %u kHz, %u bpp, TMDS %u kHz from %s%s",
             'A' + c->pipe, (unsigned)cfg->mode.hdisplay, (unsigned)cfg->mode.vdisplay,
             (cfg->mode.flags & DRM_MODE_FLAG_INTERLACE) ? "i" : "",
             (unsigned)drm_mode_vrefresh(&cfg->mode), cfg->has_hdmi_sink ? "HDMI" : "DVI",
             'A' + cfg->port, (unsigned)cfg->mode.clock, (unsigned)cfg->bpp,
             (unsigned)cfg->port_clock, i915->display.dpll[cfg->pll].name,
             cfg->limited_range ? ", limited range" : "");
        return;
    }
    klog(KLOG_INFO, "i915",
         "display: pipe %c set: %ux%u in %ux%u@%u, %u kHz, %u bpp, %u lane%s at %u kHz%s%s",
         'A' + c->pipe, (unsigned)cfg->src_w, (unsigned)cfg->src_h, (unsigned)cfg->mode.hdisplay,
         (unsigned)cfg->mode.vdisplay, (unsigned)drm_mode_vrefresh(&cfg->mode),
         (unsigned)cfg->mode.clock, (unsigned)cfg->bpp, (unsigned)cfg->lanes,
         cfg->lanes == 1 ? "" : "s", (unsigned)cfg->port_clock, cfg->pfit ? ", fitted" : "",
         cfg->output != INTEL_OUTPUT_DP ? ""
         : cfg->port == INTEL_PORT_B    ? ", DP B"
         : cfg->port == INTEL_PORT_C    ? ", DP C"
                                        : ", DP D");
}

/**
 * The pipe goes off, Linux's hsw_crtc_disable after its planes: its
 * planes turned off, the output, the transcoder, the fitter and the port
 * off, the PLL and the display well let go, and its held event sent.
 */
static void intel_crtc_disable(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
    struct intel_crtc *c = to_intel_crtc(crtc);
    struct drm_i915_private *i915 = c->i915;
    uint32_t flags;
    int slot;

    for (slot = 0; slot < INTEL_NUM_PLANES; slot++) {
        plane_off(c, slot);
    }
    i915_posting_read(&i915->hw, DSPSURF(c->pipe));
    intel_wm_cancel(c);
    /* Linux's intel_crtc_disable_pipe_crc: no CRC of a pipe going off. */
    if (c->crc_ctl != 0) {
        i915_write(&i915->hw, PIPE_CRC_CTL(c->pipe), 0);
    }
    if (c->cfg.active) {
        intel_ddi_disable(i915, &c->cfg);
        intel_ddi_post_disable(i915, c, &c->cfg);
        intel_dpll_disable(i915, c->pipe, &c->cfg);
        if (c->cfg.well) {
            i915_power_put(i915);
        }
    }
    k_memset(&c->cfg, 0, sizeof(c->cfg));
    c->cfg.pll = DPLL_ID_NONE;
    /* A pipe that stays off gives its share of the FIFO back. */
    if (!state->crtcs_new[crtc->index]->active) {
        intel_wm_commit(c, state, 1);
    }
    flags = kirq_save();
    intel_crtc_send_event(c);
    kirq_restore(flags);
}

/** Before the planes: a pipe that comes on turned on, as Linux's commit
 * tail turns it on before it writes the planes, else the watermarks
 * raised to cover the planes the commit shows; and an async flip's flip
 * done unmasked, since the chip latches only an unmasked event and the
 * flip may be done as soon as the surface is written. */
static void intel_crtc_begin(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
    struct intel_crtc *c = to_intel_crtc(crtc);
    struct drm_crtc_state *cs = state->crtcs_new[crtc->index];

    if (cs && drm_atomic_crtc_needs_modeset(cs) && cs->active) {
        crtc_enable(c, state);
    } else if (cs && !drm_atomic_crtc_needs_modeset(cs)) {
        intel_wm_commit(c, state, 0);
    }
    intel_crtc_arm_flip_done(c, state);
}

/** After the planes: the commit's event handed to the vblank path, or,
 * on a pipe that came on, left to the core, which arms it once the
 * pipe's vblanks are on. */
static void intel_crtc_flush(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
    struct intel_crtc *c = to_intel_crtc(crtc);
    struct drm_crtc_state *cs = state->crtcs_new[crtc->index];

    if (cs && drm_atomic_crtc_needs_modeset(cs)) {
        return;
    }
    /* Linux's intel_update_crtc: a pipe the firmware lit reports its
     * underruns from its first fastset. */
    if (cs && cs->active && c->inherited) {
        c->inherited = 0;
        intel_fifo_underrun_reporting(c, 1);
    }
    intel_crtc_arm_event(c, state);
}

static const struct drm_crtc_funcs intel_crtc_funcs = {intel_crtc_check,
                                                       intel_crtc_enable,
                                                       intel_crtc_disable,
                                                       intel_crtc_begin,
                                                       intel_crtc_flush,
                                                       intel_crtc_mode_valid,
                                                       intel_enable_vblank,
                                                       intel_disable_vblank,
                                                       intel_crtc_set_crc_source,
                                                       intel_crtc_verify_crc_source,
                                                       intel_crtc_get_crc_sources,
                                                       intel_get_vblank_counter,
                                                       intel_crtc_get_vblank_timestamp};

/**
 * Choose the PLLs of the pipes `state` sets again, Linux's
 * intel_compute_shared_dplls on Haswell: each pipe that goes on takes a
 * PLL that runs its clock, in the order the commit turns them on, while
 * the PLLs of the pipes it turns off are free. 0, or -EINVAL or -EBUSY
 * when no PLL is left for a clock.
 */
static int32_t check_dplls(struct drm_i915_private *i915, struct drm_atomic_state *state)
{
    struct intel_dpll_plan plan;
    struct intel_crtc_config cfg;
    uint32_t leaving = 0;
    int32_t err;
    int pipe;

    for (pipe = 0; pipe < I915_NUM_PIPES; pipe++) {
        struct drm_crtc_state *cs = state->crtcs_new[i915->display.crtc[pipe].base.index];

        if (cs && drm_atomic_crtc_needs_modeset(cs)) {
            leaving |= 1u << pipe;
        }
    }
    if (leaving == 0) {
        return 0;
    }
    intel_dpll_plan_init(i915, leaving, &plan);
    for (pipe = 0; pipe < I915_NUM_PIPES; pipe++) {
        struct intel_crtc *c = &i915->display.crtc[pipe];
        struct drm_crtc_state *cs = state->crtcs_new[c->base.index];

        if (!(leaving & (1u << pipe)) || !cs->active) {
            continue;
        }
        if ((err = compute_config(c, cs, state, &cfg)) != 0 ||
            (err = intel_dpll_compute(i915, &plan, pipe, &cfg)) != 0) {
            return err;
        }
    }
    return 0;
}

/**
 * The device's check, Linux's intel_atomic_check: a new "scaling mode"
 * sets its CRTC's mode again
 * (intel_digital_connector_atomic_check); then the core's checks, and the
 * PLLs of the pipes set again.
 */
static int32_t intel_atomic_check(struct drm_device *dev, struct drm_atomic_state *state)
{
    int32_t err;
    uint32_t i;

    for (i = 0; i < dev->mode_config.num_connector; i++) {
        const struct drm_connector_state *conn = state->connectors_new[i];
        struct drm_crtc_state *cs;

        if (!conn || !conn->crtc || conn->scaling_mode == state->connectors_old[i]->scaling_mode) {
            continue;
        }
        cs = drm_atomic_get_crtc_state(state, conn->crtc);
        if (!cs) {
            return -ENOMEM;
        }
        cs->mode_changed = 1;
    }
    err = drm_atomic_helper_check(dev, state);
    return err != 0 ? err : check_dplls(to_i915(dev), state);
}

struct intel_crtc *intel_dp_active_crtc(struct intel_dp *dp)
{
    int pipe;

    for (pipe = 0; pipe < I915_NUM_PIPES; pipe++) {
        struct intel_crtc *c = &dp->i915->display.crtc[pipe];

        if (c->cfg.active && c->cfg.output != INTEL_OUTPUT_HDMI && c->cfg.port == dp->port) {
            return c;
        }
    }
    return NULL;
}

int32_t intel_display_commit_pipe(struct intel_crtc *c)
{
    struct drm_device *dev = &c->i915->drm;
    struct drm_atomic_state *state;
    struct drm_crtc_state *cs;
    int32_t rc;

    /* A blocking commit waits for a pending flip, which the worker this
     * runs in may be the one to finish. */
    if (c->base.flip_pending) {
        return -EBUSY;
    }
    drm_modeset_lock_nointr(dev);
    state = drm_atomic_state_alloc(dev);
    cs = state ? drm_atomic_get_crtc_state(state, &c->base) : NULL;
    rc = cs ? 0 : -ENOMEM;
    if (rc == 0) {
        cs->connectors_changed = 1;
        rc = drm_atomic_add_affected_connectors(state, &c->base);
    }
    if (rc == 0) {
        rc = drm_atomic_add_affected_planes(state, &c->base);
    }
    if (rc == 0) {
        rc = drm_atomic_commit(state);
    }
    drm_atomic_state_free(state);
    drm_modeset_unlock(dev);
    return rc;
}

/** The pipe the eDP transcoder drives, which the readout found, or NULL. */
static struct intel_crtc *edp_crtc(struct drm_i915_private *i915)
{
    int pipe;

    for (pipe = 0; pipe < I915_NUM_PIPES; pipe++) {
        struct intel_crtc *c = &i915->display.crtc[pipe];

        if (c->hw.active && c->hw.mode.clock != 0 && c->hw.trans == I915_TRANS_EDP) {
            return c;
        }
    }
    return NULL;
}

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
 * Take what the firmware set pipe `c` to as its configuration, Linux's
 * readout of the CRTC state: the timings, the link, the M/N values, the
 * fitter, the line times and the PLL, and the eDP link as trained.
 */
static void adopt_config(struct drm_i915_private *i915, struct intel_crtc *c)
{
    struct intel_crtc_config *cfg = &c->cfg;
    const struct intel_pipe_readout *hw = &c->hw;
    struct intel_dp *dp = &i915->display.dp;
    uint32_t ddi = rd(i915, TRANS_DDI_FUNC_CTL(I915_TRANS_EDP)), lt;

    k_memset(cfg, 0, sizeof(*cfg));
    cfg->active = 1;
    cfg->output = INTEL_OUTPUT_EDP;
    cfg->port = INTEL_PORT_A;
    cfg->trans = I915_TRANS_EDP;
    cfg->pixel_multiplier = 1;
    cfg->dither = hw->dither;
    cfg->mode = hw->mode;
    cfg->src_w = hw->src_w;
    cfg->src_h = hw->src_h;
    cfg->bpp = hw->bpp;
    cfg->port_clock = hw->port_clock;
    cfg->lanes = hw->lanes;
    cfg->data_m = hw->data_m;
    cfg->data_n = hw->data_n;
    cfg->link_m = hw->link_m;
    cfg->link_n = hw->link_n;
    cfg->tu = hw->tu;
    cfg->pfit = hw->pf_ctl != 0;
    cfg->pf_pos = hw->pf_win_pos;
    cfg->pf_size = hw->pf_win_sz;
    cfg->force_thru = (ddi & TRANS_DDI_EDP_INPUT_MASK) == TRANS_DDI_EDP_INPUT_A_ONOFF;
    cfg->pixel_rate = intel_pfit_pixel_rate(cfg);
    lt = rd(i915, WM_LINETIME(c->pipe));
    cfg->linetime = lt & HSW_LINETIME_MASK;
    cfg->ips_linetime = (lt >> HSW_IPS_LINETIME_SHIFT) & HSW_LINETIME_MASK;
    cfg->pll = DPLL_ID_NONE;
    (void)intel_dpll_readout(i915, c->pipe, rd(i915, PORT_CLK_SEL(INTEL_PORT_A)), cfg);
    cfg->well = intel_crtc_needs_well(c->pipe, cfg) && i915_power_get(i915) == 0;
    dp->link_rate = hw->port_clock;
    dp->lane_count = hw->lanes;
    dp->link_trained = 1;
}

/**
 * Adopt the picture the firmware left on `c` into the atomic state, as
 * Linux's intel_modeset_readout_hw_state and intel_find_initial_plane_obj
 * do: the CRTC on with the readout's mode, the source's size when the
 * fitter scales it, the primary plane showing the firmware's framebuffer,
 * and the eDP connector routed to it, so that a commit that keeps the
 * mode programs the planes alone.
 */
static void adopt(struct drm_i915_private *i915, struct intel_crtc *c)
{
    struct intel_display *d = &i915->display;
    struct drm_crtc_state *cs = c->base.state;
    struct drm_plane_state *ps = c->primary.state;
    struct drm_connector_state *conn = d->connector.state;
    struct drm_mode_modeinfo mode = c->hw.mode;
    struct drm_framebuffer *fb;

    adopt_config(i915, c);
    /* Linux's intel_crtc_readout_derived_state: the mode a user set is
     * the panel's timings at the pipe's source size. */
    mode.hdisplay = (uint16_t)c->cfg.src_w;
    mode.vdisplay = (uint16_t)c->cfg.src_h;
    drm_mode_set_name(&mode);
    if (drm_atomic_set_mode_for_crtc(cs, &mode) != 0) {
        return;
    }
    cs->active = 1;
    /* Linux's intel_sanitize_fifo_underrun_reporting: an active pipe
     * starts with its underruns unreported, until its first commit. */
    c->underrun_off = 1;
    c->inherited = 1;
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
    ps->crtc_w = c->cfg.src_w;
    ps->crtc_h = c->cfg.src_h;
    ps->src_x = c->hw.x << 16;
    ps->src_y = c->hw.y << 16;
    ps->src_w = c->cfg.src_w << 16;
    ps->src_h = c->cfg.src_h << 16;
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
    klog(KLOG_INFO, "i915",
         "display: pipe %c adopted: %ux%u in %ux%u@%u, %u kHz, %u bpp, %u lane%s at %u kHz, "
         "the firmware's framebuffer at GTT 0x%x, stride %u",
         'A' + c->pipe, (unsigned)c->cfg.src_w, (unsigned)c->cfg.src_h,
         (unsigned)c->hw.mode.hdisplay, (unsigned)c->hw.mode.vdisplay,
         (unsigned)c->hw.mode.vrefresh, (unsigned)c->hw.mode.clock, (unsigned)c->hw.bpp,
         (unsigned)c->hw.lanes, c->hw.lanes == 1 ? "" : "s", (unsigned)c->hw.port_clock,
         (unsigned)c->hw.surf, (unsigned)c->hw.stride);
}

/**
 * Turn off a pipe the firmware lit on DDI B to D, which the driver does
 * not adopt, as Linux's intel_sanitize_crtc turns off a pipe it cannot
 * take: the planes, the transcoder and its function, the fitter, the
 * transcoder's clock, the port's buffer and clock, and the pipe's PLL.
 */
static void sanitize_pipe(struct drm_i915_private *i915, struct intel_crtc *c)
{
    struct i915_hw *hw = &i915->hw;
    const struct intel_pipe_readout *r = &c->hw;
    struct intel_crtc_config cfg;
    int t = r->trans, slot;

    k_memset(&cfg, 0, sizeof(cfg));
    cfg.pll = DPLL_ID_NONE;
    cfg.trans = t;
    cfg.port = r->port;
    cfg.pfit = r->pf_ctl != 0;
    (void)intel_dpll_readout(i915, c->pipe, rd(i915, PORT_CLK_SEL(r->port)), &cfg);
    for (slot = 0; slot < INTEL_NUM_PLANES; slot++) {
        plane_off(c, slot);
    }
    i915_rmw(hw, TRANSCONF(t), TRANSCONF_ENABLE, 0);
    (void)i915_wait_for(hw, "pipe off", TRANSCONF(t), TRANSCONF_STATE_ENABLE, 0, PIPE_OFF_US);
    i915_rmw(hw, TRANS_DDI_FUNC_CTL(t),
             TRANS_DDI_FUNC_ENABLE | TRANS_DDI_PORT_MASK | TRANS_DDI_MODE_SELECT_MASK, 0);
    ilk_pfit_disable(i915, c->pipe, &cfg);
    i915_write(hw, TRANS_CLK_SEL(t), TRANS_CLK_SEL_DISABLED);
    if (r->port >= INTEL_PORT_B && r->port <= INTEL_PORT_D) {
        i915_rmw(hw, DDI_BUF_CTL(r->port), DDI_BUF_CTL_ENABLE, 0);
        i915_rmw(hw, DP_TP_CTL(r->port), DP_TP_CTL_ENABLE, 0);
        (void)i915_wait_for(hw, "port idle", DDI_BUF_CTL(r->port), DDI_BUF_IS_IDLE, DDI_BUF_IS_IDLE,
                            DDI_BUF_IDLE_US);
        i915_write(hw, PORT_CLK_SEL(r->port), PORT_CLK_SEL_NONE);
    }
    intel_dpll_disable(i915, c->pipe, &cfg);
    klog(KLOG_INFO, "i915", "display: pipe %c on port %c, lit by the firmware, turned off",
         'A' + c->pipe, 'A' + r->port);
}

/**
 * Make DDI B, C and D's outputs, Linux's intel_ddi_init on Haswell: each
 * port the VBT names and SFUSE_STRAP reports, with its encoder, which
 * reaches every pipe, the bits DDI_BUF_CTL keeps (lane reversal, the
 * VBT's too), and a DisplayPort connector, then an HDMI one, as the VBT
 * says the port carries them. 0 or the DRM core's error.
 */
static int32_t ports_init(struct drm_i915_private *i915)
{
    static const uint32_t strap_bit[3] = {SFUSE_STRAP_DDIB_DETECTED, SFUSE_STRAP_DDIC_DETECTED,
                                          SFUSE_STRAP_DDID_DETECTED};
    struct intel_display *d = &i915->display;
    uint32_t strap = rd(i915, SFUSE_STRAP);
    int32_t err;
    int i;

    for (i = 0; i < 3; i++) {
        struct intel_ddi_port *p = &d->ddi[i];
        int port = INTEL_PORT_B + i;
        const struct intel_bios_encoder *enc = intel_bios_encoder_lookup(&i915->vbt, port);

        p->port = port;
        p->hpd_pin = HPD_PORT_B + i;
        p->dp.i915 = i915;
        if (!enc || !(strap & strap_bit[i])) {
            continue;
        }
        p->has_hdmi = intel_bios_encoder_supports_dvi(enc) || intel_bios_encoder_supports_hdmi(enc);
        p->has_dp = intel_bios_encoder_supports_dp(enc) != 0;
        if (!p->has_dp && !p->has_hdmi) {
            continue;
        }
        if ((err = drm_encoder_init(&i915->drm, &p->encoder, DRM_MODE_ENCODER_TMDS)) != 0) {
            return err;
        }
        p->encoder.possible_crtcs = 0x7u;
        p->present = 1;
        p->dp.saved_port_bits =
            rd(i915, DDI_BUF_CTL(port)) & (DDI_BUF_PORT_REVERSAL | DDI_A_4_LANES);
        if (enc->lane_reversal) {
            p->dp.saved_port_bits |= DDI_BUF_PORT_REVERSAL;
        }
        if ((p->has_dp && (err = intel_dp_ext_connector_init(i915, p)) != 0) ||
            (p->has_hdmi && (err = intel_hdmi_connector_init(i915, p)) != 0)) {
            return err;
        }
        klog(KLOG_INFO, "i915", "display: port %c: %s%s%s", 'A' + port, p->has_dp ? "DP" : "",
             p->has_dp && p->has_hdmi ? "+" : "", p->has_hdmi ? "HDMI" : "");
    }
    return 0;
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
    mc->atomic_check = intel_atomic_check;
    mc->atomic_commit = intel_atomic_commit;
    mc->fb_create = intel_fb_create;
    mc->fb_destroy = intel_fb_destroy;
    intel_cdclk_init(i915);
    intel_wm_init(i915);
    intel_gmbus_setup(i915);
    intel_dpll_init(i915);
    for (pipe = 0; pipe < I915_NUM_PIPES; pipe++) {
        struct intel_crtc *c = &d->crtc[pipe];

        c->i915 = i915;
        c->pipe = pipe;
        c->cfg.pll = DPLL_ID_NONE;
        if ((err = intel_planes_init(c)) != 0 ||
            (err = drm_crtc_init_with_planes(dev, &c->base, &c->primary, &c->cursor,
                                             &intel_crtc_funcs)) != 0) {
            return err;
        }
    }
    /* A board without the panel, or a panel that does not answer, has no
     * connector, as Linux's intel_edp_init_connector fails. */
    if (intel_dp_init(i915) == 0 && (err = intel_dp_connector_init(i915)) != 0) {
        return err;
    }
    if ((err = ports_init(i915)) != 0) {
        return err;
    }
    err = intel_vblank_init(i915);
    if (err != 0) {
        return err;
    }
    edp = d->dp.present ? edp_crtc(i915) : NULL;
    if (edp) {
        adopt(i915, edp);
    } else if (d->dp.present) {
        klog(KLOG_INFO, "i915", "display: no pipe lit by the firmware; the eDP panel is dark");
        /* The VDD the init's AUX messages forced, let go now as Linux's
         * timer would: nothing reaches the dark panel's AUX before a
         * mode set powers it. */
        intel_pps_vdd_off_unlocked(i915, 1);
    }
    for (pipe = 0; pipe < I915_NUM_PIPES; pipe++) {
        struct intel_crtc *c = &d->crtc[pipe];

        if (c->hw.active && c->hw.trans != I915_TRANS_EDP) {
            sanitize_pipe(i915, c);
        }
    }
    intel_dpll_sanitize(i915);
    /* After the firmware's framebuffer is the primary's, which it needs. */
    intel_wm_sanitize(i915);
    if (d->dp.present) {
        intel_backlight_setup(i915);
    }
    return intel_hotplug_init(i915);
}

void intel_display_fini(struct drm_i915_private *i915)
{
    struct intel_display *d = &i915->display;
    int pipe, slot;

    if (!d->ready) {
        return;
    }
    intel_hotplug_fini(i915);
    for (pipe = 0; pipe < I915_NUM_PIPES; pipe++) {
        intel_wm_cancel(&d->crtc[pipe]);
    }
    intel_vblank_fini(i915);
    intel_backlight_fini(i915);
    /* Linux's intel_dp_encoder_flush_work: VDD that no mode set wants
     * goes off now. */
    intel_pps_vdd_off_unlocked(i915, 1);
    for (pipe = 0; pipe < I915_NUM_PIPES; pipe++) {
        for (slot = 0; slot < INTEL_NUM_PLANES; slot++) {
            scanout_put(&d->crtc[pipe].parked[slot]);
            scanout_put(&d->crtc[pipe].shown[slot]);
        }
    }
}

int32_t intel_get_pipe_from_crtc_id_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_i915_get_pipe_from_crtc_id *args = (struct drm_i915_get_pipe_from_crtc_id *)data;
    struct drm_crtc *crtc = drm_crtc_find(dev, file, args->crtc_id);

    if (!crtc) {
        return -ENOENT;
    }
    args->pipe = (uint32_t)to_intel_crtc(crtc)->pipe;
    return 0;
}
