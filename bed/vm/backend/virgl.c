/*
 * virgl.c: the backend's renderer, libvirglrenderer behind the operations
 * device.c calls, as QEMU's virtio-gpu-virgl.c drives it, and the display
 * operations passed on to kms.c.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/stat.h>
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

/* The places of the account's ledger, twice the resources the device's
 * table holds, a power of two for the hash. */
#define ADMIT_SLOTS (2u * VD_OBJS)

/* The most bytes the account judges for one create, blob or backing, 256
 * TiB: past any VM's memory, and small enough that the ledger's VD_OBJS
 * entries sum in 64 bits without wrapping. */
#define VR_BYTES_MAX ((uint64_t)1 << 48)

/* A resource whose storage the backend cannot secure (`op_create`,
 * `op_blob`), charged at its bound from its create to its UNREF or the
 * epoch's end; id 0 is a free place, since virtio-gpu has no resource 0. */
struct vr_admit {
    uint32_t id;
    uint64_t bytes;
};

/* A resource's storage secured at its create (`hold_new`): the module's pin
 * of the dma-buf or shmem file the renderer exported, which holds its pages
 * until the UNREF or the epoch's end, that file's identity and the bytes
 * pinned from its start, so that storage already held for another resource
 * is charged only past the largest live hold of the same file. */
struct vr_hold {
    uint32_t id, handle;
    dev_t dev;
    ino_t ino;
    uint64_t size;
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
    /* The account (`mem_room`): the ledger, by open addressing on the
     * resource id, with the sum of the bounds it holds, and the holds. */
    struct vr_admit admit[ADMIT_SLOTS];
    uint32_t nadmit;
    uint64_t admitted;
    struct vr_hold holds[PINS];
    uint32_t nholds;
};

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
 * the asynchronous one. Returns 0, or virglrenderer's error.
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
static int hold_new(struct vr *r, uint32_t id, int fd, const char *what);
static void hold_drop(struct vr *r, uint32_t id);

/* Gallium's targets of a 3D texture, whose depth halves at each level, and
 * of a cube map, which has six faces. */
#define PIPE_TEXTURE_3D   3u
#define PIPE_TEXTURE_CUBE 4u

/**
 * The bytes of one block of virgl format `format`, a texel or a compressed
 * block, with the block's width and height in texels in *bw and *bh. A
 * format the GPU may store wider than its own size is given at the wider:
 * three 8-bit channels at four, and ETC and ATC, which Haswell cannot
 * sample and Mesa keeps decompressed, at 4 bytes a texel. A format not
 * listed is given 16 bytes a texel, more than any but those of 64-bit
 * channels, which are listed. The numbers are enum virgl_formats' in
 * virglrenderer's virgl_hw.h.
 */
