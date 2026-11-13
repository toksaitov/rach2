/**
 * i915_reset_hosttest.c: the Intel graphics driver's recovery from a hang,
 * i915_hangcheck.c, i915_reset.c and i915_gpu_error.c with the contexts'
 * hang counts of i915_gem_context.c, against the host model of the
 * Lenovo's HD 4400, whose rings run the requests' commands from memory and
 * whose GEN6_GDRST resets them.
 *
 * host-deps: tests/host/compat/i915_time_host.c
 * host-deps: rach2/sys/drm/i915/i915_hangcheck.c
 * host-deps: rach2/sys/drm/i915/i915_reset.c
 * host-deps: rach2/sys/drm/i915/i915_gpu_error.c
 * host-deps: rach2/sys/drm/i915/i915_gem_context.c
 * host-deps: rach2/sys/drm/i915/i915_user_extensions.c
 * host-deps: rach2/sys/drm/i915/i915_request.c
 * host-deps: rach2/sys/drm/i915/i915_getparam.c
 * host-deps: rach2/sys/drm/i915/i915_ring.c rach2/sys/drm/i915/i915_irq.c
 * host-deps: rach2/sys/drm/i915/i915_ppgtt.c
 * host-deps: rach2/sys/drm/i915/i915_uncore.c rach2/sys/drm/i915/i915_power.c
 * host-deps: rach2/sys/drm/i915/i915_gtt.c rach2/sys/drm/i915/i915_hw.c
 * host-deps: rach2/sys/dmabuf/fence.c rach2/sys/tick.c
 * host-deps: tests/host/i915_model.c rach2/sys/lib/kstring.c
 * host-deps: rach2/sys/lib/kctypes.c tests/host/compat/spinlock_host.c
 * host-deps: tests/host/compat/atomic_host.c
 * host-deps: tests/host/compat/i915_gem_lock_host.c
 * host-deps: tests/host/compat/mutex_none.c
 *
 * The kernel around the driver is i915_ctx_hosttest.c's: a row makes a
 * fresh model and a device record over it, brought up as the bind does up
 * to the rings, and two files opened on it; the model's clock moves a
 * millisecond at a time and the processor takes the device's messages as
 * it does. A hang is a ring the row holds stuck, which runs nothing until
 * a reset of its domain; the heartbeat runs as the driver's worker would
 * run it, after each 10 ms of the model's clock. GEM's fences are the
 * test's, which counts their restoring.
 *
 * The rows, after 78-6a's subtasks: the heartbeat, the engine's reset and
 * the error state it captures; a reset of the whole GPU when the engine's
 * does not finish; a wedge when that does not either, and the reset that
 * ends it; the guilty context's bans and counts. One row is 78-6c.1's: the
 * GT's and the render engine's workarounds, written at the start and again
 * after each reset. Two are 84-13's: the heartbeat's stop when a beat finds
 * i915.enable_hangcheck off, and its return per engine when it is on again.
 * One is 84-13b's: an engine that goes busy while another keeps the GT
 * awake, armed no sooner than an interval after it went busy.
 *
 * Returns 0 when every row passes, 1 otherwise.
 */

#include <stdarg.h>

#include "drm/i915/i915.h"
#include "drm/i915/i915_drm.h"
#include "drm/i915/i915_reg.h"
#include "dmabuf/dmabuf.h"
#include "lib/kerrno.h"
#include "lib/kirq.h"
#include "mm/kheap.h"
#include "mm/paging.h"
#include "mm/usercopy.h"
#include "proc/percpu.h"
#include "rtc.h"
#include "udelay.h"
#include "i915_model.h"
#include "i915_lenovo.h"
#include "i915_model_ring.h"
#include "i915_model_reset.h"

/* The boot processor's tick count, which forcewake's release timer reads. */
volatile uint64_t kernel_ticks;

extern int printf(const char *format, ...);
extern int vsnprintf(char *s, unsigned long n, const char *format, va_list ap);
extern int posix_memalign(void **p, unsigned long align, unsigned long size);
extern void *calloc(unsigned long count, unsigned long size);
extern void *malloc(unsigned long size);
extern void free(void *p);
extern char *strstr(const char *s, const char *t);

static int fails;

#define CHECK(what, cond)                                \
    do {                                                 \
        if (!(cond)) {                                   \
            printf("i915-reset-test: fail: %s\n", what); \
            fails++;                                     \
        }                                                \
    } while (0)

/* ---- the kernel around the driver ---- */

static char log_text[16384];

/** The log, kept whole for the rows that read it. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    char line[512];
    unsigned long n = 0, k;
    va_list ap;

    (void)level;
    (void)subsystem;
    va_start(ap, format);
    vsnprintf(line, sizeof(line), format, ap);
    va_end(ap);
    while (log_text[n]) {
        n++;
    }
    for (k = 0; line[k] && n + 2 < sizeof(log_text); k++) {
        log_text[n++] = line[k];
    }
    log_text[n++] = '\n';
    log_text[n] = 0;
}

/* The processor's interrupt flag: off while `irq_depth` is above 0, and
 * never on unless the row sets `irq_on`. */
static int irq_depth, irq_on;

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

void k_udelay(uint32_t usec)
{
    (void)usec;
}

/* The heap's pages: each a host page with a physical address of its own,
 * two pages apart from the last. */
#define TEST_PAGES 1024
static struct {
    uint8_t *host;
    uint64_t phys;
} pages[TEST_PAGES];
static int npages, pages_live;
static uint64_t next_phys = 0x50000000ull;

void *kalloc_pages(uint32_t count, uintptr_t *phys)
{
    void *block = 0;
    uint32_t i;

    if (npages + (int)count > TEST_PAGES ||
        posix_memalign(&block, 4096, (unsigned long)count * 4096u) != 0) {
        return 0;
    }
    for (i = 0; i < count; i++) {
        pages[npages].host = (uint8_t *)block + i * 4096u;
        pages[npages].phys = next_phys;
        next_phys += 0x2000u;
        npages++;
    }
    pages_live += (int)count;
    if (phys) {
        *phys = 0;
    }
    return block;
}

void kfree_pages(void *p, uint32_t count)
{
    int i;

    for (i = 0; i < npages; i++) {
        if (pages[i].host >= (uint8_t *)p && pages[i].host < (uint8_t *)p + count * 4096u) {
            pages[i].host = 0;
        }
    }
    pages_live -= (int)count;
    free(p);
}

static int heap_live;

void *kmalloc(size_t size)
{
    void *p = malloc(size ? size : 1);

    heap_live += p != 0;
    return p;
}

void *kzalloc(size_t size)
{
    void *p = calloc(1, size ? size : 1);

    heap_live += p != 0;
    return p;
}

void kfree(void *p)
{
    heap_live -= p != 0;
    free(p);
}

uintptr_t paging_virt_to_phys(uintptr_t va)
{
    int i;

    for (i = 0; i < npages; i++) {
        if (pages[i].host && va >= (uintptr_t)pages[i].host &&
            va < (uintptr_t)pages[i].host + 4096u) {
            return (uintptr_t)(pages[i].phys + (va - (uintptr_t)pages[i].host));
        }
    }
    return 0;
}

size_t frame_total_count(void)
{
    return 0x80000; /* 2 GiB */
}

static uint32_t scratch_page[1024];

#define TEST_SCRATCH 0x7f000000u

/** The model's way to memory: the host page at `phys`. */
static void *test_dma(struct i915_model *m, uint64_t phys)
{
    int i;

    (void)m;
    if (phys == TEST_SCRATCH) {
        return scratch_page;
    }
    for (i = 0; i < npages; i++) {
        if (pages[i].host && pages[i].phys == phys) {
            return pages[i].host;
        }
    }
    return 0;
}

