/*
 * virgl.c: the backend's renderer, libvirglrenderer behind the operations
 * device.c calls, as QEMU's virtio-gpu-virgl.c drives it, and the display
 * operations passed on to kms.c.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <virgl/virglrenderer.h>

#include "backend.h"
#include "vgpushm_uapi.h"

#define CAPSET_VIRGL  1u
#define CAPSET_VIRGL2 2u
#define CAPSET_VENUS  4u

/* virglrenderer's flag for a resource whose first row is the top one. */
#define RESOURCE_Y_0_TOP 1u

/* The Venus contexts, whose attached resources the probe imports. */
#define VENUS_CTXS 16

/* The blobs pinned for page lists at once, one per resource at most. */
#define PINS VD_OBJS

/* A blob pinned through the module: its resource and the pin's handle. */
struct vr_pin {
    uint32_t id, handle;
    uint32_t map_info; /* the cache type virglrenderer gave at the pin */
};

struct vr {
    struct vd *dev;
    struct kms *kms;
    int gbm_fd; /* the DRM device virglrenderer's GBM allocates on, or -1 */
    uint32_t capset[3];
    uint32_t ncapsets;
    uint32_t venus[VENUS_CTXS]; /* live Venus contexts' ids, 0 when free */
    int probe;                  /* import each resource a Venus context takes into Vulkan */
    int verbose;
    int module; /* the vgpushm module's node, which pins blobs, or -1 */
    struct vr_pin pins[PINS];
    uint32_t npins;
    struct vr_pin_counts pc;
};

/* virglrenderer calls back with the cookie vr_init passed. */
static struct virgl_renderer_callbacks callbacks;

/** Retire the global timeline's fences up to fence. */
static void write_fence(void *cookie, uint32_t fence)
{
    struct vr *r = cookie;

    vd_fence_retired(r->dev, 0, 0, 0, fence);
}

/** Retire a context ring's fences up to fence_id. */
static void write_context_fence(void *cookie, uint32_t ctx, uint32_t ring, uint64_t fence_id)
{
    struct vr *r = cookie;

    vd_fence_retired(r->dev, ctx, 1, ring, fence_id);
}

/** Give virglrenderer its own descriptor of the GBM device, or none. */
static int get_drm_fd(void *cookie)
{
    struct vr *r = cookie;

    return r->gbm_fd >= 0 ? fcntl(r->gbm_fd, F_DUPFD_CLOEXEC, 0) : -1;
}

/** Write virglrenderer's log lines to stderr as the backend's. */
static void log_line(enum virgl_log_level_flags level, const char *message, void *data)
{
    size_t n = strlen(message);

    (void)data;
    fprintf(stderr, "backend: virgl: %s%s%s", level >= VIRGL_LOG_LEVEL_ERROR ? "error: " : "",
            message, n && message[n - 1] == '\n' ? "" : "\n");
}

/**
 * Start virglrenderer with EGL: on GBM over gbm_fd when it is a DRM
 * device, so that resources it allocates there export as dma-bufs, or
 * surfaceless when gbm_fd is -1; with Venus through the render server
 * when venus is set. Returns the renderer, or 0 when virglrenderer fails.
 */
struct vr *vr_init(struct vd *dev, int gbm_fd, int venus, int verbose)
{
    static const uint32_t ids[] = {CAPSET_VIRGL, CAPSET_VIRGL2, CAPSET_VENUS};
    struct vr *r = calloc(1, sizeof(*r));
    int flags = VIRGL_RENDERER_USE_EGL | VIRGL_RENDERER_THREAD_SYNC, err;
    uint32_t i, ver, size;

