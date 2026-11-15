/*
 * intel_plane.c: the primary and sprite planes of Haswell's pipes, after
 * Linux's display/i9xx_plane.c and display/intel_sprite.c: their formats
 * and modifiers, the checks of a framebuffer they scan out, and the
 * registers that show it.
 */

#include "i915.h"
#include "i915_drm.h"
#include "i915_reg.h"

#include "../../lib/kdef.h"
#include "../../lib/kerrno.h"
#include "../../lib/kstring.h"

/* The primary plane's formats on Haswell, Linux's ivb_primary_formats;
 * ARGB is not among them, since the primary plane has no alpha. */
static const uint32_t primary_formats[] = {
    DRM_FORMAT_C8,          DRM_FORMAT_RGB565,      DRM_FORMAT_XRGB8888,     DRM_FORMAT_XBGR8888,
    DRM_FORMAT_XRGB2101010, DRM_FORMAT_XBGR2101010, DRM_FORMAT_XBGR16161616F};

/* The sprite's, Linux's hsw_sprite_formats. */
static const uint32_t sprite_formats[] = {DRM_FORMAT_XRGB8888,      DRM_FORMAT_XBGR8888,
                                          DRM_FORMAT_XRGB2101010,   DRM_FORMAT_XBGR2101010,
                                          DRM_FORMAT_XRGB16161616F, DRM_FORMAT_XBGR16161616F,
                                          DRM_FORMAT_YUYV,          DRM_FORMAT_YVYU,
                                          DRM_FORMAT_UYVY,          DRM_FORMAT_VYUY};

/* Both planes take X tiles and linear surfaces, as Linux's
 * i9xx_format_modifiers. */
static const uint64_t plane_modifiers[] = {I915_FORMAT_MOD_X_TILED, DRM_FORMAT_MOD_LINEAR,
                                           DRM_FORMAT_MOD_INVALID};

/* An X tile: 512 bytes by 8 rows. */
#define X_TILE_WIDTH  512u
#define X_TILE_HEIGHT 8u
#define X_TILE_SIZE   4096u

/* The largest offset either plane's offset register takes. */
#define PLANE_MAX_XY 4095u

/*
 * How a plane reads its framebuffer, Linux's intel_fb_view of the first
 * colour plane: through the object's normal view when `remap.width` is 0,
 * or through the remapped view `remap`. `stride` is the bytes from one row
 * to the next as the plane reads them, Linux's mapping_stride; with a
 * remapped view, `x` and `y` are the source's corner from the view's first
 * page.
 */
struct plane_view {
    struct i915_remap_info remap;
    uint32_t stride;
    uint32_t x, y;
};

#define primary_crtc(p) \
    ((struct intel_crtc *)((uintptr_t)(p) - offsetof(struct intel_crtc, primary)))
#define sprite_crtc(p) ((struct intel_crtc *)((uintptr_t)(p) - offsetof(struct intel_crtc, sprite)))

/** The state of `crtc` that `state` holds, or its current one. */
static const struct drm_crtc_state *crtc_state_of(struct drm_atomic_state *state,
                                                  struct drm_crtc *crtc)
{
    return state->crtcs_new[crtc->index] ? state->crtcs_new[crtc->index] : crtc->state;
}

/** Whether `format` is a packed YUV 4:2:2 one. */
static int is_yuv422(uint32_t format)
{
    return format == DRM_FORMAT_YUYV || format == DRM_FORMAT_YVYU || format == DRM_FORMAT_UYVY ||
           format == DRM_FORMAT_VYUY;
}

/**
 * The widest stride the primary plane, or the sprite, reads of a format of
 * `cpp` bytes a pixel, Linux's hsw_primary_max_stride and
 * hsw_sprite_max_stride: 8192 pixels, which keeps the offset register's x
 * in range, and at most 32 KiB for the primary plane and 16 KiB for the
 * sprite.
 */
static uint32_t plane_max_stride(int primary, uint32_t cpp)
{
    uint32_t cap = primary ? 32u * 1024u : 16u * 1024u;

    return 8192u * cpp < cap ? 8192u * cpp : cap;
}