uintptr_t alloc_frame(void)
{
    return TEST_SCRATCH;
}

void free_frame(uintptr_t phys)
{
    (void)phys;
}

int paging_zero_frame(uintptr_t phys)
{
    (void)phys;
    return 0;
}

void arch_dma_fence(void)
{}

int pat_write_combining(void)
{
    return 1;
}

uintptr_t map_mmio_typed(uintptr_t phys_addr, size_t size, pte_t cache)
{
    (void)phys_addr;
    (void)size;
    (void)cache;
    return 0;
}

/* User memory: the host's, but for the first page, which faults. */
#define BAD_USER 16u

int copy_from_user(void *dst, const void __user *src, size_t n)
{
    if ((uintptr_t)src < 4096u) {
        return -1;
    }
    memcpy(dst, (const void *)src, n);
    return 0;
}

int copy_to_user(void __user *dst, const void *src, size_t n)
{
    if ((uintptr_t)dst < 4096u) {
        return -1;
    }
    memcpy((void *)dst, src, n);
    return 0;
}

/* The process that calls, and the clock. */
static process_t caller;
struct percpu cpu_table[NR_CPUS];
static struct i915_model *bus;

uint16_t pci_read_config16(uint8_t b, uint8_t d, uint8_t f, uint8_t offset)
{
    return bus && b == 0 && d == 2 && f == 0 ? (uint16_t)i915_model_config(bus, offset, 2)
                                             : 0xffffu;
}

int kcmdline_has(const char *flag)
{
    (void)flag;
    return 0;
}
static struct drm_i915_private dev;
static int sleeps, stuck_sleeps;

uint64_t rtc_ticks_now(void)
{
    return bus ? bus->now_us / 1000u : 0;
}

void rtc_monotonic(int64_t *sec, int64_t *nsec)
{
    uint64_t us = bus ? bus->now_us : 0;

    *sec = (int64_t)(us / 1000000u);
    *nsec = (int64_t)(us % 1000000u) * 1000;
}

/** Take every message the model has pending, while the processor's
 * interrupts are on. */
static void take_messages(struct i915_model *m)
{
    if (!irq_on || irq_depth != 0) {
        return;
    }
    while (i915_model_take_message(m)) {
        i915_irq_handle(&dev);
    }
}

void wait_queue_init(wait_queue_t *wq)
{
    wq->head = 0;
}

/** A sleep: the model's clock moves a millisecond at a time until `cond`
 * holds; one that never would ends after a minute of the model's clock. */
int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    int steps = 0;

    (void)wq;
    sleeps++;
    while (!cond(arg)) {
        if (++steps > 60000) {
            stuck_sleeps++;
            return 0;
        }
        i915_delay_us(&dev.hw, 1000);
    }
    return 0;
}

int wq_wait_event(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    return wq_wait_event_nointr(wq, cond, arg);
}

void wq_wake_all(wait_queue_t *wq)
{
    (void)wq;
}

/* The GT's park and unpark, RC6's and RPS's, counted. */
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

/** A request retires: no request here is boosted, so RPS counts nothing. */
void i915_rps_retire(struct i915_request *rq)
{
    (void)rq;
}

/** The waitboost, RPS's, which no row here measures. */
void i915_rps_boost(struct i915_request *rq)
{
    (void)rq;
}

/* The GTT's allocator for the kernel's own ranges, i915_vma.c's: from
 * 16 MiB up, each range its own, `held` counting those not given back. */
static uint64_t next_range;
static int ranges_held, ranges_refuse;

int32_t i915_ggtt_reserve(struct drm_i915_private *i915, uint64_t size, uint32_t flags,
                          uint64_t *offset)
{
    (void)i915;
    (void)flags;
    if (ranges_refuse) {
        return -ENOSPC;
    }
    *offset = next_range;
    next_range += size;
    ranges_held++;
    return 0;
}

void i915_ggtt_release(struct drm_i915_private *i915, uint64_t offset)
{
    (void)i915;
    (void)offset;
    ranges_held--;
}

/* GEM's part of a reset, i915_fence.c's and i915_vma.c's, counted. */
static int fences_restored;

void i915_gem_restore_fences(struct drm_i915_private *i915)
{
    (void)i915;
    fences_restored++;
}

/** Nothing: execbuffer, which fills the pool of shadow batches, is the exec
 * test's. No frame is freed. */
uint32_t i915_gem_pool_trim(struct drm_i915_private *i915, int how)
{
    (void)i915;
    (void)how;
    return 0;
}

void i915_vma_revoke_mmap(struct i915_vma *vma)
{
    (void)vma;
}

/* The id table: the Lenovo's chip alone. */
static const struct i915_platform hsw = {"HASWELL", 7, 50, 7, 2, 0xf, 1, 1, 1, 0, 1, 31, 40};
static const struct i915_id lenovo_id = {0x0A16, 1, 0, &hsw};

/* A fault the row arms: the submission that fails after `submit_after`
 * more pass, when it is not negative. */
static int submit_after = -1;
static struct i915_hw_ops test_ops;

/** i915_hw.h's fault hook: -EIO for the submission the row armed. */
static int32_t test_fault(struct i915_hw *hw, int point)
{
    (void)hw;
    if (point != I915_FAULT_SUBMIT || submit_after < 0) {
        return 0;
    }
    if (submit_after-- == 0) {
        return -EIO;
    }
    return 0;
}

/** Whether the log holds `text`. */
static int logged(const char *text)
{
    return strstr(log_text, text) != 0;
}

/* ---- the device ---- */

/** The model's clock moved: the processor takes the messages due. */
static void on_delay(struct i915_model *m)
{
    take_messages(m);
}

static struct drm_file file_a, file_b;

/**
 * A fresh model and device brought up as the bind does up to the rings,
 * the GT parked, a process with no privilege calling, and a file open.
 */
static struct drm_i915_private *fresh(struct i915_model *m)
{
    int i;

    CHECK("the model is made", i915_model_init(m) == 0);
    bus = m;
    m->on_delay = on_delay;
    i915_model_irq_state(m)->dma = test_dma;
    memset(&dev, 0, sizeof(dev));
    dev.drm.driver_priv = &dev;
    dev.pci = m->pci;
    dev.hw = m->hw;
    test_ops = *m->hw.ops;
    test_ops.fault = test_fault;
    dev.hw.ops = &test_ops;
    dev.id = &lenovo_id;
    dev.revision = 0x0b;
    dev.mmio_phys = LENOVO_GPU_BAR0;
    dev.gmadr_phys = LENOVO_GPU_BAR2;
    dev.gmch_ctrl = (uint16_t)i915_model_config(m, SNB_GMCH_CTRL, 2);
    dev.bsm = i915_model_config(m, INTEL_BSM, 4);
    dev.msi_vector = 48;
    log_text[0] = 0;
    irq_depth = 0;
    irq_on = 1;
    for (i = 0; i < npages; i++) {
        pages[i].host = 0;
    }
    npages = pages_live = 0;
    next_phys = 0x50000000ull;
    next_range = 0x1000000u;
    ranges_held = ranges_refuse = 0;
    submit_after = -1;
    parks = unparks = fences_restored = 0;
    memset(&caller, 0, sizeof(caller));
    caller.euid = 1000;
    caller.pid = 42;
    memcpy(caller.comm, "glxgears", 9);
    cpu_table[0].current = &caller;
    sleeps = stuck_sleeps = 0;
    i915_uncore_init(&dev);
    CHECK("the GTT comes up", i915_ggtt_init(&dev) == 0);
    i915_irq_reset(&dev);
    i915_irq_install(&dev);
    CHECK("the rings start", i915_gt_init(&dev) == 0);
    memset(&file_a, 0, sizeof(file_a));
    memset(&file_b, 0, sizeof(file_b));
    file_a.dev = file_b.dev = &dev.drm;
    CHECK("a file opens", i915_driver_open(&dev.drm, &file_a) == 0);
    CHECK("another file opens", i915_driver_open(&dev.drm, &file_b) == 0);
    return &dev;
}

