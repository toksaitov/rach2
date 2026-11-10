/*
 * i915_exec_hosttest.c: the Intel driver's EXECBUFFER2,
 * i915_gem_execbuffer.c, with its requests, contexts, syncobjs and sync
 * files, on the chip of i915_gem_model.c, whose rings run what the driver
 * gives them as each tail moves: the requests' flushes, switches and
 * breadcrumbs, and the batches they start, of which it runs
 * MI_STORE_DATA_IMM, so that a row sees where a relocated address points.
 * The user interrupt is taken when a row asks for it.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/spinlock_host.c
 *
 * The rows, one for each of 78-5b's subtasks: the call's checks and
 * Linux's refusals; relocations as Linux's eb_relocate_entry applies them,
 * with presumed offsets, I915_EXEC_NO_RELOC, I915_EXEC_HANDLE_LUT, signed
 * deltas and I915_EXEC_BATCH_FIRST, and objects placed at a pinned offset,
 * with a pad and an alignment; the batch started on the render and the
 * blitter ring, the stream output reset, and the request's fence; the
 * in-fence, the submit fence, the out-fence and the fence array; the
 * command parser's shadow and trampoline on the call's path (78-8b), and
 * the objects held until the request retires; a ring that refuses the
 * commands.
 *
 * Returns 0 when every row passes, 1 otherwise.
 */

#define MODEL_TEST "i915-exec-test"
#define MODEL_HAS_REQUESTS
#define MODEL_HAS_SYNC_FILE
#include "i915_gem_model.c"
#include "drm/drm_syncobj.c"
#include "dmabuf/sync_file.c"
#include "drm/i915/i915_gem_context.c"
#include "drm/i915/i915_getparam.c"
#include "drm/i915/i915_request.c"
#include "drm/i915/i915_ring.c"
#include "drm/i915/i915_ppgtt.c"
#include "drm/i915/i915_gem_execbuffer.c"
#include "drm/i915/i915_cmd_parser.c"

/* The batch's commands the chip runs: a store to a GTT address. */
#define MI_STORE_DATA_IMM_GTT (MI_INSTR(0x20, 2) | (1u << 22))

/* ---- the kernel around the driver ---- */

size_t frame_total_count(void)
{
    return 1u << 20;
}

struct eventfd *eventfd_get(int fd, int32_t *err)
{
    (void)fd;
    *err = -EBADF;
    return 0;
}

void eventfd_put(struct eventfd *ev)
{
    (void)ev;
}

void eventfd_signal(struct eventfd *ev)
{
    (void)ev;
}

void *kalloc_pages(uint32_t count, uintptr_t *phys)
{
    void *p = 0;

    if (posix_memalign(&p, 4096, (unsigned long)count * 4096u) != 0) {
        return 0;
    }
    allocations++;
    if (phys) {
        *phys = (uintptr_t)p;
    }
    return p;
}

void kfree_pages(void *p, uint32_t count)
{
    (void)count;
    kfree(p);
}

uintptr_t paging_virt_to_phys(uintptr_t va)
{
    return va;
}

static int irq_depth;

uint32_t kirq_save(void)
{
    irq_depth++;
    return 0;
}

void kirq_restore(uint32_t flags)
{
    (void)flags;
    irq_depth--;
}

/* ---- the chip's base, which the rings' code calls ---- */

/* The faults a row asks for, by I915_FAULT_ point: how many calls fail. */
static int fault_left[2];

int32_t i915_hw_fault(struct i915_hw *hw, int point)
{
    (void)hw;
    if (point >= 0 && point < 2 && fault_left[point] > 0) {
        fault_left[point]--;
        return point == I915_FAULT_SUBMIT ? -EIO : -ENOSPC;
    }
    return 0;
}

void i915_delay_us(struct i915_hw *hw, uint32_t us)
{
    (void)hw;
    now_ns += (int64_t)us * 1000;
}

int32_t i915_wait_for(struct i915_hw *hw, const char *what, uint32_t reg, uint32_t mask,
                      uint32_t value, uint32_t timeout_us)
{
    (void)hw;
    (void)what;
    (void)reg;
    (void)mask;
    (void)value;
    (void)timeout_us;
    return 0;
}

int32_t i915_wait_until(struct i915_hw *hw, const char *what, int (*done)(struct i915_hw *, void *),
                        void *arg, uint32_t timeout_us)
{
    (void)what;
    (void)timeout_us;
    return done(hw, arg) ? 0 : -ETIMEDOUT;
}

int32_t i915_forcewake_get(struct drm_i915_private *i915)
{
    (void)i915;
    return 0;
}

void i915_forcewake_put(struct drm_i915_private *i915)
{
    (void)i915;
}

static int parks, unparks;

void i915_gt_park(struct drm_i915_private *i915)
{
    (void)i915;
    parks++;
}

void i915_gt_unpark(struct drm_i915_private *i915)
{
    (void)i915;
    unparks++;
}

/* The user interrupt's handlers by bit, and the bits the rings raised. */
static i915_irq_fn irq_fn[32];
static void *irq_data[32];
static uint32_t irq_pending;

int32_t i915_irq_register(struct drm_i915_private *i915, int bank, uint32_t bits, i915_irq_fn fn,
                          void *data)
{
    int b;

    (void)i915;
    (void)bank;
    for (b = 0; b < 32; b++) {
        if (bits & (1u << b)) {
            irq_fn[b] = fn;
            irq_data[b] = data;
        }
    }
    return 0;
}

void i915_irq_unregister(struct drm_i915_private *i915, int bank, uint32_t bits)
{
    int b;

    (void)i915;
    (void)bank;
    for (b = 0; b < 32; b++) {
        if (bits & (1u << b)) {
            irq_fn[b] = 0;
        }
    }
}

void i915_irq_unmask(struct drm_i915_private *i915, int bank, uint32_t bits)
{
    (void)i915;
    (void)bank;
    (void)bits;
}