/**
 * The view through which the primary plane, or the sprite, reads the
 * framebuffer of `ps`, into `*v`, as Linux's intel_plane_compute_gtt
 * makes it. A visible plane whose stride passes its limit reads the
 * remapped view of its source rectangle (intel_plane_remap_gtt), which
 * the plane's check has clipped: X tiles of 512 bytes by 8 rows, or of a
 * linear surface whose stride is whole pages, rows of a page; the tile
 * holding the source's corner is the view's first, and the view is as
 * many tiles wide and high as reach the rectangle's far corner. Any other
 * plane reads the normal view at the framebuffer's stride.
 */
static void plane_view(const struct drm_plane_state *ps, int primary, struct plane_view *v)
{
    const struct drm_framebuffer *fb = ps->fb;
    uint32_t pitch = fb->pitches[0], cpp = fb->info->cpp[0];
    uint32_t sx = (uint32_t)ps->src.x1 >> 16, sy = (uint32_t)ps->src.y1 >> 16;
    uint32_t w = (uint32_t)(ps->src.x2 - ps->src.x1) >> 16;
    uint32_t h = (uint32_t)(ps->src.y2 - ps->src.y1) >> 16;
    uint32_t tile_bytes, tile_w, tile_h;
    uint64_t at;
    int tiled = intel_fb_modifier(fb) == I915_FORMAT_MOD_X_TILED;

    k_memset(v, 0, sizeof(*v));
    v->stride = pitch;
    if (!ps->visible || !fb->obj[0] || fb->obj[0]->funcs != &i915_gem_object_funcs ||
        pitch <= plane_max_stride(primary, cpp) || (!tiled && pitch % X_TILE_SIZE != 0)) {
        return;
    }
    tile_bytes = tiled ? X_TILE_WIDTH : X_TILE_SIZE;
    tile_w = tile_bytes / cpp;
    tile_h = tiled ? X_TILE_HEIGHT : 1u;
    if (tiled) {
        /* The framebuffer's offset is whole tiles (check_surface). */
        v->remap.offset =
            fb->offsets[0] / X_TILE_SIZE + (sy / tile_h) * (pitch / tile_bytes) + sx / tile_w;
        v->x = sx % tile_w;
        v->y = sy % tile_h;
    } else {
        /* A stride of whole pages puts the corner in the first row. */
        at = (uint64_t)fb->offsets[0] + (uint64_t)sy * pitch + (uint64_t)sx * cpp;
        v->remap.offset = (uint32_t)(at >> PAGE_SHIFT);
        v->x = (uint32_t)(at & (X_TILE_SIZE - 1u)) / cpp;
        v->y = 0;
    }
    v->remap.width = (v->x + w + tile_w - 1u) / tile_w;
    v->remap.height = (v->y + h + tile_h - 1u) / tile_h;
    v->remap.src_stride = pitch / tile_bytes;
    v->stride = v->remap.width * tile_bytes;
}

int intel_plane_remap(const struct drm_plane_state *ps, struct i915_remap_info *remap)
{
    struct plane_view v;

    remap->width = 0;
    if (!ps->fb || ps->plane->type == DRM_PLANE_TYPE_CURSOR) {
        return 0;
    }
    plane_view(ps, ps->plane->type == DRM_PLANE_TYPE_PRIMARY, &v);
    *remap = v.remap;
    return v.remap.width != 0;
}

/**
 * Check `ps`'s framebuffer as Linux's intel_fb checks and
 * i9xx_check_plane_surface do: its modifier and its object's tiling agree,
 * its stride is aligned to its modifier's unit, 64 bytes or a tile's
 * width, and its first plane's offset lies on a pixel or, X-tiled, on a
 * tile. The plane's stride and the offset register are the caller's to
 * check. 0 or -EINVAL.
 */
static int32_t check_surface(const struct drm_plane_state *ps)
{
    const struct drm_framebuffer *fb = ps->fb;
    uint64_t modifier = intel_fb_modifier(fb);
    uint32_t pitch = fb->pitches[0], cpp = fb->info->cpp[0];
    const struct drm_gem_object *base = fb->obj[0];

    if (base->funcs == &i915_gem_object_funcs) {
        const struct drm_i915_gem_object *obj = (const struct drm_i915_gem_object *)base;

        if (obj->tiling != I915_TILING_NONE &&
            (modifier != I915_FORMAT_MOD_X_TILED || obj->tiling != I915_TILING_X ||
             obj->stride != pitch)) {
            return -EINVAL;
        }
    }
    if (modifier == I915_FORMAT_MOD_X_TILED) {
        if (pitch % X_TILE_WIDTH != 0 || fb->offsets[0] % X_TILE_SIZE != 0) {
            return -EINVAL;
        }
    } else if (modifier != DRM_FORMAT_MOD_LINEAR || pitch % 64u != 0 || fb->offsets[0] % cpp != 0) {
        return -EINVAL;
    }
    return 0;
}

