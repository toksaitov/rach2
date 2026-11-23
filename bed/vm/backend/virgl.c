/*
 * virgl.c: the backend's renderer, libvirglrenderer behind the operations
 * device.c calls, as QEMU's virtio-gpu-virgl.c drives it, and the display
 * operations passed on to kms.c.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/uio.h>

#include <virglrenderer.h>

#include "backend.h"
#include "vgpushm_uapi.h"

#define CAPSET_VIRGL  1u
#define CAPSET_VIRGL2 2u
#define CAPSET_VENUS  4u
#define CAPSET_DRM    6u

/* Gallium's target of a buffer, as virgl's resources number targets. */
#define PIPE_BUFFER 0u

/* virglrenderer's flag for a resource whose first row is the top one. */
#define RESOURCE_Y_0_TOP 1u

/* The Venus contexts, whose attached resources the probe imports. */
#define VENUS_CTXS 16

/* The blobs and backings pinned for page lists at once, one per resource
 * at most. */
#define PINS VD_OBJS

/* virtio-gpu's cache type of the VM's ordinary memory. */
#define MAP_CACHE_CACHED 1u

/* A blob or a backing pinned through the module: its resource and the
 * pin's handle. A backing is a shmem file of the VM's memory that the
 * module makes, mapped here and attached to the resource as its one iovec,
 * which virglrenderer keeps a pointer to until the detach, so the iovec has
 * storage of its own. */
struct vr_pin {
    uint32_t id, handle;
    uint32_t map_info;  /* the cache type virglrenderer gave at the pin */
    struct iovec *back; /* a backing's mapping and length, or 0 for a blob */
};

/* The places of the account's ledger of creates, twice the resources the
 * device's table holds, a power of two for the hash. */
#define ADMIT_SLOTS (2u * VD_OBJS)

/* The DRM clients of this process that the epoch's first reading names at
 * most, vrend's, Mesa's and GBM's; the backend has three or four. */
#define OWN_CLIENTS 16

/* A create that the account admitted (`op_create`), held at its bound from
 * the admission to its UNREF or the epoch's end; id 0 is a free place,
 * since virtio-gpu has no resource 0. */
struct vr_admit {
    uint32_t id;
    uint32_t gen; /* the reading it was admitted under (`vr.gen`) */
    uint64_t bytes;
};

struct vr {
    struct vd *dev;
    struct kms *kms;
    int flush_crc; /* read pixels at each surfaceless flush for diagnosis */
    int gbm_fd;    /* the DRM device virglrenderer's GBM allocates on, or -1 */
    uint32_t capset[4];
    uint32_t ncapsets;
    uint32_t venus[VENUS_CTXS]; /* live Venus contexts' ids, 0 when free */
    int probe;                  /* import each resource a Venus context takes into Vulkan */
    int verbose;
    int module; /* the vgpushm module's node, which pins blobs, or -1 */
    struct vr_pin pins[PINS];
    uint32_t npins;
    struct vr_pin_counts pc;
    int poll_fd;     /* virglrenderer's, readable when it asks for a poll, or -1 */
    int async_fence; /* fences retire on virglrenderer's threads */
    int flags;       /* VIRGL_RENDERER_* given to each start, but the fences' */
    int native;      /* the DRM renderer runs native contexts */
    /* The account's ledger of creates (`ledger_held`): open addressing by
     * resource id, with the sum of the bounds it holds. */
    struct vr_admit admit[ADMIT_SLOTS];
    uint32_t nadmit;
    uint64_t admitted;
    /* The account's last reading of the GPU objects (`mem_room`): the bytes
     * it counted, the ledger's among them when it missed a client, its
     * generation, and the bounds of the creates admitted since, which it
     * does not hold. */
    int have_reading;
    uint64_t read_sum;
    uint32_t gen;
    uint64_t since;
    /* The DRM clients of this process that the epoch's start read, by
     * "drm-pdev drm-client-id", and whether that reading could not name
     * them all, which makes every judgment count the ledger. */
    char own[OWN_CLIENTS][80];
    uint32_t nown;
    int own_unknown;
};

static void own_clients(struct vr *r);

/* virglrenderer calls back with the cookie vr_init passed. */
static struct virgl_renderer_callbacks callbacks;

/**
 * Complete the commands of the fences retired up to id and ring the driver,
 * at once. Under VIRGL_RENDERER_ASYNC_FENCE_CB this runs on the renderer's
 * thread that saw the fence signal: vrend's sync thread, which waits on
 * each GL fence in turn, a Venus context's or a native context's; else on
 * the serving thread in virgl_renderer_poll. The device's lock alone keeps
 * its queues (`vd_fence_retired`), and no renderer lock is held across it.
 */
static void retire(struct vr *r, uint32_t ctx, int per_ctx, uint32_t ring, uint64_t id)
{
    vd_fence_retired(r->dev, ctx, per_ctx, ring, id);
    vd_kick(r->dev);
}

/** Retire the global timeline's fences up to fence. */
static void write_fence(void *cookie, uint32_t fence)
{
    struct vr *r = cookie;

    retire(r, 0, 0, 0, fence);
}