/** Close the files, unplug and stop the rings, and take the model off. */
static void done(struct i915_model *m)
{
    i915_driver_postclose(&dev.drm, &file_a);
    i915_driver_postclose(&dev.drm, &file_b);
    i915_requests_unplug(&dev);
    i915_gt_fini(&dev);
    CHECK("done: every context image and range given back", ranges_held == 0);
    i915_model_free(m);
    bus = 0;
}

/** Move the model's clock `ms` milliseconds. */
static void run_ms(uint32_t ms)
{
    while (ms--) {
        i915_delay_us(&dev.hw, 1000);
    }
}

/** The state of `file`'s context `id` on engine slot `idx`, with a
 * reference, or NULL. */
static struct intel_context *engine_of(struct drm_file *file, uint32_t id, uint32_t idx)
{
    struct i915_gem_context *ctx;
    struct intel_context *ce;

    if (i915_gem_context_lookup(file, id, &ctx) < 0) {
        return 0;
    }
    ce = i915_gem_context_engine(ctx, idx);
    i915_gem_context_put(ctx);
    return ce;
}

/** A request on `ce` that loads `value` into `reg` when `reg` is not 0;
 * NULL when it could not be made. */
static struct i915_request *submit(struct intel_context *ce, uint32_t reg, uint32_t value)
{
    struct i915_request *rq = 0;
    uint32_t *cs;

    if (i915_request_create(ce, &rq) < 0) {
        return 0;
    }
    if (reg) {
        cs = i915_request_begin(rq, 4);
        if (cs) {
            cs[0] = MI_LOAD_REGISTER_IMM(1);
            cs[1] = reg;
            cs[2] = value;
            cs[3] = MI_NOOP;
        }
    }
    if (i915_request_add(rq) < 0) {
        i915_request_put(rq);
        return 0;
    }
    return rq;
}

/** The index of the first `want` in the ring of `e` between the offsets
 * `from` and `to`, or -1. */
static long ring_find(const struct i915_engine *e, uint32_t from, uint32_t to, uint32_t want)
{
    uint32_t at;

    for (at = from; at != to; at = (at + 4u) & (e->ring_size - 1u)) {
        if (e->ring[at / 4u] == want) {
            return (long)at;
        }
    }
    return -1;
}

/** Make a context with CONTEXT_CREATE and return its id, or 0. */
static uint32_t make_context(struct drm_file *file)
{
    struct drm_i915_gem_context_create_ext c;

    memset(&c, 0, sizeof(c));
    return i915_gem_context_create_ioctl(&dev.drm, &c, file) == 0 ? c.ctx_id : 0;
}

/** CONTEXT_SETPARAM of `param` on `id` with `size` and `value`. */
static int32_t ctx_set(uint32_t id, uint64_t param, uint32_t size, uint64_t value)
{
    struct drm_i915_gem_context_param p;

    memset(&p, 0, sizeof(p));
    p.ctx_id = id;
    p.param = param;
    p.size = size;
    p.value = value;
    return i915_gem_context_setparam_ioctl(&dev.drm, &p, &file_a);
}

/* ---- 78-5a.1: hardware contexts ---- */

/** 78-5a.1: the images, the default state, the switch and its flags. */
#define file_priv(f) ((struct i915_file *)(f)->driver_priv)

/** fresh() with the GT's contexts made and the default state recorded by
 * a first request, as before any hang a row makes. */
static struct drm_i915_private *ready(struct i915_model *m)
{
    struct drm_i915_private *i915 = fresh(m);
    struct intel_context *ce = engine_of(&file_a, 0, I915_RCS0);
    struct i915_request *rq = ce ? submit(ce, 0, 0) : 0;

    CHECK("the default state recorded", rq && i915_request_wait(rq, 0, 100000000) == 0);
    if (rq) {
        i915_request_put(rq);
    }
    intel_context_put(ce);
    return i915;
}

/* A register no block of the model claims, which a request's payload
 * loads, so that a row sees whether the payload ran. */
#define TEST_REG 0x7300u

/** Run the model and the heartbeat, 10 ms at a time, until a beat finds an
 * engine hung or `limit_ms` pass: the milliseconds run, with the engines
 * the beat found in `*hung`. */
static uint32_t beat_until_hung(uint32_t limit_ms, uint32_t *hung)
{
    uint32_t ms;

    *hung = 0;
    for (ms = 0; ms < limit_ms && !*hung; ms += 10) {
        run_ms(10);
        *hung = i915_hangcheck(&dev);
    }
    return ms;
}

/** GET_RESET_STATS of `file`'s context `id`, with privilege. */
static struct drm_i915_reset_stats stats_of(struct drm_file *file, uint32_t id)
{
    struct drm_i915_reset_stats rs;
    uint32_t euid = caller.euid;

    memset(&rs, 0, sizeof(rs));
    rs.ctx_id = id;
    caller.euid = 0;
    if (i915_gem_context_reset_stats_ioctl(&dev.drm, &rs, file) < 0) {
        rs.reset_count = rs.batch_active = rs.batch_pending = 0xffffffffu;
    }
    caller.euid = euid;
    return rs;
}

/** Whether `rq` has completed with the fence error `err`. */
static int ended(const struct i915_request *rq, int err)
{
    return rq && rq->fence.base.signaled && rq->fence.base.error == err;
}

/* ---- 78-6a.1 and 78-6a.2: a hang and the engine's reset ---- */

/** 78-6a.1 and .2: a stuck request found by the heartbeat within Linux's
 * period, its engine reset, its payload skipped, and the request behind it
 * run. */
