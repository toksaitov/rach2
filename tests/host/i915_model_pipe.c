/*
 * i915_model_pipe.c: the pipes, transcoders, planes and panel fitters of
 * the host model, the registers from 0x60000 to 0x7ffff (i915_model_pipe.h),
 * with the firmware's picture on pipe A as the Lenovo leaves it.
 */

#include "i915_model_pipe.h"
#include "i915_lenovo.h"
#include "i915_model_ring.h"

#include "drm/i915/i915.h"
#include "drm/i915/i915_reg.h"

static const struct i915_model_block i915_model_pipe_block;

/** The state of the block in `m`. */
static struct i915_model_pipes *pipes_of(struct i915_model *m)
{
    int i;

    for (i = 0; i < i915_model_nblocks(); i++) {
        if (i915_model_block_at(i) == &i915_model_pipe_block) {
            return (struct i915_model_pipes *)m->priv[i];
        }
    }
    return 0;
}

struct i915_model_pipes *i915_model_pipes(struct i915_model *m)
{
    return pipes_of(m);
}

/** The transcoder routed to `pipe`, by the eDP transcoder's input or the
 * pipe's own function control, or -1. */
static int pipe_trans(struct i915_model *m, int pipe)
{
    uint32_t edp = i915_model_peek(m, TRANS_DDI_FUNC_CTL(I915_TRANS_EDP));
    int in = -1;

    if (edp & TRANS_DDI_FUNC_ENABLE) {
        switch (edp & TRANS_DDI_EDP_INPUT_MASK) {
        case TRANS_DDI_EDP_INPUT_A_ON:
        case TRANS_DDI_EDP_INPUT_A_ONOFF:
            in = I915_PIPE_A;
            break;
        case TRANS_DDI_EDP_INPUT_B_ONOFF:
            in = I915_PIPE_B;
            break;
        case TRANS_DDI_EDP_INPUT_C_ONOFF:
            in = I915_PIPE_C;
            break;
        default:
            break;
        }
    }
    if (in == pipe) {
        return I915_TRANS_EDP;
    }
    return (i915_model_peek(m, TRANS_DDI_FUNC_CTL(pipe)) & TRANS_DDI_FUNC_ENABLE) ? pipe : -1;
}

int i915_model_pipe_on(struct i915_model *m, int pipe)
{
    int t = pipe_trans(m, pipe);

    return t >= 0 && (i915_model_peek(m, TRANSCONF(t)) & TRANSCONF_ENABLE);
}

/** The pipe whose register `reg` is, by its 0x1000 spacing in the plane
 * range, and the register at pipe A's offset in `*a`. */
static int reg_pipe(uint32_t reg, uint32_t *a)
{
    int pipe = (int)((reg - 0x70000u) / 0x1000u);

    *a = reg - (uint32_t)pipe * 0x1000u;
    return pipe;
}

/** The firmware's state: pipe A on the eDP transcoder with the panel's
 * mode, its primary plane scanning out the framebuffer at GTT 0, the
 * panel lit by the sequencer and the PCH's PWM, as the dump has them. */
static void pipe_reset(struct i915_model *m, void *priv)
{
    struct i915_model_pipes *s = (struct i915_model_pipes *)priv;
    struct i915_model_plane *pri = &s->pipe[I915_PIPE_A].primary;
    int t = I915_TRANS_EDP;

    i915_model_poke(m, TRANS_DDI_FUNC_CTL(t), LENOVO_FW_EDP_DDI_FUNC);
    i915_model_poke(m, TRANSCONF(t), LENOVO_FW_EDP_TRANSCONF);
    i915_model_poke(m, TRANS_HTOTAL(t), (LENOVO_PANEL_HTOTAL - 1u) << 16 | (1366u - 1u));
    i915_model_poke(m, TRANS_HBLANK(t), (LENOVO_PANEL_HTOTAL - 1u) << 16 | (1366u - 1u));
    i915_model_poke(m, TRANS_HSYNC(t), (1384u - 1u) << 16 | (1374u - 1u));
    i915_model_poke(m, TRANS_VTOTAL(t), (LENOVO_PANEL_VTOTAL - 1u) << 16 | (768u - 1u));
    i915_model_poke(m, TRANS_VBLANK(t), (LENOVO_PANEL_VTOTAL - 1u) << 16 | (768u - 1u));
    i915_model_poke(m, TRANS_VSYNC(t), (772u - 1u) << 16 | (771u - 1u));
    i915_model_poke(m, PIPE_DATA_M1(t), LENOVO_DATA_M);
    i915_model_poke(m, PIPE_DATA_N1(t), LENOVO_DATA_N);
    i915_model_poke(m, PIPE_LINK_M1(t), LENOVO_LINK_M);
    i915_model_poke(m, PIPE_LINK_N1(t), LENOVO_LINK_N);
    i915_model_poke(m, DDI_BUF_CTL(0), LENOVO_FW_DDI_BUF_CTL_A);
    i915_model_poke(m, PORT_CLK_SEL(0), LENOVO_FW_PORT_CLK_A);
    i915_model_poke(m, PIPESRC(I915_PIPE_A),
                    ((uint32_t)(LENOVO_FW_FB_WIDTH - 1) << 16) | (LENOVO_FW_FB_HEIGHT - 1));
    i915_model_poke(m, PIPE_FRMCOUNT_G4X(I915_PIPE_A), LENOVO_FW_FRMCOUNT);
    i915_model_poke(m, DSPCNTR(I915_PIPE_A), DISP_ENABLE | DISP_FORMAT_BGRX888);
    i915_model_poke(m, DSPSURF(I915_PIPE_A), LENOVO_FW_FB_GGTT);
    i915_model_poke(m, DSPSTRIDE(I915_PIPE_A), LENOVO_FW_FB_STRIDE);
    i915_model_poke(m, DSPOFFSET(I915_PIPE_A), 0);
    pri->ctl = DISP_ENABLE | DISP_FORMAT_BGRX888;
    pri->surf = LENOVO_FW_FB_GGTT;
    pri->stride = LENOVO_FW_FB_STRIDE;
    i915_model_poke(m, PP_STATUS, LENOVO_FW_PP_STATUS);
    i915_model_poke(m, PP_CONTROL, LENOVO_FW_PP_CONTROL);
    i915_model_poke(m, PP_ON_DELAYS, LENOVO_FW_PP_ON_DELAYS);
    i915_model_poke(m, PP_OFF_DELAYS, LENOVO_FW_PP_OFF_DELAYS);
    i915_model_poke(m, BLC_PWM_CPU_CTL2, LENOVO_FW_BLC_CPU_CTL2);
    i915_model_poke(m, BLC_PWM_CPU_CTL, LENOVO_FW_BLC_CPU_CTL);
    i915_model_poke(m, BLC_PWM_PCH_CTL1, LENOVO_FW_BLC_PCH_CTL1);
    i915_model_poke(m, BLC_PWM_PCH_CTL2, LENOVO_FW_BLC_PCH_CTL2);
}