static int32_t fb_attach(struct drm_framebuffer *fb, struct drm_i915_gem_object *obj);

/* The object's tiling and `fb_refs` are read and changed under the GEM
 * lock, as SET_TILING changes them. */
int32_t intel_fb_create(struct drm_device *dev, struct drm_framebuffer *fb)
{
    struct drm_i915_gem_object *obj = (struct drm_i915_gem_object *)fb->obj[0];
    int32_t rc;
    int taken;

    if (!obj || obj->base.funcs != &i915_gem_object_funcs) {
        return 0;
    }
    taken = i915_gem_lock(to_i915(dev));
    rc = fb_attach(fb, obj);
    i915_gem_unlock(to_i915(dev), taken);
    return rc;
}

/** The checks of `intel_fb_create` of `fb` on `obj`, and its count on the
 * object, under the GEM lock: 0 or -EINVAL. */
static int32_t fb_attach(struct drm_framebuffer *fb, struct drm_i915_gem_object *obj)
{
    uint32_t rows;

    if (fb->flags & DRM_MODE_FB_MODIFIERS) {
        /* The planes take linear and X tiles alone, so the core let no
         * other modifier through. */
        if (obj->tiling != I915_TILING_NONE &&
            (obj->tiling != I915_TILING_X || fb->modifier != I915_FORMAT_MOD_X_TILED)) {
            return -EINVAL;
        }
    } else if (obj->tiling == I915_TILING_X) {
        fb->modifier = I915_FORMAT_MOD_X_TILED;
    } else if (obj->tiling == I915_TILING_Y) {
        return -EINVAL;
    }
    if (obj->tiling != I915_TILING_NONE && fb->pitches[0] != obj->stride) {
        return -EINVAL;
    }
    /* Linux's intel_fill_fb_info: X tiles take whole rows of tiles. */
    if (fb->modifier == I915_FORMAT_MOD_X_TILED) {
        rows = (fb->height + X_TILE_HEIGHT - 1u) / X_TILE_HEIGHT;
        if ((uint64_t)(fb->offsets[0] & ~(X_TILE_SIZE - 1u)) +
                (uint64_t)rows * X_TILE_HEIGHT * fb->pitches[0] >
            obj->base.size) {
            return -EINVAL;
        }
    }
    obj->fb_refs++;
    return 0;
}

void intel_fb_destroy(struct drm_framebuffer *fb)
{
    struct drm_i915_gem_object *obj = (struct drm_i915_gem_object *)fb->obj[0];

    int taken;

    if (obj && obj->base.funcs == &i915_gem_object_funcs) {
        taken = i915_gem_lock(to_i915(obj->base.dev));
        obj->fb_refs--;
        i915_gem_unlock(to_i915(obj->base.dev), taken);
    }
}

/**
 * The surface address and offset of `ps`'s source corner, as Linux's
 * intel_plane_compute_aligned_offset: the framebuffer's offset and the
 * corner, less what fits the offset register, go into the surface, aligned
 * to a page for a linear surface and to a tile for an X-tiled one; the
 * corner within that is in `*x` and `*y`. The surface is from the GGTT
 * address of the framebuffer's object.
 */
static uint32_t surface_offset(const struct drm_plane_state *ps, uint32_t *x, uint32_t *y)
{
    const struct drm_framebuffer *fb = ps->fb;
    uint32_t pitch = fb->pitches[0], cpp = fb->info->cpp[0];
    uint32_t sx = (uint32_t)ps->src.x1 >> 16, sy = (uint32_t)ps->src.y1 >> 16;
    uint32_t off, aligned, tile_w;

    if (intel_fb_modifier(fb) == I915_FORMAT_MOD_X_TILED) {
        tile_w = X_TILE_WIDTH / cpp;
        aligned = fb->offsets[0] +
                  ((sy / X_TILE_HEIGHT) * (pitch / X_TILE_WIDTH) + sx / tile_w) * X_TILE_SIZE;
        *x = sx % tile_w;
        *y = sy % X_TILE_HEIGHT;
        return aligned;
    }
    off = fb->offsets[0] + sy * pitch + sx * cpp;
    aligned = off & ~(X_TILE_SIZE - 1u);
    *y = (off - aligned) / pitch;
    *x = ((off - aligned) % pitch) / cpp;
    return aligned;
}

