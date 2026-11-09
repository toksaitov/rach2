/*
 * peer.c: the driver's side of the shared-memory GPU transport on Linux, with
 * a loopback device in the same file, a check of fuzzer seeds and a writer
 * of the built-in virgl stream. bed/shm/Readme.md describes its use.
 */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define VGS_IMPLEMENTATION
#include "vgpushm.h"

/* virtio-gpu's wire format, from the virtio 1.2 specification, 5.7.6. */
#define GPU_F_VIRGL 0

#define GPU_CMD_GET_DISPLAY_INFO 0x0100
#define GPU_CMD_RESOURCE_CREATE_2D 0x0101
#define GPU_CMD_RESOURCE_UNREF 0x0102
#define GPU_CMD_SET_SCANOUT 0x0103
#define GPU_CMD_RESOURCE_FLUSH 0x0104
#define GPU_CMD_TRANSFER_TO_HOST_2D 0x0105
#define GPU_CMD_RESOURCE_ATTACH_BACKING 0x0106
#define GPU_CMD_RESOURCE_DETACH_BACKING 0x0107
#define GPU_CMD_CTX_CREATE 0x0200
#define GPU_CMD_CTX_DESTROY 0x0201
#define GPU_CMD_CTX_ATTACH_RESOURCE 0x0202
#define GPU_CMD_CTX_DETACH_RESOURCE 0x0203
#define GPU_CMD_RESOURCE_CREATE_3D 0x0204
#define GPU_CMD_SUBMIT_3D 0x0207
#define GPU_CMD_UPDATE_CURSOR 0x0300
#define GPU_CMD_MOVE_CURSOR 0x0301

#define GPU_RESP_OK_NODATA 0x1100
#define GPU_RESP_OK_DISPLAY_INFO 0x1101
#define GPU_RESP_ERR_UNSPEC 0x1200
#define GPU_RESP_ERR_OUT_OF_MEMORY 0x1201
#define GPU_RESP_ERR_INVALID_SCANOUT_ID 0x1202
#define GPU_RESP_ERR_INVALID_RESOURCE_ID 0x1203
#define GPU_RESP_ERR_INVALID_CONTEXT_ID 0x1204
#define GPU_RESP_ERR_INVALID_PARAMETER 0x1205

#define GPU_FLAG_FENCE 1u
#define GPU_FORMAT_B8G8R8X8_UNORM 2u
#define GPU_MAX_SCANOUTS 16

struct gpu_hdr {
    uint32_t type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint8_t ring_idx;
    uint8_t padding[3];
};

struct gpu_rect {
    uint32_t x, y, width, height;
};

struct gpu_create_2d {
    struct gpu_hdr hdr;
    uint32_t resource_id, format, width, height;
};

struct gpu_create_3d {
    struct gpu_hdr hdr;
    uint32_t resource_id, target, format, bind, width, height, depth;
    uint32_t array_size, last_level, nr_samples, flags, padding;
};

struct gpu_resource {
    struct gpu_hdr hdr;
    uint32_t resource_id, padding;
};

struct gpu_set_scanout {
    struct gpu_hdr hdr;
    struct gpu_rect r;
    uint32_t scanout_id, resource_id;
};

struct gpu_flush {
    struct gpu_hdr hdr;
    struct gpu_rect r;
    uint32_t resource_id, padding;
};

struct gpu_transfer_2d {
    struct gpu_hdr hdr;
    struct gpu_rect r;
    uint64_t offset;
    uint32_t resource_id, padding;
};

struct gpu_attach {
    struct gpu_hdr hdr;
    uint32_t resource_id, nr_entries;
};

struct gpu_mem_entry {
    uint64_t addr;
    uint32_t length, padding;
};

struct gpu_display_info {
    struct gpu_hdr hdr;
    struct {
        struct gpu_rect r;
        uint32_t enabled, flags;
    } pmodes[GPU_MAX_SCANOUTS];
};

struct gpu_ctx_create {
    struct gpu_hdr hdr;
    uint32_t nlen, context_init;
    char debug_name[64];
};

struct gpu_submit {
    struct gpu_hdr hdr;
    uint32_t size, padding;
};

/* A recorded stream: this magic, then records of a kind, a length and bytes. */
#define REC_MAGIC "VGSREC1\n"
#define REC_CREATE_3D 1u /* gpu_create_3d without its header: 48 bytes */
#define REC_SUBMIT 2u    /* virgl commands for SUBMIT_3D, whole dwords */

/* virgl's encoding, from Mesa's virgl_protocol.h and p_defines.h. */
#define VIRGL_CMD0(cmd, obj, len) ((uint32_t)(cmd) | ((uint32_t)(obj) << 8) | ((uint32_t)(len) << 16))
#define VIRGL_CCMD_CREATE_OBJECT 1
#define VIRGL_CCMD_SET_FRAMEBUFFER_STATE 5
#define VIRGL_CCMD_CLEAR 7
#define VIRGL_CCMD_SET_SUB_CTX 28
#define VIRGL_CCMD_CREATE_SUB_CTX 29
#define VIRGL_OBJECT_SURFACE 8
#define PIPE_TEXTURE_2D 2
#define PIPE_BIND_RENDER_TARGET 2
#define PIPE_CLEAR_COLOR0 4

/* The test image: a 32 by 32 resource of 32-bit pixels. */
#define IMG_W 32u
#define IMG_H 32u
#define IMG_BYTES (IMG_W * IMG_H * 4u)

static const char *prog = "peer";
static double timeout_s = 10.0;

static void die(const char *fmt, const char *arg)
{
    fprintf(stderr, "%s: ", prog);
    fprintf(stderr, fmt, arg);
    fputc('\n', stderr);
    exit(1);
}

static double now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void pause_briefly(void)
{
    struct timespec ts = {0, 50000};

    nanosleep(&ts, 0);
}

/** Return the CRC-32 of n bytes, as zlib computes it. */
static uint32_t crc32(const uint8_t *p, size_t n)
{
    uint32_t c = 0xffffffffu;
    size_t i;
    int k;

    for (i = 0; i < n; i++) {
        c ^= p[i];
        for (k = 0; k < 8; k++) {
            c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1u)));
        }
    }
    return ~c;
}

/* Copy between private memory and the region, which is volatile. */
static void region_get(volatile uint8_t *base, uint64_t off, void *dst, size_t n)
{
    uint8_t *d = dst;
    size_t i;

    for (i = 0; i < n; i++) {
        d[i] = base[off + i];
    }
}

static void region_put(volatile uint8_t *base, uint64_t off, const void *src, size_t n)
{
    const uint8_t *s = src;
    size_t i;

    for (i = 0; i < n; i++) {
        base[off + i] = s[i];
    }
}

static void region_zero(volatile uint8_t *base, uint64_t off, uint64_t n)
{
    uint64_t i;

    for (i = 0; i < n; i++) {
        base[off + i] = 0;
    }
}

/* ---- The loopback device. ---- */

/* The device's limits; the region's heap bounds what it can hold anyway. */
static const struct vgs_limits dev_limits = {16, 4, 1u << 20, 16, 8, 4096, 64, 0};

#define DEV_OBJS 32

/* A resource the loopback device holds beside its table entry. */
struct dev_res {
    uint32_t w, h;
    uint8_t *image; /* w * h * 4 bytes: the host's copy */
    uint32_t nent;
    struct gpu_mem_entry ent[16]; /* checked backing, at most max_entries */
};