static uint32_t format_block(uint32_t format, uint32_t *bw, uint32_t *bh)
{
    /* 1 byte: L8, A8, I8, S8, R8 of each kind, L8_SRGB, R1, L4A4, B2G3R3,
     * the 8-bit SNORM alphas, lumas and intensities, A4R4, R4A4, the 8-bit
     * integer reds, alphas, lumas and intensities, R8_SRGB, R3G3B2, the Y8
     * formats and the integer B2G3R3 and R3G3B2. */
    static const uint16_t one[] = {9,   10,  11,  23,  64,  69,  74,  82,  95,  127, 130,
                                   139, 147, 148, 150, 168, 169, 177, 181, 201, 202, 203,
                                   205, 206, 207, 312, 318, 322, 326, 375, 405};
    /* 2 bytes: the 16-bit packed colours, L8A8, L16, Z16, R16 and R8G8 of
     * each kind, the 16-bit alphas, lumas and intensities, R8A8, A8R8,
     * A8L8, G8R8 and their integer and SRGB forms. */
    static const uint16_t two[] = {5,   6,   7,   12,  13,  16,  48,  52,  56,  60,  65,  70,  75,
                                   83,  91,  96,  122, 135, 141, 142, 149, 151, 152, 154, 155, 156,
                                   158, 170, 171, 178, 182, 185, 189, 204, 208, 209, 210, 211, 213,
                                   214, 215, 242, 247, 248, 254, 259, 260, 261, 263, 264, 311, 313,
                                   319, 320, 321, 330, 331, 332, 333, 338, 339, 340, 376, 377, 378,
                                   401, 406, 407, 408, 409, 410, 411, 427, 428};
    /* 8 bytes: two 32-bit or three or four 16-bit channels, one 64-bit
     * channel, Z32_FLOAT_S8X24, X32_S8X24, L32A32 and the 16-bit YUV
     * formats. */
    static const uint16_t eight[] = {24,  29,  33,  37,  41,  45,  50,  51,  54,  55,  58,
                                     59,  62,  63,  88,  93,  94,  126, 138, 161, 187, 188,
                                     191, 192, 194, 198, 220, 224, 234, 235, 236, 237, 238,
                                     246, 251, 252, 418, 419, 433, 434, 435, 436, 441, 442};
    /* 16 bytes: three or four 32-bit channels, or two 64-bit ones. */
    static const uint16_t sixteen[] = {25, 30, 31,  34,  35,  38,  39,  42,  43,  46,  47,
                                       89, 90, 195, 196, 199, 200, 239, 240, 241, 416, 417};
    /* 32 bytes: three or four 64-bit channels. */
    static const uint16_t big[] = {26, 27, 412, 413, 414, 415};
    /* Blocks of 4 by 4 texels: 8 bytes for DXT1, RGTC1 and LATC1, 16 for
     * DXT3, DXT5, RGTC2, LATC2 and BPTC. FXT1's are 8 by 4, of 16 bytes. */
    static const uint16_t dxt8[] = {105, 106, 109, 110, 113, 114, 143, 144};
    static const uint16_t dxt16[] = {107, 108, 111, 112, 115, 116, 145, 146, 255, 256, 257, 258};
    static const struct {
        const uint16_t *list;
        size_t n;
        uint32_t bytes, bw, bh;
    } sizes[] = {
        {one, sizeof(one) / sizeof(one[0]), 1, 1, 1},
        {two, sizeof(two) / sizeof(two[0]), 2, 1, 1},
        {eight, sizeof(eight) / sizeof(eight[0]), 8, 1, 1},
        {sixteen, sizeof(sixteen) / sizeof(sixteen[0]), 16, 1, 1},
        {big, sizeof(big) / sizeof(big[0]), 32, 1, 1},
        {dxt8, sizeof(dxt8) / sizeof(dxt8[0]), 8, 4, 4},
        {dxt16, sizeof(dxt16) / sizeof(dxt16[0]), 16, 4, 4},
    };
    size_t i, k;

    for (k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
        for (i = 0; i < sizes[k].n; i++) {
            if (format == sizes[k].list[i]) {
                *bw = sizes[k].bw;
                *bh = sizes[k].bh;
                return sizes[k].bytes;
            }
        }
    }
    *bw = *bh = 1;
    if (format == 395 || format == 396) {
        *bw = 8;
        *bh = 4;
        return 16;
    }
    /* 4 bytes: every other format of the enum, of 32 bits or less or of
     * three 8-bit channels, ETC and ATC decompressed, and the 8- to 12-bit
     * YUV formats; the enum has no format 0, 73, 78 to 81, 86, 133, 279 to
     * 307, 309, 310 or 348 to 367. */
    if (format == 0 || format == 73 || (format >= 78 && format <= 81) || format == 86 ||
        format == 133 || (format >= 279 && format <= 307) || format == 309 || format == 310 ||
        (format >= 348 && format <= 367) || format > 442) {
        return 16;
    }
    return 4;
}

/* What a level's row bytes and rows are rounded up to, so that the bound
 * covers each of i915's tilings: the pitch of an X tile, 512 bytes by 8
 * rows, and the height of a Y tile, 128 bytes by 32 rows; a W tile is 64 by
 * 64 and a linear surface's pitch 64 bytes. */
#define TILE_ROW_BYTES 512u
#define TILE_ROWS      32u
/* The rows gen7 adds to an array layer's pitch past h0 and h1: 12 * valign
 * for a valign of 4, and 3 rows each for rounding h0 and h1 up to it. */
#define LAYER_ROWS     54u