/**
 * The GGTT address of the surface the primary plane, or the sprite, reads
 * for `ps`, with the stride in `*stride` and the source's corner from the
 * surface in `*x` and `*y`: from the remapped view the commit pinned when
 * the plane needs one (`plane_view`), whose first tile holds the corner,
 * or from the object's normal view (`surface_offset`).
 */
static uint32_t plane_surface(const struct drm_plane_state *ps, int primary, uint32_t *stride,
                              uint32_t *x, uint32_t *y)
{
    struct plane_view v;

    plane_view(ps, primary, &v);
    *stride = v.stride;
    if (v.remap.width) {
        *x = v.x;
        *y = v.y;
        return intel_fb_ggtt_remapped(ps->fb, &v.remap);
    }
    return intel_fb_ggtt(ps->fb) + surface_offset(ps, x, y);
}

/**
 * The checks every plane of a pipe makes of an async flip, Linux's
 * intel_async_flip_check_hw: only the primary plane takes one, of an
 * X-tiled framebuffer that keeps the old one's format, stride, modifier,
 * place and rotation. -EINVAL otherwise.
 */
static int32_t check_async(struct drm_atomic_state *state, struct drm_plane *plane, int primary)
{
    const struct drm_plane_state *old = state->planes_old[plane->index];
    const struct drm_plane_state *ps = state->planes_new[plane->index];

    if (!primary || !old->fb || !ps->fb || !old->visible || !ps->visible) {
        return -EINVAL;
    }
    if (intel_fb_modifier(ps->fb) != I915_FORMAT_MOD_X_TILED ||
        intel_fb_modifier(old->fb) != I915_FORMAT_MOD_X_TILED ||
        ps->fb->format != old->fb->format || ps->fb->pitches[0] != old->fb->pitches[0] ||
        ps->rotation != old->rotation || ps->src_x != old->src_x || ps->src_y != old->src_y ||
        ps->src_w != old->src_w || ps->src_h != old->src_h || ps->crtc_x != old->crtc_x ||
        ps->crtc_y != old->crtc_y || ps->crtc_w != old->crtc_w || ps->crtc_h != old->crtc_h) {
        return -EINVAL;
    }
    return 0;
}

/**
 * A primary or sprite plane's check: clipped to the CRTC without scaling,
 * the primary covering it whole as Haswell's primary plane must, then the
 * surface's checks, and the stride of the view the plane reads within the
 * plane's widest (`plane_view`, Linux's intel_plane_check_stride); C8
 * requires a 256-entry GAMMA_LUT, and a YUV source must start and end on
 * a pair of pixels. A plane is visible only on a CRTC that is on. 0,
 * -EINVAL or -ERANGE.
 */
static int32_t intel_plane_check(struct drm_plane *plane, struct drm_atomic_state *state,
                                 int primary)
{
    struct drm_plane_state *ps = state->planes_new[plane->index];
    const struct drm_crtc_state *cs;
    struct plane_view v;
    uint32_t x, y;
    int32_t err;

    if (!ps) {
        return 0;
    }
    cs = ps->crtc ? crtc_state_of(state, ps->crtc) : NULL;
    err = drm_atomic_helper_check_plane_state(ps, cs, DRM_PLANE_NO_SCALING, DRM_PLANE_NO_SCALING,
                                              !primary, 1);
    if (err != 0) {
        return err;
    }
    if (cs && !cs->active) {
        ps->visible = 0;
    }
    if (cs && cs->async_flip) {
        return check_async(state, plane, primary);
    }
    if (!ps->visible) {
        return 0;
    }
    if (ps->fb->format == DRM_FORMAT_C8 &&
        (!cs || !cs->gamma_lut || cs->gamma_lut->length != 256u * sizeof(struct drm_color_lut))) {
        return -EINVAL;
    }
    err = check_surface(ps);
    if (err != 0) {
        return err;
    }
    plane_view(ps, primary, &v);
    if (v.stride > plane_max_stride(primary, ps->fb->info->cpp[0])) {
        return -EINVAL;
    }
    if (is_yuv422(ps->fb->format) &&
        (((ps->src.x1 >> 16) & 1) != 0 || (((ps->src.x2 - ps->src.x1) >> 16) & 1) != 0)) {
        return -EINVAL;
    }
    if (v.remap.width) {
        x = v.x;
        y = v.y;
    } else {
        (void)surface_offset(ps, &x, &y);
    }
    return x > PLANE_MAX_XY || y > PLANE_MAX_XY ? -EINVAL : 0;
}

