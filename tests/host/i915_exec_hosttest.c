/*
 * i915_exec_hosttest.c: the Intel driver's EXECBUFFER2,
 * i915_gem_execbuffer.c, with its requests, contexts, syncobjs and sync
 * files, on the chip of i915_gem_model.c, whose rings run what the driver
 * gives them as each tail moves: the requests' flushes, switches and
 * breadcrumbs, and the batches they start, of which it runs
 * MI_STORE_DATA_IMM, so that a row sees where a relocated address points.
 * The user interrupt is taken when a row asks for it.
 *
 * host-deps: rach2/sys/tick.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/spinlock_host.c
 * host-deps: tests/host/compat/atomic_host.c
 * host-deps: tests/host/compat/fd_flat.c
 * host-deps: tests/host/compat/mutex_none.c
 * host-deps: tests/host/compat/mmap_lock_host.c
 *
 * The rows, one for each of 78-5b's subtasks: the call's checks and
 * Linux's refusals; relocations as Linux's eb_relocate_entry applies them,
 * with presumed offsets, I915_EXEC_NO_RELOC, I915_EXEC_HANDLE_LUT, signed
 * deltas and I915_EXEC_BATCH_FIRST, and objects placed at a pinned offset,
 * with a pad and an alignment; the batch started on the render and the
 * blitter ring, the stream output reset, and the request's fence; the
 * in-fence, the submit fence, the out-fence and the fence array; the
 * timeline fences of the call's extension (78-11a.1); each of the
 * driver's ioctls that Mesa's Crocus and hasvk call, against Linux's
 * numbers, and the sync files of an image's dma-buf (78-11a.2); the
 * command parser's shadow and trampoline on the call's path (78-8b), and
 * the objects held until the request retires, their bindings kept while
 * the object is busy (78-5c); a batch whose breadcrumb is written before
 * its interrupt is taken, idle to WAIT and BUSY with the GT left awake
 * (80-61); the unbind wait's signal asked for before its spin (80-72.1); a
 * ring that refuses the commands; the heap's blocks a call shaped as
 * Crocus makes it takes on an idle ring and on a busy one (84-146); and
 * the GT's parks and unparks, the interrupt's unmasks, the shadow's pages
 * and the heap's blocks of such calls each waited for (84-157); and the
 * retire worker's pass that only tries the GEM lock, with the call
 * retiring its own engine (84-164).
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
#include "drm/i915/i915_hangcheck.c"
#include "drm/i915/i915_reset.c"
#include "drm/i915/i915_gpu_error.c"
#include "drm/i915/i915_ring.c"
#include "drm/i915/i915_ppgtt.c"
#include "drm/i915/i915_gem_execbuffer.c"
#include "drm/i915/i915_cmd_parser.c"

/* The heap's blocks a Crocus call takes at most, on an idle ring and on a
 * busy one: the call's objects, its fence array, and its request with its
 * hold in one block, and on a busy ring a new shadow, whose pool's are all
 * in use. Before 84-146 the reservations' merges and copies made them 21
 * and 23, and before 84-157 the hold's own block 4 and 7. */
#define HEAP_IDLE_CALL 3
#define HEAP_BUSY_CALL 6

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

/* The GTT's own entries, which the aliasing PPGTT's directory takes; the
 * chip's GTT keeps its pages in `gtt` and walks no PPGTT. */
static uint32_t gsm[GTT_ENTRIES];

uint32_t i915_gtt_read(struct i915_hw *hw, uint32_t index)
{
    (void)hw;
    return index < GTT_ENTRIES ? gsm[index] : 0;
}

void i915_gtt_write(struct i915_hw *hw, uint32_t index, uint32_t pte)
{
    (void)hw;
    if (index < GTT_ENTRIES) {
        gsm[index] = pte;
    }
}

void i915_ggtt_invalidate(struct drm_i915_private *i915)
{
    (void)i915;
}

uint32_t i915_ggtt_pte_encode(uint64_t addr, int cache)
{
    return GEN6_PTE_VALID | HSW_GTT_ADDR_ENCODE(addr) |
           (cache != I915_CACHE_NONE ? HSW_WB_LLC_AGE3 : 0);
}

static void ring_release(int i);

/* The engine whose held ring the next delay lets run, as the chip that
 * finishes a request during a wait's spin; -1 for none. */
static int release_in_delay = -1;

