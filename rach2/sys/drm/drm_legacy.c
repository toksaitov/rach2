/*
 * drm_legacy.c: the mode-setting calls that came before atomic, SETCRTC,
 * SETPLANE, CURSOR, PAGE_FLIP, DIRTYFB, SETGAMMA and the property sets, each
 * made an atomic commit as Linux's atomic helpers make them.
 */

#include "drm.h"

#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"
#include "../mm/usercopy.h"

#define DRM_INT_MAX 0x7fffffffL

/* ------------------------------------------------------------------------
 * Planes
 */

/** Turn `plane` off, Linux's drm_atomic_helper_disable_plane. The caller
 * holds the commit lock. */
static int32_t disable_plane(struct drm_plane *plane)
{
    struct drm_atomic_state *state = drm_atomic_state_alloc(plane->dev);
    struct drm_plane_state *ps;
    int32_t rc;

    if (!state) {
        return -ENOMEM;
    }
    ps = drm_atomic_get_plane_state(state, plane);
    if (!ps) {
        drm_atomic_state_free(state);
        return -ENOMEM;
    }
    if (ps->crtc && ps->crtc->cursor == plane) {
        state->legacy_cursor_update = 1;
    }
    rc = drm_atomic_set_crtc_for_plane(state, ps, NULL);
    if (rc == 0) {
        drm_atomic_set_fb_for_plane(ps, NULL);
        ps->crtc_x = ps->crtc_y = 0;
        ps->crtc_w = ps->crtc_h = 0;
        ps->src_x = ps->src_y = ps->src_w = ps->src_h = 0;
        rc = drm_atomic_commit(state);
    }
    drm_atomic_state_free(state);
    return rc;
}

/**
 * Show `fb` on `plane` over `crtc` at the place and with the source `r`
 * gives (its ids unused), Linux's __setplane_atomic: the checks of
 * __setplane_check, then the commit of drm_atomic_helper_update_plane;
 * NULL `fb` turns the plane off. The caller holds the commit lock.
 */
static int32_t setplane_atomic(struct drm_plane *plane, struct drm_crtc *crtc,
                               struct drm_framebuffer *fb, const struct drm_mode_set_plane *r)
{
    struct drm_atomic_state *state;
    struct drm_plane_state *ps;
    int32_t rc;

    if (!fb) {
        return disable_plane(plane);
    }
    if (!(plane->possible_crtcs & drm_crtc_mask(crtc)) ||
        !drm_plane_has_format(plane, fb->format, fb->modifier)) {
        return -EINVAL;
    }
    if (r->crtc_w > DRM_INT_MAX || r->crtc_x > DRM_INT_MAX - (int32_t)r->crtc_w ||
        r->crtc_h > DRM_INT_MAX || r->crtc_y > DRM_INT_MAX - (int32_t)r->crtc_h) {
        return -ERANGE;
    }
    rc = drm_framebuffer_check_src_coords(r->src_x, r->src_y, r->src_w, r->src_h, fb);
    if (rc != 0) {
        return rc;
    }
    state = drm_atomic_state_alloc(plane->dev);
    if (!state) {
        return -ENOMEM;
    }
    ps = drm_atomic_get_plane_state(state, plane);
    rc = ps ? drm_atomic_set_crtc_for_plane(state, ps, crtc) : -ENOMEM;
    if (rc == 0) {
        drm_atomic_set_fb_for_plane(ps, fb);
        ps->crtc_x = r->crtc_x;
        ps->crtc_y = r->crtc_y;
        ps->crtc_w = r->crtc_w;
        ps->crtc_h = r->crtc_h;
        ps->src_x = r->src_x;
        ps->src_y = r->src_y;
        ps->src_w = r->src_w;
        ps->src_h = r->src_h;
        if (plane == crtc->cursor) {
            state->legacy_cursor_update = 1;
        }
        rc = drm_atomic_commit(state);
    }
    drm_atomic_state_free(state);
    return rc;
}

/** SETPLANE, Linux's drm_mode_setplane: show a framebuffer on a plane, or
 * turn the plane off for framebuffer 0. */
