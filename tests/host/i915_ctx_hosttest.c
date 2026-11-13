/**
 * i915_ctx_hosttest.c: the Intel graphics driver's contexts, requests and
 * parameters, i915_gem_context.c, i915_request.c and i915_getparam.c, and
 * THROTTLE, against the host model of the Lenovo's HD 4400, whose rings run the
 * requests' commands from memory and whose render ring saves and restores
 * context images.
 *
 * host-deps: tests/host/compat/i915_time_host.c
 * host-deps: rach2/sys/drm/i915/i915_gem_context.c
 * host-deps: rach2/sys/drm/i915/i915_user_extensions.c
 * host-deps: rach2/sys/drm/i915/i915_request.c
 * host-deps: rach2/sys/drm/i915/i915_getparam.c
 * host-deps: rach2/sys/drm/i915/i915_reg_read.c
 * host-deps: rach2/sys/drm/i915/i915_hangcheck.c
 * host-deps: rach2/sys/drm/i915/i915_reset.c
 * host-deps: rach2/sys/drm/i915/i915_gpu_error.c
 * host-deps: rach2/sys/drm/i915/i915_ring.c rach2/sys/drm/i915/i915_irq.c
 * host-deps: rach2/sys/drm/i915/i915_ppgtt.c
 * host-deps: rach2/sys/drm/i915/i915_uncore.c rach2/sys/drm/i915/i915_power.c
 * host-deps: rach2/sys/drm/i915/i915_gtt.c rach2/sys/drm/i915/i915_hw.c
 * host-deps: rach2/sys/dmabuf/fence.c rach2/sys/tick.c
 * host-deps: tests/host/i915_model.c rach2/sys/lib/kstring.c
 * host-deps: rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/atomic_host.c
 * host-deps: tests/host/compat/i915_gem_lock_host.c
 * host-deps: tests/host/compat/mutex_none.c
 *
 * A row makes a fresh model and a device record over it, brought up as
 * the bind does up to the rings, and a file opened on it. The kernel
 * around the driver is a set of records, as in i915_ring_hosttest.c: pages
 * with physical addresses spread apart, which the model's rings reach back
 * through the test; the processor taking the device's messages whenever
 * the model's clock moves while its interrupts are on; a sleep moving the
 * clock a millisecond at a time; the clock in nanoseconds for fences; and
 * spinlocks whose release a row can follow with an interrupt.
 * A user address below 4096 faults. The GTT's allocator for the context
 * images is the test's, which counts what is held.
 *
 * The rows, one for each of 78-5a's subtasks: hardware contexts (the image
 * of 17 pages, the default state recorded with Linux's workarounds,
 * MI_SET_CONTEXT's flags, a context's registers saved and restored across
 * a switch, an image kept until the switch away retires); CONTEXT_CREATE
 * with its extensions, DESTROY, GETPARAM and SETPARAM with each of Linux's
 * refusals, and engine maps; requests (sequence numbers, the breadcrumbs,
 * fences signalled from the interrupt, retirement, the GT parked, waits
 * with timeouts, a lost interrupt, polling, refused commands taken back,
 * the unplug); 80-47's parking, when Linux's retirement would let the GT
 * park and not between a client's requests, with 80-50's signal of an
 * older request while a newer one is built and its listener that left
 * before the signal, a signal just after the retire worker takes its
 * notice, 80-55's merge and timeline point that ask for no signal, and
 * 80-61's callback asked for once the breadcrumb is written, which does
 * not count, and retirement before the interrupt, which parks the GT for
 * a request listened for while it ran;
 * GETPARAM and I915_QUERY row
 * by row; GET_RESET_STATS; 78-5c's THROTTLE, which IGT calls at each open;
 * and 78-11a.2's REG_READ of the render ring's timestamp, which the model
 * counts on at each read of a half.
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

/* The boot processor's tick count, which forcewake's release timer reads. */
volatile uint64_t kernel_ticks;

extern int printf(const char *format, ...);
extern int vsnprintf(char *s, unsigned long n, const char *format, va_list ap);
extern int posix_memalign(void **p, unsigned long align, unsigned long size);
extern void *calloc(unsigned long count, unsigned long size);
extern void *malloc(unsigned long size);
extern void free(void *p);
extern char *strstr(const char *s, const char *t);
extern unsigned long strlen(const char *s);
extern int snprintf(char *s, unsigned long n, const char *format, ...);

static int fails;

