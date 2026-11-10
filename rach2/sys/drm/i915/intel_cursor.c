/*
 * intel_cursor.c: the cursor plane of Haswell's pipes, after Linux's
 * display/intel_cursor.c: a square ARGB image of 64, 128 or 256 pixels,
 * placed anywhere, partly off the screen too.
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

/** CURCNTR's mode for a cursor `width` pixels square, or 0 for a size it
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
 * The cursor's check, Linux's i9xx_check_cursor: clipped to the CRTC
 * without scaling, anywhere on it; a visible cursor shows its whole
 * framebuffer, square and of a size the plane takes, linear with a stride
 * of its width and no offset. 0, -EINVAL or -ERANGE.
 */
static int32_t cursor_check(struct drm_plane *plane, struct drm_atomic_state *state)
{
    struct drm_plane_state *ps = state->planes_new[plane->index];
    const struct drm_crtc_state *cs;
    const struct drm_framebuffer *fb;
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
    fb = ps->fb;
    if (!fb) {
        return 0;
    }
    if (fb->width != fb->height || !cursor_mode(fb->width) || ps->src_x != 0 || ps->src_y != 0 ||
        ps->src_w != fb->width << 16 || ps->src_h != fb->height << 16 || ps->crtc_w != fb->width ||
        ps->crtc_h != fb->height || fb->pitches[0] != fb->width * 4u || fb->offsets[0] != 0 ||
        intel_fb_modifier(fb) != DRM_FORMAT_MOD_LINEAR) {
        return -EINVAL;
    }
    return 0;
}

/** CURPOS's coordinate of `v`: a sign bit and the magnitude, 15 bits of
 * each half. */
static uint32_t cursor_coord(int32_t v, uint32_t sign)
{
    return v < 0 ? sign | ((uint32_t)-v & CURSOR_POS_MASK) : (uint32_t)v & CURSOR_POS_MASK;
}

/**
 * Show the cursor's state, Linux's i9xx_cursor_update_arm: the control,
 * the place, then the base, whose write arms both for the next vblank.
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
        i915_write(hw, CURCNTR(c->pipe), 0);
        i915_write(hw, CURBASE(c->pipe), 0);
        c->planes_on &= ~(1u << INTEL_PLANE_CURSOR);
        return;
    }
    cntr = cursor_mode(ps->fb->width);
    if (ps->rotation & DRM_MODE_ROTATE_180) {
        cntr |= MCURSOR_ROTATE_180;
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