struct dev {
    volatile uint8_t *base;
    struct vgs_layout l;
    struct vgs_device me; /* the offer, as written */
    struct vgs_driver drv;
    struct vgs_table t;
    struct vgs_obj obj[DEV_OBJS];
    struct dev_res res[DEV_OBJS];
    uint16_t last[VGS_QUEUES]; /* avail entries consumed */
    uint16_t used[VGS_QUEUES]; /* used entries published */
    uint32_t scanout;          /* resource id on scanout 0, or 0 */
    int report;                /* pipe to the driver for flushes */
    int virgl;
};

static void dev_publish(struct dev *d, uint32_t state, uint32_t err)
{
    volatile struct vgs_device *v = VGS_DEVICE(d->base);

    d->me.state = state;
    d->me.error = err;
    v->error = err;
    VGS_MB();
    v->state = state;
    VGS_MB();
}

/* Write the device's offer for epoch, every field before the state. */
static void dev_offer(struct dev *d, uint32_t epoch)
{
    volatile struct vgs_device *v = VGS_DEVICE(d->base);
    uint32_t i;

    for (i = 0; i < DEV_OBJS; i++) {
        free(d->res[i].image);
        memset(&d->res[i], 0, sizeof(d->res[i]));
    }
    vgs_table_epoch(&d->t, epoch);
    memset(d->last, 0, sizeof(d->last));
    memset(d->used, 0, sizeof(d->used));
    d->scanout = 0;
    memset(&d->me, 0, sizeof(d->me));
    d->me.epoch = epoch;
    d->me.state = VGS_DEV_READY;
    d->me.peer = 1;
    d->me.features = VGS_F_VERSION_1 | (1ULL << GPU_F_VIRGL);
    d->me.limits = dev_limits;
    d->me.num_scanouts = 1;
    d->me.num_capsets = 1;
    v->state = VGS_DEV_NONE;
    VGS_MB();
    region_put(d->base, VGS_DEVICE_OFF + 8, (const uint8_t *)&d->me + 8, sizeof(d->me) - 8);
    VGS_MB();
    v->epoch = epoch;
    VGS_MB();
    v->state = VGS_DEV_READY;
    VGS_MB();
}

static struct dev_res *dev_res(struct dev *d, uint32_t id)
{
    struct vgs_obj *o = vgs_table_find(&d->t, VGS_OBJ_RESOURCE, id);

    return o ? &d->res[o - d->obj] : 0;
}

static int rect_in(const struct gpu_rect *r, uint32_t w, uint32_t h)
{
    return r->width != 0 && r->height != 0 && r->x <= w && r->width <= w - r->x && r->y <= h &&
           r->height <= h - r->y;
}

/* Copy n bytes at offset off of a resource's backing out of the region. */
static int dev_backing_get(struct dev *d, struct dev_res *r, uint64_t off, uint8_t *dst, uint64_t n)
{
    uint32_t i;

    for (i = 0; i < r->nent && n != 0; i++) {
        if (off >= r->ent[i].length) {
            off -= r->ent[i].length;
            continue;
        }
        {
            uint64_t k = r->ent[i].length - off;

            if (k > n) {
                k = n;
            }
            region_get(d->base, r->ent[i].addr + off, dst, (size_t)k);
            dst += k;
            n -= k;
            off = 0;
        }
    }
    return n == 0;
}

/* Count the virgl commands in a stream, or return -1 when one overruns it. */
static long virgl_frame(const uint8_t *p, uint32_t bytes)
{
    uint32_t n = bytes / 4, i = 0;
    long cmds = 0;

    while (i < n) {
        uint32_t w, len;

        memcpy(&w, p + 4 * i, 4);
        len = w >> 16;
        if (len > n - i - 1) {
            return -1;
        }
        i += 1 + len;
        cmds++;
    }
    return cmds;
}

/*
 * Carry out one command of rlen bytes in cmd, writing the response to resp
 * and returning its length. A command the device refuses gets an error
 * response; the transport is not at fault.
 */