#define CHECK(what, cond)                              \
    do {                                               \
        if (!(cond)) {                                 \
            printf("i915-ctx-test: fail: %s\n", what); \
            fails++;                                   \
        }                                              \
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

/* Run once at the next copy from user memory, as another thread would run
 * while the copy sleeps on a page not yet in memory. */
static void (*copy_sleep)(void);

int copy_from_user(void *dst, const void __user *src, size_t n)
{
    void (*hook)(void) = copy_sleep;

    copy_sleep = 0;
    if (hook) {
        hook();
    }
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

/* A ring the row holds stuck until the next sleep begins, so that a wait
 * finds its request running past its spin; NULL for none. */
static struct i915_model_ring *unstick;

/** A sleep: the model's clock moves a millisecond at a time until `cond`
 * holds; one that never would ends after a minute of the model's clock.
 * The ring `unstick` names runs again first. */
int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    int steps = 0;

    (void)wq;
    sleeps++;
    if (unstick) {
        unstick->stuck = 0;
        unstick = 0;
    }
    while (!cond(arg)) {
        if (++steps > 60000) {
            stuck_sleeps++;
            return 0;
        }
        i915_delay_us(&dev.hw, 1000);
    }
    return 0;
}

/* Whether the next interruptible sleep ends at once with -EINTR, as one
 * a signal ends. */
static int intr_next;

int wq_wait_event(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    if (intr_next) {
        intr_next = 0;
        return -EINTR;
    }
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

/* No request here holds a buffer object: execbuffer, which hands them
 * over, is the exec test's. */
void i915_gem_object_put(struct drm_i915_gem_object *obj)
{
    (void)obj;
}

/** Nothing: execbuffer, which fills the pool of shadow batches, is the exec
 * test's. No frame is freed. */
uint32_t i915_gem_pool_trim(struct drm_i915_private *i915, int how)
{
    (void)i915;
    (void)how;
    return 0;
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

/* GEM's part of a reset, i915_fence.c's and i915_vma.c's, which no row
 * reaches. */
void i915_gem_restore_fences(struct drm_i915_private *i915)
{
    (void)i915;
}

void i915_vma_revoke_mmap(struct i915_vma *vma)
{
    (void)vma;
}

/* The id table: the Lenovo's chip alone. */
static const struct i915_platform hsw = {"HASWELL", 7, 50, 7, 2, 0xf, 1, 1, 1, 0, 1, 31, 40};
static const struct i915_id lenovo_id = {0x0A16, 1, 0, &hsw};

/* An interrupt the row arms at a lock's release: at the first release of
 * any lock after which `unlock_when` returns 1, the model's clock moves a
 * millisecond, taking the messages due as an interrupt there would;
 * `unlock_fired` counts those moves. */
static int (*unlock_when)(void);
static int unlock_fired;

/** Take `lock`; trap if it is already held, which on one processor is the
 * same path taking it twice, which on the machine spins for ever. */
void spin_lock(struct spinlock *lock)
{
    if (lock->locked) {
        __builtin_trap();
    }
    lock->locked = 1;
}

/** Release `lock`. */
void spin_unlock(struct spinlock *lock)
{
    lock->locked = 0;
}

/** Save the interrupt flag, take `lock`, and return the flag. */
uint32_t spin_lock_irqsave(struct spinlock *lock)
{
    uint32_t flags = kirq_save();

    spin_lock(lock);
    return flags;
}

/** Release `lock`, restore the interrupt flag `flags`, and take the
 * interrupt the row armed when its condition now holds. */
void spin_unlock_irqrestore(struct spinlock *lock, uint32_t flags)
{
    spin_unlock(lock);
    kirq_restore(flags);
    if (unlock_when && unlock_when()) {
        unlock_when = 0;
        unlock_fired++;
        i915_delay_us(&dev.hw, 1000);
    }
}

/* A fault the row arms: the submission that fails after `submit_after`
 * more pass, when it is not negative. `inject_after`, when not negative,
 * is the submissions that pass before the model's clock moves a
 * millisecond inside the next, taking the messages due as an interrupt
 * there would; `injected` counts those moves. */
static int submit_after = -1, inject_after = -1, injected;
static struct i915_hw_ops test_ops;

/** i915_hw.h's fault hook: the interrupt the row armed, then -EIO for the
 * submission the row armed. */
static int32_t test_fault(struct i915_hw *hw, int point)
{
    (void)hw;
    if (point != I915_FAULT_SUBMIT) {
        return 0;
    }
    if (inject_after >= 0 && inject_after-- == 0) {
        injected++;
        i915_delay_us(&dev.hw, 1000);
    }
    if (submit_after < 0) {
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

/** Whether the `n` bytes at `a` and `b` are the same. */
static int same_bytes(const void *a, const void *b, unsigned long n)
{
    const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;

    while (n--) {
        if (*x++ != *y++) {
            return 0;
        }
    }
    return 1;
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
    submit_after = inject_after = -1;
    injected = intr_next = 0;
    unlock_when = 0;
    unlock_fired = 0;
    unstick = 0;
    parks = unparks = 0;
    memset(&caller, 0, sizeof(caller));
    caller.euid = 1000;
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

/** Wait `timeout_ns` for `rq` on the ring `rr` as for a request still
 * running when the wait begins: the ring runs nothing until the wait
 * sleeps, so that its spin does not find the request done and it asks for
 * the signal. */
static int32_t wait_running(struct i915_model_ring *rr, struct i915_request *rq, int64_t timeout_ns)
{
    rr->stuck = 1;
    unstick = rr;
    return rq ? i915_request_wait(rq, 0, timeout_ns) : -EINVAL;
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

/** CONTEXT_GETPARAM of `param` on `id`: its error, the value in `*value`. */
static int32_t ctx_get(uint32_t id, uint64_t param, uint64_t *value)
{
    struct drm_i915_gem_context_param p;
    int32_t err;

    memset(&p, 0, sizeof(p));
    p.ctx_id = id;
    p.param = param;
    p.size = 7; /* written back as 0 */
    err = i915_gem_context_getparam_ioctl(&dev.drm, &p, &file_a);
    if (value) {
        *value = p.value;
    }
    if (err == 0 && param != I915_CONTEXT_PARAM_SSEU && p.size != 0) {
        return -1000;
    }
    return err;
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

/**
 * Whether the commands around the MI_SET_CONTEXT at byte `at` of `ring`
 * are Linux's mi_set_context on Haswell's four rings: arbitration off, an
 * MI_LOAD_REGISTER_IMM of the other three rings' RING_PSMI_CTL before it
 * and after it, the last read back by MI_STORE_REGISTER_MEM, and
 * arbitration on.
 */
static int around_set_context(const uint32_t *ring, long at)
{
    const uint32_t *c = ring + at / 4;

    return at >= 36 && c[-9] == (MI_ARB_ON_OFF | MI_ARB_DISABLE) &&
           c[-8] == MI_LOAD_REGISTER_IMM(3) && c[-7] == RING_PSMI_CTL(BLT_RING_BASE) &&
           c[-5] == RING_PSMI_CTL(GEN6_BSD_RING_BASE) && c[-3] == RING_PSMI_CTL(VEBOX_RING_BASE) &&
           c[3] == MI_LOAD_REGISTER_IMM(3) && c[8] == RING_PSMI_CTL(VEBOX_RING_BASE) &&
           c[10] == (MI_STORE_REGISTER_MEM | MI_SRM_LRM_GLOBAL_GTT) &&
           c[11] == RING_PSMI_CTL(VEBOX_RING_BASE) && c[14] == (MI_ARB_ON_OFF | MI_ARB_ENABLE);
}

/** 78-5a.1: the images, the default state, the switch and its flags. */
static void row_contexts(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m);
    struct i915_engine *rcs = &i915->gt.engine[I915_RCS0];
    struct i915_model_ring *ring = i915_model_ring_state(&m, 0);
    struct intel_context *a, *b, *bcs = 0;
    struct i915_request *rq, *rq2, *rq3;
    uint32_t ida, idb, a_ggtt, i, flags;
    long at;
    int ok;

    a = engine_of(&file_a, 0, I915_RCS0);
    CHECK("contexts: the default context, 0, has a state on each engine",
          a && a->engine == rcs && (bcs = engine_of(&file_a, 0, I915_BCS0)) != 0 &&
              bcs->engine == &i915->gt.engine[I915_BCS0]);
    intel_context_put(bcs);
    CHECK("contexts: no image before the first request, nothing made at open",
          a && !a->image && !i915->gt.ctx_ready && !i915->gt.scratch && ranges_held == 0);

    rq = submit(a, 0, 0);
    CHECK("contexts: the first request is made", rq != 0);
    CHECK("contexts: the scratch page, the GT's own states and the default state made",
          i915->gt.ctx_ready && i915->gt.scratch && rcs->kernel_ce &&
              i915->gt.engine[I915_BCS0].kernel_ce && i915->gt.default_state);
    CHECK("contexts: the default state recorded with its restore inhibited, then saved",
          ring->ctx.inhibited >= 2 && ring->ctx.saves >= 1 && !i915->gt.wedged &&
              i915->gt.default_state[0] == I915_MODEL_CTX_SAVED);
    CHECK("contexts: the default state holds Linux's context workarounds",
          (i915->gt.default_state[1] & INSTPM_FORCE_ORDERING) &&
              !(i915->gt.default_state[2] & RC_OP_FLUSH_ENABLE) &&
              (i915->gt.default_state[3] & PIXEL_SUBSPAN_COLLECT_OPT_DISABLE));
    ok = a && a->image != 0 && a->image_valid;
    for (i = 0; ok && i < I915_HSW_CXT_PAGES; i++) {
        uint64_t phys = paging_virt_to_phys((uintptr_t)a->image + i * 4096u);
        ok = phys && i915_ggtt_pte(i915, a->image_ggtt + i * 4096u) ==
                         i915_ggtt_pte_encode(phys, I915_CACHE_LLC);
    }
    CHECK("contexts: the image is 17 pages, each mapped at the LLC's level, from the default",
          ok && a->image[0] == I915_MODEL_CTX_SAVED);
    flags = MI_SAVE_EXT_STATE_EN | MI_MM_SPACE_GTT | MI_RESTORE_EXT_STATE_EN;
    at = rq ? ring_find(rcs, rq->head, rq->postfix, MI_SET_CONTEXT) : -1;
    CHECK("contexts: MI_SET_CONTEXT with the image, saved and restored with its extended state",
          at >= 0 && rcs->ring[at / 4 + 1] == (a->image_ggtt | flags) &&
              rcs->ring[at / 4 + 2] == MI_NOOP && rcs->ring[at / 4 - 1] == MI_NOOP);
    CHECK("contexts: arbitration off around it, and the other three engines' sleep messages "
          "held, the last one's read after as a delay",
          around_set_context(rcs->ring, at));
    CHECK("contexts: the request completes, its image restored",
          rq && i915_request_wait(rq, 0, 100000000) == 0 && !ring->faulted &&
              ring->ctx.restores >= 1 && rcs->last_ce == a);
    i915_request_put(rq);

    /* A context's registers follow it across a switch. */
    ida = make_context(&file_a);
    idb = make_context(&file_a);
    intel_context_put(a);
    a = engine_of(&file_a, ida, I915_RCS0);
    b = engine_of(&file_a, idb, I915_RCS0);
    rq = submit(a, CACHE_MODE_1, I915_MASKED_ENABLE(1u << 3));
    rq2 = submit(b, CACHE_MODE_1, I915_MASKED_DISABLE(1u << 3));
    CHECK("contexts: the second switch keeps the first context until it retires",
          rq2 && rq2->switched && rq2->prev_ce == a && rcs->last_ce == b);
    rq3 = submit(a, 0, 0);
    CHECK("contexts: three requests complete",
          rq3 && i915_request_wait(rq3, 0, 100000000) == 0 && !ring->faulted);
    CHECK("contexts: the first context's register restored when it is switched back",
          (i915_model_peek(&m, CACHE_MODE_1) & (1u << 3)) != 0);
    CHECK("contexts: and the second's saved without it",
          b->image[0] == I915_MODEL_CTX_SAVED && !(b->image[3] & (1u << 3)));
    i915_request_put(rq);
    i915_request_put(rq2);
    i915_request_put(rq3);

    /* An image the chip holds stays until the switch away retires. */
    a_ggtt = a->image_ggtt;
    {
        struct drm_i915_gem_context_destroy d = {0, 0};
        d.ctx_id = ida;
        CHECK("contexts: the context the chip holds is destroyed",
              i915_gem_context_destroy_ioctl(&dev.drm, &d, &file_a) == 0);
    }
    intel_context_put(a);
    CHECK("contexts: its image is still mapped while the chip holds it",
          rcs->last_ce && rcs->last_ce->image_ggtt == a_ggtt &&
              i915_ggtt_pte(i915, a_ggtt) != i915->ggtt.scratch_pte);
    rq = submit(b, 0, 0);
    CHECK("contexts: the switch away completes", rq && i915_request_wait(rq, 0, 100000000) == 0);
    i915_request_put(rq);
    i915_retire_requests(i915);
    CHECK("contexts: then the image is unmapped and its range given back",
          i915_ggtt_pte(i915, a_ggtt) == i915->ggtt.scratch_pte);
    CHECK("contexts: the blitter's states have no image",
          (bcs = engine_of(&file_a, idb, I915_BCS0)) != 0 && intel_context_pin_image(bcs) == 0 &&
              !bcs->image);
    intel_context_put(bcs);
    intel_context_put(b);
    CHECK("contexts: nothing waited to its bound", i915->hw.timeouts == 0 && stuck_sleeps == 0);
    done(&m);
}

/** A recording that the chip never finishes wedges the GPU. */
static void row_contexts_wedged(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m);
    struct intel_context *a = engine_of(&file_a, 0, I915_RCS0);
    struct i915_request *rq = 0;
    struct drm_i915_gem_context_create_ext c;

    i915_model_ring_state(&m, 0)->stuck = 1;
    CHECK("wedged: a recording the chip never finishes fails the request with EIO",
          i915_request_create(a, &rq) == -EIO && i915->gt.wedged && logged("wedged"));
    CHECK("wedged: and every request after it", i915_request_create(a, &rq) == -EIO);
    memset(&c, 0, sizeof(c));
    CHECK("wedged: CONTEXT_CREATE fails with EIO",
          i915_gem_context_create_ioctl(&dev.drm, &c, &file_a) == -EIO);
    i915_model_ring_state(&m, 0)->stuck = 0;
    intel_context_put(a);
    done(&m);
}

/* ---- 78-5a.2: the context ioctls ---- */

/* An extension of CONTEXT_CREATE: SETPARAM, as programs lay it out. */
static struct drm_i915_gem_context_create_ext_setparam ext[4];

/** CONTEXT_CREATE with `flags` and the extensions at `chain`: its error,
 * the new id in `*id`. */
static int32_t create_ext(uint32_t flags, uint64_t chain, uint32_t *id)
{
    struct drm_i915_gem_context_create_ext c;
    int32_t err;

    memset(&c, 0, sizeof(c));
    c.flags = flags;
    c.extensions = chain;
    err = i915_gem_context_create_ioctl(&dev.drm, &c, &file_a);
    if (id) {
        *id = c.ctx_id;
    }
    return err;
}

/** 78-5a.2: CONTEXT_CREATE, its extensions and DESTROY. */
static void row_create_destroy(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m);
    struct drm_i915_gem_context_destroy d;
    struct i915_file *fp;
    uint64_t v;
    uint32_t id, id1, id2, id3;

    id1 = make_context(&file_a);
    id2 = make_context(&file_a);
    id3 = make_context(&file_a);
    CHECK("create: ids from 1, the lowest free", id1 == 1 && id2 == 2 && id3 == 3);
    memset(&d, 0, sizeof(d));
    d.ctx_id = 2;
    CHECK("destroy: 2", i915_gem_context_destroy_ioctl(&dev.drm, &d, &file_a) == 0);
    CHECK("create: 2 again, the lowest free", make_context(&file_a) == 2);
    CHECK("destroy: twice is ENOENT",
          i915_gem_context_destroy_ioctl(&dev.drm, &d, &file_a) == 0 &&
              i915_gem_context_destroy_ioctl(&dev.drm, &d, &file_a) == -ENOENT);
    d.ctx_id = 0;
    CHECK("destroy: the default context is ENOENT",
          i915_gem_context_destroy_ioctl(&dev.drm, &d, &file_a) == -ENOENT);
    d.ctx_id = 77;
    CHECK("destroy: no such context is ENOENT",
          i915_gem_context_destroy_ioctl(&dev.drm, &d, &file_a) == -ENOENT);
    d.ctx_id = 1;
    d.pad = 1;
    CHECK("destroy: a pad is EINVAL",
          i915_gem_context_destroy_ioctl(&dev.drm, &d, &file_a) == -EINVAL);
    CHECK("create: an unknown flag is EINVAL", create_ext(1u << 2, 0, 0) == -EINVAL);
    CHECK("create: SINGLE_TIMELINE needs execlists: EINVAL",
          create_ext(I915_CONTEXT_CREATE_FLAGS_SINGLE_TIMELINE, 0, 0) == -EINVAL);
    CHECK("create: a context of another file is not this file's",
          i915_driver_open(&dev.drm, &file_b) == 0 && ctx_get(3, 0x3, &v) == 0 &&
              i915_gem_context_destroy_ioctl(&dev.drm, &d, &file_b) == -EINVAL);
    fp = (struct i915_file *)file_a.driver_priv;
    fp->ban_score = I915_CLIENT_SCORE_BANNED;
    CHECK("create: a banned file is EIO",
          make_context(&file_a) == 0 && create_ext(0, 0, 0) == -EIO);
    fp->ban_score = 0;
    /* The context calls run without the big lock, under the file's context
     * list lock, which each gives back on every path above. */
    CHECK("locks: each call, made or refused, gave the file's context list lock back",
          !mutex_held(&fp->lock) && !mutex_held(&((struct i915_file *)file_b.driver_priv)->lock));

    /* Extensions. */
    memset(ext, 0, sizeof(ext));
    ext[0].base.name = I915_CONTEXT_CREATE_EXT_SETPARAM;
    ext[0].param.param = I915_CONTEXT_PARAM_RECOVERABLE;
    ext[0].param.value = 0;
    ext[0].base.next_extension = (uint64_t)(uintptr_t)&ext[1];
    ext[1].base.name = I915_CONTEXT_CREATE_EXT_SETPARAM;
    ext[1].param.param = I915_CONTEXT_PARAM_NO_ERROR_CAPTURE;
    ext[1].param.value = 1;
    CHECK("create: SETPARAM extensions set the new context's parameters",
          create_ext(I915_CONTEXT_CREATE_FLAGS_USE_EXTENSIONS, (uint64_t)(uintptr_t)&ext[0], &id) ==
                  0 &&
              ctx_get(id, I915_CONTEXT_PARAM_RECOVERABLE, &v) == 0 && v == 0 &&
              ctx_get(id, I915_CONTEXT_PARAM_NO_ERROR_CAPTURE, &v) == 0 && v == 1);
    CHECK("create: extensions without the flag are not read",
          create_ext(0, BAD_USER, &id) == 0 && id != 0);
    ext[1].param.ctx_id = 1;
    CHECK("create: a SETPARAM with a context id is EINVAL",
          create_ext(I915_CONTEXT_CREATE_FLAGS_USE_EXTENSIONS, (uint64_t)(uintptr_t)&ext[0], 0) ==
              -EINVAL);
    ext[1].param.ctx_id = 0;
    ext[1].param.param = I915_CONTEXT_PARAM_PRIORITY;
    CHECK("create: a SETPARAM's refusal is the call's: PRIORITY is ENODEV",
          create_ext(I915_CONTEXT_CREATE_FLAGS_USE_EXTENSIONS, (uint64_t)(uintptr_t)&ext[0], 0) ==
              -ENODEV);
    ext[1].param.param = I915_CONTEXT_PARAM_NO_ERROR_CAPTURE;
    ext[1].base.name = I915_CONTEXT_CREATE_EXT_CLONE;
    CHECK("create: CLONE is EINVAL", create_ext(I915_CONTEXT_CREATE_FLAGS_USE_EXTENSIONS,
                                                (uint64_t)(uintptr_t)&ext[0], 0) == -EINVAL);
    ext[1].base.name = 9;
    CHECK("create: an unknown extension is EINVAL",
          create_ext(I915_CONTEXT_CREATE_FLAGS_USE_EXTENSIONS, (uint64_t)(uintptr_t)&ext[0], 0) ==
              -EINVAL);
    ext[1].base.name = I915_CONTEXT_CREATE_EXT_SETPARAM;
    ext[1].base.flags = 1;
    CHECK("create: an extension's flags are EINVAL",
          create_ext(I915_CONTEXT_CREATE_FLAGS_USE_EXTENSIONS, (uint64_t)(uintptr_t)&ext[0], 0) ==
              -EINVAL);
    ext[1].base.flags = 0;
    ext[1].base.rsvd[3] = 1;
    CHECK("create: an extension's reserved word is EINVAL",
          create_ext(I915_CONTEXT_CREATE_FLAGS_USE_EXTENSIONS, (uint64_t)(uintptr_t)&ext[0], 0) ==
              -EINVAL);
    ext[1].base.rsvd[3] = 0;
    ext[1].base.next_extension = BAD_USER;
    CHECK("create: an extension that cannot be read is EFAULT",
          create_ext(I915_CONTEXT_CREATE_FLAGS_USE_EXTENSIONS, (uint64_t)(uintptr_t)&ext[0], 0) ==
              -EFAULT);
    ext[1].base.next_extension = (uint64_t)(uintptr_t)&ext[1];
    CHECK("create: a chain past 512 extensions is E2BIG",
          create_ext(I915_CONTEXT_CREATE_FLAGS_USE_EXTENSIONS, (uint64_t)(uintptr_t)&ext[0], 0) ==
              -E2BIG);
    CHECK("create: the plain 8-byte call makes a context",
          make_context(&file_a) != 0 && i915->gt.wedged == 0);
    done(&m);
}

/** Whether SSEU through `p`, which points at `sseu`, answers no slices
 * and no EUs for the engine class `engine_class`. */
static int sseu_none(struct drm_i915_gem_context_param *p,
                     struct drm_i915_gem_context_param_sseu *sseu, uint16_t engine_class)
{
    sseu->engine.engine_class = engine_class;
    return i915_gem_context_getparam_ioctl(&dev.drm, p, &file_a) == 0 && sseu->slice_mask == 0 &&
           sseu->max_eus_per_subslice == 0;
}

/** 78-5a.2: CONTEXT_GETPARAM and SETPARAM, each parameter and refusal. */
static void row_params(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m);
    struct drm_i915_gem_context_param p;
    struct drm_i915_gem_context_param_sseu sseu;
    uint64_t v;
    uint32_t proto, fin;

    proto = make_context(&file_a);
    fin = make_context(&file_a);
    CHECK("getparam: GTT_SIZE is the global GTT's",
          ctx_get(fin, I915_CONTEXT_PARAM_GTT_SIZE, &v) == 0 && v == i915->ggtt.total);
    CHECK("getparam: bannable, recoverable and persistent by default, with error capture",
          ctx_get(fin, I915_CONTEXT_PARAM_BANNABLE, &v) == 0 && v == 1 &&
              ctx_get(fin, I915_CONTEXT_PARAM_RECOVERABLE, &v) == 0 && v == 1 &&
              ctx_get(fin, I915_CONTEXT_PARAM_PERSISTENCE, &v) == 0 && v == 1 &&
              ctx_get(fin, I915_CONTEXT_PARAM_NO_ERROR_CAPTURE, &v) == 0 && v == 0);
    CHECK("getparam: PRIORITY 0 and PROTECTED_CONTENT 0",
          ctx_get(fin, I915_CONTEXT_PARAM_PRIORITY, &v) == 0 && v == 0 &&
              ctx_get(fin, I915_CONTEXT_PARAM_PROTECTED_CONTENT, &v) == 0 && v == 0);
    CHECK("getparam: VM needs a full PPGTT: ENODEV",
          ctx_get(fin, I915_CONTEXT_PARAM_VM, 0) == -ENODEV);
    CHECK("getparam: BAN_PERIOD, NO_ZEROMAP, ENGINES, RINGSIZE, CONTEXT_IMAGE, LOW_LATENCY "
          "and unknown are EINVAL",
          ctx_get(fin, I915_CONTEXT_PARAM_BAN_PERIOD, 0) == -EINVAL &&
              ctx_get(fin, I915_CONTEXT_PARAM_NO_ZEROMAP, 0) == -EINVAL &&
              ctx_get(fin, I915_CONTEXT_PARAM_ENGINES, 0) == -EINVAL &&
              ctx_get(fin, I915_CONTEXT_PARAM_RINGSIZE, 0) == -EINVAL &&
              ctx_get(fin, I915_CONTEXT_PARAM_CONTEXT_IMAGE, 0) == -EINVAL &&
              ctx_get(fin, I915_CONTEXT_PARAM_LOW_LATENCY, 0) == -EINVAL &&
              ctx_get(fin, 0x99, 0) == -EINVAL);
    CHECK("getparam: no such context is ENOENT",
          ctx_get(99, I915_CONTEXT_PARAM_GTT_SIZE, 0) == -ENOENT);

    /* SSEU. */
    memset(&p, 0, sizeof(p));
    p.ctx_id = fin;
    p.param = I915_CONTEXT_PARAM_SSEU;
    CHECK("getparam: SSEU with a size of 0 gives the structure's",
          i915_gem_context_getparam_ioctl(&dev.drm, &p, &file_a) == 0 && p.size == 32);
    memset(&sseu, 0, sizeof(sseu));
    p.value = (uint64_t)(uintptr_t)&sseu;
    CHECK("getparam: SSEU of the render ring: one slice, two subslices, 10 EUs",
          i915_gem_context_getparam_ioctl(&dev.drm, &p, &file_a) == 0 && sseu.slice_mask == 1 &&
              sseu.subslice_mask == 3 && sseu.min_eus_per_subslice == 10 &&
              sseu.max_eus_per_subslice == 10);
    CHECK("getparam: SSEU of the blitter: none", sseu_none(&p, &sseu, I915_ENGINE_CLASS_COPY));
    CHECK("getparam: SSEU of the video ring: none", sseu_none(&p, &sseu, I915_ENGINE_CLASS_VIDEO));
    sseu.engine.engine_class = 4; /* I915_ENGINE_CLASS_COMPUTE */
    CHECK("getparam: SSEU of an engine that does not run is EINVAL",
          i915_gem_context_getparam_ioctl(&dev.drm, &p, &file_a) == -EINVAL);
    sseu.engine.engine_class = I915_ENGINE_CLASS_RENDER;
    sseu.flags = I915_CONTEXT_SSEU_FLAG_ENGINE_INDEX;
    CHECK("getparam: SSEU by index without an engine map is EINVAL",
          i915_gem_context_getparam_ioctl(&dev.drm, &p, &file_a) == -EINVAL);
    sseu.flags = 2;
    CHECK("getparam: SSEU's unknown flag is EINVAL",
          i915_gem_context_getparam_ioctl(&dev.drm, &p, &file_a) == -EINVAL);
    sseu.flags = 0;
    sseu.rsvd = 1;
    CHECK("getparam: SSEU's reserved word is EINVAL",
          i915_gem_context_getparam_ioctl(&dev.drm, &p, &file_a) == -EINVAL);
    sseu.rsvd = 0;
    p.size = 31;
    CHECK("getparam: SSEU short of the structure is EINVAL",
          i915_gem_context_getparam_ioctl(&dev.drm, &p, &file_a) == -EINVAL);
    p.size = 32;
    p.value = BAD_USER;
    CHECK("getparam: SSEU that cannot be read is EFAULT",
          i915_gem_context_getparam_ioctl(&dev.drm, &p, &file_a) == -EFAULT);

    /* SETPARAM, on a proto context and on a finalized one. */
    CHECK("setparam: RECOVERABLE off, then read back",
          ctx_set(proto, I915_CONTEXT_PARAM_RECOVERABLE, 0, 0) == 0 &&
              ctx_get(proto, I915_CONTEXT_PARAM_RECOVERABLE, &v) == 0 && v == 0 &&
              ctx_set(proto, I915_CONTEXT_PARAM_RECOVERABLE, 0, 1) == 0 &&
              ctx_get(proto, I915_CONTEXT_PARAM_RECOVERABLE, &v) == 0 && v == 1);
    proto = make_context(&file_a);
    CHECK("setparam: a size is EINVAL for each flag",
          ctx_set(proto, I915_CONTEXT_PARAM_RECOVERABLE, 4, 0) == -EINVAL &&
              ctx_set(proto, I915_CONTEXT_PARAM_BANNABLE, 4, 1) == -EINVAL &&
              ctx_set(proto, I915_CONTEXT_PARAM_NO_ERROR_CAPTURE, 4, 1) == -EINVAL &&
              ctx_set(proto, I915_CONTEXT_PARAM_PERSISTENCE, 4, 1) == -EINVAL &&
              ctx_set(fin, I915_CONTEXT_PARAM_RECOVERABLE, 4, 0) == -EINVAL);
    CHECK("setparam: BANNABLE off without privilege is EPERM",
          ctx_set(proto, I915_CONTEXT_PARAM_BANNABLE, 0, 0) == -EPERM &&
              ctx_set(fin, I915_CONTEXT_PARAM_BANNABLE, 0, 0) == -EPERM &&
              ctx_set(fin, I915_CONTEXT_PARAM_BANNABLE, 0, 1) == 0);
    caller.euid = 0;
    CHECK("setparam: BANNABLE off with privilege",
          ctx_set(fin, I915_CONTEXT_PARAM_BANNABLE, 0, 0) == 0 &&
              ctx_get(fin, I915_CONTEXT_PARAM_BANNABLE, &v) == 0 && v == 0);
    caller.euid = 1000;
    CHECK("setparam: PRIORITY needs a scheduler: ENODEV, EINVAL with a size",
          ctx_set(proto, I915_CONTEXT_PARAM_PRIORITY, 0, 0) == -ENODEV &&
              ctx_set(fin, I915_CONTEXT_PARAM_PRIORITY, 0, 1) == -ENODEV &&
              ctx_set(proto, I915_CONTEXT_PARAM_PRIORITY, 4, 0) == -EINVAL);
    CHECK("setparam: PERSISTENCE on, and off needs preemption: ENODEV",
          ctx_set(proto, I915_CONTEXT_PARAM_PERSISTENCE, 0, 1) == 0 &&
              ctx_set(proto, I915_CONTEXT_PARAM_PERSISTENCE, 0, 0) == -ENODEV &&
              ctx_set(fin, I915_CONTEXT_PARAM_PERSISTENCE, 0, 1) == 0 &&
              ctx_set(fin, I915_CONTEXT_PARAM_PERSISTENCE, 0, 0) == -ENODEV);
    CHECK("setparam: SSEU short is EINVAL, whole is ENODEV",
          ctx_set(proto, I915_CONTEXT_PARAM_SSEU, 31, 0) == -EINVAL &&
              ctx_set(proto, I915_CONTEXT_PARAM_SSEU, 32, 0) == -ENODEV &&
              ctx_set(fin, I915_CONTEXT_PARAM_SSEU, 32, 0) == -ENODEV);
    CHECK("setparam: VM on a proto context: EINVAL with a size, ENODEV without",
          ctx_set(proto, I915_CONTEXT_PARAM_VM, 4, 1) == -EINVAL &&
              ctx_set(proto, I915_CONTEXT_PARAM_VM, 0, 1) == -ENODEV);
    CHECK("setparam: PROTECTED_CONTENT on a proto context: on is ENODEV, off is taken",
          ctx_set(proto, I915_CONTEXT_PARAM_PROTECTED_CONTENT, 0, 1) == -ENODEV &&
              ctx_set(proto, I915_CONTEXT_PARAM_PROTECTED_CONTENT, 0, 0) == 0);
    CHECK("setparam: VM, ENGINES and PROTECTED_CONTENT after the first use are EINVAL",
          ctx_set(fin, I915_CONTEXT_PARAM_VM, 0, 1) == -EINVAL &&
              ctx_set(fin, I915_CONTEXT_PARAM_ENGINES, 8, 1) == -EINVAL &&
              ctx_set(fin, I915_CONTEXT_PARAM_PROTECTED_CONTENT, 0, 0) == -EINVAL);
    CHECK("setparam: GTT_SIZE, NO_ZEROMAP, BAN_PERIOD, RINGSIZE, CONTEXT_IMAGE and LOW_LATENCY "
          "are EINVAL",
          ctx_set(proto, I915_CONTEXT_PARAM_GTT_SIZE, 0, 1) == -EINVAL &&
              ctx_set(proto, I915_CONTEXT_PARAM_NO_ZEROMAP, 0, 1) == -EINVAL &&
              ctx_set(proto, I915_CONTEXT_PARAM_BAN_PERIOD, 0, 1) == -EINVAL &&
              ctx_set(proto, I915_CONTEXT_PARAM_RINGSIZE, 0, 4096) == -EINVAL &&
              ctx_set(fin, I915_CONTEXT_PARAM_CONTEXT_IMAGE, 0, 1) == -EINVAL &&
              ctx_set(proto, I915_CONTEXT_PARAM_LOW_LATENCY, 0, 1) == -EINVAL &&
              ctx_set(fin, I915_CONTEXT_PARAM_NO_ZEROMAP, 0, 1) == -EINVAL);
    CHECK("setparam: no such context is ENOENT",
          ctx_set(99, I915_CONTEXT_PARAM_RECOVERABLE, 0, 0) == -ENOENT);
    CHECK("setparam: NO_ERROR_CAPTURE on and off",
          ctx_set(fin, I915_CONTEXT_PARAM_NO_ERROR_CAPTURE, 0, 1) == 0 &&
              ctx_get(fin, I915_CONTEXT_PARAM_NO_ERROR_CAPTURE, &v) == 0 && v == 1 &&
              ctx_set(fin, I915_CONTEXT_PARAM_NO_ERROR_CAPTURE, 0, 0) == 0 &&
              ctx_get(fin, I915_CONTEXT_PARAM_NO_ERROR_CAPTURE, &v) == 0 && v == 0);
    done(&m);
}

/* An engine map as programs lay it out: the extensions, then the engines. */
static struct {
    uint64_t extensions;
    struct i915_engine_class_instance engines[66];
} map;

/* LOAD_BALANCE, BOND and PARALLEL_SUBMIT, as Linux lays them out. */
static uint8_t ext_buf[128];

/** Store the 16-bit `v` at byte `at` of the extension buffer. */
static void ext_put16(uint32_t at, uint16_t v)
{
    memcpy(ext_buf + at, &v, 2);
}

/** Set the first `n` engines of `map` as context `id`'s engine map. */
static int32_t set_map(uint32_t id, uint32_t n)
{
    return ctx_set(id, I915_CONTEXT_PARAM_ENGINES, I915_CONTEXT_PARAM_ENGINES_HEADER + 4u * n,
                   (uint64_t)(uintptr_t)&map);
}

/* The context another thread works on while a SETPARAM's copy sleeps. */
static uint32_t raced_id;

/** Another thread's CONTEXT_DESTROY of `raced_id`. */
static void destroy_raced(void)
{
    struct drm_i915_gem_context_destroy d = {0, 0};

    d.ctx_id = raced_id;
    (void)i915_gem_context_destroy_ioctl(&dev.drm, &d, &file_a);
}

/** Another thread's first use of `raced_id`, which finalizes it. */
static void use_raced(void)
{
    intel_context_put(engine_of(&file_a, raced_id, 0));
}

/** Another thread's ENGINES of one slot on `raced_id`. */
static void map_raced(void)
{
    (void)set_map(raced_id, 1);
}

/**
 * ENGINES whose copy sleeps while another thread destroys the context,
 * uses it or gives it a map: the context held across the copy, and no map
 * stored on a context that went, was finalized without one or has one.
 */
static void row_engines_raced(void)
{
    struct i915_model m;
    struct i915_gem_context *ctx = 0;

    (void)fresh(&m);
    memset(&map, 0, sizeof(map));
    map.engines[0].engine_class = I915_ENGINE_CLASS_RENDER;
    map.engines[1].engine_class = I915_ENGINE_CLASS_COPY;

    raced_id = make_context(&file_a);
    copy_sleep = destroy_raced;
    CHECK("engines: destroyed while the copy sleeps: ENOENT, the context freed after",
          set_map(raced_id, 2) == -ENOENT &&
              i915_gem_context_lookup(&file_a, raced_id, &ctx) == -ENOENT);
    raced_id = make_context(&file_a);
    copy_sleep = use_raced;
    CHECK("engines: finalized while the copy sleeps: EINVAL, the default engines kept",
          set_map(raced_id, 2) == -EINVAL &&
              i915_gem_context_lookup(&file_a, raced_id, &ctx) == 0 && ctx->nmap == -1 &&
              ctx->nce == I915_NUM_ENGINES);
    i915_gem_context_put(ctx);
    ctx = 0;
    raced_id = make_context(&file_a);
    copy_sleep = map_raced;
    CHECK("engines: given a map while the copy sleeps: EINVAL, the first map kept",
          set_map(raced_id, 2) == -EINVAL &&
              i915_gem_context_lookup(&file_a, raced_id, &ctx) == 0 && ctx->nmap == 1 &&
              ctx->nce == 1);
    i915_gem_context_put(ctx);
    done(&m);
}

/** 78-5a.2: engine maps and their extensions. */
static void row_engine_maps(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m);
    struct drm_i915_gem_context_param p;
    struct drm_i915_gem_context_param_sseu sseu;
    struct intel_context *ce;
    uint32_t id;
    int i;

    memset(&map, 0, sizeof(map));
    map.engines[0].engine_class = I915_ENGINE_CLASS_RENDER;
    map.engines[1].engine_class = I915_ENGINE_CLASS_INVALID;
    map.engines[1].engine_instance = I915_ENGINE_CLASS_INVALID_NONE;
    map.engines[2].engine_class = I915_ENGINE_CLASS_COPY;
    map.engines[3].engine_class = I915_ENGINE_CLASS_RENDER;
    id = make_context(&file_a);
    CHECK("engines: a map of render, a hole, copy and render again", set_map(id, 4) == 0);
    CHECK("engines: set twice is EINVAL", set_map(id, 4) == -EINVAL);
    ce = engine_of(&file_a, id, 0);
    CHECK("engines: slot 0 is the render ring's", ce && ce->engine == &i915->gt.engine[I915_RCS0]);
    intel_context_put(ce);
    CHECK("engines: slot 1 is empty", engine_of(&file_a, id, 1) == 0);
    ce = engine_of(&file_a, id, 2);
    CHECK("engines: slot 2 is the blitter's", ce && ce->engine == &i915->gt.engine[I915_BCS0]);
    intel_context_put(ce);
    {
        struct intel_context *a = engine_of(&file_a, id, 0), *b = engine_of(&file_a, id, 3);
        CHECK("engines: two slots of one engine are two states", a && b && a != b);
        intel_context_put(a);
        intel_context_put(b);
    }
    CHECK("engines: past the map is nothing", engine_of(&file_a, id, 4) == 0);
    memset(&p, 0, sizeof(p));
    memset(&sseu, 0, sizeof(sseu));
    p.ctx_id = id;
    p.param = I915_CONTEXT_PARAM_SSEU;
    p.size = sizeof(sseu);
    p.value = (uint64_t)(uintptr_t)&sseu;
    CHECK("engines: SSEU by class with a map is EINVAL",
          i915_gem_context_getparam_ioctl(&dev.drm, &p, &file_a) == -EINVAL);
    sseu.flags = I915_CONTEXT_SSEU_FLAG_ENGINE_INDEX;
    CHECK("engines: SSEU by index 0 is the render ring's",
          i915_gem_context_getparam_ioctl(&dev.drm, &p, &file_a) == 0 && sseu.slice_mask == 1);
    sseu.engine.engine_instance = 1;
    CHECK("engines: SSEU of the hole is EINVAL",
          i915_gem_context_getparam_ioctl(&dev.drm, &p, &file_a) == -EINVAL);

    id = make_context(&file_a);
    CHECK("engines: a size short of the header or not of whole engines is EINVAL",
          ctx_set(id, I915_CONTEXT_PARAM_ENGINES, 4, (uint64_t)(uintptr_t)&map) == -EINVAL &&
              ctx_set(id, I915_CONTEXT_PARAM_ENGINES, 10, (uint64_t)(uintptr_t)&map) == -EINVAL);
    CHECK("engines: more than 64 is EINVAL", set_map(id, 65) == -EINVAL);
    CHECK("engines: a map that cannot be read is EFAULT",
          ctx_set(id, I915_CONTEXT_PARAM_ENGINES, 12, BAD_USER) == -EFAULT);
    map.engines[1].engine_class = I915_ENGINE_CLASS_VIDEO;
    map.engines[1].engine_instance = 1;
    CHECK("engines: a second video ring, which Haswell lacks, is ENOENT",
          set_map(id, 2) == -ENOENT);
    map.engines[1].engine_instance = 0;
    map.engines[1].engine_class = I915_ENGINE_CLASS_COPY;
    CHECK("engines: an empty map is taken", set_map(id, 0) == 0 && engine_of(&file_a, id, 0) == 0);

    /* The extensions. */
    memset(ext_buf, 0, sizeof(ext_buf));
    map.extensions = (uint64_t)(uintptr_t)ext_buf;
    map.engines[0].engine_class = I915_ENGINE_CLASS_INVALID;
    map.engines[0].engine_instance = I915_ENGINE_CLASS_INVALID_NONE;
    ((struct i915_user_extension *)(void *)ext_buf)->name = I915_CONTEXT_ENGINES_EXT_LOAD_BALANCE;
    id = make_context(&file_a);
    CHECK("engines: LOAD_BALANCE needs execlists: ENODEV", set_map(id, 2) == -ENODEV);
    ((struct i915_user_extension *)(void *)ext_buf)->name = I915_CONTEXT_ENGINES_EXT_BOND;
    map.engines[0].engine_class = I915_ENGINE_CLASS_RENDER;
    map.engines[0].engine_instance = 0;
    ext_put16(36, 0); /* the virtual index */
    ext_put16(38, 1); /* one bond */
    memcpy(ext_buf + 80, &map.engines[1], 4);
    memcpy(ext_buf + 32, &map.engines[0], 4);
    CHECK("engines: BOND is checked and taken", set_map(id, 2) == 0);
    id = make_context(&file_a);
    ext_put16(36, 2);
    CHECK("engines: BOND past the map is EINVAL", set_map(id, 2) == -EINVAL);
    ext_put16(36, 0);
    ext_buf[32] = 4; /* I915_ENGINE_CLASS_COMPUTE */
    CHECK("engines: BOND with a master that does not run is EINVAL", set_map(id, 2) == -EINVAL);
    ext_buf[32] = I915_ENGINE_CLASS_RENDER;
    ext_buf[48] = 1;
    CHECK("engines: BOND's words that must be 0 are EINVAL", set_map(id, 2) == -EINVAL);
    ext_buf[48] = 0;

    memset(ext_buf, 0, sizeof(ext_buf));
    ((struct i915_user_extension *)(void *)ext_buf)->name =
        I915_CONTEXT_ENGINES_EXT_PARALLEL_SUBMIT;
    map.engines[1].engine_class = I915_ENGINE_CLASS_INVALID;
    map.engines[1].engine_instance = I915_ENGINE_CLASS_INVALID_NONE;
    ext_put16(32, 1); /* the slot */
    ext_put16(34, 2); /* the width */
    ext_put16(36, 1); /* one sibling */
    for (i = 0; i < 2; i++) {
        ext_put16(72 + 4 * i, I915_ENGINE_CLASS_COPY);
    }
    CHECK("engines: PARALLEL_SUBMIT of two blitters fails the logical mask's check: EINVAL",
          set_map(id, 2) == -EINVAL);
    ext_put16(36, 2);
    CHECK("engines: PARALLEL_SUBMIT with two siblings needs GuC: EINVAL",
          set_map(id, 2) == -EINVAL);
    ext_put16(36, 1);
    ext_put16(72, I915_ENGINE_CLASS_RENDER);
    CHECK("engines: PARALLEL_SUBMIT on the render ring is EINVAL", set_map(id, 2) == -EINVAL);
    map.extensions = 0;
    CHECK("engines: the context still takes a map after the refusals", set_map(id, 2) == 0);
    done(&m);
}

/* ---- 78-5a.3: requests ---- */

/** IGT's busy-create on the model: a retired request gives its ring's room
 * back, so that requests made behind one that never ends do not wait. */
static void row_ring_room(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m);
    struct i915_engine *bcs = &i915->gt.engine[I915_BCS0];
    struct intel_context *b = engine_of(&file_a, 0, I915_BCS0);
    struct i915_model_ring *br = i915_model_ring_state(&m, 1);
    struct i915_request *rq, *rq2;
    uint64_t t0;
    int i;

    /* IGT's busy-create: requests run and retire on the blitter until its
     * ring has wrapped, then one never ends, as a spinner, and the next
     * are made behind it without waiting for it. */
    rq = 0;
    for (i = 0; i < 26; i++) {
        i915_request_put(rq);
        rq = submit(b, 0, 0);
        if (!rq || i915_request_wait(rq, 0, 100000000) != 0) {
            break;
        }
    }
    i915_retire_requests(i915);
    CHECK("requests: a retired request gives back its room, the head at its end, as Linux's "
          "i915_request_retire",
          i == 26 && rq && bcs->head == rq->postfix && !bcs->requests);
    i915_request_put(rq);
    br->stuck = 1;
    rq = submit(b, 0, 0);
    t0 = m.now_us;
    for (i = 0; i < 20; i++) {
        rq2 = submit(b, 0, 0);
        if (!rq2) {
            break;
        }
        i915_request_put(rq2);
    }
    CHECK("requests: twenty made behind one that never ends, none waiting for it",
          rq && i == 20 && m.now_us == t0 && !i915_request_completed(rq));
    br->stuck = 0;
    CHECK("requests: and all of them run once it ends", rq && i915_request_wait(rq, 0, -1) == 0);
    i915_request_put(rq);
    intel_context_put(b);
    done(&m);
}

/** 78-5a.3: requests, their fences, retirement and waits. */
static void row_requests(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m);
    struct i915_engine *rcs = &i915->gt.engine[I915_RCS0], *bcs = &i915->gt.engine[I915_BCS0];
    struct intel_context *r = engine_of(&file_a, 0, I915_RCS0);
    struct intel_context *b = engine_of(&file_a, 0, I915_BCS0);
    struct i915_model_ring *rr = i915_model_ring_state(&m, 0), *br = i915_model_ring_state(&m, 1);
    struct i915_request *rq, *rq2;
    uint32_t seqno, tail;
    uint64_t t0;
    int i;

    rq = submit(r, 0, 0);
    CHECK("requests: the GT unparked with the first, after the default state's recording",
          rq && unparks == 2 && parks == 1 && i915->gt.awake);
    CHECK("requests: the fence on the engine's timeline, its seqno the breadcrumb's",
          rq && rq->fence.base.context == rcs->fence_context && rq->fence.base.seqno == rq->seqno &&
              rq->seqno == rcs->seqno && rq->fence.uabi_class == I915_ENGINE_CLASS_RENDER);
    CHECK("requests: the user interrupt unmasked while the engine has requests",
          rcs->irq_refs == 1 && rcs->requests == rq && rcs->requests_tail == rq);
    seqno = rq ? rq->seqno : 0;
    run_ms(1);
    CHECK("requests: the interrupt signals its fence, with no one waiting",
          rq && rq->fence.base.signaled && rq->fence.base.error == 0 &&
              rcs->hws[I915_GEM_HWS_SEQNO] == seqno);
    CHECK("requests: the render breadcrumb is a PIPE_CONTROL to the status page",
          rq && rcs->ring[rq->postfix / 4] == GFX_OP_PIPE_CONTROL(4) &&
              rcs->ring[rq->postfix / 4 + 2] == rcs->hws_ggtt + I915_GEM_HWS_SEQNO_ADDR &&
              rcs->ring[rq->postfix / 4 + 4] == MI_USER_INTERRUPT && rr->ctx.pipe_controls >= 3);
    i915_retire_requests(i915);
    CHECK("requests: retired, the interrupt kept armed until the GT parks, the GT awake",
          !rcs->requests && !rcs->requests_tail && rcs->irq_refs == 1 && i915->gt.awake &&
              parks == 1);
    i915_request_put(rq);

    rq = submit(r, 0, 0);
    rq2 = submit(r, 0, 0);
    CHECK("requests: sequence numbers follow each other on an engine",
          rq && rq2 && rq2->seqno == rq->seqno + 1u &&
              rq2->fence.base.context == rq->fence.base.context);
    CHECK("requests: both completed by the wait for the second",
          i915_request_wait(rq2, 0, 100000000) == 0 && i915_request_completed(rq));
    i915_request_put(rq);
    i915_request_put(rq2);

    rq = submit(b, 0, 0);
    CHECK("requests: the blitter's fence is on a timeline of its own",
          rq && rq->fence.base.context == bcs->fence_context &&
              bcs->fence_context != rcs->fence_context &&
              rq->fence.uabi_class == I915_ENGINE_CLASS_COPY);
    CHECK("requests: the blitter's breadcrumb, MI_FLUSH_DW and 32 stores",
          rq && rq->tail - rq->postfix == 4u * 104u &&
              bcs->ring[rq->postfix / 4] == (MI_FLUSH_DW | MI_INVALIDATE_TLB |
                                             MI_FLUSH_DW_OP_STOREDW | MI_FLUSH_DW_STORE_INDEX) &&
              bcs->ring[rq->postfix / 4 + 3] == MI_STORE_DWORD_INDEX);
    CHECK("requests: the blitter's completes", rq && i915_request_wait(rq, 0, 100000000) == 0 &&
                                                   bcs->hws[I915_GEM_HWS_SEQNO] == rq->seqno &&
                                                   br->ctx.flush_dws >= 3 && !br->faulted);
    i915_request_put(rq);

    /* Waits. */
    rr->stuck = 1;
    rq = submit(r, 0, 0);
    CHECK("requests: a wait of 0 on a request not done is ETIME",
          rq && i915_request_wait(rq, 0, 0) == -ETIME);
    t0 = m.now_us;
    CHECK("requests: a wait of 5 ms on a stuck ring is ETIME after 5 ms",
          rq && i915_request_wait(rq, 1, 5000000) == -ETIME && m.now_us - t0 >= 5000u &&
              m.now_us - t0 < 7000u);
    rr->stuck = 0;
    CHECK("requests: once the ring runs, a wait for ever completes",
          rq && i915_request_wait(rq, 1, -1) == 0);
    i915_request_put(rq);

    irq_on = 0;
    rq = submit(r, 0, 0);
    t0 = m.now_us;
    CHECK("requests: with the interrupt lost, the wait reads the page again within a second",
          rq && wait_running(rr, rq, -1) == 0 && m.now_us - t0 >= 1000000u &&
              m.now_us - t0 < 1100000u);
    i915_request_put(rq);
    irq_on = 1;
    i915->msi_vector = -1;
    rq = submit(r, 0, 0);
    t0 = m.now_us;
    CHECK("requests: without a message the breadcrumb is polled",
          rq && i915_request_wait(rq, 0, 100000000) == 0 && m.now_us - t0 < 1000u);
    i915_request_put(rq);
    i915->msi_vector = 48;

    /* Refused commands are taken back. */
    i915_retire_requests(i915);
    tail = rcs->tail;
    submit_after = 0;
    rq = 0;
    CHECK("requests: a ring that refuses the first commands fails the request with EIO",
          i915_request_create(r, &rq) == -EIO && rcs->tail == tail && rcs->reserved == 0);
    submit_after = 6; /* the invalidation, the directory's load in four, the switch */
    CHECK("requests: one that refuses the breadcrumb", i915_request_create(r, &rq) == 0);
    CHECK("requests: fails the add with EIO, the fence signalled with it, the ring as it was",
          i915_request_add(rq) == -EIO && rq->fence.base.signaled && rq->fence.base.error == -EIO &&
              rcs->tail == tail && !rcs->requests && rcs->last_ce == r);
    i915_request_put(rq);
    submit_after = -1;
    rq = submit(r, 0, 0);
    CHECK("requests: the next request runs", rq && i915_request_wait(rq, 0, 100000000) == 0);
    i915_request_put(rq);

    /* A hundred requests on both rings wrap the rings and retire. */
    for (i = 0; i < 100; i++) {
        rq = submit(i & 1 ? b : r, 0, 0);
        if (!rq || i915_request_wait(rq, 0, 100000000) != 0) {
            break;
        }
        i915_request_put(rq);
    }
    CHECK("requests: a hundred on both rings, past their ends",
          i == 100 && !rr->faulted && !br->faulted);

    /* The unplug fails what is left. */
    rr->stuck = 1;
    rq = submit(r, 0, 0);
    i915_requests_unplug(i915);
    CHECK("requests: the unplug fails a request not done with EIO",
          rq && rq->fence.base.signaled && rq->fence.base.error == -EIO && !rcs->requests &&
              rcs->irq_refs == 0 && !rcs->kernel_ce && !rcs->last_ce && !i915->gt.scratch &&
              !i915->gt.default_state);
    CHECK("requests: and no request is made after it", submit(r, 0, 0) == 0);
    i915_request_put(rq);
    intel_context_put(r);
    intel_context_put(b);
    rr->stuck = 0;
    CHECK("requests: no sleep reached its bound", stuck_sleeps == 0);
    done(&m);
}

/** A fence callback that does nothing, a listener as the flip's is. */
static void park_listener(struct dma_fence *fence, struct dma_fence_cb *cb)
{
    (void)fence;
    (void)cb;
}

/**
 * 80-47: the GT parks when Linux 6.12's would on Haswell, whose ring
 * submission gives each engine one timeline: once every engine's requests
 * are retired, which happens at once only when an engine's last request
 * signals to a listener, and otherwise at the retire worker's pass, on
 * the first whole second after a second from the unpark, as Linux's
 * round_jiffies_up_relative(HZ). Here, requests no one waits for: a
 * client's next request finds the GT awake, with no park and unpark
 * between them.
 */
static void row_park_unwatched(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m);
    struct i915_engine *rcs = &i915->gt.engine[I915_RCS0];
    struct intel_context *r = engine_of(&file_a, 0, I915_RCS0);
    struct i915_request *rq;
    uint64_t at;
    int n, p, u;

    rq = submit(r, 0, 0);
    run_ms(1);
    i915_retire_requests(i915);
    p = parks;
    u = unparks;
    CHECK("park: a request no one waits for retires, the engine held and the GT awake",
          rq && rq->fence.base.signaled && !rcs->requests && rcs->unretired && i915->gt.awake);
    i915_request_put(rq);
    rq = submit(r, 0, 0);
    CHECK("park: the next request finds the GT awake: no park and unpark between them",
          rq && parks == p && unparks == u && i915->gt.awake);
    run_ms(1);
    (void)i915_submit_work();
    CHECK("park: the retire worker does not pass before its second",
          i915->gt.awake && rcs->unretired);
    for (n = 0; n < 2100 && i915->gt.awake; n++) {
        run_ms(1);
        (void)i915_submit_work();
    }
    at = rtc_ticks_now();
    CHECK("park: its pass retires the engine and parks the GT",
          !i915->gt.awake && parks == p + 1 && !rcs->requests && !rcs->unretired);
    CHECK("park: on the first whole second after a second from the unpark",
          at % 1000u == 0 && at - i915->gt.unpark_ticks > 1000u &&
              at - i915->gt.unpark_ticks <= 2000u);
    i915_request_put(rq);
    intel_context_put(r);
    done(&m);
}