void i915_irq_mask(struct drm_i915_private *i915, int bank, uint32_t bits)
{
    (void)i915;
    (void)bank;
    (void)bits;
}

/* ---- the rings ---- */

/* A batch a ring started: the ring's engine, MI_BATCH_BUFFER_START's
 * first dword, and the address. */
struct started {
    int engine;
    uint32_t cmd;
    uint32_t addr;
};

static struct started starts[64];
static int nstarts;

/* The last MI_BATCH_BUFFER_START a batch chained to, and how many. */
static struct started chained;
static int nchained;

/* The engines whose ring holds still, as a hung GPU's would. */
static int ring_hung[I915_NUM_ENGINES];

/** Store `value` at the GTT address `addr`, as the chip's writes reach
 * memory. */
static void gtt_store(struct chip *c, uint32_t addr, uint32_t value)
{
    uint8_t *m = addr < GTT_BYTES ? chip_memory(c, addr) : 0;

    if (m) {
        k_memcpy(m, &value, 4);
    }
}

/** The dword at the GTT address `addr`, 0 where nothing is mapped. */
static uint32_t gtt_load(struct chip *c, uint32_t addr)
{
    uint8_t *m = addr < GTT_BYTES ? chip_memory(c, addr) : 0;
    uint32_t v = 0;

    if (m) {
        k_memcpy(&v, m, 4);
    }
    return v;
}

/** The dwords of the command whose first is `dw`, as the chip parses
 * them: MI commands below 0x10 are one, the others and the 2D and 3D
 * commands carry their length less two. */
static uint32_t command_dwords(uint32_t dw)
{
    if ((dw >> 29) == 0 && ((dw >> 23) & 0x3fu) < 0x10u) {
        return 1;
    }
    return (dw & 0xffu) + 2u;
}

/** Run a batch at the GTT address `addr` up to MI_BATCH_BUFFER_END, of
 * which the chip does MI_STORE_DATA_IMM, follows MI_BATCH_BUFFER_START to
 * the batch it chains to, four deep at most, and passes over the rest. */
static void batch_run(struct chip *c, uint32_t addr)
{
    uint32_t n = 0, dw;
    int hops = 0;

    while (n < 1024u) {
        dw = gtt_load(c, addr + 4u * n);
        if (dw == MI_BATCH_BUFFER_END) {
            return;
        }
        if (dw == MI_STORE_DATA_IMM_GTT) {
            gtt_store(c, gtt_load(c, addr + 4u * (n + 2u)), gtt_load(c, addr + 4u * (n + 3u)));
        }
        if (MI_OPCODE(dw) == MI_OPCODE(MI_BATCH_BUFFER_START) && (dw >> 29) == 0) {
            if (++hops > 4) {
                return;
            }
            chained.cmd = dw;
            chained.addr = gtt_load(c, addr + 4u * (n + 1u));
            nchained++;
            addr = chained.addr;
            n = 0;
            continue;
        }
        n += command_dwords(dw);
    }
}

/** Run one command of engine `idx`'s ring, `d` its dwords. */
static void command_run(struct chip *c, int idx, const uint32_t *d)
{
    struct i915_engine *e = &dev_a->gt.engine[idx];
    uint32_t hws = c->regs[e->hws_pga / 4], k;

    if ((d[0] >> 29) == 3u && (d[0] >> 16) == (GFX_OP_PIPE_CONTROL(4) >> 16)) {
        if (d[1] & PIPE_CONTROL_QW_WRITE) {
            gtt_store(c, d[2] & ~3u, d[3]);
        }
        return;
    }
    if ((d[0] >> 29) != 0) {
        return;
    }
    switch ((d[0] >> 23) & 0x3fu) {
    case 0x02: /* MI_USER_INTERRUPT */
        irq_pending |= e->irq_bit;
        break;
    case 0x21: /* MI_STORE_DWORD_INDEX */
        gtt_store(c, hws + d[1], d[2]);
        break;
    case 0x22: /* MI_LOAD_REGISTER_IMM */
        for (k = 0; 2u * k + 2u < command_dwords(d[0]); k++) {
            c->regs[d[1 + 2 * k] / 4] = d[2 + 2 * k];
        }
        break;
    case 0x26: /* MI_FLUSH_DW */
        if (d[0] & MI_FLUSH_DW_STORE_INDEX) {
            gtt_store(c, hws + (d[1] & ~MI_FLUSH_DW_USE_GTT), d[2]);
        }
        break;
    case 0x31: /* MI_BATCH_BUFFER_START */
        if (nstarts < 64) {
            starts[nstarts].engine = idx;
            starts[nstarts].cmd = d[0];
            starts[nstarts].addr = d[1];
            nstarts++;
        }
        batch_run(c, d[1]);
        break;
    default:
        break;
    }
}

/** The chip's side of a register write: a ring whose tail moved runs from
 * its head to the tail, unless it is hung. */
static void ring_write(struct chip *c, uint32_t reg, uint32_t value)
{
    uint32_t d[128], head, start, size, n, k;
    int i;

    for (i = 0; i < I915_NUM_ENGINES; i++) {
        struct i915_engine *e = &dev_a->gt.engine[i];

        if (reg != RING_TAIL(e->mmio_base) || ring_hung[i] || !e->ring_size) {
            continue;
        }
        start = c->regs[RING_START(e->mmio_base) / 4];
        head = c->regs[RING_HEAD(e->mmio_base) / 4] & HEAD_ADDR;
        size = e->ring_size;
        while (head != value) {
            d[0] = gtt_load(c, start + head);
            n = command_dwords(d[0]);
            for (k = 1; k < n && k < 128u; k++) {
                d[k] = gtt_load(c, start + ((head + 4u * k) & (size - 1u)));
            }
            command_run(c, i, d);
            head = (head + 4u * n) & (size - 1u);
        }
        c->regs[RING_HEAD(e->mmio_base) / 4] = head;
    }
}

