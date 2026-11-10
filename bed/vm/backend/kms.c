/*
 * kms.c: the backend's display, Rach2's scanout on the VM's KMS as the DRM
 * master: one connector, its crtc and primary plane by atomic commits, the
 * mode chosen from the connector's modes for the scanout's size, frames
 * copied into dumb buffers or a resource's own dma-buf scanned out, and a
 * writeback connector's copy of each frame checked when asked.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include "backend.h"

#define MAX_MODES 64

/* A framebuffer the display can show. */
struct fb {
    uint32_t id;     /* the framebuffer, 0 when none */
    uint32_t handle; /* its GEM handle */
    uint32_t pitch, width, height;
    uint64_t size;
    uint8_t *map; /* a dumb buffer's mapping, or 0 for an import */
    uint32_t res; /* the resource an import shows, or 0 */
    int flip;     /* its first row is the bottom one: shown reflected */
};

/* Property ids, found by name once. */
struct props {
    uint32_t conn_crtc, crtc_mode, crtc_active;
    uint32_t fb, crtc, src_x, src_y, src_w, src_h, crtc_x, crtc_y, crtc_w, crtc_h;
    uint32_t wb_crtc, wb_fb, wb_fence;
    uint32_t rotation; /* the plane's, with rotate-0 and reflect-y, or 0 */
};

struct kms {
    int fd;
    struct vr *vr;
    int direct;    /* scan a resource's dma-buf out when it can */
    int writeback; /* check each frame through the writeback connector */
    uint32_t conn, crtc, plane, wb_conn;
    drmModeModeInfo modes[MAX_MODES];
    uint32_t nmodes, preferred;
    int mode;      /* the mode set, or -1 */
    uint32_t blob; /* its MODE_ID blob */
    struct fb buf[2], wb, imp;
    uint32_t cur;        /* the dumb buffer shown */
    uint32_t res;        /* the scanout's resource, or 0 */
    uint32_t refused;    /* the last resource refused direct scanout */
    struct vd_rect rect; /* its rectangle of the resource */
    uint8_t edid[VD_EDID_MAX];
    uint32_t edid_len;
    struct props p;
    struct kms_counts n;
};

/** Find a property of an object by name; return its id or 0. */
static uint32_t prop(int fd, uint32_t obj, uint32_t type, const char *name)
{
    drmModeObjectProperties *ps = drmModeObjectGetProperties(fd, obj, type);
    uint32_t i, id = 0;

    for (i = 0; ps && i < ps->count_props && id == 0; i++) {
        drmModePropertyRes *pr = drmModeGetProperty(fd, ps->props[i]);

        if (pr && strcmp(pr->name, name) == 0) {
            id = pr->prop_id;
        }
        drmModeFreeProperty(pr);
    }
    drmModeFreeObjectProperties(ps);
    return id;
}

/** Return a property's value on an object, or def when it has none. */
static uint64_t prop_value(int fd, uint32_t obj, uint32_t type, const char *name, uint64_t def)
{
    drmModeObjectProperties *ps = drmModeObjectGetProperties(fd, obj, type);
    uint64_t v = def;
    uint32_t i;

    for (i = 0; ps && i < ps->count_props; i++) {
        drmModePropertyRes *pr = drmModeGetProperty(fd, ps->props[i]);

        if (pr && strcmp(pr->name, name) == 0) {
            v = ps->prop_values[i];
        }
        drmModeFreeProperty(pr);
    }
    drmModeFreeObjectProperties(ps);
    return v;
}