/** 80-47: a wait for an engine's last request lets the engine go once it
 * signals, and the retire worker's pass parks the GT (84-157). */
static void row_park_waited(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m);
    struct i915_engine *rcs = &i915->gt.engine[I915_RCS0];
    struct i915_model_ring *rr = i915_model_ring_state(&m, 0);
    struct intel_context *r = engine_of(&file_a, 0, I915_RCS0);
    struct i915_request *rq;
    int p;

    rq = submit(r, 0, 0);
    p = parks;
    CHECK("park: a wait for the engine's last request lets the engine go when it signals",
          rq && wait_running(rr, rq, 100000000) == 0 && rq->waiters == 0 && !rcs->unretired &&
              !rcs->requests && i915->gt.awake && parks == p);
    i915_gt_retire_requests(i915);
    CHECK("park: and the retire worker's pass parks the GT", !i915->gt.awake && parks == p + 1);
    i915_request_put(rq);
    intel_context_put(r);
    done(&m);
}

/** 80-47: a fence callback on an engine's last request lets the engine go
 * once it signals, and the retire worker's pass parks the GT (84-157); on
 * an earlier request, or with another engine's request unwatched, the
 * engine is held until the pass or the wait for idle. */
static void row_park_watched(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m);
    struct i915_engine *rcs = &i915->gt.engine[I915_RCS0], *bcs = &i915->gt.engine[I915_BCS0];
    struct i915_model_ring *rr = i915_model_ring_state(&m, 0);
    struct intel_context *r = engine_of(&file_a, 0, I915_RCS0);
    struct intel_context *b = engine_of(&file_a, 0, I915_BCS0);
    struct i915_request *rq, *rq2, *rq3;
    struct dma_fence_cb cb, cb2;
    int p;

    p = parks;
    rq = submit(r, 0, 0);
    rq2 = submit(r, 0, 0);
    CHECK("park: a callback on a request that is not the engine's last",
          rq && rq2 && dma_fence_add_callback(&rq->fence.base, &cb, park_listener) == 0);
    run_ms(1);
    (void)i915_submit_work();
    CHECK("park: keeps the engine held, the GT awake",
          rq2 && rq2->fence.base.signaled && rcs->unretired && i915->gt.awake && parks == p);
    rq3 = submit(r, 0, 0);
    CHECK("park: a callback on the last, as the flip's on its fence",
          rq3 && dma_fence_add_callback(&rq3->fence.base, &cb2, park_listener) == 0);
    run_ms(1);
    CHECK("park: its signal lets the engine go", !rcs->unretired && i915->gt.awake);
    (void)i915_submit_work();
    CHECK("park: and the worker retires it at once, the GT awake",
          i915->gt.awake && !rcs->requests && parks == p);
    i915_gt_retire_requests(i915);
    CHECK("park: until the worker's pass parks it", !i915->gt.awake && parks == p + 1);
    i915_request_put(rq);
    i915_request_put(rq2);
    i915_request_put(rq3);
    p = parks;
    rq = submit(b, 0, 0);
    rq2 = submit(r, 0, 0);
    CHECK("park: a wait for the render ring's last leaves the GT to the blitter's request",
          rq && rq2 && wait_running(rr, rq2, 100000000) == 0 && !rcs->unretired && bcs->unretired &&
              i915->gt.awake && parks == p);
    CHECK("park: which the wait for the GT's idle retires, parking it",
          i915_gt_wait_for_idle(i915, 0) == 0 && !bcs->unretired && !i915->gt.awake &&
              parks == p + 1);
    i915_request_put(rq);
    i915_request_put(rq2);
    intel_context_put(r);
    intel_context_put(b);
    done(&m);
}