int32_t drm_mode_setplane(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_mode_set_plane *req = (struct drm_mode_set_plane *)data;
    struct drm_framebuffer *fb = NULL;
    struct drm_crtc *crtc = NULL;
    struct drm_plane *plane;
    int32_t rc;

    if (!drm_core_check_feature(dev, DRIVER_MODESET)) {
        return -EOPNOTSUPP;
    }
    plane = drm_plane_find(dev, file, req->plane_id);
    if (!plane) {
        return -ENOENT;
    }
    if (req->fb_id) {
        fb = drm_framebuffer_lookup(dev, file, req->fb_id);
        if (!fb) {
            return -ENOENT;
        }
        crtc = drm_crtc_find(dev, file, req->crtc_id);
        if (!crtc) {
            drm_framebuffer_put(fb);
            return -ENOENT;
        }
    }
    rc = drm_modeset_lock(dev);
    if (rc == 0) {
        rc = setplane_atomic(plane, crtc, fb, req);
        drm_modeset_unlock(dev);
    }
    drm_framebuffer_put(fb);
    return rc;
}

/* ------------------------------------------------------------------------
 * The cursor
 */

/**
 * Linux's drm_mode_cursor_universal: CURSOR through the CRTC's cursor
 * plane. A new buffer becomes an ARGB8888 framebuffer of its size, with
 * the hotspot set where the plane has one; handle 0 hides the cursor; a
 * move alone keeps the framebuffer. The caller holds the commit lock.
 */
static int32_t cursor_universal(struct drm_crtc *crtc, const struct drm_mode_cursor2 *req,
                                struct drm_file *file)
{
    struct drm_plane *plane = crtc->cursor;
    struct drm_framebuffer *fb = NULL;
    struct drm_mode_set_plane place;
    struct drm_mode_fb_cmd2 fbreq;
    int32_t rc = 0;

    if (req->flags & DRM_MODE_CURSOR_BO) {
        if (req->handle) {
            k_memset(&fbreq, 0, sizeof(fbreq));
            fbreq.width = req->width;
            fbreq.height = req->height;
            fbreq.pixel_format = DRM_FORMAT_ARGB8888;
            fbreq.pitches[0] = req->width * 4;
            fbreq.handles[0] = req->handle;
            fb = drm_internal_framebuffer_create(crtc->dev, &fbreq, file, &rc);
            if (!fb) {
                return rc;
            }
            /* Linux sets the hotspot in the current state, which the
             * commit's state copies. */
            if (plane->hotspot_x_property) {
                plane->state->hotspot_x = req->hot_x;
            }
            if (plane->hotspot_y_property) {
                plane->state->hotspot_y = req->hot_y;
            }
        }
    } else {
        fb = plane->state->fb;
        drm_framebuffer_get(fb);
    }
    k_memset(&place, 0, sizeof(place));
    if (req->flags & DRM_MODE_CURSOR_MOVE) {
        place.crtc_x = req->x;
        place.crtc_y = req->y;
    } else {
        place.crtc_x = crtc->cursor_x;
        place.crtc_y = crtc->cursor_y;
    }
    if (fb) {
        place.crtc_w = fb->width;
        place.crtc_h = fb->height;
        place.src_w = fb->width << 16;
        place.src_h = fb->height << 16;
    }
    rc = setplane_atomic(plane, crtc, fb, &place);
    drm_framebuffer_put(fb);
    if (rc == 0 && (req->flags & DRM_MODE_CURSOR_MOVE)) {
        crtc->cursor_x = req->x;
        crtc->cursor_y = req->y;
    }
    return rc;
}

/** Linux's drm_mode_cursor_common: CURSOR and CURSOR2 on a CRTC with a
 * cursor plane; a CRTC without one has no cursor to set (ENXIO) or move
 * (EFAULT), as Linux answers for a driver without cursor hooks. */
static int32_t cursor_common(struct drm_device *dev, const struct drm_mode_cursor2 *req,
                             struct drm_file *file)
{
    struct drm_crtc *crtc;
    int32_t rc;

    if (!drm_core_check_feature(dev, DRIVER_MODESET)) {
        return -EOPNOTSUPP;
    }
    if (!req->flags || (req->flags & ~(uint32_t)DRM_MODE_CURSOR_FLAGS)) {
        return -EINVAL;
    }
    crtc = drm_crtc_find(dev, file, req->crtc_id);
    if (!crtc) {
        return -ENOENT;
    }
    rc = drm_modeset_lock(dev);
    if (rc != 0) {
        return rc;
    }
    if (crtc->cursor) {
        rc = cursor_universal(crtc, req, file);
    } else {
        rc = (req->flags & DRM_MODE_CURSOR_BO) ? -ENXIO : -EFAULT;
    }
    drm_modeset_unlock(dev);
    return rc;
}