/** Flush the processor's cache of the rows of `ps`'s framebuffer its
 * damage names, which the console's shadow and DIRTYFB drew through the
 * cache, so that the display reads them from memory. */
static void flush_damage(const struct drm_plane_state *ps)
{
    const struct drm_framebuffer *fb = ps->fb;
    const struct drm_mode_rect *r;
    uint32_t i, n;
    int taken;

    if (!ps->fb_damage_clips || fb->obj[0]->funcs != &i915_gem_object_funcs) {
        return;
    }
    r = (const struct drm_mode_rect *)ps->fb_damage_clips->data;
    n = ps->fb_damage_clips->length / (uint32_t)sizeof(*r);
    /* The object's pages are the GEM lock's. */
    taken = i915_gem_lock(to_i915(fb->dev));
    for (i = 0; i < n; i++) {
        if (r[i].y2 <= r[i].y1 || r[i].y1 < 0) {
            continue;
        }
        i915_gem_object_flush_range((struct drm_i915_gem_object *)fb->obj[0],
                                    fb->offsets[0] + (uint64_t)r[i].y1 * fb->pitches[0],
                                    fb->offsets[0] + (uint64_t)r[i].y2 * fb->pitches[0]);
    }
    i915_gem_unlock(to_i915(fb->dev), taken);
}

/* ------------------------------------------------------------------------
 * The primary plane
 */

/** DSPCNTR's format of `format`, the inverse of the readout's. */
static uint32_t primary_format(uint32_t format)
{
    switch (format) {
    case DRM_FORMAT_C8:
        return DISP_FORMAT_8BPP;
    case DRM_FORMAT_RGB565:
        return DISP_FORMAT_BGRX565;
    case DRM_FORMAT_XBGR8888:
        return DISP_FORMAT_RGBX888;
    case DRM_FORMAT_XRGB2101010:
        return DISP_FORMAT_BGRX101010;
    case DRM_FORMAT_XBGR2101010:
        return DISP_FORMAT_RGBX101010;
    case DRM_FORMAT_XBGR16161616F:
        return DISP_FORMAT_RGBX161616;
    default:
        return DISP_FORMAT_BGRX888;
    }
}

/** The primary plane's check (`intel_plane_check`). */
static int32_t primary_check(struct drm_plane *plane, struct drm_atomic_state *state)
{
    return intel_plane_check(plane, state, 1);
}

/** The committed pipe's gamma-enable bit, with the mode lock held. */
int intel_crtc_gamma_enabled(const struct intel_crtc *crtc)
{
    const struct drm_plane_state *ps = crtc->primary.state;

    return crtc->base.state->gamma_lut &&
           !(ps && ps->visible && ps->fb && ps->fb->format == DRM_FORMAT_C8);
}

uint32_t intel_plane_ctl_crtc(const struct intel_crtc *crtc)
{
    return (crtc->cfg.limited_range ? DISP_PIPE_CSC_ENABLE : 0) |
           (intel_crtc_gamma_enabled(crtc) ? DISP_PIPE_GAMMA_ENABLE : 0);
}

/**
 * Show the primary plane's state, Linux's i9xx_plane_update_arm: the
 * stride, the offset and the control, then the surface, whose write arms
 * the rest for the next vblank; an async flip writes the control with its
 * async bit and the surface alone, which the plane takes at once. The VGA
 * plane, which would cover it, is turned off before the first.
 */