/** 80-50: a wait whose request the chip finishes within the wait's first
 * 8 us asks for no signal, as Linux's optimistic spin, and the GT stays
 * awake to the worker's pass. */
static void row_park_spin(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m);
    struct i915_engine *rcs = &i915->gt.engine[I915_RCS0];
    struct intel_context *r = engine_of(&file_a, 0, I915_RCS0);
    struct i915_request *rq;
    int p, s;

    rq = submit(r, 0, 0);
    p = parks;
    s = sleeps;
    CHECK("spin: a wait for a request done within its spin returns without a sleep",
          rq && rq->submitted && i915_request_wait(rq, 0, 100000000) == 0 && sleeps == s);
    CHECK("spin: asks for no signal, the request retired, the engine held and the GT awake",
          rq && !rq->fence.base.listened && rq->fence.base.signaled && !rcs->requests &&
              rcs->unretired && i915->gt.awake && parks == p);
    i915_request_put(rq);
    intel_context_put(r);
    done(&m);
}

/**
 * 80-55: a merge of two engines' last requests, which a reservation's
 * snapshot makes, and a timeline point on a request, which SYNCOBJ_QUERY
 * asks after, ask neither request for its signal until someone waits, as
 * Linux's dma_fence_array and dma_fence_chain add their callbacks only
 * from enable_signaling; both requests, done, keep the GT awake to the
 * worker's pass.
 */