/** CURSOR: CURSOR2 with the hotspot at the corner. */
int32_t drm_mode_cursor_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    const struct drm_mode_cursor *req = (const struct drm_mode_cursor *)data;
    struct drm_mode_cursor2 new_req;

    new_req.flags = req->flags;
    new_req.crtc_id = req->crtc_id;
    new_req.x = req->x;
    new_req.y = req->y;
    new_req.width = req->width;
    new_req.height = req->height;
    new_req.handle = req->handle;
    new_req.hot_x = 0;
    new_req.hot_y = 0;
    return cursor_common(dev, &new_req, file);
}

/** CURSOR2: set or move the cursor, with its hotspot. */
int32_t drm_mode_cursor2_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    return cursor_common(dev, (const struct drm_mode_cursor2 *)data, file);
}

/* ------------------------------------------------------------------------
 * SETCRTC and PAGE_FLIP
 */

/**
 * The framebuffer and mode of a SETCRTC that sets a mode, with Linux's
 * checks: framebuffer -1 for the one the primary plane shows, the mode's
 * aspect only for a client that asked for it, the format the primary plane
 * takes and a viewport inside the framebuffer, Linux's
 * drm_crtc_check_viewport. Sets `*fb` to a reference the caller puts, also
 * on an error. The caller holds the commit lock.
 */
static int32_t setcrtc_fb_mode(struct drm_crtc *crtc, struct drm_file *file,
                               const struct drm_mode_crtc *req, struct drm_mode_modeinfo *mode,
                               struct drm_framebuffer **fb)
{
    uint32_t w, h;
    int32_t rc;

    if (req->fb_id == 0xffffffffu) {
        *fb = crtc->primary ? crtc->primary->state->fb : NULL;
        if (!*fb) {
            return -EINVAL;
        }
        drm_framebuffer_get(*fb);
    } else {
        *fb = drm_framebuffer_lookup(crtc->dev, file, req->fb_id);
        if (!*fb) {
            return -ENOENT;
        }
    }
    if (!file->aspect_ratio && (req->mode.flags & DRM_MODE_FLAG_PIC_AR_MASK)) {
        return -EINVAL;
    }
    rc = drm_mode_convert_umode(mode, &req->mode);
    if (rc != 0) {
        return rc;
    }
    if (!crtc->primary) {
        return 0;
    }
    if (!drm_plane_has_format(crtc->primary, (*fb)->format, (*fb)->modifier)) {
        return -EINVAL;
    }
    drm_mode_get_hv_timing(mode, &w, &h);
    if (crtc->primary->state->rotation & (DRM_MODE_ROTATE_90 | DRM_MODE_ROTATE_270)) {
        uint32_t t = w;

        w = h;
        h = t;
    }
    return drm_framebuffer_check_src_coords(req->x << 16, req->y << 16, w << 16, h << 16, *fb);
}

/** SETCRTC, Linux's drm_mode_setcrtc: set a CRTC's mode, framebuffer and
 * connectors, or turn it off, through `drm_atomic_helper_set_config`. */
int32_t drm_mode_setcrtc(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_mode_crtc *req = (struct drm_mode_crtc *)data;
    struct drm_connector *connectors[DRM_MAX_CONNECTORS];
    struct drm_framebuffer *fb = NULL;
    struct drm_mode_modeinfo mode;
    struct drm_mode_set set;
    struct drm_crtc *crtc;
    uint32_t i, id;
    int32_t rc;

    if (!drm_core_check_feature(dev, DRIVER_MODESET)) {
        return -EOPNOTSUPP;
    }
    /* A plane's source offsets are 16.16. */
    if ((req->x & 0xffff0000u) || (req->y & 0xffff0000u)) {
        return -ERANGE;
    }
    crtc = drm_crtc_find(dev, file, req->crtc_id);
    if (!crtc) {
        return -ENOENT;
    }
    rc = drm_modeset_lock(dev);
    if (rc != 0) {
        return rc;
    }
    if (req->mode_valid) {
        rc = setcrtc_fb_mode(crtc, file, req, &mode, &fb);
        if (rc != 0) {
            goto out;
        }
    }
    if ((req->count_connectors == 0 && req->mode_valid) ||
        (req->count_connectors > 0 && (!req->mode_valid || !fb)) ||
        req->count_connectors > dev->mode_config.num_connector) {
        rc = -EINVAL;
        goto out;
    }
    for (i = 0; i < req->count_connectors; i++) {
        if (copy_from_user(&id, (const void __user *)(uintptr_t)(req->set_connectors_ptr + 4u * i),
                           4) != 0) {
            rc = -EFAULT;
            goto out;
        }
        connectors[i] = drm_connector_lookup(dev, file, id);
        if (!connectors[i]) {
            rc = -ENOENT;
            goto out;
        }
    }
    set.crtc = crtc;
    set.fb = fb;
    set.x = req->x;
    set.y = req->y;
    set.mode = req->mode_valid ? &mode : NULL;
    set.connectors = connectors;
    set.num_connectors = req->count_connectors;
    rc = drm_atomic_helper_set_config(&set);
out:
    drm_framebuffer_put(fb);
    drm_modeset_unlock(dev);
    return rc;
}