static void row_engine_reset(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = ready(&m);
    struct i915_engine *rcs = &i915->gt.engine[I915_RCS0];
    struct i915_model_ring *rr = i915_model_ring_state(&m, 0);
    struct i915_model_reset *rs = i915_model_reset_state(&m);
    uint32_t ida = make_context(&file_a), idb = make_context(&file_b), hung, run;
    struct intel_context *a = engine_of(&file_a, ida, I915_RCS0);
    struct intel_context *b = engine_of(&file_b, idb, I915_RCS0);
    struct intel_context *blt = engine_of(&file_b, idb, I915_BCS0);
    struct i915_request *rq, *rqa, *rqb, *rqc;
    struct drm_i915_reset_stats sa, sb;
    uint32_t a_head, a_infix, a_postfix;

    CHECK("engine: the GT parked, the heartbeat finds nothing",
          beat_until_hung(10000, &hung) == 10000 && hung == 0);
    rq = submit(a, 0, 0);
    CHECK("engine: a request that completes", rq && i915_request_wait(rq, 0, 100000000) == 0);
    i915_request_put(rq);
    CHECK("engine: a busy engine that completes its work is not hung",
          beat_until_hung(10000, &hung) == 10000 && hung == 0);

    rr->stuck = 1;
    rqa = submit(a, TEST_REG, 0xa);
    rqb = submit(b, TEST_REG, 0xb);
    rqc = submit(blt, 0, 0);
    a_head = rqa ? rqa->head : 0;
    a_infix = rqa ? rqa->infix : 0;
    a_postfix = rqa ? rqa->postfix : 0;
    run = beat_until_hung(10000, &hung);
    CHECK("engine: a stuck request is hung between Linux's 5 and 7.5 s",
          hung == 1u && run >= 5000 && run <= 7500);
    CHECK("engine: the heartbeat says why",
          logged("hangcheck: rcs0: no heartbeat in 2500 ms: breadcrumb ") &&
              logged("reset: rcs0 for stopped heartbeat on rcs0"));
    CHECK("engine: the render domain reset, twice, as Linux asks for it",
          rs->writes == 2 && rs->resets[1] == 2 && rs->resets[0] == 0 && rs->resets[3] == 0 &&
              i915_model_peek(&m, GEN6_GDRST) == 0);
    CHECK("engine: the engine's reset counted, the GPU's not",
          i915->gt.reset_engine_count[I915_ENGINE_CLASS_RENDER] == 1 &&
              i915->gt.reset_engine_count[I915_ENGINE_CLASS_COPY] == 0 &&
              i915->gt.reset_count == 0 && fences_restored == 0);
    CHECK("engine: the guilty request's payload skipped",
          rqa && rqa->fence.base.error == -EIO && !rqa->fence.base.signaled &&
              a_infix < a_postfix &&
              ring_find(rcs, a_infix, a_postfix, MI_LOAD_REGISTER_IMM(1)) < 0);
    CHECK("engine: the ring restarted at the guilty request, the interrupt unmasked again",
          !rr->stuck && rcs->running && rcs->head == a_head &&
              i915_model_peek(&m, RING_IMR(RENDER_RING_BASE)) == ~rcs->irq_bit);
    run_ms(1);
    CHECK("engine: the guilty request's breadcrumb ran, its fence failed with EIO",
          ended(rqa, -EIO));
    CHECK("engine: the request behind it ran, and only its payload",
          ended(rqb, 0) && i915_model_peek(&m, TEST_REG) == 0xb && !rr->faulted);
    CHECK("engine: the blitter's request ran untouched", ended(rqc, 0));
    sa = stats_of(&file_a, ida);
    sb = stats_of(&file_b, idb);
    CHECK("engine: GET_RESET_STATS counts the guilty context alone",
          sa.batch_active == 1 && sa.batch_pending == 0 && sb.batch_active == 0 &&
              sb.batch_pending == 0 && sa.reset_count == 0);
    CHECK("engine: a recoverable context is not banned at its first hang",
          !a->banned && file_priv(&file_a)->ban_score == 0 &&
              logged("reset: glxgears[42] context reset due to GPU hang"));
    i915_request_put(rqa);
    i915_request_put(rqb);
    i915_request_put(rqc);
    rq = submit(a, 0, 0);
    CHECK("engine: the engine runs on", rq && i915_request_wait(rq, 0, 100000000) == 0);
    i915_request_put(rq);
    intel_context_put(a);
    intel_context_put(b);
    intel_context_put(blt);
    CHECK("engine: no sleep reached its bound", stuck_sleeps == 0);
    done(&m);
}

/* ---- 84-13: the heartbeat with i915.enable_hangcheck off ---- */

/** 84-13.1 and .3, with rcs kept busy by a stuck request across a beat
 * that stopped every heartbeat and hangcheck on again: bcs going from idle
 * to busy gets its heartbeat back, as Linux's __engine_unpark restarts it,
 * is armed by the beat after, and its stuck request is reset, rcs left. */
static void restart_on_busy(struct i915_model *m, struct drm_i915_private *i915,
                            struct i915_request *rqa)
{
    struct i915_engine *rcs = &i915->gt.engine[I915_RCS0], *bcs = &i915->gt.engine[I915_BCS0];
    struct i915_model_ring *rr = i915_model_ring_state(m, 0), *br = i915_model_ring_state(m, 1);
    uint32_t idb = make_context(&file_b), hung, run;
    struct intel_context *blt = engine_of(&file_b, idb, I915_BCS0);
    struct i915_request *rqc;

    br->stuck = 1;
    rqc = submit(blt, 0, 0);
    CHECK("hangcheck on: an engine going busy gets its heartbeat back, the other not",
          rqc && i915->gt.hc_next != 0 && !bcs->hc_stopped && !bcs->hc_armed && rcs->hc_stopped);
    CHECK("hangcheck on: the beat after arms it and does not reset it",
          beat_until_hung(3000, &hung) == 3000 && hung == 0 && bcs->hc_armed &&
              bcs->hc_seqno == rqc->seqno && !rcs->hc_armed &&
              i915->gt.reset_engine_count[I915_ENGINE_CLASS_COPY] == 0);
    run = 3000u + beat_until_hung(10000, &hung);
    CHECK("hangcheck on: its stuck request is hung between Linux's 5 and 7.5 s",
          hung == 2u && run >= 5000u && run <= 7500u &&
              logged("reset: bcs0 for stopped heartbeat on bcs0"));
    CHECK("hangcheck on: the blitter alone reset",
          i915->gt.reset_engine_count[I915_ENGINE_CLASS_COPY] == 1 &&
              i915->gt.reset_engine_count[I915_ENGINE_CLASS_RENDER] == 0 && rr->stuck);
    run_ms(1);
    CHECK("hangcheck on: the blitter's request failed with EIO, the render one still stuck",
          ended(rqc, -EIO) && !br->stuck && !rqa->fence.base.signaled);
    i915_request_put(rqc);
    intel_context_put(blt);
}

/** 84-13.1 and .3: a beat with hangcheck off resets nothing and stops every
 * engine's heartbeat; turned on again, an engine kept busy all the while
 * has none until it next unparks, and another engine's comes back as it
 * goes busy (restart_on_busy). */
static void row_hangcheck_off(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = ready(&m);
    struct i915_engine *rcs = &i915->gt.engine[I915_RCS0], *bcs = &i915->gt.engine[I915_BCS0];
    struct i915_model_ring *rr = i915_model_ring_state(&m, 0);
    uint32_t ida = make_context(&file_a), hung;
    struct intel_context *a = engine_of(&file_a, ida, I915_RCS0);
    struct i915_request *rqa;

    rr->stuck = 1;
    rqa = submit(a, TEST_REG, 0xa);
    i915_hangcheck_enable(i915, 0);
    CHECK("hangcheck off: a beat resets nothing",
          rqa && i915->gt.hc_next != 0 && beat_until_hung(3000, &hung) == 3000 && hung == 0 &&
              i915->gt.reset_engine_count[I915_ENGINE_CLASS_RENDER] == 0);
    CHECK("hangcheck off: and stops every engine's heartbeat",
          i915->gt.hc_next == 0 && rcs->hc_stopped && bcs->hc_stopped && !rcs->hc_armed);
    CHECK("hangcheck off: the stuck request is not found",
          beat_until_hung(10000, &hung) == 10000 && hung == 0 && !rqa->fence.base.signaled &&
              i915->gt.awake && i915->gt.reset_engine_count[I915_ENGINE_CLASS_RENDER] == 0);
    i915_hangcheck_enable(i915, 1);
    CHECK("hangcheck on: an engine kept busy all the while has no heartbeat",
          i915->gt.hc_next == 0 && beat_until_hung(10000, &hung) == 10000 && hung == 0 &&
              rcs->hc_stopped && i915->gt.reset_engine_count[I915_ENGINE_CLASS_RENDER] == 0);
    restart_on_busy(&m, i915, rqa);
    i915_request_put(rqa);
    intel_context_put(a);
    done(&m);
}

/** 84-13.2 and .3: turning hangcheck on disarms an engine armed before, the
 * beat after arms it again without a reset, and a beat that finds hangcheck
 * off still resets an engine that missed its heartbeat before it stops, as
 * Linux's heartbeat judges the systole before it reads the parameter. */