static void primary_update(struct drm_plane *plane, struct drm_atomic_state *state)
{
    struct intel_crtc *c = primary_crtc(plane);
    struct drm_i915_private *i915 = c->i915;
    struct i915_hw *hw = &i915->hw;
    const struct drm_plane_state *ps = plane->state;
    const struct drm_crtc_state *cs = c->base.state;
    uint32_t cntr, surf, stride, x, y;

    if (!ps->visible || !ps->fb || !cs->active) {
        i915_write(hw, DSPCNTR(c->pipe), intel_plane_ctl_crtc(c));
        i915_write(hw, DSPSURF(c->pipe), 0);
        c->planes_on &= ~(1u << INTEL_PLANE_PRIMARY);
        return;
    }
    if (!i915->display.vga_off) {
        if (!(i915_read(hw, CPU_VGACNTRL) & VGA_DISP_DISABLE)) {
            i915_write(hw, CPU_VGACNTRL, VGA_DISP_DISABLE);
            i915_posting_read(hw, CPU_VGACNTRL);
        }
        i915->display.vga_off = 1;
    }
    flush_damage(ps);
    surf = plane_surface(ps, 1, &stride, &x, &y);
    cntr = DISP_ENABLE | primary_format(ps->fb->format);
    if (intel_fb_modifier(ps->fb) == I915_FORMAT_MOD_X_TILED) {
        cntr |= DISP_TILED;
    }
    if (ps->rotation & DRM_MODE_ROTATE_180) {
        cntr |= DISP_ROTATE_180;
    }
    cntr |= intel_plane_ctl_crtc(c);
    if (state->crtcs_new[c->base.index] && state->crtcs_new[c->base.index]->async_flip) {
        i915_write(hw, DSPCNTR(c->pipe), cntr | DISP_ASYNC_FLIP);
        i915_write(hw, DSPSURF(c->pipe), surf);
    } else {
        i915_write(hw, DSPSTRIDE(c->pipe), stride);
        i915_write(hw, DSPOFFSET(c->pipe), y << 16 | x);
        i915_write(hw, DSPCNTR(c->pipe), cntr);
        i915_write(hw, DSPSURF(c->pipe), surf);
    }
    c->planes_on |= 1u << INTEL_PLANE_PRIMARY;
}

static const struct drm_plane_funcs primary_funcs = {primary_check, primary_update, NULL};

/* ------------------------------------------------------------------------
 * The sprite plane
 */

/** SPRCTL's format bits of `format`, Linux's ivb_sprite_ctl. */
static uint32_t sprite_format(uint32_t format)
{
    switch (format) {
    case DRM_FORMAT_XBGR8888:
        return SPRITE_FORMAT_RGBX888 | SPRITE_RGB_ORDER_RGBX;
    case DRM_FORMAT_XRGB2101010:
        return SPRITE_FORMAT_RGBX101010;
    case DRM_FORMAT_XBGR2101010:
        return SPRITE_FORMAT_RGBX101010 | SPRITE_RGB_ORDER_RGBX;
    case DRM_FORMAT_XRGB16161616F:
        return SPRITE_FORMAT_RGBX161616;
    case DRM_FORMAT_XBGR16161616F:
        return SPRITE_FORMAT_RGBX161616 | SPRITE_RGB_ORDER_RGBX;
    case DRM_FORMAT_YUYV:
        return SPRITE_FORMAT_YUV422 | SPRITE_YUV_ORDER_YUYV;
    case DRM_FORMAT_YVYU:
        return SPRITE_FORMAT_YUV422 | SPRITE_YUV_ORDER_YVYU;
    case DRM_FORMAT_UYVY:
        return SPRITE_FORMAT_YUV422 | SPRITE_YUV_ORDER_UYVY;
    case DRM_FORMAT_VYUY:
        return SPRITE_FORMAT_YUV422 | SPRITE_YUV_ORDER_VYUY;
    default:
        return SPRITE_FORMAT_RGBX888;
    }
}

/** The sprite's check (`intel_plane_check`). */
static int32_t sprite_check(struct drm_plane *plane, struct drm_atomic_state *state)
{
    return intel_plane_check(plane, state, 0);
}

/**
 * Show the sprite's state, Linux's ivb_sprite_update_noarm and
 * ivb_sprite_update_arm: its stride, place, size, no colour key and no
 * scaling, its offset and control, then its surface, which arms them.
 */