    if (r == 0) {
        return 0;
    }
    r->dev = dev;
    r->gbm_fd = gbm_fd;
    r->verbose = verbose;
    r->module = -1;
    if (gbm_fd < 0) {
        flags |= VIRGL_RENDERER_USE_SURFACELESS;
    }
    if (venus) {
        flags |= VIRGL_RENDERER_VENUS | VIRGL_RENDERER_RENDER_SERVER;
    }
    virgl_set_log_callback(log_line, 0, 0);
    callbacks.version = 3;
    callbacks.write_fence = write_fence;
    callbacks.get_drm_fd = get_drm_fd;
    callbacks.write_context_fence = write_context_fence;
    if ((err = virgl_renderer_init(r, flags, &callbacks)) != 0) {
        fprintf(stderr, "backend: virglrenderer failed to start: %d\n", err);
        free(r);
        return 0;
    }
    for (i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        virgl_renderer_get_cap_set(ids[i], &ver, &size);
        /* Venus reports version 0; QEMU too takes a capset by its size.
           virglrenderer sizes Venus's capset even when Venus is off. */
        if (size > 0 && (ids[i] != CAPSET_VENUS || venus)) {
            r->capset[r->ncapsets++] = ids[i];
        }
    }
    return r;
}

/** Return the descriptor that is readable when fences may have retired. */
int vr_poll_fd(struct vr *r)
{
    (void)r;
    return virgl_renderer_get_poll_fd();
}

/** Retire the fences that have signalled, on every timeline. */
void vr_poll(struct vr *r)
{
    (void)r;
    virgl_renderer_poll();
}

/** Return the number of capsets the renderer offers. */
uint32_t vr_capsets(struct vr *r)
{
    return r->ncapsets;
}

/** Import each resource a Venus context takes into Vulkan, with -I. */
void vr_set_probe(struct vr *r, int probe)
{
    r->probe = probe;
}

/** Give the renderer its display, or none. */
void vr_set_kms(struct vr *r, struct kms *k)
{
    r->kms = k;
}

/** Map virglrenderer's errno results to virtio-gpu's responses. */
static int resp(int err)
{
    if (err == 0) {
        return 0;
    }
    if (err == ENOMEM || err == -ENOMEM) {
        return VD_RESP_ERR_OUT_OF_MEMORY;
    }
    if (err == EINVAL || err == -EINVAL) {
        return VD_RESP_ERR_PARAMETER;
    }
    return VD_RESP_ERR_UNSPEC;
}

/** Create a resource without backing; device.c attaches it later. */
static int op_create(void *c, const struct vd_create *a)
{
    struct virgl_renderer_resource_create_args args;

    (void)c;
    args.handle = a->id;
    args.target = a->target;
    args.format = a->format;
    args.bind = a->bind;
    args.width = a->width;
    args.height = a->height;
    args.depth = a->depth;
    args.array_size = a->array_size;
    args.last_level = a->last_level;
    args.nr_samples = a->nr_samples;
    args.flags = a->flags;
    return resp(virgl_renderer_resource_create(&args, 0, 0));
}

/** Drop a resource, and its framebuffer on the display. */
static void op_unref(void *c, uint32_t id)
{
    struct vr *r = c;

    if (r->kms) {
        kms_forget(r->kms, id);
    }
    virgl_renderer_resource_unref(id);
}

/** Attach backing in the region; device.c keeps the list. */
static int op_attach(void *c, uint32_t id, struct iovec *iov, uint32_t n)
{
    (void)c;
    return resp(virgl_renderer_resource_attach_iov((int)id, iov, (int)n));
}

/** Detach a resource's backing. */
static void op_detach(void *c, uint32_t id)
{
    (void)c;
    virgl_renderer_resource_detach_iov((int)id, 0, 0);
}

/*
 * virglrenderer's struct virgl_box, which its public header declares but
 * does not define, has struct vd_box's six 32-bit fields in its order, as
 * virtio-gpu's box has; QEMU passes virtio-gpu's box the same way.
 */
#define VIRGL_BOX(b) ((struct virgl_box *)(b))

/** Transfer between a resource and its backing, in context t->ctx. */
static int op_transfer(void *c, const struct vd_transfer *t)
{
    struct vd_box box = t->box;

    (void)c;
    if (t->to_host) {
        return resp(virgl_renderer_transfer_write_iov(t->id, t->ctx, (int)t->level, t->stride,
                                                      t->layer_stride, VIRGL_BOX(&box), t->offset,
                                                      0, 0));
    }
    return resp(virgl_renderer_transfer_read_iov(
        t->id, t->ctx, t->level, t->stride, t->layer_stride, VIRGL_BOX(&box), t->offset, 0, 0));
}