/** Take the user interrupts the rings raised, then retire what completed,
 * as the interrupt and the next request would. */
static void idle(void)
{
    uint32_t pending = irq_pending;
    int b;

    irq_pending = 0;
    for (b = 0; b < 32; b++) {
        if ((pending & (1u << b)) && irq_fn[b]) {
            irq_fn[b](dev_a, 1u << b, irq_data[b]);
        }
    }
    i915_retire_requests(dev_a);
}

/** Run the submission worker until it moves nothing. */
static void kick(void)
{
    while (i915_submit_work() > 0) {
    }
}

/* ---- the calls ---- */

/** The arguments of a call on the `n` objects at `o` with `flags`. */
static void args_init(struct drm_i915_gem_execbuffer2 *a, struct drm_i915_gem_exec_object2 *o,
                      uint32_t n, uint64_t flags)
{
    k_memset(a, 0, sizeof(*a));
    a->buffers_ptr = (uintptr_t)o;
    a->buffer_count = n;
    a->flags = flags;
}

/** An entry for `handle`, with nothing else asked. */
static void entry_init(struct drm_i915_gem_exec_object2 *o, uint32_t handle)
{
    k_memset(o, 0, sizeof(*o));
    o->handle = handle;
}

/** EXECBUFFER2's answer for `a`. */
static int32_t execbuf(struct drm_i915_gem_execbuffer2 *a)
{
    return i915_gem_execbuffer2_ioctl(&dev_a->drm, a, &file_a);
}

/** The call on the one batch `handle` with `flags`: its answer. */
static int32_t exec1(uint32_t handle, uint64_t flags)
{
    struct drm_i915_gem_exec_object2 o;
    struct drm_i915_gem_execbuffer2 a;

    entry_init(&o, handle);
    args_init(&a, &o, 1, flags);
    return execbuf(&a);
}

/** Write `value` at `offset` of `handle`. */
static void dword_put(uint32_t handle, uint64_t offset, uint32_t value)
{
    CHECK(prw(&file_a, handle, offset, &value, 4, 1) == 0, "pwrite: a dword written");
}

/** The dword at `offset` of `handle`. */
static uint32_t dword_at(uint32_t handle, uint64_t offset)
{
    uint32_t v = 0;

    CHECK(prw(&file_a, handle, offset, &v, 4, 0) == 0, "pread: a dword read");
    return v;
}

/** An object of 4 KiB holding a batch that ends at once: its handle. */
static uint32_t batch_end(void)
{
    uint32_t h = create(&file_a, 4096);

    dword_put(h, 0, MI_BATCH_BUFFER_END);
    return h;
}

/** The GTT offset of the binding of the whole of `handle`, or ~0. */
static uint64_t offset_of(uint32_t handle)
{
    struct i915_vma *v = eb_binding(object(&file_a, handle));

    return v ? v->start : ~(uint64_t)0;
}

/** A fence of the test's own, not signalled. */
static struct dma_fence *test_fence(void)
{
    struct dma_fence *f = (struct dma_fence *)kzalloc(sizeof(*f));

    dma_fence_init(f, 0, dma_fence_context_alloc(1), 1);
    return f;
}

/** A new syncobj of the file: its handle. */
static uint32_t syncobj_new(void)
{
    struct drm_syncobj_create c;

    k_memset(&c, 0, sizeof(c));
    CHECK(drm_syncobj_create_ioctl(&dev_a->drm, &c, &file_a) == 0, "syncobj: made");
    return c.handle;
}

/** The fence the syncobj `handle` holds, borrowed. */
static struct dma_fence *syncobj_fence(uint32_t handle)
{
    struct drm_syncobj *s = drm_syncobj_find(&file_a, handle);
    struct dma_fence *f = s ? s->fence : 0;

    drm_syncobj_put(s);
    return f;
}

/** The fence of the sync file `fd`, borrowed. */
static struct dma_fence *sync_fence(int32_t fd)
{
    struct dma_fence *f = sync_file_get_fence(fd);

    dma_fence_put(f);
    return f;
}

/* ---- the rows ---- */

/**
 * 78-5b.2: the batch starts on the ring the flags name, unprivileged, at
 * its binding plus its start, the default and RENDER on the render ring
 * and BLT on the blitter; the request's fence is on the object until it
 * retires; the engines that do not run are refused.
 */
static void rings(void)
{
    uint32_t b = batch_end();
    struct drm_i915_gem_object *obj = object(&file_a, b);
    struct drm_i915_gem_exec_object2 o;
    struct drm_i915_gem_execbuffer2 a;

    nstarts = 0;
    CHECK(exec1(b, I915_EXEC_RENDER) == 0 && nstarts == 1 && starts[0].engine == I915_RCS0 &&
              starts[0].cmd ==
                  (MI_BATCH_BUFFER_START | MI_BATCH_PPGTT_HSW | MI_BATCH_NON_SECURE_HSW) &&
              starts[0].addr == (uint32_t)offset_of(b),
          "rings: RENDER starts the batch on rcs0, unprivileged through the PPGTT bit");
    CHECK(i915_gem_object_is_active(obj) && (obj->read_domains & I915_GEM_GPU_DOMAINS) &&
              !obj->write_domain,
          "rings: the request's fence on the object, read by the GPU");
    idle();
    CHECK(!i915_gem_object_is_active(obj) && !dev_a->gt.engine[I915_RCS0].requests,
          "rings: retired once the interrupt is taken");
    CHECK(exec1(b, I915_EXEC_DEFAULT) == 0 && nstarts == 2 && starts[1].engine == I915_RCS0,
          "rings: the default ring is rcs0");
    CHECK(exec1(b, I915_EXEC_BLT) == 0 && nstarts == 3 && starts[2].engine == I915_BCS0 &&
              starts[2].cmd == (MI_BATCH_BUFFER_START | MI_BATCH_NON_SECURE_I965),
          "rings: BLT starts it on bcs0, unprivileged");
    idle();
    entry_init(&o, b);
    args_init(&a, &o, 1, I915_EXEC_RENDER);
    a.batch_start_offset = 64;
    CHECK(execbuf(&a) == 0 && nstarts == 4 && starts[3].addr == (uint32_t)offset_of(b) + 64u,
          "rings: the batch starts at its start");
    idle();
    CHECK(exec1(b, I915_EXEC_BSD) == -EINVAL && exec1(b, I915_EXEC_VEBOX) == -EINVAL &&
              exec1(b, 5) == -EINVAL && exec1(b, I915_EXEC_RENDER | (1u << 13)) == -EINVAL &&
              nstarts == 4,
          "rings: BSD, VEBOX, ring 5 and BSD's flags on RENDER EINVAL");
    close_handle(&file_a, b);
}

