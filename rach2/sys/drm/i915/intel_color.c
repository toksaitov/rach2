/*
 * intel_color.c: a Haswell pipe's colour management, after Linux 6.12's
 * display/intel_color.c for Haswell (hsw_color_funcs): the DEGAMMA_LUT, CTM
 * and GAMMA_LUT properties, their checks, and the gamma tables, the colour
 * matrix and the modes they set.
 */

#include "i915.h"
#include "i915_reg.h"

#include "../../lib/kerrno.h"
#include "../../lib/kstring.h"

/* The tables' sizes, Linux's IVB_COLORS, and the legacy table's, which
 * SETGAMMA fills. */
#define COLOR_LUT_SIZE        1024u
#define COLOR_LUT_SPLIT_SIZE  512u
#define COLOR_LEGACY_LUT_SIZE 256u

/* The CTM's S31.32 sign-magnitude coefficients (Linux's CTM_COEFF_). */
#define CTM_COEFF_SIGN          ((uint64_t)1 << 63)
#define CTM_COEFF_1_0           ((uint64_t)1 << 32)
#define CTM_COEFF_2_0           (CTM_COEFF_1_0 << 1)
#define CTM_COEFF_4_0           (CTM_COEFF_2_0 << 1)
#define CTM_COEFF_0_5           (CTM_COEFF_1_0 >> 1)
#define CTM_COEFF_0_25          (CTM_COEFF_0_5 >> 1)
#define CTM_COEFF_0_125         (CTM_COEFF_0_25 >> 1)
/* 219/255 in 0.32, Linux's CTM_COEFF_LIMITED_RANGE. */
#define CTM_COEFF_LIMITED_RANGE 3688618971u

/* The matrix's 1.0 and limited range gain, exponent 0 and a 12-bit
 * fraction, and the limited range's offset of 16/256 of full scale. */
#define ILK_CSC_COEFF_1_0             0x7800u
#define ILK_CSC_COEFF_LIMITED_RANGE   ((235u - 16u) << 4)
#define ILK_CSC_POSTOFF_LIMITED_RANGE (16u << 4)

/* A colour matrix as the pipe takes it: offsets and coefficients, each in
 * the low 16 bits of its register's half. */
struct color_csc {
    uint16_t preoff[3];
    uint16_t coeff[9];
    uint16_t postoff[3];
};

/*
 * What a CRTC state sets the pipe's colour to, Linux's ivb_color_check:
 * the modes (`hw`), and the tables before and after the matrix, `pre` and
 * `post`, each loaded at `pre_size` or `post_size` entries, `post` mapped
 * into 16-235 when `post_limited`. `csc` is the matrix when
 * `hw.csc_enable`.
 */
struct color_plan {
    struct intel_color hw;
    const struct drm_property_blob *pre, *post;
    uint32_t pre_size, post_size;
    int post_limited;
    struct color_csc csc;
};

/** The entries of a table's blob, Linux's drm_color_lut_size. */
static uint32_t color_lut_size(const struct drm_property_blob *lut)
{
    return lut ? lut->length / (uint32_t)sizeof(struct drm_color_lut) : 0;
}

/** Whether `lut` is a table of the legacy size, Linux's lut_is_legacy. */
static int color_lut_is_legacy(const struct drm_property_blob *lut)
{
    return lut && color_lut_size(lut) == COLOR_LEGACY_LUT_SIZE;
}

/** A table's 16-bit value rounded to `bits`, Linux's
 * drm_color_lut_extract. */
static uint32_t color_lut_extract(uint32_t value, int bits)
{
    uint32_t max = 0xffffu >> (16 - bits);

    if (bits < 16) {
        value += 1u << (16 - bits - 1);
        value >>= 16 - bits;
    }
    return value > max ? max : value;
}

/** A value mapped into 16-235, Linux's lut_limited_range. */
static uint16_t color_lut_limited(uint32_t value)
{
    const uint32_t min = 16u << 8, max = 235u << 8;

    return (uint16_t)(value * (max - min) / 0xffffu + min);
}

/**
 * Entry `i` of `lut` loaded at `size` entries, Linux's create_resized_lut:
 * the entry it falls on when the sizes differ, mapped into 16-235 when
 * `limited`.
 */
static struct drm_color_lut color_lut_entry(const struct drm_property_blob *lut, uint32_t i,
                                            uint32_t size, int limited)
{
    const struct drm_color_lut *in = (const struct drm_color_lut *)lut->data;
    uint32_t n = color_lut_size(lut);
    struct drm_color_lut e = in[size == n ? i : i * (n - 1u) / (size - 1u)];

    if (limited) {
        e.red = color_lut_limited(e.red);
        e.green = color_lut_limited(e.green);
        e.blue = color_lut_limited(e.blue);
    }
    return e;
}

