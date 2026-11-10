/**
 * Fuzz harness for the device side of the shared-memory GPU transport
 * (bed/vm/backend/device.c), the parser that the VM's backend runs over a
 * region Rach2's driver writes. The input is a run of 32 KiB frames, each a
 * whole region as the driver left it, the last padded with zeros: the first
 * is the region, and each later one is copied over it but for the device's
 * own block, as a driver that went on writing would leave it. After each
 * frame the device steps as it would over the region, offering, accepting
 * the driver's setup and serving its queues, until it has nothing to do
 * twice. The renderer and display operations are stubs that read every byte
 * of each buffer the device hands them and write every byte it lets them, so
 * that a range the device let through unchecked is an access out of bounds;
 * each fence is retired after the step that made it. The seed `run` is the
 * frames bed/shm/corpus holds, in order: the loopback run of bed/shm/peer.c.
 *
 * fuzz-deps: bed/vm/backend/device.c
 * fuzz-cflags: -I rach2/sys/virtio -I bed/vm/backend
 */
#include <string.h>

#include "../../bed/vm/backend/vgpudev.h"
#include "../fuzz/fuzz.h"

/* A frame: the smallest region, whose layout the device checks. */
#define FRAME  VGS_REGION_MIN
/* Frames of one input. */
#define FRAMES 32
/* Steps of the device after one frame at most. */
#define STEPS  24
/* Fences of one step that the harness retires after it. */
#define FENCES VD_PENDING

/* A fence the device asked for, retired after the step. */
struct fence {
    uint32_t ctx, per_ctx, ring;
    uint64_t id;
};

static struct fence fences[FENCES];
static unsigned int nfences;
static volatile uint8_t sink;

/** Read every byte of n buffers. */
static void touch(const struct iovec *iov, uint32_t n)
{
    uint32_t i;
    size_t j;
    uint8_t x = 0;

    for (i = 0; i < n; i++) {
        for (j = 0; j < iov[i].iov_len; j++) {
            x ^= ((const uint8_t *)iov[i].iov_base)[j];
        }
    }
    sink ^= x;
}

/** Stub of create: refuse format 0, which no renderer has, so that the
 * device's path for a renderer's refusal runs. */
static int op_create(void *c, const struct vd_create *a)
{
    (void)c;
    return a->format != 0 ? 0 : VD_RESP_ERR_PARAMETER;
}

/** Stub of unref and detach. */
static void op_id(void *c, uint32_t id)
{
    (void)c;
    (void)id;
}

/** Stub of attach: read the backing. */
static int op_attach(void *c, uint32_t id, struct iovec *iov, uint32_t n)
{
    (void)c;
    (void)id;
    touch(iov, n);
    return 0;
}

/** Stub of transfer. */
static int op_transfer(void *c, const struct vd_transfer *t)
{
    (void)c;
    sink ^= (uint8_t)(t->id ^ t->box.w);
    return 0;
}

/** Stub of blob: read the guest memory, if any. */
static int op_blob(void *c, const struct vd_blob *a, struct iovec *iov, uint32_t n)
{
    (void)c;
    (void)a;
    touch(iov, n);
    return 0;
}

/** Stub of ctx_create: read the name. */
static int op_ctx_create(void *c, uint32_t ctx, uint32_t capset, uint32_t nlen, const char *name)
{
    struct iovec v;

    (void)c;
    (void)ctx;
    (void)capset;
    v.iov_base = (void *)name;
    v.iov_len = nlen;
    touch(&v, 1);
    return 0;
}

/** Stub of ctx_destroy. */
static void op_ctx_destroy(void *c, uint32_t ctx)
{
    (void)c;
    (void)ctx;
}

/** Stub of ctx_resource. */
static void op_ctx_resource(void *c, uint32_t ctx, uint32_t id, int attach)
{
    (void)c;
    (void)ctx;
    (void)id;
    (void)attach;
}

/** Stub of submit: read the stream. */
static int op_submit(void *c, uint32_t ctx, void *buf, uint32_t bytes)
{
    struct iovec v;

    (void)c;
    (void)ctx;
    v.iov_base = buf;
    v.iov_len = bytes;
    touch(&v, 1);
    return 0;
}

/** Stub of fence: keep it for the harness to retire. */
static int op_fence(void *c, uint32_t ctx, int per_ctx, uint32_t ring, uint64_t id)
{
    (void)c;
    if (nfences == FENCES) {
        return VD_RESP_ERR_OUT_OF_MEMORY;
    }
    fences[nfences].ctx = ctx;
    fences[nfences].per_ctx = (uint32_t)per_ctx;
    fences[nfences].ring = ring;
    fences[nfences].id = id;
    nfences++;
    return 0;
}

