/*
 * intel_wm.c: Haswell's display watermarks, after Linux's i9xx_wm.c for
 * ILK to BDW: the memory latencies, each pipe's watermarks at its five
 * levels from the planes it shows, and the registers merged from all pipes.
 */

#include "i915.h"
#include "i915_reg.h"

#include "../../klog.h"
#include "../../lib/kerrno.h"
#include "../../lib/kstring.h"

/* The register fields of WM0_PIPE and WM_LP (Linux's i9xx_wm_regs.h). */
#define WM0_PIPE_CURSOR_MASK 0xffu
#define WM_LP_FBC_ILK_MASK   0xfu
#define WM_LP_PRIMARY_MASK   0x7ffu
#define WM_LP_CURSOR_MASK    0xffu
#define WM_LP_SPRITE_MASK    0x7ffu

/* Haswell's display FIFO in cachelines, shared by its three pipes. */
#define DISPLAY_FIFO 768u

/* The most each value of a level may be (Linux's ilk_wm_maximums). */
struct wm_max {
    uint32_t pri, spr, cur, fbc;
};

/* What the pipes on show ask of the low-power levels (intel_wm_config). */
struct wm_config {
    int pipes;
    uint8_t sprites_enabled, sprites_scaled;
};

/** The larger of `a` and `b`. */
static uint32_t wm_maxu(uint32_t a, uint32_t b)
{
    return a > b ? a : b;
}

/** The smaller of `a` and `b`. */
static uint32_t wm_minu(uint32_t a, uint32_t b)
{
    return a < b ? a : b;
}

/** Linux's ilk_wm_method1: the cachelines the latency drains at `rate`
 * kHz and `cpp` bytes a pixel, plus two. `latency` in 0.1 us. */
static uint32_t method1(uint32_t rate, uint32_t cpp, uint32_t latency)
{
    uint64_t bytes = drm_div64((uint64_t)rate * cpp * latency + 9999u, 10000u);

    return (uint32_t)((bytes + 63u) / 64u) + 2u;
}

/** Linux's ilk_wm_method2: the lines the latency spans and one more, of
 * `width` pixels, in cachelines plus two. */
static uint32_t method2(uint32_t rate, uint32_t htotal, uint32_t width, uint32_t cpp,
                        uint32_t latency)
{
    uint32_t lines;

    if (htotal == 0) {
        htotal = 1;
    }
    lines = (uint32_t)drm_div64((uint64_t)latency * rate, (uint64_t)htotal * 10000u);
    return ((lines + 1u) * width * cpp + 63u) / 64u + 2u;
}

/*
 * A pipe's inputs to its watermarks: the pixel rate after the fitter and
 * the panel's line, and each plane's state when the plane shows on it.
 */
struct wm_input {
    uint32_t rate, htotal;
    const struct drm_plane_state *pri, *spr, *cur;
};

/** The source's width in pixels of plane state `ps`. */
static uint32_t src_width(const struct drm_plane_state *ps)
{
    return ps->src_w >> 16;
}

/** Linux's ilk_compute_wm_level: level `level`'s values for `in`; a
 * latency of 0 makes the level unusable (the values' most). */
static void compute_level(const struct drm_i915_private *i915, int level, const struct wm_input *in,
                          struct intel_wm_level *r)
{
    uint32_t lat = i915->display.wm_latency[level];
    const struct drm_plane_state *ps;

    /* Levels 1 and up are in 0.5 us. */
    if (level > 0) {
        lat *= 5u;
    }
    k_memset(r, 0, sizeof(*r));
    r->enable = 1;
    if (lat == 0) {
        r->pri = r->spr = r->cur = 0xffffffffu;
        return;
    }
    if ((ps = in->pri) != NULL) {
        r->pri = method1(in->rate, ps->fb->info->cpp[0], lat);
        if (level > 0) {
            r->pri = wm_minu(
                r->pri, method2(in->rate, in->htotal, src_width(ps), ps->fb->info->cpp[0], lat));
        }
        /* Linux's ilk_wm_fbc. */
        r->fbc = (r->pri * 64u + src_width(ps) * ps->fb->info->cpp[0] - 1u) /
                     (src_width(ps) * ps->fb->info->cpp[0]) +
                 2u;
    }
    if ((ps = in->spr) != NULL) {
        r->spr = wm_minu(method1(in->rate, ps->fb->info->cpp[0], lat),
                         method2(in->rate, in->htotal, src_width(ps), ps->fb->info->cpp[0], lat));
    }
    if ((ps = in->cur) != NULL) {
        r->cur = method2(in->rate, in->htotal, src_width(ps), ps->fb->info->cpp[0], lat);
    }
}