/** Return the plane's rotation property when it can reflect in y. */
static uint32_t reflect_prop(int fd, uint32_t plane)
{
    uint32_t id = prop(fd, plane, DRM_MODE_OBJECT_PLANE, "rotation"), found = 0;
    drmModePropertyRes *pr;
    int i;

    if (id == 0 || (pr = drmModeGetProperty(fd, id)) == 0) {
        return 0;
    }
    for (i = 0; i < pr->count_enums; i++) {
        /* A bitmask property's enum values are bit numbers. */
        if ((strcmp(pr->enums[i].name, "rotate-0") == 0 &&
             (1ULL << pr->enums[i].value) == DRM_MODE_ROTATE_0) ||
            (strcmp(pr->enums[i].name, "reflect-y") == 0 &&
             (1ULL << pr->enums[i].value) == DRM_MODE_REFLECT_Y)) {
            found++;
        }
    }
    drmModeFreeProperty(pr);
    return found == 2 ? id : 0;
}

/** Keep the connector's modes and EDID; return -1 when it has no mode. */
static int take_connector(struct kms *k, drmModeConnector *c)
{
    uint64_t blob_id;
    uint32_t i;

    k->conn = c->connector_id;
    k->nmodes = 0;
    k->preferred = 0;
    for (i = 0; i < (uint32_t)c->count_modes && k->nmodes < MAX_MODES; i++) {
        if (c->modes[i].type & DRM_MODE_TYPE_PREFERRED) {
            k->preferred = k->nmodes;
        }
        k->modes[k->nmodes++] = c->modes[i];
    }
    blob_id = prop_value(k->fd, c->connector_id, DRM_MODE_OBJECT_CONNECTOR, "EDID", 0);
    if (blob_id != 0) {
        drmModePropertyBlobRes *b = drmModeGetPropertyBlob(k->fd, (uint32_t)blob_id);

        if (b && b->length <= sizeof(k->edid)) {
            memcpy(k->edid, b->data, b->length);
            k->edid_len = b->length;
        }
        drmModeFreePropertyBlob(b);
    }
    return k->nmodes ? 0 : -1;
}

/** Choose a crtc the connector's encoders reach; return -1 when none. */
static int take_crtc(struct kms *k, drmModeRes *res, drmModeConnector *c)
{
    int i, j;

    for (i = 0; i < c->count_encoders; i++) {
        drmModeEncoder *e = drmModeGetEncoder(k->fd, c->encoders[i]);

        for (j = 0; e && j < res->count_crtcs; j++) {
            if (e->possible_crtcs & (1u << j)) {
                k->crtc = res->crtcs[j];
                drmModeFreeEncoder(e);
                return j;
            }
        }
        drmModeFreeEncoder(e);
    }
    return -1;
}

/** Find the primary plane of the crtc at index pipe. */
static int take_plane(struct kms *k, int pipe)
{
    drmModePlaneRes *pr = drmModeGetPlaneResources(k->fd);
    uint32_t i;

    for (i = 0; pr && i < pr->count_planes && k->plane == 0; i++) {
        drmModePlane *p = drmModeGetPlane(k->fd, pr->planes[i]);

        if (p && (p->possible_crtcs & (1u << pipe)) &&
            prop_value(k->fd, p->plane_id, DRM_MODE_OBJECT_PLANE, "type", ~0ULL) ==
                DRM_PLANE_TYPE_PRIMARY) {
            k->plane = p->plane_id;
        }
        drmModeFreePlane(p);
    }
    drmModeFreePlaneResources(pr);
    return k->plane ? 0 : -1;
}

/** Find the display's objects: the first connected connector and the rest. */
static int find_objects(struct kms *k)
{
    drmModeRes *res = drmModeGetResources(k->fd);
    int i, pipe = -1;

    for (i = 0; res && i < res->count_connectors; i++) {
        drmModeConnector *c = drmModeGetConnector(k->fd, res->connectors[i]);

        if (c && c->connector_type == DRM_MODE_CONNECTOR_WRITEBACK) {
            k->wb_conn = c->connector_id;
        } else if (c && k->conn == 0 && c->connection == DRM_MODE_CONNECTED &&
                   take_connector(k, c) == 0) {
            pipe = take_crtc(k, res, c);
        }
        drmModeFreeConnector(c);
    }
    drmModeFreeResources(res);
    if (k->conn == 0 || pipe < 0 || take_plane(k, pipe) != 0) {
        return -1;
    }
    return 0;
}