/** The call's checks before anything runs, Linux's each. */
static void checks(void)
{
    uint32_t b = batch_end(), t = create(&file_a, 4096);
    struct drm_i915_gem_exec_object2 o[2];
    struct drm_i915_gem_execbuffer2 a;

    nstarts = 0;
    entry_init(&o[0], b);
    args_init(&a, o, 0, 0);
    CHECK(execbuf(&a) == -EINVAL, "checks: no objects EINVAL");
    args_init(&a, o, 1, (uint64_t)1 << 22);
    CHECK(execbuf(&a) == -EINVAL, "checks: an unknown flag EINVAL");
    args_init(&a, o, 1, I915_EXEC_CONSTANTS_MASK);
    CHECK(execbuf(&a) == -EINVAL, "checks: the constants' mode EINVAL");
    args_init(&a, o, 1, I915_EXEC_RESOURCE_STREAMER);
    CHECK(execbuf(&a) == -EINVAL, "checks: the resource streamer EINVAL");
    args_init(&a, o, 1, 0);
    a.num_cliprects = 1;
    CHECK(execbuf(&a) == -EINVAL, "checks: cliprects EINVAL");
    args_init(&a, o, 1, 0);
    a.DR4 = 0xffffffffu;
    CHECK(execbuf(&a) == 0, "checks: DR4 of ~0 taken as 0, for old Mesa");
    idle();
    a.DR1 = 1;
    CHECK(execbuf(&a) == -EINVAL, "checks: DR1 EINVAL");
    args_init(&a, o, 1, 0);
    a.batch_start_offset = 4;
    CHECK(execbuf(&a) == -EINVAL, "checks: a start not of quadwords EINVAL");
    args_init(&a, o, 1, 0);
    a.batch_len = 12;
    CHECK(execbuf(&a) == -EINVAL, "checks: a length not of quadwords EINVAL");
    args_init(&a, o, 1, 0);
    a.batch_start_offset = 4096;
    CHECK(execbuf(&a) == -EINVAL, "checks: a start past the batch EINVAL");
    args_init(&a, o, 1, 0);
    a.batch_start_offset = 8;
    a.batch_len = 4096;
    CHECK(execbuf(&a) == -EINVAL, "checks: a length past the batch EINVAL");
    args_init(&a, o, 1, 0);
    a.rsvd1 = 7;
    CHECK(execbuf(&a) == -ENOENT, "checks: no such context ENOENT");
    args_init(&a, o, 1, 0);
    a.buffers_ptr = 16;
    CHECK(execbuf(&a) == -EFAULT, "checks: objects outside user space EFAULT");
    entry_init(&o[0], 0x7777);
    args_init(&a, o, 1, 0);
    CHECK(execbuf(&a) == -ENOENT, "checks: no such object ENOENT");
    entry_init(&o[0], t);
    entry_init(&o[1], b);
    args_init(&a, o, 2, 0);
    o[0].flags = (uint64_t)1 << 9;
    CHECK(execbuf(&a) == -EINVAL, "checks: an unknown object flag EINVAL");
    o[0].flags = 0;
    o[0].alignment = 3;
    CHECK(execbuf(&a) == -EINVAL, "checks: an alignment not a power of two EINVAL");
    o[0].alignment = 0;
    o[0].flags = EXEC_OBJECT_PINNED;
    o[0].offset = 0x100100;
    CHECK(execbuf(&a) == -EINVAL, "checks: a pinned offset not of pages EINVAL");
    o[0].offset = (uint64_t)1 << 47;
    CHECK(execbuf(&a) == -EINVAL, "checks: a pinned offset not canonical EINVAL");
    o[0].offset = 0x8000000;
    CHECK(execbuf(&a) == -EINVAL, "checks: a pinned offset past the GTT EINVAL");
    o[0].flags = EXEC_OBJECT_PAD_TO_SIZE;
    o[0].offset = 0;
    o[0].pad_to_size = 0x1800;
    CHECK(execbuf(&a) == -EINVAL, "checks: a pad not of pages EINVAL");
    o[0].flags = 0;
    o[1].flags = EXEC_OBJECT_WRITE;
    CHECK(execbuf(&a) == -EINVAL, "checks: a batch that writes itself EINVAL");
    o[1].flags = 0;
    o[0].pad_to_size = 0x1800;
    CHECK(execbuf(&a) == 0 && o[0].pad_to_size == 0x1800,
          "checks: a pad without PAD_TO_SIZE is ignored, as a reserved field was");
    idle();
    CHECK(nstarts == 2, "checks: only the calls that pass start their batch");
    close_handle(&file_a, t);
    close_handle(&file_a, b);
}

/** The batch `b` with a store of 0xc0ffee to the address its dword 2 holds,
 * and its relocation to `target` plus `delta` there. */