static void row_park_merge_query(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m);
    struct i915_engine *rcs = &i915->gt.engine[I915_RCS0], *bcs = &i915->gt.engine[I915_BCS0];
    struct intel_context *r = engine_of(&file_a, 0, I915_RCS0);
    struct intel_context *b = engine_of(&file_a, 0, I915_BCS0);
    struct i915_request *rq, *rq2;
    struct dma_fence *both[2], *f;
    struct dma_fence_chain *c = dma_fence_chain_alloc();
    int p;

    rq = submit(r, 0, 0);
    rq2 = submit(b, 0, 0);
    p = parks;
    both[0] = rq ? &rq->fence.base : 0;
    both[1] = rq2 ? &rq2->fence.base : 0;
    f = rq && rq2 ? dma_fence_merge(both, 2) : 0;
    CHECK("merge: a merge of two engines' requests, queried, asks neither for its signal",
          f && !dma_fence_is_signaled(f) && !rq->fence.base.listened && !rq2->fence.base.listened);
    dma_fence_put(f);
    if (c && rq) {
        dma_fence_chain_init(c, 0, dma_fence_get(&rq->fence.base), 1);
    }
    CHECK("merge: nor does a query of a timeline point on one",
          c && rq && !dma_fence_is_signaled(&c->base) && !rq->fence.base.listened);
    dma_fence_put(c ? &c->base : 0);
    run_ms(1);
    (void)i915_submit_work();
    i915_retire_requests(i915);
    CHECK("merge: both, done, keep the GT awake to the worker's pass",
          rq && rq2 && rq->fence.base.signaled && rq2->fence.base.signaled && rcs->unretired &&
              bcs->unretired && i915->gt.awake && parks == p);
    i915_request_put(rq);
    i915_request_put(rq2);
    intel_context_put(r);
    intel_context_put(b);
    done(&m);
}