/* The addresses of a plane's registers, 0 for one the plane does not have. */
struct plane_regs {
    uint32_t ctl, stride, pos, size, offset, surf;
};

/** Take a plane's registers `r` from the register file, as a vblank
 * latches them. */
static void plane_take(struct i915_model *m, struct i915_model_plane *p, const struct plane_regs *r)
{
    p->ctl = i915_model_peek(m, r->ctl);
    p->stride = r->stride ? i915_model_peek(m, r->stride) : 0;
    p->pos = r->pos ? i915_model_peek(m, r->pos) : 0;
    p->size = r->size ? i915_model_peek(m, r->size) : 0;
    p->offset = r->offset ? i915_model_peek(m, r->offset) : 0;
    p->surf = i915_model_peek(m, r->surf);
    p->armed = 0;
}

/** The live surface registers answer from what the pipe scans out. */
static uint32_t pipe_read(struct i915_model *m, void *priv, uint32_t reg)
{
    struct i915_model_pipes *s = (struct i915_model_pipes *)priv;
    uint32_t a;
    int pipe;

    if (reg >= 0x70000u && reg < 0x73000u) {
        pipe = reg_pipe(reg, &a);
        if (a == DSPSURFLIVE(0)) {
            return s->pipe[pipe].primary.surf;
        }
        if (a == SPRSURFLIVE(0)) {
            return s->pipe[pipe].sprite.surf;
        }
        if (a == CURSURFLIVE(0)) {
            return s->pipe[pipe].cursor.surf;
        }
    }
    return i915_model_peek(m, reg);
}

/**
 * A write: stored, and a plane's surface, or the cursor's base, arms the
 * plane's registers for the next vblank; the primary's surface with the
 * async bit in its control is taken at once and raises its flip done. The
 * live and frame counter registers take nothing.
 */
static void pipe_write(struct i915_model *m, void *priv, uint32_t reg, uint32_t value)
{
    struct i915_model_pipes *s = (struct i915_model_pipes *)priv;
    struct i915_model_pipe *p;
    uint32_t a;
    int pipe;

    if (reg < 0x70000u || reg >= 0x73000u) {
        i915_model_poke(m, reg, value);
        return;
    }
    pipe = reg_pipe(reg, &a);
    p = &s->pipe[pipe];
    if (a == DSPSURFLIVE(0) || a == SPRSURFLIVE(0) || a == CURSURFLIVE(0) ||
        a == PIPE_FRMCOUNT_G4X(0)) {
        return;
    }
    i915_model_poke(m, reg, value);
    if (a == DSPSURF(0)) {
        if (i915_model_peek(m, DSPCNTR(pipe)) & DISP_ASYNC_FLIP) {
            p->primary.surf = value & ~0xfffu;
            p->primary.ctl = i915_model_peek(m, DSPCNTR(pipe));
            p->flips++;
            i915_model_irq_event(m, I915_IRQ_DE, DE_PLANE_FLIP_DONE_IVB(pipe));
        } else {
            p->primary.armed = 1;
        }
    } else if (a == SPRSURF(0)) {
        p->sprite.armed = 1;
    } else if (a == CURBASE(0)) {
        p->cursor.armed = 1;
    }
}