static void store_batch(uint32_t b, struct drm_i915_gem_relocation_entry *r, uint32_t target,
                        uint32_t delta)
{
    dword_put(b, 0, MI_STORE_DATA_IMM_GTT);
    dword_put(b, 4, 0);
    dword_put(b, 8, 0);
    dword_put(b, 12, 0xc0ffeeu);
    dword_put(b, 16, MI_BATCH_BUFFER_END);
    k_memset(r, 0, sizeof(*r));
    r->target_handle = target;
    r->delta = delta;
    r->offset = 8;
    r->presumed_offset = 0xdead0000u;
    r->read_domains = I915_GEM_DOMAIN_RENDER;
    r->write_domain = I915_GEM_DOMAIN_RENDER;
}

/**
 * 78-5b.1: Linux's relocations: a presumed offset that is wrong is written
 * with the target's offset plus the signed delta and given back, and the
 * moved objects' offsets go back too; a right one is left; NO_RELOC skips
 * them while every offset is right; HANDLE_LUT names targets by index;
 * the refusals leave the program's offsets alone; the batch with
 * relocations lies above 256 KiB; the write domain makes the target the
 * request's to write.
 */
static void relocations(void)
{
    uint32_t t = create(&file_a, 8192), b = create(&file_a, 4096);
    struct drm_i915_gem_relocation_entry r;
    struct drm_i915_gem_exec_object2 o[2];
    struct drm_i915_gem_execbuffer2 a;
    uint64_t ts, bs;

    store_batch(b, &r, t, 0x100);
    entry_init(&o[0], t);
    entry_init(&o[1], b);
    o[1].relocation_count = 1;
    o[1].relocs_ptr = (uintptr_t)&r;
    args_init(&a, o, 2, I915_EXEC_RENDER);
    CHECK(execbuf(&a) == 0, "relocations: the call passes");
    ts = offset_of(t);
    bs = offset_of(b);
    CHECK(r.presumed_offset == ts && o[0].offset == ts && o[1].offset == bs && bs >= 0x40000u,
          "relocations: the target's offset and both objects' go back, the batch above 256 KiB");
    CHECK(object(&file_a, t)->write_domain == I915_GEM_DOMAIN_RENDER &&
              object(&file_a, t)->base.resv->write,
          "relocations: the write domain makes the request the target's writer");
    idle();
    CHECK(dword_at(b, 8) == (uint32_t)ts + 0x100u && dword_at(t, 0x100) == 0xc0ffeeu,
          "relocations: the address written, and the batch stored through it");
    dword_put(b, 8, 0);
    CHECK(execbuf(&a) == 0, "relocations: again with the presumed offset right");
    idle();
    CHECK(dword_at(b, 8) == 0, "relocations: a right presumed offset is not written");
    r.presumed_offset = 0xdead0000u;
    args_init(&a, o, 2, I915_EXEC_RENDER | I915_EXEC_NO_RELOC);
    CHECK(execbuf(&a) == 0, "relocations: NO_RELOC with every offset right");
    idle();
    CHECK(dword_at(b, 8) == 0 && r.presumed_offset == 0xdead0000u,
          "relocations: NO_RELOC skips them while nothing moved");
    o[0].offset = ts + 0x10000u;
    CHECK(execbuf(&a) == 0 && o[0].offset == ts, "relocations: NO_RELOC, an offset wrong");
    idle();
    CHECK(dword_at(b, 8) == (uint32_t)ts + 0x100u && r.presumed_offset == ts,
          "relocations: an object the program misplaced makes them apply");
    r.target_handle = 0;
    r.presumed_offset = 0xdead0000u;
    r.delta = 0xfffffff0u;
    args_init(&a, o, 2, I915_EXEC_RENDER | I915_EXEC_HANDLE_LUT);
    CHECK(execbuf(&a) == 0, "relocations: HANDLE_LUT, the target by index");
    idle();
    CHECK(dword_at(b, 8) == (uint32_t)ts - 16u,
          "relocations: by index, and the delta signed: the target's offset less 16");
    r.target_handle = 5;
    r.presumed_offset = 0;
    o[0].offset = 0x123000;
    CHECK(execbuf(&a) == -ENOENT && o[0].offset == 0x123000,
          "relocations: an index past the objects ENOENT, the offsets not given back");
    r.target_handle = 0;
    r.presumed_offset = 0xdead0000u;
    r.offset = 4096;
    CHECK(execbuf(&a) == -EINVAL, "relocations: past the object EINVAL");
    r.offset = 6;
    CHECK(execbuf(&a) == -EINVAL, "relocations: not of dwords EINVAL");
    r.offset = 8;
    r.write_domain = I915_GEM_DOMAIN_RENDER | I915_GEM_DOMAIN_SAMPLER;
    CHECK(execbuf(&a) == -EINVAL, "relocations: two write domains EINVAL");
    r.write_domain = 0;
    r.read_domains = I915_GEM_DOMAIN_CPU;
    CHECK(execbuf(&a) == -EINVAL, "relocations: the CPU's domain EINVAL");
    r.read_domains = I915_GEM_DOMAIN_RENDER;
    o[1].relocs_ptr = 32;
    CHECK(execbuf(&a) == -EFAULT, "relocations: entries outside user space EFAULT");
    o[1].relocs_ptr = (uintptr_t)&r;
    entry_init(&o[0], b);
    o[0].relocation_count = 1;
    o[0].relocs_ptr = (uintptr_t)&r;
    entry_init(&o[1], t);
    r.target_handle = 1;
    r.delta = 0x200;
    args_init(&a, o, 2, I915_EXEC_RENDER | I915_EXEC_HANDLE_LUT | I915_EXEC_BATCH_FIRST);
    nstarts = 0;
    CHECK(execbuf(&a) == 0 && nstarts == 1 && starts[0].addr == (uint32_t)bs,
          "relocations: BATCH_FIRST runs the first object");
    idle();
    CHECK(dword_at(t, 0x200) == 0xc0ffeeu, "relocations: and its store lands");
    close_handle(&file_a, b);
    close_handle(&file_a, t);
}