/**
 * The bytes the account judges a create at before the renderer makes it,
 * and charges one whose storage it cannot hold (`charge`): twice a buffer's
 * width; for a texture, twice its samples by the larger of two sums. The
 * first is its layers, six for a cube, by the sum over its mipmap chain of
 * each level's blocks (`format_block`), its row bytes and its rows rounded
 * up to any tiling's (TILE_ROW_BYTES, TILE_ROWS), by its depth, which
 * halves at each level of a 3D texture and never falls below 1. The second,
 * for more than one layer, is level 0's rounded row bytes by LAYER_ROWS
 * past the block rows of levels 0 and 1 for each layer, and a tile's rows
 * more: gen7's hardware spaces an array's layers h0 + h1 + 12 * valign
 * rows apart, h1 counted even with one level, valign at most 4 rows, and
 * each height rounded up to valign. The total is rounded up to a 4 KiB
 * page. i915's isl places a miptree's levels in one surface, level 1 under
 * level 0 and the rest beside level 1, so that the surface is no wider than
 * the levels together and no taller than levels 0 and 1, and adds the
 * separate stencil, HiZ, MCS and CCS surfaces, each well below the main
 * one; Mesa's cache gives a buffer an object at most a quarter larger than
 * its width. The factor of two covers both. UINT64_MAX for a size past any
 * GPU's limits or VR_BYTES_MAX.
 */
static uint64_t create_bytes(const struct vd_create *a)
{
    uint64_t w = a->width ? a->width : 1, h = a->height ? a->height : 1;
    uint64_t d = a->depth ? a->depth : 1, layers = a->array_size ? a->array_size : 1;
    uint64_t samples = a->nr_samples ? a->nr_samples : 1, n = 0, row, rows;
    uint64_t row0 = 0, pitch, spaced;
    uint32_t level, bw, bh, bytes;

    if (a->target == PIPE_BUFFER) {
        return (2u * (uint64_t)a->width + 4095u) & ~(uint64_t)4095u;
    }
    if ((w | h | d | layers) > 65536u || samples > 64u) {
        return UINT64_MAX; /* past any GPU's limits, and the sum's 64 bits */
    }
    bytes = format_block(a->format, &bw, &bh);
    if (a->target == PIPE_TEXTURE_CUBE && layers < 6) {
        layers = 6;
    }
    pitch = (h + bh - 1) / bh + ((h > 1 ? h / 2 : 1) + bh - 1) / bh + LAYER_ROWS;
    for (level = 0; level <= a->last_level && level < 32; level++) {
        row = ((w + bw - 1) / bw * bytes + TILE_ROW_BYTES - 1) & ~(uint64_t)(TILE_ROW_BYTES - 1);
        rows = ((h + bh - 1) / bh + TILE_ROWS - 1) & ~(uint64_t)(TILE_ROWS - 1);
        if (level == 0) {
            row0 = row;
        }
        n += row * rows * d;
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
        if (a->target == PIPE_TEXTURE_3D) {
            d = d > 1 ? d / 2 : 1;
        }
    }
    if (n > VR_BYTES_MAX / (2u * layers * samples)) {
        return UINT64_MAX;
    }
    n *= layers;
    if (layers > 1) {
        spaced = row0 * (pitch * layers + TILE_ROWS); /* row0 <= 2^20, the rest < 2^33 */
        if (spaced > VR_BYTES_MAX / (2u * samples)) {
            return UINT64_MAX;
        }
        n = spaced > n ? spaced : n;
    }
    n *= 2u * samples;
    return (n + 4095u) & ~(uint64_t)4095u;
}

/**
 * Judge `bytes` of a resource not yet made against the room (`mem_room`),
 * before the renderer allocates any of it: a size past VR_BYTES_MAX, or
 * one the room does not hold, is refused. Returns 0, or ERR_OUT_OF_MEMORY
 * with a line naming the resource and `what`, the command.
 */
static int judge(struct vr *r, uint32_t id, uint64_t bytes, const char *what)
{
    if (bytes > VR_BYTES_MAX) {
        fprintf(stderr, "backend: %s: resource %u: refused: %llu bytes: past the account's bound\n",
                what, (unsigned)id, (unsigned long long)bytes);
        return VD_RESP_ERR_OUT_OF_MEMORY;
    }
    if (!mem_room(r, bytes)) {
        fprintf(stderr,
                "backend: %s: resource %u: refused: %llu bytes: no room in the VM's counts\n", what,
                (unsigned)id, (unsigned long long)bytes);
        return VD_RESP_ERR_OUT_OF_MEMORY;
    }
    return 0;
}