static void row_hangcheck_rearm(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = ready(&m);
    struct i915_engine *rcs = &i915->gt.engine[I915_RCS0], *bcs = &i915->gt.engine[I915_BCS0];
    struct i915_model_ring *rr = i915_model_ring_state(&m, 0);
    uint32_t ida = make_context(&file_a), hung;
    struct intel_context *a = engine_of(&file_a, ida, I915_RCS0);
    struct i915_request *rqa;

    rr->stuck = 1;
    rqa = submit(a, TEST_REG, 0xa);
    CHECK("rearm: the first beat arms the busy engine",
          rqa && beat_until_hung(3000, &hung) == 3000 && hung == 0 && rcs->hc_armed);
    i915_hangcheck_enable(i915, 0);
    i915_hangcheck_enable(i915, 1);
    CHECK("rearm: turning hangcheck on disarms it", !rcs->hc_armed && i915->gt.hc_next != 0);
    CHECK("rearm: the beat after arms it again and does not reset it",
          beat_until_hung(2500, &hung) == 2500 && hung == 0 && rcs->hc_armed &&
              i915->gt.reset_engine_count[I915_ENGINE_CLASS_RENDER] == 0);
    i915_hangcheck_enable(i915, 0);
    (void)beat_until_hung(3000, &hung);
    CHECK("rearm: a beat with hangcheck off resets the engine that missed its heartbeat",
          hung == 1u && logged("reset: rcs0 for stopped heartbeat on rcs0") &&
              i915->gt.reset_engine_count[I915_ENGINE_CLASS_RENDER] == 1);
    CHECK("rearm: and then stops every engine's heartbeat",
          i915->gt.hc_next == 0 && rcs->hc_stopped && bcs->hc_stopped && !rcs->hc_armed);
    run_ms(1);
    CHECK("rearm: the guilty request failed with EIO", ended(rqa, -EIO));
    i915_hangcheck_enable(i915, 1);
    i915_request_put(rqa);
    intel_context_put(a);
    done(&m);
}

/* ---- 84-13b: an engine's first beat ---- */

/** 84-13b.1: with rcs kept busy and the GT awake, bcs going busy between
 * two beats is not armed by the first, which comes before an interval has
 * passed, as Linux's next_heartbeat queues the engine's first beat an
 * interval after its unpark; its stuck request is reset no sooner than
 * Linux's 5 s, within 7.5 s and a step of the beat. */
static void row_hangcheck_first(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = ready(&m);
    struct i915_engine *rcs = &i915->gt.engine[I915_RCS0], *bcs = &i915->gt.engine[I915_BCS0];
    struct i915_model_ring *rr = i915_model_ring_state(&m, 0), *br = i915_model_ring_state(&m, 1);
    uint32_t ida = make_context(&file_a), idb = make_context(&file_b), hung = 0, early = 0;
    struct intel_context *a = engine_of(&file_a, ida, I915_RCS0);
    struct intel_context *blt = engine_of(&file_b, idb, I915_BCS0);
    struct i915_request *rqa, *rqc;
    uint64_t t0, beat;

    rr->stuck = 1;
    t0 = rtc_ticks_now();
    rqa = submit(a, TEST_REG, 0xa);
    while (rqa && !rcs->hc_armed && rtc_ticks_now() < t0 + 10000u) {
        run_ms(10);
        hung |= i915_hangcheck(&dev);
    }
    run_ms(1000);
    CHECK("first beat: a beat armed the render engine, which keeps the GT awake",
          rqa && rcs->hc_armed && hung == 0 && i915->gt.awake);
    br->stuck = 1;
    t0 = rtc_ticks_now();
    rqc = submit(blt, 0, 0);
    beat = i915->gt.hc_next;
    CHECK("first beat: the blitter goes busy with the next beat less than an interval away",
          rqc && beat != 0 && beat < t0 + I915_HEARTBEAT_INTERVAL_MS && !bcs->hc_armed);
    while (rqc && rtc_ticks_now() + 10u < t0 + I915_HEARTBEAT_INTERVAL_MS) {
        run_ms(10);
        hung |= i915_hangcheck(&dev);
        early |= bcs->hc_armed;
    }
    CHECK("first beat: the beat within its first interval does not arm the blitter",
          i915->gt.hc_next != beat && !early && !(hung & 2u));
    while (rqc && !(hung & 2u) && rtc_ticks_now() < t0 + 10000u) {
        run_ms(10);
        hung |= i915_hangcheck(&dev);
    }
    CHECK("first beat: its stuck request is hung between Linux's 5 and 7.5 s",
          (hung & 2u) && rtc_ticks_now() >= t0 + 5000u && rtc_ticks_now() <= t0 + 7510u &&
              logged("reset: bcs0 for stopped heartbeat on bcs0"));
    run_ms(1);
    CHECK("first beat: the blitter's request failed with EIO", ended(rqc, -EIO));
    if (rqc) {
        i915_request_put(rqc);
    }
    if (rqa) {
        i915_request_put(rqa);
    }
    intel_context_put(a);
    intel_context_put(blt);
    done(&m);
}

/* ---- 78-6a.4: the error state ---- */

/** Check the error state `err` of a hang of the render ring, whose
 * requests on the chip were `rqa`, the guilty, and `rqb`. */
static void check_error_state(struct i915_model *m, const struct i915_gpu_error *err,
                              const struct i915_request *rqa, const struct i915_request *rqb)
{
    const struct i915_error_engine *ee = &err->engine[0];
    const struct i915_engine *rcs = &dev.gt.engine[I915_RCS0];

    CHECK("error: the reason and the message, with the guilty program",
          strstr(err->reason, "stopped heartbeat on rcs0") && err->engine_mask == 1u &&
              logged("hang: GPU HANG: ecode 7:1:") && logged(", in glxgears [42]") &&
              strstr(err->msg, "GPU HANG: ecode 7:1:") == err->msg);
    CHECK("error: the GPU's registers, read before they were cleared",
          err->pgtbl_er == 0x10u && i915_model_peek(m, PGTBL_ER) == 0 &&
              ee->fault_reg == (0x1000u | RING_FAULT_VALID) &&
              i915_model_peek(m, RING_FAULT_REG_RCS) == 0x1000u);
    CHECK("error: the hung engine and the other",
          ee->hung && !err->engine[1].hung && ee->start == rcs->ring_ggtt &&
              ee->hws_seqno + 1u == ee->rq[0].seqno && ee->seqno == rqb->seqno);
    CHECK("error: the requests on the chip, the guilty first",
          ee->nrequests == 2 && ee->rq[0].seqno == rqa->seqno &&
              ee->rq[0].ctx_id == rqa->ce->ctx->id && ee->rq[0].pid == 42 &&
              ee->rq[0].head == rqa->head && ee->rq[1].seqno == rqb->seqno &&
              ee->rq[1].ctx_id == rqb->ce->ctx->id);
    CHECK("error: the ring's dwords at the head the chip stopped at",
          ee->ring[0] == rcs->ring[(ee->head & HEAD_ADDR) / 4u]);
}

/** 78-6a.4: the state of a hang captured before the reset, the registers
 * cleared after it, and the first state kept until it is cleared. */