/** FNV-1a over `n` words. */
static uint32_t fnv(uint32_t h, const uint32_t *w, int n)
{
    int i, b;

    for (i = 0; i < n; i++) {
        for (b = 0; b < 32; b += 8) {
            h ^= (w[i] >> b) & 0xffu;
            h *= 16777619u;
        }
    }
    return h;
}

/** The hash of a plane's scanned out registers. */
static uint32_t plane_hash(uint32_t h, const struct i915_model_plane *p)
{
    uint32_t w[6];

    w[0] = p->ctl;
    w[1] = p->stride;
    w[2] = p->pos;
    w[3] = p->size;
    w[4] = p->offset;
    w[5] = p->surf;
    return fnv(h, w, 6);
}

uint32_t i915_model_pipe_crc(struct i915_model *m, int pipe, uint32_t ctl)
{
    struct i915_model_pipe *p = &pipes_of(m)->pipe[pipe];
    uint32_t h = 2166136261u;

    switch (ctl & PIPE_CRC_SOURCE_MASK_IVB) {
    case PIPE_CRC_SOURCE_PRIMARY_IVB:
        return plane_hash(h, &p->primary);
    case PIPE_CRC_SOURCE_SPRITE_IVB:
        return plane_hash(h, &p->sprite);
    default:
        return plane_hash(plane_hash(plane_hash(h, &p->primary), &p->sprite), &p->cursor);
    }
}

void i915_model_pipe_vblank(struct i915_model *m, int pipe)
{
    struct i915_model_pipe *p = &pipes_of(m)->pipe[pipe];
    uint32_t ctl;

    if (!i915_model_pipe_on(m, pipe)) {
        return;
    }
    p->vblanks++;
    p->us = 0;
    i915_model_poke(m, PIPE_FRMCOUNT_G4X(pipe), i915_model_peek(m, PIPE_FRMCOUNT_G4X(pipe)) + 1u);
    if (p->primary.armed) {
        struct plane_regs r = {DSPCNTR(pipe),   DSPSTRIDE(pipe), 0, 0,
                               DSPOFFSET(pipe), DSPSURF(pipe)};

        plane_take(m, &p->primary, &r);
    }
    if (p->sprite.armed) {
        struct plane_regs r = {SPRCTL(pipe),  SPRSTRIDE(pipe), SPRPOS(pipe),
                               SPRSIZE(pipe), SPROFFSET(pipe), SPRSURF(pipe)};

        plane_take(m, &p->sprite, &r);
    }
    if (p->cursor.armed) {
        struct plane_regs r = {CURCNTR(pipe), 0, CURPOS(pipe), 0, 0, CURBASE(pipe)};

        plane_take(m, &p->cursor, &r);
    }
    ctl = i915_model_peek(m, PIPE_CRC_CTL(pipe));
    if (ctl & PIPE_CRC_ENABLE) {
        i915_model_poke(m, PIPE_CRC_RES_HSW(pipe), i915_model_pipe_crc(m, pipe, ctl));
        p->crc_frames++;
    }
    i915_model_irq_event(m, I915_IRQ_DE, DE_PIPE_VBLANK_IVB(pipe));
}

/** The microseconds of a frame of `pipe`'s transcoder: its total pixels
 * at the clock the link's M/N give of 270 MHz, or 0 for none. */
static uint64_t frame_us(struct i915_model *m, int pipe)
{
    int t = pipe_trans(m, pipe);
    uint64_t n, clock_khz, pixels;

    if (t < 0) {
        return 0;
    }
    n = i915_model_peek(m, PIPE_LINK_N1(t)) & DATA_LINK_M_N_MASK;
    if (n == 0) {
        return 0;
    }
    clock_khz = (uint64_t)(i915_model_peek(m, PIPE_LINK_M1(t)) & DATA_LINK_M_N_MASK) * 270000u / n;
    pixels = (uint64_t)((i915_model_peek(m, TRANS_HTOTAL(t)) >> 16) + 1u) *
             ((i915_model_peek(m, TRANS_VTOTAL(t)) >> 16) + 1u);
    return clock_khz ? pixels * 1000u / clock_khz : 0;
}

/** Time passes: each pipe that is on has a vblank per frame. */
static void pipe_tick(struct i915_model *m, void *priv, uint32_t us)
{
    struct i915_model_pipes *s = (struct i915_model_pipes *)priv;
    int pipe;

    for (pipe = 0; pipe < 3; pipe++) {
        uint64_t frame = frame_us(m, pipe);

        if (frame == 0 || !i915_model_pipe_on(m, pipe)) {
            continue;
        }
        s->pipe[pipe].us += us;
        while (s->pipe[pipe].us >= frame) {
            s->pipe[pipe].us -= frame;
            i915_model_pipe_vblank(m, pipe);
        }
    }
}

static const struct i915_model_block i915_model_pipe_block = {
    "pipes",    0x60000u,  0x7fffcu,   sizeof(struct i915_model_pipes),
    pipe_reset, pipe_read, pipe_write, pipe_tick};