/**
 * Charge resource `id`, which the renderer has made after `judge` admitted
 * `bytes`, to the account: hold the whole of the storage exported as `fd`
 * (`hold_new`); else, for no descriptor (-1) or one that cannot be held,
 * charge the ledger (`admit_add`) the larger of `bytes` and the exported
 * file's size, rounded up to a page, judging the whole of a size past
 * `bytes` again, since `judge` reserved none of it in the ledger. fd is
 * closed. Returns 0, or ERR_OUT_OF_MEMORY with a line naming the
 * refusal, `what` the command; the caller unmakes a refused resource.
 */
static int charge(struct vr *r, uint32_t id, int fd, uint64_t bytes, const char *what)
{
    int err = fd >= 0 ? hold_new(r, id, fd, what) : -1;
    uint64_t judged = bytes;
    off_t end;

    if (fd >= 0 && err < 0 && (end = lseek(fd, 0, SEEK_END)) > 0 && (uint64_t)end > bytes) {
        bytes = ((uint64_t)end + 4095u) & ~(uint64_t)4095u;
    }
    if (fd >= 0) {
        close(fd);
    }
    if (err >= 0) {
        return err;
    }
    if (bytes > judged && (bytes > VR_BYTES_MAX || !mem_room(r, bytes))) {
        fprintf(stderr,
                "backend: %s: resource %u: refused: %llu bytes: no room in the VM's counts\n", what,
                (unsigned)id, (unsigned long long)bytes);
        return VD_RESP_ERR_OUT_OF_MEMORY;
    }
    if (admit_add(r, id, bytes) != 0) {
        fprintf(stderr, "backend: %s: resource %u: refused: the account's ledger is full\n", what,
                (unsigned)id);
        return VD_RESP_ERR_OUT_OF_MEMORY;
    }
    return 0;
}

/**
 * Create a resource without backing; device.c attaches it later. Its bound
 * (`create_bytes`) is judged before the renderer allocates (`judge`), since
 * llvmpipe allocates and clears a texture's storage in the create, and its
 * storage is held or charged after it, before the answer (`charge`): a
 * resource the renderer allocated through GBM exports as a dma-buf and is
 * held whole, and any other is charged at its bound. A create the VM has no
 * room for is refused with ERR_OUT_OF_MEMORY, unmade if made, as the
 * backing of one is, since the renderer takes the storage from the VM's
 * memory, which a GPU's driver fills at the storage's first use and does
 * not give back while the resource lives: past the room the VM's OOM
 * killer ends the process that maps the most, this one.
 */