/** 80-47: a request being built holds the GT awake past the retire
 * worker's pass, as Linux's request holds its engine from its creation. */
static void row_park_building(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m);
    struct i915_engine *rcs = &i915->gt.engine[I915_RCS0];
    struct i915_model_ring *rr = i915_model_ring_state(&m, 0);
    struct intel_context *r = engine_of(&file_a, 0, I915_RCS0);
    struct i915_request *rq = 0;
    int32_t err;
    int p;

    err = i915_request_create(r, &rq);
    p = parks;
    CHECK("park: a request being built",
          err == 0 && rcs->reserved != 0 && rcs->building && i915->gt.awake);
    run_ms(2001);
    (void)i915_submit_work();
    CHECK("park: holds the GT awake past the worker's pass", i915->gt.awake && parks == p);
    CHECK("park: and once added and waited for, lets it go",
          rq && i915_request_add(rq) == 0 && wait_running(rr, rq, 100000000) == 0 &&
              !rcs->unretired && i915->gt.awake && parks == p);
    i915_gt_retire_requests(i915);
    CHECK("park: for the worker's pass to park", !i915->gt.awake && parks == p + 1);
    i915_request_put(rq);
    intel_context_put(r);
    done(&m);
}

/** The release that ends the render ring's lock in request_new, whose
 * request is being built and has no room kept yet for its breadcrumb. */
static int creating_unlocked(void)
{
    struct i915_engine *rcs = &dev.gt.engine[I915_RCS0];

    return rcs->building && rcs->reserved == 0;
}

/**
 * 80-50: an older request that signals to a listener while the engine's
 * next request is being created, at once after request_new puts it on the
 * engine and before its breadcrumb's room is kept, leaves the engine held,
 * as on Linux, where the next request is on the timeline from its creation
 * under the timeline's mutex, and the older one is no longer its last; the
 * next, done with no listener, then keeps the GT awake to the worker's
 * pass.
 */
static void row_park_signal_creating(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m);
    struct i915_engine *rcs = &i915->gt.engine[I915_RCS0];
    struct intel_context *r = engine_of(&file_a, 0, I915_RCS0);
    struct i915_request *old, *rq = 0;
    struct dma_fence_cb cb;
    int32_t err;
    int p;

    old = submit(r, 0, 0);
    CHECK("creating: a callback on the engine's last request",
          old && dma_fence_add_callback(&old->fence.base, &cb, park_listener) == 0);
    p = parks;
    unlock_when = creating_unlocked;
    err = i915_request_create(r, &rq);
    CHECK("creating: its signal while the next is created leaves the engine held",
          err == 0 && unlock_fired == 1 && old && old->fence.base.signaled && rcs->unretired);
    CHECK("creating: the next is added", rq && i915_request_add(rq) == 0);
    run_ms(1);
    (void)i915_submit_work();
    i915_retire_requests(i915);
    CHECK("creating: done with no listener, it keeps the GT awake to the worker's pass",
          rq && rq->fence.base.signaled && !rcs->requests && rcs->unretired && i915->gt.awake &&
              parks == p);
    i915_request_put(old);
    i915_request_put(rq);
    intel_context_put(r);
    done(&m);
}

/* The releases of the submission's lock left before the one that ends the
 * retire worker's taking of its notice, in i915_submit_work. */
static int unlocks_left;

/** The release `unlocks_left` counts down to. */
static int notice_taken(void)
{
    return --unlocks_left == 0;
}

/**
 * 80-50: a last request that signals to a listener just after the retire
 * worker has taken and cleared its notice kicks the next pass, which
 * retires the engine at once, as Linux's intel_engine_add_retire queues
 * the work again after its xchg; the pass at its second parks the GT.
 */
static void row_park_kick_after_notice(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m);
    struct i915_engine *rcs = &i915->gt.engine[I915_RCS0];
    struct intel_context *r = engine_of(&file_a, 0, I915_RCS0);
    struct i915_request *rq;
    struct dma_fence_cb cb;
    int p;

    rq = submit(r, 0, 0);
    CHECK("kick: a callback on the engine's last request",
          rq && dma_fence_add_callback(&rq->fence.base, &cb, park_listener) == 0);
    p = parks;
    /* One release for each engine's submission notice, then the retire
     * worker's. */
    unlocks_left = I915_NUM_ENGINES + 1;
    unlock_when = notice_taken;
    (void)i915_submit_work();
    CHECK("kick: its signal just after the worker took its notice leaves this pass alone",
          unlock_fired == 1 && rq && rq->fence.base.signaled && !rcs->unretired && rcs->requests &&
              i915->gt.awake && parks == p);
    (void)i915_submit_work();
    CHECK("kick: and the next pass retires the engine at once",
          !rcs->requests && i915->gt.awake && parks == p);
    i915_gt_retire_requests(i915);
    CHECK("kick: and the pass at its second parks the GT", !i915->gt.awake && parks == p + 1);
    i915_request_put(rq);
    intel_context_put(r);
    done(&m);
}

/**
 * 80-50: an older request that signals to a listener while the engine's
 * next request is being added, between its breadcrumb's room let go and
 * its place at the end of the engine's list, leaves the engine held, as
 * Linux holds the timeline's mutex until i915_request_add has put the
 * request on the timeline.
 */
static void row_park_signal_adding(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m);
    struct i915_engine *rcs = &i915->gt.engine[I915_RCS0];
    struct intel_context *r = engine_of(&file_a, 0, I915_RCS0);
    struct i915_request *old, *rq = 0;
    struct dma_fence_cb cb;
    int32_t err;
    int p;

    old = submit(r, 0, 0);
    CHECK("adding: a callback on the engine's last request",
          old && dma_fence_add_callback(&old->fence.base, &cb, park_listener) == 0);
    err = i915_request_create(r, &rq);
    CHECK("adding: the next is created, the older not yet signalled",
          err == 0 && rq && old && !old->fence.base.signaled);
    p = parks;
    inject_after = 0; /* the breadcrumb, the add's first commands */
    CHECK("adding: its signal while the next is added leaves the engine held",
          rq && i915_request_add(rq) == 0 && injected == 1 && old && old->fence.base.signaled &&
              rcs->unretired);
    run_ms(1);
    (void)i915_submit_work();
    i915_retire_requests(i915);
    CHECK("adding: the next, done with no listener, keeps the GT awake to the worker's pass",
          rq && rq->fence.base.signaled && !rcs->requests && rcs->unretired && i915->gt.awake &&
              parks == p);
    i915_request_put(old);
    i915_request_put(rq);
    intel_context_put(r);
    done(&m);
}

/**
 * 80-50: a wait that timed out, a wait a signal interrupted and a callback
 * taken back, each on the engine's last request before it signalled, still
 * let the engine go when it signals, and the worker retires it at once, as
 * Linux keeps a request's signalling enabled once a callback or a wait has
 * enabled it; the worker's pass at its second parks the GT.
 */
static void row_park_listener_left(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m);
    struct i915_engine *rcs = &i915->gt.engine[I915_RCS0];
    struct i915_model_ring *rr = i915_model_ring_state(&m, 0);
    struct intel_context *r = engine_of(&file_a, 0, I915_RCS0);
    struct i915_request *rq;
    struct dma_fence_cb cb;
    int p;

    rr->stuck = 1;
    rq = submit(r, 0, 0);
    p = parks;
    CHECK("left: a wait for the engine's last request times out",
          rq && i915_request_wait(rq, 0, 5000000) == -ETIME && rq->waiters == 0);
    rr->stuck = 0;
    run_ms(1);
    CHECK("left: its signal after the timeout lets the engine go",
          rq && rq->fence.base.signaled && !rcs->unretired);
    (void)i915_submit_work();
    CHECK("left: the worker retires it, the GT awake", !rcs->requests && i915->gt.awake);
    i915_gt_retire_requests(i915);
    CHECK("left: and the worker's pass parks the GT", !i915->gt.awake && parks == p + 1);
    i915_request_put(rq);

    rr->stuck = 1;
    rq = submit(r, 0, 0);
    p = parks;
    intr_next = 1;
    CHECK("left: a wait for the engine's last request is interrupted",
          rq && i915_request_wait(rq, 1, -1) == -EINTR && rq->waiters == 0);
    rr->stuck = 0;
    run_ms(1);
    CHECK("left: its signal after the interruption lets the engine go",
          rq && rq->fence.base.signaled && !rcs->unretired);
    (void)i915_submit_work();
    CHECK("left: the worker retires it, the GT awake", !rcs->requests && i915->gt.awake);
    i915_gt_retire_requests(i915);
    CHECK("left: and the worker's pass parks the GT again", !i915->gt.awake && parks == p + 1);
    i915_request_put(rq);

    rr->stuck = 1;
    rq = submit(r, 0, 0);
    p = parks;
    CHECK("left: a callback on the engine's last request is taken back",
          rq && dma_fence_add_callback(&rq->fence.base, &cb, park_listener) == 0 &&
              dma_fence_remove_callback(&rq->fence.base, &cb) == 1);
    rr->stuck = 0;
    run_ms(1);
    CHECK("left: its signal after the removal lets the engine go",
          rq && rq->fence.base.signaled && !rcs->unretired);
    (void)i915_submit_work();
    CHECK("left: the worker retires it, the GT awake", !rcs->requests && i915->gt.awake);
    i915_gt_retire_requests(i915);
    CHECK("left: and the worker's pass parks the GT a third time",
          !i915->gt.awake && parks == p + 1);
    i915_request_put(rq);
    CHECK("left: no sleep reached its bound", stuck_sleeps == 0);
    intel_context_put(r);
    done(&m);
}

/**
 * 80-61: a request whose breadcrumb is written before its interrupt is
 * taken. A callback asked for then finds it signalled, as Linux's
 * i915_fence_signaled and i915_fence_enable_signaling answer
 * (i915_request.c:88-96), and does not count it listened for, so its
 * retirement leaves the engine held to the worker's pass. One asked for
 * while it ran lets the engine go at the first retirement, before the
 * interrupt, though a query signalled its fence first, as Linux's
 * signal_irq_work goes by I915_FENCE_FLAG_SIGNAL.
 */
static void row_park_before_interrupt(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m);
    struct i915_engine *rcs = &i915->gt.engine[I915_RCS0];
    struct intel_context *r = engine_of(&file_a, 0, I915_RCS0);
    struct i915_request *rq;
    struct dma_fence_cb cb;
    int p;

    rq = submit(r, 0, 0);
    p = parks;
    irq_on = 0;
    run_ms(1);
    CHECK("early: the breadcrumb written, the interrupt not taken",
          rq && i915_engine_passed(rcs, rq->seqno) && !rq->fence.base.signaled);
    CHECK("early: a callback asked for then finds it signalled, not listened for",
          rq && dma_fence_add_callback(&rq->fence.base, &cb, park_listener) == -ENOENT &&
              rq->fence.base.signaled && !rq->signaling);
    i915_retire_requests(i915);
    CHECK("early: its retirement leaves the engine held and the GT awake",
          !rcs->requests && rcs->unretired && i915->gt.awake && parks == p);
    i915_request_put(rq);
    irq_on = 1;

    rq = submit(r, 0, 0);
    CHECK("early: a callback on the engine's last request while it runs",
          rq && dma_fence_add_callback(&rq->fence.base, &cb, park_listener) == 0 && rq->signaling);
    irq_on = 0;
    run_ms(1);
    CHECK("early: a query once its breadcrumb is written signals it, the engine still held",
          rq && dma_fence_is_signaled(&rq->fence.base) && rcs->unretired && i915->gt.awake);
    i915_retire_requests(i915);
    CHECK("early: the retirement before the interrupt lets the engine go",
          !rcs->requests && !rcs->unretired && i915->gt.awake && parks == p);
    i915_gt_retire_requests(i915);
    CHECK("early: and the worker's pass parks the GT", !i915->gt.awake && parks == p + 1);
    irq_on = 1;
    i915_request_put(rq);
    intel_context_put(r);
    done(&m);
}