/** Whether a plane of `c` shows a C8 framebuffer, in `state`'s new
 * states when `fresh` and its old ones otherwise, or as the planes are
 * now when `state` is NULL; Linux's c8_planes. */
static int color_planes_c8(struct intel_crtc *c, struct drm_atomic_state *state, int fresh)
{
    struct drm_plane *planes[3];
    int i;

    planes[0] = &c->primary;
    planes[1] = &c->sprite;
    planes[2] = &c->cursor;
    for (i = 0; i < 3; i++) {
        const struct drm_plane_state *ps = planes[i]->state;

        if (state && state->planes_new[planes[i]->index]) {
            ps = fresh ? state->planes_new[planes[i]->index] : state->planes_old[planes[i]->index];
        }
        if (ps && ps->crtc == &c->base && ps->visible && ps->fb &&
            ps->fb->format == DRM_FORMAT_C8) {
            return 1;
        }
    }
    return 0;
}

/**
 * Convert the user's CTM to the pipe's matrix, Linux's ilk_csc_convert_ctm:
 * each coefficient's magnitude clamped below 4.0, with a sign bit, a
 * 3-bit exponent and a 9-bit mantissa from bit 3; and with `limited`, the
 * matrix scaled to 16-235 and the limited range's offsets, Linux's
 * ctm_mult_by_limited.
 */
static void color_convert_ctm(const struct drm_color_ctm *ctm, int limited, struct color_csc *csc)
{
    int i;

    k_memset(csc, 0, sizeof(*csc));
    if (limited) {
        csc->postoff[0] = csc->postoff[1] = csc->postoff[2] = ILK_CSC_POSTOFF_LIMITED_RANGE;
    }
    for (i = 0; i < 9; i++) {
        uint64_t in = ctm->matrix[i], v = in & ~CTM_COEFF_SIGN;
        uint32_t frac, exp;
        uint64_t fp;

        if (v > CTM_COEFF_4_0 - 1u) {
            v = CTM_COEFF_4_0 - 1u;
        }
        if (limited) {
            /* The magnitude below 4.0 in 2.30, times 219/255. */
            v = (uint64_t)CTM_COEFF_LIMITED_RANGE * (uint32_t)(v >> 2) >> 30;
        }
        if (v < CTM_COEFF_0_125) {
            exp = 3;
            frac = 12;
        } else if (v < CTM_COEFF_0_25) {
            exp = 2;
            frac = 11;
        } else if (v < CTM_COEFF_0_5) {
            exp = 1;
            frac = 10;
        } else if (v < CTM_COEFF_1_0) {
            exp = 0;
            frac = 9;
        } else if (v < CTM_COEFF_2_0) {
            exp = 7;
            frac = 8;
        } else {
            exp = 6;
            frac = 7;
        }
        /* Linux's ILK_CSC_COEFF_FP: rounded, clamped and from bit 3. */
        fp = (v >> (32u - frac - 3u)) + 4u;
        csc->coeff[i] = (uint16_t)(exp << 12 | ((fp > 0xfffu ? 0xfffu : (uint32_t)fp) & 0xff8u) |
                                   ((in & CTM_COEFF_SIGN) ? 1u << 15 : 0u));
    }
}

/**
 * Plan the pipe's colour for the CRTC state `cs` on a pipe of
 * `limited_range` RGB with C8 planes when `c8`, Linux's ivb_color_check
 * past its checks, with ivb_assign_luts and ilk_assign_csc.
 */
static void color_plan_make(const struct drm_crtc_state *cs, int limited_range, int c8,
                            struct color_plan *p)
{
    const struct drm_property_blob *degamma = cs->degamma_lut, *gamma = cs->gamma_lut;
    /* Linux's ilk_lut_limited_range: after a degamma table or a CTM the
     * gamma table maps into 16-235, else the matrix does. */
    int lut_limited = limited_range && !c8 && gamma && (degamma || cs->ctm);
    int csc_limited = limited_range && !lut_limited;