static void sprite_update(struct drm_plane *plane, struct drm_atomic_state *state)
{
    struct intel_crtc *c = sprite_crtc(plane);
    struct i915_hw *hw = &c->i915->hw;
    const struct drm_plane_state *ps = plane->state;
    const struct drm_crtc_state *cs = c->base.state;
    uint32_t ctl, surf, stride, x, y, w, h;

    (void)state;
    if (!ps->visible || !ps->fb || !cs->active) {
        i915_write(hw, SPRCTL(c->pipe), 0);
        i915_write(hw, SPRSCALE(c->pipe), 0);
        i915_write(hw, SPRSURF(c->pipe), 0);
        c->planes_on &= ~(1u << INTEL_PLANE_SPRITE);
        return;
    }
    flush_damage(ps);
    surf = plane_surface(ps, 0, &stride, &x, &y);
    w = (uint32_t)(ps->dst.x2 - ps->dst.x1);
    h = (uint32_t)(ps->dst.y2 - ps->dst.y1);
    ctl = SPRITE_ENABLE | SPRITE_PLANE_GAMMA_DISABLE | sprite_format(ps->fb->format);
    if (intel_fb_modifier(ps->fb) == I915_FORMAT_MOD_X_TILED) {
        ctl |= SPRITE_TILED;
    }
    if (ps->rotation & DRM_MODE_ROTATE_180) {
        ctl |= SPRITE_ROTATE_180;
    }
    if (c->cfg.limited_range) {
        ctl |= SPRITE_PIPE_CSC_ENABLE;
    }
    if (intel_crtc_gamma_enabled(c)) {
        ctl |= SPRITE_PIPE_GAMMA_ENABLE;
    }
    i915_write(hw, SPRSTRIDE(c->pipe), stride);
    i915_write(hw, SPRPOS(c->pipe), (uint32_t)ps->dst.y1 << 16 | (uint32_t)ps->dst.x1);
    i915_write(hw, SPRSIZE(c->pipe), (h - 1u) << 16 | (w - 1u));
    i915_write(hw, SPRSCALE(c->pipe), 0);
    i915_write(hw, SPRKEYVAL(c->pipe), 0);
    i915_write(hw, SPRKEYMAX(c->pipe), 0);
    i915_write(hw, SPRKEYMSK(c->pipe), 0);
    i915_write(hw, SPROFFSET(c->pipe), y << 16 | x);
    i915_write(hw, SPRCTL(c->pipe), ctl);
    i915_write(hw, SPRSURF(c->pipe), surf);
    c->planes_on |= 1u << INTEL_PLANE_SPRITE;
}

static const struct drm_plane_funcs sprite_funcs = {sprite_check, sprite_update, NULL};

int32_t intel_planes_init(struct intel_crtc *crtc)
{
    struct drm_device *dev = &crtc->i915->drm;
    uint32_t pipe_mask = 1u << crtc->pipe;
    int32_t err;

    if ((err = drm_universal_plane_init(dev, &crtc->primary, pipe_mask, &primary_funcs,
                                        primary_formats,
                                        sizeof(primary_formats) / sizeof(primary_formats[0]),
                                        plane_modifiers, DRM_PLANE_TYPE_PRIMARY)) != 0 ||
        (err = drm_plane_create_rotation_property(&crtc->primary, DRM_MODE_ROTATE_0,
                                                  DRM_MODE_ROTATE_0 | DRM_MODE_ROTATE_180)) != 0 ||
        (err = drm_plane_create_zpos_property(&crtc->primary, 0, 0, 0)) != 0) {
        return err;
    }
    if ((err =
             drm_universal_plane_init(dev, &crtc->sprite, pipe_mask, &sprite_funcs, sprite_formats,
                                      sizeof(sprite_formats) / sizeof(sprite_formats[0]),
                                      plane_modifiers, DRM_PLANE_TYPE_OVERLAY)) != 0 ||
        (err = drm_plane_create_rotation_property(&crtc->sprite, DRM_MODE_ROTATE_0,
                                                  DRM_MODE_ROTATE_0 | DRM_MODE_ROTATE_180)) != 0 ||
        (err = drm_plane_create_zpos_property(&crtc->sprite, 1, 1, 1)) != 0) {
        return err;
    }
    return intel_cursor_init(crtc);
}