static int op_create(void *c, const struct vd_create *a)
{
    struct virgl_renderer_resource_create_args args;
    uint64_t bytes = create_bytes(a);
    struct vr_export e;
    struct vr *r = c;
    int err;

    if ((err = judge(r, a->id, bytes, "create")) != 0) {
        return err;
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
    if (vr_export(r, a->id, &e) != 0) {
        e.fd = -1;
    }
    if ((err = charge(r, a->id, e.fd, bytes, "create")) != 0) {
        virgl_renderer_resource_unref(a->id);
    }
    return err;
}

/** Drop a resource, its framebuffer on the display, its hold and its place
 * in the account's ledger. */
static void op_unref(void *c, uint32_t id)
{
    struct vr *r = c;

    if (r->kms) {
        kms_forget(r->kms, id);
    }
    hold_drop(r, id);
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

/**
 * Create a blob resource: guest memory in the region, or a context's. A
 * blob of a context's memory is judged at its size, rounded up to a page,
 * before the renderer allocates it (`judge`), and held or charged after,
 * before the answer (`charge`): the storage the renderer exports for it, a
 * dma-buf or a shared-memory file, is held whole, at the file's own size,
 * and a blob whose storage cannot be exported or held, as a GPU's own
 * memory or a Vulkan allocation's opaque descriptor cannot, is charged the
 * larger of its size and the exported file's. One the VM has no room for
 * is refused with ERR_OUT_OF_MEMORY, unmade if made.
 */
static int op_blob(void *c, const struct vd_blob *a, struct iovec *iov, uint32_t n)
{
    struct virgl_renderer_resource_create_blob_args args;
    uint64_t bytes = a->size > VR_BYTES_MAX ? a->size : (a->size + 4095u) & ~(uint64_t)4095u;
    struct vr *r = c;
    uint32_t fd_type;
    int err, fd = -1;

    if (a->mem != VIRGL_RENDERER_BLOB_MEM_GUEST && (err = judge(r, a->id, bytes, "blob")) != 0) {
        return err;
    }
    memset(&args, 0, sizeof(args));
    args.res_handle = a->id;
    args.ctx_id = a->ctx;
    args.blob_mem = a->mem;
    args.blob_flags = a->flags;
    args.blob_id = a->blob_id;
    args.size = a->size;
    args.iovecs = iov;
    args.num_iovs = n;
    if ((err = virgl_renderer_resource_create_blob(&args)) != 0) {
        return resp(err);
    }
    if (a->mem == VIRGL_RENDERER_BLOB_MEM_GUEST) {
        return 0;
    }
    if (virgl_renderer_resource_export_blob(a->id, &fd_type, &fd) != 0) {
        fd = -1;
    }
    if ((err = charge(r, a->id, fd, bytes, "blob")) != 0) {
        virgl_renderer_resource_unref(a->id);
    }
    return err;
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
 * Charge resource id to the ledger at `bytes`, its bound, at most
 * VR_BYTES_MAX as `mem_room` judged it, until its UNREF or the epoch's end.
 * Returns 0, or -1 when the ledger holds VD_OBJS resources already, which
 * the device's table of resources keeps from happening.
 */
static int admit_add(struct vr *r, uint32_t id, uint64_t bytes)
{
    uint32_t i = admit_slot(r, id);

    if (r->admit[i].id == id) {
        r->admitted -= r->admit[i].bytes;
    } else if (r->nadmit == VD_OBJS) {
        return -1;
    } else {
        r->nadmit++;
    }
    r->admit[i].id = id;
    r->admit[i].bytes = bytes;
    r->admitted += bytes;
    r->pc.charged++;
    if (r->admitted > r->pc.ledger_most) {
        r->pc.ledger_most = r->admitted;
    }
    return 0;
}

/**
 * Drop resource id from the ledger, if it holds it, moving each later
 * entry of its run back into the free place when its home allows, so that
 * every entry stays reachable from its home (Knuth's algorithm R, TAOCP
 * 6.4).
 */
static void admit_drop(struct vr *r, uint32_t id)
{
    uint32_t i = admit_slot(r, id), j, k;

    if (r->admit[i].id != id) {
        return;
    }
    r->admitted -= r->admit[i].bytes;
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
 * Hold the storage of resource id that the renderer exported as fd, a
 * dma-buf or a shmem file: the whole file, its size from lseek rounded up
 * to a page, pinned through the module (VGPUSHM_IOC_PIN), which brings in
 * every page it has not and keeps them until `hold_drop`, as i915 keeps a
 * dma-buf's pages while a device maps it. Only the bytes past the largest
 * live hold of the same file, which pins its pages already, are judged
 * against the room (`mem_room`); the file's pages are held while any hold
 * of it lives, each over its own length. Returns 0; ERR_OUT_OF_MEMORY,
 * with a line, when the room or the pin's allocation refuses it; or -1
 * when it cannot be held at all, with no module, no place left, a size of
 * 0 or past the module's largest pin, or a file the module cannot pin,
 * which the caller charges to the ledger instead (`charge`). `what` names
 * the command in the line.
 */
static int hold_new(struct vr *r, uint32_t id, int fd, const char *what)
{
    struct vgpushm_pin pin;
    uint64_t size, covered = 0;
    struct stat st;
    uint32_t i;
    off_t end;
    int err;

    if (r->module < 0 || r->nholds == PINS || fstat(fd, &st) != 0 ||
        (end = lseek(fd, 0, SEEK_END)) <= 0 || (uint64_t)end > VGPUSHM_PIN_MAX_SIZE) {
        return -1;
    }
    size = ((uint64_t)end + 4095u) & ~(uint64_t)4095u;
    for (i = 0; i < r->nholds; i++) {
        if (r->holds[i].dev == st.st_dev && r->holds[i].ino == st.st_ino &&
            r->holds[i].size > covered) {
            covered = r->holds[i].size;
        }
    }
    if (size > covered && !mem_room(r, size - covered)) {
        fprintf(stderr,
                "backend: %s: resource %u: refused: %llu bytes: no room in the VM's counts\n", what,
                (unsigned)id, (unsigned long long)(size - covered));
        return VD_RESP_ERR_OUT_OF_MEMORY;
    }
    memset(&pin, 0, sizeof(pin));
    pin.fd = fd;
    pin.epoch = r->dev->drv.epoch;
    pin.size = size;
    if (ioctl(r->module, VGPUSHM_IOC_PIN, &pin) != 0) {
        err = errno;
        if (err != ENOMEM) {
            return -1;
        }
        fprintf(stderr, "backend: %s: resource %u: refused: %llu bytes: the pin: %s\n", what,
                (unsigned)id, (unsigned long long)size, strerror(err));
        return VD_RESP_ERR_OUT_OF_MEMORY;
    }
    r->holds[r->nholds].id = id;
    r->holds[r->nholds].handle = pin.handle;
    r->holds[r->nholds].dev = st.st_dev;
    r->holds[r->nholds].ino = st.st_ino;
    r->holds[r->nholds].size = size;
    r->nholds++;
    r->pc.held++;
    return 0;
}

/** Let go of resource id's hold, if it has one. */
static void hold_drop(struct vr *r, uint32_t id)
{
    uint32_t i, handle;

    for (i = 0; i < r->nholds; i++) {
        if (r->holds[i].id == id) {
            handle = r->holds[i].handle;
            if (ioctl(r->module, VGPUSHM_IOC_UNPIN, &handle) != 0) {
                fprintf(stderr, "backend: unpin: resource %u: hold: %s\n", (unsigned)id,
                        strerror(errno));
            }
            r->holds[i] = r->holds[--r->nholds];
            return;
        }
    }
}

/**
 * Whether the VM has room for `size` bytes more, of a backing, a hold or a
 * resource charged to the ledger: MemAvailable, read afresh from
 * /proc/meminfo, holds them, an eighth of MemTotal, and the ledger's
 * bounds, which the VM's memory does not hold yet, since a GPU's driver
 * takes a resource's pages at their first use. A backing and a hold are
 * pinned whole at their admission, so MemAvailable holds them at every
 * later judgment; a ledger's resource made resident is counted twice,
 * which refuses early, and never not at all. This refuses what a guest
 * asks past the VM's memory, as a Linux guest's allocation fails when its
 * RAM runs out, before the pin's reclaim, or i915's at a first use, stalls
 * the VM and every answer with it or wakes its OOM killer. MemAvailable
 * reads 0, not less, when the free pages are down to the watermarks, which
 * is no room. 1 for a size of 0; 0 for one past VR_BYTES_MAX, and when
 * /proc/meminfo cannot be read or lacks either count.
 */
static int mem_room(struct vr *r, uint64_t size)
{
    unsigned long long kib, total = 0, avail = 0;
    int has_total = 0, has_avail = 0, failed;
    uint64_t need, room;
    char line[128];
    FILE *f;

    if (size == 0) {
        return 1;
    }
    if (size > VR_BYTES_MAX || (f = fopen("/proc/meminfo", "r")) == 0) {
        return 0;
    }
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "MemTotal: %llu kB", &kib) == 1) {
            total = kib;
            has_total = 1;
        } else if (sscanf(line, "MemAvailable: %llu kB", &kib) == 1) {
            avail = kib;
            has_avail = 1;
        }
    }
    failed = ferror(f);
    fclose(f);
    if (failed || !has_total || !has_avail || total > VR_BYTES_MAX / 1024u ||
        avail > VR_BYTES_MAX / 1024u) {
        return 0;
    }
    need = total * 1024u / 8u + r->admitted;
    room = need <= avail * 1024u ? avail * 1024u - need : 0;
    if (r->pc.judged++ == 0 || room < r->pc.least_room) {
        r->pc.least_room = room;
    }
    return size <= room;
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
 * End every context, resource, pin, hold and the account's ledger for a new
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
    while (r->nholds) {
        hold_drop(r, r->holds[0].id);
    }
    memset(r->admit, 0, sizeof(r->admit));
    r->nadmit = 0;
    r->admitted = 0;
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