/** Stub of capset_info: two capsets, the second as large as a response
 * holds. */
static void op_capset_info(void *c, uint32_t index, uint32_t *id, uint32_t *max_ver,
                           uint32_t *max_size)
{
    (void)c;
    *id = index + 1;
    *max_ver = 2;
    *max_size = index ? VD_RESP_MAX : 64;
}

/** Stub of capset: write every byte. */
static int op_capset(void *c, uint32_t id, uint32_t ver, void *out, uint32_t size)
{
    (void)c;
    memset(out, (int)(id ^ ver), size);
    return 0;
}

/** Stub of display. */
static int op_display(void *c, uint32_t scanout, struct vd_mode *m)
{
    (void)c;
    if (scanout != 0) {
        return VD_RESP_ERR_SCANOUT_ID;
    }
    m->width = 1024;
    m->height = 768;
    m->enabled = 1;
    return 0;
}

/** Stub of edid: the largest EDID the device takes. */
static int op_edid(void *c, uint32_t scanout, uint8_t *out, uint32_t *size)
{
    (void)c;
    memset(out, (int)scanout, VD_EDID_MAX);
    *size = VD_EDID_MAX;
    return 0;
}

/** Stub of scanout and flush; r is 0 when a scanout is taken off. */
static int op_rect(void *c, uint32_t scanout, uint32_t id, const struct vd_rect *r)
{
    (void)c;
    sink ^= (uint8_t)(scanout ^ id ^ (r ? r->width : 0));
    return 0;
}

/** Stub of reset: forget the fences. */
static void op_reset(void *c)
{
    (void)c;
    nfences = 0;
}

static const struct vd_ops ops = {
    op_create,     op_id,          op_attach,       op_id,     op_transfer, op_blob,
    op_ctx_create, op_ctx_destroy, op_ctx_resource, op_submit, op_fence,    op_capset_info,
    op_capset,     op_display,     op_edid,         op_rect,   op_rect,     op_reset,
};

/** Stub of the doorbell. */
static void ring(void *cookie, uint32_t peer, uint32_t vector)
{
    (void)cookie;
    sink ^= (uint8_t)(peer ^ vector);
}

/** Step the device until it has nothing to do twice or the driver has
 * gone, retiring each step's fences; return 1 when the driver has gone. */
static int steps(struct vd *dev)
{
    unsigned int i, f, idle = 0;
    int r;

    for (i = 0; i < STEPS && idle < 2; i++) {
        r = vd_step(dev);
        for (f = 0; f < nfences; f++) {
            vd_fence_retired(dev, fences[f].ctx, (int)fences[f].per_ctx, fences[f].ring,
                             fences[f].id);
        }
        nfences = 0;
        if (r == VD_GONE) {
            return 1;
        }
        idle = r == VD_IDLE ? idle + 1 : 0;
    }
    return 0;
}

/**
 * Lay the input's frames over one region in turn, the device's block kept,
 * and step the device after each.
 */
int LLVMFuzzerTestOneInput(const unsigned char *data, unsigned long size)
{
    static struct vd dev;
    static uint8_t region[FRAME];
    uint8_t mine[sizeof(struct vgs_device)];
    struct vd_offer off;
    unsigned long at, n;

    if (size == 0 || size > (unsigned long)FRAME * FRAMES) {
        return 0;
    }
    memset(&off, 0, sizeof(off));
    off.limits.max_resources = 512;
    off.limits.max_contexts = 64;
    off.limits.max_backing = FRAME;
    off.limits.max_entries = 1024;
    off.limits.max_chain = VGS_CHAIN_MAX;
    off.limits.max_cmd = 1u << 20;
    off.limits.max_queue = 256;
    off.features = (1ULL << VD_F_VIRGL) | (1ULL << VD_F_EDID) | (1ULL << VD_F_BLOB) |
                   (1ULL << VD_F_CONTEXT_INIT);
    off.transport_features = VGS_TF_DOORBELL;
    off.peer = 1;
    off.num_scanouts = 1;
    off.num_capsets = 2;
    nfences = 0;
    memset(region, 0, sizeof(region));
    if (vd_init(&dev, region, FRAME, &off, &ops, 0) != 0) {
        return 0;
    }
    dev.ring = ring;
    for (at = 0; at < size; at += FRAME) {
        n = size - at < FRAME ? size - at : FRAME;
        memcpy(mine, region + VGS_DEVICE_OFF, sizeof(mine));
        memcpy(region, data + at, n);
        memset(region + n, 0, FRAME - n);
        if (at != 0) {
            memcpy(region + VGS_DEVICE_OFF, mine, sizeof(mine));
        }
        if (steps(&dev)) {
            break;
        }
    }
    vd_fini(&dev);
    return 0;
}