/** Create a blob resource: guest memory in the region, or a context's. */
static int op_blob(void *c, const struct vd_blob *a, struct iovec *iov, uint32_t n)
{
    struct virgl_renderer_resource_create_blob_args args;

    (void)c;
    memset(&args, 0, sizeof(args));
    args.res_handle = a->id;
    args.ctx_id = a->ctx;
    args.blob_mem = a->mem;
    args.blob_flags = a->flags;
    args.blob_id = a->blob_id;
    args.size = a->size;
    args.iovecs = iov;
    args.num_iovs = n;
    return resp(virgl_renderer_resource_create_blob(&args));
}

/** Return the slot of Venus context ctx, or of a free one for ctx 0;
 * -1 when there is none. */
static int venus_slot(struct vr *r, uint32_t ctx)
{
    int i;

    for (i = 0; i < VENUS_CTXS; i++) {
        if (r->venus[i] == ctx) {
            return i;
        }
    }
    return -1;
}

/** Create a context, of a capset when context_init chose one. */
static int op_ctx_create(void *c, uint32_t ctx, uint32_t capset, uint32_t nlen, const char *name)
{
    struct vr *r = c;
    int err, i;

    if (capset == 0) {
        return resp(virgl_renderer_context_create(ctx, nlen, name));
    }
    err = resp(virgl_renderer_context_create_with_flags(ctx, capset, nlen, name));
    if (err == 0 && capset == CAPSET_VENUS && (i = venus_slot(r, 0)) >= 0) {
        r->venus[i] = ctx;
    }
    return err;
}

/** Destroy a context. */
static void op_ctx_destroy(void *c, uint32_t ctx)
{
    struct vr *r = c;
    int i = venus_slot(r, ctx);

    if (i >= 0) {
        r->venus[i] = 0;
    }
    virgl_renderer_context_destroy(ctx);
}

/**
 * Import a resource that a Venus context took into Vulkan as a dma-buf,
 * as Venus's renderer does when the driver allocates memory from it, and
 * report the CRC-32 of its rows as stored, X bytes set to 0xff.
 */
static void venus_probe(uint32_t id)
{
    struct vr_export e;
    uint32_t crc;

    if (vr_export(0, id, &e) != 0) {
        fprintf(stderr, "backend: venus: resource %u exports no dma-buf\n", (unsigned)id);
        return;
    }
    if (vk_probe(&e, &crc) == 0) {
        fprintf(stderr, "backend: venus: imported resource %u, %ux%u, crc %08x\n", (unsigned)id,
                (unsigned)e.width, (unsigned)e.height, (unsigned)crc);
    }
    close(e.fd);
}

/** Attach a resource to a context, or detach it. */
static void op_ctx_resource(void *c, uint32_t ctx, uint32_t id, int attach)
{
    struct vr *r = c;

    if (attach) {
        virgl_renderer_ctx_attach_resource((int)ctx, (int)id);
        if (r->probe && venus_slot(r, ctx) >= 0) {
            venus_probe(id);
        }
    } else {
        virgl_renderer_ctx_detach_resource((int)ctx, (int)id);
    }
}

/** Submit a context's command stream from the device's private copy. */
static int op_submit(void *c, uint32_t ctx, void *buf, uint32_t bytes)
{
    (void)c;
    return resp(virgl_renderer_submit_cmd(buf, (int)ctx, (int)(bytes / 4)));
}

/** Fence the work so far on the global timeline or a context's ring. */
static int op_fence(void *c, uint32_t ctx, int per_ctx, uint32_t ring, uint64_t id)
{
    (void)c;
    if (per_ctx) {
        return virgl_renderer_context_create_fence(ctx, 0, ring, id);
    }
    return virgl_renderer_create_fence((int)(uint32_t)id, 0);
}