static uint32_t dev_command(struct dev *d, const uint8_t *cmd, uint32_t rlen, uint8_t *resp)
{
    struct gpu_hdr h, *o = (struct gpu_hdr *)resp;
    uint32_t type = GPU_RESP_OK_NODATA, rsize = sizeof(struct gpu_hdr);

    memset(resp, 0, sizeof(struct gpu_display_info));
    if (rlen < sizeof(h)) {
        o->type = GPU_RESP_ERR_INVALID_PARAMETER;
        return rsize;
    }
    memcpy(&h, cmd, sizeof(h));
#define NEED(T)                                                                                                        \
    do {                                                                                                               \
        if (rlen < sizeof(T)) {                                                                                        \
            type = GPU_RESP_ERR_INVALID_PARAMETER;                                                                     \
            goto out;                                                                                                  \
        }                                                                                                              \
    } while (0)
    switch (h.type) {
    case GPU_CMD_GET_DISPLAY_INFO: {
        struct gpu_display_info *di = (struct gpu_display_info *)resp;

        di->pmodes[0].r.width = 1024;
        di->pmodes[0].r.height = 768;
        di->pmodes[0].enabled = 1;
        type = GPU_RESP_OK_DISPLAY_INFO;
        rsize = sizeof(*di);
        break;
    }
    case GPU_CMD_RESOURCE_CREATE_2D:
    case GPU_CMD_RESOURCE_CREATE_3D: {
        uint32_t id, w, hh, depth = 1, layers = 1;
        int err;

        if (h.type == GPU_CMD_RESOURCE_CREATE_2D) {
            struct gpu_create_2d c;

            NEED(c);
            memcpy(&c, cmd, sizeof(c));
            id = c.resource_id;
            w = c.width;
            hh = c.height;
            if (c.format != GPU_FORMAT_B8G8R8X8_UNORM && c.format != 1) {
                type = GPU_RESP_ERR_INVALID_PARAMETER;
                break;
            }
        } else {
            struct gpu_create_3d c;

            NEED(c);
            memcpy(&c, cmd, sizeof(c));
            if (!d->virgl) {
                type = GPU_RESP_ERR_UNSPEC;
                break;
            }
            id = c.resource_id;
            w = c.width;
            hh = c.height;
            depth = c.depth;
            layers = c.array_size;
        }
        if (w == 0 || hh == 0 || depth == 0 || layers == 0 ||
            (uint64_t)w * hh * depth * layers * 4 > d->t.limits.max_backing) {
            type = GPU_RESP_ERR_INVALID_PARAMETER;
            break;
        }
        err = vgs_table_add(&d->t, VGS_OBJ_RESOURCE, id);
        if (err == VGS_E_QUOTA) {
            type = GPU_RESP_ERR_OUT_OF_MEMORY;
            break;
        }
        if (err != VGS_OK) {
            type = GPU_RESP_ERR_INVALID_RESOURCE_ID;
            break;
        }
        {
            struct dev_res *r = dev_res(d, id);

            r->w = w;
            r->h = hh * depth * layers;
            r->image = calloc((size_t)w * r->h, 4);
            if (r->image == 0) {
                vgs_table_del(&d->t, vgs_table_find(&d->t, VGS_OBJ_RESOURCE, id));
                type = GPU_RESP_ERR_OUT_OF_MEMORY;
            }
        }
        break;
    }
    case GPU_CMD_RESOURCE_UNREF: {
        struct gpu_resource c;
        struct dev_res *r;

        NEED(c);
        memcpy(&c, cmd, sizeof(c));
        if ((r = dev_res(d, c.resource_id)) == 0) {
            type = GPU_RESP_ERR_INVALID_RESOURCE_ID;
            break;
        }
        free(r->image);
        memset(r, 0, sizeof(*r));
        if (d->scanout == c.resource_id) {
            d->scanout = 0;
        }
        vgs_table_del(&d->t, vgs_table_find(&d->t, VGS_OBJ_RESOURCE, c.resource_id));
        break;
    }
    case GPU_CMD_RESOURCE_ATTACH_BACKING: {
        struct gpu_attach c;
        struct dev_res *r;
        uint64_t total = 0;
        uint32_t i;

        NEED(c);
        memcpy(&c, cmd, sizeof(c));
        if ((r = dev_res(d, c.resource_id)) == 0) {
            type = GPU_RESP_ERR_INVALID_RESOURCE_ID;
            break;
        }
        if (r->nent != 0 || c.nr_entries == 0 || c.nr_entries > d->t.limits.max_entries ||
            rlen < sizeof(c) + (uint64_t)c.nr_entries * sizeof(struct gpu_mem_entry)) {
            type = GPU_RESP_ERR_INVALID_PARAMETER;
            break;
        }
        for (i = 0; i < c.nr_entries; i++) {
            struct gpu_mem_entry e;

            memcpy(&e, cmd + sizeof(c) + i * sizeof(e), sizeof(e));
            if (vgs_check_range(&d->l, e.addr, e.length) != VGS_OK) {
                type = GPU_RESP_ERR_INVALID_PARAMETER;
                goto out;
            }
            r->ent[i] = e;
            total += e.length;
        }
        if (vgs_table_charge(&d->t, vgs_table_find(&d->t, VGS_OBJ_RESOURCE, c.resource_id), total) != VGS_OK) {
            type = GPU_RESP_ERR_OUT_OF_MEMORY;
            break;
        }
        r->nent = c.nr_entries;
        break;
    }
    case GPU_CMD_RESOURCE_DETACH_BACKING: {
        struct gpu_resource c;
        struct dev_res *r;

        NEED(c);
        memcpy(&c, cmd, sizeof(c));
        if ((r = dev_res(d, c.resource_id)) == 0 || r->nent == 0) {
            type = r ? GPU_RESP_ERR_UNSPEC : GPU_RESP_ERR_INVALID_RESOURCE_ID;
            break;
        }
        vgs_table_charge(&d->t, vgs_table_find(&d->t, VGS_OBJ_RESOURCE, c.resource_id), 0);
        r->nent = 0;
        break;
    }
    case GPU_CMD_TRANSFER_TO_HOST_2D: {
        struct gpu_transfer_2d c;
        struct dev_res *r;
        uint32_t y, stride;

        NEED(c);
        memcpy(&c, cmd, sizeof(c));
        if ((r = dev_res(d, c.resource_id)) == 0) {
            type = GPU_RESP_ERR_INVALID_RESOURCE_ID;
            break;
        }
        if (!rect_in(&c.r, r->w, r->h) || r->nent == 0) {
            type = GPU_RESP_ERR_INVALID_PARAMETER;
            break;
        }
        stride = r->w * 4;
        for (y = 0; y < c.r.height; y++) {
            uint64_t src = c.offset + (uint64_t)y * stride;
            uint8_t *dst = r->image + ((size_t)(c.r.y + y) * r->w + c.r.x) * 4;

            if (!dev_backing_get(d, r, src, dst, (uint64_t)c.r.width * 4)) {
                type = GPU_RESP_ERR_INVALID_PARAMETER;
                break;
            }
        }
        break;
    }
    case GPU_CMD_SET_SCANOUT: {
        struct gpu_set_scanout c;
        struct dev_res *r;

        NEED(c);
        memcpy(&c, cmd, sizeof(c));
        if (c.scanout_id >= d->me.num_scanouts) {
            type = GPU_RESP_ERR_INVALID_SCANOUT_ID;
            break;
        }
        if (c.resource_id == 0) {
            d->scanout = 0;
            break;
        }
        if ((r = dev_res(d, c.resource_id)) == 0) {
            type = GPU_RESP_ERR_INVALID_RESOURCE_ID;
            break;
        }
        if (!rect_in(&c.r, r->w, r->h)) {
            type = GPU_RESP_ERR_INVALID_PARAMETER;
            break;
        }
        d->scanout = c.resource_id;
        break;
    }
    case GPU_CMD_RESOURCE_FLUSH: {
        struct gpu_flush c;
        struct dev_res *r;

        NEED(c);
        memcpy(&c, cmd, sizeof(c));
        if ((r = dev_res(d, c.resource_id)) == 0) {
            type = GPU_RESP_ERR_INVALID_RESOURCE_ID;
            break;
        }
        if (!rect_in(&c.r, r->w, r->h)) {
            type = GPU_RESP_ERR_INVALID_PARAMETER;
            break;
        }
        if (d->scanout == c.resource_id && d->report >= 0) {
            uint32_t crc = crc32(r->image, (size_t)r->w * r->h * 4);

            if (write(d->report, &crc, sizeof(crc)) != sizeof(crc)) {
                type = GPU_RESP_ERR_UNSPEC;
            }
        }
        break;
    }
    case GPU_CMD_CTX_CREATE: {
        struct gpu_ctx_create c;
        int err;

        NEED(c);
        memcpy(&c, cmd, sizeof(c));
        if (!d->virgl || c.nlen > sizeof(c.debug_name) || c.context_init != 0) {
            type = GPU_RESP_ERR_INVALID_PARAMETER;
            break;
        }
        err = vgs_table_add(&d->t, VGS_OBJ_CONTEXT, h.ctx_id);
        if (err != VGS_OK) {
            type = err == VGS_E_QUOTA ? GPU_RESP_ERR_OUT_OF_MEMORY : GPU_RESP_ERR_INVALID_CONTEXT_ID;
        }
        break;
    }
    case GPU_CMD_CTX_DESTROY: {
        struct vgs_obj *c = vgs_table_find(&d->t, VGS_OBJ_CONTEXT, h.ctx_id);

        if (c == 0) {
            type = GPU_RESP_ERR_INVALID_CONTEXT_ID;
            break;
        }
        vgs_table_del(&d->t, c);
        break;
    }
    case GPU_CMD_CTX_ATTACH_RESOURCE:
    case GPU_CMD_CTX_DETACH_RESOURCE: {
        struct gpu_resource c;

        NEED(c);
        memcpy(&c, cmd, sizeof(c));
        if (vgs_table_find(&d->t, VGS_OBJ_CONTEXT, h.ctx_id) == 0) {
            type = GPU_RESP_ERR_INVALID_CONTEXT_ID;
        } else if (dev_res(d, c.resource_id) == 0) {
            type = GPU_RESP_ERR_INVALID_RESOURCE_ID;
        }
        break;
    }
    case GPU_CMD_SUBMIT_3D: {
        struct gpu_submit c;
        long cmds;

        NEED(c);
        memcpy(&c, cmd, sizeof(c));
        if (vgs_table_find(&d->t, VGS_OBJ_CONTEXT, h.ctx_id) == 0) {
            type = GPU_RESP_ERR_INVALID_CONTEXT_ID;
            break;
        }
        if (c.size % 4 != 0 || c.size > rlen - sizeof(c) || (cmds = virgl_frame(cmd + sizeof(c), c.size)) < 0) {
            type = GPU_RESP_ERR_INVALID_PARAMETER;
            break;
        }
        printf("responder: submit: %ld virgl command(s)\n", cmds);
        break;
    }
    default:
        type = GPU_RESP_ERR_UNSPEC;
        break;
    }
#undef NEED
out:
    o->type = type;
    if (h.flags & GPU_FLAG_FENCE) {
        o->flags = GPU_FLAG_FENCE;
        o->fence_id = h.fence_id;
        o->ctx_id = h.ctx_id;
    }
    return rsize;
}