    k_memset(p, 0, sizeof(*p));
    p->hw.gamma_enable = (gamma || degamma) && !c8;
    p->hw.csc_enable = csc_limited || cs->ctm;
    if (degamma && gamma) {
        p->hw.gamma_mode = GAMMA_MODE_MODE_SPLIT;
    } else if (!p->hw.gamma_enable || color_lut_is_legacy(gamma)) {
        p->hw.gamma_mode = GAMMA_MODE_MODE_8BIT;
    } else {
        p->hw.gamma_mode = GAMMA_MODE_MODE_10BIT;
    }
    /* Linux's ivb_csc_mode: the matrix after the table in degamma and
     * full to limited range, before it otherwise. */
    p->hw.csc_mode = degamma || csc_limited ? 0 : CSC_POSITION_BEFORE_GAMMA;
    if (p->hw.gamma_mode == GAMMA_MODE_MODE_SPLIT) {
        p->pre = degamma;
        p->post = gamma;
        p->pre_size = p->post_size = COLOR_LUT_SPLIT_SIZE;
        p->post_limited = lut_limited;
    } else if (lut_limited) {
        p->post = gamma;
        p->post_limited = 1;
    } else if (degamma || (p->hw.csc_mode & CSC_POSITION_BEFORE_GAMMA)) {
        p->pre = degamma;
        p->post = gamma;
    } else {
        p->pre = gamma;
    }
    if (p->hw.gamma_mode != GAMMA_MODE_MODE_SPLIT) {
        p->pre_size = color_lut_size(p->pre);
        p->post_size = color_lut_size(p->post);
    }
    if (cs->ctm) {
        color_convert_ctm((const struct drm_color_ctm *)cs->ctm->data, csc_limited, &p->csc);
    } else if (csc_limited) {
        p->csc.coeff[0] = p->csc.coeff[4] = p->csc.coeff[8] = ILK_CSC_COEFF_LIMITED_RANGE;
        p->csc.postoff[0] = p->csc.postoff[1] = p->csc.postoff[2] = ILK_CSC_POSTOFF_LIMITED_RANGE;
    }
}

int32_t intel_color_crtc_init(struct intel_crtc *c)
{
    int32_t err = drm_mode_crtc_set_gamma_size(&c->base, COLOR_LEGACY_LUT_SIZE);

    if (err == 0) {
        drm_crtc_enable_color_mgmt(&c->base, COLOR_LUT_SIZE, 1, COLOR_LUT_SIZE);
    }
    return err;
}

int32_t intel_color_check(struct intel_crtc *c, struct drm_atomic_state *state,
                          const struct intel_crtc_config *cfg)
{
    struct drm_crtc_state *cs = state->crtcs_new[c->base.index];
    const struct drm_crtc_state *old = state->crtcs_old[c->base.index];
    struct color_plan was, now;
    int c8 = color_planes_c8(c, state, 1);
    uint32_t gamma = color_lut_size(cs->gamma_lut);
    struct drm_plane *planes[3];
    int i;

    /* Linux's intel_color_check: a C8 plane coming or going changes the
     * pipe's gamma enable. */
    if (c8 != color_planes_c8(c, state, 0)) {
        cs->color_mgmt_changed = 1;
    }
    if (!cs->color_mgmt_changed && !drm_atomic_crtc_needs_modeset(cs)) {
        return 0;
    }
    /* Linux's check_luts and ivb_color_check. */
    if ((cs->degamma_lut && color_lut_size(cs->degamma_lut) != COLOR_LUT_SIZE) ||
        (cs->gamma_lut && gamma != COLOR_LUT_SIZE && gamma != COLOR_LEGACY_LUT_SIZE)) {
        return -EINVAL;
    }
    if (c8 && (!color_lut_is_legacy(cs->gamma_lut) || cs->degamma_lut)) {
        return -EINVAL;
    }
    if (!cs->active || drm_atomic_crtc_needs_modeset(cs)) {
        return 0;
    }
    /* Linux's intel_color_add_affected_planes: the planes' control holds
     * the pipe's gamma and matrix enables, the primary's even while it is
     * off, for the pipe's background. */
    color_plan_make(cs, cfg->limited_range, c8, &now);
    color_plan_make(old, c->cfg.limited_range, color_planes_c8(c, state, 0), &was);
    if (now.hw.gamma_enable == was.hw.gamma_enable && now.hw.csc_enable == was.hw.csc_enable) {
        return 0;
    }
    planes[0] = &c->primary;
    planes[1] = &c->sprite;
    planes[2] = &c->cursor;
    for (i = 0; i < 3; i++) {
        const struct drm_plane_state *ps = state->planes_new[planes[i]->index];

        if (!ps) {
            ps = planes[i]->state;
        }
        if ((i == 0 || (ps && ps->crtc == &c->base && ps->visible)) &&
            !drm_atomic_get_plane_state(state, planes[i])) {
            return -ENOMEM;
        }
    }
    return 0;
}

/** Load `n` entries of `lut` into pipe `pipe`'s precision table from
 * `index`, Linux's ivb_load_lut_10, an entry's index written before it
 * since Haswell does not advance the index; then the index back to 0,
 * which the legacy table's writes need. */
static void color_load_lut_10(struct i915_hw *hw, int pipe, const struct drm_property_blob *lut,
                              uint32_t n, int limited, uint32_t index)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        struct drm_color_lut e = color_lut_entry(lut, i, n, limited);

        i915_write(hw, PREC_PAL_INDEX(pipe), index + i);
        i915_write(hw, PREC_PAL_DATA(pipe),
                   color_lut_extract(e.red, 10) << 20 | color_lut_extract(e.green, 10) << 10 |
                       color_lut_extract(e.blue, 10));
    }
    i915_write(hw, PREC_PAL_INDEX(pipe), PAL_PREC_INDEX_VALUE(0));
}