/**
 * Placement: a pinned object lies at its offset and takes it from an idle
 * binding there; a pad makes the node larger; an alignment is kept.
 */
static void placement(void)
{
    uint32_t b = batch_end(), p = create(&file_a, 4096), q = create(&file_a, 4096);
    struct drm_i915_gem_exec_object2 o[2];
    struct drm_i915_gem_execbuffer2 a;
    struct i915_vma *v;

    entry_init(&o[0], p);
    o[0].flags = EXEC_OBJECT_PINNED;
    o[0].offset = 0x200000;
    entry_init(&o[1], b);
    args_init(&a, o, 2, I915_EXEC_NO_RELOC);
    CHECK(execbuf(&a) == 0 && offset_of(p) == 0x200000u && o[0].offset == 0x200000u,
          "placement: a pinned object at its offset");
    idle();
    o[0].handle = q;
    CHECK(execbuf(&a) == 0 && offset_of(q) == 0x200000u && offset_of(p) == ~(uint64_t)0,
          "placement: another pinned there takes the place of the idle one");
    idle();
    entry_init(&o[0], p);
    o[0].flags = EXEC_OBJECT_PAD_TO_SIZE;
    o[0].pad_to_size = 0x10000;
    o[0].alignment = 0x20000;
    args_init(&a, o, 2, 0);
    CHECK(execbuf(&a) == 0, "placement: a pad and an alignment");
    v = eb_binding(object(&file_a, p));
    CHECK(v && v->node_size >= 0x10000u && (v->start & 0x1ffffu) == 0 && o[0].offset == v->start,
          "placement: the node padded and aligned, its offset given back");
    idle();
    close_handle(&file_a, q);
    close_handle(&file_a, p);
    close_handle(&file_a, b);
}

/**
 * 78-5b.3: the in-fence holds the batch until it signals, an error on it
 * keeps the batch from running and reaches the request; the submit fence
 * holds it until the other ring's request is given to the chip; the
 * out-fence is a sync file in the high half of rsvd2.
 */
static void fences(void)
{
    uint32_t b = batch_end(), c = batch_end();
    struct dma_fence *f = test_fence(), *g, *out;
    struct drm_i915_gem_exec_object2 o;
    struct drm_i915_gem_execbuffer2 a;
    int32_t in_fd = sync_file_create(f, "test"), out_fd, out2;

    nstarts = 0;
    entry_init(&o, b);
    args_init(&a, &o, 1, I915_EXEC_RENDER | I915_EXEC_FENCE_OUT);
    a.rsvd2 = 0x1234;
    CHECK(execbuf(&a) == 0 && (a.rsvd2 & 0xffffffffu) == 0x1234, "fences: FENCE_OUT");
    out_fd = (int32_t)(a.rsvd2 >> 32);
    out = sync_fence(out_fd);
    CHECK(out && out->ops == &i915_fence_ops && !dma_fence_is_signaled(out),
          "fences: the out-fence is the request's, not signalled");
    idle();
    CHECK(dma_fence_is_signaled(out) && !out->error, "fences: signalled once it retires");
    (void)fd_free(self.fds, out_fd);
    args_init(&a, &o, 1, I915_EXEC_RENDER | I915_EXEC_FENCE_IN | I915_EXEC_FENCE_OUT);
    a.rsvd2 = (uint32_t)in_fd;
    nstarts = 0;
    CHECK(execbuf(&a) == 0 && nstarts == 0, "fences: FENCE_IN holds the batch");
    out_fd = (int32_t)(a.rsvd2 >> 32);
    dma_fence_signal(f);
    kick();
    CHECK(nstarts == 1, "fences: the batch runs once the in-fence signals");
    idle();
    CHECK(dma_fence_is_signaled(sync_fence(out_fd)) && !sync_fence(out_fd)->error,
          "fences: and its request completes");
    (void)fd_free(self.fds, out_fd);
    (void)fd_free(self.fds, in_fd);
    dma_fence_put(f);

    f = test_fence();
    in_fd = sync_file_create(f, "test");
    a.rsvd2 = (uint32_t)in_fd;
    CHECK(execbuf(&a) == 0, "fences: an in-fence that will fail");
    out_fd = (int32_t)(a.rsvd2 >> 32);
    dma_fence_set_error(f, -EIO);
    dma_fence_signal(f);
    kick();
    idle();
    CHECK(nstarts == 1 && dma_fence_is_signaled(sync_fence(out_fd)) &&
              sync_fence(out_fd)->error == -EIO,
          "fences: its error skips the batch and reaches the request");
    (void)fd_free(self.fds, out_fd);
    dma_fence_put(f);

    a.flags = I915_EXEC_RENDER | I915_EXEC_FENCE_IN | I915_EXEC_FENCE_SUBMIT;
    CHECK(execbuf(&a) == -EINVAL, "fences: FENCE_IN with FENCE_SUBMIT EINVAL");
    a.flags = I915_EXEC_RENDER | I915_EXEC_FENCE_IN;
    a.rsvd2 = 63;
    CHECK(execbuf(&a) == -EINVAL, "fences: an in-fence that is no sync file EINVAL");
    (void)fd_free(self.fds, in_fd);

    g = test_fence();
    in_fd = sync_file_create(g, "test");
    entry_init(&o, c);
    args_init(&a, &o, 1, I915_EXEC_BLT | I915_EXEC_FENCE_IN | I915_EXEC_FENCE_OUT);
    a.rsvd2 = (uint32_t)in_fd;
    CHECK(execbuf(&a) == 0, "fences: a blitter batch held by a fence");
    out2 = (int32_t)(a.rsvd2 >> 32);
    entry_init(&o, b);
    args_init(&a, &o, 1, I915_EXEC_RENDER | I915_EXEC_FENCE_SUBMIT);
    a.rsvd2 = (uint32_t)out2;
    nstarts = 0;
    CHECK(execbuf(&a) == 0 && nstarts == 0,
          "fences: FENCE_SUBMIT holds the render batch behind the blitter's");
    dma_fence_signal(g);
    kick();
    CHECK(nstarts == 2 && starts[0].engine == I915_BCS0 && starts[1].engine == I915_RCS0,
          "fences: both run once the blitter's is given to the chip, in that order");
    idle();
    (void)fd_free(self.fds, out2);
    (void)fd_free(self.fds, in_fd);
    dma_fence_put(g);
    close_handle(&file_a, c);
    close_handle(&file_a, b);
}