/*
 * Serve one queue's available chains. Returns VGS_OK, or the transport
 * fault that puts the device in NEEDS_RESET.
 */
static int dev_queue(struct dev *d, int qi)
{
    const struct vgs_queue *q = &d->drv.queue[qi];
    static uint8_t cmd[8192], resp[sizeof(struct gpu_display_info)];
    uint16_t idx;
    int err;

    if (q->size == 0) {
        return VGS_OK;
    }
    if ((err = vgs_avail_read(d->base, q, d->last[qi], &idx)) != VGS_OK) {
        return err;
    }
    while (d->last[qi] != idx) {
        struct vgs_chain c;
        uint32_t head = vgs_avail_head(d->base, q, d->last[qi]), i, off = 0, rsize = 0, put = 0;

        if ((err = vgs_chain_read(d->base, &d->l, q, &d->me.limits, head, &c)) != VGS_OK) {
            return err;
        }
        for (i = 0; i < c.nread; i++) {
            region_get(d->base, c.seg[i].addr, cmd + off, c.seg[i].len);
            off += c.seg[i].len;
        }
        if (qi == VGS_QUEUE_CONTROL) {
            rsize = dev_command(d, cmd, off, resp);
            for (i = c.nread; i < c.n && put < rsize; i++) {
                uint32_t k = c.seg[i].len < rsize - put ? c.seg[i].len : rsize - put;

                region_put(d->base, c.seg[i].addr, resp + put, k);
                put += k;
            }
        }
        d->last[qi]++;
        vgs_used_push(d->base, q, &d->used[qi], head, put);
    }
    return VGS_OK;
}

/*
 * The loopback device: answer the driver's handshake for each epoch, serve
 * its queues, and stop when the driver detaches or the parent goes. Returns
 * the process's exit status.
 */
static int responder(volatile uint8_t *base, uint64_t size, int report)
{
    struct dev *d = calloc(1, sizeof(*d));
    int err;

    d->base = base;
    d->report = report;
    vgs_table_init(&d->t, d->obj, DEV_OBJS, &dev_limits, 0);
    for (;;) {
        struct vgs_driver drv;

        if (getppid() == 1) {
            return 1;
        }
        if (vgs_snap_driver(base, &drv) != VGS_OK || drv.state == VGS_DRV_NONE) {
            pause_briefly();
            continue;
        }
        if (drv.state == VGS_DRV_GONE) {
            dev_publish(d, VGS_DEV_GONE, VGS_OK);
            return 0;
        }
        if (drv.epoch != d->me.epoch) {
            if (vgs_snap_layout(base, &d->l) != VGS_OK || vgs_check_layout(&d->l, size) != VGS_OK) {
                pause_briefly();
                continue;
            }
            dev_offer(d, drv.epoch);
            printf("responder: offer: epoch %u\n", (unsigned)drv.epoch);
            continue;
        }
        if (d->me.state == VGS_DEV_READY && drv.state == VGS_DRV_OK) {
            err = vgs_check_accept(&d->l, &drv, &d->me);
            if (err != VGS_OK) {
                printf("responder: refused: %s\n", vgs_strerror(err));
                dev_publish(d, VGS_DEV_FAILED, (uint32_t)err);
                continue;
            }
            d->drv = drv;
            d->virgl = (drv.features >> GPU_F_VIRGL) & 1;
            dev_publish(d, VGS_DEV_RUNNING, VGS_OK);
            continue;
        }
        if (d->me.state == VGS_DEV_RUNNING) {
            if ((err = dev_queue(d, VGS_QUEUE_CONTROL)) == VGS_OK) {
                err = dev_queue(d, VGS_QUEUE_CURSOR);
            }
            if (err != VGS_OK) {
                printf("responder: fault: %s\n", vgs_strerror(err));
                dev_publish(d, VGS_DEV_NEEDS_RESET, (uint32_t)err);
            }
        }
        fflush(stdout);
        pause_briefly();
    }
}

/* ---- The driver. ---- */

struct drv {
    volatile uint8_t *base;
    uint64_t size;
    struct vgs_layout l;
    struct vgs_driver me;
    struct vgs_device dev; /* the device's offer for this epoch */
    uint16_t avail, used;  /* the control queue's indices */
    uint64_t brk, mark;    /* the heap's allocator: bump, and a mark */
    pid_t child;           /* the loopback device, or 0 */
    int report;            /* the loopback device's flush reports, or -1 */
    const char *dump;      /* directory for seeds, or 0 */
    int ndump;
    uint64_t fence;
};

/* Write a copy of the region as a seed when seeds are asked for. */
static void dump(struct drv *dr, const char *name, const uint8_t *img)
{
    char path[1024];
    FILE *f;

    if (dr->dump == 0) {
        return;
    }
    snprintf(path, sizeof(path), "%s/%02d-%s", dr->dump, ++dr->ndump, name);
    if ((f = fopen(path, "wb")) == 0 || fwrite(img, 1, (size_t)dr->size, f) != dr->size || fclose(f) != 0) {
        die("cannot write %s", path);
    }
}

/* Return a private copy of the region, for a seed. */
static uint8_t *snapshot(struct drv *dr)
{
    uint8_t *img = malloc((size_t)dr->size);

    VGS_MB();
    region_get(dr->base, 0, img, (size_t)dr->size);
    return img;
}

/** Allocate n bytes of the heap, 64-aligned; a full heap is fatal here. */
static uint64_t heap_alloc(struct drv *dr, uint64_t n)
{
    uint64_t a = (dr->brk + 63) & ~63ULL;

    if (n > dr->l.heap_off + dr->l.heap_len - a) {
        die("heap full: %s", "the region is too small");
    }
    dr->brk = a + n;
    return a;
}

static void device_alive(struct drv *dr)
{
    int st;

    if (dr->child != 0 && waitpid(dr->child, &st, WNOHANG) == dr->child) {
        dr->child = 0;
        die("the device died: %s", "loopback responder exited");
    }
}

/* Wait until the device's block answers this epoch in one of two states. */
static void wait_device(struct drv *dr, uint32_t want, uint32_t want2)
{
    double end = now() + timeout_s;

    for (;;) {
        if (vgs_snap_device(dr->base, &dr->dev) == VGS_OK && dr->dev.epoch == dr->me.epoch) {
            if (dr->dev.state == want || dr->dev.state == want2) {
                return;
            }
            if (dr->dev.state == VGS_DEV_FAILED || dr->dev.state == VGS_DEV_NEEDS_RESET) {
                die("the device stopped: %s", vgs_strerror((int)dr->dev.error));
            }
        }
        device_alive(dr);
        if (now() > end) {
            die("no answer from the device: %s", "timed out");
        }
        pause_briefly();
    }
}

