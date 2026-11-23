/*
 * vgpubackend_hosttest.c: the device side of the shared-memory GPU transport,
 * bed/vm/backend/device.c, as the VM's backend serves it, against a driver
 * of the test's own that lays out a region, takes the offer and puts
 * chains on the control queue, and a renderer whose fences retire only
 * when a row says.
 *
 * host-cflags: -iquote rach2/sys/virtio -iquote bed/vm/backend
 *
 * The rows: a fenced command the device has no memory to copy is refused
 * with ERR_OUT_OF_MEMORY only once the renderer has retired the fences
 * before it on its timeline, the global one and a context's ring, so that
 * the answer, which retires them in the driver, comes after their work; a
 * stream sent in parts (VGS_TF_PARTS) reaches the renderer whole in the
 * SUBMIT_3D that follows them, whose fenced answer waits for its fence; a
 * stream the VM has no memory for, a part out of order, a part of another
 * context and a joined SUBMIT_3D with no stream are refused, the joined
 * SUBMIT_3D behind its fence; a device whose driver did not accept the
 * parts refuses a part and a joined SUBMIT_3D; a command whose fence the
 * renderer refuses to make stays held behind the work before it, until
 * the renderer takes its fence when asked again or a later fence on its
 * timeline retires; and CTX_DESTROY frees the stream its context was
 * sending, the context's later parts allocate nothing, and the commands
 * held on its rings are answered with their fences.
 *
 * Returns 0 when every row passes, 1 otherwise.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* An allocation of the device above this many bytes fails, as in a VM
 * with no memory for it; 0 lets every one through. */
static size_t refuse_over;

/** realloc for the device, refused above `refuse_over`. */
static void *dev_realloc(void *p, size_t n)
{
    return refuse_over != 0 && n > refuse_over ? NULL : realloc(p, n);
}

/** malloc for the device, refused above `refuse_over`. */
static void *dev_malloc(size_t n)
{
    return refuse_over != 0 && n > refuse_over ? NULL : malloc(n);
}

/* device.c sets the feature level the headers above have set already. */
#undef _POSIX_C_SOURCE
#define realloc dev_realloc
#define malloc  dev_malloc
#include "../../bed/vm/backend/device.c"
#undef realloc
#undef malloc

#define REGION   (8u << 20)
#define QSIZE    64u
#define CTX      7u
#define FLAGS_FR (VD_FLAG_FENCE | VD_FLAG_INFO_RING_IDX)

static int failures;

/** Count a failed check and name it. */
#define CHECK(cond, what)                                                     \
    do {                                                                      \
        if (!(cond)) {                                                        \
            failures++;                                                       \
            printf("vgpubackend-test: fail: %s (line %d)\n", what, __LINE__); \
        }                                                                     \
    } while (0)

/* The renderer: the fences it was asked for, in order, and what the last
 * submission gave it. */
static struct {
    uint32_t ctx, per_ctx, ring;
    uint64_t id;
} asked[64];
static unsigned nasked;
static uint32_t submits, submitted, submitted_sum;
/* The renderer refuses to make a fence while `fence_fails`, and fence
 * `fail_id` always, counting the refusals; a refused fence is not in
 * `asked`. */
static int fence_fails;
static uint64_t fail_id;
static unsigned refused;

/* The driver: the region, its layout and block, the queue's indices, the
 * heap's next free byte, the chains in flight by head and their answers. */
static uint8_t *base;
static struct vd dev;
static struct vgs_layout lay;
static struct vgs_driver me;
static uint16_t avail_idx, used_idx;
static uint64_t brk;
static uint64_t answer_at[QSIZE];
static uint32_t answered[QSIZE]; /* the answer's type, 0 while none */
static struct vd_hdr answer_hdr[QSIZE];
static uint32_t next_head;

/** FNV-1a of `n` bytes at `p`. */
static uint32_t fnv1a(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u;

    while (n--) {
        h = (h ^ *p++) * 16777619u;
    }
    return h;
}

/** Stub of create: every resource is made. */
static int op_zero(void *c, const struct vd_create *a)
{
    (void)c;
    (void)a;
    return 0;
}