/** Linux's ilk_plane_wm_max for Haswell: a pipe's share of the FIFO at
 * level 0 or with more pipes on, split with a sprite 1:1 or 5:6, within
 * what the register holds. */
static uint32_t plane_max(int level, const struct wm_config *config, int part_5_6, int sprite)
{
    uint32_t fifo = DISPLAY_FIFO;

    if (sprite && !config->sprites_enabled) {
        return 0;
    }
    if (level == 0 || config->pipes > 1) {
        fifo /= I915_NUM_PIPES;
    }
    if (config->sprites_enabled) {
        if (level > 0 && part_5_6) {
            fifo = (sprite ? fifo * 5u : fifo) / 6u;
        } else {
            fifo /= 2u;
        }
    }
    return wm_minu(fifo, level == 0 ? 127u : 1023u);
}

/** Linux's ilk_compute_wm_maximums. */
static void level_max(int level, const struct wm_config *config, int part_5_6, struct wm_max *max)
{
    max->pri = plane_max(level, config, part_5_6, 0);
    max->spr = plane_max(level, config, part_5_6, 1);
    max->cur = level > 0 && config->pipes > 1 ? 64u : level == 0 ? 63u : 255u;
    max->fbc = WM_LP_FBC_ILK_MASK;
}

/** Linux's ilk_compute_wm_reg_maximums: what the registers hold. */
static void reg_max(int level, struct wm_max *max)
{
    max->pri = max->spr = level == 0 ? 127u : 1023u;
    max->cur = level == 0 ? 63u : 255u;
    max->fbc = WM_LP_FBC_ILK_MASK;
}

/** Linux's ilk_validate_wm_level: whether `r` fits `max`; a level 0 that
 * does not is clamped and kept on, as Linux's does. */
static int validate_level(int level, const struct wm_max *max, struct intel_wm_level *r)
{
    int ok;

    if (!r->enable) {
        return 0;
    }
    ok = r->pri <= max->pri && r->spr <= max->spr && r->cur <= max->cur;
    r->enable = (uint8_t)ok;
    if (level == 0 && !ok) {
        r->pri = wm_minu(r->pri, max->pri);
        r->spr = wm_minu(r->spr, max->spr);
        r->cur = wm_minu(r->cur, max->cur);
        r->enable = 1;
    }
    return ok;
}

/** Linux's ilk_validate_pipe_wm: level 0 within the pipe's share of the
 * FIFO, split 1:1 with its sprite. */
static int validate_pipe(struct intel_pipe_wm *wm)
{
    struct wm_config config;
    struct wm_max max;

    config.pipes = 1;
    config.sprites_enabled = wm->sprites_enabled;
    config.sprites_scaled = wm->sprites_scaled;
    level_max(0, &config, 0, &max);
    return validate_level(0, &max, &wm->wm[0]);
}

/** The state of `plane` in `state`, or its state now. */
static const struct drm_plane_state *plane_state(struct drm_atomic_state *state,
                                                 struct drm_plane *plane)
{
    const struct drm_plane_state *ps = state ? state->planes_new[plane->index] : NULL;

    return ps ? ps : plane->state;
}

/** Linux's intel_wm_plane_visible: the plane shows on `c`, the cursor
 * with any framebuffer; its state, or NULL. */
static const struct drm_plane_state *shown(const struct intel_crtc *c,
                                           const struct drm_plane_state *ps, int cursor)
{
    if (!ps || ps->crtc != &c->base || !ps->fb || (!cursor && !ps->visible)) {
        return NULL;
    }
    return ps;
}