/* Store the driver's block: fields first, then epoch, then state. */
static void drv_store(volatile uint8_t *base, const struct vgs_driver *me)
{
    volatile struct vgs_driver *v = VGS_DRIVER(base);

    v->state = VGS_DRV_RESET;
    VGS_MB();
    region_put(base, VGS_DRIVER_OFF + 8, (const uint8_t *)me + 8, sizeof(*me) - 8);
    VGS_MB();
    v->epoch = me->epoch;
    VGS_MB();
    v->state = me->state;
    VGS_MB();
}

/*
 * Start a new epoch: lay the region out when it is not, store the driver's
 * block in RESET, take the device's offer, accept it and wait for RUNNING.
 */
static void handshake(struct drv *dr, int fresh, uint64_t want)
{
    struct vgs_layout l;
    uint8_t *img;
    uint64_t off;
    int err;

    if (fresh) {
        if ((err = vgs_layout_make(&l, dr->size, VGS_PAGE)) != VGS_OK) {
            die("bad region size: %s", vgs_strerror(err));
        }
        img = snapshot(dr);
        memset(img, 0, VGS_CTRL_SIZE);
        memcpy(img + VGS_LAYOUT_OFF, &l, sizeof(l));
        dr->l = l;
    } else {
        img = snapshot(dr);
    }
    memset(&dr->me, 0, sizeof(dr->me));
    dr->me.epoch = fresh ? 1 : VGS_DRIVER(dr->base)->epoch + 1;
    if (dr->me.epoch == 0) {
        dr->me.epoch = 1;
    }
    dr->me.state = VGS_DRV_RESET;
    dr->me.peer = 0;
    memcpy(img + VGS_DRIVER_OFF, &dr->me, sizeof(dr->me));
    dump(dr, "reset", img);
    if (fresh) {
        /* A new region's control area starts empty; the layout goes in
           before any state, its magic last. */
        VGS_LAYOUT(dr->base)->magic = 0;
        VGS_MB();
        region_zero(dr->base, 4, VGS_CTRL_SIZE - 4);
        VGS_MB();
        region_put(dr->base, 4, (const uint8_t *)&l + 4, sizeof(l) - 4);
        VGS_MB();
        VGS_LAYOUT(dr->base)->magic = VGS_MAGIC;
    }
    drv_store(dr->base, &dr->me);
    printf("peer: reset: epoch %u\n", (unsigned)dr->me.epoch);

    wait_device(dr, VGS_DEV_READY, VGS_DEV_READY);
    if ((err = vgs_check_offer(&dr->dev, dr->me.epoch, dr->me.peer)) != VGS_OK) {
        die("bad offer: %s", vgs_strerror(err));
    }
    dr->me.features = dr->dev.features & (VGS_F_VERSION_1 | want);
    dr->me.transport_features = 0;
    off = vgs_queue_place(&dr->me.queue[VGS_QUEUE_CONTROL], dr->l.ring_off,
                          dr->dev.limits.max_queue < 64 ? dr->dev.limits.max_queue : 64);
    vgs_queue_place(&dr->me.queue[VGS_QUEUE_CURSOR], off, 16 < dr->dev.limits.max_queue ? 16 : dr->dev.limits.max_queue);
    dr->me.state = VGS_DRV_OK;
    free(img);
    /* Rings start empty in each epoch. */
    img = snapshot(dr);
    memcpy(img + VGS_DRIVER_OFF, &dr->me, sizeof(dr->me));
    memset(img + dr->l.ring_off, 0, (size_t)dr->l.ring_len);
    dump(dr, "accept", img);
    free(img);
    region_zero(dr->base, dr->l.ring_off, dr->l.ring_len);
    drv_store(dr->base, &dr->me);
    wait_device(dr, VGS_DEV_RUNNING, VGS_DEV_RUNNING);
    dr->avail = dr->used = 0;
    dr->brk = dr->l.heap_off;
    printf("peer: running: features 0x%llx\n", (unsigned long long)dr->me.features);
}

/*
 * Send one command on the control queue: cmd of clen bytes, then an
 * optional second readable buffer, then a response buffer of rlen bytes.
 * Waits for the device, checks the used element and returns the response
 * type, with the response copied to resp.
 */
static uint32_t send_cmd(struct drv *dr, const char *name, void *cmd, uint32_t clen, const void *data,
                         uint32_t dlen, void *resp, uint32_t rlen)
{
    const struct vgs_queue *q = &dr->me.queue[VGS_QUEUE_CONTROL];
    struct gpu_hdr *h = cmd, r;
    struct vgs_desc ds[3];
    uint32_t n = 0, i, id, len;
    uint64_t mark = dr->brk;
    uint8_t *img;
    double end;

    if (h->flags & GPU_FLAG_FENCE) {
        h->fence_id = ++dr->fence;
    }
    ds[n].addr = heap_alloc(dr, clen);
    ds[n].len = clen;
    region_put(dr->base, ds[n].addr, cmd, clen);
    n++;
    if (dlen != 0) {
        ds[n].addr = heap_alloc(dr, dlen);
        ds[n].len = dlen;
        region_put(dr->base, ds[n].addr, data, dlen);
        n++;
    }
    ds[n].addr = heap_alloc(dr, rlen);
    ds[n].len = rlen;
    n++;
    for (i = 0; i < n; i++) {
        ds[i].flags = (uint16_t)((i + 1 < n ? VGS_DESC_F_NEXT : 0) | (i + 1 == n ? VGS_DESC_F_WRITE : 0));
        ds[i].next = (uint16_t)(i + 1 < n ? i + 1 : 0);
        vgs_desc_write(dr->base, q, i, &ds[i]);
    }
    img = snapshot(dr);
    {
        uint16_t tmp = dr->avail;

        vgs_avail_push(img, q, &tmp, 0);
    }
    dump(dr, name, img);
    free(img);
    vgs_avail_push(dr->base, q, &dr->avail, 0);

    end = now() + timeout_s;
    for (;;) {
        uint16_t idx;
        int err = vgs_used_read(dr->base, q, dr->used, &idx);

        if (err != VGS_OK) {
            die("bad used ring: %s", vgs_strerror(err));
        }
        if (idx != dr->used) {
            break;
        }
        wait_device(dr, VGS_DEV_RUNNING, VGS_DEV_RUNNING);
        if (now() > end) {
            die("no answer to %s", name);
        }
        pause_briefly();
    }
    if (vgs_used_elem(dr->base, q, dr->used, &id, &len) != VGS_OK || id != 0 || len > rlen || len < sizeof(r)) {
        die("bad used element for %s", name);
    }
    dr->used++;
    region_get(dr->base, ds[n - 1].addr, resp, len);
    memcpy(&r, resp, sizeof(r));
    if ((h->flags & GPU_FLAG_FENCE) && (!(r.flags & GPU_FLAG_FENCE) || r.fence_id != h->fence_id)) {
        die("fence not answered for %s", name);
    }
    dr->brk = mark;
    return r.type;
}

static void expect(uint32_t got, uint32_t want, const char *name)
{
    if (got != want) {
        fprintf(stderr, "%s: %s: response 0x%x, expected 0x%x\n", prog, name, (unsigned)got, (unsigned)want);
        exit(1);
    }
    printf("peer: %s: ok\n", name);
}

