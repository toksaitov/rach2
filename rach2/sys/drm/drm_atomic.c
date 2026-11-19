/*
 * drm_atomic.c: the atomic commit, Linux's drm_atomic.c, drm_atomic_uapi.c
 * and drm_atomic_helper.c: the commit lock, the states a commit duplicates
 * and sets, its check and its commit, and the ATOMIC ioctl.
 */

#include "drm.h"
#include "../dmabuf/dmabuf.h"

#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"
#include "../mm/usercopy.h"
#include "../proc/percpu.h"
#include "../proc/process.h"
#include "../klog.h"
#include "../lib/kirq.h"
#include "../rtc.h"
#include "../timer.h"
#ifndef RACH2_HOSTTEST
#include "../arch/arch.h"
#include "../proc/sched.h"
#include "../proc/smp.h"
#endif

#define DRM_INT_MAX 0x7fffffffL

/* ------------------------------------------------------------------------
 * The commit lock
 */

int32_t drm_modeset_lock(struct drm_device *dev)
{
    if (mutex_held(&dev->mode_lock)) {
        dev->mode_lock_depth++;
        return 0;
    }
    return mutex_lock_interruptible(&dev->mode_lock);
}

void drm_modeset_lock_nointr(struct drm_device *dev)
{
    if (mutex_held(&dev->mode_lock)) {
        dev->mode_lock_depth++;
        return;
    }
    mutex_lock(&dev->mode_lock);
}

void drm_modeset_unlock(struct drm_device *dev)
{
    if (dev->mode_lock_depth != 0) {
        dev->mode_lock_depth--;
        return;
    }
    mutex_unlock(&dev->mode_lock);
}

int drm_modeset_held(const struct drm_device *dev)
{
    return mutex_held(&dev->mode_lock);
}

/* ------------------------------------------------------------------------
 * States
 */

void drm_plane_state_destroy(struct drm_plane_state *ps)
{
    if (ps) {
        drm_framebuffer_put(ps->fb);
        drm_property_blob_put(ps->fb_damage_clips);
        dma_fence_put(ps->fence);
        kfree(ps);
    }
}

void drm_crtc_state_destroy(struct drm_crtc_state *cs)
{
    if (cs) {
        drm_property_blob_put(cs->mode_blob);
        drm_property_blob_put(cs->degamma_lut);
        drm_property_blob_put(cs->ctm);
        drm_property_blob_put(cs->gamma_lut);
        if (cs->event) {
            drm_event_cancel_free(cs->event);
        }
        kfree(cs);
    }
}

struct drm_atomic_state *drm_atomic_state_alloc(struct drm_device *dev)
{
    struct drm_atomic_state *state = (struct drm_atomic_state *)kzalloc(sizeof(*state));

    if (state) {
        state->dev = dev;
        state->allow_modeset = 1;
    }
    return state;
}

void drm_atomic_state_free(struct drm_atomic_state *state)
{
    int i;

    if (!state) {
        return;
    }
    for (i = 0; i < DRM_MAX_PLANES; i++) {
        drm_plane_state_destroy(state->planes_new[i]);
    }
    for (i = 0; i < DRM_MAX_CRTCS; i++) {
        drm_crtc_state_destroy(state->crtcs_new[i]);
    }
    for (i = 0; i < DRM_MAX_CONNECTORS; i++) {
        kfree(state->connectors_new[i]);
    }
    kfree(state);
}

struct drm_crtc_state *drm_atomic_get_crtc_state(struct drm_atomic_state *state,
                                                 struct drm_crtc *crtc)
{
    struct drm_crtc_state *cs = state->crtcs_new[crtc->index];

    if (cs) {
        return cs;
    }
    cs = (struct drm_crtc_state *)kmalloc(sizeof(*cs));
    if (!cs) {
        return NULL;
    }
    /* Linux's __drm_atomic_helper_crtc_duplicate_state. */
    *cs = *crtc->state;
    drm_property_blob_get(cs->mode_blob);
    drm_property_blob_get(cs->degamma_lut);
    drm_property_blob_get(cs->ctm);
    drm_property_blob_get(cs->gamma_lut);
    cs->mode_changed = cs->active_changed = cs->connectors_changed = 0;
    cs->planes_changed = cs->color_mgmt_changed = cs->async_flip = 0;
    cs->event = NULL;
    cs->out_fence_ptr = NULL;
    state->crtcs_old[crtc->index] = crtc->state;
    state->crtcs_new[crtc->index] = cs;
    return cs;
}

struct drm_plane_state *drm_atomic_get_plane_state(struct drm_atomic_state *state,
                                                   struct drm_plane *plane)
{
    struct drm_plane_state *ps = state->planes_new[plane->index];

    if (ps) {
        return ps;
    }
    ps = (struct drm_plane_state *)kmalloc(sizeof(*ps));
    if (!ps) {
        return NULL;
    }
    *ps = *plane->state;
    drm_framebuffer_get(ps->fb);
    ps->fence = NULL;
    ps->fb_damage_clips = NULL;
    state->planes_old[plane->index] = plane->state;
    state->planes_new[plane->index] = ps;
    if (ps->crtc && !drm_atomic_get_crtc_state(state, ps->crtc)) {
        return NULL;
    }
    return ps;
}

struct drm_connector_state *drm_atomic_get_connector_state(struct drm_atomic_state *state,
                                                           struct drm_connector *connector)
{
    struct drm_connector_state *cs = state->connectors_new[connector->index];

    if (cs) {
        return cs;
    }
    cs = (struct drm_connector_state *)kmalloc(sizeof(*cs));
    if (!cs) {
        return NULL;
    }
    *cs = *connector->state;
    state->connectors_old[connector->index] = connector->state;
    state->connectors_new[connector->index] = cs;
    if (cs->crtc && !drm_atomic_get_crtc_state(state, cs->crtc)) {
        return NULL;
    }
    return cs;
}

int32_t drm_atomic_set_crtc_for_plane(struct drm_atomic_state *state,
                                      struct drm_plane_state *plane_state, struct drm_crtc *crtc)
{
    struct drm_plane *plane = plane_state->plane;
    struct drm_crtc_state *cs;

    if (plane_state->crtc == crtc) {
        return 0;
    }
    if (plane_state->crtc) {
        cs = drm_atomic_get_crtc_state(state, plane_state->crtc);
        if (!cs) {
            return -ENOMEM;
        }
        cs->plane_mask &= ~drm_plane_mask(plane);
    }
    plane_state->crtc = crtc;
    if (crtc) {
        cs = drm_atomic_get_crtc_state(state, crtc);
        if (!cs) {
            return -ENOMEM;
        }
        cs->plane_mask |= drm_plane_mask(plane);
    }
    return 0;
}

void drm_atomic_set_fb_for_plane(struct drm_plane_state *plane_state, struct drm_framebuffer *fb)
{
    drm_framebuffer_get(fb);
    drm_framebuffer_put(plane_state->fb);
    plane_state->fb = fb;
}

int32_t drm_atomic_set_crtc_for_connector(struct drm_atomic_state *state,
                                          struct drm_connector_state *conn_state,
                                          struct drm_crtc *crtc)
{
    struct drm_connector *connector = conn_state->connector;
    struct drm_crtc_state *cs;

    if (conn_state->crtc == crtc) {
        return 0;
    }
    if (conn_state->crtc) {
        cs = drm_atomic_get_crtc_state(state, conn_state->crtc);
        if (!cs) {
            return -ENOMEM;
        }
        cs->connector_mask &= ~drm_connector_mask(connector);
    }
    if (crtc) {
        cs = drm_atomic_get_crtc_state(state, crtc);
        if (!cs) {
            return -ENOMEM;
        }
        cs->connector_mask |= drm_connector_mask(connector);
    }
    conn_state->crtc = crtc;
    return 0;
}

/** Whether the `n` bytes at `a` and `b` are the same. */
static int same_bytes(const void *a, const void *b, uint32_t n)
{
    const unsigned char *p = (const unsigned char *)a, *q = (const unsigned char *)b;

    while (n > 0 && *p == *q) {
        p++;
        q++;
        n--;
    }
    return n == 0;
}

int32_t drm_atomic_set_mode_for_crtc(struct drm_crtc_state *crtc_state,
                                     const struct drm_mode_modeinfo *mode)
{
    struct drm_property_blob *blob;
    int32_t err = 0;

    if (mode && crtc_state->mode_blob && same_bytes(&crtc_state->mode, mode, sizeof(*mode))) {
        return 0;
    }
    drm_property_blob_put(crtc_state->mode_blob);
    crtc_state->mode_blob = NULL;
    if (mode) {
        blob = drm_property_create_blob(crtc_state->crtc->dev, sizeof(*mode), mode, &err);
        if (!blob) {
            return err;
        }
        crtc_state->mode = *mode;
        crtc_state->mode_blob = blob;
        crtc_state->enable = 1;
    } else {
        k_memset(&crtc_state->mode, 0, sizeof(crtc_state->mode));
        crtc_state->enable = 0;
    }
    return 0;
}

int32_t drm_atomic_set_mode_prop_for_crtc(struct drm_crtc_state *crtc_state,
                                          struct drm_property_blob *blob)
{
    int32_t rc;

    if (blob == crtc_state->mode_blob) {
        return 0;
    }
    drm_property_blob_put(crtc_state->mode_blob);
    crtc_state->mode_blob = NULL;
    k_memset(&crtc_state->mode, 0, sizeof(crtc_state->mode));
    if (blob) {
        if (blob->length != sizeof(struct drm_mode_modeinfo)) {
            return -EINVAL;
        }
        rc =
            drm_mode_convert_umode(&crtc_state->mode, (const struct drm_mode_modeinfo *)blob->data);
        if (rc != 0) {
            return rc;
        }
        crtc_state->mode_blob = drm_property_blob_get(blob);
        crtc_state->enable = 1;
    } else {
        crtc_state->enable = 0;
    }
    return 0;
}

int32_t drm_atomic_add_affected_connectors(struct drm_atomic_state *state, struct drm_crtc *crtc)
{
    struct drm_mode_config *mc = &state->dev->mode_config;
    struct drm_crtc_state *cs = drm_atomic_get_crtc_state(state, crtc);
    uint32_t i;

    if (!cs) {
        return -ENOMEM;
    }
    for (i = 0; i < mc->num_connector; i++) {
        if ((cs->connector_mask & drm_connector_mask(mc->connectors[i])) &&
            !drm_atomic_get_connector_state(state, mc->connectors[i])) {
            return -ENOMEM;
        }
    }
    return 0;
}