int32_t intel_wm_compute(struct intel_crtc *c, struct drm_atomic_state *state,
                         const struct intel_crtc_config *cfg, struct intel_pipe_wm *wm)
{
    struct wm_input in;
    struct wm_max max;
    int level, usable = INTEL_WM_LEVELS - 1;

    k_memset(wm, 0, sizeof(*wm));
    k_memset(&in, 0, sizeof(in));
    wm->pipe_enabled = cfg->active;
    if (cfg->active) {
        in.rate = cfg->pixel_rate;
        in.htotal = cfg->mode.htotal;
        in.pri = shown(c, plane_state(state, &c->primary), 0);
        in.spr = shown(c, plane_state(state, &c->sprite), 0);
        in.cur = shown(c, plane_state(state, &c->cursor), 1);
    }
    wm->sprites_enabled = in.spr != NULL;
    wm->sprites_scaled =
        in.spr && (in.spr->src_w >> 16 != in.spr->crtc_w || in.spr->src_h >> 16 != in.spr->crtc_h);
    /* Linux's LP1 and up only without a scaled sprite. */
    if (wm->sprites_scaled) {
        usable = 0;
    }
    compute_level(c->i915, 0, &in, &wm->wm[0]);
    if (!validate_pipe(wm)) {
        return -EINVAL;
    }
    reg_max(1, &max);
    for (level = 1; level <= usable; level++) {
        compute_level(c->i915, level, &in, &wm->wm[level]);
        /* A level past what the registers hold is never valid. */
        if (!validate_level(level, &max, &wm->wm[level])) {
            k_memset(&wm->wm[level], 0, sizeof(wm->wm[level]));
            break;
        }
    }
    return 0;
}

int32_t intel_wm_intermediate(const struct intel_pipe_wm *old, struct intel_pipe_wm *wm)
{
    int level;

    wm->pipe_enabled |= old->pipe_enabled;
    wm->sprites_enabled |= old->sprites_enabled;
    wm->sprites_scaled |= old->sprites_scaled;
    for (level = 0; level < INTEL_WM_LEVELS; level++) {
        struct intel_wm_level *a = &wm->wm[level];
        const struct intel_wm_level *b = &old->wm[level];

        a->enable &= b->enable;
        a->pri = wm_maxu(a->pri, b->pri);
        a->spr = wm_maxu(a->spr, b->spr);
        a->cur = wm_maxu(a->cur, b->cur);
        a->fbc = wm_maxu(a->fbc, b->fbc);
    }
    return validate_pipe(wm) ? 0 : -EINVAL;
}

/** Linux's ilk_wm_merge: each low-power level the most of the pipes on,
 * off past the first that does not fit `max`, the compressor's
 * watermarks off before a level is. */
static void merge(const struct drm_i915_private *i915, const struct wm_max *max,
                  struct intel_pipe_wm *merged)
{
    int level, pipe, last = INTEL_WM_LEVELS - 1;

    k_memset(merged, 0, sizeof(*merged));
    merged->fbc_wm_enabled = 1;
    for (level = 1; level < INTEL_WM_LEVELS; level++) {
        struct intel_wm_level *r = &merged->wm[level];

        r->enable = 1;
        for (pipe = 0; pipe < I915_NUM_PIPES; pipe++) {
            const struct intel_pipe_wm *a = &i915->display.crtc[pipe].wm_active;
            const struct intel_wm_level *w = &a->wm[level];

            if (!a->pipe_enabled) {
                continue;
            }
            /* A level once used keeps its values while it is off. */
            if (!w->enable) {
                r->enable = 0;
            }
            r->pri = wm_maxu(r->pri, w->pri);
            r->spr = wm_maxu(r->spr, w->spr);
            r->cur = wm_maxu(r->cur, w->cur);
            r->fbc = wm_maxu(r->fbc, w->fbc);
        }
        if (level > last) {
            r->enable = 0;
        } else if (!validate_level(level, max, r)) {
            last = level - 1;
        }
        if (r->fbc > max->fbc) {
            if (r->enable) {
                merged->fbc_wm_enabled = 0;
            }
            r->fbc = 0;
        }
    }
}

/** Linux's ilk_find_best_result: the merge with the highest level on, the
 * one with the compressor's watermarks of equals, `a` of the same. */