static void hdr(void *cmd, size_t n, uint32_t type, uint32_t flags, uint32_t ctx)
{
    struct gpu_hdr *h = cmd;

    memset(cmd, 0, n);
    h->type = type;
    h->flags = flags;
    h->ctx_id = ctx;
}

static uint32_t check_flush_report(struct drv *dr, uint32_t want)
{
    uint32_t got = 0;

    if (dr->report < 0) {
        return 1;
    }
    if (read(dr->report, &got, sizeof(got)) != sizeof(got)) {
        die("no flush report from %s", "the responder");
    }
    if (got != want) {
        fprintf(stderr, "%s: flush: crc %08x, expected %08x\n", prog, (unsigned)got, (unsigned)want);
        exit(1);
    }
    printf("peer: flush: crc %08x matches\n", (unsigned)got);
    return 1;
}

/* The 2D sequence: a patterned 32 by 32 resource put on scanout 0. */
static uint32_t run_2d(struct drv *dr, uint32_t id, uint64_t *backing)
{
    union {
        struct gpu_display_info di;
        struct gpu_create_2d c2;
        struct gpu_attach at;
        struct gpu_transfer_2d tr;
        struct gpu_set_scanout ss;
        struct gpu_flush fl;
        uint8_t raw[512];
    } u, resp;
    struct gpu_mem_entry e;
    static uint8_t pix[IMG_BYTES];
    uint32_t x, y;

    hdr(&u, sizeof(u.di.hdr), GPU_CMD_GET_DISPLAY_INFO, 0, 0);
    expect(send_cmd(dr, "display-info", &u, sizeof(u.di.hdr), 0, 0, &resp, sizeof(resp.di)),
           GPU_RESP_OK_DISPLAY_INFO, "display-info");

    hdr(&u, sizeof(u.c2), GPU_CMD_RESOURCE_CREATE_2D, 0, 0);
    u.c2.resource_id = id;
    u.c2.format = GPU_FORMAT_B8G8R8X8_UNORM;
    u.c2.width = IMG_W;
    u.c2.height = IMG_H;
    expect(send_cmd(dr, "create-2d", &u, sizeof(u.c2), 0, 0, &resp, sizeof(struct gpu_hdr)), GPU_RESP_OK_NODATA,
           "create-2d");

    for (y = 0; y < IMG_H; y++) {
        for (x = 0; x < IMG_W; x++) {
            uint8_t *p = pix + (y * IMG_W + x) * 4;

            p[0] = (uint8_t)(x * 8);
            p[1] = (uint8_t)(y * 8);
            p[2] = (uint8_t)((x ^ y) * 8);
            p[3] = 0xff;
        }
    }
    *backing = heap_alloc(dr, IMG_BYTES);
    region_put(dr->base, *backing, pix, IMG_BYTES);
    hdr(&u, sizeof(u.at), GPU_CMD_RESOURCE_ATTACH_BACKING, 0, 0);
    u.at.resource_id = id;
    u.at.nr_entries = 1;
    e.addr = *backing;
    e.length = IMG_BYTES;
    e.padding = 0;
    expect(send_cmd(dr, "attach-backing", &u, sizeof(u.at), &e, sizeof(e), &resp, sizeof(struct gpu_hdr)),
           GPU_RESP_OK_NODATA, "attach-backing");

    hdr(&u, sizeof(u.tr), GPU_CMD_TRANSFER_TO_HOST_2D, 0, 0);
    u.tr.r.width = IMG_W;
    u.tr.r.height = IMG_H;
    u.tr.resource_id = id;
    expect(send_cmd(dr, "transfer-2d", &u, sizeof(u.tr), 0, 0, &resp, sizeof(struct gpu_hdr)), GPU_RESP_OK_NODATA,
           "transfer-2d");

    hdr(&u, sizeof(u.ss), GPU_CMD_SET_SCANOUT, 0, 0);
    u.ss.r.width = IMG_W;
    u.ss.r.height = IMG_H;
    u.ss.resource_id = id;
    expect(send_cmd(dr, "set-scanout", &u, sizeof(u.ss), 0, 0, &resp, sizeof(struct gpu_hdr)), GPU_RESP_OK_NODATA,
           "set-scanout");

    hdr(&u, sizeof(u.fl), GPU_CMD_RESOURCE_FLUSH, GPU_FLAG_FENCE, 0);
    u.fl.r.width = IMG_W;
    u.fl.r.height = IMG_H;
    u.fl.resource_id = id;
    expect(send_cmd(dr, "flush", &u, sizeof(u.fl), 0, 0, &resp, sizeof(struct gpu_hdr)), GPU_RESP_OK_NODATA,
           "flush");
    return check_flush_report(dr, crc32(pix, IMG_BYTES));
}

/* Read a whole file into memory; its length goes to *n. */
static uint8_t *slurp(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf = 0;
    size_t cap = 0, got;

    if (f == 0) {
        die("cannot open %s", path);
    }
    *n = 0;
    for (;;) {
        if (*n == cap) {
            cap = cap ? 2 * cap : 65536;
            buf = realloc(buf, cap);
        }
        got = fread(buf + *n, 1, cap - *n, f);
        if (got == 0) {
            break;
        }
        *n += got;
    }
    fclose(f);
    return buf;
}

/*
 * Replay a recorded virgl stream in a new context: each CREATE_3D record
 * makes a resource with backing and attaches it, each SUBMIT record goes in
 * one SUBMIT_3D, and the context and its resources are then destroyed.
 */