/** Name the capset at index and its sizes. */
static void op_capset_info(void *c, uint32_t index, uint32_t *id, uint32_t *max_ver,
                           uint32_t *max_size)
{
    struct vr *r = c;

    *id = r->capset[index];
    virgl_renderer_get_cap_set(*id, max_ver, max_size);
}

/** Fill a capset of size bytes. */
static int op_capset(void *c, uint32_t id, uint32_t ver, void *out, uint32_t size)
{
    (void)c;
    memset(out, 0, size);
    virgl_renderer_fill_caps(id, ver, out);
    return 0;
}

/** Report scanout 0 as the display's mode, or 1024 by 768 without one. */
static int op_display(void *c, uint32_t scanout, struct vd_mode *m)
{
    struct vr *r = c;

    if (scanout != 0) {
        return VD_RESP_ERR_SCANOUT_ID;
    }
    if (r->kms) {
        return kms_display(r->kms, m);
    }
    m->width = 1024;
    m->height = 768;
    m->enabled = 1;
    return 0;
}

/** Copy the display's EDID. */
static int op_edid(void *c, uint32_t scanout, uint8_t *out, uint32_t *size)
{
    struct vr *r = c;

    if (scanout != 0 || r->kms == 0) {
        return VD_RESP_ERR_PARAMETER;
    }
    return kms_edid(r->kms, out, size);
}

/** Point the display at a resource's rectangle, or turn it off. */
static int op_scanout(void *c, uint32_t scanout, uint32_t id, const struct vd_rect *rect)
{
    struct vr *r = c;

    if (scanout != 0) {
        return VD_RESP_ERR_SCANOUT_ID;
    }
    return r->kms ? kms_scanout(r->kms, id, rect) : 0;
}

/**
 * Show a flushed resource: on the display, or, without one, report the
 * CRC-32 of its pixels in the flushed rectangle, read back, one line each.
 */
static int op_flush(void *c, uint32_t scanout, uint32_t id, const struct vd_rect *rect)
{
    struct vr *r = c;
    uint8_t *px;
    int err;

    (void)scanout;
    if (r->kms) {
        return kms_flush(r->kms, id, rect);
    }
    if (rect->width == 0 || rect->height == 0 || rect->width > 16384 || rect->height > 16384) {
        return VD_RESP_ERR_PARAMETER;
    }
    px = malloc((size_t)rect->width * rect->height * 4);
    if (px == 0) {
        return VD_RESP_ERR_OUT_OF_MEMORY;
    }
    err = vr_read(r, id, rect, px, rect->width * 4);
    if (err == 0) {
        fprintf(stderr, "backend: flush: resource %u, %ux%u at %u,%u, crc %08x\n", (unsigned)id,
                (unsigned)rect->width, (unsigned)rect->height, (unsigned)rect->x, (unsigned)rect->y,
                (unsigned)crc32_bytes(px, (uint64_t)rect->width * rect->height * 4));
    }
    free(px);
    return err;
}

/** The pin of resource id, or 0. */
static struct vr_pin *pin_find(struct vr *r, uint32_t id)
{
    uint32_t i;

    for (i = 0; i < r->npins; i++) {
        if (r->pins[i].id == id) {
            return &r->pins[i];
        }
    }
    return 0;
}

/** Let go of a pin in the module and drop it from the table. */
static void pin_drop(struct vr *r, struct vr_pin *p)
{
    uint32_t handle = p->handle;

    if (ioctl(r->module, VGPUSHM_IOC_UNPIN, &handle) != 0) {
        fprintf(stderr, "backend: unpin: resource %u: %s\n", (unsigned)p->id, strerror(errno));
    }
    r->pc.unpinned++;
    *p = r->pins[--r->npins];
}

/**
 * Pin blob q->id through the module: its memory exported by virglrenderer
 * as a dma-buf or a memfd, as Venus and the shared-memory blobs give it,
 * and pinned whole for the driver's epoch. Returns the pin, or 0 for a
 * blob that cannot be exported or pinned, as a GPU's own memory cannot.
 */
static struct vr_pin *pin_new(struct vr *r, const struct vd_pages *q)
{
    struct vgpushm_pin pin;
    uint32_t fd_type = 0, info = 0;
    struct vr_pin *p;
    int fd = -1, err;