/* The ring that the next release of a lock lets run, for
 * row_park_between_passes. */
static struct i915_model_ring *run_at_unlock;

/** At the next release of a lock, let `run_at_unlock` run. */
static int ring_runs_now(void)
{
    if (!run_at_unlock) {
        return 0;
    }
    run_at_unlock->stuck = 0;
    run_at_unlock = 0;
    return 1;
}

/**
 * 80-61: the engine's last request, listened for, whose breadcrumb the
 * chip writes after the retirement's signalling pass and before its
 * removal, with no interrupt taken: the removal signals it under the same
 * rule, letting the engine go, as Linux's retirement drops the request's
 * hold on its context in every path (i915_request.c:409), for the worker's
 * pass to park the GT.
 */
static void row_park_between_passes(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m);
    struct i915_engine *rcs = &i915->gt.engine[I915_RCS0];
    struct i915_model_ring *rr = i915_model_ring_state(&m, 0);
    struct intel_context *r = engine_of(&file_a, 0, I915_RCS0);
    struct i915_request *rq;
    struct dma_fence_cb cb;
    int p;

    rr->stuck = 1;
    rq = submit(r, 0, 0);
    CHECK("between: a callback on the engine's last request, which the ring holds",
          rq && dma_fence_add_callback(&rq->fence.base, &cb, park_listener) == 0 && rq->signaling);
    p = parks;
    irq_on = 0;
    run_at_unlock = rr;
    unlock_when = ring_runs_now;
    i915_retire_requests(i915);
    CHECK("between: the breadcrumb written after the signalling pass, the interrupt not taken",
          unlock_fired == 1 && rq && rq->fence.base.signaled && !rq->signaling);
    CHECK("between: the removal lets the engine go",
          !rcs->requests && !rcs->unretired && i915->gt.awake && parks == p);
    i915_gt_retire_requests(i915);
    CHECK("between: and the worker's pass parks the GT", !i915->gt.awake && parks == p + 1);
    irq_on = 1;
    i915_request_put(rq);
    intel_context_put(r);
    done(&m);
}

/* ---- 78-5a.4: GETPARAM and I915_QUERY ---- */

/* GETPARAM's answers on the Lenovo, as Linux 6.18.58 gave them, which are
 * 6.12's but for MMAP_GTT_VERSION (i915_getparam.c). */
static const struct {
    int32_t param;
    int32_t err;
    int value;
} params[] = {
    {I915_PARAM_IRQ_ACTIVE, -ENODEV, 0},
    {I915_PARAM_ALLOW_BATCHBUFFER, -ENODEV, 0},
    {I915_PARAM_LAST_DISPATCH, -ENODEV, 0},
    {I915_PARAM_CHIPSET_ID, 0, 0x0a16},
    {I915_PARAM_HAS_GEM, 0, 1},
    {I915_PARAM_NUM_FENCES_AVAIL, 0, 32},
    {I915_PARAM_HAS_OVERLAY, 0, 0},
    {I915_PARAM_HAS_PAGEFLIPPING, 0, 1},
    {I915_PARAM_HAS_EXECBUF2, 0, 1},
    {I915_PARAM_HAS_BSD, 0, 1},
    {I915_PARAM_HAS_BLT, 0, 1},
    {I915_PARAM_HAS_RELAXED_FENCING, 0, 1},
    {I915_PARAM_HAS_COHERENT_RINGS, 0, 1},
    {I915_PARAM_HAS_EXEC_CONSTANTS, -ENODEV, 0},
    {I915_PARAM_HAS_RELAXED_DELTA, 0, 1},
    {I915_PARAM_HAS_GEN7_SOL_RESET, 0, 1},
    {I915_PARAM_HAS_LLC, 0, 1},
    {I915_PARAM_HAS_ALIASING_PPGTT, 0, 1},
    {I915_PARAM_HAS_WAIT_TIMEOUT, 0, 1},
    {I915_PARAM_HAS_SEMAPHORES, 0, 0},
    {I915_PARAM_HAS_PRIME_VMAP_FLUSH, 0, 1},
    {I915_PARAM_HAS_VEBOX, 0, 1},
    {I915_PARAM_HAS_SECURE_BATCHES, 0, 0},
    {I915_PARAM_HAS_PINNED_BATCHES, 0, 1},
    {I915_PARAM_HAS_EXEC_NO_RELOC, 0, 1},
    {I915_PARAM_HAS_EXEC_HANDLE_LUT, 0, 1},
    {I915_PARAM_HAS_WT, 0, 0},
    {I915_PARAM_CMD_PARSER_VERSION, 0, 10},
    {I915_PARAM_HAS_COHERENT_PHYS_GTT, 0, 1},
    {I915_PARAM_MMAP_VERSION, 0, 1},
    {I915_PARAM_HAS_BSD2, 0, 0},
    {I915_PARAM_REVISION, 0, 11},
    {I915_PARAM_SUBSLICE_TOTAL, 0, 2},
    {I915_PARAM_EU_TOTAL, 0, 20},
    {I915_PARAM_HAS_GPU_RESET, 0, 2},
    {I915_PARAM_HAS_RESOURCE_STREAMER, 0, 0},
    {I915_PARAM_HAS_EXEC_SOFTPIN, 0, 1},
    {I915_PARAM_HAS_POOLED_EU, 0, 0},
    {I915_PARAM_MIN_EU_IN_POOL, 0, 0},
    {I915_PARAM_MMAP_GTT_VERSION, 0, 5},
    {I915_PARAM_HAS_SCHEDULER, 0, 0},
    {I915_PARAM_HUC_STATUS, -ENODEV, 0},
    {I915_PARAM_HAS_EXEC_ASYNC, 0, 1},
    {I915_PARAM_HAS_EXEC_FENCE, 0, 1},
    {I915_PARAM_HAS_EXEC_CAPTURE, 0, 1},
    {I915_PARAM_SLICE_MASK, 0, 1},
    {I915_PARAM_SUBSLICE_MASK, 0, 3},
    {I915_PARAM_HAS_EXEC_BATCH_FIRST, 0, 1},
    {I915_PARAM_HAS_EXEC_FENCE_ARRAY, 0, 1},
    {I915_PARAM_HAS_CONTEXT_ISOLATION, 0, 1},
    {I915_PARAM_CS_TIMESTAMP_FREQUENCY, 0, 12500000},
    {I915_PARAM_MMAP_GTT_COHERENT, 0, 1},
    {I915_PARAM_HAS_EXEC_SUBMIT_FENCE, 0, 1},
    {I915_PARAM_PERF_REVISION, 0, 7},
    {I915_PARAM_HAS_EXEC_TIMELINE_FENCES, 0, 1},
    {I915_PARAM_HAS_USERPTR_PROBE, 0, 1},
    {I915_PARAM_OA_TIMESTAMP_FREQUENCY, 0, 12500000},
    {I915_PARAM_PXP_STATUS, -ENODEV, 0},
    {I915_PARAM_HAS_CONTEXT_FREQ_HINT, 0, -EINVAL},
    {0, -EINVAL, 0},
    {60, -EINVAL, 0},
};

/** GETPARAM of `param` into `*value`: its error. */
static int32_t getparam(int32_t param, int *value)
{
    struct drm_i915_getparam gp;

    gp.param = param;
    gp.value = value;
    return i915_getparam_ioctl(&dev.drm, &gp, &file_a);
}

/** One query item of `id` with `flags` and `length` into `buf`: its length
 * as the call left it, or the call's error below -1000. */
static int32_t query1(uint64_t id, uint32_t flags, int32_t length, void *buf)
{
    struct drm_i915_query q;
    struct drm_i915_query_item item;
    int32_t err;

    memset(&item, 0, sizeof(item));
    item.query_id = id;
    item.flags = flags;
    item.length = length;
    item.data_ptr = (uint64_t)(uintptr_t)buf;
    q.num_items = 1;
    q.flags = 0;
    q.items_ptr = (uint64_t)(uintptr_t)&item;
    err = i915_query_ioctl(&dev.drm, &q, &file_a);
    return err ? err - 1000 : item.length;
}

/** 78-5a.4: GETPARAM against Linux's answers, row by row. */
static void row_getparam(void)
{
    struct i915_model m;
    unsigned i;
    int value, row_ok;

    (void)fresh(&m);
    for (i = 0; i < sizeof(params) / sizeof(params[0]); i++) {
        int32_t err;

        value = 0x5a5a;
        err = getparam(params[i].param, &value);
        row_ok = err == params[i].err && (err != 0 || value == params[i].value);
        if (!row_ok) {
            printf("i915-ctx-test: GETPARAM %d: error %d value %d, want %d and %d\n",
                   (int)params[i].param, (int)err, value, (int)params[i].err, params[i].value);
        }
        CHECK("getparam: a row of Linux's table", row_ok);
    }
    CHECK("getparam: a value that cannot be written is EFAULT",
          getparam(I915_PARAM_CHIPSET_ID, (int *)(uintptr_t)BAD_USER) == -EFAULT);
    done(&m);

    /* A part with 8 EUs a subslice. */
    (void)fresh(&m);
    i915_model_poke(&m, HSW_PAVP_FUSE1, 1u << HSW_F1_EU_DIS_SHIFT);
    i915_model_poke(&m, HSW_EDRAM_CAP, EDRAM_ENABLED);
    dev.sseu.ready = 0; /* read at the open, before the fuse was set */
    CHECK("getparam: 8 EUs a subslice from the fuse: 16 in all",
          getparam(I915_PARAM_EU_TOTAL, &value) == 0 && value == 16);
    CHECK("getparam: eDRAM gives HAS_WT", getparam(I915_PARAM_HAS_WT, &value) == 0 && value == 1);
    done(&m);
}

/**
 * Whether the four entries of ENGINE_INFO at `info` are rcs0, bcs0, vcs0
 * and vecs0, in that order, each instance 0 with its logical instance 0
 * and no capabilities, as Linux answers on Haswell.
 */
static int engines_listed(const struct drm_i915_engine_info *info)
{
    static const uint16_t classes[4] = {I915_ENGINE_CLASS_RENDER, I915_ENGINE_CLASS_COPY,
                                        I915_ENGINE_CLASS_VIDEO, I915_ENGINE_CLASS_VIDEO_ENHANCE};
    int i, ok = 1;

    for (i = 0; i < 4; i++) {
        ok &= info[i].engine.engine_class == classes[i] && info[i].engine.engine_instance == 0 &&
              info[i].flags == I915_ENGINE_INFO_HAS_LOGICAL_INSTANCE &&
              info[i].logical_instance == 0 && info[i].capabilities == 0;
    }
    return ok;
}