/** Find every property id the commits use. */
static int find_props(struct kms *k)
{
    struct props *p = &k->p;
    int fd = k->fd;

    p->conn_crtc = prop(fd, k->conn, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");
    p->crtc_mode = prop(fd, k->crtc, DRM_MODE_OBJECT_CRTC, "MODE_ID");
    p->crtc_active = prop(fd, k->crtc, DRM_MODE_OBJECT_CRTC, "ACTIVE");
    p->fb = prop(fd, k->plane, DRM_MODE_OBJECT_PLANE, "FB_ID");
    p->crtc = prop(fd, k->plane, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
    p->src_x = prop(fd, k->plane, DRM_MODE_OBJECT_PLANE, "SRC_X");
    p->src_y = prop(fd, k->plane, DRM_MODE_OBJECT_PLANE, "SRC_Y");
    p->src_w = prop(fd, k->plane, DRM_MODE_OBJECT_PLANE, "SRC_W");
    p->src_h = prop(fd, k->plane, DRM_MODE_OBJECT_PLANE, "SRC_H");
    p->crtc_x = prop(fd, k->plane, DRM_MODE_OBJECT_PLANE, "CRTC_X");
    p->crtc_y = prop(fd, k->plane, DRM_MODE_OBJECT_PLANE, "CRTC_Y");
    p->crtc_w = prop(fd, k->plane, DRM_MODE_OBJECT_PLANE, "CRTC_W");
    p->crtc_h = prop(fd, k->plane, DRM_MODE_OBJECT_PLANE, "CRTC_H");
    p->rotation = reflect_prop(fd, k->plane);
    if (k->wb_conn) {
        p->wb_crtc = prop(fd, k->wb_conn, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");
        p->wb_fb = prop(fd, k->wb_conn, DRM_MODE_OBJECT_CONNECTOR, "WRITEBACK_FB_ID");
        p->wb_fence = prop(fd, k->wb_conn, DRM_MODE_OBJECT_CONNECTOR, "WRITEBACK_OUT_FENCE_PTR");
    }
    return p->conn_crtc && p->crtc_mode && p->crtc_active && p->fb && p->crtc && p->src_w &&
                   p->crtc_w
               ? 0
               : -1;
}

/**
 * Take the KMS device fd, opened from path, as its master and find its
 * display; fd is the display's from then on. direct lets a resource's
 * dma-buf be scanned out without a copy; writeback checks each frame
 * through the device's writeback connector, which vkms has with
 * enable_writeback=1. Returns the display, or 0 with a message, fd closed.
 */
struct kms *kms_open(int fd, const char *path, struct vr *r, int direct, int writeback)
{
    struct kms *k = calloc(1, sizeof(*k));

    if (k == 0) {
        close(fd);
        return 0;
    }
    k->fd = fd;
    k->vr = r;
    k->direct = direct;
    k->writeback = writeback;
    k->mode = -1;
    if (drmSetMaster(k->fd) != 0 && drmIsMaster(k->fd) == 0) {
        fprintf(stderr, "backend: cannot be the master of %s: %s\n", path, strerror(errno));
        goto fail;
    }
    if (drmSetClientCap(k->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) != 0 ||
        drmSetClientCap(k->fd, DRM_CLIENT_CAP_ATOMIC, 1) != 0) {
        fprintf(stderr, "backend: %s has no atomic modesetting\n", path);
        goto fail;
    }
    drmSetClientCap(k->fd, DRM_CLIENT_CAP_WRITEBACK_CONNECTORS, 1);
    if (find_objects(k) != 0 || find_props(k) != 0) {
        fprintf(stderr, "backend: %s has no connected display\n", path);
        goto fail;
    }
    if (writeback && (k->wb_conn == 0 || k->p.wb_fb == 0 || k->p.wb_fence == 0)) {
        fprintf(stderr, "backend: %s has no writeback connector\n", path);
        goto fail;
    }
    return k;

fail:
    close(k->fd);
    free(k);
    return 0;
}

/** Copy the connector's EDID; -1 when it has none. */
int kms_edid(struct kms *k, uint8_t *out, uint32_t *size)
{
    if (k->edid_len == 0) {
        return VD_RESP_ERR_UNSPEC;
    }
    memcpy(out, k->edid, k->edid_len);
    *size = k->edid_len;
    return 0;
}

/** Report the connector's preferred mode as scanout 0's. */
int kms_display(struct kms *k, struct vd_mode *m)
{
    m->width = k->modes[k->preferred].hdisplay;
    m->height = k->modes[k->preferred].vdisplay;
    m->enabled = 1;
    return 0;
}

/**
 * Choose the mode for a scanout of w by h: one of that size, the preferred
 * one first, else the smallest that holds it, else the preferred one.
 */
static int choose_mode(struct kms *k, uint32_t w, uint32_t h)
{
    uint64_t best_area = ~0ULL;
    int best = -1;
    uint32_t i;

    if (k->modes[k->preferred].hdisplay == w && k->modes[k->preferred].vdisplay == h) {
        return (int)k->preferred;
    }
    for (i = 0; i < k->nmodes; i++) {
        uint64_t area = (uint64_t)k->modes[i].hdisplay * k->modes[i].vdisplay;

        if (k->modes[i].hdisplay == w && k->modes[i].vdisplay == h) {
            return (int)i;
        }
        if (k->modes[i].hdisplay >= w && k->modes[i].vdisplay >= h && area < best_area) {
            best_area = area;
            best = (int)i;
        }
    }
    return best >= 0 ? best : (int)k->preferred;
}

/** Free a framebuffer: its id, mapping and handle. */
static void fb_free(struct kms *k, struct fb *f)
{
    if (f->id) {
        drmModeRmFB(k->fd, f->id);
    }
    if (f->map) {
        munmap(f->map, f->size);
    }
    if (f->handle) {
        drmCloseBufferHandle(k->fd, f->handle);
    }
    memset(f, 0, sizeof(*f));
}

/** Make a mapped XRGB8888 dumb buffer of w by h as a framebuffer. */
static int fb_dumb(struct kms *k, struct fb *f, uint32_t w, uint32_t h)
{
    uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};
    uint64_t off;

    memset(f, 0, sizeof(*f));
    if (drmModeCreateDumbBuffer(k->fd, w, h, 32, 0, &f->handle, &f->pitch, &f->size) != 0) {
        return -1;
    }
    f->width = w;
    f->height = h;
    handles[0] = f->handle;
    pitches[0] = f->pitch;
    if (drmModeAddFB2(k->fd, w, h, DRM_FORMAT_XRGB8888, handles, pitches, offsets, &f->id, 0) !=
            0 ||
        drmModeMapDumbBuffer(k->fd, f->handle, &off) != 0) {
        fb_free(k, f);
        return -1;
    }
    f->map = mmap(0, f->size, PROT_READ | PROT_WRITE, MAP_SHARED, k->fd, off);
    if (f->map == MAP_FAILED) {
        f->map = 0;
        fb_free(k, f);
        return -1;
    }
    memset(f->map, 0, f->size);
    return 0;
}

/** Add the primary plane's properties showing f at the top left,
 * reflected in y when its rows are stored bottom first. */
static void plane_props(struct kms *k, drmModeAtomicReq *req, const struct fb *f, uint32_t w,
                        uint32_t h)
{
    drmModeAtomicAddProperty(req, k->plane, k->p.fb, f ? f->id : 0);
    drmModeAtomicAddProperty(req, k->plane, k->p.crtc, f ? k->crtc : 0);
    drmModeAtomicAddProperty(req, k->plane, k->p.src_x, 0);
    drmModeAtomicAddProperty(req, k->plane, k->p.src_y, 0);
    drmModeAtomicAddProperty(req, k->plane, k->p.src_w, (uint64_t)w << 16);
    drmModeAtomicAddProperty(req, k->plane, k->p.src_h, (uint64_t)h << 16);
    drmModeAtomicAddProperty(req, k->plane, k->p.crtc_x, 0);
    drmModeAtomicAddProperty(req, k->plane, k->p.crtc_y, 0);
    drmModeAtomicAddProperty(req, k->plane, k->p.crtc_w, w);
    drmModeAtomicAddProperty(req, k->plane, k->p.crtc_h, h);
    if (k->p.rotation) {
        drmModeAtomicAddProperty(req, k->plane, k->p.rotation,
                                 DRM_MODE_ROTATE_0 | (f && f->flip ? DRM_MODE_REFLECT_Y : 0));
    }
}

/** Wait up to a second for a sync file to signal; return 0 when it did. */
static int wait_fence(int fd)
{
    struct pollfd p = {fd, POLLIN, 0};

    return poll(&p, 1, 1000) == 1 ? 0 : -1;
}

/**
 * Report the CRC-32 of the frame's top-left w by h pixels, as the
 * writeback connector copied them, X bytes set to 0xff as the driver's
 * patterns have them.
 */
static void report_writeback(struct kms *k, uint32_t w, uint32_t h)
{
    uint8_t *row = malloc((size_t)w * 4), *all = malloc((size_t)w * h * 4);
    uint32_t y, x;

    if (row == 0 || all == 0) {
        free(row);
        free(all);
        return;
    }
    for (y = 0; y < h; y++) {
        memcpy(row, k->wb.map + (uint64_t)y * k->wb.pitch, (size_t)w * 4);
        for (x = 0; x < w; x++) {
            row[x * 4 + 3] = 0xff;
        }
        memcpy(all + (uint64_t)y * w * 4, row, (size_t)w * 4);
    }
    fprintf(stderr, "backend: scanout: writeback %ux%u of mode %ux%u, crc %08x\n", (unsigned)w,
            (unsigned)h, (unsigned)k->modes[k->mode].hdisplay, (unsigned)k->modes[k->mode].vdisplay,
            (unsigned)crc32_bytes(all, (uint64_t)w * h * 4));
    k->n.writebacks++;
    free(row);
    free(all);
}

/**
 * Commit f on the primary plane, with a modeset first when the mode
 * changed, and wait for it; when check is set and writeback is on, copy
 * the frame back and report its CRC. Returns 0 or a VD_RESP_ERR_*.
 */
static int commit(struct kms *k, const struct fb *f, int modeset, int check)
{
    drmModeModeInfo *m = &k->modes[k->mode];
    drmModeAtomicReq *req = drmModeAtomicAlloc();
    uint32_t flags = modeset ? DRM_MODE_ATOMIC_ALLOW_MODESET : 0;
    int32_t fence = -1;
    int err;

    if (req == 0) {
        return VD_RESP_ERR_OUT_OF_MEMORY;
    }
    if (modeset) {
        drmModeAtomicAddProperty(req, k->conn, k->p.conn_crtc, k->crtc);
        drmModeAtomicAddProperty(req, k->crtc, k->p.crtc_mode, k->blob);
        drmModeAtomicAddProperty(req, k->crtc, k->p.crtc_active, 1);
    }
    plane_props(k, req, f, m->hdisplay, m->vdisplay);
    if (check && k->writeback && k->wb.id) {
        drmModeAtomicAddProperty(req, k->wb_conn, k->p.wb_crtc, k->crtc);
        drmModeAtomicAddProperty(req, k->wb_conn, k->p.wb_fb, k->wb.id);
        drmModeAtomicAddProperty(req, k->wb_conn, k->p.wb_fence, (uint64_t)(uintptr_t)&fence);
        flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;
    }
    err = drmModeAtomicCommit(k->fd, req, flags, 0);
    drmModeAtomicFree(req);
    if (err != 0) {
        fprintf(stderr, "backend: kms: commit failed: %s\n", strerror(errno));
        return VD_RESP_ERR_UNSPEC;
    }
    k->n.flips++;
    if (modeset) {
        k->n.modesets++;
    }
    if (fence >= 0) {
        if (wait_fence(fence) == 0) {
            report_writeback(k, k->rect.width < m->hdisplay ? k->rect.width : m->hdisplay,
                             k->rect.height < m->vdisplay ? k->rect.height : m->vdisplay);
        }
        close(fence);
    }
    return 0;
}

/** Set the mode at index i: its blob and its two dumb buffers. */
static int set_mode(struct kms *k, int i)
{
    drmModeModeInfo *m = &k->modes[i];

    fb_free(k, &k->buf[0]);
    fb_free(k, &k->buf[1]);
    fb_free(k, &k->wb);
    if (k->blob) {
        drmModeDestroyPropertyBlob(k->fd, k->blob);
        k->blob = 0;
    }
    if (drmModeCreatePropertyBlob(k->fd, m, sizeof(*m), &k->blob) != 0 ||
        fb_dumb(k, &k->buf[0], m->hdisplay, m->vdisplay) != 0 ||
        fb_dumb(k, &k->buf[1], m->hdisplay, m->vdisplay) != 0 ||
        (k->writeback && fb_dumb(k, &k->wb, m->hdisplay, m->vdisplay) != 0)) {
        fprintf(stderr, "backend: kms: no buffers for %ux%u\n", (unsigned)m->hdisplay,
                (unsigned)m->vdisplay);
        k->mode = -1;
        return VD_RESP_ERR_OUT_OF_MEMORY;
    }
    k->mode = i;
    k->cur = 0;
    fprintf(stderr, "backend: kms: mode %ux%u for a scanout of %ux%u\n", (unsigned)m->hdisplay,
            (unsigned)m->vdisplay, (unsigned)k->rect.width, (unsigned)k->rect.height);
    return commit(k, &k->buf[0], 1, 0);
}

/**
 * Point the display at a resource's rectangle, choosing the mode for its
 * size; resource 0 takes the plane off the crtc. The frame shows on the
 * next flush.
 */
int kms_scanout(struct kms *k, uint32_t id, const struct vd_rect *r)
{
    int want;

    if (id == 0) {
        k->res = 0;
        if (k->mode >= 0) {
            return commit(k, 0, 0, 0);
        }
        return 0;
    }
    k->res = id;
    k->rect = *r;
    k->refused = 0;
    want = choose_mode(k, r->width, r->height);
    if (want != k->mode) {
        return set_mode(k, want);
    }
    return 0;
}

/** Say once per resource why it is copied instead of scanned out. */
static int refuse(struct kms *k, uint32_t id, const char *why)
{
    if (k->refused != id) {
        fprintf(stderr, "backend: kms: resource %u is copied: %s\n", (unsigned)id, why);
        k->refused = id;
    }
    return -1;
}

/**
 * Import the resource's dma-buf as a framebuffer that fills the mode; one
 * stored bottom row first needs a plane that reflects it in y. The handle
 * the import makes is the display's own, since GBM has its own open of the
 * card. Returns 0, or -1 when the resource must be copied.
 */
static int import(struct kms *k, uint32_t id)
{
    uint32_t handles[4] = {0}, pitches[4] = {0}, offsets[4] = {0};
    uint64_t mods[4] = {0};
    drmModeModeInfo *m = &k->modes[k->mode];
    struct vr_export e;
    int err;

    if (k->imp.res == id && k->imp.id) {
        return 0;
    }
    fb_free(k, &k->imp);
    if (vr_export(k->vr, id, &e) != 0) {
        return refuse(k, id, "it has no dma-buf");
    }
    if (e.width != m->hdisplay || e.height != m->vdisplay || k->rect.x != 0 || k->rect.y != 0) {
        close(e.fd);
        return refuse(k, id, "it does not fill the mode");
    }
    if (e.bottom_up && k->p.rotation == 0) {
        close(e.fd);
        return refuse(k, id, "its rows are bottom first and the plane cannot reflect them");
    }
    err = drmPrimeFDToHandle(k->fd, e.fd, &k->imp.handle);
    close(e.fd);
    if (err != 0) {
        return refuse(k, id, "the display cannot import its dma-buf");
    }
    handles[0] = k->imp.handle;
    pitches[0] = e.stride;
    offsets[0] = e.offset;
    /* Without DRM_MODE_FB_MODIFIERS the kernel wants every modifier 0. */
    mods[0] = e.modifier != DRM_FORMAT_MOD_INVALID ? e.modifier : 0;
    err = drmModeAddFB2WithModifiers(
        k->fd, e.width, e.height, e.fourcc, handles, pitches, offsets, mods, &k->imp.id,
        e.modifier != DRM_FORMAT_MOD_INVALID ? DRM_MODE_FB_MODIFIERS : 0);
    if (err != 0) {
        fb_free(k, &k->imp);
        return refuse(k, id, "the display takes no framebuffer of its format");
    }
    k->imp.res = id;
    k->imp.flip = e.bottom_up;
    k->imp.width = e.width;
    k->imp.height = e.height;
    return 0;
}

/**
 * Show a flushed resource if it is the scanout's: its own buffer when
 * direct scanout is on and it exports one that fills the mode, else a copy
 * of the scanout's rectangle into the dumb buffer not shown, which then is.
 */
int kms_flush(struct kms *k, uint32_t id, const struct vd_rect *r)
{
    drmModeModeInfo *m;
    struct vd_rect src;
    struct fb *f;
    int err;

    (void)r;
    if (id != k->res || k->mode < 0) {
        return 0;
    }
    k->n.flushes++;
    m = &k->modes[k->mode];
    if (k->direct && k->refused != id && import(k, id) == 0) {
        k->n.direct++;
        return commit(k, &k->imp, 0, 1);
    }
    f = &k->buf[k->cur ^ 1];
    src = k->rect;
    if (src.width > m->hdisplay) {
        src.width = m->hdisplay;
    }
    if (src.height > m->vdisplay) {
        src.height = m->vdisplay;
    }
    if ((err = vr_read(k->vr, id, &src, f->map, f->pitch)) != 0) {
        k->n.skipped++;
        return err;
    }
    k->n.copies++;
    if ((err = commit(k, f, 0, 1)) == 0) {
        k->cur ^= 1;
    }
    return err;
}

/** Drop the import of a resource that is going away. */
void kms_forget(struct kms *k, uint32_t id)
{
    if (k->imp.res == id) {
        if (k->res == id && k->mode >= 0) {
            commit(k, 0, 0, 0);
        }
        fb_free(k, &k->imp);
    }
    if (k->res == id) {
        k->res = 0;
    }
}

/** Take the plane off and forget the scanout, for a new epoch. */
void kms_reset(struct kms *k)
{
    if (k->mode >= 0 && k->res != 0) {
        commit(k, 0, 0, 0);
    }
    k->res = 0;
    fb_free(k, &k->imp);
}

/** Return the display's counters. */
const struct kms_counts *kms_counts(struct kms *k)
{
    return &k->n;
}

/** Free the buffers and give up the device. */
void kms_close(struct kms *k)
{
    if (k == 0) {
        return;
    }
    fb_free(k, &k->imp);
    fb_free(k, &k->buf[0]);
    fb_free(k, &k->buf[1]);
    fb_free(k, &k->wb);
    if (k->blob) {
        drmModeDestroyPropertyBlob(k->fd, k->blob);
    }
    drmDropMaster(k->fd);
    close(k->fd);
    free(k);
}
