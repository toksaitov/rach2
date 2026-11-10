/*
 * intel_cursor.c: the cursor plane of Haswell's pipes, after Linux's
 * display/intel_cursor.c: an ARGB image 64, 128 or 256 pixels wide and 8
 * lines to its width high, placed anywhere, partly off the screen too.
 */

#include "i915.h"
#include "i915_reg.h"

#include "../../lib/kdef.h"
#include "../../lib/kerrno.h"

static const uint32_t cursor_formats[] = {DRM_FORMAT_ARGB8888};

static const uint64_t cursor_modifiers[] = {DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_MOD_INVALID};

/* The sizes the cursor takes, which SIZE_HINTS gives. */
static const struct drm_plane_size_hint cursor_sizes[] = {{64, 64}, {128, 128}, {256, 256}};

#define cursor_crtc(p) ((struct intel_crtc *)((uintptr_t)(p) - offsetof(struct intel_crtc, cursor)))

/** CURCNTR's mode for a cursor `width` pixels wide, or 0 for a width it
 * does not take. */
static uint32_t cursor_mode(uint32_t width)
{
    switch (width) {
    case 64:
        return MCURSOR_MODE_64_ARGB_AX;
    case 128:
        return MCURSOR_MODE_128_ARGB_AX;
    case 256:
        return MCURSOR_MODE_256_ARGB_AX;
    default:
        return 0;
    }
}

/**
 * The cursor's size and layout, Linux's i9xx_cursor_size_ok and the rest
 * of i9xx_check_cursor: 64, 128 or 256 pixels wide; 8 lines to the width
 * high, which CUR_FBC_CTL gives Haswell, or square when turned; the source
 * the size of the cursor at the framebuffer's corner; linear, with a stride
 * of the cursor's width and no offset. 0 or -EINVAL.
 */
static int32_t cursor_size_check(const struct drm_plane_state *ps)
{
    const struct drm_framebuffer *fb = ps->fb;
    uint32_t w = ps->crtc_w, h = ps->crtc_h;

    if (!cursor_mode(w) || h > w || h < 8 || ((ps->rotation & DRM_MODE_ROTATE_0) == 0 && h != w)) {
        return -EINVAL;
    }
    if (ps->src_x != 0 || ps->src_y != 0 || ps->src_w != w << 16 || ps->src_h != h << 16 ||
        fb->pitches[0] != w * 4u || fb->offsets[0] != 0 ||
        intel_fb_modifier(fb) != DRM_FORMAT_MOD_LINEAR) {
        return -EINVAL;
    }
    return 0;
}

/**
 * The cursor's check, Linux's i9xx_check_cursor: clipped to the CRTC
 * without scaling, anywhere on it, and of a size and layout
 * cursor_size_check takes. 0, -EINVAL or -ERANGE.
 */
static int32_t cursor_check(struct drm_plane *plane, struct drm_atomic_state *state)
{
    struct drm_plane_state *ps = state->planes_new[plane->index];
    const struct drm_crtc_state *cs;
    int32_t err;

    if (!ps) {
        return 0;
    }
    cs = NULL;
    if (ps->crtc) {
        cs =
            state->crtcs_new[ps->crtc->index] ? state->crtcs_new[ps->crtc->index] : ps->crtc->state;
    }
    if (cs && cs->async_flip) {
        return -EINVAL;
    }
    err = drm_atomic_helper_check_plane_state(ps, cs, DRM_PLANE_NO_SCALING, DRM_PLANE_NO_SCALING, 1,
                                              1);
    if (err != 0) {
        return err;
    }
    if (cs && !cs->active) {
        ps->visible = 0;
    }
    return ps->fb ? cursor_size_check(ps) : 0;
}

/** CURPOS's coordinate of `v`: a sign bit and the magnitude, 15 bits of
 * each half. */
static uint32_t cursor_coord(int32_t v, uint32_t sign)
{
    return v < 0 ? sign | ((uint32_t)-v & CURSOR_POS_MASK) : (uint32_t)v & CURSOR_POS_MASK;
}

/**
 * Show the cursor's state, Linux's i9xx_cursor_update_arm: the height of
 * a cursor lower than it is wide, the control, the place, then the base,
 * whose write arms the rest for the next vblank.
 * Haswell turns a cursor by 180 degrees from its base, without the moved
 * base older chips need.
 */
static void cursor_update(struct drm_plane *plane, struct drm_atomic_state *state)
{
    struct intel_crtc *c = cursor_crtc(plane);
    struct i915_hw *hw = &c->i915->hw;
    const struct drm_plane_state *ps = plane->state;
    uint32_t cntr;

    (void)state;
    if (!ps->visible || !ps->fb || !c->base.state->active) {
        i915_write(hw, CUR_FBC_CTL(c->pipe), 0);
        i915_write(hw, CURCNTR(c->pipe), 0);
        i915_write(hw, CURBASE(c->pipe), 0);
        c->planes_on &= ~(1u << INTEL_PLANE_CURSOR);
        return;
    }
    i915_write(hw, CUR_FBC_CTL(c->pipe),
               ps->crtc_h != ps->crtc_w ? CUR_FBC_EN | ((ps->crtc_h - 1) & CUR_FBC_HEIGHT_MASK)
                                        : 0);
    cntr = cursor_mode(ps->crtc_w);
    if (ps->rotation & DRM_MODE_ROTATE_180) {
        cntr |= MCURSOR_ROTATE_180;
    }
    if (c->cfg.limited_range) {
        cntr |= MCURSOR_PIPE_CSC_ENABLE;
    }
    i915_write(hw, CURCNTR(c->pipe), cntr);
    i915_write(hw, CURPOS(c->pipe),
               cursor_coord(ps->crtc_y, CURSOR_POS_X_SIGN) << 16 |
                   cursor_coord(ps->crtc_x, CURSOR_POS_X_SIGN));
    i915_write(hw, CURBASE(c->pipe), intel_fb_ggtt(ps->fb));
    c->planes_on |= 1u << INTEL_PLANE_CURSOR;
}

static const struct drm_plane_funcs cursor_funcs = {cursor_check, cursor_update, NULL};

int32_t intel_cursor_init(struct intel_crtc *crtc)
{
    struct drm_device *dev = &crtc->i915->drm;
    struct drm_property_blob *hints;
    int32_t err;

    if ((err = drm_universal_plane_init(dev, &crtc->cursor, 1u << crtc->pipe, &cursor_funcs,
                                        cursor_formats, 1, cursor_modifiers,
                                        DRM_PLANE_TYPE_CURSOR)) != 0 ||
        (err = drm_plane_create_rotation_property(&crtc->cursor, DRM_MODE_ROTATE_0,
                                                  DRM_MODE_ROTATE_0 | DRM_MODE_ROTATE_180)) != 0 ||
        (err = drm_plane_create_zpos_property(&crtc->cursor, 2, 2, 2)) != 0) {
        return err;
    }
    hints = drm_property_create_blob(dev, sizeof(cursor_sizes), cursor_sizes, &err);
    if (!hints) {
        return err;
    }
    /* The device's blob list keeps the hints for the plane's life. */
    drm_object_attach_property(&crtc->cursor.base, dev->mode_config.size_hints_property,
                               hints->base.id);
    return 0;
}