static void row_error_state(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = ready(&m);
    struct i915_model_ring *rr = i915_model_ring_state(&m, 0);
    uint32_t ida = make_context(&file_a), idb = make_context(&file_b), hung;
    struct intel_context *a = engine_of(&file_a, ida, I915_RCS0);
    struct intel_context *b = engine_of(&file_b, idb, I915_RCS0);
    struct i915_request *rq, *rqa, *rqb;
    const struct i915_gpu_error *err;

    i915_model_poke(&m, PGTBL_ER, 0x10u);
    i915_model_poke(&m, RING_FAULT_REG_RCS, 0x1000u | RING_FAULT_VALID);
    rr->stuck = 1;
    rqa = submit(a, TEST_REG, 0xa);
    rqb = submit(b, TEST_REG, 0xb);
    (void)beat_until_hung(10000, &hung);
    err = i915->gt.error;
    CHECK("error: the state kept", hung == 1u && err != 0 && rqa && rqb);
    if (err && rqa && rqb) {
        check_error_state(&m, err, rqa, rqb);
    }
    run_ms(1);
    i915_request_put(rqa);
    i915_request_put(rqb);

    rr->stuck = 1;
    rq = submit(b, 0, 0);
    (void)beat_until_hung(10000, &hung);
    run_ms(1);
    CHECK("error: a second hang is reset too", hung == 1u && ended(rq, -EIO));
    CHECK("error: the first state stays until it is read and cleared",
          i915->gt.error == err && i915->gt.reset_engine_count[I915_ENGINE_CLASS_RENDER] == 2);
    i915_request_put(rq);
    i915_gpu_error_clear(i915);
    CHECK("error: cleared", i915->gt.error == 0);
    intel_context_put(a);
    intel_context_put(b);
    done(&m);
}

/** 78-6a.1: a request's wait runs the heartbeat itself, so that a program
 * waiting on a hung request sees it end without the worker. */
static void row_wait_resets(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = ready(&m);
    struct i915_model_ring *rr = i915_model_ring_state(&m, 0);
    struct intel_context *r = engine_of(&file_a, 0, I915_RCS0);
    struct i915_request *rq;
    uint64_t t0;

    rr->stuck = 1;
    rq = submit(r, 0, 0);
    t0 = m.now_us;
    CHECK("wait: a wait for ever on a hung request ends after its reset",
          rq && i915_request_wait(rq, 0, -1) == 0 && ended(rq, -EIO) && m.now_us - t0 >= 5000000u &&
              m.now_us - t0 <= 7500000u);
    CHECK("wait: by the engine's reset", i915->gt.reset_engine_count[0] == 1);
    i915_request_put(rq);
    intel_context_put(r);
    done(&m);
}

/* ---- 78-6a.2: the whole GPU's reset and the wedge ---- */

/** 78-6a.2: an engine whose domain does not reset is reset with the whole
 * GPU, the other engine's request run again as innocent, the fences
 * restored. */
static void row_full_reset(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = ready(&m);
    struct i915_model_ring *rr = i915_model_ring_state(&m, 0), *br = i915_model_ring_state(&m, 1);
    struct i915_model_reset *rs = i915_model_reset_state(&m);
    uint32_t ida = make_context(&file_a), idb = make_context(&file_b), hung, run;
    struct intel_context *a = engine_of(&file_a, ida, I915_RCS0);
    struct intel_context *blt = engine_of(&file_b, idb, I915_BCS0);
    struct i915_request *rqa, *rqc;
    struct drm_i915_reset_stats sb;

    rs->stuck = GEN6_GRDOM_RENDER;
    rr->stuck = 1;
    rqa = submit(a, TEST_REG, 0xa);
    /* The blitter sticks after the first beat, so that its request is on
     * the chip, not yet hung, at the reset. */
    CHECK("full: the first beat finds nothing", beat_until_hung(3000, &hung) == 3000 && !hung);
    br->stuck = 1;
    rqc = submit(blt, 0, 0);
    run = 3000u + beat_until_hung(10000, &hung);
    CHECK("full: the render ring hung", hung == 1u && run >= 5000u && run <= 7500u);
    CHECK("full: its domain stuck, the whole GPU reset",
          logged("reset: rcs0: its domain did not reset: GDRST reads 0x00000002") &&
              logged("reset: the chip for stopped heartbeat on rcs0") && rs->writes == 3 &&
              rs->resets[1] == 0 && rs->resets[0] == 2);
    CHECK("full: counted as Linux counts", i915->gt.reset_count == 1 &&
                                               i915->gt.reset_engine_count[0] == 1 &&
                                               i915->gt.reset_engine_count[1] == 0);
    CHECK("full: the fences restored", fences_restored == 1);
    run_ms(1);
    CHECK("full: the guilty request failed", ended(rqa, -EIO));
    CHECK("full: the blitter's request ran again, innocent",
          ended(rqc, -EAGAIN) && !br->stuck && !br->faulted);
    sb = stats_of(&file_b, idb);
    CHECK("full: GET_RESET_STATS counts the GPU's reset and the innocent batch",
          sb.reset_count == 1 && sb.batch_active == 0 && sb.batch_pending == 1);
    CHECK("full: the blitter not reset again for its stale beat",
          i915->gt.reset_engine_count[1] == 0 && i915->gt.reset_count == 1);
    i915_request_put(rqa);
    i915_request_put(rqc);
    rs->stuck = 0;
    rqa = submit(a, TEST_REG, 0xc);
    CHECK("full: both rings run on",
          rqa && i915_request_wait(rqa, 0, 100000000) == 0 && i915_model_peek(&m, TEST_REG) == 0xc);
    i915_request_put(rqa);
    intel_context_put(a);
    intel_context_put(blt);
    done(&m);
}

/* ---- 78-6c.1: the workarounds, at the start and after each reset ---- */

/** Whether the render engine's workarounds stand in the model's registers
 * as Linux's rcs_engine_wa_init leaves them on Haswell. */
static int rcs_was_stand(struct i915_model *m)
{
    uint32_t mode = GFX_TLB_INVALIDATE_EXPLICIT | GFX_REPLAY_MODE;

    return (i915_model_peek(m, RING_MI_MODE(RENDER_RING_BASE)) & ASYNC_FLIP_PERF_DISABLE) &&
           (i915_model_peek(m, RING_MODE_GEN7(RENDER_RING_BASE)) & mode) == mode &&
           !(i915_model_peek(m, CACHE_MODE_0_GEN7) &
             (HIZ_RAW_STALL_OPT_DISABLE | RC_OP_FLUSH_ENABLE)) &&
           (i915_model_peek(m, CACHE_MODE_1) & PIXEL_SUBSPAN_COLLECT_OPT_DISABLE) &&
           (i915_model_peek(m, GEN7_GT_MODE) & GEN6_WIZ_HASHING_MASK) == GEN6_WIZ_HASHING_16x4 &&
           (i915_model_peek(m, HSW_HALF_SLICE_CHICKEN3) & HSW_SAMPLE_C_PERFORMANCE);
}

/** Whether the GT's state stands as Linux's intel_gt_init_hw leaves it on
 * a Haswell GT2. */
static int gt_was_stand(struct i915_model *m)
{
    return !(i915_model_peek(m, GEN7_FF_THREAD_MODE) & GEN7_FF_VS_REF_CNT_FFME) &&
           i915_model_peek(m, HSW_SCRATCH1) == HSW_SCRATCH1_L3_DATA_ATOMICS_DISABLE &&
           (i915_model_peek(m, HSW_ROW_CHICKEN3) & HSW_ROW_CHICKEN3_L3_GLOBAL_ATOMICS_DISABLE) &&
           i915_model_peek(m, HSW_MI_PREDICATE_RESULT_2) == LOWER_SLICE_DISABLED;
}

/** HSW_SCRATCH1 read as a chip that did not keep its write. */
static uint32_t scratch1_lost(struct i915_model *m, uint32_t reg, uint32_t value)
{
    (void)m;
    return reg == HSW_SCRATCH1 ? 0 : value;
}

/** 78-6c.1: the GT's and the render engine's workarounds written at the
 * start, the engine's again after its reset, both after the whole GPU's,
 * and one the chip does not keep logged and counted. */