int32_t drm_atomic_add_affected_planes(struct drm_atomic_state *state, struct drm_crtc *crtc)
{
    struct drm_mode_config *mc = &state->dev->mode_config;
    const struct drm_crtc_state *old = crtc->state;
    uint32_t i;

    if (!drm_atomic_get_crtc_state(state, crtc)) {
        return -ENOMEM;
    }
    for (i = 0; i < mc->num_plane; i++) {
        if ((old->plane_mask & drm_plane_mask(mc->planes[i])) &&
            !drm_atomic_get_plane_state(state, mc->planes[i])) {
            return -ENOMEM;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------
 * Properties in a state
 */

/**
 * Set one of the properties a driver attaches to a plane, the rest of
 * Linux's drm_atomic_plane_set_property. 0, or -EINVAL for a rotation of
 * other than one angle or a property the plane does not have.
 */
static int32_t plane_set_optional(struct drm_plane_state *ps, struct drm_property *prop,
                                  uint64_t val)
{
    struct drm_plane *plane = ps->plane;
    uint32_t rot;

    if (prop == plane->alpha_property) {
        ps->alpha = (uint16_t)val;
    } else if (prop == plane->rotation_property) {
        rot = (uint32_t)val & DRM_MODE_ROTATE_MASK;
        if (rot == 0 || (rot & (rot - 1)) != 0) {
            return -EINVAL;
        }
        ps->rotation = (uint32_t)val;
    } else if (prop == plane->zpos_property) {
        ps->zpos = (uint32_t)val;
    } else if (prop == plane->color_encoding_property) {
        ps->color_encoding = (uint32_t)val;
    } else if (prop == plane->color_range_property) {
        ps->color_range = (uint32_t)val;
    } else if (prop == plane->hotspot_x_property) {
        ps->hotspot_x = (int32_t)(int64_t)val;
    } else if (prop == plane->hotspot_y_property) {
        ps->hotspot_y = (int32_t)(int64_t)val;
    } else {
        return -EINVAL;
    }
    return 0;
}

/** Set a plane property, Linux's drm_atomic_plane_set_property. */
static int32_t plane_set_property(struct drm_atomic_state *state, struct drm_file *file,
                                  struct drm_plane_state *ps, struct drm_property *prop,
                                  uint64_t val)
{
    struct drm_plane *plane = ps->plane;
    struct drm_device *dev = plane->dev;
    struct drm_mode_config *mc = &dev->mode_config;
    struct drm_framebuffer *fb;
    struct drm_crtc *crtc;
    int replaced = 0;

    if (prop == mc->prop_fb_id) {
        fb = drm_framebuffer_lookup(dev, file, (uint32_t)val);
        drm_atomic_set_fb_for_plane(ps, fb);
        drm_framebuffer_put(fb);
    } else if (prop == mc->prop_in_fence_fd) {
        if (ps->fence) {
            return -EINVAL;
        }
        if ((int64_t)val == -1) {
            return 0;
        }
        ps->fence = sync_file_get_fence((int32_t)val);
        if (!ps->fence) {
            return -EINVAL;
        }
    } else if (prop == mc->prop_crtc_id) {
        crtc = drm_crtc_find(dev, file, (uint32_t)val);
        if (val && !crtc) {
            return -EACCES;
        }
        return drm_atomic_set_crtc_for_plane(state, ps, crtc);
    } else if (prop == mc->prop_crtc_x) {
        ps->crtc_x = (int32_t)(int64_t)val;
    } else if (prop == mc->prop_crtc_y) {
        ps->crtc_y = (int32_t)(int64_t)val;
    } else if (prop == mc->prop_crtc_w) {
        ps->crtc_w = (uint32_t)val;
    } else if (prop == mc->prop_crtc_h) {
        ps->crtc_h = (uint32_t)val;
    } else if (prop == mc->prop_src_x) {
        ps->src_x = (uint32_t)val;
    } else if (prop == mc->prop_src_y) {
        ps->src_y = (uint32_t)val;
    } else if (prop == mc->prop_src_w) {
        ps->src_w = (uint32_t)val;
    } else if (prop == mc->prop_src_h) {
        ps->src_h = (uint32_t)val;
    } else if (prop == mc->prop_fb_damage_clips) {
        return drm_property_replace_blob_from_id(dev, &ps->fb_damage_clips, val, -1,
                                                 (int32_t)sizeof(struct drm_mode_rect), &replaced);
    } else {
        return plane_set_optional(ps, prop, val);
    }
    return 0;
}

/** Set a CRTC property, Linux's drm_atomic_crtc_set_property. */
static int32_t crtc_set_property(struct drm_crtc_state *cs, struct drm_property *prop, uint64_t val)
{
    struct drm_device *dev = cs->crtc->dev;
    struct drm_mode_config *mc = &dev->mode_config;
    struct drm_property_blob *blob;
    int32_t minus_one = -1, rc;
    int replaced = 0;

    if (prop == mc->prop_active) {
        cs->active = val != 0;
    } else if (prop == mc->prop_mode_id) {
        blob = val ? drm_property_lookup_blob(dev, (uint32_t)val) : NULL;
        rc = drm_atomic_set_mode_prop_for_crtc(cs, blob);
        drm_property_blob_put(blob);
        return rc;
    } else if (prop == mc->prop_vrr_enabled) {
        cs->vrr_enabled = val != 0;
    } else if (prop == mc->degamma_lut_property) {
        rc = drm_property_replace_blob_from_id(dev, &cs->degamma_lut, val, -1,
                                               (int32_t)sizeof(struct drm_color_lut), &replaced);
        cs->color_mgmt_changed |= replaced;
        return rc;
    } else if (prop == mc->ctm_property) {
        rc = drm_property_replace_blob_from_id(
            dev, &cs->ctm, val, (int32_t)sizeof(struct drm_color_ctm), -1, &replaced);
        cs->color_mgmt_changed |= replaced;
        return rc;
    } else if (prop == mc->gamma_lut_property) {
        rc = drm_property_replace_blob_from_id(dev, &cs->gamma_lut, val, -1,
                                               (int32_t)sizeof(struct drm_color_lut), &replaced);
        cs->color_mgmt_changed |= replaced;
        return rc;
    } else if (prop == mc->prop_out_fence_ptr) {
        if (val == 0) {
            return 0;
        }
        if (copy_to_user((void __user *)(uintptr_t)val, &minus_one, sizeof(minus_one)) != 0) {
            return -EFAULT;
        }
        cs->out_fence_ptr = (int32_t __user *)(uintptr_t)val;
    } else {
        return -EINVAL;
    }
    return 0;
}

/** Set a connector property, Linux's drm_atomic_connector_set_property. */
static int32_t connector_set_property(struct drm_atomic_state *state, struct drm_file *file,
                                      struct drm_connector_state *cs, struct drm_property *prop,
                                      uint64_t val)
{
    struct drm_device *dev = cs->connector->dev;
    struct drm_mode_config *mc = &dev->mode_config;
    struct drm_crtc *crtc;

    if (prop == mc->prop_crtc_id) {
        crtc = drm_crtc_find(dev, file, (uint32_t)val);
        if (val && !crtc) {
            return -EACCES;
        }
        return drm_atomic_set_crtc_for_connector(state, cs, crtc);
    }
    if (prop == mc->link_status_property) {
        /* Linux lets a program raise a link to good, never mark it bad, so
         * that a state restored after a VT switch cannot break the link. */
        if (cs->link_status != DRM_MODE_LINK_STATUS_GOOD) {
            cs->link_status = (uint32_t)val;
        }
        return 0;
    }
    if (prop == cs->connector->scaling_mode_property) {
        cs->scaling_mode = (uint32_t)val;
        return 0;
    }
    if (prop == cs->connector->max_bpc_property) {
        cs->max_requested_bpc = (uint32_t)val;
        return 0;
    }
    if (prop == cs->connector->audio_property) {
        cs->force_audio = (int32_t)(int64_t)val;
        return 0;
    }
    /* DPMS is the legacy call's alone; the rest are not settable. */
    return -EINVAL;
}

int32_t drm_atomic_set_property(struct drm_atomic_state *state, struct drm_file *file,
                                struct drm_mode_object *obj, struct drm_property *prop,
                                uint64_t value)
{
    struct drm_plane_state *ps;
    struct drm_crtc_state *cs;
    struct drm_connector_state *conn;

    if (!drm_property_change_valid(prop, value)) {
        return -EINVAL;
    }
    switch (obj->type) {
    case DRM_MODE_OBJECT_CONNECTOR:
        conn = drm_atomic_get_connector_state(state, (struct drm_connector *)obj);
        return conn ? connector_set_property(state, file, conn, prop, value) : -ENOMEM;
    case DRM_MODE_OBJECT_CRTC:
        cs = drm_atomic_get_crtc_state(state, (struct drm_crtc *)obj);
        return cs ? crtc_set_property(cs, prop, value) : -ENOMEM;
    case DRM_MODE_OBJECT_PLANE:
        ps = drm_atomic_get_plane_state(state, (struct drm_plane *)obj);
        return ps ? plane_set_property(state, file, ps, prop, value) : -ENOMEM;
    default:
        return -EINVAL;
    }
}

/** The value of a plane's property `prop` in its current state, Linux's
 * drm_atomic_plane_get_property. 0, or -EINVAL for a property it has
 * not. */
static int32_t plane_get_property(struct drm_plane *plane, struct drm_property *prop,
                                  uint64_t *value)
{
    struct drm_mode_config *mc = &plane->dev->mode_config;
    const struct drm_plane_state *ps = plane->state;

    if (prop == mc->prop_fb_id) {
        *value = ps->fb ? ps->fb->base.id : 0;
    } else if (prop == mc->prop_in_fence_fd) {
        *value = (uint64_t)(int64_t)-1;
    } else if (prop == mc->prop_crtc_id) {
        *value = ps->crtc ? ps->crtc->base.id : 0;
    } else if (prop == mc->prop_crtc_x) {
        *value = (uint64_t)(int64_t)ps->crtc_x;
    } else if (prop == mc->prop_crtc_y) {
        *value = (uint64_t)(int64_t)ps->crtc_y;
    } else if (prop == mc->prop_crtc_w) {
        *value = ps->crtc_w;
    } else if (prop == mc->prop_crtc_h) {
        *value = ps->crtc_h;
    } else if (prop == mc->prop_src_x) {
        *value = ps->src_x;
    } else if (prop == mc->prop_src_y) {
        *value = ps->src_y;
    } else if (prop == mc->prop_src_w) {
        *value = ps->src_w;
    } else if (prop == mc->prop_src_h) {
        *value = ps->src_h;
    } else if (prop == plane->alpha_property) {
        *value = ps->alpha;
    } else if (prop == plane->rotation_property) {
        *value = ps->rotation;
    } else if (prop == plane->zpos_property) {
        *value = ps->zpos;
    } else if (prop == plane->color_encoding_property) {
        *value = ps->color_encoding;
    } else if (prop == plane->color_range_property) {
        *value = ps->color_range;
    } else if (prop == mc->prop_fb_damage_clips) {
        *value = ps->fb_damage_clips ? ps->fb_damage_clips->base.id : 0;
    } else if (prop == plane->hotspot_x_property) {
        *value = (uint64_t)(int64_t)ps->hotspot_x;
    } else if (prop == plane->hotspot_y_property) {
        *value = (uint64_t)(int64_t)ps->hotspot_y;
    } else {
        return -EINVAL;
    }
    return 0;
}

int32_t drm_atomic_get_property(struct drm_mode_object *obj, struct drm_property *prop,
                                uint64_t *value)
{
    struct drm_device *dev = prop->dev;
    struct drm_mode_config *mc = &dev->mode_config;

    if (obj->type == DRM_MODE_OBJECT_PLANE) {
        return plane_get_property((struct drm_plane *)obj, prop, value);
    }
    if (obj->type == DRM_MODE_OBJECT_CRTC) {
        const struct drm_crtc_state *cs = ((struct drm_crtc *)obj)->state;

        if (prop == mc->prop_active) {
            *value = (uint64_t)cs->active;
        } else if (prop == mc->prop_mode_id) {
            *value = cs->mode_blob ? cs->mode_blob->base.id : 0;
        } else if (prop == mc->prop_vrr_enabled) {
            *value = (uint64_t)cs->vrr_enabled;
        } else if (prop == mc->degamma_lut_property) {
            *value = cs->degamma_lut ? cs->degamma_lut->base.id : 0;
        } else if (prop == mc->ctm_property) {
            *value = cs->ctm ? cs->ctm->base.id : 0;
        } else if (prop == mc->gamma_lut_property) {
            *value = cs->gamma_lut ? cs->gamma_lut->base.id : 0;
        } else if (prop == mc->prop_out_fence_ptr) {
            *value = 0;
        } else {
            return -EINVAL;
        }
        return 0;
    }
    if (obj->type == DRM_MODE_OBJECT_CONNECTOR) {
        const struct drm_connector *connector = (struct drm_connector *)obj;

        if (prop == mc->prop_crtc_id) {
            *value = connector->state->crtc ? connector->state->crtc->base.id : 0;
        } else if (prop == mc->dpms_property) {
            *value = (uint64_t)connector->dpms;
        } else if (prop == mc->link_status_property) {
            *value = connector->state->link_status;
        } else if (prop == connector->scaling_mode_property) {
            *value = connector->state->scaling_mode;
        } else if (prop == connector->max_bpc_property) {
            *value = connector->state->max_requested_bpc;
        } else if (prop == connector->audio_property) {
            *value = (uint64_t)(int64_t)connector->state->force_audio;
        } else {
            return -EINVAL;
        }
        return 0;
    }
    return -EINVAL;
}

/* ------------------------------------------------------------------------
 * The check
 */

/** The core's check of a plane's new state, Linux's drm_atomic_plane_check:
 * a CRTC and a framebuffer both or neither, a CRTC the plane can take, a
 * format it can show, coordinates that cannot overflow, a source inside
 * the framebuffer, damage inside it, and no direct move between CRTCs. */
static int32_t plane_check(const struct drm_plane_state *old, const struct drm_plane_state *ps)
{
    const struct drm_plane *plane = ps->plane;
    const struct drm_framebuffer *fb = ps->fb;
    const struct drm_mode_rect *clips;
    uint32_t fb_width, fb_height, n;

    if ((ps->crtc && !fb) || (fb && !ps->crtc)) {
        return -EINVAL;
    }
    if (!ps->crtc) {
        return 0;
    }
    if (!(plane->possible_crtcs & drm_crtc_mask(ps->crtc))) {
        return -EINVAL;
    }
    if (!drm_plane_has_format(plane, fb->format, fb->modifier)) {
        return -EINVAL;
    }
    if (ps->crtc_w > DRM_INT_MAX || ps->crtc_x > DRM_INT_MAX - (int32_t)ps->crtc_w ||
        ps->crtc_h > DRM_INT_MAX || ps->crtc_y > DRM_INT_MAX - (int32_t)ps->crtc_h) {
        return -ERANGE;
    }
    if (drm_framebuffer_check_src_coords(ps->src_x, ps->src_y, ps->src_w, ps->src_h, fb) != 0) {
        return -ENOSPC;
    }
    /* Linux compares the clips, in pixels, with the size in 16.16. */
    fb_width = fb->width << 16;
    fb_height = fb->height << 16;
    if (ps->fb_damage_clips) {
        clips = (const struct drm_mode_rect *)ps->fb_damage_clips->data;
        for (n = ps->fb_damage_clips->length / sizeof(*clips); n > 0; n--, clips++) {
            if (clips->x1 >= clips->x2 || clips->y1 >= clips->y2 || clips->x1 < 0 ||
                clips->y1 < 0 || (uint32_t)clips->x2 > fb_width ||
                (uint32_t)clips->y2 > fb_height) {
                return -EINVAL;
            }
        }
    }
    if (old->crtc && ps->crtc && old->crtc != ps->crtc) {
        return -EINVAL;
    }
    return 0;
}

/** The core's check of a CRTC's new state, Linux's drm_atomic_crtc_check. */
static int32_t crtc_check(const struct drm_crtc_state *old, const struct drm_crtc_state *cs)
{
    if (cs->active && !cs->enable) {
        return -EINVAL;
    }
    if ((cs->enable && !cs->mode_blob) || (!cs->enable && cs->mode_blob)) {
        return -EINVAL;
    }
    /* An event on a CRTC that stays off would never come. */
    if (cs->event && !cs->active && !old->active) {
        return -EINVAL;
    }
    return 0;
}

int32_t drm_atomic_check_only(struct drm_atomic_state *state)
{
    struct drm_device *dev = state->dev;
    int32_t rc;
    int i;

    for (i = 0; i < DRM_MAX_PLANES; i++) {
        if (state->planes_new[i] &&
            (rc = plane_check(state->planes_old[i], state->planes_new[i])) != 0) {
            return rc;
        }
    }
    for (i = 0; i < DRM_MAX_CRTCS; i++) {
        if (state->crtcs_new[i] &&
            (rc = crtc_check(state->crtcs_old[i], state->crtcs_new[i])) != 0) {
            return rc;
        }
    }
    rc = dev->mode_config.atomic_check ? dev->mode_config.atomic_check(dev, state)
                                       : drm_atomic_helper_check(dev, state);
    if (rc != 0) {
        return rc;
    }
    if (!state->allow_modeset) {
        for (i = 0; i < DRM_MAX_CRTCS; i++) {
            if (state->crtcs_new[i] && drm_atomic_crtc_needs_modeset(state->crtcs_new[i])) {
                return -EINVAL;
            }
        }
    }
    return 0;
}

/** The encoder that drives `connector`, Linux's
 * drm_connector_get_single_encoder: the first it may use, or NULL. */
static struct drm_encoder *single_encoder(const struct drm_connector *connector)
{
    const struct drm_mode_config *mc = &connector->dev->mode_config;
    uint32_t i;

    for (i = 0; i < mc->num_encoder; i++) {
        if (connector->possible_encoders & (1u << i)) {
            return mc->encoders[i];
        }
    }
    return NULL;
}

/** Route a connector's new state, Linux's update_connector_routing: mark
 * the CRTCs it leaves and joins, and choose its encoder. 0 or -EINVAL. */
static int32_t connector_routing(struct drm_atomic_state *state,
                                 const struct drm_connector_state *old,
                                 struct drm_connector_state *cs)
{
    struct drm_encoder *encoder;

    if (old->crtc != cs->crtc) {
        if (old->crtc) {
            state->crtcs_new[old->crtc->index]->connectors_changed = 1;
        }
        if (cs->crtc) {
            state->crtcs_new[cs->crtc->index]->connectors_changed = 1;
        }
    }
    if (!cs->crtc) {
        cs->best_encoder = NULL;
        return 0;
    }
    encoder = single_encoder(cs->connector);
    if (!encoder) {
        return -EINVAL;
    }
    if (encoder->possible_crtcs && !(encoder->possible_crtcs & drm_crtc_mask(cs->crtc))) {
        return -EINVAL;
    }
    if (encoder != cs->best_encoder) {
        cs->best_encoder = encoder;
        state->crtcs_new[cs->crtc->index]->connectors_changed = 1;
    }
    return 0;
}

/**
 * Linux's handle_conflicting_encoders: two connectors of `state` may not
 * take the one encoder each can use, nor one take an encoder a connector
 * the commit leaves out holds, unless `disable`, as for a legacy mode set,
 * takes the encoder from that connector and turns its CRTC off when no
 * connector is left there. 0, -EINVAL or -ENOMEM.
 */
static int32_t conflicting_encoders(struct drm_atomic_state *state, int disable)
{
    const struct drm_mode_config *mc = &state->dev->mode_config;
    struct drm_connector_state *conn;
    struct drm_connector *connector;
    struct drm_crtc_state *cs;
    struct drm_encoder *encoder;
    uint32_t used = 0, bit, i;
    int32_t rc;

    for (i = 0; i < DRM_MAX_CONNECTORS; i++) {
        conn = state->connectors_new[i];
        if (!conn || !conn->crtc || !single_encoder(conn->connector)) {
            continue;
        }
        bit = 1u << single_encoder(conn->connector)->index;
        if (used & bit) {
            return -EINVAL;
        }
        used |= bit;
    }
    for (i = 0; used && i < mc->num_connector; i++) {
        connector = mc->connectors[i];
        encoder = connector->state->best_encoder;
        if (state->connectors_new[connector->index] || !encoder ||
            !(used & (1u << encoder->index))) {
            continue;
        }
        if (!disable) {
            return -EINVAL;
        }
        conn = drm_atomic_get_connector_state(state, connector);
        if (!conn) {
            return -ENOMEM;
        }
        cs = conn->crtc ? drm_atomic_get_crtc_state(state, conn->crtc) : NULL;
        conn->best_encoder = NULL;
        if ((rc = drm_atomic_set_crtc_for_connector(state, conn, NULL)) != 0) {
            return rc;
        }
        if (cs && !cs->connector_mask) {
            if ((rc = drm_atomic_set_mode_prop_for_crtc(cs, NULL)) != 0) {
                return rc;
            }
            cs->active = 0;
        }
    }
    return 0;
}

/** Linux's drm_atomic_helper_check_modeset: the modeset marks, the
 * routing, the connectors and planes a modeset takes with it, and the
 * CRTCs' `mode_valid` on a changed mode. */
static int32_t check_modeset(struct drm_device *dev, struct drm_atomic_state *state)
{
    struct drm_crtc_state *old, *cs;
    struct drm_connector_state *conn;
    struct drm_crtc *crtc;
    int32_t rc;
    int i;

    for (i = 0; i < DRM_MAX_CRTCS; i++) {
        cs = state->crtcs_new[i];
        old = state->crtcs_old[i];
        if (!cs) {
            continue;
        }
        if (!drm_mode_equal(&old->mode, &cs->mode)) {
            cs->mode_changed = 1;
        }
        if (old->enable != cs->enable) {
            cs->mode_changed = 1;
            cs->connectors_changed = 1;
        }
        if (old->active != cs->active) {
            cs->active_changed = 1;
        }
        if (cs->enable != (cs->connector_mask != 0)) {
            return -EINVAL;
        }
    }
    if ((rc = conflicting_encoders(state, 0)) != 0) {
        return rc;
    }
    for (i = 0; i < DRM_MAX_CONNECTORS; i++) {
        conn = state->connectors_new[i];
        if (!conn) {
            continue;
        }
        rc = connector_routing(state, state->connectors_old[i], conn);
        if (rc != 0) {
            return rc;
        }
        crtc = state->connectors_old[i]->crtc;
        if (crtc && (state->connectors_old[i]->link_status != conn->link_status ||
                     state->connectors_old[i]->max_requested_bpc != conn->max_requested_bpc)) {
            state->crtcs_new[crtc->index]->connectors_changed = 1;
        }
        /* i915's intel_digital_connector_atomic_check: a new "audio" is a
         * modeset of the CRTC the connector is on. */
        if (conn->crtc && state->crtcs_new[conn->crtc->index] &&
            state->connectors_old[i]->force_audio != conn->force_audio) {
            state->crtcs_new[conn->crtc->index]->mode_changed = 1;
        }
    }
    for (i = 0; i < (int)dev->mode_config.num_crtc; i++) {
        cs = state->crtcs_new[i];
        if (!cs || !drm_atomic_crtc_needs_modeset(cs)) {
            continue;
        }
        if ((rc = drm_atomic_add_affected_connectors(state, cs->crtc)) != 0 ||
            (rc = drm_atomic_add_affected_planes(state, cs->crtc)) != 0) {
            return rc;
        }
    }
    for (i = 0; i < DRM_MAX_CONNECTORS; i++) {
        conn = state->connectors_new[i];
        if (!conn || !conn->crtc || !conn->best_encoder) {
            continue;
        }
        cs = state->crtcs_new[conn->crtc->index];
        if (!cs || (!cs->mode_changed && !cs->connectors_changed)) {
            continue;
        }
        crtc = conn->crtc;
        if (crtc->funcs && crtc->funcs->mode_valid &&
            crtc->funcs->mode_valid(crtc, &cs->mode) != 0) {
            return -EINVAL;
        }
    }
    return 0;
}

int32_t drm_atomic_helper_check(struct drm_device *dev, struct drm_atomic_state *state)
{
    int32_t rc = check_modeset(dev, state);
    int i;

    if (rc != 0) {
        return rc;
    }
    /* Linux's drm_atomic_helper_check_planes. */
    for (i = 0; i < DRM_MAX_PLANES; i++) {
        struct drm_plane_state *ps = state->planes_new[i];

        if (ps && ps->plane->funcs && ps->plane->funcs->atomic_check &&
            (rc = ps->plane->funcs->atomic_check(ps->plane, state)) != 0) {
            return rc;
        }
    }
    for (i = 0; i < DRM_MAX_CRTCS; i++) {
        struct drm_crtc_state *cs = state->crtcs_new[i];

        if (cs && cs->crtc->funcs && cs->crtc->funcs->atomic_check &&
            (rc = cs->crtc->funcs->atomic_check(cs->crtc, state)) != 0) {
            return rc;
        }
    }
    return 0;
}

/* ---- Rectangles, Linux's drm_rect.c ---- */

/** Turn `r`, in a buffer `width` by `height`, by `rotation`, Linux's
 * drm_rect_rotate. */
static void rect_rotate(struct drm_rect *r, int32_t width, int32_t height, uint32_t rotation)
{
    struct drm_rect t;

    if (rotation & DRM_MODE_REFLECT_MASK) {
        t = *r;
        if (rotation & DRM_MODE_REFLECT_X) {
            r->x1 = width - t.x2;
            r->x2 = width - t.x1;
        }
        if (rotation & DRM_MODE_REFLECT_Y) {
            r->y1 = height - t.y2;
            r->y2 = height - t.y1;
        }
    }
    t = *r;
    switch (rotation & DRM_MODE_ROTATE_MASK) {
    case DRM_MODE_ROTATE_90:
        r->x1 = t.y1;
        r->x2 = t.y2;
        r->y1 = width - t.x2;
        r->y2 = width - t.x1;
        break;
    case DRM_MODE_ROTATE_180:
        r->x1 = width - t.x2;
        r->x2 = width - t.x1;
        r->y1 = height - t.y2;
        r->y2 = height - t.y1;
        break;
    case DRM_MODE_ROTATE_270:
        r->x1 = height - t.y2;
        r->x2 = height - t.y1;
        r->y1 = t.x1;
        r->y2 = t.x2;
        break;
    default:
        break;
    }
}

/** Undo `rect_rotate`, Linux's drm_rect_rotate_inv. */
static void rect_rotate_inv(struct drm_rect *r, int32_t width, int32_t height, uint32_t rotation)
{
    struct drm_rect t = *r;

    switch (rotation & DRM_MODE_ROTATE_MASK) {
    case DRM_MODE_ROTATE_90:
        r->x1 = width - t.y2;
        r->x2 = width - t.y1;
        r->y1 = t.x1;
        r->y2 = t.x2;
        break;
    case DRM_MODE_ROTATE_180:
        r->x1 = width - t.x2;
        r->x2 = width - t.x1;
        r->y1 = height - t.y2;
        r->y2 = height - t.y1;
        break;
    case DRM_MODE_ROTATE_270:
        r->x1 = t.y1;
        r->x2 = t.y2;
        r->y1 = height - t.x2;
        r->y2 = height - t.x1;
        break;
    default:
        break;
    }
    if (rotation & DRM_MODE_REFLECT_MASK) {
        t = *r;
        if (rotation & DRM_MODE_REFLECT_X) {
            r->x1 = width - t.x2;
            r->x2 = width - t.x1;
        }
        if (rotation & DRM_MODE_REFLECT_Y) {
            r->y1 = height - t.y2;
            r->y2 = height - t.y1;
        }
    }
}

/** The scale of `src` to `dst` in 16.16, rounded up when it shrinks,
 * Linux's drm_calc_scale. */
static int32_t calc_scale(int32_t src, int32_t dst)
{
    if (src < 0 || dst < 0) {
        return -EINVAL;
    }
    if (dst == 0) {
        return 0;
    }
    if (src > (int32_t)((uint32_t)dst << 16)) {
        return (src + dst - 1) / dst;
    }
    return src / dst;
}

/** Linux's drm_rect_calc_hscale and _vscale: the scale, -ERANGE outside
 * `min` to `max`. */
static int32_t rect_scale(int32_t src, int32_t dst, int32_t min, int32_t max)
{
    int32_t scale = calc_scale(src, dst);

    if (scale < 0 || dst == 0) {
        return scale;
    }
    return scale < min || scale > max ? -ERANGE : scale;
}

/** Linux's clip_scaled: the source length left when `clip` of `dst` goes,
 * rounded toward a scale of 1. */
static uint32_t clip_scaled(int32_t src, int32_t dst, int32_t *clip)
{
    uint64_t t;

    if (dst == 0) {
        return 0;
    }
    if (*clip > dst) {
        *clip = dst;
    }
    t = (uint64_t)(uint32_t)src * (uint32_t)(dst - *clip);
    if (src < (int32_t)((uint32_t)dst << 16)) {
        return (uint32_t)drm_div64(t + (uint32_t)dst - 1, (uint32_t)dst);
    }
    return (uint32_t)drm_div64(t, (uint32_t)dst);
}

/** `a` - `b` wrapped to 32 bits, as Linux's kernel, built with
 * -fno-strict-overflow, computes a rectangle's sides. */
static int32_t wrap_sub(int32_t a, int32_t b)
{
    return (int32_t)((uint32_t)a - (uint32_t)b);
}

/**
 * Clip `dst` to `clip` and `src` with it, Linux's drm_rect_clip_scaled;
 * whether anything is left, its drm_rect_visible. The differences wrap as
 * Linux's do, so that a rectangle at INT_MIN, whose distance to the clip
 * overflows, is left as wide as the overflow says and is not visible.
 */
static int rect_clip_scaled(struct drm_rect *src, struct drm_rect *dst, const struct drm_rect *clip)
{
    int32_t diff;

    diff = wrap_sub(clip->x1, dst->x1);
    if (diff > 0) {
        src->x1 = src->x2 - (int32_t)clip_scaled(wrap_sub(src->x2, src->x1),
                                                 wrap_sub(dst->x2, dst->x1), &diff);
        dst->x1 = clip->x1;
    }
    diff = wrap_sub(clip->y1, dst->y1);
    if (diff > 0) {
        src->y1 = src->y2 - (int32_t)clip_scaled(wrap_sub(src->y2, src->y1),
                                                 wrap_sub(dst->y2, dst->y1), &diff);
        dst->y1 = clip->y1;
    }
    diff = wrap_sub(dst->x2, clip->x2);
    if (diff > 0) {
        src->x2 = src->x1 + (int32_t)clip_scaled(wrap_sub(src->x2, src->x1),
                                                 wrap_sub(dst->x2, dst->x1), &diff);
        dst->x2 = clip->x2;
    }
    diff = wrap_sub(dst->y2, clip->y2);
    if (diff > 0) {
        src->y2 = src->y1 + (int32_t)clip_scaled(wrap_sub(src->y2, src->y1),
                                                 wrap_sub(dst->y2, dst->y1), &diff);
        dst->y2 = clip->y2;
    }
    return wrap_sub(dst->x2, dst->x1) > 0 && wrap_sub(dst->y2, dst->y1) > 0;
}

int32_t drm_atomic_helper_check_plane_state(struct drm_plane_state *ps,
                                            const struct drm_crtc_state *crtc_state,
                                            int32_t min_scale, int32_t max_scale, int can_position,
                                            int can_update_disabled)
{
    const struct drm_framebuffer *fb = ps->fb;
    struct drm_rect clip = {0, 0, 0, 0};
    int32_t fw, fh;

    ps->src.x1 = (int32_t)ps->src_x;
    ps->src.y1 = (int32_t)ps->src_y;
    ps->src.x2 = (int32_t)(ps->src_x + ps->src_w);
    ps->src.y2 = (int32_t)(ps->src_y + ps->src_h);
    ps->dst.x1 = ps->crtc_x;
    ps->dst.y1 = ps->crtc_y;
    ps->dst.x2 = ps->crtc_x + (int32_t)ps->crtc_w;
    ps->dst.y2 = ps->crtc_y + (int32_t)ps->crtc_h;
    if (!fb || !ps->crtc) {
        ps->visible = 0;
        return 0;
    }
    if (!crtc_state->enable && !can_update_disabled) {
        return -EINVAL;
    }
    fw = (int32_t)(fb->width << 16);
    fh = (int32_t)(fb->height << 16);
    rect_rotate(&ps->src, fw, fh, ps->rotation);
    if (rect_scale(ps->src.x2 - ps->src.x1, ps->dst.x2 - ps->dst.x1, min_scale, max_scale) < 0 ||
        rect_scale(ps->src.y2 - ps->src.y1, ps->dst.y2 - ps->dst.y1, min_scale, max_scale) < 0) {
        return -ERANGE;
    }
    if (crtc_state->enable) {
        uint32_t w, h;

        drm_mode_get_hv_timing(&crtc_state->mode, &w, &h);
        clip.x2 = (int32_t)w;
        clip.y2 = (int32_t)h;
    }
    ps->visible = rect_clip_scaled(&ps->src, &ps->dst, &clip);
    rect_rotate_inv(&ps->src, fw, fh, ps->rotation);
    if (!ps->visible) {
        return 0;
    }
    if (!can_position && (ps->dst.x1 != clip.x1 || ps->dst.y1 != clip.y1 || ps->dst.x2 != clip.x2 ||
                          ps->dst.y2 != clip.y2)) {
        return -EINVAL;
    }
    return 0;
}

/* ------------------------------------------------------------------------
 * The commit
 */

/** Make the commit's states the objects', Linux's
 * drm_atomic_helper_swap_state: afterwards `*_old` are the states the
 * commit owns and frees, and `*_new` the objects'. */
static void swap_state(struct drm_atomic_state *state)
{
    struct drm_mode_config *mc = &state->dev->mode_config;
    uint32_t i;

    for (i = 0; i < mc->num_connector; i++) {
        if (state->connectors_new[i]) {
            mc->connectors[i]->state = state->connectors_new[i];
        }
    }
    for (i = 0; i < mc->num_crtc; i++) {
        if (state->crtcs_new[i]) {
            mc->crtcs[i]->state = state->crtcs_new[i];
        }
    }
    for (i = 0; i < mc->num_plane; i++) {
        if (state->planes_new[i]) {
            mc->planes[i]->state = state->planes_new[i];
        }
    }
}

/** Linux's drm_atomic_helper_commit_modeset_disables: turn off each CRTC
 * that goes off or changes its mode, then set the legacy links and DPMS. */
static void commit_disables(struct drm_atomic_state *state)
{
    struct drm_mode_config *mc = &state->dev->mode_config;
    uint32_t i;

    for (i = 0; i < mc->num_crtc; i++) {
        struct drm_crtc *crtc = mc->crtcs[i];
        struct drm_crtc_state *cs = state->crtcs_new[i];

        if (!cs || !drm_atomic_crtc_needs_modeset(cs) || !state->crtcs_old[i]->active) {
            continue;
        }
        if (crtc->funcs && crtc->funcs->atomic_disable) {
            crtc->funcs->atomic_disable(crtc, state);
        }
        if (drm_crtc_has_vblank(crtc)) {
            drm_crtc_vblank_off(crtc);
        }
    }
    for (i = 0; i < mc->num_connector; i++) {
        struct drm_connector *connector = mc->connectors[i];
        struct drm_connector_state *cs = state->connectors_new[i];
        struct drm_crtc *crtc;

        if (!cs) {
            continue;
        }
        if (connector->encoder) {
            connector->encoder->crtc = NULL;
            connector->encoder = NULL;
        }
        crtc = cs->crtc;
        if ((!crtc && state->connectors_old[i]->crtc) ||
            (crtc && drm_atomic_crtc_needs_modeset(crtc->state))) {
            connector->dpms = crtc && crtc->state->active ? DRM_MODE_DPMS_ON : DRM_MODE_DPMS_OFF;
        }
        if (crtc && cs->best_encoder) {
            connector->encoder = cs->best_encoder;
            connector->encoder->crtc = crtc;
        }
    }
}

/** Linux's drm_atomic_helper_commit_planes: each CRTC's begin, each plane's
 * update or disable, each CRTC's flush. */
static void commit_planes(struct drm_atomic_state *state)
{
    int i;

    for (i = 0; i < DRM_MAX_CRTCS; i++) {
        struct drm_crtc_state *cs = state->crtcs_new[i];

        if (cs && cs->crtc->funcs && cs->crtc->funcs->atomic_begin) {
            cs->crtc->funcs->atomic_begin(cs->crtc, state);
        }
    }
    for (i = 0; i < DRM_MAX_PLANES; i++) {
        struct drm_plane_state *ps = state->planes_new[i];
        const struct drm_plane_funcs *funcs;
        int disabling;

        if (!ps || !(funcs = ps->plane->funcs)) {
            continue;
        }
        disabling = state->planes_old[i]->crtc && !ps->crtc;
        if (disabling && funcs->atomic_disable) {
            funcs->atomic_disable(ps->plane, state);
        } else if ((ps->crtc || disabling) && funcs->atomic_update) {
            funcs->atomic_update(ps->plane, state);
        }
    }
    for (i = 0; i < DRM_MAX_CRTCS; i++) {
        struct drm_crtc_state *cs = state->crtcs_new[i];

        if (cs && cs->crtc->funcs && cs->crtc->funcs->atomic_flush) {
            cs->crtc->funcs->atomic_flush(cs->crtc, state);
        }
    }
}

/** Linux's drm_atomic_helper_commit_modeset_enables: turn on each CRTC that
 * comes on or changed its mode. */
static void commit_enables(struct drm_atomic_state *state)
{
    int i;

    for (i = 0; i < DRM_MAX_CRTCS; i++) {
        struct drm_crtc_state *cs = state->crtcs_new[i];

        if (!cs || !drm_atomic_crtc_needs_modeset(cs) || !cs->active || !cs->enable) {
            continue;
        }
        if (cs->crtc->funcs && cs->crtc->funcs->atomic_enable) {
            cs->crtc->funcs->atomic_enable(cs->crtc, state);
        }
        if (drm_crtc_has_vblank(cs->crtc)) {
            drm_crtc_vblank_on(cs->crtc);
        }
    }
}

/** Arm each CRTC's event for the vblank after the commit, as Linux's
 * drivers do in `atomic_flush`, or send it now on a CRTC without vblanks
 * or that is off, as drm_atomic_helper_fake_vblank and the drivers'
 * `atomic_disable` do. An unplugged device's out-fences signal -ENODEV. */
static void commit_signal(struct drm_atomic_state *state)
{
    int i;

    for (i = 0; i < DRM_MAX_CRTCS; i++) {
        struct drm_crtc_state *cs = state->crtcs_new[i];
        struct drm_pending_event *e;

        if (!cs || !(e = cs->event)) {
            continue;
        }
        cs->event = NULL;
        if (e->flip_crtc && cs->crtc->flip_pending == DRM_FLIP_QUEUED) {
            cs->crtc->flip_pending = DRM_FLIP_ARMED;
            wq_wake_all(&cs->crtc->vblank.wait);
        }
        if (state->dev->unplugged) {
            dma_fence_set_error(e->fence, -ENODEV);
        }
        if (cs->active && drm_crtc_vblank_get(cs->crtc) == 0) {
            drm_crtc_arm_vblank_event(cs->crtc, e);
        } else {
            drm_crtc_send_vblank_event(cs->crtc, e);
        }
    }
}

/** The commit's tail, Linux's commit_tail after the swap: the in-fences let
 * go, the CRTCs and planes programmed, the events armed, and the old
 * states freed, which leaves `state` holding nothing. May sleep in the
 * drivers' hooks. */
static void commit_tail(struct drm_atomic_state *state)
{
    struct drm_device *dev = state->dev;
    int i;

    /* The hardware's part, with the device awake. */
    drm_dev_pm_get(dev);
    for (i = 0; i < DRM_MAX_PLANES; i++) {
        struct drm_plane_state *ps = state->planes_new[i];

        if (ps && ps->fence) {
            dma_fence_put(ps->fence);
            ps->fence = NULL;
        }
    }
    commit_disables(state);
    commit_planes(state);
    commit_enables(state);
    commit_signal(state);
    drm_dev_pm_put(dev);
    /* The objects own the new states now; the commit owns the old ones. */
    for (i = 0; i < DRM_MAX_PLANES; i++) {
        if (state->planes_new[i]) {
            drm_plane_state_destroy(state->planes_old[i]);
            state->planes_old[i] = state->planes_new[i] = NULL;
        }
    }
    for (i = 0; i < DRM_MAX_CRTCS; i++) {
        if (state->crtcs_new[i]) {
            drm_crtc_state_destroy(state->crtcs_old[i]);
            state->crtcs_old[i] = state->crtcs_new[i] = NULL;
        }
    }
    for (i = 0; i < DRM_MAX_CONNECTORS; i++) {
        if (state->connectors_new[i]) {
            kfree(state->connectors_old[i]);
            state->connectors_old[i] = state->connectors_new[i] = NULL;
        }
    }
}

/* ------------------------------------------------------------------------
 * The commit worker
 */

/*
 * A nonblocking commit whose in-fences had not all signalled, which the
 * worker, or a blocking commit of the same objects, finishes once they
 * have: the commit's states, moved from the caller's after the swap, a
 * callback on the current plane's fence, whether it has yet to signal,
 * the last plane reached (`waited`, -1 before the first wait and
 * DRM_MAX_PLANES after the last), the tick at which that wait times out
 * under the driver's `commit_fence_timeout_ms` (`deadline`, 0 for none),
 * whether its tail has begun, and a hold on the device. The queue keeps
 * commit order, and a work stays on it until its tail ends.
 */
struct commit_work;
struct commit_fence_cb {
    struct dma_fence_cb cb; /* first, so that the callback finds its record */
    struct commit_work *work;
};
struct commit_work {
    struct drm_atomic_state state;
    struct commit_fence_cb cbs[DRM_MAX_PLANES];
    uint32_t waiting;
    int waited;
    uint64_t deadline;
    int timed_out; /* once set, no later plane is waited for */
    int running;
    struct commit_work *next;
};

/*
 * `work_lock` keeps the three queues below (`works`, `hotplugs` and
 * `dev_works`), their counts and each queued commit's wait state
 * (`waiting`, `waited`, `deadline`, `timed_out`, `running`), against the
 * fence callbacks and the interrupt handlers that queue works, on any
 * processor. It is taken with interrupts off, and no fence is called and
 * no queue woken under it, since a fence's enabling takes the signaller's
 * locks, under which the callbacks run. A queued commit's waits advance
 * and its tail runs only under its device's commit lock, whoever runs
 * them, the worker or a blocking commit of the same objects, so that one
 * at a time does.
 */
static struct spinlock work_lock = SPINLOCK_INIT_NAMED("drm_work_lock");
static struct commit_work *works;
/* The worker sleeps here; static storage starts it empty. */
static wait_queue_t work_wait;
/* A commit stalled on a queued one sleeps here, woken as a queued
 * commit's fences signal or its tail ends; static storage starts it
 * empty. */
static wait_queue_t work_done_wait;

static void work_fence_done(struct dma_fence *fence, struct dma_fence_cb *cb);

/**
 * Arm `w`'s wait on plane `i`'s fence, `fence`: the wait recorded first,
 * since the callback may run at once on another processor and must find
 * it, then the callback added and the fence handed to the driver's
 * `commit_fence_wait`. 1 once armed, or 0 when the fence had signalled.
 */
static int work_fence_arm(struct commit_work *w, int i, struct dma_fence *fence)
{
    const struct drm_mode_config *mc = &w->state.dev->mode_config;
    uint32_t flags;

    w->cbs[i].work = w;
    flags = spin_lock_irqsave(&work_lock);
    w->waiting = 1;
    w->waited = i;
    w->deadline = mc->commit_fence_timeout_ms
                      ? rtc_ticks_now() + MS_TO_TICKS(mc->commit_fence_timeout_ms) + 1u
                      : 0;
    spin_unlock_irqrestore(&work_lock, flags);
    if (dma_fence_add_callback(fence, &w->cbs[i].cb, work_fence_done) != 0) {
        return 0;
    }
    if (mc->commit_fence_wait) {
        mc->commit_fence_wait(w->state.dev, fence);
    }
    return 1;
}

/**
 * Move `w`'s tail to the first plane after `from` whose fence has not
 * signalled, arm only that fence's callback, hand it to the driver's
 * `commit_fence_wait` and start its timeout, as Linux's
 * intel_atomic_commit_fence_wait (intel_display.c:7262-7280) waits for
 * each plane's fence in turn, at the start of the tail that Linux queues
 * at once. Process context under the device's commit lock, without
 * `work_lock`; the previous callback is no longer armed.
 */
static void work_fence_next(struct commit_work *w, int from)
{
    uint32_t flags;
    int i;

    for (i = from + 1; i < DRM_MAX_PLANES; i++) {
        struct drm_plane_state *ps = w->state.planes_new[i];

        if (ps && ps->fence && work_fence_arm(w, i, ps->fence)) {
            return;
        }
    }
    flags = spin_lock_irqsave(&work_lock);
    w->waiting = 0;
    w->waited = DRM_MAX_PLANES;
    w->deadline = 0;
    spin_unlock_irqrestore(&work_lock, flags);
}

/** Whether `w` may advance its waits or run its tail at tick `now`: it has
 * not begun the tail, and no callback is armed or the current wait timed
 * out, after which Linux's tail waits for no other plane. `work_lock` is
 * held. */
static int work_due_locked(const struct commit_work *w, uint64_t now)
{
    return !w->running && (!w->waiting || (w->deadline && now >= w->deadline));
}

/** `work_due_locked` under `work_lock`. */
static int work_due(const struct commit_work *w, uint64_t now)
{
    uint32_t flags = spin_lock_irqsave(&work_lock);
    int due = work_due_locked(w, now);

    spin_unlock_irqrestore(&work_lock, flags);
    return due;
}

/** Record completion and whether it missed the deadline, then wake the
 * worker to advance in process context without the signaller's locks.
 * dma_fence_signal runs this with interrupts masked, perhaps under the
 * signaller's locks and without the big lock. */
static void work_fence_done(struct dma_fence *fence, struct dma_fence_cb *cb)
{
    struct commit_fence_cb *fcb = (struct commit_fence_cb *)cb;
    struct commit_work *w = fcb->work;
    uint32_t flags = spin_lock_irqsave(&work_lock);

    (void)fence;
    w->timed_out = w->deadline && rtc_ticks_now() >= w->deadline;
    w->deadline = 0;
    w->waiting = 0;
    spin_unlock_irqrestore(&work_lock, flags);
    wq_wake_all(&work_wait);
    wq_wake_all(&work_done_wait);
}

/** Advance a runnable commit's waits in process context, under its
 * device's commit lock and without driver locks. Return 1 once every wait
 * is over or one timed out, 0 while the current callback is armed. */
static int work_fence_progress(struct commit_work *w)
{
    uint32_t flags;
    int from, done;

    for (;;) {
        flags = spin_lock_irqsave(&work_lock);
        if (w->deadline && rtc_ticks_now() >= w->deadline) {
            w->timed_out = 1;
        }
        if (w->timed_out || w->waiting || w->waited >= DRM_MAX_PLANES) {
            done = w->timed_out || w->waited >= DRM_MAX_PLANES;
            spin_unlock_irqrestore(&work_lock, flags);
            return done;
        }
        from = w->waited;
        spin_unlock_irqrestore(&work_lock, flags);
        work_fence_next(w, from);
    }
}

/* The connectors whose hotplug event waits for the worker, each with a
 * hold on its device, under `work_lock`; static storage starts it
 * empty. */
#define HOTPLUG_QUEUE 8
static struct drm_connector *hotplugs[HOTPLUG_QUEUE];
static int nhotplugs;

static int32_t commit_worker_start(void);

void drm_connector_hotplug_later(struct drm_connector *connector)
{
    uint32_t flags = spin_lock_irqsave(&work_lock);
    int i;

    for (i = 0; i < nhotplugs; i++) {
        if (hotplugs[i] == connector) {
            spin_unlock_irqrestore(&work_lock, flags);
            return;
        }
    }
    if (nhotplugs == HOTPLUG_QUEUE) {
        spin_unlock_irqrestore(&work_lock, flags);
        klog(KLOG_WARN, "drm", "%s: hotplug event lost: the queue is full", connector->name);
        return;
    }
    drm_dev_get(connector->dev);
    hotplugs[nhotplugs++] = connector;
    spin_unlock_irqrestore(&work_lock, flags);
    wq_wake_all(&work_wait);
}

/* The drivers' works that wait for the worker, in the order queued, under
 * `work_lock`; static storage starts it empty. */
static struct drm_work *dev_works, **dev_works_tail = &dev_works;

/* Counts the works queued, the drivers' and the commits', so that the
 * worker's wait ends on a new one whose time may come before the deadline
 * it armed. */
static uint32_t dev_works_gen;

void drm_dev_work_later_ms(struct drm_device *dev, struct drm_work *work, uint32_t ms)
{
    uint64_t due = ms ? rtc_ticks_now() + MS_TO_TICKS(ms) + 1u : 0;
    uint32_t flags = spin_lock_irqsave(&work_lock);

    if (work->queued) {
        if (work->due > due) {
            work->due = due;
            dev_works_gen++;
        }
        spin_unlock_irqrestore(&work_lock, flags);
        if (due == 0) {
            wq_wake_all(&work_wait);
        }
        return;
    }
    drm_dev_get(dev);
    work->dev = dev;
    work->next = NULL;
    work->queued = 1;
    work->due = due;
    *dev_works_tail = work;
    dev_works_tail = &work->next;
    dev_works_gen++;
    spin_unlock_irqrestore(&work_lock, flags);
    wq_wake_all(&work_wait);
}

void drm_dev_work_later(struct drm_device *dev, struct drm_work *work)
{
    drm_dev_work_later_ms(dev, work, 0);
}

/** Whether `work`'s time has come. */
static int dev_work_due(const struct drm_work *work, uint64_t now)
{
    return work->due == 0 || work->due <= now;
}

/** Unlink `work` from the queue; the caller holds `work_lock` and `work`
 * is queued. */
static void dev_work_unlink(struct drm_work *work)
{
    struct drm_work **link = &dev_works;

    while (*link != work) {
        link = &(*link)->next;
    }
    *link = work->next;
    if (dev_works_tail == &work->next) {
        dev_works_tail = link;
    }
    work->queued = 0;
}

int drm_dev_work_cancel(struct drm_work *work)
{
    uint32_t flags = spin_lock_irqsave(&work_lock);
    struct drm_device *dev = work->dev;

    if (!work->queued) {
        spin_unlock_irqrestore(&work_lock, flags);
        return 0;
    }
    dev_work_unlink(work);
    spin_unlock_irqrestore(&work_lock, flags);
    drm_dev_put(dev);
    return 1;
}

#ifndef RACH2_HOSTTEST
/** Take the big lock in the worker, which runs without it, for code that
 * still needs it, when `needed`. */
static void worker_bkl_enter(int needed)
{
    if (needed) {
        bkl_enter();
    }
}

/** Give back what `worker_bkl_enter` took with the same `needed`. */
static void worker_bkl_exit(int needed)
{
    if (needed) {
        bkl_exit();
    }
}
#else
int drm_worker_bkl_takes;

/** Count a take the worker would make, for the host tests, which have no
 * big lock. */
static void worker_bkl_enter(int needed)
{
    drm_worker_bkl_takes += needed != 0;
}

/** The host tests have no big lock. */
static void worker_bkl_exit(int needed)
{
    (void)needed;
}
#endif

/** Run the queued drivers' works whose time has come, each once, under the
 * big lock, which a driver's work, such as i915's hotplug probe, still
 * needs; the number run. A work that queues itself again waits for the
 * next round. The lock is taken before a work leaves the queue, so that an
 * owner's free, which cancels its work under the lock, either takes the
 * work off the queue or runs once `fn` has begun, never between them.
 * drm_dev_work_cancel does not wait for a running `fn`, as Linux's
 * cancel_work_sync does (kernel/workqueue.c:4423-4424), so an `fn` that
 * sleeps holds a reference to its owner across the sleep. */
static int dev_work_run(void)
{
    struct drm_work *work;
    struct drm_device *dev;
    uint64_t now = rtc_ticks_now();
    uint32_t flags;
    int n = 0, todo = 0;

    flags = spin_lock_irqsave(&work_lock);
    for (work = dev_works; work; work = work->next) {
        todo += dev_work_due(work, now);
    }
    spin_unlock_irqrestore(&work_lock, flags);
    while (n < todo) {
        worker_bkl_enter(1);
        flags = spin_lock_irqsave(&work_lock);
        for (work = dev_works; work && !dev_work_due(work, now); work = work->next) {
        }
        if (!work) {
            spin_unlock_irqrestore(&work_lock, flags);
            worker_bkl_exit(1);
            break;
        }
        dev_work_unlink(work);
        spin_unlock_irqrestore(&work_lock, flags);
        dev = work->dev;
        if (!dev->unplugged) {
            work->fn(work);
        }
        worker_bkl_exit(1);
        drm_dev_put(dev);
        n++;
    }
    return n;
}

/** Send the queued hotplug events, Linux's
 * drm_kms_helper_connector_hotplug_event for each, under the big lock,
 * which the uevent and the console's client take the lock for; the number
 * sent. */
static int hotplug_work(void)
{
    struct drm_connector *connector;
    uint32_t flags;
    int n = 0;

    for (;;) {
        flags = spin_lock_irqsave(&work_lock);
        if (nhotplugs == 0) {
            spin_unlock_irqrestore(&work_lock, flags);
            return n;
        }
        connector = hotplugs[0];
        nhotplugs--;
        k_memmove(hotplugs, hotplugs + 1, (uint32_t)nhotplugs * sizeof(hotplugs[0]));
        spin_unlock_irqrestore(&work_lock, flags);
        worker_bkl_enter(1);
        if (!connector->dev->unplugged) {
            drm_sysfs_connector_hotplug_event(connector);
            drm_client_dev_hotplug(connector->dev);
        }
        worker_bkl_exit(1);
        drm_dev_put(connector->dev);
        n++;
    }
}

/** Run the tail of `w`, a queued commit that `work_due` lets run, its
 * callbacks on fences that have not signalled taken back, then take it off
 * the queue and free it. Under its device's commit lock; may sleep in the
 * drivers' hooks. */
static void work_finish(struct commit_work *w)
{
    struct commit_work **link;
    struct drm_device *dev = w->state.dev;
    uint32_t flags;
    int i;

    flags = spin_lock_irqsave(&work_lock);
    w->running = 1;
    spin_unlock_irqrestore(&work_lock, flags);
    for (i = 0; i < DRM_MAX_PLANES; i++) {
        struct drm_plane_state *ps = w->state.planes_new[i];

        if (ps && ps->fence && dma_fence_remove_callback(ps->fence, &w->cbs[i].cb)) {
            flags = spin_lock_irqsave(&work_lock);
            w->waiting--;
            spin_unlock_irqrestore(&work_lock, flags);
        }
    }
    commit_tail(&w->state);
    flags = spin_lock_irqsave(&work_lock);
    for (link = &works; *link != w; link = &(*link)->next) {
    }
    *link = w->next;
    spin_unlock_irqrestore(&work_lock, flags);
    drm_dev_put(dev);
    kfree(w);
    wq_wake_all(&work_done_wait);
}

/** The first queued commit whose waits may advance or whose tail may run
 * now (`work_due`), or NULL, with a hold on its device in `*dev` when `dev`
 * is not NULL; a pointer that only its device's commit lock, taken after,
 * keeps from being finished and freed. A wait predicate may call it. */
static struct commit_work *work_next_ready(struct drm_device **dev)
{
    struct commit_work *w;
    uint64_t now = rtc_ticks_now();
    uint32_t flags = spin_lock_irqsave(&work_lock);

    for (w = works; w && !work_due_locked(w, now); w = w->next) {
    }
    if (w && dev) {
        *dev = w->state.dev;
        drm_dev_get(*dev);
    }
    spin_unlock_irqrestore(&work_lock, flags);
    return w;
}

/** Whether `w` is still on the queue: a work found without the commit
 * lock may have been finished by a blocking commit since. */
static int work_queued(const struct commit_work *w)
{
    const struct commit_work *q;
    uint32_t flags = spin_lock_irqsave(&work_lock);

    for (q = works; q && q != w; q = q->next) {
    }
    spin_unlock_irqrestore(&work_lock, flags);
    return q != NULL;
}

int drm_atomic_commit_work(void)
{
    struct commit_work *w;
    struct drm_device *dev;
    int n = dev_work_run(), big;

    n += hotplug_work();
    while ((w = work_next_ready(&dev)) != NULL) {
        /* A tail reaches the driver's display hooks, which take only their
         * own locks on a device with DRIVER_KMS_UNLOCKED and need the big
         * lock on any other, as the ATOMIC ioctl's dispatch decides
         * (`ioctl_call`, drm.c). The commit lock orders this with a
         * blocking commit of the device, which may finish the work
         * meanwhile; the hold keeps the device. */
        big = !drm_core_check_feature(dev, DRIVER_KMS_UNLOCKED);
        worker_bkl_enter(big);
        drm_modeset_lock_nointr(dev);
        if (work_queued(w) && w->state.dev == dev && work_due(w, rtc_ticks_now()) &&
            work_fence_progress(w)) {
            work_finish(w);
            n++;
        }
        drm_modeset_unlock(dev);
        worker_bkl_exit(big);
        drm_dev_put(dev);
    }
    return n;
}

#ifndef RACH2_HOSTTEST
static process_t *commit_thread;

/* What the worker's sleep began with: the generation of the queues and
 * the tick it wakes at, 0 for none. */
struct worker_wait {
    uint32_t gen;
    uint64_t tick;
};

/** Whether a queued commit may run, a hotplug event or a driver's work
 * waits whose time has come, a work was queued since the wait began, or
 * the tick the wait armed has passed, all of which `arg`, the sleep's
 * `struct worker_wait`, holds. */
static int work_ready(void *arg)
{
    const struct worker_wait *ww = (const struct worker_wait *)arg;
    const struct drm_work *dw;
    uint64_t now = rtc_ticks_now();
    uint32_t flags = spin_lock_irqsave(&work_lock);
    int ready = nhotplugs != 0 || dev_works_gen != ww->gen || (ww->tick && now >= ww->tick);

    for (dw = dev_works; dw && !ready; dw = dw->next) {
        ready = dev_work_due(dw, now);
    }
    spin_unlock_irqrestore(&work_lock, flags);
    return ready || work_next_ready(NULL) != NULL;
}

/**
 * The worker, kdrm: sleep in an idle wait until a queued commit may run, then
 * finish what is ready. It gives up the big lock it starts with, as Linux's
 * commit work runs the tail on a workqueue under no global lock
 * (drm_atomic_helper.c:1876-1882 and 2099 of 6.12.115), and takes the big
 * lock back only for what still needs it:
 * a driver's work, a hotplug event, and a commit's tail on a device without
 * DRIVER_KMS_UNLOCKED (`drm_atomic_commit_work`). So a nonblocking commit's
 * tail on i915 runs under the device's commit lock alone, as a blocking one
 * does in the ATOMIC ioctl. Like every kernel thread, it is not switched by
 * the tick (`no_tick_switch`, which `setup_process_stack` sets): a commit's
 * tail and a driver's work run here as they run inline in a system call, and
 * give way only where they sleep, so a driver's state, such as the transport
 * a virtio-gpu card sits on, never changes under them; the worker also gives
 * way between passes.
 */
static void commit_thread_main(void)
{
    const struct drm_work *dw;
    const struct commit_work *w;
    struct worker_wait ww;
    uint32_t flags;

    bkl_exit();
    arch_irq_enable();
    for (;;) {
        sched_service_point();
        /* Sleep until the soonest delayed work's tick or commit's fence
         * timeout at the latest. */
        flags = spin_lock_irqsave(&work_lock);
        ww.gen = dev_works_gen;
        ww.tick = 0;
        for (dw = dev_works; dw; dw = dw->next) {
            if (dw->due && (ww.tick == 0 || dw->due < ww.tick)) {
                ww.tick = dw->due;
            }
        }
        for (w = works; w; w = w->next) {
            if (!w->running && w->deadline && (ww.tick == 0 || w->deadline < ww.tick)) {
                ww.tick = w->deadline;
            }
        }
        spin_unlock_irqrestore(&work_lock, flags);
        current_proc->wakeup_tick = ww.tick;
        (void)wq_wait_event_idle(&work_wait, work_ready, &ww);
        current_proc->wakeup_tick = 0;
        (void)drm_atomic_commit_work();
    }
}

/** Start the worker, once, under the big lock, which keeps the process
 * table: 0, or -ENOMEM. Process context; `drm_dev_register` starts it,
 * and a commit that defers to it tries again if that failed. */
static int32_t commit_worker_start(void)
{
    process_t *p;
    uintptr_t base;
    uint32_t flags;

    if (commit_thread) {
        return 0;
    }
    bkl_enter();
    if (commit_thread) {
        bkl_exit();
        return 0;
    }
    flags = kirq_save();
    base = kstack_alloc();
    p = base ? alloc_process() : NULL;
    if (!p) {
        if (base) {
            kstack_free(base + KERNEL_STACK_SIZE);
        }
        kirq_restore(flags);
        bkl_exit();
        klog(KLOG_ERR, "drm", "worker: refused: no record or stack for it");
        return -ENOMEM;
    }
    p->kernel_stack = base + KERNEL_STACK_SIZE;
    p->space_root = 0;
    p->page_dir = NULL;
    p->parent_pid = 0;
    proc_set_comm(p, "kdrm");
    setup_process_stack(p, commit_thread_main);
    commit_thread = p;
    sched_make_ready(p);
    kirq_restore(flags);
    bkl_exit();
    klog(KLOG_DEBUG, "drm", "worker: kdrm is pid %d", (int)p->pid);
    return 0;
}
#else
/** The host tests run the queue themselves (`drm_atomic_commit_work`). */
static int32_t commit_worker_start(void)
{
    return 0;
}
#endif

int32_t drm_atomic_worker_start(void)
{
    return commit_worker_start();
}

/** Queue the commit `state`, swapped in, for the worker: its states move
 * to `w`, and the worker begins its fence waits in process context. */
static void work_queue(struct commit_work *w, struct drm_atomic_state *state)
{
    struct commit_work **link;
    uint32_t flags;

    w->state = *state;
    k_memset(state->planes_old, 0, sizeof(state->planes_old));
    k_memset(state->planes_new, 0, sizeof(state->planes_new));
    k_memset(state->crtcs_old, 0, sizeof(state->crtcs_old));
    k_memset(state->crtcs_new, 0, sizeof(state->crtcs_new));
    k_memset(state->connectors_old, 0, sizeof(state->connectors_old));
    k_memset(state->connectors_new, 0, sizeof(state->connectors_new));
    w->waited = -1;
    drm_dev_get(w->state.dev);
    flags = spin_lock_irqsave(&work_lock);
    /* The worker's sleep looks again, to begin this commit's waits. */
    dev_works_gen++;
    for (link = &works; *link; link = &(*link)->next) {
    }
    w->next = NULL;
    *link = w;
    spin_unlock_irqrestore(&work_lock, flags);
    wq_wake_all(&work_wait);
}

/** Give each CRTC the commit leaves or makes active an event, the caller's
 * or one of the core's, which ends the flip when it is sent, as Linux's
 * drm_atomic_helper_setup_commit does for its flip_done. A legacy cursor
 * update has none. 0 or -ENOMEM. */
static int32_t setup_flips(struct drm_atomic_state *state)
{
    int i;

    if (state->legacy_cursor_update) {
        return 0;
    }
    for (i = 0; i < DRM_MAX_CRTCS; i++) {
        struct drm_crtc_state *cs = state->crtcs_new[i];

        if (!cs || (!cs->active && !state->crtcs_old[i]->active)) {
            continue;
        }
        if (!cs->event) {
            cs->event = drm_crtc_flip_event(cs->crtc, 0);
            if (!cs->event) {
                return -ENOMEM;
            }
        }
        cs->event->flip_crtc = cs->crtc;
    }
    return 0;
}

/**
 * Wait for every in-fence of `state`, interruptibly when `intr`, as
 * Linux's drm_atomic_helper_wait_for_fences; or, under the driver's
 * `commit_fence_timeout_ms`, for each plane's fence in turn for at most
 * that long, uninterruptibly, and for none after one that times out, as
 * Linux's intel_atomic_commit_fence_wait (intel_display.c:7262-7280). 0 or
 * -EINTR.
 */
static int32_t wait_for_fences(struct drm_atomic_state *state, int intr)
{
    uint32_t ms = state->dev->mode_config.commit_fence_timeout_ms;
    int32_t rc;
    int i;

    for (i = 0; i < DRM_MAX_PLANES; i++) {
        struct drm_plane_state *ps = state->planes_new[i];

        if (!ps || !ps->fence) {
            continue;
        }
        if (ms) {
            if (dma_fence_wait_until(ps->fence, 0, dma_fence_now() + (int64_t)ms * 1000000) != 0) {
                return 0;
            }
        } else if ((rc = dma_fence_wait_until(ps->fence, intr, -1)) != 0) {
            return rc;
        }
    }
    return 0;
}

/** The first queued commit of `state`'s device that holds a state of a
 * CRTC, plane or connector `state` changes, whose tail reads the states
 * that `state`'s tail would free as old; or NULL. The caller holds the
 * device's commit lock, which keeps the work it finds queued. */
static struct commit_work *work_overlapping(const struct drm_atomic_state *state)
{
    struct commit_work *w;
    uint32_t flags = spin_lock_irqsave(&work_lock);
    int i, hit;

    for (w = works; w; w = w->next) {
        if (w->state.dev != state->dev) {
            continue;
        }
        hit = 0;
        for (i = 0; i < DRM_MAX_PLANES; i++) {
            hit |= state->planes_new[i] && w->state.planes_new[i];
        }
        for (i = 0; i < DRM_MAX_CRTCS; i++) {
            hit |= state->crtcs_new[i] && w->state.crtcs_new[i];
        }
        for (i = 0; i < DRM_MAX_CONNECTORS; i++) {
            hit |= state->connectors_new[i] && w->state.connectors_new[i];
        }
        if (hit) {
            break;
        }
    }
    spin_unlock_irqrestore(&work_lock, flags);
    return w;
}

/** Whether queued hardware work would overwrite an independent cursor
 * update of `plane`; the caller holds the commit lock. */
int drm_atomic_helper_plane_pending(struct drm_plane *plane)
{
    struct commit_work *w;
    struct drm_crtc *crtc = plane->state->crtc;
    uint32_t flags = spin_lock_irqsave(&work_lock);
    int pending = 0;

    for (w = works; w && !pending; w = w->next) {
        struct drm_crtc_state *cs;

        if (w->state.dev != plane->dev) {
            continue;
        }
        cs = crtc ? w->state.crtcs_new[crtc->index] : NULL;
        pending = w->state.planes_new[plane->index] != NULL ||
                  (cs && (drm_atomic_crtc_needs_modeset(cs) || cs->color_mgmt_changed));
    }
    spin_unlock_irqrestore(&work_lock, flags);
    return pending;
}

/* What a commit stalled on a queued one waits with: its state and the
 * tick its sleep wakes at, the queued commit's fence timeout, 0 for
 * none. */
struct overlap_wait {
    const struct drm_atomic_state *state;
    uint64_t tick;
};

/** Whether a commit stalled on a queued one may act, or look again: none
 * holds its objects, the first that does may run (`work_due`), or the
 * tick the sleep armed in `arg`, a `struct overlap_wait`, has passed. */
static int overlap_ready(void *arg)
{
    const struct overlap_wait *ow = (const struct overlap_wait *)arg;
    const struct commit_work *w = work_overlapping(ow->state);
    uint64_t now = rtc_ticks_now();

    return !w || work_due(w, now) || (ow->tick && now >= ow->tick);
}

/**
 * Linux's stall on the commits of a state's objects, which a queued commit
 * has yet to finish: EBUSY for a nonblocking commit; a blocking one waits
 * for the queued commit's fences and runs its tail itself, or waits for the
 * worker to end a tail it began, since the worker's other works may want
 * the commit lock the caller holds. 0, -EBUSY or -EINTR.
 */
static int32_t stall_on_works(struct drm_atomic_state *state, int nonblock)
{
    struct commit_work *w;
    struct overlap_wait ow;
    uint64_t saved = 0;
    uint32_t flags;
    int32_t rc;

    while ((w = work_overlapping(state)) != NULL) {
        if (nonblock) {
            return -EBUSY;
        }
        if (work_due(w, rtc_ticks_now())) {
            if (work_fence_progress(w)) {
                work_finish(w);
            }
            continue;
        }
        ow.state = state;
        flags = spin_lock_irqsave(&work_lock);
        ow.tick = w->running ? 0 : w->deadline;
        spin_unlock_irqrestore(&work_lock, flags);
        if (current_proc) {
            saved = current_proc->wakeup_tick;
            current_proc->wakeup_tick = ow.tick;
        }
        rc = wq_wait_event(&work_done_wait, overlap_ready, &ow);
        if (current_proc) {
            current_proc->wakeup_tick = saved;
        }
        if (rc != 0) {
            return -EINTR;
        }
    }
    return 0;
}

/**
 * Linux's stall_checks: a nonblocking commit may not get ahead of a queued
 * commit of its objects or of a flip; a blocking one finishes the queued
 * commit first, as stall_on_works says, then waits at most 10 s for each
 * flip's vblank. 0, -EBUSY or -EINTR.
 */
static int32_t stall_checks(struct drm_atomic_state *state, int nonblock)
{
    int32_t rc = stall_on_works(state, nonblock);
    int i;

    if (rc != 0) {
        return rc;
    }
    for (i = 0; i < DRM_MAX_CRTCS; i++) {
        struct drm_crtc_state *cs = state->crtcs_new[i];

        if (!cs || !cs->crtc->flip_pending) {
            continue;
        }
        if (nonblock) {
            return -EBUSY;
        }
        rc = drm_crtc_wait_flip(cs->crtc, DRM_FLIP_QUEUED, 1, 0);
        if (rc == 0) {
            rc = drm_crtc_wait_flip(cs->crtc, DRM_FLIP_ARMED, 1, 10000);
        }
        if (rc == -EINTR) {
            return rc;
        }
    }
    return 0;
}

/**
 * Give each plane of `state` that shows a framebuffer and has no in-fence
 * the pending writes of the framebuffer's objects as its fence, Linux's
 * drm_gem_plane_helper_prepare_fb, which drm_atomic_helper_prepare_planes
 * runs for every GEM driver without a prepare_fb of its own and which
 * i915's intel_prepare_plane_fb and virtio-gpu's virtio_gpu_plane_prepare_fb
 * (virtgpu_plane.c:281 in Linux 6.12.115) run too. A flip then waits for
 * the render job that draws the buffer, which a compositor that passes no
 * IN_FENCE_FD relies on; an in-fence overrules them, as on Linux. Returns
 * 0, or -ENOMEM when two objects' writes could not be merged into one
 * fence.
 */
static int32_t implicit_fences(struct drm_atomic_state *state)
{
    struct dma_fence *w[4];
    int i, j, k, n;

    for (i = 0; i < DRM_MAX_PLANES; i++) {
        struct drm_plane_state *ps = state->planes_new[i];

        if (!ps || !ps->fb || ps->fence) {
            continue;
        }
        n = 0;
        for (j = 0; j < 4; j++) {
            struct drm_gem_object *obj = ps->fb->obj[j];
            struct dma_fence *f = obj ? dma_resv_get_write(obj->resv) : NULL;

            for (k = 0; f && k < n && w[k] != f; k++) {
            }
            if (f && k == n) {
                w[n++] = f;
            } else {
                dma_fence_put(f);
            }
        }
        if (n == 1) {
            ps->fence = w[0];
        } else if (n > 1) {
            ps->fence = dma_fence_merge(w, (uint32_t)n);
            for (k = 0; k < n; k++) {
                dma_fence_put(w[k]);
            }
            if (!ps->fence) {
                return -ENOMEM;
            }
        }
    }
    return 0;
}

/** Allocate `*work` to defer a nonblocking mode set or an unsignalled
 * in-fence, or leave it NULL for an inline tail. 0, -ENOMEM or the
 * worker's start error; failure leaves the state unchanged. */
static int32_t defer_work(struct drm_atomic_state *state, int nonblock, struct commit_work **work)
{
    int32_t rc;
    int i, defer = 0;

    *work = NULL;
    for (i = 0; i < DRM_MAX_CRTCS && nonblock; i++) {
        struct drm_crtc_state *cs = state->crtcs_new[i];

        defer |= cs && drm_atomic_crtc_needs_modeset(cs);
    }
    for (i = 0; i < DRM_MAX_PLANES && nonblock; i++) {
        struct drm_plane_state *ps = state->planes_new[i];

        defer |= ps && ps->fence && !dma_fence_is_signaled(ps->fence);
    }
    if (defer) {
        *work = (struct commit_work *)kzalloc(sizeof(**work));
        if (!*work) {
            return -ENOMEM;
        }
        rc = commit_worker_start();
        if (rc != 0) {
            kfree(*work);
            *work = NULL;
            return rc;
        }
    }
    return 0;
}

int32_t drm_atomic_helper_commit(struct drm_device *dev, struct drm_atomic_state *state,
                                 int nonblock)
{
    struct commit_work *w;
    uint32_t flips = 0;
    int32_t rc;
    int i;

    rc = stall_checks(state, nonblock);
    if (rc == 0) {
        rc = implicit_fences(state);
    }
    if (rc != 0) {
        return rc;
    }
    rc = defer_work(state, nonblock, &w);
    if (rc != 0) {
        return rc;
    }
    /* Linux waits before the swap for a blocking commit. */
    if (!w && (rc = wait_for_fences(state, 1)) != 0) {
        return rc;
    }
    rc = setup_flips(state);
    if (rc != 0) {
        kfree(w);
        return rc;
    }
    swap_state(state);
    for (i = 0; i < DRM_MAX_CRTCS; i++) {
        struct drm_crtc_state *cs = state->crtcs_new[i];

        if (cs && cs->event && cs->event->flip_crtc) {
            cs->crtc->flip_pending = w ? DRM_FLIP_QUEUED : DRM_FLIP_ARMED;
            flips |= 1u << i;
        }
    }
    if (w) {
        work_queue(w, state);
        return 0;
    }
    commit_tail(state);
    /* Linux's wait for the flips, which a blocking commit makes. */
    for (i = 0; i < DRM_MAX_CRTCS && !nonblock; i++) {
        if (flips & (1u << i)) {
            (void)drm_crtc_wait_flip(dev->mode_config.crtcs[i], DRM_FLIP_ARMED, 0, 100);
        }
    }
    return 0;
}

/** Check and commit `state` through the device's hooks or the core's. */
static int32_t commit(struct drm_atomic_state *state, int nonblock)
{
    struct drm_device *dev = state->dev;
    int32_t rc = drm_atomic_check_only(state);

    if (rc != 0) {
        return rc;
    }
    return dev->mode_config.atomic_commit ? dev->mode_config.atomic_commit(dev, state, nonblock)
                                          : drm_atomic_helper_commit(dev, state, nonblock);
}

int32_t drm_atomic_commit(struct drm_atomic_state *state)
{
    return commit(state, 0);
}

int32_t drm_atomic_nonblocking_commit(struct drm_atomic_state *state)
{
    return commit(state, 1);
}

/** Fill `state` with every connector routed nowhere, every CRTC off with
 * no mode and every plane off: 0 or -ENOMEM. */
static int32_t disable_all(struct drm_device *dev, struct drm_atomic_state *state)
{
    struct drm_mode_config *mc = &dev->mode_config;
    struct drm_connector_state *conn;
    struct drm_crtc_state *cs;
    struct drm_plane_state *ps;
    int32_t rc = 0;
    uint32_t i;

    for (i = 0; rc == 0 && i < mc->num_connector; i++) {
        conn = drm_atomic_get_connector_state(state, mc->connectors[i]);
        rc = conn ? drm_atomic_set_crtc_for_connector(state, conn, NULL) : -ENOMEM;
    }
    for (i = 0; rc == 0 && i < mc->num_crtc; i++) {
        cs = drm_atomic_get_crtc_state(state, mc->crtcs[i]);
        rc = cs ? drm_atomic_set_mode_for_crtc(cs, NULL) : -ENOMEM;
        if (rc == 0) {
            cs->active = 0;
        }
    }
    for (i = 0; rc == 0 && i < mc->num_plane; i++) {
        ps = drm_atomic_get_plane_state(state, mc->planes[i]);
        rc = ps ? drm_atomic_set_crtc_for_plane(state, ps, NULL) : -ENOMEM;
        if (rc == 0) {
            drm_atomic_set_fb_for_plane(ps, NULL);
        }
    }
    return rc;
}

int32_t drm_atomic_helper_shutdown(struct drm_device *dev)
{
    struct drm_atomic_state *state;
    int32_t rc;

    if (!drm_core_check_feature(dev, DRIVER_MODESET)) {
        return 0;
    }
    drm_modeset_lock_nointr(dev);
    state = drm_atomic_state_alloc(dev);
    rc = state ? disable_all(dev, state) : -ENOMEM;
    if (rc == 0) {
        rc = drm_atomic_commit(state);
    }
    drm_atomic_state_free(state);
    drm_modeset_unlock(dev);
    return rc;
}

struct drm_pending_event *drm_crtc_flip_event(struct drm_crtc *crtc, uint64_t user_data)
{
    struct drm_pending_event *e = (struct drm_pending_event *)kzalloc(sizeof(*e));

    if (e) {
        e->length = sizeof(e->event.vbl);
        e->event.vbl.base.type = DRM_EVENT_FLIP_COMPLETE;
        e->event.vbl.base.length = sizeof(e->event.vbl);
        e->event.vbl.user_data = user_data;
        e->event.vbl.crtc_id = crtc->base.id;
    }
    return e;
}

/* ------------------------------------------------------------------------
 * The ATOMIC ioctl
 */

/* An out-fence of a CRTC: a point on the CRTC's timeline, signalled when
 * the commit's event is sent. The names live in the fence, which a sync
 * file may keep after the device has gone. */
struct crtc_fence {
    struct dma_fence base; /* first: the record is freed with kfree */
    const char *driver;
    char timeline[DRM_PROP_NAME_LEN];
};

/** An out-fence's driver name, the device's driver's. */
static const char *crtc_fence_driver(struct dma_fence *fence)
{
    return ((struct crtc_fence *)fence)->driver;
}

/** An out-fence's timeline name, Linux's "CRTC:ID-NAME". */
static const char *crtc_fence_timeline(struct dma_fence *fence)
{
    return ((struct crtc_fence *)fence)->timeline;
}

static const struct dma_fence_ops crtc_fence_ops = {
    crtc_fence_driver, crtc_fence_timeline, NULL, NULL, NULL, NULL, 0};

/** Append the string `src` to `out` of `cap` bytes at `*at`, cut to fit. */
static void put_str(char *out, uint32_t cap, uint32_t *at, const char *src)
{
    while (*src && *at + 1 < cap) {
        out[(*at)++] = *src++;
    }
    out[*at] = '\0';
}

/** Append the decimal `v` to `out` of `cap` bytes at `*at`. */
static void put_dec(char *out, uint32_t cap, uint32_t *at, uint64_t v)
{
    char digits[24];
    int n = 0;

    do {
        digits[n++] = (char)('0' + (int)(v - drm_div64(v, 10) * 10));
        v = drm_div64(v, 10);
    } while (v);
    while (n > 0 && *at + 1 < cap) {
        out[(*at)++] = digits[--n];
    }
    out[*at] = '\0';
}

/** A new out-fence of `crtc`, Linux's drm_crtc_create_fence, with one
 * reference, the caller's; NULL with no memory. */
static struct dma_fence *crtc_create_fence(struct drm_crtc *crtc)
{
    struct crtc_fence *f = (struct crtc_fence *)kzalloc(sizeof(*f));
    uint32_t at = 0;

    if (!f) {
        return NULL;
    }
    if (!crtc->fence_context) {
        crtc->fence_context = dma_fence_context_alloc(1);
    }
    dma_fence_init(&f->base, &crtc_fence_ops, crtc->fence_context, ++crtc->fence_seqno);
    f->driver = crtc->dev->driver->ident.name;
    put_str(f->timeline, sizeof(f->timeline), &at, "CRTC:");
    put_dec(f->timeline, sizeof(f->timeline), &at, crtc->base.id);
    put_str(f->timeline, sizeof(f->timeline), &at, "-");
    put_str(f->timeline, sizeof(f->timeline), &at, crtc->name);
    return &f->base;
}

/** The out-fences of an ATOMIC call, by CRTC index: each fence, held,
 * where its descriptor goes, and, once out_fence_setup has run, its sync
 * file, not yet attached, and the descriptor reserved for it. */
struct out_fences {
    struct dma_fence *fence[DRM_MAX_CRTCS];
    int32_t __user *ptr[DRM_MAX_CRTCS];
    struct sync_file *file[DRM_MAX_CRTCS];
    int32_t fd[DRM_MAX_CRTCS];
};

/**
 * Linux's setup_out_fence for CRTC `i` of `out`: reserve the descriptor,
 * write it where OUT_FENCE_PTR points, and make the sync file, all before
 * the commit, so that a commit that succeeds cannot then fail to give it. 0, or
 * -EMFILE, -EFAULT or -ENOMEM with nothing reserved or made.
 */
static int32_t out_fence_setup(struct out_fences *out, int i)
{
    int32_t fd = fd_alloc(current_proc->fds);

    if (fd < 0) {
        return fd;
    }
    if (copy_to_user(out->ptr[i], &fd, sizeof(fd)) != 0) {
        fd_free(current_proc->fds, fd);
        return -EFAULT;
    }
    out->file[i] = sync_file_new(out->fence[i], NULL);
    if (!out->file[i]) {
        fd_free(current_proc->fds, fd);
        return -ENOMEM;
    }
    out->fd[i] = fd;
    return 0;
}

/**
 * Give each CRTC of the commit its event when the caller asked for events
 * or an out-fence, Linux's prepare_signaling: a reserved FLIP_COMPLETE, and
 * for OUT_FENCE_PTR a new fence of the CRTC that the event signals, kept
 * in `out` too with its descriptor and sync file (out_fence_setup). Each
 * CRTC of a PAGE_FLIP_ASYNC commit is marked async first, a TEST_ONLY one
 * too, so that the driver's check applies the async rules to both.
 * -EINVAL when no CRTC would send an asked event, -ENOMEM, or the error of
 * the reservation or of out_fence_setup.
 */
static int32_t prepare_signaling(struct drm_atomic_state *state, const struct drm_mode_atomic *arg,
                                 struct drm_file *file, struct out_fences *out)
{
    struct dma_fence *fence;
    int i, crtcs = 0;
    int32_t rc;

    for (i = 0; i < DRM_MAX_CRTCS; i++) {
        if (state->crtcs_new[i] && (arg->flags & DRM_MODE_PAGE_FLIP_ASYNC)) {
            state->crtcs_new[i]->async_flip = 1;
        }
    }
    if (arg->flags & DRM_MODE_ATOMIC_TEST_ONLY) {
        return 0;
    }
    for (i = 0; i < DRM_MAX_CRTCS; i++) {
        struct drm_crtc_state *cs = state->crtcs_new[i];

        if (!cs) {
            continue;
        }
        crtcs++;
        if (!(arg->flags & DRM_MODE_PAGE_FLIP_EVENT) && !cs->out_fence_ptr) {
            continue;
        }
        cs->event = drm_crtc_flip_event(cs->crtc, arg->user_data);
        if (!cs->event) {
            return -ENOMEM;
        }
        if (arg->flags & DRM_MODE_PAGE_FLIP_EVENT) {
            rc = drm_event_reserve(file, cs->event);
            if (rc != 0) {
                kfree(cs->event);
                cs->event = NULL;
                return rc;
            }
        }
        if (cs->out_fence_ptr) {
            fence = crtc_create_fence(cs->crtc);
            if (!fence) {
                return -ENOMEM;
            }
            cs->event->fence = fence;
            out->fence[i] = dma_fence_get(fence);
            out->ptr[i] = cs->out_fence_ptr;
            rc = out_fence_setup(out, i);
            if (rc != 0) {
                return rc;
            }
        }
    }
    if (crtcs == 0 && (arg->flags & DRM_MODE_PAGE_FLIP_EVENT)) {
        return -EINVAL;
    }
    return 0;
}

/**
 * Linux's complete_signaling: attach the sync file of each out-fence of a
 * commit that `rc` says succeeded to its reserved descriptor; for a failed
 * one, drop the files, free the descriptors and write -1 where
 * OUT_FENCE_PTR points. Drop the fences either way. Returns `rc`.
 */
static int32_t complete_signaling(struct out_fences *out, int32_t rc)
{
    int32_t none = -1;
    int i;

    for (i = 0; i < DRM_MAX_CRTCS; i++) {
        if (!out->fence[i]) {
            continue;
        }
        if (rc == 0) {
            sync_file_attach(out->fd[i], out->file[i]);
        } else {
            if (out->file[i]) {
                sync_file_drop(out->file[i]);
                fd_free(current_proc->fds, out->fd[i]);
            }
            (void)copy_to_user(out->ptr[i], &none, sizeof(none));
        }
        dma_fence_put(out->fence[i]);
    }
    return rc;
}

/** Read the 32-bit element `i` of the user array at `ptr` into `*v`. */
static int32_t get_u32(uint64_t ptr, uint32_t i, uint32_t *v)
{
    return copy_from_user(v, (const void __user *)(uintptr_t)(ptr + 4u * i), 4) ? -EFAULT : 0;
}

/** Set the properties the ioctl names in `state`, Linux's loop in
 * drm_mode_atomic_ioctl. */
static int32_t set_properties(struct drm_device *dev, struct drm_file *file,
                              struct drm_atomic_state *state, const struct drm_mode_atomic *arg)
{
    uint32_t i, j, obj_id, count, prop_id, props = 0;
    struct drm_mode_object *obj;
    struct drm_property *prop;
    uint64_t value;
    int32_t rc;

    for (i = 0; i < arg->count_objs; i++) {
        if (get_u32(arg->objs_ptr, i, &obj_id) != 0) {
            return -EFAULT;
        }
        obj = drm_mode_object_find(dev, file, obj_id, DRM_MODE_OBJECT_ANY);
        if (!obj) {
            return -ENOENT;
        }
        if (obj->type != DRM_MODE_OBJECT_CRTC && obj->type != DRM_MODE_OBJECT_PLANE &&
            obj->type != DRM_MODE_OBJECT_CONNECTOR) {
            drm_mode_object_put(obj);
            return -ENOENT;
        }
        if (get_u32(arg->count_props_ptr, i, &count) != 0) {
            return -EFAULT;
        }
        for (j = 0; j < count; j++, props++) {
            if (get_u32(arg->props_ptr, props, &prop_id) != 0) {
                return -EFAULT;
            }
            prop = drm_object_find_property(obj, prop_id);
            if (!prop) {
                return -ENOENT;
            }
            if (copy_from_user(&value,
                               (const void __user *)(uintptr_t)(arg->prop_values_ptr + 8u * props),
                               8) != 0) {
                return -EFAULT;
            }
            rc = drm_atomic_set_property(state, file, obj, prop, value);
            if (rc != 0) {
                return rc;
            }
        }
    }
    return 0;
}

/** ATOMIC, Linux's drm_mode_atomic_ioctl: set the properties of the
 * objects named, then test, or commit with or without blocking, and send
 * a flip event from each CRTC the commit touched when asked. */
int32_t drm_mode_atomic_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_mode_atomic *arg = (struct drm_mode_atomic *)data;
    struct drm_atomic_state *state;
    struct out_fences out;
    int32_t rc;

    if (!drm_core_check_feature(dev, DRIVER_ATOMIC)) {
        return -EOPNOTSUPP;
    }
    if (!file->atomic) {
        return -EINVAL;
    }
    if ((arg->flags & ~(uint32_t)DRM_MODE_ATOMIC_FLAGS) || arg->reserved) {
        return -EINVAL;
    }
    if ((arg->flags & DRM_MODE_PAGE_FLIP_ASYNC) && !dev->mode_config.async_page_flip) {
        return -EINVAL;
    }
    if ((arg->flags & DRM_MODE_ATOMIC_TEST_ONLY) && (arg->flags & DRM_MODE_PAGE_FLIP_EVENT)) {
        return -EINVAL;
    }
    state = drm_atomic_state_alloc(dev);
    if (!state) {
        return -ENOMEM;
    }
    state->flags = arg->flags;
    state->allow_modeset = (arg->flags & DRM_MODE_ATOMIC_ALLOW_MODESET) != 0;
    rc = drm_modeset_lock(dev);
    if (rc != 0) {
        drm_atomic_state_free(state);
        return rc;
    }
    k_memset(&out, 0, sizeof(out));
    rc = set_properties(dev, file, state, arg);
    if (rc == 0) {
        rc = prepare_signaling(state, arg, file, &out);
    }
    if (rc == 0) {
        if (arg->flags & DRM_MODE_ATOMIC_TEST_ONLY) {
            rc = drm_atomic_check_only(state);
        } else if (arg->flags & DRM_MODE_ATOMIC_NONBLOCK) {
            rc = drm_atomic_nonblocking_commit(state);
        } else {
            rc = drm_atomic_commit(state);
        }
    }
    rc = complete_signaling(&out, rc);
    /* A failed commit's events go back unsent with its states. */
    drm_atomic_state_free(state);
    drm_modeset_unlock(dev);
    return rc;
}

/* ------------------------------------------------------------------------
 * The legacy mode set
 */

/** Route the connectors of `set` to its CRTC and the rest off it, and turn
 * off each other CRTC left with none, Linux's update_output_state. */
static int32_t update_output_state(struct drm_atomic_state *state, struct drm_mode_set *set)
{
    struct drm_mode_config *mc = &state->dev->mode_config;
    struct drm_connector_state *conn;
    uint32_t i, j;
    int32_t rc;

    for (i = 0; i < mc->num_crtc; i++) {
        if (state->crtcs_new[i] &&
            (rc = drm_atomic_add_affected_connectors(state, mc->crtcs[i])) != 0) {
            return rc;
        }
    }
    for (i = 0; i < mc->num_connector; i++) {
        conn = state->connectors_new[i];
        if (conn && conn->crtc == set->crtc) {
            if ((rc = drm_atomic_set_crtc_for_connector(state, conn, NULL)) != 0) {
                return rc;
            }
            conn->link_status = DRM_MODE_LINK_STATUS_GOOD;
        }
    }
    for (j = 0; j < set->num_connectors; j++) {
        conn = drm_atomic_get_connector_state(state, set->connectors[j]);
        if (!conn) {
            return -ENOMEM;
        }
        if ((rc = drm_atomic_set_crtc_for_connector(state, conn, set->crtc)) != 0) {
            return rc;
        }
    }
    for (i = 0; i < mc->num_crtc; i++) {
        struct drm_crtc_state *cs = state->crtcs_new[i];

        if (!cs || mc->crtcs[i] == set->crtc || cs->connector_mask) {
            continue;
        }
        if ((rc = drm_atomic_set_mode_prop_for_crtc(cs, NULL)) != 0) {
            return rc;
        }
        cs->active = 0;
    }
    return 0;
}

int32_t drm_atomic_helper_set_config(struct drm_mode_set *set)
{
    struct drm_crtc *crtc = set->crtc;
    struct drm_atomic_state *state = drm_atomic_state_alloc(crtc->dev);
    struct drm_crtc_state *cs;
    struct drm_plane_state *ps;
    uint32_t w, h;
    int32_t rc;

    if (!state) {
        return -ENOMEM;
    }
    cs = drm_atomic_get_crtc_state(state, crtc);
    ps = crtc->primary ? drm_atomic_get_plane_state(state, crtc->primary) : NULL;
    if (!cs || (crtc->primary && !ps)) {
        drm_atomic_state_free(state);
        return -ENOMEM;
    }
    /* Linux's __drm_atomic_helper_set_config. */
    if (!set->mode) {
        rc = drm_atomic_set_mode_for_crtc(cs, NULL);
        cs->active = 0;
        if (rc == 0 && ps) {
            rc = drm_atomic_set_crtc_for_plane(state, ps, NULL);
            drm_atomic_set_fb_for_plane(ps, NULL);
        }
    } else {
        rc = drm_atomic_set_mode_for_crtc(cs, set->mode);
        cs->active = 1;
        if (rc == 0 && ps) {
            rc = drm_atomic_set_crtc_for_plane(state, ps, crtc);
            drm_atomic_set_fb_for_plane(ps, set->fb);
            /* The picture's size, both eyes of a frame-packed mode. */
            drm_mode_get_hv_timing(set->mode, &w, &h);
            ps->crtc_x = 0;
            ps->crtc_y = 0;
            ps->crtc_w = w;
            ps->crtc_h = h;
            ps->src_x = set->x << 16;
            ps->src_y = set->y << 16;
            if (ps->rotation & (DRM_MODE_ROTATE_90 | DRM_MODE_ROTATE_270)) {
                ps->src_w = h << 16;
                ps->src_h = w << 16;
            } else {
                ps->src_w = w << 16;
                ps->src_h = h << 16;
            }
        }
    }
    if (rc == 0) {
        rc = update_output_state(state, set);
    }
    if (rc == 0) {
        rc = conflicting_encoders(state, 1);
    }
    if (rc == 0) {
        rc = drm_atomic_commit(state);
    }
    drm_atomic_state_free(state);
    return rc;
}