static int best_is_b(const struct intel_pipe_wm *a, const struct intel_pipe_wm *b)
{
    int level, la = 0, lb = 0;

    for (level = 1; level < INTEL_WM_LEVELS; level++) {
        if (a->wm[level].enable) {
            la = level;
        }
        if (b->wm[level].enable) {
            lb = level;
        }
    }
    return la == lb ? b->fbc_wm_enabled && !a->fbc_wm_enabled : lb > la;
}

/** Linux's ilk_compute_wm_results: the registers' values for `merged`,
 * LP1 to LP3 levels 1 to 3, or 1, 3 and 4 when level 4 is on. */
static void results(const struct drm_i915_private *i915, const struct intel_pipe_wm *merged,
                    int part_5_6, struct intel_wm_values *v)
{
    int lp, pipe;

    k_memset(v, 0, sizeof(*v));
    v->enable_fbc_wm = merged->fbc_wm_enabled;
    v->partition_5_6 = (uint8_t)part_5_6;
    for (lp = 1; lp <= 3; lp++) {
        int level = lp + (lp >= 2 && merged->wm[4].enable);
        const struct intel_wm_level *r = &merged->wm[level];

        /* The latency field is the level's double on Haswell. Values of a
         * level off are kept, lest the FIFO underrun. */
        v->wm_lp[lp - 1] = (uint32_t)(2 * level) << WM_LP_LATENCY_SHIFT |
                           (r->pri & WM_LP_PRIMARY_MASK) << WM_LP_PRIMARY_SHIFT |
                           (r->cur & WM_LP_CURSOR_MASK) |
                           (r->fbc & WM_LP_FBC_ILK_MASK) << WM_LP_FBC_ILK_SHIFT;
        if (r->enable) {
            v->wm_lp[lp - 1] |= WM_LP_ENABLE;
        }
        v->wm_lp_spr[lp - 1] = r->spr & WM_LP_SPRITE_MASK;
    }
    for (pipe = 0; pipe < I915_NUM_PIPES; pipe++) {
        const struct intel_wm_level *r = &i915->display.crtc[pipe].wm_active.wm[0];

        v->wm_pipe[pipe] = r->pri << WM0_PIPE_PRIMARY_SHIFT | r->spr << WM0_PIPE_SPRITE_SHIFT |
                           (r->cur & WM0_PIPE_CURSOR_MASK);
    }
}

/** Linux's ilk_write_wm_values: write what changed, the low-power levels
 * from the first that changed turned off first, and anything else that
 * changed with all of them off. */
static void write_values(struct drm_i915_private *i915, const struct intel_wm_values *v)
{
    static const uint32_t lp_reg[3] = {WM1_LP_ILK, WM2_LP_ILK, WM3_LP_ILK};
    static const uint32_t spr_reg[3] = {WM1S_LP_ILK, WM2S_LP_IVB, WM3S_LP_IVB};
    struct intel_wm_values *hw = &i915->display.wm_hw;
    int pipe, lp, first = 4, all = 0;

    for (pipe = 0; pipe < I915_NUM_PIPES; pipe++) {
        if (hw->wm_pipe[pipe] != v->wm_pipe[pipe]) {
            all = 1;
        }
    }
    if (hw->enable_fbc_wm != v->enable_fbc_wm || hw->partition_5_6 != v->partition_5_6) {
        all = 1;
    }
    for (lp = 1; lp <= 3 && first == 4; lp++) {
        if (all || hw->wm_lp[lp - 1] != v->wm_lp[lp - 1] ||
            hw->wm_lp_spr[lp - 1] != v->wm_lp_spr[lp - 1]) {
            first = lp;
        }
    }
    if (!all && first == 4) {
        return;
    }
    for (lp = 3; lp >= first; lp--) {
        if (hw->wm_lp[lp - 1] & WM_LP_ENABLE) {
            hw->wm_lp[lp - 1] &= ~WM_LP_ENABLE;
            i915_write(&i915->hw, lp_reg[lp - 1], hw->wm_lp[lp - 1]);
        }
    }
    for (pipe = 0; pipe < I915_NUM_PIPES; pipe++) {
        if (hw->wm_pipe[pipe] != v->wm_pipe[pipe]) {
            i915_write(&i915->hw, WM0_PIPE_ILK(pipe), v->wm_pipe[pipe]);
        }
    }
    if (hw->partition_5_6 != v->partition_5_6) {
        i915_rmw(&i915->hw, WM_MISC, WM_MISC_DATA_PARTITION_5_6,
                 v->partition_5_6 ? WM_MISC_DATA_PARTITION_5_6 : 0);
    }
    if (hw->enable_fbc_wm != v->enable_fbc_wm) {
        i915_rmw(&i915->hw, DISP_ARB_CTL, DISP_FBC_WM_DIS, v->enable_fbc_wm ? 0 : DISP_FBC_WM_DIS);
    }
    for (lp = first; lp <= 3; lp++) {
        if (hw->wm_lp_spr[lp - 1] != v->wm_lp_spr[lp - 1]) {
            i915_write(&i915->hw, spr_reg[lp - 1], v->wm_lp_spr[lp - 1]);
        }
    }
    for (lp = first; lp <= 3; lp++) {
        if (hw->wm_lp[lp - 1] != v->wm_lp[lp - 1]) {
            i915_write(&i915->hw, lp_reg[lp - 1], v->wm_lp[lp - 1]);
        }
    }
    *hw = *v;
}