static void row_workarounds(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = ready(&m);
    struct i915_model_ring *rr = i915_model_ring_state(&m, 0);
    struct i915_model_reset *rs = i915_model_reset_state(&m);
    struct intel_context *r = engine_of(&file_a, 0, I915_RCS0);
    struct i915_request *rq;
    uint32_t hung;

    CHECK("workarounds: the GT's and the render engine's stand after the start",
          gt_was_stand(&m) && rcs_was_stand(&m) && i915->gt.wa_lost == 0 &&
              logged("workarounds: applied: 3 of the GT and 6 of rcs0, 0 lost"));
    CHECK(
        "workarounds: the GT's two registers of the clock gating, the PPGTT's caching kept",
        (i915_model_peek(&m, GEN7_SQ_CHICKEN_MBCUNIT_CONFIG) & GEN7_SQ_CHICKEN_MBCUNIT_SQINTMOB) &&
            (i915_model_peek(&m, GAM_ECOCHK) & (HSW_ECOCHK_ARB_PRIO_SOL | ECOCHK_PPGTT_WB_HSW)) ==
                (HSW_ECOCHK_ARB_PRIO_SOL | ECOCHK_PPGTT_WB_HSW));

    rr->stuck = 1;
    rq = submit(r, 0, 0);
    (void)beat_until_hung(10000, &hung);
    run_ms(1);
    CHECK("workarounds: the render engine's written again after its reset",
          hung == 1u && rs->resets[1] == 2 && rs->resets[0] == 0 && ended(rq, -EIO) &&
              rcs_was_stand(&m) && gt_was_stand(&m));
    i915_request_put(rq);

    rs->stuck = GEN6_GRDOM_RENDER;
    rr->stuck = 1;
    rq = submit(r, 0, 0);
    (void)beat_until_hung(10000, &hung);
    run_ms(1);
    CHECK("workarounds: the GT's and the engine's written again after the whole GPU's reset",
          hung == 1u && i915->gt.reset_count == 1 && rs->resets[0] == 2 && ended(rq, -EIO) &&
              gt_was_stand(&m) && rcs_was_stand(&m) && i915->gt.wa_lost == 0);
    i915_request_put(rq);
    rs->stuck = 0;

    m.on_read = scratch1_lost;
    CHECK("workarounds: forcewake for the GT's", i915_forcewake_get(i915) == 0);
    i915_gt_init_hw(i915);
    i915_forcewake_put(i915);
    m.on_read = 0;
    CHECK("workarounds: one the chip did not keep is logged and counted",
          i915->gt.wa_lost == 1 &&
              logged("workarounds: gt: lost: register 0xb038 reads 0x00000000, expected "
                     "0x08000000 of 0xffffffff"));
    intel_context_put(r);
    done(&m);
}

/** 78-6a.2: a GPU that does not reset is wedged, every request failed,
 * and a later reset that succeeds lets it take work again. */
static void row_wedge(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = ready(&m);
    struct i915_model_ring *rr = i915_model_ring_state(&m, 0);
    struct i915_model_reset *rs = i915_model_reset_state(&m);
    struct intel_context *r = engine_of(&file_a, 0, I915_RCS0);
    struct intel_context *b = engine_of(&file_b, 0, I915_BCS0);
    struct i915_request *rq, *rq2, *rq3 = 0;
    uint32_t hung;

    rs->stuck = GEN6_GRDOM_RENDER | GEN6_GRDOM_FULL;
    rr->stuck = 1;
    rq = submit(r, 0, 0);
    rq2 = submit(r, 0, 0);
    (void)beat_until_hung(10000, &hung);
    CHECK("wedge: the engine's reset and four rounds of three of the GPU's tried",
          hung == 1u && rs->writes == 13 && rs->resets[0] == 0 && rs->resets[1] == 0 &&
              logged("reset: the chip did not reset: GDRST reads 0x00000001"));
    CHECK("wedge: wedged, every request failed with EIO, the GT parked",
          i915->gt.wedged && !i915->gt.wedged_terminal && ended(rq, -EIO) && ended(rq2, -EIO) &&
              !i915->gt.engine[0].requests && !i915->gt.awake &&
              logged("reset: wedged: the GPU takes no work until a reset succeeds"));
    CHECK("wedge: no request and no context is made",
          i915_request_create(b, &rq3) == -EIO && make_context(&file_a) == 0);
    CHECK("wedge: the heartbeat stops", beat_until_hung(5000, &hung) == 5000 && hung == 0);
    i915_request_put(rq);
    i915_request_put(rq2);
    rs->stuck = 0;
    i915_reset_gt(i915, 0, "a test");
    CHECK("wedge: a reset that succeeds ends the wedge",
          !i915->gt.wedged && i915->gt.reset_count == 2 && rs->resets[0] == 2 && !rr->stuck);
    rq = submit(r, TEST_REG, 0xd);
    rq2 = submit(b, 0, 0);
    CHECK("wedge: both rings take work again",
          rq && rq2 && i915_request_wait(rq, 0, 100000000) == 0 &&
              i915_request_wait(rq2, 0, 100000000) == 0 && i915_model_peek(&m, TEST_REG) == 0xd);
    i915_request_put(rq);
    i915_request_put(rq2);
    i915_requests_unplug(i915);
    i915_reset_gt(i915, 0, "a test");
    CHECK("wedge: the unplug's wedge is for good",
          i915->gt.wedged && i915->gt.reset_count == 2 && rs->resets[0] == 2);
    intel_context_put(r);
    intel_context_put(b);
    done(&m);
}

/**
 * A wedge with a request the chip was never given, which waits for another
 * driver's fence: its commands and its switch are taken back, so that the
 * reset that ends the wedge runs none of them.
 */
static void row_wedge_queued(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = ready(&m);
    struct i915_engine *e = &i915->gt.engine[I915_RCS0];
    struct i915_model_reset *rs = i915_model_reset_state(&m);
    struct intel_context *r = engine_of(&file_a, 0, I915_RCS0);
    struct intel_context *r2 = engine_of(&file_a, make_context(&file_a), I915_RCS0);
    struct i915_request *rq, *rq2 = 0;
    struct dma_fence f;
    uint32_t hung, *cs;
    int was_valid = 0;

    k_memset(&f, 0, sizeof(f));
    dma_fence_init(&f, 0, dma_fence_context_alloc(1), 1);
    rs->stuck = GEN6_GRDOM_RENDER | GEN6_GRDOM_FULL;
    i915_model_ring_state(&m, 0)->stuck = 1;
    rq = submit(r, 0, 0);
    if (r2 && i915_request_create(r2, &rq2) == 0) {
        cs = i915_request_begin(rq2, 4);
        if (cs) {
            cs[0] = MI_LOAD_REGISTER_IMM(1);
            cs[1] = TEST_REG;
            cs[2] = 0xbad;
            cs[3] = MI_NOOP;
        }
        (void)i915_request_await_fence(rq2, &f, 0);
        (void)i915_request_add(rq2);
        was_valid = rq2->was_valid;
    }
    CHECK("wedge: a request waits for another driver's fence, its switch emitted",
          rq && rq2 && !rq2->submitted && rq2->set_context && e->tail != e->hw_tail &&
              e->last_ce == r2);
    (void)beat_until_hung(10000, &hung);
    CHECK("wedge: its commands and switch taken back, the tail at what the chip has",
          i915->gt.wedged && ended(rq2, -EIO) && e->tail == e->hw_tail && e->reserved == 0 &&
              e->last_ce == r && r2->image_valid == was_valid);
    i915_request_put(rq);
    i915_request_put(rq2);
    rs->stuck = 0;
    i915_reset_gt(i915, 0, "a test");
    rq = submit(r, 0, 0);
    CHECK("wedge: after the reset the ring runs none of the cancelled commands",
          rq && i915_request_wait(rq, 0, 100000000) == 0 && i915_model_peek(&m, TEST_REG) != 0xbad);
    i915_request_put(rq);
    (void)dma_fence_signal(&f);
    intel_context_put(r);
    intel_context_put(r2);
    done(&m);
}