    if (r->module < 0 || r->npins == PINS) {
        r->pc.refused++;
        return 0;
    }
    if ((err = virgl_renderer_resource_export_blob(q->id, &fd_type, &fd)) != 0) {
        fprintf(stderr, "backend: map: resource %u: refused: not exported: %d\n", (unsigned)q->id,
                err);
        r->pc.refused++;
        return 0;
    }
    if ((err = virgl_renderer_resource_get_map_info(q->id, &info)) != 0) {
        fprintf(stderr, "backend: map: resource %u: refused: no map info: %d\n", (unsigned)q->id,
                err);
        close(fd);
        r->pc.refused++;
        return 0;
    }
    memset(&pin, 0, sizeof(pin));
    pin.fd = fd;
    pin.epoch = q->epoch;
    pin.size = (q->size + 4095u) & ~(uint64_t)4095u;
    err = ioctl(r->module, VGPUSHM_IOC_PIN, &pin) != 0 ? errno : 0;
    close(fd);
    if (err != 0) {
        fprintf(stderr, "backend: map: resource %u: refused: fd type %u: %s\n", (unsigned)q->id,
                (unsigned)fd_type, strerror(err));
        r->pc.refused++;
        return 0;
    }
    p = &r->pins[r->npins++];
    p->id = q->id;
    p->handle = pin.handle;
    p->map_info = info;
    r->pc.pinned++;
    if (r->verbose) {
        fprintf(stderr, "backend: pin: resource %u, fd type %u, %s, %llu pages\n", (unsigned)q->id,
                (unsigned)fd_type, pin.kind == VGPUSHM_PIN_DMABUF ? "dma-buf" : "shmem",
                (unsigned long long)pin.pages);
    }
    return p;
}

/** A mappable blob's page list from q->first: pinned at the first ask,
 * then read from the module. Returns 0 or VD_RESP_ERR_UNSPEC. */
static int op_pages(void *c, struct vd_pages *q)
{
    struct vr *r = c;
    struct vr_pin *p = pin_find(r, q->id);
    struct vgpushm_pages pg;

    if (p == 0 && (p = pin_new(r, q)) == 0) {
        return VD_RESP_ERR_UNSPEC;
    }
    memset(&pg, 0, sizeof(pg));
    pg.handle = p->handle;
    pg.max = q->max < VGPUSHM_PAGES_MAX_RUNS ? q->max : VGPUSHM_PAGES_MAX_RUNS;
    pg.first = q->first;
    /* struct vgs_run and struct vgpushm_run are both two u64s, address first. */
    pg.runs = (uint64_t)(uintptr_t)q->runs;
    if (ioctl(r->module, VGPUSHM_IOC_PAGES, &pg) != 0 || pg.count == 0) {
        fprintf(stderr, "backend: map: resource %u: pages from %llu: %s\n", (unsigned)q->id,
                (unsigned long long)q->first, strerror(errno));
        return VD_RESP_ERR_UNSPEC;
    }
    q->count = pg.count;
    q->map_info = p->map_info;
    return 0;
}

/** Let go of a blob's pin. */
static void op_unpin(void *c, uint32_t id)
{
    struct vr *r = c;
    struct vr_pin *p = pin_find(r, id);

    if (p) {
        pin_drop(r, p);
    }
}

/** End every context, resource and pin for a new epoch. */
static void op_reset(void *c)
{
    struct vr *r = c;

    while (r->npins) {
        pin_drop(r, &r->pins[0]);
    }
    if (r->kms) {
        kms_reset(r->kms);
    }
    memset(r->venus, 0, sizeof(r->venus));
    virgl_renderer_reset();
}