void i915_delay_us(struct i915_hw *hw, uint32_t us)
{
    int i = release_in_delay;

    (void)hw;
    now_ns += (int64_t)us * 1000;
    if (i >= 0) {
        release_in_delay = -1;
        ring_release(i);
    }
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

/* The unmasks of a ring's user interrupt, which park_per_call counts. */
static int unmasks;

void i915_irq_unmask(struct drm_i915_private *i915, int bank, uint32_t bits)
{
    (void)i915;
    (void)bank;
    (void)bits;
    unmasks++;
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
 * them: MI commands below 0x10 are one and the others carry their length
 * less two in their low six bits, except MI_LOAD_REGISTER_IMM's eight.
 * MI_FLUSH_DW has the video ring's MI_INVALIDATE_BSD above its six length
 * bits; the 2D and 3D commands carry their length in their low byte. */
static uint32_t command_dwords(uint32_t dw)
{
    if ((dw >> 29) == 0) {
        return MI_OPCODE(dw) < 0x10u    ? 1u
               : MI_OPCODE(dw) == 0x22u ? (dw & 0xffu) + 2u
                                        : MI_LENGTH(dw);
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
    uint32_t d[257], head, start, size, n, k;
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
            for (k = 1; k < n; k++) {
                d[k] = gtt_load(c, start + ((head + 4u * k) & (size - 1u)));
            }
            command_run(c, i, d);
            head = (head + 4u * n) & (size - 1u);
        }
        c->regs[RING_HEAD(e->mmio_base) / 4] = head;
    }
}

/** Take the user interrupts the rings raised. */
static void interrupts(void)
{
    uint32_t pending = irq_pending;
    int b;

    irq_pending = 0;
    for (b = 0; b < 32; b++) {
        if ((pending & (1u << b)) && irq_fn[b]) {
            irq_fn[b](dev_a, 1u << b, irq_data[b]);
        }
    }
}

/** Take the user interrupts the rings raised, then retire what completed,
 * as the interrupt and the next request would. */
static void idle(void)
{
    interrupts();
    i915_retire_requests(dev_a);
}

/** Hold the ring of engine `i` still, so that the requests it is given
 * stay running until `ring_release`. */
static void ring_hold(int i)
{
    ring_hung[i] = 1;
}

/** Let the ring of engine `i` run what it was given while it was held. */
static void ring_release(int i)
{
    struct i915_engine *e = &dev_a->gt.engine[i];

    ring_hung[i] = 0;
    ring_write(&chip_a, RING_TAIL(e->mmio_base), chip_a.regs[RING_TAIL(e->mmio_base) / 4]);
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

/** 78-14a.1: the batch `b` started on the video rings, the fifth to the
 * seventh start of rings() below, and retired. */
static void rings_video(uint32_t b)
{
    CHECK(exec1(b, I915_EXEC_BSD) == 0 && nstarts == 5 && starts[4].engine == I915_VCS0 &&
              starts[4].cmd == (MI_BATCH_BUFFER_START | MI_BATCH_NON_SECURE_I965),
          "rings: BSD starts it on vcs0, unprivileged");
    CHECK(exec1(b, I915_EXEC_BSD | I915_EXEC_BSD_RING2) == 0 && nstarts == 6 &&
              starts[5].engine == I915_VCS0,
          "rings: BSD's ring flags name vcs0 too, Haswell's one video ring");
    CHECK(exec1(b, I915_EXEC_VEBOX) == 0 && nstarts == 7 && starts[6].engine == I915_VECS0 &&
              starts[6].cmd == (MI_BATCH_BUFFER_START | MI_BATCH_NON_SECURE_I965),
          "rings: VEBOX starts it on vecs0, unprivileged");
    idle();
    CHECK(!dev_a->gt.engine[I915_VCS0].requests && !dev_a->gt.engine[I915_VECS0].requests,
          "rings: the video rings' requests retired once their interrupts are taken");
}

/**
 * 78-5b.2 and 78-14a.1: the batch starts on the ring the flags name,
 * unprivileged, at its binding plus its start, the default and RENDER on
 * the render ring, BLT on the blitter, BSD with any of its ring flags on
 * the video ring and VEBOX on the video enhancement ring; the request's
 * fence is on the object until it retires; a ring number past them and
 * BSD's flags on another ring are refused.
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
    CHECK(obj->base.resv->nreads == 1 && obj->base.resv->reads[0]->ops == &i915_fence_ops &&
              (obj->read_domains & I915_GEM_GPU_DOMAINS) && !obj->write_domain,
          "rings: the request's fence on the object, read by the GPU");
    CHECK(!i915_gem_object_is_active(obj) && dev_a->gt.engine[I915_RCS0].requests,
          "rings: idle once the breadcrumb is written, before the interrupt, as Linux's");
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
    rings_video(b);
    CHECK(exec1(b, 5) == -EINVAL && exec1(b, I915_EXEC_RENDER | (1u << 13)) == -EINVAL &&
              nstarts == 7,
          "rings: ring 5 and BSD's flags on RENDER EINVAL");
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
 * 78-5c: a program that closes its objects while their batch runs leaves
 * them to the request, which keeps their bindings and pages until the chip
 * is past it and it retires, as Linux's active bindings keep an object;
 * a binding of a busy object is not undone meanwhile.
 */
static void lifetime(void)
{
    uint32_t t = create(&file_a, 8192), b = create(&file_a, 4096);
    struct drm_i915_gem_relocation_entry r;
    struct drm_i915_gem_exec_object2 o[2];
    struct drm_i915_gem_execbuffer2 a;
    struct drm_i915_gem_object *obj = object(&file_a, t);
    struct i915_vma *v;
    uint64_t ts, bs;
    uintptr_t frame;

    store_batch(b, &r, t, 0x100);
    entry_init(&o[0], t);
    entry_init(&o[1], b);
    o[1].relocation_count = 1;
    o[1].relocs_ptr = (uintptr_t)&r;
    args_init(&a, o, 2, I915_EXEC_RENDER);
    ring_hold(I915_RCS0);
    CHECK(execbuf(&a) == 0, "lifetime: the call passes");
    ts = offset_of(t);
    bs = offset_of(b);
    frame = (uintptr_t)chip_a.gtt[ts >> 12];
    close_handle(&file_a, t);
    close_handle(&file_a, b);
    v = eb_binding(obj);
    CHECK(i915_gem_object_is_active(obj) && v && v->start == ts && frame != 0 &&
              (uintptr_t)chip_a.gtt[ts >> 12] == frame && chip_a.gtt[bs >> 12] != 0,
          "lifetime: closed while the request runs, both keep their bindings");
    CHECK(v && i915_vma_unbind(v, 1) == -EBUSY && i915_gem_object_unbind(obj, 1) == -EAGAIN &&
              chip_a.gtt[ts >> 12] != 0,
          "lifetime: a busy object's binding is not undone");
    ring_release(I915_RCS0);
    idle();
    CHECK(chip_a.gtt[ts >> 12] == 0 && chip_a.gtt[bs >> 12] == 0,
          "lifetime: both freed and unbound once the request retires");
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
    CHECK(out && out->ops == &i915_fence_ops && !out->signaled,
          "fences: the out-fence is the request's, not yet signalled");
    CHECK(out && dma_fence_is_signaled(out) && !out->listened,
          "fences: asked once its breadcrumb is written, before the interrupt, it has signalled");
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
    CHECK(execbuf(&a) == -EINVAL && !syncobj_fence(empty),
          "fence array: even when it also signals, as Linux");
    fa[0].flags = 0;
    CHECK(execbuf(&a) == 0 && !syncobj_fence(empty), "fence array: neither flag, left alone");
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
    ext.name = 1;
    a.cliprects_ptr = (uintptr_t)&ext;
    CHECK(execbuf(&a) == -EINVAL, "extensions: an unknown one EINVAL");
    close_handle(&file_a, b);
}

/** The timeline extension `t` of `n` entries at `fa` and `pt`, chained to
 * `next`. */
static void timeline_init(struct drm_i915_gem_execbuffer_ext_timeline_fences *t,
                          struct drm_i915_gem_exec_fence *fa, uint64_t *pt, uint64_t n, void *next)
{
    k_memset(t, 0, sizeof(*t));
    t->base.name = DRM_I915_GEM_EXECBUFFER_EXT_TIMELINE_FENCES;
    t->base.next_extension = (uintptr_t)next;
    t->fence_count = n;
    t->handles_ptr = (uintptr_t)fa;
    t->values_ptr = (uintptr_t)pt;
}

/** The point of the timeline syncobj `handle` that its fence is, or 0 when
 * the fence is no point. */
static uint64_t newest_point(uint32_t handle)
{
    struct dma_fence *f = syncobj_fence(handle);

    return f && to_dma_fence_chain(f) ? f->seqno : 0;
}

/** Add point `point` of `fence` to the syncobj `handle`, as a timeline
 * signal from elsewhere would. */
static void point_add(uint32_t handle, struct dma_fence *fence, uint64_t point)
{
    struct drm_syncobj *s = drm_syncobj_find(&file_a, handle);

    drm_syncobj_add_point(s, dma_fence_chain_alloc(), fence, point);
    drm_syncobj_put(s);
}

/**
 * 78-11a.1: the timeline fences, DRM_I915_GEM_EXECBUFFER_EXT_TIMELINE_FENCES,
 * as Linux's add_timeline_fence_array takes them, on the timeline syncobj
 * `tl`: a point to signal added to it as the request's fence, a point to
 * wait for holding the batch until its fence signals, a point signalled
 * and gone skipped. `tl` ends at point 3.
 */
static void timeline_points(uint32_t tl)
{
    uint32_t b = batch_end();
    struct drm_i915_gem_execbuffer_ext_timeline_fences t;
    struct drm_i915_gem_exec_fence fa[2];
    struct drm_i915_gem_exec_object2 o;
    struct drm_i915_gem_execbuffer2 a;
    struct dma_fence *h = test_fence(), *p;
    uint64_t pt[2];

    entry_init(&o, b);
    args_init(&a, &o, 1, I915_EXEC_RENDER | I915_EXEC_USE_EXTENSIONS);
    a.cliprects_ptr = (uintptr_t)&t;
    fa[0].handle = tl;
    fa[0].flags = I915_EXEC_FENCE_SIGNAL;
    pt[0] = 1;
    timeline_init(&t, fa, pt, 1, 0);
    nstarts = 0;
    CHECK(execbuf(&a) == 0 && nstarts == 1 && newest_point(tl) == 1,
          "timeline: SIGNAL at point 1 adds the point");
    p = syncobj_fence(tl);
    CHECK(p && to_dma_fence_chain(p)->fence->ops == &i915_fence_ops && !p->signaled,
          "timeline: whose fence is the request's");
    idle();
    CHECK(dma_fence_is_signaled(syncobj_fence(tl)), "timeline: the point signals as it retires");

    fa[0].flags = I915_EXEC_FENCE_WAIT;
    nstarts = 0;
    CHECK(execbuf(&a) == 0 && nstarts == 1,
          "timeline: WAIT on a point signalled and gone runs at once");
    idle();

    point_add(tl, h, 2);
    fa[0].flags = I915_EXEC_FENCE_WAIT;
    pt[0] = 2;
    fa[1].handle = tl;
    fa[1].flags = I915_EXEC_FENCE_SIGNAL;
    pt[1] = 3;
    timeline_init(&t, fa, pt, 2, 0);
    nstarts = 0;
    CHECK(execbuf(&a) == 0 && nstarts == 0 && newest_point(tl) == 3,
          "timeline: WAIT at point 2 holds the batch, SIGNAL adds point 3");
    p = syncobj_fence(tl);
    dma_fence_signal(h);
    kick();
    CHECK(nstarts == 1, "timeline: the batch runs once point 2's fence signals");
    idle();
    CHECK(dma_fence_is_signaled(p), "timeline: and point 3 signals as it retires");
    dma_fence_put(h);
    close_handle(&file_a, b);
}

/**
 * 78-11a.1: the timeline fences' refusals as Linux's, with the chain record
 * of a point taken before a refusal freed; a syncobj with neither flag,
 * passed over without a fence and waited for with one, as Linux; a binary
 * syncobj at point 0; two extensions in one chain; and the timeline `tl`
 * that timeline_points left at point 3.
 */
static void timeline_refusals(uint32_t tl)
{
    uint32_t b = batch_end(), bin = syncobj_new(), empty = syncobj_new();
    struct drm_i915_gem_execbuffer_ext_timeline_fences t, t2;
    struct drm_i915_gem_exec_fence fa[2], fb[1];
    struct drm_i915_gem_exec_object2 o;
    struct drm_i915_gem_execbuffer2 a;
    struct dma_fence *h;
    struct drm_syncobj *s;
    uint64_t pt[2], pb[1];

    entry_init(&o, b);
    args_init(&a, &o, 1, I915_EXEC_RENDER | I915_EXEC_USE_EXTENSIONS);
    a.cliprects_ptr = (uintptr_t)&t;
    fa[0].handle = tl;
    fa[0].flags = I915_EXEC_FENCE_WAIT;
    pt[0] = 9;
    timeline_init(&t, fa, pt, 1, 0);
    CHECK(execbuf(&a) == -EINVAL, "timeline: WAIT on a point not reached EINVAL");
    fa[0].flags = I915_EXEC_FENCE_WAIT | I915_EXEC_FENCE_SIGNAL;
    CHECK(execbuf(&a) == -EINVAL && newest_point(tl) == 3,
          "timeline: WAIT and SIGNAL on one point EINVAL");
    fa[0].flags = I915_EXEC_FENCE_SIGNAL;
    pt[0] = 4;
    fa[1].handle = 0x999;
    fa[1].flags = I915_EXEC_FENCE_WAIT;
    pt[1] = 0;
    timeline_init(&t, fa, pt, 2, 0);
    CHECK(execbuf(&a) == -ENOENT && newest_point(tl) == 3,
          "timeline: no such syncobj ENOENT, the point before it not added");
    fa[1].handle = tl;
    fa[1].flags = 4;
    CHECK(execbuf(&a) == -EINVAL, "timeline: an unknown flag EINVAL");

    fa[0].handle = empty;
    fa[0].flags = I915_EXEC_FENCE_WAIT;
    pt[0] = 0;
    timeline_init(&t, fa, pt, 1, 0);
    CHECK(execbuf(&a) == -EINVAL, "timeline: WAIT on a syncobj with no fence EINVAL");
    fa[0].flags = 0;
    nstarts = 0;
    CHECK(execbuf(&a) == 0 && nstarts == 1 && !syncobj_fence(empty),
          "timeline: neither flag on it, skipped");
    idle();
    fa[0].flags = I915_EXEC_FENCE_WAIT | I915_EXEC_FENCE_SIGNAL;
    CHECK(execbuf(&a) == 0 && syncobj_fence(empty) && !to_dma_fence_chain(syncobj_fence(empty)),
          "timeline: WAIT and SIGNAL at point 0 of one with no fence, as Linux");
    idle();

    h = test_fence();
    s = drm_syncobj_find(&file_a, bin);
    drm_syncobj_replace_fence(s, h);
    drm_syncobj_put(s);
    fa[0].handle = bin;
    fa[0].flags = 0;
    timeline_init(&t, fa, pt, 1, 0);
    nstarts = 0;
    CHECK(execbuf(&a) == 0 && nstarts == 0 && syncobj_fence(bin) == h,
          "timeline: neither flag on a binary syncobj with a fence waits for it, as Linux");
    dma_fence_signal(h);
    kick();
    idle();
    CHECK(nstarts == 1, "timeline: and runs once it signals");
    dma_fence_put(h);

    fa[0].handle = bin;
    fa[0].flags = I915_EXEC_FENCE_SIGNAL;
    pt[0] = 0;
    fb[0].handle = tl;
    fb[0].flags = I915_EXEC_FENCE_SIGNAL;
    pb[0] = 5;
    timeline_init(&t2, fb, pb, 1, 0);
    timeline_init(&t, fa, pt, 1, &t2);
    CHECK(execbuf(&a) == 0 && syncobj_fence(bin)->ops == &i915_fence_ops && newest_point(tl) == 5,
          "timeline: two extensions in a chain, a binary syncobj and a point both signalled");
    idle();

    fa[0].handle = tl;
    fa[0].flags = I915_EXEC_FENCE_SIGNAL;
    pt[0] = 6;
    fa[1].handle = syncobj_new();
    fa[1].flags = 0;
    pt[1] = 0;
    timeline_init(&t, fa, pt, 2, 0);
    CHECK(execbuf(&a) == 0 && newest_point(tl) == 6,
          "timeline: an empty syncobj with neither flag after a new point, which Linux refuses "
          "with the point's stale error");
    idle();

    timeline_init(&t, fa, pt, 0, 0);
    nstarts = 0;
    CHECK(execbuf(&a) == 0 && nstarts == 1, "timeline: no entries");
    idle();
    timeline_init(&t, fa, (uint64_t *)(uintptr_t)8, 1, 0);
    CHECK(execbuf(&a) == -EFAULT, "timeline: points at a bad address EFAULT");
    timeline_init(&t, fa, pt, (uint64_t)1 << 62, 0);
    CHECK(execbuf(&a) == -EINVAL, "timeline: too many entries EINVAL");
    close_handle(&file_a, b);
}

/* An ioctl of the driver that Crocus or hasvk calls (78-11a.2): Rach2's
 * number for it, Linux 6.12's for x86-64, read by compiling Linux's
 * include/uapi/drm/i915_drm.h, and whether the driver serves it, or it is
 * a probe whose failure Mesa takes as the feature's absence. */
struct trace_call {
    const char *name;
    uint32_t ours, linux_cmd;
    int served;
};

static const struct trace_call trace_calls[] = {
    {"GETPARAM", DRM_IOCTL_I915_GETPARAM, 0xc0106446u, 1},
    {"GEM_BUSY", DRM_IOCTL_I915_GEM_BUSY, 0xc0086457u, 1},
    {"GEM_CREATE", DRM_IOCTL_I915_GEM_CREATE, 0xc010645bu, 1},
    {"GEM_SET_DOMAIN", DRM_IOCTL_I915_GEM_SET_DOMAIN, 0x400c645fu, 1},
    {"GEM_SET_TILING", DRM_IOCTL_I915_GEM_SET_TILING, 0xc0106461u, 1},
    {"GEM_GET_TILING", DRM_IOCTL_I915_GEM_GET_TILING, 0xc0106462u, 1},
    {"GEM_GET_APERTURE", DRM_IOCTL_I915_GEM_GET_APERTURE, 0x80106463u, 1},
    {"GEM_MMAP_OFFSET", DRM_IOCTL_I915_GEM_MMAP_OFFSET, 0xc0206464u, 1},
    {"GEM_MADVISE", DRM_IOCTL_I915_GEM_MADVISE, 0xc00c6466u, 1},
    {"GEM_EXECBUFFER2", DRM_IOCTL_I915_GEM_EXECBUFFER2, 0x40406469u, 1},
    {"GEM_EXECBUFFER2_WR", DRM_IOCTL_I915_GEM_EXECBUFFER2_WR, 0xc0406469u, 1},
    {"GEM_WAIT", DRM_IOCTL_I915_GEM_WAIT, 0xc010646cu, 1},
    {"GEM_CONTEXT_CREATE", DRM_IOCTL_I915_GEM_CONTEXT_CREATE, 0xc008646du, 1},
    {"GEM_CONTEXT_CREATE_EXT", DRM_IOCTL_I915_GEM_CONTEXT_CREATE_EXT, 0xc010646du, 1},
    {"GEM_CONTEXT_DESTROY", DRM_IOCTL_I915_GEM_CONTEXT_DESTROY, 0x4008646eu, 1},
    {"GEM_SET_CACHING", DRM_IOCTL_I915_GEM_SET_CACHING, 0x4008646fu, 1},
    {"REG_READ", DRM_IOCTL_I915_REG_READ, 0xc0106471u, 1},
    {"GET_RESET_STATS", DRM_IOCTL_I915_GET_RESET_STATS, 0xc0186472u, 1},
    {"GEM_USERPTR", DRM_IOCTL_I915_GEM_USERPTR, 0xc0186473u, 1},
    {"GEM_CONTEXT_GETPARAM", DRM_IOCTL_I915_GEM_CONTEXT_GETPARAM, 0xc0186474u, 1},
    {"GEM_CONTEXT_SETPARAM", DRM_IOCTL_I915_GEM_CONTEXT_SETPARAM, 0xc0186475u, 1},
    {"PERF_REMOVE_CONFIG", 0x40086478u, 0x40086478u, 0},
    {"QUERY", DRM_IOCTL_I915_QUERY, 0xc0106479u, 1},
};

/**
 * 78-11a.2: each ioctl of the driver that Crocus made for eglinfo and
 * hasvk for vkcube under Linux on the Lenovo, and that Mesa 25.0.7's
 * sources make: its number Linux's, so that its structure has Linux's
 * size, and a row of the driver's table with a handler that a render node
 * may call and a direction that takes Linux's; the probe of i915 perf's
 * dynamic configurations has no row, so the dispatcher refuses it.
 */
static void trace(void)
{
    const struct drm_ioctl_desc *row;
    uint32_t i, j, nr, bad;

    for (i = 0; i < sizeof(trace_calls) / sizeof(trace_calls[0]); i++) {
        nr = UAPI_IOC_NR(trace_calls[i].linux_cmd);
        row = 0;
        for (j = 0; j < I915_NUM_IOCTLS; j++) {
            if (UAPI_IOC_NR(i915_ioctls[j].cmd) == nr) {
                row = &i915_ioctls[j];
            }
        }
        bad = trace_calls[i].ours != trace_calls[i].linux_cmd;
        if (trace_calls[i].served) {
            bad |= !row || !row->func || !(row->flags & DRM_RENDER_ALLOW) ||
                   (UAPI_IOC_DIR(trace_calls[i].linux_cmd) & ~UAPI_IOC_DIR(row->cmd)) != 0;
        } else {
            bad |= row != 0;
        }
        if (bad) {
            printf("%s: fail: trace: %s\n", MODEL_TEST, trace_calls[i].name);
            failures++;
        }
    }
}

/**
 * 78-11a.2: the sync files that hasvk's window system code moves through
 * an image's dma-buf each frame: EXPORT_SYNC_FILE for a reader gives the
 * fence of the batch that wrote the image, and IMPORT_SYNC_FILE of a fence
 * as a writer holds the next batch that reads the image until it signals.
 */
static void wsi_sync_files(void)
{
    uint32_t b = batch_end(), img = create(&file_a, 4096);
    struct drm_i915_gem_exec_object2 o[2];
    struct drm_i915_gem_execbuffer2 a;
    struct dma_buf_export_sync_file ex;
    struct dma_buf_import_sync_file im;
    struct dma_fence *f = test_fence(), *got;
    int buf = export_fd(&file_a, img);
    fd_entry_t *e = fd_get(self.fds, buf);

    entry_init(&o[0], img);
    o[0].flags = EXEC_OBJECT_WRITE;
    entry_init(&o[1], b);
    args_init(&a, o, 2, I915_EXEC_RENDER);
    nstarts = 0;
    CHECK(execbuf(&a) == 0 && nstarts == 1, "wsi: a batch writes the image");
    ex.flags = DMA_BUF_SYNC_READ;
    ex.fd = -1;
    CHECK(e && e->ops->ioctl(e, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, (uintptr_t)&ex) == 0 && ex.fd >= 0,
          "wsi: EXPORT_SYNC_FILE for a reader");
    got = sync_fence(ex.fd);
    CHECK(got && got->ops == &i915_fence_ops && !got->signaled,
          "wsi: its fence is the batch's request");
    idle();
    CHECK(got && dma_fence_is_signaled(got), "wsi: and signals as it retires");
    (void)fd_free(self.fds, ex.fd);

    im.flags = DMA_BUF_SYNC_WRITE;
    im.fd = sync_file_create(f, "test");
    CHECK(e && e->ops->ioctl(e, DMA_BUF_IOCTL_IMPORT_SYNC_FILE, (uintptr_t)&im) == 0,
          "wsi: IMPORT_SYNC_FILE of a writer's fence");
    o[0].flags = 0;
    nstarts = 0;
    CHECK(execbuf(&a) == 0 && nstarts == 0, "wsi: the next batch that reads the image waits");
    dma_fence_signal(f);
    kick();
    CHECK(nstarts == 1, "wsi: and runs once the fence signals");
    idle();
    (void)fd_free(self.fds, im.fd);
    (void)fd_free(self.fds, buf);
    dma_fence_put(f);
    close_handle(&file_a, img);
    close_handle(&file_a, b);
}

/**
 * The parser's shadow in the GT's pool, after `parser`'s rows, with its batch
 * `b` and its call `a`: the shadow's pages and a closed batch's live until
 * the retire; the shadow then stays in the pool, idle, for the next batch,
 * which takes it with no new page. The shrinker drops an idle shadow
 * first, and no new shadow goes in the pool of a GT wedged for good. A
 * retire pass's trim keeps a shadow for a second from when it first finds
 * it idle, not from its take, and drops it once it has been idle a second,
 * as Linux's pool_retire and pool_free_older_than
 * (gt/intel_gt_buffer_pool.c:37-85,98-122): its take 600 ms before the first
 * trim and a second trim 600 ms after the first keep it, where an age run
 * from the take would drop it at the second, and a third trim a second after
 * the first frees it.
 */
static void parser_pool(uint32_t b, struct drm_i915_gem_execbuffer2 *a)
{
    struct drm_i915_gem_object *kept, *got = 0;
    uint32_t frames;

    i915_gem_pool_trim(dev_a, I915_POOL_ALL);
    frames = (uint32_t)frames_live;
    a->flags = I915_EXEC_RENDER;
    a->batch_len = 8;
    CHECK(execbuf(a) == 0 && (uint32_t)frames_live == frames + 1u,
          "parser: the shadow's page held while the batch runs");
    idle();
    kept = dev_a->gt.pool[0];
    CHECK(kept && kept->pool_next == 0 && kept->base.refs == 1u &&
              (uint32_t)frames_live == frames + 1u,
          "parser: the shadow kept in the pool, idle, once the request retires");
    /* The device the scan works on, which the bind's registration names;
     * the model's frames call no shrinker, so none is registered. */
    dev_a->gem.shrinker.i915 = dev_a;
    CHECK(i915_gem_shrinker_scan(&dev_a->gem.shrinker.base, 1u, 0) >= 1u &&
              dev_a->gt.pool[0] == 0 && (uint32_t)frames_live == frames,
          "parser: the shrinker frees an idle shadow");
    dev_a->gt.wedged_terminal = 1;
    CHECK(pool_get(dev_a, 8u + I915_CMD_PARSER_TRAMPOLINE_SIZE, &got) == -EIO && got == 0 &&
              dev_a->gt.pool[0] == 0 && (uint32_t)frames_live == frames,
          "parser: a GT wedged for good takes no new shadow into its pool");
    dev_a->gt.wedged_terminal = 0;
    CHECK(execbuf(a) == 0 && (uint32_t)frames_live == frames + 1u,
          "parser: a new shadow once the GT may run again");
    idle();
    kept = dev_a->gt.pool[0];
    CHECK(kept && kept->pool_next == 0 && kept->base.refs == 1u,
          "parser: and it is kept in the pool, idle");
    CHECK(execbuf(a) == 0 && dev_a->gt.pool[0] == kept && kept->pool_next == 0 &&
              kept->base.refs > 1u && (uint32_t)frames_live == frames + 1u,
          "parser: the next batch takes the same shadow, with no new page");
    i915_delay_us(0, 600000u);
    close_handle(&file_a, b);
    CHECK((uint32_t)frames_live == frames + 1u, "parser: the closed batch held while it runs");
    idle();
    CHECK(i915_gem_pool_trim(dev_a, I915_POOL_STALE) == 0 && dev_a->gt.pool[0] == kept,
          "parser: a retire pass's trim keeps a shadow it first finds idle, 600 ms after its take");
    i915_delay_us(0, 600000u);
    CHECK(i915_gem_pool_trim(dev_a, I915_POOL_STALE) == 0 && dev_a->gt.pool[0] == kept,
          "parser: and keeps it 600 ms later, its idle time counted from that trim");
    i915_delay_us(0, 400000u);
    CHECK(i915_gem_pool_trim(dev_a, I915_POOL_STALE) == 1u && dev_a->gt.pool[0] == 0,
          "parser: a retire pass's trim frees it once it has been idle a second");
    CHECK((uint32_t)frames_live == frames - 1u,
          "parser: both freed once the request retires and the pool is trimmed");
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

    parser_pool(b, &a);
}

/**
 * 80-61: GEM_WAIT on a batch whose breadcrumb is written, before its
 * interrupt is taken: a wait of 0 finds the object idle, and a wait
 * returns at once without asking for the signal, so the request's
 * retirement leaves the engine held for the retire worker's pass, as
 * Linux's i915_fence_signaled answers (i915_request.c:88-91,
 * gem/i915_gem_wait.c:24-25).
 */
static void wait_before_interrupt(void)
{
    uint32_t b = batch_end();
    struct drm_i915_gem_object *obj = object(&file_a, b);
    struct drm_i915_gem_wait w;
    struct dma_fence *f;
    int p = parks;

    nstarts = 0;
    CHECK(exec1(b, I915_EXEC_RENDER) == 0 && nstarts == 1 && obj->base.resv->nreads == 1,
          "early: a batch run, its interrupt not taken");
    f = dma_fence_get(obj->base.resv->reads[0]);
    k_memset(&w, 0, sizeof(w));
    w.bo_handle = b;
    CHECK(i915_gem_wait_ioctl(&dev_a->drm, &w, &file_a) == 0 && f->signaled,
          "early: a wait of 0 finds the object idle");
    w.timeout_ns = -1;
    CHECK(i915_gem_wait_ioctl(&dev_a->drm, &w, &file_a) == 0 && !f->listened,
          "early: a wait returns at once, the signal not asked for");
    idle();
    CHECK(!dev_a->gt.engine[I915_RCS0].requests && dev_a->gt.engine[I915_RCS0].unretired &&
              dev_a->gt.awake && parks == p,
          "early: retired at the interrupt, the engine held and the GT awake");
    dma_fence_put(f);
    close_handle(&file_a, b);
}

/**
 * 80-72.1: the unbind wait asks for the signal of a running batch before
 * it spins, as Linux's i915_active_wait enables its fences
 * (i915_active.c:559-571, 589-620), so a last request the chip finishes
 * during the spin lets its engine go (gt/intel_breadcrumbs.c:220-235), and
 * the retire worker's pass parks the GT (84-157).
 */
static void unbind_wait_spin(void)
{
    uint32_t b = batch_end();
    struct drm_i915_gem_object *obj = object(&file_a, b);
    struct i915_engine *rcs = &dev_a->gt.engine[I915_RCS0];
    struct dma_fence *f = 0;
    int p;

    /* The earlier rows' engines retired and the GT parked. */
    idle();
    i915_gt_retire_requests(dev_a);
    p = parks;
    ring_hold(I915_RCS0);
    CHECK(exec1(b, I915_EXEC_RENDER) == 0 && obj->base.resv->nreads == 1,
          "unbind: a batch running, the engine's last");
    if (obj->base.resv->nreads == 1) {
        f = dma_fence_get(obj->base.resv->reads[0]);
    }
    release_in_delay = I915_RCS0;
    CHECK(f && !f->signaled && i915_gem_object_wait_idle(obj, 1) == 0 && release_in_delay < 0,
          "unbind: the wait returns once the chip finishes the batch during its spin");
    CHECK(f && f->listened && f->signaled && !rcs->requests && !rcs->unretired && dev_a->gt.awake &&
              parks == p,
          "unbind: its signal asked for first, the engine let go and the GT awake");
    i915_gt_retire_requests(dev_a);
    CHECK(!dev_a->gt.awake && parks == p + 1, "unbind: the retire worker's pass parks the GT");
    release_in_delay = -1;
    dma_fence_put(f);
    close_handle(&file_a, b);
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

/** The longest LRI fills its eight-bit length and the model's command
 * buffer; the ring must reach the interrupt after all 128 register writes. */
static void long_lri(void)
{
    struct i915_engine *e = &dev_a->gt.engine[I915_RCS0];
    uint32_t *cs = i915_ring_begin(e, dev_a, 258), i;

    CHECK(cs != NULL, "lri: room for 128 register writes and an interrupt");
    if (!cs) {
        return;
    }
    cs[0] = MI_LOAD_REGISTER_IMM(128);
    for (i = 0; i < 128; i++) {
        cs[1 + 2 * i] = GEN7_SO_WRITE_OFFSET(0);
        cs[2 + 2 * i] = i + 1;
    }
    cs[257] = MI_USER_INTERRUPT;
    irq_pending = 0;
    i915_ring_advance(e, dev_a);
    CHECK(chip_a.regs[GEN7_SO_WRITE_OFFSET(0) / 4] == 128 && (irq_pending & e->irq_bit) &&
              (chip_a.regs[RING_HEAD(e->mmio_base) / 4] & HEAD_ADDR) == e->tail,
          "lri: the last register written and the following interrupt reached");
    chip_a.regs[GEN7_SO_WRITE_OFFSET(0) / 4] = 0;
    idle();
}

/** Run the request, submission and ring-model rows. */
/* ---- RPS's waitboost and interactive mode, which the driver calls ---- */

/** The waitboost, RPS's, which no row here measures. */
void i915_rps_boost(struct i915_request *rq)
{
    (void)rq;
}

/** A request retires: no request here is boosted, so RPS counts nothing. */
void i915_rps_retire(struct i915_request *rq)
{
    (void)rq;
}

/**
 * 84-146: the heap's blocks a call takes, shaped as Mesa's Crocus makes
 * it on the Lenovo: NO_RELOC, HANDLE_LUT and BATCH_FIRST, a batch with a
 * length to parse and a relocation, a buffer read and a target written,
 * and a syncobj to signal, run again and again on one ring. The first
 * call places the objects and, since they moved, copies the relocations
 * in and applies them; the next ones read none. A buffer the ring keeps
 * busy holds one fence of the ring as its writer and one as a reader, its
 * newer point in the older one's place, as Linux's dma_resv_add_fence.
 * Linux 6.12's path allocates the request, its sync points, the fence
 * array's and the call's records; the counts here are of the same kind.
 */
static void heap_per_call(void)
{
    uint32_t b = batch_end(), s = create(&file_a, 4096), t = create(&file_a, 8192);
    uint32_t so = syncobj_new();
    struct drm_i915_gem_relocation_entry r;
    struct drm_i915_gem_exec_object2 o[3];
    struct drm_i915_gem_exec_fence f;
    struct drm_i915_gem_execbuffer2 a;
    struct dma_resv *rt = object(&file_a, t)->base.resv, *rs = object(&file_a, s)->base.resv;
    struct dma_fence *w;
    int i, n, idle_calls, busy_calls, ok = 1;

    k_memset(&r, 0, sizeof(r));
    r.target_handle = 2;
    r.offset = 0x100;
    r.read_domains = I915_GEM_DOMAIN_RENDER;
    entry_init(&o[0], b);
    o[0].relocation_count = 1;
    o[0].relocs_ptr = (uintptr_t)&r;
    entry_init(&o[1], s);
    entry_init(&o[2], t);
    o[2].flags = EXEC_OBJECT_WRITE;
    k_memset(&f, 0, sizeof(f));
    f.handle = so;
    f.flags = I915_EXEC_FENCE_SIGNAL;
    args_init(&a, o, 3,
              I915_EXEC_RENDER | I915_EXEC_NO_RELOC | I915_EXEC_HANDLE_LUT | I915_EXEC_BATCH_FIRST |
                  I915_EXEC_FENCE_ARRAY);
    a.batch_len = 8;
    a.cliprects_ptr = (uintptr_t)&f;
    a.num_cliprects = 1;
    CHECK(execbuf(&a) == 0 && o[2].offset == offset_of(t) && r.presumed_offset == offset_of(t) &&
              dword_at(b, 0x100) == (uint32_t)offset_of(t),
          "heap: the first call places the objects and applies the relocation");
    idle();
    n = kmalloc_calls;
    for (i = 0; i < 4; i++) {
        ok &= execbuf(&a) == 0;
        idle();
    }
    idle_calls = (kmalloc_calls - n) / 4;
    CHECK(ok, "heap: four calls on an idle ring pass");
    ring_hold(I915_RCS0);
    n = kmalloc_calls;
    for (i = 0; i < 4; i++) {
        ok &= execbuf(&a) == 0;
    }
    busy_calls = (kmalloc_calls - n) / 4;
    w = rt->write;
    CHECK(ok && w && w->ops == &i915_fence_ops && !dma_fence_is_signaled(w) && rs->nreads == 1 &&
              rs->reads[0] == w && !rs->write,
          "heap: four on a busy ring leave the target one writer and the buffer one reader, "
          "the last request");
    ring_release(I915_RCS0);
    idle();
    printf("%s: heap: %d blocks a call on an idle ring, %d on a busy one\n", MODEL_TEST, idle_calls,
           busy_calls);
    CHECK(idle_calls <= HEAP_IDLE_CALL && busy_calls <= HEAP_BUSY_CALL,
          "heap: a call takes no more blocks than its records need");
    close_handle(&file_a, t);
    close_handle(&file_a, s);
    close_handle(&file_a, b);
}

/** A fence callback that does nothing: a listener, as a sleeper in
 * SYNCOBJ_WAIT is. */
static void listen_cb(struct dma_fence *f, struct dma_fence_cb *cb)
{
    (void)f;
    (void)cb;
}

/**
 * 84-157: what a Crocus call followed by a wait on its syncobj, as
 * WineD3D's GL round trip makes them on the Lenovo, costs on the path
 * besides the call's own records: the GT's unparks and parks, the user
 * interrupt's unmasks, and new shadows of the parser. Each call's request
 * signals to its listener, the interrupt is taken and the retire worker
 * runs, as on the machine between one call and the next. Linux 6.12 keeps
 * the GT awake from one call to the next: its Lenovo loop of 1,003 calls
 * took 1,013 of the GPU's interrupts, where a park after each would add the
 * switch to the kernel context and its interrupt (gt/intel_engine_pm.c:
 * 151-288); it keeps the user interrupt armed while the engine is awake
 * (gt/intel_breadcrumbs.c:182-207, 266-272) and a shadow in its pool until
 * it has been idle a second (gt/intel_gt_buffer_pool.c:87-122).
 */
static void park_per_call(void)
{
    uint32_t b = batch_end(), t = create(&file_a, 8192), so = syncobj_new();
    struct drm_i915_gem_exec_object2 o[2];
    struct drm_i915_gem_exec_fence f;
    struct drm_i915_gem_execbuffer2 a;
    struct dma_fence_cb cb;
    struct dma_fence *sf;
    int i, n = 0, p = 0, u = 0, m = 0, fr = 0, ok = 1;

    entry_init(&o[0], b);
    entry_init(&o[1], t);
    o[1].flags = EXEC_OBJECT_WRITE;
    k_memset(&f, 0, sizeof(f));
    f.handle = so;
    f.flags = I915_EXEC_FENCE_SIGNAL;
    args_init(&a, o, 2,
              I915_EXEC_RENDER | I915_EXEC_NO_RELOC | I915_EXEC_HANDLE_LUT | I915_EXEC_BATCH_FIRST |
                  I915_EXEC_FENCE_ARRAY);
    a.batch_len = 8;
    a.cliprects_ptr = (uintptr_t)&f;
    a.num_cliprects = 1;
    /* The earlier rows' engines retired and the GT parked; the first call
     * unparks it and the second call is the first measured. */
    idle();
    i915_gt_retire_requests(dev_a);
    CHECK(execbuf(&a) == 0, "per call: the first call");
    idle();
    kick();
    for (i = 0; i < 9; i++) {
        if (i == 1) {
            fr = frames_made;
            n = kmalloc_calls;
            p = parks;
            u = unparks;
            m = unmasks;
        }
        ring_hold(I915_RCS0);
        ok &= execbuf(&a) == 0;
        sf = syncobj_fence(so);
        ok &= sf && dma_fence_add_callback(sf, &cb, listen_cb) == 0;
        ring_release(I915_RCS0);
        idle();
        kick();
    }
    CHECK(ok, "per call: nine calls, each waited for");
    printf("%s: per call: of 8 calls, %d unparks, %d parks, %d interrupt unmasks, %d shadow pages "
           "taken, "
           "%d heap blocks\n",
           MODEL_TEST, unparks - u, parks - p, unmasks - m, frames_made - fr, kmalloc_calls - n);
    CHECK(unparks == u && parks == p && unmasks == m && frames_made == fr,
          "per call: the GT stays awake, its interrupt armed, and the shadow is reused");
    close_handle(&file_a, t);
    close_handle(&file_a, b);
}

/* Another thread's mark on a mutex it holds, which compat/mutex_none.c
 * tells from the test's own. */
static char other_thread;

/**
 * 84-164: the retire worker, woken by a request that signalled to its
 * listener, only tries the GEM lock and leaves the engine to the holder,
 * as Linux's engine_retire only tries the timeline's mutex
 * (gt/intel_gt_requests.c:77-80); EXECBUFFER2 retires its own engine,
 * as Linux's eb_request_add does after it queues the request
 * (gem/i915_gem_execbuffer.c:3104-3106); and with the lock free the
 * worker retires at once. A worker that waited for the lock would stop
 * the test, since in one thread the other holder never lets it go.
 */
static void retire_in_caller(void)
{
    uint32_t b = batch_end(), t = create(&file_a, 8192), so = syncobj_new();
    struct i915_engine *e = &dev_a->gt.engine[I915_RCS0];
    struct drm_i915_gem_exec_object2 o[2];
    struct drm_i915_gem_exec_fence f;
    struct drm_i915_gem_execbuffer2 a;
    struct i915_request *first;
    struct dma_fence_cb cb;
    struct dma_fence *sf;
    int ok;

    entry_init(&o[0], b);
    entry_init(&o[1], t);
    o[1].flags = EXEC_OBJECT_WRITE;
    k_memset(&f, 0, sizeof(f));
    f.handle = so;
    f.flags = I915_EXEC_FENCE_SIGNAL;
    args_init(&a, o, 2,
              I915_EXEC_RENDER | I915_EXEC_NO_RELOC | I915_EXEC_HANDLE_LUT | I915_EXEC_BATCH_FIRST |
                  I915_EXEC_FENCE_ARRAY);
    a.batch_len = 8;
    a.cliprects_ptr = (uintptr_t)&f;
    a.num_cliprects = 1;
    idle();
    kick();
    ring_hold(I915_RCS0);
    ok = execbuf(&a) == 0;
    sf = syncobj_fence(so);
    ok &= sf && dma_fence_add_callback(sf, &cb, listen_cb) == 0;
    first = e->requests_tail;
    ring_release(I915_RCS0);
    interrupts();
    CHECK(ok && first && dma_fence_is_signaled(&first->fence.base) && e->requests == first,
          "retire: the call's request signalled to its listener and waits for retirement");
    dev_a->gem.lock.owner = &other_thread;
    kick();
    CHECK(e->requests == first, "retire: the worker's pass leaves the engine while another thread "
                                "holds the GEM lock");
    dev_a->gem.lock.owner = 0;
    ring_hold(I915_RCS0);
    ok = execbuf(&a) == 0;
    CHECK(ok && e->requests && e->requests != first && e->requests == e->requests_tail,
          "retire: the next call retired the finished request itself");
    sf = syncobj_fence(so);
    ok = sf && dma_fence_add_callback(sf, &cb, listen_cb) == 0;
    ring_release(I915_RCS0);
    interrupts();
    kick();
    CHECK(ok && !e->requests, "retire: with the lock free the worker's pass retires at once");
    close_handle(&file_a, t);
    close_handle(&file_a, b);
}

int main(void)
{
    uint32_t tl;
    int before;

    model_start();
    dev_a = bind(&chip_a, 1);
    dev_a->drm.features |= DRIVER_SYNCOBJ;
    file_a.dev = &dev_a->drm;
    chip_write_hook = ring_write;
    CHECK(i915_gt_init(dev_a) == 0, "setup: the rings start");
    CHECK(i915_driver_open(&dev_a->drm, &file_a) == 0, "setup: the file opened");
    long_lri();
    rings();
    /* The PPGTT's tables, made as buffers are bound, stay with the rings,
     * and the file's PRIME buckets, made at its first export, with the file. */
    before = allocations - (int)dev_a->gt.ppgtt.tables - (file_a.objects.prime != NULL);
    checks();
    relocations();
    placement();
    fences();
    fence_array();
    tl = syncobj_new();
    timeline_points(tl);
    timeline_refusals(tl);
    trace();
    wsi_sync_files();
    parser();
    wait_before_interrupt();
    unbind_wait_spin();
    sol_and_faults();
    lifetime();
    heap_per_call();
    park_per_call();
    retire_in_caller();
    idle();
    /* The pool keeps its shadows past the rows until the GT parks. */
    i915_gem_pool_trim(dev_a, I915_POOL_ALL);
    drm_syncobj_release(&file_a);
    CHECK(allocations - (int)dev_a->gt.ppgtt.tables - (file_a.objects.prime != NULL) == before,
          "the end: every record of the calls freed");
    CHECK(irq_depth == 0, "the end: the interrupt flag as it was");
    CHECK(!mutex_held(&dev_a->gem.lock) &&
              !mutex_held(&((struct i915_file *)file_a.driver_priv)->lock),
          "the end: EXECBUFFER2, which runs without the big lock, gave the GEM lock and the "
          "file's context list lock back");
    printf("%s: %s\n", MODEL_TEST, failures ? "failed" : "ok");
    return failures != 0;
}