/**
 * A request that waits for another driver's fence that never signals runs
 * after Linux's 10 s, its fence's error ETIMEDOUT, and not before.
 */
static void row_fence_timeout(void)
{
    struct i915_model m;
    struct intel_context *r;
    struct i915_request *rq = 0;
    struct dma_fence f;

    (void)ready(&m);
    r = engine_of(&file_a, 0, I915_RCS0);
    k_memset(&f, 0, sizeof(f));
    dma_fence_init(&f, 0, dma_fence_context_alloc(1), 1);
    if (r && i915_request_create(r, &rq) == 0) {
        (void)i915_request_await_fence(rq, &f, 0);
        (void)i915_request_add(rq);
    }
    run_ms(9990);
    (void)i915_submit_work();
    CHECK("fence timeout: held back for 10 s by another driver's fence",
          rq && !rq->submitted && !rq->fence.base.error);
    run_ms(20);
    (void)i915_submit_work();
    CHECK("fence timeout: then given to the chip with ETIMEDOUT, and run",
          rq && rq->submitted && i915_request_wait(rq, 0, 100000000) == 0 && ended(rq, -ETIMEDOUT));
    (void)dma_fence_signal(&f);
    i915_request_put(rq);
    intel_context_put(r);
    done(&m);
}

/* ---- 78-6a.3: bans and counts ---- */

/** Hang the render ring with a request of `ce`, then run the heartbeat
 * until it is reset: the request, which the caller puts, or NULL. */
static struct i915_request *hang_once(struct intel_context *ce)
{
    struct i915_request *rq;
    uint32_t hung;

    i915_model_ring_state(bus, 0)->stuck = 1;
    rq = submit(ce, TEST_REG, 0xe);
    (void)beat_until_hung(10000, &hung);
    run_ms(1);
    return rq;
}

/** 78-6a.3: the third hang within 120 s bans a context, a context that is
 * not recoverable is banned at its first, an unbannable one never, and a
 * file whose score reaches Linux's limit makes no more contexts. */
static void row_ban(void)
{
    struct i915_model m;
    struct i915_file *fp;
    uint32_t ida, idc, idd, ide, idb, i, hung;
    struct intel_context *a, *c, *d, *e, *b;
    struct i915_request *rq, *rq2, *rq3;
    struct drm_i915_gem_context_destroy destroy;
    struct drm_i915_reset_stats s;
    int ok;

    (void)ready(&m);
    fp = file_priv(&file_a);
    idb = make_context(&file_b);
    b = engine_of(&file_b, idb, I915_RCS0);
    /* Unbannable: only with privilege. */
    idd = make_context(&file_a);
    CHECK("ban: an unbannable context needs privilege",
          ctx_set(idd, I915_CONTEXT_PARAM_BANNABLE, 0, 0) == -EPERM);
    caller.euid = 0;
    CHECK("ban: made unbannable", ctx_set(idd, I915_CONTEXT_PARAM_BANNABLE, 0, 0) == 0);
    caller.euid = 1000;
    d = engine_of(&file_a, idd, I915_RCS0);
    for (i = 0, ok = 1; i < 4; i++) {
        rq = hang_once(d);
        ok &= ended(rq, -EIO);
        i915_request_put(rq);
    }
    s = stats_of(&file_a, idd);
    CHECK("ban: an unbannable context is never banned, its hangs counted",
          ok && !d->banned && s.batch_active == 4 && fp->ban_score == 0 && fp->hang_ticks == 0);
    intel_context_put(d);

    /* Three hangs within 120 s. */
    ida = make_context(&file_a);
    a = engine_of(&file_a, ida, I915_RCS0);
    rq = hang_once(a);
    i915_request_put(rq);
    CHECK("ban: the first hang, not banned", !a->banned && fp->ban_score == 0);
    run_ms(10000);
    rq = hang_once(a);
    i915_request_put(rq);
    CHECK("ban: the second, within a minute of the first, scores 1",
          !a->banned && fp->ban_score == 1);
    i915_model_ring_state(&m, 0)->stuck = 1;
    rq = submit(a, TEST_REG, 0xa);
    rq2 = submit(b, TEST_REG, 0xb);
    rq3 = submit(a, TEST_REG, 0xf);
    (void)beat_until_hung(10000, &hung);
    run_ms(1);
    CHECK("ban: the third within 120 s bans the context, scoring 3 and 1",
          a->banned && fp->ban_score == 5 &&
              logged("reset: context glxgears[42]: guilty 3, banned"));
    CHECK("ban: its later request failed with EIO, another context's ran",
          ended(rq, -EIO) && ended(rq3, -EIO) && ended(rq2, 0) &&
              i915_model_peek(&m, TEST_REG) == 0xb);
    i915_request_put(rq);
    i915_request_put(rq2);
    i915_request_put(rq3);
    rq = submit(a, TEST_REG, 0x10);
    run_ms(1);
    CHECK("ban: a new request of the banned context fails with EIO, its payload skipped",
          ended(rq, -EIO) && i915_model_peek(&m, TEST_REG) == 0xb);
    i915_request_put(rq);
    s = stats_of(&file_a, ida);
    CHECK("ban: GET_RESET_STATS counts its three hangs", s.batch_active == 3);
    s = stats_of(&file_b, idb);
    CHECK("ban: and none for the other file's", s.batch_active == 0 && s.batch_pending == 0);
    intel_context_put(a);

    /* Not recoverable: banned at once. */
    idc = make_context(&file_a);
    CHECK("ban: made not recoverable", ctx_set(idc, I915_CONTEXT_PARAM_RECOVERABLE, 0, 0) == 0);
    c = engine_of(&file_a, idc, I915_RCS0);
    rq = hang_once(c);
    CHECK("ban: a context that is not recoverable is banned at its first hang",
          ended(rq, -EIO) && c->banned && fp->ban_score == 9);
    i915_request_put(rq);
    intel_context_put(c);
    CHECK("ban: a file with Linux's score of 9 makes no more contexts",
          make_context(&file_a) == 0 && make_context(&file_b) != 0);

    /* Closed: banned with no count. */
    ide = make_context(&file_b);
    e = engine_of(&file_b, ide, I915_RCS0);
    i915_model_ring_state(&m, 0)->stuck = 1;
    rq = submit(e, 0, 0);
    memset(&destroy, 0, sizeof(destroy));
    destroy.ctx_id = ide;
    CHECK("ban: a context with a request on the chip destroyed",
          i915_gem_context_destroy_ioctl(&dev.drm, &destroy, &file_b) == 0);
    (void)beat_until_hung(10000, &hung);
    run_ms(1);
    CHECK("ban: a closed context is banned at its hang, its file not scored",
          ended(rq, -EIO) && e->banned && file_priv(&file_b)->ban_score == 0);
    i915_request_put(rq);
    intel_context_put(e);
    intel_context_put(b);
    done(&m);
}

/** Nothing: one thread has no one to give way to. */
void sched_give_way(void)
{}

int main(void)
{
    row_engine_reset();
    row_error_state();
    row_wait_resets();
    row_full_reset();
    row_workarounds();
    row_wedge();
    row_wedge_queued();
    row_fence_timeout();
    row_ban();
    row_hangcheck_off();
    row_hangcheck_rearm();
    row_hangcheck_first();
    CHECK("the heap: every record freed", heap_live == 0);
    if (fails) {
        printf("i915-reset-test: %d failures\n", fails);
        return 1;
    }
    printf("i915-reset-test: ok\n");
    return 0;
}