/**
 * PAGE_FLIP, Linux's drm_mode_page_flip_ioctl with
 * drm_atomic_helper_page_flip: show another framebuffer of the same format
 * on a CRTC's primary plane in a nonblocking commit that may not change the
 * mode, and send FLIP_COMPLETE when asked. A target vblank is refused with
 * EINVAL, as Linux refuses it for a driver without page_flip_target.
 */
int32_t drm_mode_page_flip_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_mode_crtc_page_flip *req = (struct drm_mode_crtc_page_flip *)data;
    struct drm_pending_event *e = NULL;
    struct drm_framebuffer *fb = NULL, *old_fb;
    struct drm_atomic_state *state;
    struct drm_crtc_state *cs;
    struct drm_plane_state *ps, *cur;
    struct drm_crtc *crtc;
    int32_t rc;

    if (!drm_core_check_feature(dev, DRIVER_MODESET)) {
        return -EOPNOTSUPP;
    }
    if ((req->flags & ~(uint32_t)DRM_MODE_PAGE_FLIP_FLAGS) ||
        (req->reserved != 0 && !(req->flags & DRM_MODE_PAGE_FLIP_TARGET)) ||
        (req->flags & DRM_MODE_PAGE_FLIP_TARGET) == DRM_MODE_PAGE_FLIP_TARGET ||
        ((req->flags & DRM_MODE_PAGE_FLIP_ASYNC) && !dev->mode_config.async_page_flip)) {
        return -EINVAL;
    }
    crtc = drm_crtc_find(dev, file, req->crtc_id);
    if (!crtc) {
        return -ENOENT;
    }
    if (!crtc->primary || (req->flags & DRM_MODE_PAGE_FLIP_TARGET)) {
        return -EINVAL;
    }
    rc = drm_modeset_lock(dev);
    if (rc != 0) {
        return rc;
    }
    cur = crtc->primary->state;
    old_fb = cur->fb;
    if (!old_fb) {
        /* Unbound, as after a hotplug the program has not seen yet. */
        rc = -EBUSY;
        goto out;
    }
    fb = drm_framebuffer_lookup(dev, file, req->fb_id);
    if (!fb) {
        rc = -ENOENT;
        goto out;
    }
    rc = drm_framebuffer_check_src_coords(cur->src_x, cur->src_y, cur->src_w, cur->src_h, fb);
    if (rc != 0) {
        goto out;
    }
    if (old_fb->format != fb->format) {
        rc = -EINVAL;
        goto out;
    }
    if (req->flags & DRM_MODE_PAGE_FLIP_EVENT) {
        e = drm_crtc_flip_event(crtc, req->user_data);
        if (!e) {
            rc = -ENOMEM;
            goto out;
        }
        rc = drm_event_reserve(file, e);
        if (rc != 0) {
            kfree(e);
            goto out;
        }
    }
    state = drm_atomic_state_alloc(dev);
    cs = state ? drm_atomic_get_crtc_state(state, crtc) : NULL;
    if (!cs) {
        if (e) {
            drm_event_cancel_free(e);
        }
        drm_atomic_state_free(state);
        rc = -ENOMEM;
        goto out;
    }
    /* Linux's page_flip_common; the state owns the event from here. */
    cs->event = e;
    cs->async_flip = (req->flags & DRM_MODE_PAGE_FLIP_ASYNC) != 0;
    ps = drm_atomic_get_plane_state(state, crtc->primary);
    rc = ps ? drm_atomic_set_crtc_for_plane(state, ps, crtc) : -ENOMEM;
    if (rc == 0) {
        drm_atomic_set_fb_for_plane(ps, fb);
        state->allow_modeset = 0;
        /* Linux's page_flip_common refuses a legacy flip of a CRTC that is
         * off, as DPMS leaves it. */
        rc = cs->active ? drm_atomic_nonblocking_commit(state) : -EINVAL;
    }
    drm_atomic_state_free(state);