/** 78-5a.4: each I915_QUERY item and the call's refusals. */
static void row_query(void)
{
    struct i915_model m;
    uint8_t buf[256];
    struct drm_i915_query_topology_info *topo = (struct drm_i915_query_topology_info *)(void *)buf;
    struct drm_i915_query_engine_info *ei = (struct drm_i915_query_engine_info *)(void *)buf;
    struct drm_i915_engine_info *info = (struct drm_i915_engine_info *)(void *)(buf + 16);
    struct drm_i915_query_memory_regions *mr = (struct drm_i915_query_memory_regions *)(void *)buf;
    struct drm_i915_memory_region_info *region =
        (struct drm_i915_memory_region_info *)(void *)(buf + 16);
    struct drm_i915_query q;
    struct drm_i915_query_item items[3];
    static const uint8_t masks[6] = {0x01, 0x03, 0xff, 0x03, 0xff, 0x03};

    (void)fresh(&m);
    CHECK("query: TOPOLOGY_INFO's length is 22",
          query1(DRM_I915_QUERY_TOPOLOGY_INFO, 0, 0, 0) == 22);
    memset(buf, 0, sizeof(buf));
    CHECK("query: TOPOLOGY_INFO answered", query1(DRM_I915_QUERY_TOPOLOGY_INFO, 0, 22, buf) == 22);
    CHECK("query: one slice of two subslices of 10 EUs, strides 1 and 2",
          topo->max_slices == 1 && topo->max_subslices == 2 && topo->max_eus_per_subslice == 10 &&
              topo->subslice_offset == 1 && topo->subslice_stride == 1 && topo->eu_offset == 2 &&
              topo->eu_stride == 2 && topo->flags == 0);
    CHECK("query: the masks", same_bytes(buf + 16, masks, 6));
    CHECK("query: TOPOLOGY_INFO short is EINVAL",
          query1(DRM_I915_QUERY_TOPOLOGY_INFO, 0, 21, buf) == -EINVAL);
    CHECK("query: TOPOLOGY_INFO with flags is EINVAL",
          query1(DRM_I915_QUERY_TOPOLOGY_INFO, 1, 22, buf) == -EINVAL);
    CHECK("query: TOPOLOGY_INFO that cannot be written is EFAULT",
          query1(DRM_I915_QUERY_TOPOLOGY_INFO, 0, 22, (void *)(uintptr_t)BAD_USER) == -EFAULT);

    CHECK("query: ENGINE_INFO's length is four engines'",
          query1(DRM_I915_QUERY_ENGINE_INFO, 0, 0, 0) == 16 + 4 * 56);
    memset(buf, 0, sizeof(buf));
    CHECK("query: ENGINE_INFO answered", query1(DRM_I915_QUERY_ENGINE_INFO, 0, 240, buf) == 240);
    CHECK("query: rcs0, bcs0, vcs0 then vecs0, logical instance 0, no capabilities",
          ei->num_engines == 4 && engines_listed(info));
    CHECK("query: ENGINE_INFO with a count given is EINVAL",
          query1(DRM_I915_QUERY_ENGINE_INFO, 0, 240, buf) == -EINVAL);
    memset(buf, 0, sizeof(buf));
    ei->rsvd[1] = 1;
    CHECK("query: ENGINE_INFO with a reserved word is EINVAL",
          query1(DRM_I915_QUERY_ENGINE_INFO, 0, 240, buf) == -EINVAL);

    CHECK("query: PERF_CONFIG without i915 perf: ENODEV, EINVAL for another kind",
          query1(DRM_I915_QUERY_PERF_CONFIG, DRM_I915_QUERY_PERF_CONFIG_LIST, 0, 0) == -ENODEV &&
              query1(DRM_I915_QUERY_PERF_CONFIG, DRM_I915_QUERY_PERF_CONFIG_DATA_FOR_ID, 0, 0) ==
                  -ENODEV &&
              query1(DRM_I915_QUERY_PERF_CONFIG, 0, 0, 0) == -EINVAL);
    memset(buf, 0, sizeof(buf));
    CHECK("query: MEMORY_REGIONS, system memory alone",
          query1(DRM_I915_QUERY_MEMORY_REGIONS, 0, 0, 0) == 16 + 88 &&
              query1(DRM_I915_QUERY_MEMORY_REGIONS, 0, 104, buf) == 104 && mr->num_regions == 1 &&
              region->memory_class == I915_MEMORY_CLASS_SYSTEM &&
              region->probed_size == 0x80000000ull && region->unallocated_size == 0x80000000ull &&
              region->rsvd1[0] == 0x80000000ull && region->rsvd1[1] == 0x80000000ull);
    mr->num_regions = 1;
    CHECK("query: MEMORY_REGIONS with a count given is EINVAL",
          query1(DRM_I915_QUERY_MEMORY_REGIONS, 0, 104, buf) == -EINVAL);
    CHECK("query: HWCONFIG_BLOB, GEOMETRY_SUBSLICES and GUC_SUBMISSION_VERSION are ENODEV",
          query1(DRM_I915_QUERY_HWCONFIG_BLOB, 0, 0, 0) == -ENODEV &&
              query1(DRM_I915_QUERY_GEOMETRY_SUBSLICES, 0, 0, 0) == -ENODEV &&
              query1(DRM_I915_QUERY_GUC_SUBMISSION_VERSION, 0, 0, 0) == -ENODEV);
    CHECK("query: an unknown item is EINVAL in its length", query1(8, 0, 0, 0) == -EINVAL);
    CHECK("query: item 0 is EINVAL for the call", query1(0, 0, 0, 0) == -EINVAL - 1000);

    /* Several items, the length written only where it differs. */
    memset(items, 0, sizeof(items));
    items[0].query_id = DRM_I915_QUERY_TOPOLOGY_INFO;
    items[1].query_id = DRM_I915_QUERY_TOPOLOGY_INFO;
    items[1].length = 22;
    items[1].data_ptr = (uint64_t)(uintptr_t)buf;
    items[2].query_id = DRM_I915_QUERY_ENGINE_INFO;
    memset(buf, 0, sizeof(buf));
    q.num_items = 3;
    q.flags = 0;
    q.items_ptr = (uint64_t)(uintptr_t)items;
    CHECK("query: three items in one call", i915_query_ioctl(&dev.drm, &q, &file_a) == 0 &&
                                                items[0].length == 22 && items[1].length == 22 &&
                                                items[2].length == 240);
    q.flags = 1;
    CHECK("query: the call's flags are EINVAL", i915_query_ioctl(&dev.drm, &q, &file_a) == -EINVAL);
    q.flags = 0;
    q.items_ptr = BAD_USER;
    CHECK("query: items that cannot be read are EFAULT",
          i915_query_ioctl(&dev.drm, &q, &file_a) == -EFAULT);
    q.num_items = 0;
    CHECK("query: no items is 0", i915_query_ioctl(&dev.drm, &q, &file_a) == 0);
    done(&m);
}

/* ---- 78-5a.5: GET_RESET_STATS ---- */

/** 78-5a.5: GET_RESET_STATS. */
static void row_reset_stats(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m);
    struct drm_i915_reset_stats rs;
    struct i915_gem_context *ctx;
    uint32_t id = make_context(&file_a);

    memset(&rs, 0, sizeof(rs));
    i915->gt.reset_count = 3;
    CHECK("reset stats: the default context answers, the resets hidden without privilege",
          i915_gem_context_reset_stats_ioctl(&dev.drm, &rs, &file_a) == 0 && rs.reset_count == 0 &&
              rs.batch_active == 0 && rs.batch_pending == 0);
    rs.ctx_id = id;
    caller.euid = 0;
    CHECK("reset stats: a new context answers from the start, the resets shown with privilege",
          i915_gem_context_reset_stats_ioctl(&dev.drm, &rs, &file_a) == 0 && rs.reset_count == 3);
    caller.euid = 1000;
    CHECK("reset stats: the context's counts", i915_gem_context_lookup(&file_a, id, &ctx) == 0);
    ctx->guilty_count = 2;
    ctx->active_count = 5;
    i915_gem_context_put(ctx);
    CHECK("reset stats: the batches active at a hang and pending behind one",
          i915_gem_context_reset_stats_ioctl(&dev.drm, &rs, &file_a) == 0 && rs.batch_active == 2 &&
              rs.batch_pending == 5);
    rs.ctx_id = 99;
    CHECK("reset stats: no such context is ENOENT",
          i915_gem_context_reset_stats_ioctl(&dev.drm, &rs, &file_a) == -ENOENT);
    rs.ctx_id = 0;
    rs.flags = 1;
    CHECK("reset stats: flags are EINVAL",
          i915_gem_context_reset_stats_ioctl(&dev.drm, &rs, &file_a) == -EINVAL);
    rs.flags = 0;
    rs.pad = 1;
    CHECK("reset stats: a pad is EINVAL",
          i915_gem_context_reset_stats_ioctl(&dev.drm, &rs, &file_a) == -EINVAL);
    done(&m);
}

/* ---- 78-5c: THROTTLE ---- */

/** 78-5c: THROTTLE. */
static void row_throttle(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m);
    struct intel_context *r = engine_of(&file_a, 0, I915_RCS0);
    struct i915_model_ring *rr = i915_model_ring_state(&m, 0);
    struct i915_request *old, *young, *t;

    CHECK("throttle: with nothing running it returns at once",
          i915_gem_throttle_ioctl(&dev.drm, 0, &file_a) == 0);
    old = submit(r, 0, 0);
    CHECK("throttle: the first request records the default state and completes",
          old && i915_request_wait(old, 0, 100000000) == 0);
    i915_request_put(old);
    CHECK("throttle: a second file opens", i915_driver_open(&dev.drm, &file_b) == 0);
    rr->stuck = 1;
    old = submit(r, 0, 0);
    run_ms(25);
    young = submit(r, 0, 0);
    t = i915_request_throttle_target(r, dma_fence_now() - 20000000ll);
    CHECK("throttle: it waits for the newest request older than 20 ms", old && young && t == old);
    i915_request_put(t);
    CHECK("throttle: none of another file's", i915_gem_throttle_ioctl(&dev.drm, 0, &file_b) == 0);
    rr->stuck = 0;
    CHECK("throttle: once the ring runs it returns 0, the old request done",
          i915_gem_throttle_ioctl(&dev.drm, 0, &file_a) == 0 && old && i915_request_completed(old));
    CHECK("throttle: the young one completes too",
          young && i915_request_wait(young, 0, 100000000) == 0);
    i915_request_put(old);
    i915_request_put(young);
    i915->gt.wedged = 1;
    CHECK("throttle: EIO on a wedged GPU, the ABI's report of it",
          i915_gem_throttle_ioctl(&dev.drm, 0, &file_a) == -EIO);
    i915->gt.wedged = 0;
    intel_context_put(r);
    done(&m);
}

/* ---- 78-11a.2: REG_READ ---- */

/* The render ring's timestamp, which moves on by one at each read of a
 * half, and the reads of it made without the kernel's forcewake. */
static uint64_t timestamp;
static int timestamp_unwoken;

/** The model's answer for the timestamp's halves: the counter's, after it
 * moves on. */
static uint32_t timestamp_read(struct i915_model *m, uint32_t reg, uint32_t value)
{
    if (reg != RING_TIMESTAMP(RENDER_RING_BASE) && reg != RING_TIMESTAMP_UDW(RENDER_RING_BASE)) {
        return value;
    }
    if (!(i915_model_peek(m, FORCEWAKE_ACK_HSW) & FORCEWAKE_KERNEL)) {
        timestamp_unwoken++;
    }
    timestamp++;
    return reg == RING_TIMESTAMP(RENDER_RING_BASE) ? (uint32_t)timestamp
                                                   : (uint32_t)(timestamp >> 32);
}

/**
 * 78-11a.2: REG_READ as Linux 6.12's i915_reg_read_ioctl answers it on
 * Haswell: the render ring's timestamp under forcewake, whole across a
 * carry with I915_REG_READ_8B_WA as Mesa asks, low half first without it;
 * any other register or flag EINVAL; and the call's number Linux's.
 */
static void row_reg_read(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m);
    struct drm_i915_reg_read r;

    m.on_read = timestamp_read;
    timestamp = 0x1fffffffeull;
    timestamp_unwoken = 0;
    memset(&r, 0, sizeof(r));
    r.offset = 0x2358u | I915_REG_READ_8B_WA;
    CHECK("reg read: the timestamp across a carry, read again as Mesa asks",
          i915_reg_read_ioctl(&dev.drm, &r, &file_a) == 0 && r.val == 0x200000002ull);
    CHECK("reg read: under forcewake, let go after",
          timestamp_unwoken == 0 && i915->uncore.wake_count == 0);
    timestamp = 0x1fffffffeull;
    r.offset = 0x2358u;
    CHECK("reg read: without the flag, the low half and then the high",
          i915_reg_read_ioctl(&dev.drm, &r, &file_a) == 0 && r.val == 0x2ffffffffull);
    r.offset = 0x2350u;
    CHECK("reg read: another register EINVAL",
          i915_reg_read_ioctl(&dev.drm, &r, &file_a) == -EINVAL);
    r.offset = 0x235cu;
    CHECK("reg read: the upper half alone EINVAL",
          i915_reg_read_ioctl(&dev.drm, &r, &file_a) == -EINVAL);
    r.offset = 0x2358u | 2u;
    CHECK("reg read: another flag EINVAL", i915_reg_read_ioctl(&dev.drm, &r, &file_a) == -EINVAL);
    r.offset = 0x100002358ull;
    CHECK("reg read: an offset past 32 bits EINVAL",
          i915_reg_read_ioctl(&dev.drm, &r, &file_a) == -EINVAL);
    CHECK("reg read: no forcewake left held", i915->uncore.wake_count == 0);
    CHECK("reg read: Linux's number, 0xc0106471", DRM_IOCTL_I915_REG_READ == 0xc0106471u);
    m.on_read = 0;
    done(&m);
}

/** Nothing: one thread has no one to give way to. */
void sched_give_way(void)
{}

int main(void)
{
    row_contexts();
    row_contexts_wedged();
    row_create_destroy();
    row_params();
    row_engine_maps();
    row_engines_raced();
    row_requests();
    row_park_unwatched();
    row_park_waited();
    row_park_watched();
    row_park_building();
    row_park_spin();
    row_park_merge_query();
    row_park_signal_creating();
    row_park_signal_adding();
    row_park_kick_after_notice();
    row_park_listener_left();
    row_park_before_interrupt();
    row_park_between_passes();
    row_ring_room();
    row_getparam();
    row_query();
    row_reset_stats();
    row_throttle();
    row_reg_read();
    CHECK("the heap: every record freed", heap_live == 0);
    if (fails) {
        printf("i915-ctx-test: %d failures\n", fails);
        return 1;
    }
    printf("i915-ctx-test: ok\n");
    return 0;
}