/**
 * 78-5b.3: the fence array: a syncobj to wait on holds the batch, one to
 * signal takes the request's fence; Linux's refusals; and the extension
 * chain, which no extension may yet join.
 */
static void fence_array(void)
{
    uint32_t b = batch_end(), sw = syncobj_new(), ss = syncobj_new(), empty = syncobj_new();
    struct drm_i915_gem_exec_fence fa[2];
    struct i915_user_extension ext;
    struct drm_i915_gem_exec_object2 o;
    struct drm_i915_gem_execbuffer2 a;
    struct drm_syncobj *s = drm_syncobj_find(&file_a, sw);
    struct dma_fence *h = test_fence();

    drm_syncobj_replace_fence(s, h);
    drm_syncobj_put(s);
    fa[0].handle = sw;
    fa[0].flags = I915_EXEC_FENCE_WAIT;
    fa[1].handle = ss;
    fa[1].flags = I915_EXEC_FENCE_SIGNAL;
    entry_init(&o, b);
    args_init(&a, &o, 1, I915_EXEC_RENDER | I915_EXEC_FENCE_ARRAY);
    a.cliprects_ptr = (uintptr_t)fa;
    a.num_cliprects = 2;
    nstarts = 0;
    CHECK(execbuf(&a) == 0 && nstarts == 0 && syncobj_fence(ss) &&
              syncobj_fence(ss)->ops == &i915_fence_ops,
          "fence array: WAIT holds the batch, SIGNAL takes the request's fence");
    dma_fence_signal(h);
    kick();
    idle();
    CHECK(nstarts == 1 && dma_fence_is_signaled(syncobj_fence(ss)),
          "fence array: the batch runs once the syncobj's fence signals");
    dma_fence_put(h);
    fa[0].handle = empty;
    CHECK(execbuf(&a) == -EINVAL, "fence array: WAIT on a syncobj with no fence EINVAL");
    fa[0].flags = I915_EXEC_FENCE_WAIT | I915_EXEC_FENCE_SIGNAL;
    CHECK(execbuf(&a) == 0 && syncobj_fence(empty), "fence array: unless it also signals");
    idle();
    fa[0].handle = 0x999;
    CHECK(execbuf(&a) == -ENOENT, "fence array: no such syncobj ENOENT");
    fa[0].handle = sw;
    fa[0].flags = 4;
    CHECK(execbuf(&a) == -EINVAL, "fence array: an unknown flag EINVAL");
    fa[0].flags = I915_EXEC_FENCE_WAIT;
    a.flags |= I915_EXEC_USE_EXTENSIONS;
    CHECK(execbuf(&a) == -EINVAL, "fence array: with extensions EINVAL");
    args_init(&a, &o, 1, I915_EXEC_RENDER | I915_EXEC_USE_EXTENSIONS);
    CHECK(execbuf(&a) == 0, "extensions: an empty chain");
    idle();
    k_memset(&ext, 0, sizeof(ext));
    a.cliprects_ptr = (uintptr_t)&ext;
    CHECK(execbuf(&a) == -EINVAL, "extensions: the timeline fences, not yet here, EINVAL");
    close_handle(&file_a, b);
}

/**
 * 78-8b.2: the command parser on the call's path. A batch whose length the
 * call gives runs from its shadow, privileged, then from the trampoline
 * past its length, unprivileged, whoever calls; one the tables refuse has
 * its shadow end at once and the trampoline chain to the original, which
 * runs; a command the ring does not know fails the call and starts
 * nothing. A batch without a length runs as it is; SECURE needs the
 * master and privilege. The shadow, and a batch closed while it runs, are
 * freed only when the request retires.
 */