out:
    drm_framebuffer_put(fb);
    drm_modeset_unlock(dev);
    return rc;
}

/* ------------------------------------------------------------------------
 * Damage
 */

/** Commit `damage` as FB_DAMAGE_CLIPS on each plane that shows `fb`, NULL
 * for the whole framebuffer. The caller holds the commit lock. 0 or
 * -errno. */
static int32_t dirty_commit(struct drm_device *dev, struct drm_framebuffer *fb,
                            struct drm_property_blob *damage)
{
    struct drm_atomic_state *state;
    struct drm_plane_state *ps;
    struct drm_plane *plane;
    int32_t rc = 0;
    uint32_t i;

    state = drm_atomic_state_alloc(dev);
    if (!state) {
        return -ENOMEM;
    }
    for (i = 0; rc == 0 && i < dev->mode_config.num_plane; i++) {
        plane = dev->mode_config.planes[i];
        if (plane->state->fb != fb) {
            continue;
        }
        ps = drm_atomic_get_plane_state(state, plane);
        if (!ps) {
            rc = -ENOMEM;
            break;
        }
        drm_property_blob_put(ps->fb_damage_clips);
        ps->fb_damage_clips = drm_property_blob_get(damage);
    }
    if (rc == 0) {
        rc = drm_atomic_commit(state);
    }
    drm_atomic_state_free(state);
    return rc;
}

/**
 * DIRTYFB, Linux's drm_mode_dirtyfb_ioctl with drm_atomic_helper_dirtyfb:
 * give the planes that show a framebuffer its changed rectangles as
 * FB_DAMAGE_CLIPS, the first of each pair for a copy, in a commit. ENOSYS
 * for a device whose framebuffers need no flush (`fb_dirty` unset).
 */
int32_t drm_mode_dirtyfb_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_mode_fb_dirty_cmd *req = (struct drm_mode_fb_dirty_cmd *)data;
    struct drm_property_blob *damage = NULL;
    struct drm_clip_rect *clips = NULL;
    struct drm_framebuffer *fb;
    struct drm_mode_rect *rects;
    uint32_t i, n = req->num_clips, flags = req->flags & DRM_MODE_FB_DIRTY_FLAGS, step = 1;
    int32_t rc = 0;

    if (!drm_core_check_feature(dev, DRIVER_MODESET)) {
        return -EOPNOTSUPP;
    }
    fb = drm_framebuffer_lookup(dev, file, req->fb_id);
    if (!fb) {
        return -ENOENT;
    }
    if (!n != !req->clips_ptr || ((flags & DRM_MODE_FB_DIRTY_ANNOTATE_COPY) && (n % 2)) ||
        n > DRM_MODE_FB_DIRTY_MAX_CLIPS) {
        rc = -EINVAL;
        goto out;
    }
    if (n) {
        clips = (struct drm_clip_rect *)kmalloc(n * sizeof(*clips));
        if (!clips) {
            rc = -ENOMEM;
            goto out;
        }
        if (copy_from_user(clips, (const void __user *)(uintptr_t)req->clips_ptr,
                           n * sizeof(*clips)) != 0) {
            rc = -EFAULT;
            goto out;
        }
    }
    if (!dev->mode_config.fb_dirty) {
        rc = -ENOSYS;
        goto out;
    }
    if (n) {
        if (flags & DRM_MODE_FB_DIRTY_ANNOTATE_COPY) {
            step = 2;
            n /= 2;
        }
        damage = drm_property_create_blob(dev, n * (uint32_t)sizeof(*rects), NULL, &rc);
        if (!damage) {
            goto out;
        }
        rects = (struct drm_mode_rect *)damage->data;
        for (i = 0; i < n; i++) {
            rects[i].x1 = clips[i * step].x1;
            rects[i].y1 = clips[i * step].y1;
            rects[i].x2 = clips[i * step].x2;
            rects[i].y2 = clips[i * step].y2;
        }
    }
    rc = drm_modeset_lock(dev);
    if (rc != 0) {
        goto out;
    }
    rc = dirty_commit(dev, fb, damage);
    drm_modeset_unlock(dev);