/** Retire a context ring's fences up to fence_id. */
static void write_context_fence(void *cookie, uint32_t ctx, uint32_t ring, uint64_t fence_id)
{
    struct vr *r = cookie;

    retire(r, ctx, 1, ring, fence_id);
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
 * Start virglrenderer with the flags vr_init chose, under
 * VIRGL_RENDERER_ASYNC_FENCE_CB, and read its poll descriptor. vrend's
 * poll descriptor is its sync thread's eventfd; with none, the thread did
 * not start, and an asynchronous fence of vrend's would never retire
 * (vrend_renderer_use_threaded_sync), so the renderer starts again without
 * the thread and its fences are polled. A native context has no way but
 * the asynchronous one. Then the DRM clients the renderer opened are read
 * for the account (`own_clients`). Returns 0, or virglrenderer's error.
 */
static int start(struct vr *r)
{
    int err;

    if ((err = virgl_renderer_init(r, r->flags | VIRGL_RENDERER_ASYNC_FENCE_CB, &callbacks)) != 0) {
        return err;
    }
    r->async_fence = 1;
    r->poll_fd = virgl_renderer_get_poll_fd();
    if (r->poll_fd < 0 && !r->native) {
        fprintf(stderr, "backend: fences: no sync thread; polled\n");
        virgl_renderer_cleanup(r);
        if ((err = virgl_renderer_init(r, r->flags & ~VIRGL_RENDERER_THREAD_SYNC, &callbacks)) !=
            0) {
            return err;
        }
        r->async_fence = 0;
        r->poll_fd = virgl_renderer_get_poll_fd();
    } else if (r->poll_fd < 0) {
        fprintf(stderr, "backend: fences: no sync thread; vrend's fences will not retire\n");
    }
    own_clients(r);
    return 0;
}

/**
 * Start virglrenderer with EGL: on GBM over gbm_fd when it is a DRM
 * device, so that resources it allocates there export as dma-bufs, or
 * surfaceless when gbm_fd is -1; with Venus through the render server
 * when venus is set and the DRM renderer when native is set. Blobs use
 * external storage, since page lists need an exported descriptor rather
 * than a GL mapping in this process. Native
 * context requires a build of virglrenderer with a driver for the GPU.
 * Returns the renderer, or 0 when virglrenderer fails.
 */
struct vr *vr_init(struct vd *dev, int gbm_fd, int venus, int native, int verbose)
{
    static const uint32_t ids[] = {CAPSET_VIRGL, CAPSET_VIRGL2, CAPSET_VENUS, CAPSET_DRM};
    struct vr *r = calloc(1, sizeof(*r));
    int err;
    uint32_t i, ver, size;

    if (r == 0) {
        return 0;
    }
    r->dev = dev;
    r->gbm_fd = gbm_fd;
    r->verbose = verbose;
    r->native = native;
    r->module = -1;
    r->flags =
        VIRGL_RENDERER_USE_EGL | VIRGL_RENDERER_THREAD_SYNC | VIRGL_RENDERER_USE_EXTERNAL_BLOB;
    if (gbm_fd < 0) {
        r->flags |= VIRGL_RENDERER_USE_SURFACELESS;
    }
    if (venus) {
        r->flags |= VIRGL_RENDERER_VENUS | VIRGL_RENDERER_RENDER_SERVER;
    }
    if (native) {
#ifdef VIRGL_RENDERER_DRM
        r->flags |= VIRGL_RENDERER_DRM;
#else
        fprintf(stderr, "backend: native context needs virglrenderer with DRM support\n");
        free(r);
        return 0;
#endif
    }
    virgl_set_log_callback(log_line, 0, 0);
    callbacks.version = 3;
    callbacks.write_fence = write_fence;
    callbacks.get_drm_fd = get_drm_fd;
    callbacks.write_context_fence = write_context_fence;
    if ((err = start(r)) != 0) {
        fprintf(stderr, "backend: virglrenderer failed to start: %d\n", err);
        free(r);
        return 0;
    }
    for (i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        virgl_renderer_get_cap_set(ids[i], &ver, &size);
        /* Venus reports version 0; QEMU too takes a capset by its size.
           virglrenderer sizes Venus's capset even when Venus is off. */
        if (size > 0 && (ids[i] != CAPSET_VENUS || venus) && (ids[i] != CAPSET_DRM || native)) {
            r->capset[r->ncapsets++] = ids[i];
            if (verbose) {
                fprintf(stderr, "backend: capset: id %u, version %u, size %u\n", ids[i], ver, size);
            }
        }
    }
    return r;
}

/**
 * Stop virglrenderer and free the renderer: its threads, which call back
 * into the device (`retire`), are joined, so the device may be freed after.
 */
void vr_fini(struct vr *r)
{
    virgl_renderer_cleanup(r);
    free(r);
}

/** Return virglrenderer's descriptor, readable when it asks for a poll,
 * or -1 for none; a reset may change it. */
int vr_poll_fd(struct vr *r)
{
    return r->poll_fd;
}

/** Return whether fences retire on virglrenderer's own threads, so that
 * the serving loop need not poll for them. */
int vr_async_fence(struct vr *r)
{
    return r->async_fence;
}

/** Let virglrenderer do what it asked the serving thread for: under
 * asynchronous fences, read the queries that a signalled fence's callback
 * waits on; else retire the fences that have signalled. */
void vr_poll(struct vr *r)
{
    (void)r;
    virgl_renderer_poll();
}

/** Poll virglrenderer between two commands, when its descriptor says it
 * asked: vrend's sync thread, at a fence that queries wait on, holds that
 * fence's callback until the serving thread polls (vrend_renderer_poll). */
static void op_poll(void *c)
{
    struct vr *r = c;
    struct pollfd p;

    if (r->poll_fd < 0) {
        return;
    }
    p.fd = r->poll_fd;
    p.events = POLLIN;
    p.revents = 0;
    if (poll(&p, 1, 0) > 0) {
        virgl_renderer_poll();
    }
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

/** Enable the surfaceless flush's diagnostic readback. */
void vr_set_flush_crc(struct vr *r, int enabled)
{
    r->flush_crc = enabled;
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

static int mem_room(struct vr *r, uint64_t size);
static int admit_add(struct vr *r, uint32_t id, uint64_t bytes);
static void admit_drop(struct vr *r, uint32_t id);

/* Gallium's targets of a 3D texture, whose depth halves at each level, and
 * of a cube map, which has six faces. */
#define PIPE_TEXTURE_3D   3u
#define PIPE_TEXTURE_CUBE 4u

/* virgl's formats R64_FLOAT to R64G64B64A64_FLOAT, of up to 32 bytes a
 * texel; every other format has 16 at most, a block of 4 by 4 or more
 * texels of a compressed one 16 bytes. */
#define VIRGL_FORMAT_R64_FLOAT          24u
#define VIRGL_FORMAT_R64G64B64A64_FLOAT 27u

/**
 * The bytes of a resource's storage at most, as the VM's counts judge its
 * create: a buffer's width, or a texture's texels at the most bytes a texel
 * its format can have, 32 for a format of 64-bit channels and 16 for any
 * other, summed over its mipmap chain, each level's width, height and, for
 * a 3D texture, depth half the last's and never below 1, by its layers, six
 * for a cube, and its samples, and rounded up to a 4 KiB page. A bound, not
 * the driver's size: the account counts the create at this bound only
 * until its next reading of the GPU objects, after which the VM's counts
 * and i915's hold its storage (`gpu_unbacked`), unless a reading missed a
 * client (`ledger_held`), so an error of one create lies within the eighth
 * of the VM that `mem_room` keeps.
 */
static uint64_t create_bytes(const struct vd_create *a)
{
    uint64_t w = a->width ? a->width : 1, h = a->height ? a->height : 1;
    uint64_t d = a->depth ? a->depth : 1, layers = a->array_size ? a->array_size : 1;
    uint64_t texel = 16, samples = a->nr_samples ? a->nr_samples : 1, n = 0;
    uint32_t level;

    if (a->target == PIPE_BUFFER) {
        return a->width;
    }
    if ((w | h | d | layers) > 65536u || samples > 64u) {
        return UINT64_MAX; /* past any GPU's limits, and the sum's 64 bits */
    }
    if (a->format >= VIRGL_FORMAT_R64_FLOAT && a->format <= VIRGL_FORMAT_R64G64B64A64_FLOAT) {
        texel = 32;
    }
    if (a->target == PIPE_TEXTURE_CUBE && layers < 6) {
        layers = 6;
    }
    for (level = 0; level <= a->last_level && level < 32; level++) {
        n += w * h * d;
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
        if (a->target == PIPE_TEXTURE_3D) {
            d = d > 1 ? d / 2 : 1;
        }
    }
    if (n > (UINT64_MAX / 2) / (layers * samples * texel)) {
        return UINT64_MAX;
    }
    n *= layers * samples * texel;
    return (n + 4095u) & ~(uint64_t)4095u;
}

/**
 * Create a resource without backing; device.c attaches it later. A create
 * whose storage the VM has no room for (`mem_room`) is refused with
 * ERR_OUT_OF_MEMORY, as the backing of one is, since the renderer takes
 * the storage from the VM's memory, which a GPU's driver fills and does
 * not give back while the resource lives: past the room the VM's OOM
 * killer ends the process that maps the most, this one. A create made is
 * held in the account's ledger at its bound (`admit_add`) until its UNREF,
 * and one the ledger has no place for is refused the same way.
 */
static int op_create(void *c, const struct vd_create *a)
{
    struct virgl_renderer_resource_create_args args;
    struct vr *r = c;
    uint64_t bytes = create_bytes(a);
    int err;

    if (!mem_room(r, bytes)) {
        fprintf(stderr,
                "backend: create: resource %u: refused: %llu bytes: no room in the VM's counts\n",
                (unsigned)a->id, (unsigned long long)bytes);
        return VD_RESP_ERR_OUT_OF_MEMORY;
    }
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
    if ((err = virgl_renderer_resource_create(&args, 0, 0)) != 0) {
        return resp(err);
    }
    if (admit_add(r, a->id, bytes) != 0) {
        virgl_renderer_resource_unref(a->id);
        fprintf(stderr, "backend: create: resource %u: refused: the account's ledger is full\n",
                (unsigned)a->id);
        return VD_RESP_ERR_OUT_OF_MEMORY;
    }
    return 0;
}

/** Drop a resource, its framebuffer on the display and its place in the
 * account's ledger. */
static void op_unref(void *c, uint32_t id)
{
    struct vr *r = c;

    if (r->kms) {
        kms_forget(r->kms, id);
    }
    virgl_renderer_resource_unref(id);
    admit_drop(r, id);
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

/**
 * Create a context, of a capset when context_init chose one, and make
 * vrend's context 0 current after it. vrend_create_context leaves the new
 * context's GL context current without recording it, so the next switch
 * to the context vrend last ran finds that context current and skips
 * eglMakeCurrent: its commands would then run in the new GL context, which
 * has none of the vertex arrays it made, so its draw fails with
 * GL_INVALID_OPERATION. QEMU makes context 0 current before every command.
 * Here it is done only after a create, the one call known to leave another
 * GL context current, which spares each submission a context switch, which
 * flushes. virglrenderer.h promises nothing about the current GL context
 * after any other call; a Crysis flyby through the VM showed no other.
 */
static int op_ctx_create(void *c, uint32_t ctx, uint32_t capset, uint32_t nlen, const char *name)
{
    struct vr *r = c;
    int err, i;

    if (capset == 0) {
        err = resp(virgl_renderer_context_create(ctx, nlen, name));
    } else {
        err = resp(virgl_renderer_context_create_with_flags(ctx, capset, nlen, name));
    }
    virgl_renderer_force_ctx_0();
    if (err == 0 && capset == CAPSET_VENUS && (i = venus_slot(r, 0)) >= 0) {
        r->venus[i] = ctx;
    }
    return err;
}

/**
 * Destroy a context. virglrenderer frees the fences of its rings with it
 * without retiring them, in vrend and in the render server's proxy alike;
 * the device answers their commands at CTX_DESTROY.
 */
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

/** Fence the work so far on the global timeline or a context's ring; the
 * fence's callback may complete the command before this returns. */
static int op_fence(void *c, uint32_t ctx, int per_ctx, uint32_t ring, uint64_t id)
{
    (void)c;
    return per_ctx ? virgl_renderer_context_create_fence(ctx, 0, ring, id)
                   : virgl_renderer_create_fence((int)(uint32_t)id, 0);
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
 * Update the display, or, in surfaceless mode, acknowledge the flush.
 * The optional CRC diagnosis reads back the rectangle before answering.
 */
static int op_flush(void *c, uint32_t scanout, uint32_t id, const struct vd_rect *rect)
{
    struct vr *r = c;
    struct vd_profile *profile = r->dev->profile;
    uint8_t *px;
    uint64_t bytes, start = 0, read = 0, hashed = 0;
    uint32_t crc;
    int err;

    (void)scanout;
    if (r->kms) {
        return kms_flush(r->kms, id, rect);
    }
    if (!r->flush_crc) {
        return 0;
    }
    if (rect->width == 0 || rect->height == 0 || rect->width > 16384 || rect->height > 16384) {
        return VD_RESP_ERR_PARAMETER;
    }
    bytes = (uint64_t)rect->width * rect->height * 4;
    px = malloc((size_t)bytes);
    if (px == 0) {
        return VD_RESP_ERR_OUT_OF_MEMORY;
    }
    if (profile) {
        start = vd_profile_now();
    }
    err = vr_read(r, id, rect, px, rect->width * 4);
    if (profile) {
        read = vd_profile_now();
        profile->read_ns += read - start;
    }
    if (err == 0) {
        crc = crc32_bytes(px, bytes);
        if (profile) {
            hashed = vd_profile_now();
            profile->crc_bytes += bytes;
            profile->crc_ns += hashed - read;
        }
        fprintf(stderr, "backend: flush: resource %u, %ux%u at %u,%u, crc %08x\n", (unsigned)id,
                (unsigned)rect->width, (unsigned)rect->height, (unsigned)rect->x, (unsigned)rect->y,
                (unsigned)crc);
        if (profile) {
            profile->log_ns += vd_profile_now() - hashed;
        }
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

/** Let go of a pin in the module and drop it from the table; a backing is
 * detached from its resource first and its mapping let go of after. */
static void pin_drop(struct vr *r, struct vr_pin *p)
{
    uint32_t handle = p->handle;

    if (p->back) {
        virgl_renderer_resource_detach_iov((int)p->id, 0, 0);
    }
    if (ioctl(r->module, VGPUSHM_IOC_UNPIN, &handle) != 0) {
        fprintf(stderr, "backend: unpin: resource %u: %s\n", (unsigned)p->id, strerror(errno));
    }
    if (p->back) {
        munmap(p->back->iov_base, p->back->iov_len);
        free(p->back);
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
    p->back = 0;
    r->pc.pinned++;
    if (r->verbose) {
        fprintf(stderr, "backend: pin: resource %u, fd type %u, %s, %llu pages\n", (unsigned)q->id,
                (unsigned)fd_type, pin.kind == VGPUSHM_PIN_DMABUF ? "dma-buf" : "shmem",
                (unsigned long long)pin.pages);
    }
    return p;
}

/** The runs of pin p from q->first, read from the module. Returns 0 or
 * VD_RESP_ERR_UNSPEC. */
static int pin_runs(struct vr *r, const struct vr_pin *p, struct vd_pages *q)
{
    struct vgpushm_pages pg;

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

/** A mappable blob's page list from q->first: pinned at the first ask,
 * then read from the module. Returns 0 or VD_RESP_ERR_UNSPEC. */
static int op_pages(void *c, struct vd_pages *q)
{
    struct vr *r = c;
    struct vr_pin *p = pin_find(r, q->id);

    if (p == 0 && (p = pin_new(r, q)) == 0) {
        return VD_RESP_ERR_UNSPEC;
    }
    return pin_runs(r, p, q);
}

/* A DRM client that a scan counted, by its ID. */
struct gpu_client {
    char key[80];   /* "drm-pdev drm-client-id" */
    uint64_t bytes; /* the largest figure its descriptors gave */
};

/*
 * A scan of the GPU objects the renderer holds with no pages yet
 * (`gpu_unbacked`), each DRM client counted once.
 */
struct gpu_scan {
    uint64_t sum;            /* bytes of the objects counted */
    struct gpu_client *seen; /* each client with an ID read so far */
    size_t nseen, cap;
    size_t nown;     /* the first of seen, those of this process */
    size_t anon;     /* figures read with no client ID to name them */
    int failed;      /* the errno that stopped the scan, or 0 */
    char where[512]; /* the file or directory the scan failed on */
};

/* The character devices' majors of DRM's card and render nodes and of its
 * accel nodes, the only files whose fdinfo gives Linux's drm-usage stats. */
#define DRM_MAJOR   226u
#define ACCEL_MAJOR 261u

/** Stop scan s on errno err at path, the first failure kept. */
static void scan_fail(struct gpu_scan *s, const char *path, int err)
{
    if (s->failed == 0) {
        s->failed = err ? err : EIO;
        snprintf(s->where, sizeof(s->where), "%s", path);
    }
}

/**
 * Count `bytes` for the client named `key` in s, once however many of its
 * descriptors give a figure: the largest of them stands, since each
 * descriptor's fdinfo gives the client's objects at its own reading and a
 * later one holds those made since. A client's place is made with realloc;
 * its failure fails s at path with ENOMEM.
 */
static void client_count(struct gpu_scan *s, const char *path, const char *key, uint64_t bytes)
{
    size_t i;

    for (i = 0; i < s->nseen; i++) {
        if (strcmp(s->seen[i].key, key) == 0) {
            if (bytes > s->seen[i].bytes) {
                s->sum += bytes - s->seen[i].bytes;
                s->seen[i].bytes = bytes;
            }
            return;
        }
    }
    if (s->nseen == s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 16;
        struct gpu_client *seen = realloc(s->seen, cap * sizeof(*seen));

        if (seen == 0) {
            scan_fail(s, path, ENOMEM);
            return;
        }
        s->seen = seen;
        s->cap = cap;
    }
    snprintf(s->seen[s->nseen].key, sizeof(s->seen[s->nseen].key), "%s", key);
    s->seen[s->nseen++].bytes = bytes;
    s->sum += bytes;
}

/**
 * Add to s the bytes of system memory that the DRM client of fdinfo file
 * `path` holds in GPU objects with no pages yet, as Linux's drm-usage stats
 * give them: drm-total-system0 less drm-resident-system0, or the whole
 * total for a driver that gives no resident count. i915 makes an object's
 * pages at its first use, not at its creation, so these are bytes the VM's
 * counts do not hold yet and the renderer will take. A client that
 * drm-client-id and drm-pdev name is counted once however many descriptors
 * share it, at the largest figure they give (`client_count`), and named in
 * s even with no figure; a figure with no ID is added and counted in
 * s->anon. A file of any other kind adds nothing, nor does one whose
 * descriptor or process ended
 * before it was read (ENOENT, ESRCH); any other failure to open or read it
 * fails the scan.
 */
static void unbacked_of(const char *path, struct gpu_scan *s)
{
    unsigned long long n, id = 0, total = 0, resident = 0, bytes;
    int has_total = 0, has_resident = 0, has_id = 0, bad, err;
    char line[160], unit[8], pdev[40] = "", key[80];
    FILE *f = fopen(path, "r");

    if (f == 0) {
        if (errno != ENOENT && errno != ESRCH) {
            scan_fail(s, path, errno);
        }
        return;
    }
    errno = 0;
    while (fgets(line, sizeof(line), f)) {
        unsigned long long *to;
        int *has;

        if (strncmp(line, "drm-client-id:", 14) == 0) {
            has_id = sscanf(line + 14, "%llu", &id) == 1;
            continue;
        } else if (strncmp(line, "drm-pdev:", 9) == 0) {
            if (sscanf(line + 9, "%39s", pdev) != 1) {
                pdev[0] = 0;
            }
            continue;
        } else if (strncmp(line, "drm-total-system0:", 18) == 0) {
            to = &total;
            has = &has_total;
        } else if (strncmp(line, "drm-resident-system0:", 21) == 0) {
            to = &resident;
            has = &has_resident;
        } else {
            continue;
        }
        unit[0] = 0;
        if (sscanf(strchr(line, ':') + 1, "%llu %7s", &n, unit) < 1) {
            continue;
        }
        /* drm_print_memory_stats gives bytes, KiB or MiB. */
        *to = strcmp(unit, "MiB") == 0 ? n << 20 : strcmp(unit, "KiB") == 0 ? n << 10 : n;
        *has = 1;
    }
    bad = ferror(f);
    err = errno;
    fclose(f);
    if (bad) {
        if (err != ENOENT && err != ESRCH) {
            scan_fail(s, path, err);
        }
        return;
    }
    bytes = !has_total ? 0 : has_resident ? (total > resident ? total - resident : 0) : total;
    if (!has_id) {
        if (has_total) {
            s->sum += bytes;
            s->anon++;
        }
        return;
    }
    snprintf(key, sizeof(key), "%s %llu", pdev, id);
    client_count(s, path, key, bytes);
}

/**
 * Open the cgroup.procs file of this process's cgroup v2, the backend
 * service's, naming it in path. Returns the file, or 0 when the kernel has
 * no cgroup v2 to name or, with s failed, when a file cannot be opened or
 * read for a cause other than its absence.
 */
static FILE *cgroup_procs(struct gpu_scan *s, char *path, size_t size)
{
    char line[256];
    FILE *cg = fopen("/proc/self/cgroup", "r"), *procs = 0;

    if (cg == 0) {
        if (errno != ENOENT) {
            scan_fail(s, "/proc/self/cgroup", errno);
        }
        return 0;
    }
    while (fgets(line, sizeof(line), cg)) {
        if (strncmp(line, "0::", 3) == 0) {
            line[strcspn(line, "\n")] = 0;
            snprintf(path, size, "/sys/fs/cgroup%s/cgroup.procs", line + 3);
            if ((procs = fopen(path, "r")) == 0 && errno != ENOENT) {
                scan_fail(s, path, errno);
            }
            break;
        }
    }
    if (procs == 0 && ferror(cg)) {
        scan_fail(s, "/proc/self/cgroup", errno);
    }
    fclose(cg);
    return procs;
}

/**
 * Add to s what each DRM descriptor of process pid holds (`unbacked_of`),
 * which counts each client once however many descriptors name it. A
 * descriptor is one when the file its /proc/PID/fd link names is a
 * character device of DRM's or accel's major; a stat of the link costs a
 * lookup, where a read of every descriptor's fdinfo cost an open, a
 * seq_file and a close each. Descriptors are not matched to the open files
 * already read, since another thread may close one and reuse its number
 * between the two reads. A
 * descriptor or process that ended before or while it was read adds what
 * was read; any other failure to read the directory or stat a link fails s.
 */
static void scan_fds(struct gpu_scan *s, long pid)
{
    char base[64], path[512];
    struct dirent *e;
    struct stat st;
    unsigned int major_of;
    DIR *dir;

    snprintf(base, sizeof(base), "/proc/%ld/fd", pid);
    if ((dir = opendir(base)) == 0) {
        if (errno != ENOENT && errno != ESRCH) {
            scan_fail(s, base, errno);
        }
        return;
    }
    while (s->failed == 0) {
        errno = 0;
        if ((e = readdir(dir)) == 0) {
            if (errno != 0 && errno != ENOENT && errno != ESRCH) {
                scan_fail(s, base, errno);
            }
            break;
        }
        if (e->d_name[0] == '.') {
            continue;
        }
        if (fstatat(dirfd(dir), e->d_name, &st, 0) != 0) {
            if (errno != ENOENT && errno != ESRCH) {
                snprintf(path, sizeof(path), "%s/%s", base, e->d_name);
                scan_fail(s, path, errno);
            }
            continue;
        }
        major_of = major(st.st_rdev);
        if (!S_ISCHR(st.st_mode) || (major_of != DRM_MAJOR && major_of != ACCEL_MAJOR)) {
            continue;
        }
        snprintf(path, sizeof(path), "/proc/%ld/fdinfo/%s", pid, e->d_name);
        unbacked_of(path, s);
    }
    closedir(dir);
}

/**
 * Read into s->sum the bytes of the VM's memory that the renderer's GPU
 * objects will take and the VM's counts do not hold yet (`unbacked_of`),
 * over every DRM descriptor (`scan_fds`) of this process, then of every
 * other process of the backend's cgroup, the service's (`cgroup_procs`),
 * which holds virglrenderer's render server and its workers too. The
 * clients of this process are the first s->nown of s->seen, which the
 * caller frees. 0 with no GPU: llvmpipe fills a texture's memory at its
 * create. A process that ends during the scan adds nothing. Returns 0, or
 * -1 with s->failed and s->where when a file or directory of the scan
 * cannot be opened or read for any other cause, ENOMEM or EMFILE among
 * them, since a scan that skipped a client would count its objects as 0.
 */
static int gpu_unbacked(struct gpu_scan *s)
{
    char path[512];
    long pid, self = (long)getpid();
    FILE *procs;

    memset(s, 0, sizeof(*s));
    scan_fds(s, self);
    s->nown = s->nseen;
    procs = s->failed ? 0 : cgroup_procs(s, path, sizeof(path));
    while (procs && s->failed == 0) {
        if (fscanf(procs, "%ld", &pid) != 1) {
            if (ferror(procs)) {
                scan_fail(s, path, errno);
            }
            break;
        }
        if (pid != self) {
            scan_fds(s, pid);
        }
    }
    if (procs) {
        fclose(procs);
    }
    return s->failed ? -1 : 0;
}

/**
 * Name in r->own the DRM clients of this process that the renderer just
 * started holds: vrend's and Mesa's, which virgl_renderer_init opens and
 * which stay open until virgl_renderer_cleanup, and GBM's, open for the
 * backend's life. Every resource a create makes takes its storage through
 * one of them. Clients opened later, a native context's or the probe's
 * Vulkan, are not named, since none holds a create's storage and one that
 * closes would make every later reading count the ledger. A reading that
 * fails, names more than OWN_CLIENTS or reads a figure with no client ID
 * sets r->own_unknown, so that every judgment of the epoch counts the
 * ledger (`ledger_held`).
 */
static void own_clients(struct vr *r)
{
    struct gpu_scan s;
    size_t i;

    memset(&s, 0, sizeof(s));
    scan_fds(&s, (long)getpid());
    r->own_unknown = s.failed != 0 || s.anon != 0 || s.nseen > OWN_CLIENTS;
    r->nown = 0;
    for (i = 0; i < s.nseen && i < OWN_CLIENTS; i++) {
        snprintf(r->own[r->nown++], sizeof(r->own[0]), "%s", s.seen[i].key);
    }
    free(s.seen);
    if (r->own_unknown) {
        fprintf(stderr, "backend: counts: the renderer's clients are not all named: "
                        "every create is counted at its bound\n");
    }
}

/**
 * The bytes the account's ledger adds to a reading s: none when s read
 * every client of this process that the epoch's start named (`own_clients`),
 * whose figures then hold every admitted create's object that has no pages
 * yet; else the bounds of every create admitted and not yet unreferenced,
 * which counts an object with pages twice, here and in MemAvailable, and
 * refuses early, but never admits memory twice. A reading misses a client
 * when another thread moves its descriptor across the walk.
 */
static uint64_t ledger_held(const struct vr *r, const struct gpu_scan *s)
{
    uint32_t i;
    size_t j;

    if (r->own_unknown) {
        return r->admitted;
    }
    for (i = 0; i < r->nown; i++) {
        for (j = 0; j < s->nown && strcmp(r->own[i], s->seen[j].key) != 0; j++) {
        }
        if (j == s->nown) {
            return r->admitted;
        }
    }
    return 0;
}

/** The ledger's home place of resource id, by Knuth's multiplicative hash. */
static uint32_t admit_home(uint32_t id)
{
    return (id * 2654435761u) & (ADMIT_SLOTS - 1u);
}

/** The ledger's place of resource id, or the free place it would take. */
static uint32_t admit_slot(const struct vr *r, uint32_t id)
{
    uint32_t i = admit_home(id);

    while (r->admit[i].id != 0 && r->admit[i].id != id) {
        i = (i + 1u) & (ADMIT_SLOTS - 1u);
    }
    return i;
}

/**
 * Hold resource id's create in the ledger at `bytes`, its bound, which
 * counts in `since` until the account's next reading. Returns 0,
 * or -1 when the ledger holds VD_OBJS creates already, which the device's
 * table of resources keeps from happening.
 */
static int admit_add(struct vr *r, uint32_t id, uint64_t bytes)
{
    uint32_t i = admit_slot(r, id);

    if (r->admit[i].id == id) {
        r->admitted -= r->admit[i].bytes;
        if (r->admit[i].gen == r->gen) {
            r->since -= r->admit[i].bytes;
        }
    } else if (r->nadmit == VD_OBJS) {
        return -1;
    } else {
        r->nadmit++;
    }
    r->admit[i].id = id;
    r->admit[i].gen = r->gen;
    r->admit[i].bytes = bytes;
    r->admitted += bytes;
    r->since += bytes;
    return 0;
}

/**
 * Drop resource id's create from the ledger, and from `since` when it was
 * admitted since the account's last reading, if it holds it, moving each
 * later entry of its run back into the free place when its home allows,
 * so that every entry stays reachable from its home (Knuth's algorithm R,
 * TAOCP 6.4).
 */
static void admit_drop(struct vr *r, uint32_t id)
{
    uint32_t i = admit_slot(r, id), j, k;

    if (r->admit[i].id != id) {
        return;
    }
    r->admitted -= r->admit[i].bytes;
    if (r->admit[i].gen == r->gen) {
        r->since -= r->admit[i].bytes;
    }
    r->nadmit--;
    for (;;) {
        r->admit[i].id = 0;
        j = i;
        do {
            j = (j + 1u) & (ADMIT_SLOTS - 1u);
            if (r->admit[j].id == 0) {
                return;
            }
            k = admit_home(r->admit[j].id);
        } while (i <= j ? (i < k && k <= j) : (i < k || k <= j));
        r->admit[i] = r->admit[j];
        i = j;
    }
}

/**
 * Read the GPU objects afresh for the account (`gpu_unbacked`): their
 * bytes, and the ledger's when the reading missed a client of the
 * renderer's (`ledger_held`), become r->read_sum, and the creates admitted
 * from now on count in r->since until the next reading. Returns 0, or -1
 * with `backend: counts: PATH: ERROR` when the objects cannot be read,
 * which leaves no reading.
 */
static int mem_read(struct vr *r)
{
    struct gpu_scan scan;

    r->have_reading = 0;
    if (gpu_unbacked(&scan) != 0) {
        fprintf(stderr, "backend: counts: %s: %s\n", scan.where, strerror(scan.failed));
        free(scan.seen);
        return -1;
    }
    r->read_sum = scan.sum + ledger_held(r, &scan);
    free(scan.seen);
    r->have_reading = 1;
    r->gen++;
    r->since = 0;
    return 0;
}

/**
 * Whether the VM has room for `size` bytes more, of a backing or of a
 * resource's storage: its available memory, as /proc/meminfo counts it,
 * holds them, the bytes the renderer's GPU objects will yet take, and an
 * eighth of the VM's memory besides, so that a guest that asks for more
 * than the VM has is refused, as a Linux guest's allocation fails when its
 * RAM runs out, before the pin's reclaim, or i915's at an object's first
 * use, stalls the VM and every answer with it or wakes its OOM killer.
 * Creates (`op_create`) and backings (`back_new`) are judged so against
 * one account: a create's GPU object counts here from its creation until
 * it has its pages, and in MemAvailable after; a backing is pinned whole
 * at its admission, so MemAvailable holds it at every later judgment.
 * The objects' bytes are the account's last reading (`mem_read`) and the
 * bounds of the creates admitted since (`since`), which the reading does
 * not hold; MemAvailable is read at each judgment, after the objects, so
 * that one made resident since the reading is counted twice, which refuses
 * early, and never not at all. A judgment that would leave less room than
 * another eighth of the VM's memory, the margin, or would refuse, reads
 * the objects afresh and judges again, since the renderer makes GPU
 * objects by other paths too, at submissions, blob creates and in Venus's
 * workers, which only a new reading counts: the margin holds those made
 * since the last reading. The first judgment of an epoch reads them too.
 * main has the C library give back at once what llvmpipe frees. 0 when
 * the objects cannot be read (`mem_read`); 1 when the VM has no
 * /proc/meminfo or it lacks either count; 0 when it cannot be opened for
 * want of memory or its read fails, as seq_file's does when it cannot
 * allocate its buffer. MemAvailable reads 0, not less, when the free pages
 * are down to the watermarks, which is no room.
 */
static int mem_room(struct vr *r, uint64_t size)
{
    unsigned long long total, avail, kib;
    int has_total, has_avail, failed, fresh = 0, room;
    uint64_t need;
    char line[128];
    FILE *f;

    if (!r->have_reading) {
        if (mem_read(r) != 0) {
            return 0;
        }
        fresh = 1;
    }
    for (;;) {
        total = avail = 0;
        has_total = has_avail = 0;
        if ((f = fopen("/proc/meminfo", "r")) == 0) {
            return errno != ENOMEM && errno != ENFILE && errno != EMFILE;
        }
        while (fgets(line, sizeof(line), f)) {
            if (sscanf(line, "MemTotal: %llu kB", &kib) == 1) {
                total = kib * 1024u;
                has_total = 1;
            } else if (sscanf(line, "MemAvailable: %llu kB", &kib) == 1) {
                avail = kib * 1024u;
                has_avail = 1;
            }
        }
        failed = ferror(f);
        fclose(f);
        if (failed) {
            return 0;
        }
        if (!has_total || !has_avail) {
            return 1;
        }
        need = total / 8u + r->read_sum + r->since;
        room = need <= avail && size <= avail - need;
        if (fresh || (room && avail - need - size >= total / 8u)) {
            return room;
        }
        if (mem_read(r) != 0) {
            return 0;
        }
        fresh = 1;
    }
}

volatile sig_atomic_t vr_hold;
volatile sig_atomic_t vr_refuse;

/**
 * Hold the serving thread VR_HOLD_S seconds by the monotonic clock, through
 * signals, for resource id's backing, as SIGUSR2 asked (vr_hold).
 */
static void hold_backing(uint32_t id)
{
    struct timespec end, now;

    vr_hold = 0;
    fprintf(stderr, "backend: back: resource %u: holding %d s, as asked\n", (unsigned)id,
            VR_HOLD_S);
    clock_gettime(CLOCK_MONOTONIC, &end);
    end.tv_sec += VR_HOLD_S;
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &end, 0) == EINTR) {
    }
    clock_gettime(CLOCK_MONOTONIC, &now);
    fprintf(stderr, "backend: back: resource %u: held %ld s\n", (unsigned)id,
            (long)(now.tv_sec - end.tv_sec + VR_HOLD_S));
}

/**
 * Make resource q->id's backing of q->size bytes of the VM's memory: a
 * shmem file that the module makes and pins whole for the driver's epoch
 * (VGPUSHM_IOC_BACKING), its pages zeroed, then mapped shared and attached
 * to the resource as its one iovec, as QEMU attaches a guest's pages. The
 * module frees the file and what its pin brought in when the pin fails.
 * Returns 0 with the pin in *out, VD_RESP_ERR_OUT_OF_MEMORY when the VM has
 * no memory for it, by its counts (`mem_room`) or by the pin's allocation,
 * or as SIGWINCH asked (`vr_refuse`), or VD_RESP_ERR_UNSPEC. The refusal's
 * line names the step that refused.
 */
static int back_new(struct vr *r, const struct vd_pages *q, struct vr_pin **out)
{
    struct vgpushm_backing bk;
    struct iovec *iov = calloc(1, sizeof(*iov));
    int err = iov == 0 ? ENOMEM : 0, pinned = 0;
    const char *step = "the iovec";
    void *map = MAP_FAILED;

    if (vr_hold) {
        hold_backing(q->id);
    }
    if (err == 0 && vr_refuse) {
        vr_refuse = 0;
        err = ENOMEM;
        step = "as asked";
    }
    if (err == 0 && (r->module < 0 || r->npins == PINS)) {
        err = ENOSPC;
        step = "no pin";
    }
    if (err == 0 && !mem_room(r, q->size)) {
        err = ENOMEM;
        step = "no room in the VM's counts";
    }
    memset(&bk, 0, sizeof(bk));
    bk.size = q->size;
    bk.epoch = q->epoch;
    bk.fd = -1;
    if (err == 0) {
        if (ioctl(r->module, VGPUSHM_IOC_BACKING, &bk) != 0) {
            err = errno;
            step = "the pin";
        } else {
            pinned = 1;
        }
    }
    if (err == 0 &&
        (map = mmap(0, q->size, PROT_READ | PROT_WRITE, MAP_SHARED, bk.fd, 0)) == MAP_FAILED) {
        err = errno;
        step = "the mapping";
    }
    if (pinned) {
        close(bk.fd); /* the mapping and the pin hold the file */
    }
    if (err == 0) {
        iov->iov_base = map;
        iov->iov_len = q->size;
        /* virglrenderer answers EINVAL, of either sign by version. */
        err = virgl_renderer_resource_attach_iov((int)q->id, iov, 1);
        err = err < 0 ? -err : err;
        step = "the attach";
    }
    if (err != 0) {
        if (pinned) {
            ioctl(r->module, VGPUSHM_IOC_UNPIN, &bk.handle);
        }
        if (map != MAP_FAILED) {
            munmap(map, q->size);
        }
        fprintf(stderr, "backend: back: resource %u: refused: %llu bytes: %s: %s\n",
                (unsigned)q->id, (unsigned long long)q->size, step, strerror(err));
        free(iov);
        r->pc.refused++;
        return err == ENOMEM ? VD_RESP_ERR_OUT_OF_MEMORY : VD_RESP_ERR_UNSPEC;
    }
    *out = &r->pins[r->npins++];
    (*out)->id = q->id;
    (*out)->handle = bk.handle;
    (*out)->map_info = MAP_CACHE_CACHED;
    (*out)->back = iov;
    r->pc.pinned++;
    r->pc.backed++;
    return 0;
}

/** A resource's backing's page list from q->first: made at the first ask,
 * then read from the module. Returns 0 or a VD_RESP_ERR_*. */
static int op_back(void *c, struct vd_pages *q)
{
    struct vr *r = c;
    struct vr_pin *p = pin_find(r, q->id);
    int err;

    if (p == 0 && (err = back_new(r, q, &p)) != 0) {
        return err;
    }
    if (!p->back) {
        return VD_RESP_ERR_UNSPEC;
    }
    return pin_runs(r, p, q);
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

/**
 * End every context, resource, pin and the account's ledger for a new
 * epoch: virglrenderer stops and starts again (`start`), since
 * virgl_renderer_reset in 1.1.0 joins vrend's sync thread and starts none,
 * after which no asynchronous fence of vrend's retires
 * (vrend_renderer_prepare_reset and vrend_renderer_reset). A renderer that
 * fails to start ends the backend.
 */
static void op_reset(void *c)
{
    struct vr *r = c;
    int err;

    while (r->npins) {
        pin_drop(r, &r->pins[0]);
    }
    memset(r->admit, 0, sizeof(r->admit));
    r->nadmit = 0;
    r->admitted = 0;
    r->have_reading = 0;
    r->since = 0;
    if (r->kms) {
        kms_reset(r->kms);
    }
    memset(r->venus, 0, sizeof(r->venus));
    virgl_renderer_cleanup(r);
    if ((err = start(r)) != 0) {
        fprintf(stderr, "backend: virglrenderer failed to start again: %d\n", err);
        exit(1);
    }
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
    .back = op_back,
    .poll = op_poll,
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