static void parser(void)
{
    uint32_t b = batch_end(), at, shadow, frames;
    struct drm_i915_gem_exec_object2 o;
    struct drm_i915_gem_execbuffer2 a;
    struct chip *c = &chip_a;

    entry_init(&o, b);
    args_init(&a, &o, 1, I915_EXEC_RENDER);
    a.batch_len = 8;
    nstarts = 0;
    self.euid = 1000;
    CHECK(execbuf(&a) == 0 && nstarts == 2, "parser: a batch with a length starts twice");
    shadow = starts[0].addr;
    CHECK(starts[0].cmd == MI_BATCH_BUFFER_START && shadow != (uint32_t)offset_of(b),
          "parser: first its shadow, privileged");
    CHECK(starts[1].cmd == (MI_BATCH_BUFFER_START | MI_BATCH_PPGTT_HSW | MI_BATCH_NON_SECURE_HSW) &&
              starts[1].addr == shadow + 8u,
          "parser: then the trampoline past its length, unprivileged");
    CHECK(gtt_load(c, shadow) == MI_BATCH_BUFFER_END &&
              gtt_load(c, shadow + 8u) == MI_BATCH_BUFFER_END,
          "parser: the shadow holds the batch and the trampoline's end");
    idle();
    self.euid = 0;
    CHECK(execbuf(&a) == 0 && nstarts == 4 && starts[2].cmd == MI_BATCH_BUFFER_START,
          "parser: a privileged caller's batch parsed too");
    args_init(&a, &o, 1, I915_EXEC_BLT);
    a.batch_len = 8;
    CHECK(execbuf(&a) == 0 && nstarts == 6 && starts[4].cmd == MI_BATCH_BUFFER_START &&
              starts[5].cmd == (MI_BATCH_BUFFER_START | MI_BATCH_NON_SECURE_I965),
          "parser: on the blitter too");
    idle();

    /* A store through the GGTT, which the tables refuse, runs from the
     * original through the trampoline. */
    at = (uint32_t)offset_of(b) + 0x800u;
    dword_put(b, 0, MI_STORE_DATA_IMM_GTT);
    dword_put(b, 4, 0);
    dword_put(b, 8, at);
    dword_put(b, 12, 0x5eedu);
    dword_put(b, 16, MI_BATCH_BUFFER_END);
    dword_put(b, 20, MI_NOOP);
    args_init(&a, &o, 1, I915_EXEC_RENDER);
    a.batch_len = 24;
    nstarts = 0;
    nchained = 0;
    self.euid = 1000;
    CHECK(execbuf(&a) == 0 && nstarts == 2 && (uint32_t)offset_of(b) + 0x800u == at,
          "parser: a refused batch runs");
    CHECK(gtt_load(c, starts[0].addr) == MI_BATCH_BUFFER_END, "parser: its shadow ends at once");
    CHECK(nchained == 1 && chained.cmd == (MI_BATCH_BUFFER_START | MI_BATCH_NON_SECURE_HSW) &&
              chained.addr == (uint32_t)offset_of(b),
          "parser: the trampoline chains to the original, unprivileged");
    idle();
    CHECK(dword_at(b, 0x800) == 0x5eedu, "parser: the original ran its store");

    dword_put(b, 0, 7u << 29);
    CHECK(execbuf(&a) == -EINVAL && nstarts == 2, "parser: an unknown client EINVAL, nothing run");
    dword_put(b, 0, MI_BATCH_BUFFER_END);

    a.batch_len = 0;
    CHECK(execbuf(&a) == 0 && nstarts == 3 && (starts[2].cmd & MI_BATCH_NON_SECURE_HSW) &&
              starts[2].addr == (uint32_t)offset_of(b),
          "parser: without a length it is not parsed, and runs unprivileged");
    a.flags |= I915_EXEC_SECURE;
    CHECK(execbuf(&a) == -EPERM, "parser: SECURE unprivileged EPERM");
    self.euid = 0;
    CHECK(execbuf(&a) == -EPERM, "parser: SECURE without the master EPERM");
    dev_a->drm.master = &file_a;
    CHECK(execbuf(&a) == 0 && nstarts == 4 && starts[3].cmd == MI_BATCH_BUFFER_START,
          "parser: SECURE with both runs privileged");
    dev_a->drm.master = 0;
    idle();

    /* The shadow's pages and a closed batch's live until the retire. */
    frames = (uint32_t)frames_live;
    a.flags = I915_EXEC_RENDER;
    a.batch_len = 8;
    CHECK(execbuf(&a) == 0 && (uint32_t)frames_live == frames + 1u,
          "parser: the shadow's page held while the batch runs");
    close_handle(&file_a, b);
    CHECK((uint32_t)frames_live == frames + 1u, "parser: the closed batch held while it runs");
    idle();
    CHECK((uint32_t)frames_live == frames - 1u, "parser: both freed once the request retires");
}

/**
 * GEN7_SOL_RESET zeroes the four stream output offsets before the render
 * batch; on the blitter it fails, the request added without its batch.
 * A ring that refuses the commands fails the call with EIO.
 */
static void sol_and_faults(void)
{
    uint32_t b = batch_end(), i;
    struct chip *c = &chip_a;
    int bad = 0;

    for (i = 0; i < 4; i++) {
        c->regs[GEN7_SO_WRITE_OFFSET(i) / 4] = 0x55u;
    }
    nstarts = 0;
    CHECK(exec1(b, I915_EXEC_RENDER | I915_EXEC_GEN7_SOL_RESET) == 0 && nstarts == 1,
          "sol: the reset on the render ring");
    for (i = 0; i < 4; i++) {
        bad += c->regs[GEN7_SO_WRITE_OFFSET(i) / 4] != 0;
    }
    CHECK(bad == 0, "sol: the four offsets zeroed");
    CHECK(exec1(b, I915_EXEC_BLT | I915_EXEC_GEN7_SOL_RESET) == -EINVAL && nstarts == 1,
          "sol: on the blitter EINVAL, its batch not run");
    idle();
    fault_left[I915_FAULT_SUBMIT] = 1;
    CHECK(exec1(b, I915_EXEC_RENDER) == -EIO && nstarts == 1, "faults: a refused ring EIO");
    CHECK(eb_binding(object(&file_a, b))->pin_count == 0, "faults: nothing left pinned");
    close_handle(&file_a, b);
}

int main(void)
{
    int before;

    model_start();
    dev_a = bind(&chip_a, 1);
    dev_a->drm.features |= DRIVER_SYNCOBJ;
    file_a.dev = &dev_a->drm;
    chip_write_hook = ring_write;
    CHECK(i915_gt_init(dev_a) == 0, "setup: the rings start");
    CHECK(i915_driver_open(&dev_a->drm, &file_a) == 0, "setup: the file opened");
    rings();
    before = allocations;
    checks();
    relocations();
    placement();
    fences();
    fence_array();
    parser();
    sol_and_faults();
    idle();
    drm_syncobj_release(&file_a);
    CHECK(allocations == before, "the end: every record of the calls freed");
    CHECK(irq_depth == 0, "the end: the interrupt flag as it was");
    printf("%s: %s\n", MODEL_TEST, failures ? "failed" : "ok");
    return failures != 0;
}