out:
    drm_property_blob_put(damage);
    kfree(clips);
    drm_framebuffer_put(fb);
    return rc;
}

int32_t drm_framebuffer_dirty(struct drm_device *dev, struct drm_framebuffer *fb,
                              const struct drm_mode_rect *rect)
{
    struct drm_property_blob *damage;
    int32_t rc = 0;

    if (!dev->mode_config.fb_dirty) {
        return -ENOSYS;
    }
    damage = drm_property_create_blob(dev, (uint32_t)sizeof(*rect), rect, &rc);
    if (!damage) {
        return rc != 0 ? rc : -ENOMEM;
    }
    drm_modeset_lock_nointr(dev);
    rc = dirty_commit(dev, fb, damage);
    drm_modeset_unlock(dev);
    drm_property_blob_put(damage);
    return rc;
}

/* ------------------------------------------------------------------------
 * Gamma
 */

/** SETGAMMA, Linux's drm_mode_gamma_set_ioctl: load a CRTC's gamma table,
 * and commit it as GAMMA_LUT, or DEGAMMA_LUT on a CRTC with only that, as
 * Linux's drm_crtc_legacy_gamma_set does. ENOSYS for a CRTC with neither. */
int32_t drm_mode_gamma_set_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_mode_crtc_lut *req = (struct drm_mode_crtc_lut *)data;
    struct drm_mode_config *mc = &dev->mode_config;
    struct drm_property_blob *blob;
    struct drm_atomic_state *state;
    struct drm_crtc_state *cs;
    struct drm_color_lut *lut;
    struct drm_crtc *crtc;
    uint16_t *r, *g, *b;
    uint32_t i, size;
    int use_gamma, replaced;
    int32_t rc;

    if (!drm_core_check_feature(dev, DRIVER_MODESET)) {
        return -EOPNOTSUPP;
    }
    crtc = drm_crtc_find(dev, file, req->crtc_id);
    if (!crtc) {
        return -ENOENT;
    }
    use_gamma = drm_object_find_property(&crtc->base, mc->gamma_lut_property->base.id) != NULL;
    if (!crtc->gamma_size ||
        (!use_gamma && !drm_object_find_property(&crtc->base, mc->degamma_lut_property->base.id))) {
        return -ENOSYS;
    }
    if (req->gamma_size != crtc->gamma_size) {
        return -EINVAL;
    }
    rc = drm_modeset_lock(dev);
    if (rc != 0) {
        return rc;
    }
    size = crtc->gamma_size * (uint32_t)sizeof(uint16_t);
    r = crtc->gamma_store;
    g = r + crtc->gamma_size;
    b = g + crtc->gamma_size;
    if (copy_from_user(r, (const void __user *)(uintptr_t)req->red, size) != 0 ||
        copy_from_user(g, (const void __user *)(uintptr_t)req->green, size) != 0 ||
        copy_from_user(b, (const void __user *)(uintptr_t)req->blue, size) != 0) {
        drm_modeset_unlock(dev);
        return -EFAULT;
    }
    blob = drm_property_create_blob(dev, crtc->gamma_size * (uint32_t)sizeof(*lut), NULL, &rc);
    state = blob ? drm_atomic_state_alloc(dev) : NULL;
    cs = state ? drm_atomic_get_crtc_state(state, crtc) : NULL;
    if (!cs) {
        rc = blob ? -ENOMEM : rc;
    } else {
        lut = (struct drm_color_lut *)blob->data;
        for (i = 0; i < crtc->gamma_size; i++) {
            lut[i].red = r[i];
            lut[i].green = g[i];
            lut[i].blue = b[i];
        }
        replaced = cs->degamma_lut != (use_gamma ? NULL : blob) || cs->ctm != NULL ||
                   cs->gamma_lut != (use_gamma ? blob : NULL);
        drm_property_blob_put(cs->degamma_lut);
        drm_property_blob_put(cs->ctm);
        drm_property_blob_put(cs->gamma_lut);
        cs->degamma_lut = use_gamma ? NULL : drm_property_blob_get(blob);
        cs->ctm = NULL;
        cs->gamma_lut = use_gamma ? drm_property_blob_get(blob) : NULL;
        cs->color_mgmt_changed |= replaced;
        rc = drm_atomic_commit(state);
    }
    drm_atomic_state_free(state);
    drm_property_blob_put(blob);
    drm_modeset_unlock(dev);
    return rc;
}