static void replay(struct drv *dr, const char *path)
{
    union {
        struct gpu_ctx_create cc;
        struct gpu_create_3d c3;
        struct gpu_attach at;
        struct gpu_resource rs;
        struct gpu_submit sb;
        uint8_t raw[256];
    } u, resp;
    uint32_t made[16], nmade = 0, i, ctx = VGS_ID_MAKE(1, 1);
    size_t n, pos = 8;
    uint8_t *rec = slurp(path, &n);
    uint64_t mark = dr->brk;

    if ((dr->me.features & (1ULL << GPU_F_VIRGL)) == 0) {
        die("cannot replay %s: the device offers no virgl", path);
    }
    if (n < 8 || memcmp(rec, REC_MAGIC, 8) != 0) {
        die("not a recorded stream: %s", path);
    }
    hdr(&u, sizeof(u.cc), GPU_CMD_CTX_CREATE, 0, ctx);
    u.cc.nlen = 4;
    memcpy(u.cc.debug_name, "peer", 4);
    expect(send_cmd(dr, "ctx-create", &u, sizeof(u.cc), 0, 0, &resp, sizeof(struct gpu_hdr)), GPU_RESP_OK_NODATA,
           "ctx-create");
    while (pos < n) {
        uint32_t kind, len;

        if (n - pos < 8) {
            die("truncated record in %s", path);
        }
        memcpy(&kind, rec + pos, 4);
        memcpy(&len, rec + pos + 4, 4);
        pos += 8;
        if (len > n - pos || len % 4 != 0) {
            die("bad record length in %s", path);
        }
        if (kind == REC_CREATE_3D) {
            struct gpu_mem_entry e;
            uint64_t bytes;

            if (len != sizeof(u.c3) - sizeof(struct gpu_hdr) || nmade == 16) {
                die("bad resource record in %s", path);
            }
            hdr(&u, sizeof(u.c3), GPU_CMD_RESOURCE_CREATE_3D, 0, ctx);
            memcpy((uint8_t *)&u.c3 + sizeof(struct gpu_hdr), rec + pos, len);
            bytes = (uint64_t)u.c3.width * u.c3.height * u.c3.depth * u.c3.array_size * 4;
            made[nmade++] = u.c3.resource_id;
            expect(send_cmd(dr, "create-3d", &u, sizeof(u.c3), 0, 0, &resp, sizeof(struct gpu_hdr)),
                   GPU_RESP_OK_NODATA, "create-3d");
            e.addr = heap_alloc(dr, bytes);
            e.length = (uint32_t)bytes;
            e.padding = 0;
            hdr(&u, sizeof(u.at), GPU_CMD_RESOURCE_ATTACH_BACKING, 0, 0);
            u.at.resource_id = made[nmade - 1];
            u.at.nr_entries = 1;
            expect(send_cmd(dr, "attach-3d", &u, sizeof(u.at), &e, sizeof(e), &resp, sizeof(struct gpu_hdr)),
                   GPU_RESP_OK_NODATA, "attach-3d");
            hdr(&u, sizeof(u.rs), GPU_CMD_CTX_ATTACH_RESOURCE, 0, ctx);
            u.rs.resource_id = made[nmade - 1];
            expect(send_cmd(dr, "ctx-attach", &u, sizeof(u.rs), 0, 0, &resp, sizeof(struct gpu_hdr)),
                   GPU_RESP_OK_NODATA, "ctx-attach");
        } else if (kind == REC_SUBMIT) {
            if (sizeof(u.sb) + len > dr->dev.limits.max_cmd) {
                die("a submit in %s is larger than the device takes", path);
            }
            hdr(&u, sizeof(u.sb), GPU_CMD_SUBMIT_3D, GPU_FLAG_FENCE, ctx);
            u.sb.size = len;
            expect(send_cmd(dr, "submit-3d", &u, sizeof(u.sb), rec + pos, len, &resp, sizeof(struct gpu_hdr)),
                   GPU_RESP_OK_NODATA, "submit-3d");
        } else {
            die("unknown record kind in %s", path);
        }
        pos += len;
    }
    for (i = 0; i < nmade; i++) {
        hdr(&u, sizeof(u.rs), GPU_CMD_CTX_DETACH_RESOURCE, 0, ctx);
        u.rs.resource_id = made[i];
        expect(send_cmd(dr, "ctx-detach", &u, sizeof(u.rs), 0, 0, &resp, sizeof(struct gpu_hdr)),
               GPU_RESP_OK_NODATA, "ctx-detach");
        hdr(&u, sizeof(u.rs), GPU_CMD_RESOURCE_UNREF, 0, 0);
        u.rs.resource_id = made[i];
        expect(send_cmd(dr, "unref-3d", &u, sizeof(u.rs), 0, 0, &resp, sizeof(struct gpu_hdr)), GPU_RESP_OK_NODATA,
               "unref-3d");
    }
    hdr(&u, sizeof(struct gpu_hdr), GPU_CMD_CTX_DESTROY, 0, ctx);
    expect(send_cmd(dr, "ctx-destroy", &u, sizeof(struct gpu_hdr), 0, 0, &resp, sizeof(struct gpu_hdr)),
           GPU_RESP_OK_NODATA, "ctx-destroy");
    dr->brk = mark;
    free(rec);
}

/* Write the built-in stream: a render target cleared to one color. */
static void write_stream(const char *path)
{
    uint32_t res[12] = {VGS_ID_MAKE(2, 1), PIPE_TEXTURE_2D, GPU_FORMAT_B8G8R8X8_UNORM, PIPE_BIND_RENDER_TARGET,
                        IMG_W, IMG_H, 1, 1, 0, 0, 0, 0};
    float rgba[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    uint32_t s[64], n = 0, kind;
    FILE *f = fopen(path, "wb");

    s[n++] = VIRGL_CMD0(VIRGL_CCMD_CREATE_SUB_CTX, 0, 1);
    s[n++] = 1;
    s[n++] = VIRGL_CMD0(VIRGL_CCMD_SET_SUB_CTX, 0, 1);
    s[n++] = 1;
    s[n++] = VIRGL_CMD0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SURFACE, 5);
    s[n++] = 1;      /* surface handle */
    s[n++] = res[0]; /* resource */
    s[n++] = GPU_FORMAT_B8G8R8X8_UNORM;
    s[n++] = 0; /* level */
    s[n++] = 0; /* layers */
    s[n++] = VIRGL_CMD0(VIRGL_CCMD_SET_FRAMEBUFFER_STATE, 0, 3);
    s[n++] = 1; /* color buffers */
    s[n++] = 0; /* no depth surface */
    s[n++] = 1; /* color buffer 0: the surface */
    s[n++] = VIRGL_CMD0(VIRGL_CCMD_CLEAR, 0, 8);
    s[n++] = PIPE_CLEAR_COLOR0;
    memcpy(&s[n], rgba, sizeof(rgba));
    n += 4;
    s[n++] = 0; /* depth, a double */
    s[n++] = 0;
    s[n++] = 0; /* stencil */
    if (f == 0) {
        die("cannot write %s", path);
    }
    fwrite(REC_MAGIC, 1, 8, f);
    kind = REC_CREATE_3D;
    fwrite(&kind, 4, 1, f);
    kind = sizeof(res);
    fwrite(&kind, 4, 1, f);
    fwrite(res, 1, sizeof(res), f);
    kind = REC_SUBMIT;
    fwrite(&kind, 4, 1, f);
    kind = n * 4;
    fwrite(&kind, 4, 1, f);
    fwrite(s, 4, n, f);
    if (fclose(f) != 0) {
        die("cannot write %s", path);
    }
}

/*
 * Make a resource in the slot of the one just freed, with the next
 * generation, then reset into a new epoch and show that it is gone.
 */
static void run_reset(struct drv *dr, uint32_t id)
{
    struct gpu_transfer_2d tr;
    struct gpu_create_2d c2;
    uint8_t resp[64];

    hdr(&c2, sizeof(c2), GPU_CMD_RESOURCE_CREATE_2D, 0, 0);
    c2.resource_id = id;
    c2.format = GPU_FORMAT_B8G8R8X8_UNORM;
    c2.width = IMG_W;
    c2.height = IMG_H;
    expect(send_cmd(dr, "recreate-2d", &c2, sizeof(c2), 0, 0, resp, sizeof(struct gpu_hdr)), GPU_RESP_OK_NODATA,
           "recreate-2d");
    handshake(dr, 0, 1ULL << GPU_F_VIRGL);
    hdr(&tr, sizeof(tr), GPU_CMD_TRANSFER_TO_HOST_2D, 0, 0);
    tr.r.width = IMG_W;
    tr.r.height = IMG_H;
    tr.resource_id = id;
    expect(send_cmd(dr, "stale-id", &tr, sizeof(tr), 0, 0, resp, sizeof(struct gpu_hdr)),
           GPU_RESP_ERR_INVALID_RESOURCE_ID, "stale-id");
}