const struct vd_ops vr_ops = {
    .create = op_create,
    .unref = op_unref,
    .attach = op_attach,
    .detach = op_detach,
    .transfer = op_transfer,
    .blob = op_blob,
    .ctx_create = op_ctx_create,
    .ctx_destroy = op_ctx_destroy,
    .ctx_resource = op_ctx_resource,
    .submit = op_submit,
    .fence = op_fence,
    .capset_info = op_capset_info,
    .capset = op_capset,
    .display = op_display,
    .edid = op_edid,
    .scanout = op_scanout,
    .flush = op_flush,
    .reset = op_reset,
    .pages = op_pages,
    .unpin = op_unpin,
};

/** Pin blobs for page lists through the module's node fd. */
void vr_set_module(struct vr *r, int fd)
{
    r->module = fd;
}

/** What the page lists pinned and let go of. */
const struct vr_pin_counts *vr_pin_counts(struct vr *r)
{
    return &r->pc;
}

/** Reverse the order of n rows of len bytes, stride bytes apart. */
static int flip_rows(uint8_t *p, uint32_t stride, uint32_t len, uint32_t n)
{
    uint8_t *t = malloc(len);
    uint32_t i;

    if (t == 0) {
        return VD_RESP_ERR_OUT_OF_MEMORY;
    }
    for (i = 0; i < n / 2; i++) {
        memcpy(t, p + (size_t)stride * i, len);
        memcpy(p + (size_t)stride * i, p + (size_t)stride * (n - 1 - i), len);
        memcpy(p + (size_t)stride * (n - 1 - i), t, len);
    }
    free(t);
    return 0;
}

/**
 * Read a rectangle of a resource's pixels as a display shows them into
 * dst, whose rows are stride bytes apart, top row first. A resource stored
 * bottom row first, as GL draws, shows its last row at the top, as QEMU
 * shows it; rect counts from the top. Returns 0 or a VD_RESP_ERR_*.
 */
int vr_read(struct vr *r, uint32_t id, const struct vd_rect *rect, void *dst, uint32_t stride)
{
    struct vd_box box = {rect->x, rect->y, 0, rect->width, rect->height, 1};
    struct virgl_renderer_resource_info info;
    struct iovec iov;
    int bottom_up, err;

    (void)r;
    if (virgl_renderer_resource_get_info((int)id, &info) != 0) {
        return VD_RESP_ERR_RESOURCE_ID;
    }
    bottom_up = (info.flags & RESOURCE_Y_0_TOP) == 0;
    if (rect->y > info.height || rect->height > info.height - rect->y) {
        return VD_RESP_ERR_PARAMETER;
    }
    if (bottom_up) {
        box.y = info.height - rect->y - rect->height;
    }
    iov.iov_base = dst;
    iov.iov_len = (size_t)stride * rect->height;
    err = resp(virgl_renderer_transfer_read_iov(id, 0, 0, stride, 0, VIRGL_BOX(&box), 0, &iov, 1));
    if (err == 0 && bottom_up) {
        err = flip_rows(dst, stride, rect->width * 4, rect->height);
    }
    return err;
}

/**
 * Export a resource's single plane as a dma-buf. Returns 0, or -1 for a
 * resource that has none, as a GL texture virglrenderer did not allocate
 * through GBM has not. r may be 0.
 */
int vr_export(struct vr *r, uint32_t id, struct vr_export *e)
{
    struct virgl_renderer_export_query q;
    struct virgl_renderer_resource_info info;

    (void)r;
    if (virgl_renderer_resource_get_info((int)id, &info) != 0) {
        return -1;
    }
    memset(&q, 0, sizeof(q));
    q.hdr.stype = VIRGL_RENDERER_STRUCTURE_TYPE_EXPORT_QUERY;
    q.hdr.size = sizeof(q);
    q.in_resource_id = id;
    q.in_export_fds = 1;
    if (virgl_renderer_execute(&q, sizeof(q)) != 0 || q.out_num_fds != 1 || q.out_fourcc == 0) {
        return -1;
    }
    e->fd = q.out_fds[0];
    e->fourcc = q.out_fourcc;
    e->stride = q.out_strides[0];
    e->offset = q.out_offsets[0];
    e->modifier = q.out_modifier;
    e->width = info.width;
    e->height = info.height;
    e->bottom_up = (info.flags & RESOURCE_Y_0_TOP) == 0;
    return 0;
}