/* ------------------------------------------------------------------------
 * Properties
 */

/** Linux's drm_atomic_connector_commit_dpms: DPMS on or off, which turns
 * the connector's CRTC on while any of its connectors is on. The caller
 * holds the commit lock. */
static int32_t commit_dpms(struct drm_atomic_state *state, struct drm_connector *connector,
                           uint64_t value)
{
    struct drm_connector_state *conn;
    struct drm_crtc_state *cs;
    struct drm_crtc *crtc;
    int old = connector->dpms,
        mode = value == DRM_MODE_DPMS_ON ? DRM_MODE_DPMS_ON : DRM_MODE_DPMS_OFF;
    int32_t rc = 0;
    int i, active = 0;

    if (connector->dpms == mode) {
        return 0;
    }
    connector->dpms = mode;
    crtc = connector->state->crtc;
    if (!crtc) {
        return 0;
    }
    rc = drm_atomic_add_affected_connectors(state, crtc);
    cs = rc == 0 ? drm_atomic_get_crtc_state(state, crtc) : NULL;
    if (!cs) {
        connector->dpms = old;
        return rc ? rc : -ENOMEM;
    }
    for (i = 0; i < DRM_MAX_CONNECTORS; i++) {
        conn = state->connectors_new[i];
        if (conn && conn->crtc == crtc && conn->connector->dpms == DRM_MODE_DPMS_ON) {
            active = 1;
            break;
        }
    }
    cs->active = active;
    rc = drm_atomic_commit(state);
    if (rc != 0) {
        connector->dpms = old;
    }
    return rc;
}

/** OBJ_SETPROPERTY, Linux's drm_mode_obj_set_property_ioctl: set one
 * property of an object in a commit of its own; DPMS through
 * `commit_dpms`. */
int32_t drm_mode_obj_set_property_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_mode_obj_set_property *req = (struct drm_mode_obj_set_property *)data;
    struct drm_atomic_state *state;
    struct drm_mode_object *obj;
    struct drm_property *prop;
    int32_t rc = -EINVAL;

    if (!drm_core_check_feature(dev, DRIVER_MODESET)) {
        return -EOPNOTSUPP;
    }
    obj = drm_mode_object_find(dev, file, req->obj_id, req->obj_type);
    if (!obj) {
        return -ENOENT;
    }
    if (obj->type != DRM_MODE_OBJECT_CRTC && obj->type != DRM_MODE_OBJECT_PLANE &&
        obj->type != DRM_MODE_OBJECT_CONNECTOR) {
        drm_mode_object_put(obj);
        return -EINVAL;
    }
    prop = drm_object_find_property(obj, req->prop_id);
    if (!prop) {
        return -EINVAL;
    }
    state = drm_atomic_state_alloc(dev);
    if (!state) {
        return -ENOMEM;
    }
    drm_modeset_lock_nointr(dev);
    if (prop == dev->mode_config.dpms_property) {
        rc = obj->type == DRM_MODE_OBJECT_CONNECTOR
                 ? commit_dpms(state, (struct drm_connector *)obj, req->value)
                 : -EINVAL;
    } else {
        rc = drm_atomic_set_property(state, file, obj, prop, req->value);
        if (rc == 0) {
            rc = drm_atomic_commit(state);
        }
    }
    drm_atomic_state_free(state);
    drm_modeset_unlock(dev);
    return rc;
}

/** SETPROPERTY: OBJ_SETPROPERTY on a connector. */
int32_t drm_connector_property_set_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    const struct drm_mode_connector_set_property *req =
        (const struct drm_mode_connector_set_property *)data;
    struct drm_mode_obj_set_property obj;

    obj.value = req->value;
    obj.prop_id = req->prop_id;
    obj.obj_id = req->connector_id;
    obj.obj_type = DRM_MODE_OBJECT_CONNECTOR;
    return drm_mode_obj_set_property_ioctl(dev, &obj, file);
}