/** Clamp each channel's output above 1.0 to 1.0, Linux's
 * ivb_load_lut_ext_max. */
static void color_load_ext_max(struct i915_hw *hw, int pipe)
{
    i915_write(hw, PREC_PAL_EXT_GC_MAX(pipe, 0), 1u << 16);
    i915_write(hw, PREC_PAL_EXT_GC_MAX(pipe, 1), 1u << 16);
    i915_write(hw, PREC_PAL_EXT_GC_MAX(pipe, 2), 1u << 16);
}

/** Load the plan's tables, Linux's ivb_load_luts. */
static void color_load_luts(struct i915_hw *hw, int pipe, const struct color_plan *p)
{
    const struct drm_property_blob *lut = p->post ? p->post : p->pre;
    uint32_t n = p->post ? p->post_size : p->pre_size;
    int limited = p->post ? p->post_limited : 0;
    uint32_t i;

    switch (p->hw.gamma_mode) {
    case GAMMA_MODE_MODE_8BIT:
        for (i = 0; lut && i < COLOR_LEGACY_LUT_SIZE; i++) {
            struct drm_color_lut e =
                color_lut_entry(lut, i * (n - 1u) / (COLOR_LEGACY_LUT_SIZE - 1u), n, limited);

            i915_write(hw, LGC_PALETTE(pipe, i),
                       color_lut_extract(e.red, 8) << 16 | color_lut_extract(e.green, 8) << 8 |
                           color_lut_extract(e.blue, 8));
        }
        break;
    case GAMMA_MODE_MODE_SPLIT:
        color_load_lut_10(hw, pipe, p->pre, p->pre_size, 0,
                          PAL_PREC_SPLIT_MODE | PAL_PREC_INDEX_VALUE(0));
        color_load_ext_max(hw, pipe);
        color_load_lut_10(hw, pipe, p->post, p->post_size, p->post_limited,
                          PAL_PREC_SPLIT_MODE | PAL_PREC_INDEX_VALUE(COLOR_LUT_SPLIT_SIZE));
        break;
    default:
        color_load_lut_10(hw, pipe, lut, n, limited, PAL_PREC_INDEX_VALUE(0));
        color_load_ext_max(hw, pipe);
        break;
    }
}

/** Write the pipe's matrix, Linux's ilk_update_pipe_csc. */
static void color_write_csc(struct i915_hw *hw, int pipe, const struct color_csc *csc)
{
    i915_write(hw, PIPE_CSC_PREOFF_HI(pipe), csc->preoff[0]);
    i915_write(hw, PIPE_CSC_PREOFF_ME(pipe), csc->preoff[1]);
    i915_write(hw, PIPE_CSC_PREOFF_LO(pipe), csc->preoff[2]);
    i915_write(hw, PIPE_CSC_COEFF_RY_GY(pipe), (uint32_t)csc->coeff[0] << 16 | csc->coeff[1]);
    i915_write(hw, PIPE_CSC_COEFF_BY(pipe), (uint32_t)csc->coeff[2] << 16);
    i915_write(hw, PIPE_CSC_COEFF_RU_GU(pipe), (uint32_t)csc->coeff[3] << 16 | csc->coeff[4]);
    i915_write(hw, PIPE_CSC_COEFF_BU(pipe), (uint32_t)csc->coeff[5] << 16);
    i915_write(hw, PIPE_CSC_COEFF_RV_GV(pipe), (uint32_t)csc->coeff[6] << 16 | csc->coeff[7]);
    i915_write(hw, PIPE_CSC_COEFF_BV(pipe), (uint32_t)csc->coeff[8] << 16);
    i915_write(hw, PIPE_CSC_POSTOFF_HI(pipe), csc->postoff[0]);
    i915_write(hw, PIPE_CSC_POSTOFF_ME(pipe), csc->postoff[1]);
    i915_write(hw, PIPE_CSC_POSTOFF_LO(pipe), csc->postoff[2]);
}

void intel_color_commit(struct intel_crtc *c)
{
    struct i915_hw *hw = &c->i915->hw;
    struct color_plan p;

    color_plan_make(c->base.state, c->cfg.limited_range, color_planes_c8(c, NULL, 1), &p);
    color_load_luts(hw, c->pipe, &p);
    if (p.hw.csc_enable) {
        color_write_csc(hw, c->pipe, &p.csc);
    }
    i915_write(hw, GAMMA_MODE(c->pipe), p.hw.gamma_mode);
    i915_write(hw, PIPE_CSC_MODE(c->pipe), p.hw.csc_mode);
    c->color = p.hw;
}