/** Stub of unref, detach and unpin. */
static void op_id(void *c, uint32_t id)
{
    (void)c;
    (void)id;
}

/** Stub of attach. */
static int op_attach(void *c, uint32_t id, struct iovec *iov, uint32_t n)
{
    (void)c;
    (void)id;
    (void)iov;
    (void)n;
    return 0;
}

/** Stub of transfer. */
static int op_transfer(void *c, const struct vd_transfer *t)
{
    (void)c;
    (void)t;
    return 0;
}

/** Stub of blob. */
static int op_blob(void *c, const struct vd_blob *a, struct iovec *iov, uint32_t n)
{
    (void)c;
    (void)a;
    (void)iov;
    (void)n;
    return 0;
}

/** Stub of ctx_create: every context is made. */
static int op_ctx_create(void *c, uint32_t ctx, uint32_t capset, uint32_t nlen, const char *name)
{
    (void)c;
    (void)ctx;
    (void)capset;
    (void)nlen;
    (void)name;
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

/** The renderer takes a submission: its bytes and their sum. */
static int op_submit(void *c, uint32_t ctx, void *buf, uint32_t bytes)
{
    (void)c;
    (void)ctx;
    submits++;
    submitted = bytes;
    submitted_sum = fnv1a(buf, bytes);
    return 0;
}

/** The renderer is asked for a fence, which retires when a row says. */
static int op_fence(void *c, uint32_t ctx, int per_ctx, uint32_t ring, uint64_t id)
{
    (void)c;
    if (fence_fails || (fail_id != 0 && id == fail_id)) {
        refused++;
        return 12;
    }
    if (nasked < sizeof(asked) / sizeof(asked[0])) {
        asked[nasked].ctx = ctx;
        asked[nasked].per_ctx = (uint32_t)per_ctx;
        asked[nasked].ring = ring;
        asked[nasked].id = id;
        nasked++;
    }
    return 0;
}

/** Stub of capset_info: one capset, virgl's. */
static void op_capset_info(void *c, uint32_t index, uint32_t *id, uint32_t *max_ver,
                           uint32_t *max_size)
{
    (void)c;
    (void)index;
    *id = 1;
    *max_ver = 1;
    *max_size = 4;
}

/** Stub of capset: zeros. */
static int op_capset(void *c, uint32_t id, uint32_t ver, void *out, uint32_t size)
{
    (void)c;
    (void)id;
    (void)ver;
    memset(out, 0, size);
    return 0;
}

/** Stub of display: one enabled 640x480 display. */
static int op_display(void *c, uint32_t scanout, struct vd_mode *m)
{
    (void)c;
    (void)scanout;
    m->width = 640;
    m->height = 480;
    m->enabled = 1;
    return 0;
}

/** Stub of edid: none. */
static int op_edid(void *c, uint32_t scanout, uint8_t *out, uint32_t *size)
{
    (void)c;
    (void)scanout;
    (void)out;
    *size = 0;
    return VD_RESP_ERR_UNSPEC;
}

/** Stub of scanout and flush. */
static int op_rect(void *c, uint32_t scanout, uint32_t id, const struct vd_rect *r)
{
    (void)c;
    (void)scanout;
    (void)id;
    (void)r;
    return 0;
}

/** Stub of reset. */
static void op_reset(void *c)
{
    (void)c;
}

/** Stub of pages and back: refused, as no page list is asked. */
static int op_pages(void *c, struct vd_pages *q)
{
    (void)c;
    (void)q;
    return VD_RESP_ERR_UNSPEC;
}

static const struct vd_ops ops = {
    op_zero,        op_id,           op_attach, op_id,    op_transfer,    op_blob,   op_ctx_create,
    op_ctx_destroy, op_ctx_resource, op_submit, op_fence, op_capset_info, op_capset, op_display,
    op_edid,        op_rect,         op_rect,   op_reset, op_pages,       op_id,     op_pages,
};

/** Store the driver's block: fields first, then the epoch, then the state. */
static void drv_store(void)
{
    volatile struct vgs_driver *v = VGS_DRIVER(base);

    v->state = VGS_DRV_RESET;
    VGS_MB();
    memcpy((uint8_t *)base + VGS_DRIVER_OFF + 8, (const uint8_t *)&me + 8, sizeof(me) - 8);
    VGS_MB();
    v->epoch = me.epoch;
    VGS_MB();
    v->state = me.state;
    VGS_MB();
}

/** A fresh region and device, the device offering the parts, and an epoch
 * whose driver accepts them with `parts`; 0, or -1 when the handshake
 * fails. Every row starts here. */
static int start(int parts)
{
    struct vd_offer off;
    struct vgs_device d;
    uint64_t at;

    if (base) {
        vd_fini(&dev);
        free(base);
    }
    if (posix_memalign((void **)&base, VGS_PAGE, REGION) != 0) {
        base = 0;
        return -1;
    }
    if (vgs_layout_make(&lay, REGION, VGS_PAGE) != VGS_OK) {
        return -1;
    }
    memset(base, 0, REGION);
    memcpy(base + VGS_LAYOUT_OFF, &lay, sizeof(lay));
    memset(&off, 0, sizeof(off));
    off.limits.max_resources = 64;
    off.limits.max_contexts = 8;
    off.limits.max_backing = REGION;
    off.limits.max_entries = 1024;
    off.limits.max_chain = VGS_CHAIN_MAX;
    off.limits.max_cmd = REGION;
    off.limits.max_queue = QSIZE;
    off.features = 1ULL << VD_F_VIRGL;
    off.transport_features = VGS_TF_PARTS;
    off.peer = 1;
    off.num_scanouts = 1;
    off.num_capsets = 1;
    refuse_over = 0;
    if (vd_init(&dev, base, REGION, &off, &ops, 0) != 0) {
        return -1;
    }
    memset(&me, 0, sizeof(me));
    me.epoch = 1;
    me.state = VGS_DRV_RESET;
    drv_store();
    vd_step(&dev);
    if (vgs_snap_device(base, &d) != VGS_OK || vgs_check_offer(&d, 1, 0) != VGS_OK) {
        return -1;
    }
    me.features = d.features & (VGS_F_VERSION_1 | (1ULL << VD_F_VIRGL));
    me.transport_features = parts ? VGS_TF_PARTS : 0;
    at = vgs_queue_place(&me.queue[VGS_QUEUE_CONTROL], lay.ring_off, QSIZE);
    vgs_queue_place(&me.queue[VGS_QUEUE_CURSOR], at, 16);
    me.state = VGS_DRV_OK;
    drv_store();
    vd_step(&dev);
    if (vgs_snap_device(base, &d) != VGS_OK || d.state != VGS_DEV_RUNNING) {
        return -1;
    }
    avail_idx = used_idx = 0;
    brk = lay.heap_off;
    next_head = 0;
    nasked = refused = 0;
    fence_fails = 0;
    fail_id = 0;
    submits = submitted = submitted_sum = 0;
    memset(answered, 0, sizeof(answered));
    return 0;
}

/** `n` bytes of the heap, 64-aligned; the driver's heap is never freed
 * within a row. */
static uint64_t heap(uint64_t n)
{
    uint64_t a = (brk + 63) & ~63ULL;

    brk = a + n;
    return a;
}

/** Put a chain on the control queue: `req` of `rlen` bytes, then `data` of
 * `dlen` bytes in its own descriptor when given, and a 24-byte answer.
 * Returns the chain's head. */
static uint32_t put(const void *req, uint32_t rlen, const void *data, uint32_t dlen)
{
    const struct vgs_queue *q = &me.queue[VGS_QUEUE_CONTROL];
    struct vgs_desc ds[3];
    uint32_t head = next_head, n = 0, i;

    next_head = (next_head + 3) % (QSIZE - QSIZE % 3);
    ds[n].addr = heap(rlen);
    ds[n].len = rlen;
    memcpy(base + ds[n].addr, req, rlen);
    n++;
    if (dlen != 0) {
        ds[n].addr = heap(dlen);
        ds[n].len = dlen;
        memcpy(base + ds[n].addr, data, dlen);
        n++;
    }
    ds[n].addr = heap(sizeof(struct vd_hdr));
    ds[n].len = sizeof(struct vd_hdr);
    answer_at[head] = ds[n].addr;
    answered[head] = 0;
    n++;
    for (i = 0; i < n; i++) {
        ds[i].flags =
            (uint16_t)((i + 1 < n ? VGS_DESC_F_NEXT : 0) | (i + 1 == n ? VGS_DESC_F_WRITE : 0));
        ds[i].next = (uint16_t)(head + i + 1);
        vgs_desc_write(base, q, head + i, &ds[i]);
    }
    vgs_avail_push(base, q, &avail_idx, head);
    return head;
}

/** Let the device serve, then take the answers it has published. */
static void serve_all(void)
{
    const struct vgs_queue *q = &me.queue[VGS_QUEUE_CONTROL];
    uint16_t idx;
    uint32_t id, len;

    vd_step(&dev);
    if (vgs_used_read(base, q, used_idx, &idx) != VGS_OK) {
        CHECK(0, "the used ring reads");
        return;
    }
    while (used_idx != idx) {
        if (vgs_used_elem(base, q, used_idx, &id, &len) == VGS_OK && id < QSIZE &&
            len == sizeof(struct vd_hdr)) {
            memcpy(&answer_hdr[id], base + answer_at[id], sizeof(struct vd_hdr));
            answered[id] = answer_hdr[id].type;
        } else {
            CHECK(0, "each answer is a bare header of a chain in flight");
        }
        used_idx++;
    }
}

/** A command header. */
static struct vd_hdr hdr_of(uint32_t type, uint32_t flags, uint64_t fence, uint32_t ctx,
                            uint8_t ring)
{
    struct vd_hdr h;

    memset(&h, 0, sizeof(h));
    h.type = type;
    h.flags = flags;
    h.fence_id = fence;
    h.ctx_id = ctx;
    h.ring_idx = ring;
    return h;
}

/** CTX_CREATE of context `ctx`, answered OK at once. */
static void make_context_id(uint32_t ctx)
{
    uint8_t req[sizeof(struct vd_hdr) + sizeof(struct c_ctx_create)];
    struct vd_hdr h = hdr_of(CMD_CTX_CREATE, 0, 0, ctx, 0);
    uint32_t head;

    memset(req, 0, sizeof(req));
    memcpy(req, &h, sizeof(h));
    head = put(req, sizeof(req), 0, 0);
    serve_all();
    CHECK(answered[head] == VD_RESP_OK_NODATA, "the context is made");
}

/** CTX_CREATE of context CTX, answered OK at once. */
static void make_context(void)
{
    make_context_id(CTX);
}

/** CTX_DESTROY of context `ctx`, answered OK at once. */
static void destroy_context(uint32_t ctx)
{
    struct vd_hdr h = hdr_of(CMD_CTX_DESTROY, 0, 0, ctx, 0);
    uint32_t head = put(&h, sizeof(h), 0, 0);

    serve_all();
    CHECK(answered[head] == VD_RESP_OK_NODATA, "the context is destroyed");
}

/** A SUBMIT_3D of `size` bytes of `data`, or with `joined` of the stream
 * its parts carried and no bytes of its own. Returns its head. */
static uint32_t submit(uint32_t flags, uint64_t fence, uint8_t ring, const uint8_t *data,
                       uint32_t size, int joined)
{
    uint8_t req[sizeof(struct vd_hdr) + sizeof(struct c_submit)];
    struct vd_hdr h =
        hdr_of(CMD_SUBMIT_3D, flags | (joined ? VGS_FLAG_JOINED : 0), fence, CTX, ring);
    struct c_submit c;

    c.size = size;
    c.padding = 0;
    memcpy(req, &h, sizeof(h));
    memcpy(req + sizeof(h), &c, sizeof(c));
    return put(req, sizeof(req), joined ? 0 : data, joined ? 0 : size);
}

/** A VGS_CMD_PART of context `ctx` carrying `n` bytes of `data` at
 * `offset` of a stream of `total`. Returns its head. */
static uint32_t part(uint32_t ctx, uint64_t offset, uint64_t total, const uint8_t *data, uint32_t n)
{
    uint8_t req[sizeof(struct vd_hdr) + sizeof(struct vgs_part)];
    struct vd_hdr h = hdr_of(VGS_CMD_PART, 0, 0, ctx, 0);
    struct vgs_part p;

    p.offset = offset;
    p.total = total;
    memcpy(req, &h, sizeof(h));
    memcpy(req + sizeof(h), &p, sizeof(p));
    return put(req, sizeof(req), data, n);
}

/** Whether chain `head` was answered `type` with its fence, `id`. */
static int fenced_answer(uint32_t head, uint32_t type, uint64_t id)
{
    return answered[head] == type && (answer_hdr[head].flags & VD_FLAG_FENCE) &&
           answer_hdr[head].fence_id == id && !(answer_hdr[head].flags & VGS_FLAG_JOINED);
}

/** A fenced command the device cannot copy, behind a fenced submission
 * whose work is pending, on the global timeline (`ring` 0 and no ring
 * flag) or on a context's ring: its refusal comes once its own fence
 * retires, after the one before it, and never with it. */
static void refusal_behind(uint32_t flags, uint8_t ring, const uint8_t *data)
{
    uint32_t a, b;
    int per_ctx = (flags & VD_FLAG_INFO_RING_IDX) != 0;

    a = submit(flags, 1, ring, data, 4096, 0);
    serve_all();
    CHECK(submits == 1 && answered[a] == 0 && nasked == 1 && asked[0].id == 1,
          "a fenced submission waits for its fence");
    refuse_over = 1u << 20;
    b = submit(flags, 2, ring, data, 2u << 20, 0);
    serve_all();
    refuse_over = 0;
    CHECK(submits == 1, "a chain with no memory for its copy reaches no renderer");
    CHECK(answered[a] == 0 && answered[b] == 0,
          "a refusal waits behind the work of the fences before it");
    CHECK(nasked == 2 && asked[1].id == 2 && asked[1].per_ctx == (uint32_t)per_ctx &&
              asked[1].ctx == (per_ctx ? CTX : 0) && asked[1].ring == (per_ctx ? ring : 0),
          "a fenced refusal is fenced on its timeline");
    vd_fence_retired(&dev, per_ctx ? CTX : 0, per_ctx, per_ctx ? ring : 0, 1);
    serve_all();
    CHECK(fenced_answer(a, VD_RESP_OK_NODATA, 1) && answered[b] == 0,
          "the first fence answers its own command alone");
    vd_fence_retired(&dev, per_ctx ? CTX : 0, per_ctx, per_ctx ? ring : 0, 2);
    serve_all();
    CHECK(fenced_answer(b, VD_RESP_ERR_OUT_OF_MEMORY, 2),
          "the refusal comes with its own fence once that retires");
}

/** The refusal of a chain with no memory for its copy, behind pending work
 * on the global timeline and on a context's ring. */
static void row_refusal(const uint8_t *data)
{
    CHECK(start(0) == 0, "the device runs");
    make_context();
    refusal_behind(VD_FLAG_FENCE, 0, data);
    CHECK(start(0) == 0, "the device runs again");
    make_context();
    refusal_behind(FLAGS_FR, 3, data);
}

/** A stream of `total` bytes of `data` sent in parts of `each` bytes, then
 * its fenced SUBMIT_3D: each part answered OK at once, unfenced, and the
 * stream reaching the renderer whole, answered at its fence. */
static void row_parts(const uint8_t *data, uint32_t total, uint32_t each)
{
    uint32_t off, here, head, ok = 1, before;

    CHECK(start(1) == 0, "the device runs with parts");
    make_context();
    for (off = 0; off < total; off += here) {
        here = total - off < each ? total - off : each;
        head = part(CTX, off, total, data + off, here);
        serve_all();
        ok &= answered[head] == VD_RESP_OK_NODATA && !(answer_hdr[head].flags & VD_FLAG_FENCE);
    }
    CHECK(ok && submits == 0, "each part is answered OK at once, unfenced");
    before = nasked;
    head = submit(VD_FLAG_FENCE, 5, 0, 0, total, 1);
    serve_all();
    CHECK(submits == 1 && submitted == total && submitted_sum == fnv1a(data, total),
          "the joined SUBMIT_3D gives the renderer the whole stream");
    CHECK(answered[head] == 0 && nasked == before + 1, "its answer waits for its fence");
    vd_fence_retired(&dev, 0, 0, 0, 5);
    serve_all();
    CHECK(fenced_answer(head, VD_RESP_OK_NODATA, 5), "and comes at it");
}

/** Streams the device refuses: one the VM has no memory for, whose fenced
 * SUBMIT_3D is refused behind pending work; a part out of order; a part of
 * another context; a joined SUBMIT_3D with no stream; and parts and joins
 * on a device whose driver did not accept them. */
static void row_broken(const uint8_t *data)
{
    uint32_t a, p, s;

    CHECK(start(1) == 0, "the device runs with parts");
    make_context();
    a = submit(VD_FLAG_FENCE, 1, 0, data, 4096, 0);
    refuse_over = 1u << 20;
    p = part(CTX, 0, 2u << 20, data, 4096);
    serve_all();
    refuse_over = 0;
    CHECK(answered[p] == VD_RESP_ERR_OUT_OF_MEMORY, "a stream with no memory refuses its part");
    p = part(CTX, 4096, 2u << 20, data, 4096);
    s = submit(VD_FLAG_FENCE, 2, 0, 0, 2u << 20, 1);
    serve_all();
    CHECK(answered[p] == VD_RESP_ERR_OUT_OF_MEMORY && submits == 1,
          "the rest of a stream with no memory is refused");
    CHECK(answered[a] == 0 && answered[s] == 0,
          "the joined refusal waits behind the work before it");
    vd_fence_retired(&dev, 0, 0, 0, 1);
    serve_all();
    CHECK(fenced_answer(a, VD_RESP_OK_NODATA, 1) && answered[s] == 0,
          "the earlier fence answers its own command alone");
    vd_fence_retired(&dev, 0, 0, 0, 2);
    serve_all();
    CHECK(fenced_answer(s, VD_RESP_ERR_OUT_OF_MEMORY, 2), "the joined refusal comes at its fence");

    part(CTX, 0, 8192, data, 4096);
    p = part(CTX, 6144, 8192, data, 2048);
    s = submit(0, 0, 0, 0, 8192, 1);
    serve_all();
    CHECK(answered[p] == VD_RESP_ERR_PARAMETER && answered[s] == VD_RESP_ERR_PARAMETER,
          "a part out of order breaks its stream");
    make_context_id(CTX + 1);
    part(CTX, 0, 8192, data, 4096);
    p = part(CTX + 1, 4096, 8192, data, 4096);
    s = submit(0, 0, 0, 0, 8192, 1);
    serve_all();
    CHECK(answered[p] == VD_RESP_ERR_PARAMETER && answered[s] == VD_RESP_ERR_PARAMETER,
          "a part of another context breaks the stream");
    part(CTX, 0, 8192, data, 4096);
    s = submit(0, 0, 0, 0, 8192, 1);
    serve_all();
    CHECK(answered[s] == VD_RESP_ERR_PARAMETER, "a stream short of its total is refused");
    s = submit(0, 0, 0, 0, 8192, 1);
    serve_all();
    CHECK(answered[s] == VD_RESP_ERR_PARAMETER && submits == 1,
          "a joined SUBMIT_3D with no stream is refused");

    CHECK(start(0) == 0, "the device runs without parts");
    make_context();
    p = part(CTX, 0, 4096, data, 4096);
    s = submit(0, 0, 0, 0, 4096, 1);
    serve_all();
    CHECK(answered[p] == VD_RESP_ERR_UNSPEC && answered[s] == VD_RESP_ERR_PARAMETER && submits == 0,
          "without the parts accepted a part and a join are refused");
}

/** A fenced submission whose fence the renderer refuses to make, behind a
 * fenced one whose work is pending, on the global timeline (`ring` 0 and
 * no ring flag) or a context's ring: it stays held after the earlier fence
 * retires, is asked again while it is the newest held, and is answered at
 * its fence once the renderer takes it; then, with the renderer refusing
 * still, one held behind a later fence is not asked again and retires with
 * that fence. */
static void fence_refused(uint32_t flags, uint8_t ring, const uint8_t *data)
{
    int per_ctx = (flags & VD_FLAG_INFO_RING_IDX) != 0;
    uint32_t tctx = per_ctx ? CTX : 0, tring = per_ctx ? ring : 0;
    uint32_t a, b, c, e;
    unsigned before;

    a = submit(flags, 1, ring, data, 4096, 0);
    serve_all();
    fence_fails = 1;
    b = submit(flags, 2, ring, data, 4096, 0);
    serve_all();
    CHECK(submits == 2 && refused >= 1 && nasked == 1, "the renderer refuses the second fence");
    CHECK(answered[a] == 0 && answered[b] == 0,
          "a refused fence answers nothing while the work before it runs");
    CHECK(dev.npend == 2 && dev.nunfenced == 1, "the command of a refused fence stays held");
    vd_fence_retired(&dev, tctx, per_ctx, tring, 1);
    serve_all();
    CHECK(fenced_answer(a, VD_RESP_OK_NODATA, 1) && answered[b] == 0,
          "the earlier fence answers its own command alone");
    before = refused;
    serve_all();
    CHECK(refused > before, "the newest command of a refused fence asks again");
    fence_fails = 0;
    serve_all();
    CHECK(nasked == 2 && asked[1].id == 2 && asked[1].per_ctx == (uint32_t)per_ctx &&
              asked[1].ctx == tctx && asked[1].ring == tring && dev.nunfenced == 0,
          "the renderer takes the fence when asked again");
    CHECK(answered[b] == 0, "a fence taken again answers nothing before it retires");
    vd_fence_retired(&dev, tctx, per_ctx, tring, 2);
    serve_all();
    CHECK(fenced_answer(b, VD_RESP_OK_NODATA, 2), "the command comes at its fence");

    fail_id = 3;
    c = submit(flags, 3, ring, data, 4096, 0);
    serve_all();
    e = submit(flags, 4, ring, data, 4096, 0);
    serve_all();
    before = refused;
    serve_all();
    serve_all();
    CHECK(refused == before && nasked == 3 && asked[2].id == 4 && answered[c] == 0 &&
              answered[e] == 0 && dev.nunfenced == 1,
          "a command held behind a later fence is not asked again");
    vd_fence_retired(&dev, tctx, per_ctx, tring, 4);
    serve_all();
    CHECK(fenced_answer(c, VD_RESP_OK_NODATA, 3) && fenced_answer(e, VD_RESP_OK_NODATA, 4) &&
              dev.npend == 0 && dev.nunfenced == 0,
          "a later fence retires the command whose fence was refused");
}

/** A refused fence on the global timeline and on a context's ring. */
static void row_fence_refused(const uint8_t *data)
{
    CHECK(start(0) == 0, "the device runs");
    make_context();
    fence_refused(VD_FLAG_FENCE, 0, data);
    CHECK(start(0) == 0, "the device runs again");
    make_context();
    fence_refused(FLAGS_FR, 2, data);
}

/** CTX_DESTROY frees the stream its context was sending in parts and keeps
 * another context's; a part of the dead context is refused and allocates
 * nothing. */
static void row_destroy(const uint8_t *data)
{
    uint32_t p, s;

    CHECK(start(1) == 0, "the device runs with parts");
    make_context();
    make_context_id(CTX + 1);
    p = part(CTX, 0, 2u << 20, data, 4096);
    serve_all();
    CHECK(answered[p] == VD_RESP_OK_NODATA && dev.join != 0, "a stream begun holds its buffer");
    destroy_context(CTX);
    CHECK(dev.join == 0 && !dev.join_open, "CTX_DESTROY frees its context's stream");
    p = part(CTX, 0, 2u << 20, data, 4096);
    serve_all();
    CHECK(answered[p] == VD_RESP_ERR_CONTEXT_ID && dev.join == 0,
          "a part of a dead context is refused and allocates nothing");
    p = part(CTX + 1, 0, 8192, data, 4096);
    serve_all();
    make_context();
    destroy_context(CTX);
    CHECK(answered[p] == VD_RESP_OK_NODATA && dev.join != 0 && dev.join_ctx == CTX + 1,
          "CTX_DESTROY keeps another context's stream");
    p = part(CTX + 1, 4096, 8192, data + 4096, 4096);
    serve_all();
    {
        uint8_t req[sizeof(struct vd_hdr) + sizeof(struct c_submit)];
        struct vd_hdr h = hdr_of(CMD_SUBMIT_3D, VGS_FLAG_JOINED, 0, CTX + 1, 0);
        struct c_submit c;

        c.size = 8192;
        c.padding = 0;
        memcpy(req, &h, sizeof(h));
        memcpy(req + sizeof(h), &c, sizeof(c));
        s = put(req, sizeof(req), 0, 0);
    }
    serve_all();
    CHECK(answered[p] == VD_RESP_OK_NODATA && answered[s] == VD_RESP_OK_NODATA && submits == 1 &&
              submitted_sum == fnv1a(data, 8192),
          "the other context's stream reaches the renderer whole");
}

/** CTX_DESTROY answers each command held on the context's rings, its
 * fence taken or refused, with that fence, and asks the renderer for no
 * fence of the dead context; a command held on the global timeline stays
 * held until its own fence retires. */
static void row_destroy_held(const uint8_t *data)
{
    uint32_t a, b, g;
    unsigned before;

    CHECK(start(0) == 0, "the device runs");
    make_context();
    a = submit(FLAGS_FR, 1, 2, data, 4096, 0);
    g = submit(VD_FLAG_FENCE, 9, 0, data, 4096, 0);
    serve_all();
    fence_fails = 1;
    b = submit(FLAGS_FR, 2, 2, data, 4096, 0);
    serve_all();
    CHECK(answered[a] == 0 && answered[b] == 0 && answered[g] == 0 && dev.npend == 3 &&
              dev.nunfenced == 1 && nasked == 2,
          "a taken ring fence, a refused one and a global one are held");
    destroy_context(CTX);
    CHECK(fenced_answer(a, VD_RESP_OK_NODATA, 1) && fenced_answer(b, VD_RESP_OK_NODATA, 2),
          "CTX_DESTROY answers its rings' held commands with their fences");
    CHECK(answered[g] == 0 && dev.npend == 1 && dev.nunfenced == 0,
          "CTX_DESTROY keeps a command held on the global timeline");
    before = refused;
    serve_all();
    serve_all();
    fence_fails = 0;
    serve_all();
    CHECK(refused == before && nasked == 2, "no fence of a dead context is asked for");
    vd_fence_retired(&dev, 0, 0, 0, 9);
    serve_all();
    CHECK(fenced_answer(g, VD_RESP_OK_NODATA, 9) && dev.npend == 0,
          "the global command comes at its own fence");
}

/** Run the rows over the real device. */
int main(void)
{
    uint8_t *data = malloc(4u << 20);
    uint32_t i;

    if (!data) {
        printf("vgpubackend-test: FAIL: no memory for the stream\n");
        return 1;
    }
    for (i = 0; i < (4u << 20); i++) {
        data[i] = (uint8_t)(i * 2654435761u >> 24);
    }
    row_refusal(data);
    row_parts(data, 8192, 4096);
    row_parts(data, (3u << 20) + 4, 896u << 10);
    row_broken(data);
    row_fence_refused(data);
    row_destroy(data);
    row_destroy_held(data);
    if (base) {
        vd_fini(&dev);
        free(base);
    }
    free(data);
    printf("vgpubackend-test: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