/** Linux's ilk_program_watermarks: merge the pipes' active watermarks,
 * try the 5:6 split for one pipe with a sprite, and write the best. */
static void program(struct drm_i915_private *i915)
{
    struct intel_pipe_wm m12, m56;
    struct intel_wm_values v;
    struct wm_config config;
    struct wm_max max;
    int pipe, part = 0;

    k_memset(&config, 0, sizeof(config));
    for (pipe = 0; pipe < I915_NUM_PIPES; pipe++) {
        const struct intel_pipe_wm *a = &i915->display.crtc[pipe].wm_active;

        if (a->pipe_enabled) {
            config.sprites_enabled |= a->sprites_enabled;
            config.sprites_scaled |= a->sprites_scaled;
            config.pipes++;
        }
    }
    level_max(1, &config, 0, &max);
    merge(i915, &max, &m12);
    if (config.pipes == 1 && config.sprites_enabled) {
        level_max(1, &config, 1, &max);
        merge(i915, &max, &m56);
        part = best_is_b(&m12, &m56);
    }
    results(i915, part ? &m56 : &m12, part, &v);
    write_values(i915, &v);
}

/** Whether the watermarks `a` and `b` are the same. */
static int same_wm(const struct intel_pipe_wm *a, const struct intel_pipe_wm *b)
{
    int level;

    if (a->pipe_enabled != b->pipe_enabled || a->sprites_enabled != b->sprites_enabled ||
        a->sprites_scaled != b->sprites_scaled) {
        return 0;
    }
    for (level = 0; level < INTEL_WM_LEVELS; level++) {
        const struct intel_wm_level *x = &a->wm[level], *y = &b->wm[level];

        if (x->enable != y->enable || x->pri != y->pri || x->spr != y->spr || x->cur != y->cur ||
            x->fbc != y->fbc) {
            return 0;
        }
    }
    return 1;
}

void intel_wm_commit(struct intel_crtc *c, struct drm_atomic_state *state, int modeset)
{
    struct intel_pipe_wm optimal, wm;

    if (intel_wm_compute(c, state, &c->cfg, &optimal) != 0) {
        /* The check refused this; keep what the hardware has. */
        return;
    }
    wm = optimal;
    intel_wm_cancel(c);
    /* Watermarks safe for the planes before and after the vblank, Linux's
     * intermediate; a mode set takes its own at once. */
    if (!modeset && optimal.pipe_enabled && intel_wm_intermediate(&c->wm_optimal, &wm) != 0) {
        wm = optimal;
    }
    c->wm_optimal = optimal;
    c->wm_active = wm;
    program(c->i915);
    if (!same_wm(&wm, &optimal) && drm_crtc_vblank_get(&c->base) == 0) {
        c->wm_pending = 1;
        c->wm_frame = intel_get_vblank_counter(&c->base);
    }
}

void intel_wm_cancel(struct intel_crtc *c)
{
    if (c->wm_pending) {
        c->wm_pending = 0;
        drm_crtc_vblank_put(&c->base);
    }
}