/* Take the 2D resource off the scanout and free it. */
static void run_teardown(struct drv *dr, uint32_t id)
{
    union {
        struct gpu_set_scanout ss;
        struct gpu_resource rs;
    } u;
    uint8_t resp[64];

    hdr(&u, sizeof(u.ss), GPU_CMD_SET_SCANOUT, 0, 0);
    expect(send_cmd(dr, "scanout-off", &u, sizeof(u.ss), 0, 0, resp, sizeof(struct gpu_hdr)), GPU_RESP_OK_NODATA,
           "scanout-off");
    hdr(&u, sizeof(u.rs), GPU_CMD_RESOURCE_DETACH_BACKING, 0, 0);
    u.rs.resource_id = id;
    expect(send_cmd(dr, "detach-backing", &u, sizeof(u.rs), 0, 0, resp, sizeof(struct gpu_hdr)), GPU_RESP_OK_NODATA,
           "detach-backing");
    hdr(&u, sizeof(u.rs), GPU_CMD_RESOURCE_UNREF, 0, 0);
    u.rs.resource_id = id;
    expect(send_cmd(dr, "unref", &u, sizeof(u.rs), 0, 0, resp, sizeof(struct gpu_hdr)), GPU_RESP_OK_NODATA, "unref");
}

/* ---- The seed check. ---- */

/*
 * Check one region image with the header's checks, as either side would
 * read it: the layout, both blocks for the same epoch, the driver's setup
 * against the offer, every chain the device has still to take and every
 * used element the driver has still to take. Returns VGS_OK or the fault.
 */
static int check_image(const uint8_t *img, size_t n)
{
    struct vgs_layout l;
    struct vgs_driver drv;
    struct vgs_device dev;
    int err, qi;

    if (n < VGS_CTRL_SIZE) {
        return VGS_E_LAYOUT;
    }
    if ((err = vgs_snap_layout(img, &l)) != VGS_OK || (err = vgs_check_layout(&l, n)) != VGS_OK) {
        return err;
    }
    if (l.region_size != n) {
        return VGS_E_LAYOUT;
    }
    if ((err = vgs_snap_driver(img, &drv)) != VGS_OK || (err = vgs_snap_device(img, &dev)) != VGS_OK) {
        return err;
    }
    if (drv.epoch == 0 || drv.state == VGS_DRV_NONE || drv.state > VGS_DRV_GONE) {
        return VGS_E_STATE;
    }
    if (dev.state == VGS_DEV_NONE || dev.epoch != drv.epoch) {
        return drv.state == VGS_DRV_RESET ? VGS_OK : VGS_E_EPOCH;
    }
    if ((err = vgs_check_offer(&dev, drv.epoch, drv.peer)) != VGS_OK) {
        return err;
    }
    if (drv.state != VGS_DRV_OK) {
        return VGS_OK;
    }
    if ((err = vgs_check_accept(&l, &drv, &dev)) != VGS_OK) {
        return err;
    }
    for (qi = 0; qi < VGS_QUEUES; qi++) {
        const struct vgs_queue *q = &drv.queue[qi];
        uint16_t used, avail, pos;
        struct vgs_chain c;
        uint32_t id, len;

        if (q->size == 0) {
            continue;
        }
        used = *(const uint16_t *)(img + q->used + 2);
        if ((err = vgs_avail_read(img, q, used, &avail)) != VGS_OK) {
            return err;
        }
        for (pos = used; pos != avail; pos++) {
            if ((err = vgs_chain_read(img, &l, q, &dev.limits, vgs_avail_head(img, q, pos), &c)) != VGS_OK) {
                return err;
            }
        }
        for (pos = (uint16_t)(used - (used < q->size ? used : q->size)); pos != used; pos++) {
            if ((err = vgs_used_elem(img, q, pos, &id, &len)) != VGS_OK) {
                return err;
            }
        }
    }
    return VGS_OK;
}

static int check_files(int argc, char **argv)
{
    int i, bad = 0;

    for (i = 0; i < argc; i++) {
        size_t n;
        uint8_t *img = slurp(argv[i], &n);
        int err = check_image(img, n);

        if (err != VGS_OK) {
            printf("peer: check: %s: %s\n", argv[i], vgs_strerror(err));
            bad++;
        }
        free(img);
    }
    printf("peer: check: %d of %d image(s) pass\n", argc - bad, argc);
    return bad ? 1 : 0;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: %s [-l | -f PATH] [-s SIZE] [-r STREAM] [-d DIR] [-t SECONDS]\n"
            "       %s -c IMAGE...\n"
            "       %s -w STREAM\n",
            prog, prog, prog);
    exit(2);
}

int main(int argc, char **argv)
{
    struct drv dr;
    const char *file = 0, *stream = 0;
    uint64_t size = VGS_REGION_MIN, backing;
    uint32_t id = VGS_ID_MAKE(1, 1), one = 1;
    int loop = 0, opt, fresh = 1;
    int fds[2];

    if (*(uint8_t *)&one != 1) {
        die("%s", "the transport is little-endian");
    }
    memset(&dr, 0, sizeof(dr));
    dr.report = -1;
    while ((opt = getopt(argc, argv, "cd:f:lr:s:t:w:")) != -1) {
        switch (opt) {
        case 'c':
            return check_files(argc - optind, argv + optind);
        case 'd':
            dr.dump = optarg;
            break;
        case 'f':
            file = optarg;
            break;
        case 'l':
            loop = 1;
            break;
        case 'r':
            stream = optarg;
            break;
        case 's':
            size = strtoull(optarg, 0, 0);
            break;
        case 't':
            timeout_s = strtod(optarg, 0);
            break;
        case 'w':
            write_stream(optarg);
            return 0;
        default:
            usage();
        }
    }
    if (loop == (file != 0) || optind != argc) {
        usage();
    }
    setvbuf(stdout, 0, _IOLBF, 0);
    if (file) {
        struct stat st;
        int fd = open(file, O_RDWR);
        struct vgs_layout l;

        if (fd < 0 || fstat(fd, &st) != 0) {
            die("cannot open %s", file);
        }
        size = (uint64_t)st.st_size;
        dr.base = mmap(0, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (dr.base == MAP_FAILED) {
            die("cannot map %s", file);
        }
        close(fd);
        /* A region already laid out keeps its epochs. */
        if (vgs_snap_layout(dr.base, &l) == VGS_OK && vgs_check_layout(&l, size) == VGS_OK) {
            dr.l = l;
            fresh = 0;
        }
    } else {
        dr.base = mmap(0, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
        if (dr.base == MAP_FAILED) {
            die("cannot map %s", "the loopback region");
        }
        if (pipe(fds) != 0) {
            die("%s", "cannot make a pipe");
        }
        fflush(stdout);
        if ((dr.child = fork()) == 0) {
            close(fds[0]);
            _exit(responder(dr.base, size, fds[1]));
        }
        close(fds[1]);
        dr.report = fds[0];
    }
    dr.size = size;

    handshake(&dr, fresh, stream ? 1ULL << GPU_F_VIRGL : 0);
    run_2d(&dr, id, &backing);
    if (stream) {
        replay(&dr, stream);
    }
    run_teardown(&dr, id);
    if (loop) {
        run_reset(&dr, VGS_ID_MAKE(VGS_ID_SLOT(id), VGS_ID_GEN(id) + 1));
    }
    dr.me.state = VGS_DRV_GONE;
    drv_store(dr.base, &dr.me);
    if (dr.child) {
        int st;

        if (waitpid(dr.child, &st, 0) != dr.child || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
            die("%s", "the responder failed");
        }
    }
    printf("peer: result: ok\n");
    return 0;
}