void intel_wm_vblank(struct intel_crtc *c)
{
    if (!c->wm_pending || intel_get_vblank_counter(&c->base) == c->wm_frame) {
        return;
    }
    c->wm_pending = 0;
    c->wm_active = c->wm_optimal;
    program(c->i915);
    drm_crtc_vblank_put(&c->base);
}

/** Turn a low-power level's register off, written only when it was on,
 * as Linux's intel_uncore_rmw writes only a change. */
static void lp_off(struct drm_i915_private *i915, uint32_t reg)
{
    uint32_t v = i915_read(&i915->hw, reg);

    if (v & WM_LP_ENABLE) {
        i915_write(&i915->hw, reg, v & ~WM_LP_ENABLE);
    }
}

void intel_wm_init(struct drm_i915_private *i915)
{
    struct intel_display *d = &i915->display;
    struct intel_wm_values *hw = &d->wm_hw;
    uint32_t lo = i915_read(&i915->hw, MCH_SSKPD), hi = i915_read(&i915->hw, MCH_SSKPD_HI);
    int pipe, level;

    /* Linux's hsw_read_wm_latency; the sprite and cursor take the same. */
    d->wm_latency[0] = (uint16_t)(hi >> 24);
    if (d->wm_latency[0] == 0) {
        d->wm_latency[0] = (uint16_t)(lo & 0xfu);
    }
    d->wm_latency[1] = (uint16_t)((lo >> 4) & 0xffu);
    d->wm_latency[2] = (uint16_t)((lo >> 12) & 0xffu);
    d->wm_latency[3] = (uint16_t)((lo >> 20) & 0x1ffu);
    d->wm_latency[4] = (uint16_t)(hi & 0x1ffu);
    /* Linux's ilk_wm_get_hw_state: the low-power levels off until the
     * first commit computes them. */
    lp_off(i915, WM3_LP_ILK);
    lp_off(i915, WM2_LP_ILK);
    lp_off(i915, WM1_LP_ILK);
    k_memset(hw, 0, sizeof(*hw));
    for (pipe = 0; pipe < I915_NUM_PIPES; pipe++) {
        struct intel_crtc *c = &d->crtc[pipe];
        struct intel_pipe_wm *a = &c->wm_active;
        uint32_t v = i915_read(&i915->hw, WM0_PIPE_ILK(pipe));

        hw->wm_pipe[pipe] = v;
        k_memset(a, 0, sizeof(*a));
        a->pipe_enabled = c->hw.active;
        if (a->pipe_enabled) {
            /* A pipe on keeps the firmware's level 0; its low-power levels
             * are off, since they cannot be read back per pipe. */
            a->wm[0].enable = 1;
            a->wm[0].pri = v >> WM0_PIPE_PRIMARY_SHIFT;
            a->wm[0].spr = (v >> WM0_PIPE_SPRITE_SHIFT) & 0xffu;
            a->wm[0].cur = v & WM0_PIPE_CURSOR_MASK;
        } else {
            for (level = 0; level < INTEL_WM_LEVELS; level++) {
                a->wm[level].enable = 1;
            }
        }
        c->wm_optimal = *a;
    }
    hw->wm_lp[0] = i915_read(&i915->hw, WM1_LP_ILK);
    hw->wm_lp[1] = i915_read(&i915->hw, WM2_LP_ILK);
    hw->wm_lp[2] = i915_read(&i915->hw, WM3_LP_ILK);
    hw->wm_lp_spr[0] = i915_read(&i915->hw, WM1S_LP_ILK);
    hw->wm_lp_spr[1] = i915_read(&i915->hw, WM2S_LP_IVB);
    hw->wm_lp_spr[2] = i915_read(&i915->hw, WM3S_LP_IVB);
    hw->partition_5_6 = (i915_read(&i915->hw, WM_MISC) & WM_MISC_DATA_PARTITION_5_6) != 0;
    hw->enable_fbc_wm = !(i915_read(&i915->hw, DISP_ARB_CTL) & DISP_FBC_WM_DIS);
    klog(KLOG_INFO, "i915", "wm: latencies %u, %u, %u, %u and %u (level 0 in 0.1 us, then 0.5 us)",
         (unsigned)d->wm_latency[0], (unsigned)d->wm_latency[1], (unsigned)d->wm_latency[2],
         (unsigned)d->wm_latency[3], (unsigned)d->wm_latency[4]);
}
