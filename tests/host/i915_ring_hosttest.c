/**
 * i915_ring_hosttest.c: the Intel graphics driver's rings and interrupts,
 * i915_ring.c, i915_irq.c and first light, i915_firstlight.c, against the
 * host model of the Lenovo's HD 4400, whose rings run the driver's
 * commands from memory and whose interrupt registers raise its message.
 *
 * host-deps: rach2/sys/drm/i915/i915_ring.c rach2/sys/drm/i915/i915_irq.c
 * host-deps: rach2/sys/drm/i915/i915_firstlight.c
 * host-deps: rach2/sys/drm/i915/i915_ppgtt.c
 * host-deps: rach2/sys/drm/i915/i915_uncore.c rach2/sys/drm/i915/i915_power.c
 * host-deps: rach2/sys/drm/i915/i915_gtt.c rach2/sys/drm/i915/i915_hw.c
 * host-deps: tests/host/i915_model.c rach2/sys/lib/kstring.c
 * host-deps: rach2/sys/lib/kctypes.c tests/host/compat/spinlock_host.c
 *
 * A row makes a fresh model and a device record over it, and brings it up
 * as the bind does: forcewake, the global GTT, the interrupts reset and
 * installed, then the rings. The kernel around the driver is a set of
 * records: the heap hands out pages whose physical addresses are spread
 * apart, which the model's rings reach back through the test; the
 * processor takes the device's messages, by calling the driver's handler,
 * whenever the model's clock moves while its interrupts are on; a sleep
 * moves the model's clock a millisecond at a time until its condition
 * holds; and the tick count is the model's clock in milliseconds.
 *
 * The rows: the rings started (status pages and rings in the GTT's top
 * on pages scattered in memory, the stop before the start, the TLB flush,
 * RING_VALID, the interrupts left masked) and stopped; a ring that will
 * not stop, a stop forgiven on an empty ring, a dead ring, a TLB flush
 * that never ends; the interrupt table (refusals, the GT's identity read
 * and acknowledged under the master enable, the display engine's, the
 * south display's behind the PCH event, power management's, spurious and
 * unclaimed bits); breadcrumbs, waits woken by the interrupt and by
 * polling, a stuck ring's timeout, the ring's wrap and its room; and first
 * light, end to end and in each of its failures.
 *
 * Returns 0 when every row passes, 1 otherwise.
 */

#include <stdarg.h>

#include "drm/i915/i915.h"
#include "drm/i915/i915_reg.h"
#include "lib/kerrno.h"
#include "lib/kirq.h"
#include "mm/kheap.h"
#include "mm/paging.h"
#include "proc/percpu.h"
#include "rtc.h"
#include "udelay.h"
#include "i915_model.h"
#include "i915_lenovo.h"
#include "i915_model_base.h"
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
extern int strcmp(const char *a, const char *b);

static int fails;

#define CHECK(what, cond)                               \
    do {                                                \
        if (!(cond)) {                                  \
            printf("i915-ring-test: fail: %s\n", what); \
            fails++;                                    \
        }                                               \
    } while (0)

/* ---- the kernel around the driver ---- */

static char log_text[16384];

/** The boot log, kept whole for the rows that read it. */
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

/** Whether the log holds `text`. */
static int logged(const char *text)
{
    return strstr(log_text, text) != 0;
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
 * two pages apart from the last, so that no two pages of one allocation
 * are adjacent in memory. */
#define TEST_PAGES 256
static struct {
    uint8_t *host;
    uint64_t phys;
} pages[TEST_PAGES];
static int npages, pages_live, pages_refuse;

/* The pages the started GT holds: each ring's four and its status page,
 * and the PPGTT's scratch table. */
#define TEST_GT_PAGES 21
static uint64_t next_phys = 0x50000000ull;

void *kalloc_pages(uint32_t count, uintptr_t *phys)
{
    void *block = 0;
    uint32_t i;

    if (pages_refuse || npages + (int)count > TEST_PAGES ||
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

void *kmalloc(size_t size)
{
    return malloc(size);
}

void kfree(void *p)
{
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

/* A physical page whose writes the row sends to a page of its own, as a
 * write the chip lost would be, or 0; and the GTT's scratch page. */
static uint64_t drop_phys;
static uint32_t drop_page[1024], scratch_page[1024];

#define TEST_SCRATCH 0x7f000000u

/** The model's way to memory: the host page at `phys`. */
static void *test_dma(struct i915_model *m, uint64_t phys)
{
    int i;

    (void)m;
    if (drop_phys && phys == drop_phys) {
        return drop_page;
    }
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

/* The GTT's scratch page. */
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

/* The MMIO backend, which no row uses. */
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

/* Configuration space: the model's chip at 00:02.0. */
static struct i915_model *bus;

uint16_t pci_read_config16(uint8_t b, uint8_t d, uint8_t f, uint8_t offset)
{
    return bus && b == 0 && d == 2 && f == 0 ? (uint16_t)i915_model_config(bus, offset, 2)
                                             : 0xffffu;
}

static int cmdline_firstlight;

int kcmdline_has(const char *flag)
{
    return cmdline_firstlight && strcmp(flag, "rach2.i915.firstlight") == 0;
}

/* The process that waits, or none, as at boot. */
static process_t caller;
struct percpu cpu_table[NR_CPUS];

static struct drm_i915_private dev;
static int sleeps, wakes, stuck_sleeps;

uint64_t rtc_ticks_now(void)
{
    return bus ? bus->now_us / 1000u : 0;
}

/** Take every message the model has pending, while the processor's
 * interrupts are on, as the processor calls the handler. */
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

/** A sleep: the model's clock moves a millisecond at a time, the
 * processor taking the messages, until `cond` holds; one that never
 * would fails the row after ten seconds of the model's clock. */
int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    int steps = 0;

    (void)wq;
    sleeps++;
    while (!cond(arg)) {
        if (++steps > 10000) {
            stuck_sleeps++;
            return 0;
        }
        i915_delay_us(&dev.hw, 1000);
    }
    return 0;
}

void wq_wake_all(wait_queue_t *wq)
{
    (void)wq;
    wakes++;
}

/* The id table: the Lenovo's chip alone. */
static const struct i915_platform hsw = {"HASWELL", 7, 50, 7, 2, 0xf, 1, 1, 1, 0, 1, 31, 40};
static const struct i915_id lenovo_id = {0x0A16, 1, 0, &hsw};

const struct i915_id *i915_id_lookup(uint16_t device_id)
{
    return device_id == 0x0A16 ? &lenovo_id : 0;
}

/* ---- the device ---- */

/** The model's clock moved: the processor takes its messages. */
static void on_delay(struct i915_model *m)
{
    take_messages(m);
}

/**
 * A fresh model on the bus and a device record over it, brought up as the
 * bind does up to the rings: forcewake, the global GTT, the interrupts
 * reset and their master enable on. The kernel's records cleared; a
 * process waits, with its interrupts off.
 */
static struct drm_i915_private *fresh(struct i915_model *m)
{
    int i;

    CHECK("the model is made", i915_model_init(m) == 0);
    bus = m;
    m->on_delay = on_delay;
    i915_model_irq_state(m)->dma = test_dma;
    k_memset(&dev, 0, sizeof(dev));
    dev.pci = m->pci;
    dev.hw = m->hw;
    dev.mmio_phys = LENOVO_GPU_BAR0;
    dev.gmadr_phys = LENOVO_GPU_BAR2;
    dev.gmch_ctrl = (uint16_t)i915_model_config(m, SNB_GMCH_CTRL, 2);
    dev.bsm = i915_model_config(m, INTEL_BSM, 4);
    dev.msi_vector = 48;
    log_text[0] = 0;
    irq_depth = irq_on = 0;
    for (i = 0; i < npages; i++) {
        pages[i].host = 0;
    }
    npages = pages_live = pages_refuse = 0;
    next_phys = 0x50000000ull;
    drop_phys = 0;
    cmdline_firstlight = 0;
    cpu_table[0].current = &caller;
    sleeps = wakes = stuck_sleeps = 0;
    i915_uncore_init(&dev);
    CHECK("the GTT comes up", i915_ggtt_init(&dev) == 0);
    i915_irq_reset(&dev);
    i915_irq_install(&dev);
    return &dev;
}

/** Take the model off the bus. */
static void done(struct i915_model *m)
{
    i915_model_free(m);
    bus = 0;
}

/** The log index of the first write of `value` to `reg`, or a large
 * number. */
static long at(const struct i915_model *m, uint32_t reg, uint32_t value)
{
    long i = i915_model_find_write(m, reg, value, 0);
    return i < 0 ? 1L << 30 : i;
}

/** The log index of the last write to `reg`, or -1. */
static long last_write(const struct i915_model *m, uint32_t reg)
{
    long i;

    for (i = (long)m->nlog - 1; i >= 0; i--) {
        if (m->log[i].op == I915_MODEL_WRITE && m->log[i].reg == reg) {
            return i;
        }
    }
    return -1;
}

/** Whether the `n` pages from the kernel's address `v` are mapped at the
 * GTT offset `ggtt`, each at the LLC's level, to its own page. */
static int mapped(struct drm_i915_private *i915, const void *v, uint32_t n, uint32_t ggtt)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        uint64_t phys = paging_virt_to_phys((uintptr_t)v + i * 4096u);
        if (phys == 0 ||
            i915_ggtt_pte(i915, ggtt + i * 4096u) != i915_ggtt_pte_encode(phys, I915_CACHE_LLC)) {
            return 0;
        }
    }
    return 1;
}

/* ---- the rings started and stopped ---- */

/** Check that the ring of `e` was started as Linux's xcs_resume starts it. */
static void check_started(struct i915_model *m, struct drm_i915_private *i915,
                          struct i915_engine *e)
{
    uint32_t b = e->mmio_base;
    long stop = at(m, RING_MI_MODE(b), I915_MASKED_ENABLE(STOP_RING));
    long off = at(m, RING_CTL(b), 0);
    long hws = at(m, e->hws_pga, e->hws_ggtt);
    long flush =
        at(m, RING_INSTPM(b), I915_MASKED_ENABLE(INSTPM_TLB_INVALIDATE | INSTPM_SYNC_FLUSH));
    long start = at(m, RING_START(b), e->ring_ggtt);
    long valid = at(m, RING_CTL(b), 0x3000u | RING_VALID);
    long go = at(m, RING_MI_MODE(b), I915_MASKED_DISABLE(STOP_RING));

    CHECK("start: running", e->running && e->head == 0 && e->tail == 0);
    CHECK("start: stopped, emptied, then the status page, the flush, the start, RING_VALID and "
          "the go",
          stop < off && off < hws && hws < flush && flush < start && start < valid && valid < go &&
              go < (1L << 30));
    CHECK("start: the status page's address", i915_model_peek(m, e->hws_pga) == e->hws_ggtt);
    CHECK("start: the ring's start, size and head",
          i915_model_peek(m, RING_START(b)) == e->ring_ggtt &&
              i915_model_peek(m, RING_CTL(b)) == (0x3000u | RING_VALID) &&
              i915_model_peek(m, RING_HEAD(b)) == 0 && i915_model_peek(m, RING_TAIL(b)) == 0);
    CHECK("start: no longer told to stop", !(i915_model_peek(m, RING_MI_MODE(b)) & STOP_RING));
    CHECK("start: the status page's interrupt writes, but the render ring's bit 0, and the "
          "ring's interrupts masked",
          i915_model_peek(m, RING_HWSTAM(b)) == (b == RENDER_RING_BASE ? ~1u : 0xffffffffu) &&
              i915_model_peek(m, RING_IMR(b)) == 0xffffffffu);
    CHECK("start: the status page mapped at the LLC's level", mapped(i915, e->hws, 1, e->hws_ggtt));
    CHECK("start: the ring's four pages mapped, each its own",
          mapped(i915, e->ring, 4, e->ring_ggtt));
    CHECK("start: the status page and the ring at the GTT's top",
          e->hws_ggtt >= i915->gt.ggtt_bottom && e->ring_ggtt >= i915->gt.ggtt_bottom &&
              e->ring_ggtt + I915_RING_SIZE <= i915->ggtt.total);
}

/** Whether the user interrupts of the render, blitter and video rings are
 * the GT's enables and the video enhancement ring's power management's,
 * with both banks masked. */
static int user_irqs_enabled(struct i915_model *m)
{
    return i915_model_peek(m, GTIER) ==
               (GT_RENDER_USER_INTERRUPT | GT_BLT_USER_INTERRUPT | GT_BSD_USER_INTERRUPT) &&
           i915_model_peek(m, GTIMR) == 0xffffffffu &&
           i915_model_peek(m, GEN6_PMIER) == PM_VEBOX_USER_INTERRUPT &&
           i915_model_peek(m, GEN6_PMIMR) == 0xffffffffu;
}

/** Whether each ring's handler is registered at its bit of its bank. */
static int handlers_registered(struct drm_i915_private *i915)
{
    return i915->irq.fn[I915_IRQ_GT][0] && i915->irq.fn[I915_IRQ_GT][22] &&
           i915->irq.data[I915_IRQ_GT][22] == &i915->gt.engine[I915_BCS0] &&
           i915->irq.data[I915_IRQ_GT][12] == &i915->gt.engine[I915_VCS0] &&
           i915->irq.data[I915_IRQ_PM][10] == &i915->gt.engine[I915_VECS0];
}

/** The rings started on a quiet chip, and stopped. */
static void row_start(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m);
    long wake, release, first, last;
    int i;

    m.nlog = 0;
    CHECK("start: the four rings start", i915_gt_init(i915) == 0);
    for (i = 0; i < I915_NUM_ENGINES; i++) {
        check_started(&m, i915, &i915->gt.engine[i]);
    }
    CHECK("start: the render ring's registers and status page",
          i915->gt.engine[I915_RCS0].mmio_base == RENDER_RING_BASE &&
              i915->gt.engine[I915_RCS0].hws_pga == RENDER_HWS_PGA_GEN7 &&
              i915->gt.engine[I915_RCS0].irq_bit == GT_RENDER_USER_INTERRUPT);
    CHECK("start: the blitter's", i915->gt.engine[I915_BCS0].mmio_base == BLT_RING_BASE &&
                                      i915->gt.engine[I915_BCS0].hws_pga == BLT_HWS_PGA_GEN7 &&
                                      i915->gt.engine[I915_BCS0].irq_bit == GT_BLT_USER_INTERRUPT);
    CHECK("start: eight objects in 80 KiB below the GTT's top, the PPGTT's directory below them",
          i915->gt.ggtt_top == i915->ggtt.total &&
              i915->gt.ppgtt.pd_ggtt ==
                  ((i915->ggtt.total - 4u * (4096u + I915_RING_SIZE) - I915_PPGTT_PD_BYTES) &
                   ~(uint64_t)(I915_PPGTT_PD_ALIGN - 1u)) &&
              i915->gt.ggtt_bottom == i915->gt.ppgtt.pd_ggtt);
    kernel_ticks++;
    i915_uncore_tick(i915);
    kernel_ticks++;
    i915_uncore_tick(i915);
    wake = at(&m, FORCEWAKE_MT, 0x00010001u);
    release = last_write(&m, FORCEWAKE_MT);
    first = at(&m, RING_MI_MODE(RENDER_RING_BASE), I915_MASKED_ENABLE(STOP_RING));
    last = at(&m, RING_MI_MODE(BLT_RING_BASE), I915_MASKED_DISABLE(STOP_RING));
    CHECK("start: under forcewake, released by the release timer after",
          wake < first && last < release && i915->uncore.wake_count == 0 &&
              i915_model_forcewake(&m)->req == 0);
    CHECK("start: the GT's user interrupts of three rings and power management's of the "
          "fourth enabled, and masked",
          user_irqs_enabled(&m));
    CHECK("start: every other source masked and off, the master enable on",
          i915_model_peek(&m, DEIER) == DE_MASTER_IRQ_CONTROL &&
              i915_model_peek(&m, DEIMR) == 0xffffffffu && i915_model_peek(&m, SDEIER) == 0 &&
              i915_model_peek(&m, SDEIMR) == 0xffffffffu);
    CHECK("start: the handlers registered", handlers_registered(i915));
    CHECK("start: no wait reached its bound", i915->hw.timeouts == 0);
    CHECK("start: logged", logged("rings: rcs0, bcs0, vcs0 and vecs0 started: 16 KiB each"));

    i915_gt_fini(i915);
    CHECK("stop: each ring disabled and its interrupt off",
          i915_model_peek(&m, RING_CTL(RENDER_RING_BASE)) == 0 &&
              i915_model_peek(&m, RING_CTL(BLT_RING_BASE)) == 0 &&
              i915_model_peek(&m, RING_CTL(GEN6_BSD_RING_BASE)) == 0 &&
              i915_model_peek(&m, RING_CTL(VEBOX_RING_BASE)) == 0 &&
              i915_model_peek(&m, GTIER) == 0 && i915_model_peek(&m, GEN6_PMIER) == 0 &&
              !i915->gt.engine[0].running && !i915->gt.engine[1].running &&
              !i915->gt.engine[2].running && !i915->gt.engine[3].running);
    CHECK("stop: the pages given back and the GTT's space",
          pages_live == 0 && i915->gt.ggtt_bottom == i915->gt.ggtt_top);
    CHECK("stop: the entries on the scratch page again",
          i915_ggtt_pte(i915, i915->ggtt.total - 4096u) == i915->ggtt.scratch_pte);
    CHECK("stop: forcewake released", i915->uncore.wake_count == 0);
    done(&m);
}

/** A ring that will not stop, a stop forgiven on an empty ring, a dead
 * ring, a TLB flush that never ends, and no memory. */
static void row_start_failures(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m);
    uint64_t t0;

    i915_model_ring_state(&m, 0)->idle_stuck = 1;
    CHECK("stop: an empty ring that never reports idle is forgiven", i915_gt_init(i915) == 0);
    CHECK("stop: the wait named",
          i915->hw.last_timeout && strcmp(i915->hw.last_timeout, "ring stop") == 0);
    i915_gt_fini(i915);
    done(&m);

    i915 = fresh(&m);
    i915_model_ring_state(&m, 1)->idle_stuck = 1;
    i915_model_ring_state(&m, 1)->stuck = 1;
    i915_model_poke(&m, RING_CTL(BLT_RING_BASE), 0x3000u | RING_VALID);
    i915_model_poke(&m, RING_HEAD(BLT_RING_BASE), 0x40);
    i915_model_poke(&m, RING_TAIL(BLT_RING_BASE), 0x80);
    CHECK("stop: a busy ring that will not stop fails the start", i915_gt_init(i915) == -EIO);
    CHECK("stop: logged", logged("bcs0: did not stop"));
    CHECK("stop: nothing left: no ring running, no page, the GTT's space back",
          !i915->gt.engine[0].running && !i915->gt.engine[1].running && pages_live == 0 &&
              i915->gt.ggtt_bottom == i915->gt.ggtt_top && i915_model_peek(&m, GTIER) == 0);
    done(&m);

    i915 = fresh(&m);
    i915_model_ring_state(&m, 0)->start_fails = 1;
    t0 = m.now_us;
    CHECK("start: a dead ring fails", i915_gt_init(i915) == -EIO);
    CHECK("start: by name, after 5 ms", i915->hw.last_timeout &&
                                            strcmp(i915->hw.last_timeout, "ring start") == 0 &&
                                            m.now_us - t0 >= 5000u);
    CHECK("start: logged with the registers", logged("rcs0: initialization failed: ctl"));
    CHECK("start: nothing left", pages_live == 0 && !i915->gt.engine[0].running);
    done(&m);

    i915 = fresh(&m);
    i915_model_ring_state(&m, 0)->flush_stuck = 1;
    CHECK("flush: a TLB flush that never ends is logged and the start goes on",
          i915_gt_init(i915) == 0 && logged("ring tlb flush: timeout"));
    i915_gt_fini(i915);
    done(&m);

    i915 = fresh(&m);
    pages_refuse = 1;
    CHECK("start: no memory", i915_gt_init(i915) == -ENOMEM && !i915->gt.engine[0].running &&
                                  logged("rings: no memory"));
    done(&m);
}

/* ---- the interrupt table ---- */

static int calls[I915_IRQ_BANKS];
static uint32_t got_iir[I915_IRQ_BANKS];

/** A source's handler: counts its calls by the bank in `data` and keeps
 * the bits it was given. */
static void handler(struct drm_i915_private *i915, uint32_t iir, void *data)
{
    int bank = (int)(long)data;

    (void)i915;
    calls[bank]++;
    got_iir[bank] = iir;
}

/** The log index of the first read of `reg`, or -1. */
static long first_read(const struct i915_model *m, uint32_t reg)
{
    uint32_t i;

    for (i = 0; i < m->nlog; i++) {
        if (m->log[i].op == I915_MODEL_READ && m->log[i].reg == reg) {
            return (long)i;
        }
    }
    return -1;
}

/** The refusals of registration, and the GT's render interrupt registered
 * and unmasked. */
static void irq_register_rows(struct i915_model *m, struct drm_i915_private *i915)
{
    CHECK("register: the master enable refused",
          i915_irq_register(i915, I915_IRQ_DE, DE_MASTER_IRQ_CONTROL, handler, 0) == -EINVAL);
    CHECK("register: the PCH event refused",
          i915_irq_register(i915, I915_IRQ_DE, DE_PCH_EVENT_IVB, handler, 0) == -EINVAL);
    CHECK("register: no bank, no bits, no handler refused",
          i915_irq_register(i915, 4, 1, handler, 0) == -EINVAL &&
              i915_irq_register(i915, I915_IRQ_GT, 0, handler, 0) == -EINVAL &&
              i915_irq_register(i915, I915_IRQ_GT, 1, 0, 0) == -EINVAL);
    CHECK("register: the GT's render interrupt",
          i915_irq_register(i915, I915_IRQ_GT, GT_RENDER_USER_INTERRUPT, handler,
                            (void *)(long)I915_IRQ_GT) == 0);
    CHECK("register: a bit taken refused",
          i915_irq_register(i915, I915_IRQ_GT, GT_RENDER_USER_INTERRUPT | GT_BSD_USER_INTERRUPT,
                            handler, 0) == -EBUSY &&
              !i915->irq.fn[I915_IRQ_GT][12]);
    CHECK("register: enabled and still masked",
          i915_model_peek(m, GTIER) == GT_RENDER_USER_INTERRUPT &&
              i915_model_peek(m, GTIMR) == 0xffffffffu);
    i915_irq_unmask(i915, I915_IRQ_GT, GT_RENDER_USER_INTERRUPT | GT_BLT_USER_INTERRUPT);
    CHECK("unmask: only the registered bit",
          i915_model_peek(m, GTIMR) == ~GT_RENDER_USER_INTERRUPT);
}

/** The GT's render interrupt raised, read and acknowledged under the master
 * enable; then masked, and a spurious message. */
static void irq_gt_rows(struct i915_model *m, struct drm_i915_private *i915)
{
    struct i915_model_irq *mi = i915_model_irq_state(m);
    long off, read, ack, on;

    i915_model_irq_event(m, I915_IRQ_GT, GT_BLT_USER_INTERRUPT);
    CHECK("event: a masked source raises nothing",
          mi->messages == 0 && i915_model_peek(m, GTIIR) == 0);
    i915_model_irq_event(m, I915_IRQ_GT, GT_RENDER_USER_INTERRUPT);
    CHECK("event: an unmasked one latches and raises a message",
          mi->messages == 1 && i915_model_peek(m, GTIIR) == GT_RENDER_USER_INTERRUPT);
    m->nlog = 0;
    CHECK("message: taken", i915_model_take_message(m) == 1);
    i915_irq_handle(i915);
    off = at(m, DEIER, 0);
    read = first_read(m, GTIIR);
    ack = at(m, GTIIR, GT_RENDER_USER_INTERRUPT);
    on = at(m, DEIER, DE_MASTER_IRQ_CONTROL);
    CHECK("handle: the master off, the identity read and acknowledged, the master on",
          off >= 0 && off < read && read < ack && ack < on && on < (1L << 30));
    CHECK("handle: the handler once, with its bit",
          calls[I915_IRQ_GT] == 1 && got_iir[I915_IRQ_GT] == GT_RENDER_USER_INTERRUPT);
    CHECK("handle: the identity clear and no message more",
          i915_model_peek(m, GTIIR) == 0 && mi->pending == 0 && i915->irq.messages == 1 &&
              i915->irq.spurious == 0);
    i915_irq_mask(i915, I915_IRQ_GT, GT_RENDER_USER_INTERRUPT);
    CHECK("mask: masked again", i915_model_peek(m, GTIMR) == 0xffffffffu);
    i915_irq_handle(i915);
    CHECK("spurious: a message with nothing set is counted",
          i915->irq.spurious == 1 && calls[I915_IRQ_GT] == 1);
}

/** A source the firmware left latched and enabled, with no owner. */
static void irq_unclaimed_rows(struct i915_model *m, struct drm_i915_private *i915)
{
    i915_model_poke(m, GTIIR, GT_BSD_USER_INTERRUPT);
    i915_model_poke(m, GTIER, GT_RENDER_USER_INTERRUPT | GT_BSD_USER_INTERRUPT);
    i915_irq_handle(i915);
    CHECK("unclaimed: counted, acknowledged, masked and disabled",
          i915->irq.unclaimed == 1 && i915_model_peek(m, GTIIR) == 0 &&
              (i915_model_peek(m, GTIMR) & GT_BSD_USER_INTERRUPT) &&
              i915_model_peek(m, GTIER) == GT_RENDER_USER_INTERRUPT && logged("no handler"));
}

/** The display engine's: pipe A's vblank. */
static void irq_display_rows(struct i915_model *m, struct drm_i915_private *i915)
{
    CHECK("display: pipe A's vblank registered",
          i915_irq_register(i915, I915_IRQ_DE, DE_PIPE_VBLANK_IVB(0), handler,
                            (void *)(long)I915_IRQ_DE) == 0);
    i915_irq_unmask(i915, I915_IRQ_DE, DE_PIPE_VBLANK_IVB(0));
    CHECK("display: enabled beside the master, unmasked",
          i915_model_peek(m, DEIER) == (DE_MASTER_IRQ_CONTROL | DE_PIPE_VBLANK_IVB(0)) &&
              i915_model_peek(m, DEIMR) == ~DE_PIPE_VBLANK_IVB(0));
    i915_model_irq_event(m, I915_IRQ_DE, DE_PIPE_VBLANK_IVB(0));
    CHECK("display: a message", i915_model_take_message(m) == 1);
    i915_irq_handle(i915);
    CHECK("display: handled and acknowledged",
          calls[I915_IRQ_DE] == 1 && i915_model_peek(m, DEIIR) == 0 &&
              i915_model_peek(m, DEIER) == (DE_MASTER_IRQ_CONTROL | DE_PIPE_VBLANK_IVB(0)));
}

/** The south display's GMBUS interrupt, behind the PCH event. */
static void irq_south_rows(struct i915_model *m, struct drm_i915_private *i915)
{
    CHECK("south: GMBUS registered", i915_irq_register(i915, I915_IRQ_SDE, SDE_GMBUS_CPT, handler,
                                                       (void *)(long)I915_IRQ_SDE) == 0);
    CHECK("south: the PCH event enabled and unmasked with it",
          (i915_model_peek(m, DEIER) & DE_PCH_EVENT_IVB) &&
              !(i915_model_peek(m, DEIMR) & DE_PCH_EVENT_IVB) &&
              i915_model_peek(m, SDEIER) == SDE_GMBUS_CPT);
    i915_irq_unmask(i915, I915_IRQ_SDE, SDE_GMBUS_CPT);
    i915_model_irq_event(m, I915_IRQ_SDE, SDE_GMBUS_CPT);
    CHECK("south: a message", i915_model_take_message(m) == 1);
    i915_irq_handle(i915);
    CHECK("south: handled, both identities acknowledged, the enable back",
          calls[I915_IRQ_SDE] == 1 && got_iir[I915_IRQ_SDE] == SDE_GMBUS_CPT &&
              i915_model_peek(m, SDEIIR) == 0 && i915_model_peek(m, DEIIR) == 0 &&
              i915_model_peek(m, SDEIER) == SDE_GMBUS_CPT &&
              i915_model_irq_state(m)->pending == 0 && calls[I915_IRQ_DE] == 1 &&
              i915->irq.unclaimed == 1);
    i915_irq_unregister(i915, I915_IRQ_SDE, SDE_GMBUS_CPT);
    CHECK("south: the last unregistered takes the PCH event with it",
          !(i915_model_peek(m, DEIER) & DE_PCH_EVENT_IVB) &&
              (i915_model_peek(m, DEIMR) & DE_PCH_EVENT_IVB) && i915_model_peek(m, SDEIER) == 0);
}

/** Power management's, two events behind one message, and the reset. */
static void irq_pm_rows(struct i915_model *m, struct drm_i915_private *i915)
{
    CHECK("pm: the video enhancement ring's interrupt registered",
          i915_irq_register(i915, I915_IRQ_PM, PM_VEBOX_USER_INTERRUPT, handler,
                            (void *)(long)I915_IRQ_PM) == 0);
    i915_irq_unmask(i915, I915_IRQ_PM, PM_VEBOX_USER_INTERRUPT);
    i915_model_irq_event(m, I915_IRQ_PM, PM_VEBOX_USER_INTERRUPT);
    CHECK("pm: a message", i915_model_take_message(m) == 1);
    i915_irq_handle(i915);
    CHECK("pm: handled and acknowledged",
          calls[I915_IRQ_PM] == 1 && i915_model_peek(m, GEN6_PMIIR) == 0);

    i915_irq_unmask(i915, I915_IRQ_GT, GT_RENDER_USER_INTERRUPT);
    i915_model_irq_event(m, I915_IRQ_GT, GT_RENDER_USER_INTERRUPT);
    i915_model_irq_event(m, I915_IRQ_PM, PM_VEBOX_USER_INTERRUPT);
    CHECK("two: one message for both",
          i915_model_take_message(m) == 1 && i915_model_take_message(m) == 0);
    i915_irq_handle(i915);
    CHECK("two: both handled", calls[I915_IRQ_GT] == 2 && calls[I915_IRQ_PM] == 2);

    i915_irq_reset(i915);
    CHECK("reset: every handler forgotten and every source masked and off",
          !i915->irq.fn[I915_IRQ_GT][0] && !i915->irq.fn[I915_IRQ_PM][10] &&
              i915_model_peek(m, GTIER) == 0 && i915_model_peek(m, DEIER) == 0 &&
              i915_model_peek(m, GTIMR) == 0xffffffffu);
}

/** Register, unmask, raise and take each bank's sources. */
static void row_irq(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m);
    int b;

    for (b = 0; b < I915_IRQ_BANKS; b++) {
        calls[b] = 0;
        got_iir[b] = 0;
    }
    irq_register_rows(&m, i915);
    irq_gt_rows(&m, i915);
    irq_unclaimed_rows(&m, i915);
    irq_display_rows(&m, i915);
    irq_south_rows(&m, i915);
    irq_pm_rows(&m, i915);
    done(&m);
}

/* ---- breadcrumbs ---- */

/** A breadcrumb on each ring, and the waits the user interrupt wakes. */
static void crumbs_wait_rows(struct i915_model *m, struct drm_i915_private *i915)
{
    struct i915_engine *rcs = &i915->gt.engine[I915_RCS0], *bcs = &i915->gt.engine[I915_BCS0];
    uint32_t seqno = i915_engine_emit_breadcrumb(i915, rcs);

    CHECK("emit: the first sequence number", seqno == 1 && rcs->seqno == 1);
    CHECK("emit: MI_STORE_DWORD_INDEX to the status page's dword 0x40, then MI_USER_INTERRUPT",
          rcs->ring[0] == MI_STORE_DWORD_INDEX && rcs->ring[1] == 0x100u && rcs->ring[2] == 1u &&
              rcs->ring[3] == MI_USER_INTERRUPT);
    CHECK("emit: the tail handed to the chip",
          rcs->tail == 16u && i915_model_peek(m, RING_TAIL(RENDER_RING_BASE)) == 16u);
    CHECK("emit: not yet passed", !i915_engine_passed(rcs, seqno));

    wakes = 0;
    m->nlog = 0;
    m->log_lost = 0;
    CHECK("wait: woken", i915_engine_wait(i915, rcs, seqno, 1000000u) == 0);
    CHECK("wait: asleep, woken by the user interrupt",
          sleeps == 1 && rcs->irq_seen == 1 && wakes == 1 && stuck_sleeps == 0);
    CHECK("wait: the breadcrumb in the status page",
          rcs->hws[I915_GEM_HWS_SEQNO] == 1u && i915_engine_passed(rcs, 1));
    CHECK("wait: the chip's head at the tail",
          i915_model_peek(m, RING_HEAD(RENDER_RING_BASE)) == 16u &&
              i915_model_ring_state(m, 0)->executed == 2u);
    CHECK("wait: the interrupt masked again after",
          rcs->irq_refs == 0 && i915_model_peek(m, GTIMR) == 0xffffffffu &&
              i915_model_peek(m, RING_IMR(RENDER_RING_BASE)) == 0xffffffffu);
    CHECK("wait: unmasked during it, the ring's own mask first",
          at(m, RING_IMR(RENDER_RING_BASE), ~GT_RENDER_USER_INTERRUPT) <
                  at(m, GTIMR, ~GT_RENDER_USER_INTERRUPT) &&
              at(m, GTIMR, ~GT_RENDER_USER_INTERRUPT) < (1L << 30));

    m->nlog = 0;
    CHECK("wait: a breadcrumb passed returns at once",
          i915_engine_wait(i915, rcs, seqno, 1000u) == 0 && m->nlog == 0 && sleeps == 1);

    seqno = i915_engine_emit_breadcrumb(i915, bcs);
    CHECK("blitter: its own sequence", seqno == 1);
    CHECK("blitter: woken by its interrupt", i915_engine_wait(i915, bcs, seqno, 1000000u) == 0 &&
                                                 bcs->irq_seen == 1 && rcs->irq_seen == 1 &&
                                                 bcs->hws[I915_GEM_HWS_SEQNO] == 1u &&
                                                 rcs->hws[I915_GEM_HWS_SEQNO] == 1u);
}

/** A wait with no process polls; a stuck ring's waits end at their
 * timeouts, asleep and polling. */
static void crumbs_poll_stuck_rows(struct i915_model *m, struct drm_i915_private *i915)
{
    struct i915_engine *rcs = &i915->gt.engine[I915_RCS0];
    uint32_t seqno;
    uint64_t t0;

    cpu_table[0].current = 0;
    seqno = i915_engine_emit_breadcrumb(i915, rcs);
    CHECK("poll: with no process the wait polls the status page",
          i915_engine_wait(i915, rcs, seqno, 100000u) == 0 && sleeps == 2);
    cpu_table[0].current = &caller;

    i915_model_ring_state(m, 0)->stuck = 1;
    seqno = i915_engine_emit_breadcrumb(i915, rcs);
    t0 = m->now_us;
    CHECK("stuck: the wait ends at its timeout",
          i915_engine_wait(i915, rcs, seqno, 20000u) == -ETIME);
    CHECK("stuck: after the timeout and before twice it",
          m->now_us - t0 >= 20000u && m->now_us - t0 <= 40000u && stuck_sleeps == 0);
    CHECK("stuck: the interrupt masked again",
          rcs->irq_refs == 0 && i915_model_peek(m, GTIMR) == 0xffffffffu);
    cpu_table[0].current = 0;
    CHECK("stuck: polling ends at its timeout, by name",
          i915_engine_wait(i915, rcs, seqno, 20000u) == -ETIME &&
              strcmp(i915->hw.last_timeout, "breadcrumb") == 0);
    cpu_table[0].current = &caller;
    i915_model_ring_state(m, 0)->stuck = 0;
    CHECK("stuck: freed, it runs on", i915_engine_wait(i915, rcs, seqno, 1000000u) == 0);
}

/** The ring wraps under 24000 bytes of breadcrumbs, and fills while the
 * chip is stuck. */
static void crumbs_wrap_full_rows(struct i915_model *m, struct drm_i915_private *i915)
{
    struct i915_engine *rcs = &i915->gt.engine[I915_RCS0];
    uint32_t seqno = 0;
    int i;

    for (i = 0; i < 1500; i++) {
        seqno = i915_engine_emit_breadcrumb(i915, rcs);
        if (seqno == 0 || i915_engine_wait(i915, rcs, seqno, 1000000u) != 0) {
            break;
        }
    }
    CHECK("wrap: 1500 breadcrumbs, 24000 bytes, through a 16 KiB ring",
          i == 1500 && rcs->hws[I915_GEM_HWS_SEQNO] == seqno &&
              i915_model_ring_state(m, 0)->faulted == 0);

    i915_model_ring_state(m, 0)->stuck = 1;
    for (i = 0; i < 2000; i++) {
        if (i915_engine_emit_breadcrumb(i915, rcs) == 0) {
            break;
        }
    }
    CHECK("full: the ring takes what fits, then refuses", i > 1000 && i < 1024);
    CHECK("full: after a wait for room, by name", strcmp(i915->hw.last_timeout, "ring space") == 0);
    i915_model_ring_state(m, 0)->stuck = 0;
    seqno = rcs->seqno;
    CHECK("full: freed, everything runs", i915_engine_wait(i915, rcs, seqno, 1000000u) == 0 &&
                                              i915_engine_emit_breadcrumb(i915, rcs) != 0);
    CHECK("begin: an odd count, none, or more than half the ring refused",
          !i915_ring_begin(rcs, i915, 3) && !i915_ring_begin(rcs, i915, 0) &&
              !i915_ring_begin(rcs, i915, I915_RING_SIZE / 8u + 2u));
}

/** A command that would cross the ring's end starts again at 0, the end
 * filled with MI_NOOP; and sequence numbers wrap past 0 in order. */
static void crumbs_cross_rows(struct i915_model *m, struct drm_i915_private *i915)
{
    struct i915_engine *rcs = &i915->gt.engine[I915_RCS0];
    uint32_t seqno, *cs;
    int i;

    for (i = 0; i < 4096 && rcs->tail != I915_RING_SIZE - 8u; i++) {
        cs = i915_ring_begin(rcs, i915, 2);
        if (!cs) {
            break;
        }
        cs[0] = MI_NOOP;
        cs[1] = MI_NOOP;
        i915_ring_advance(rcs, i915);
    }
    CHECK("cross: the tail 8 bytes from the end", rcs->tail == I915_RING_SIZE - 8u);
    rcs->ring[I915_RING_SIZE / 4u - 2u] = 0xdeadbeefu;
    rcs->ring[I915_RING_SIZE / 4u - 1u] = 0xdeadbeefu;
    seqno = i915_engine_emit_breadcrumb(i915, rcs);
    CHECK("cross: the end filled with MI_NOOP, the breadcrumb at 0",
          seqno != 0 && rcs->ring[I915_RING_SIZE / 4u - 2u] == MI_NOOP &&
              rcs->ring[I915_RING_SIZE / 4u - 1u] == MI_NOOP &&
              rcs->ring[0] == MI_STORE_DWORD_INDEX && rcs->ring[2] == seqno && rcs->tail == 16u);
    CHECK("cross: the chip runs through the end",
          i915_engine_wait(i915, rcs, seqno, 1000000u) == 0 &&
              !i915_model_ring_state(m, 0)->faulted);

    rcs->seqno = 0xffffffffu;
    seqno = i915_engine_emit_breadcrumb(i915, rcs);
    CHECK("seqno: wraps past 0", seqno == 1);
    CHECK("seqno: ordered across the wrap", i915_engine_wait(i915, rcs, seqno, 1000000u) == 0 &&
                                                i915_engine_passed(rcs, 0xfffffff0u));
}

/** Breadcrumbs on each ring, the waits that the interrupt and polling end,
 * a stuck ring's timeout, and the ring's wrap and room. */
static void row_breadcrumbs(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m);

    CHECK("crumbs: the rings start", i915_gt_init(i915) == 0);
    irq_on = 1;
    crumbs_wait_rows(&m, i915);
    crumbs_poll_stuck_rows(&m, i915);
    crumbs_wrap_full_rows(&m, i915);
    crumbs_cross_rows(&m, i915);
    i915_gt_fini(i915);
    CHECK("crumbs: a ring stopped is not waited on",
          i915_engine_wait(i915, &i915->gt.engine[I915_RCS0], 5, 1000u) == -EIO);
    done(&m);
}

/* ---- first light ---- */

/** Force the GT's identity to read 0, so that no user interrupt is seen. */
static uint32_t no_gt_iir(struct i915_model *m, uint32_t reg, uint32_t value)
{
    (void)m;
    return reg == GTIIR ? 0 : value;
}

/** First light end to end on the model, and each of its failures. */
static void row_firstlight(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m);
    struct i915_engine *rcs;
    uint32_t bottom, seen;

    CHECK("light: the rings start", i915_gt_init(i915) == 0);
    rcs = &i915->gt.engine[I915_RCS0];
    CHECK("light: off without the option", i915_firstlight(i915) == -ENODEV && rcs->tail == 0);

    cmdline_firstlight = 1;
    bottom = (uint32_t)i915->gt.ggtt_bottom;
    CHECK("light: ok", i915_firstlight(i915) == 0);
    CHECK("light: MI_STORE_DWORD_IMM through the GTT, then the breadcrumb",
          rcs->ring[0] == (MI_STORE_DWORD_IMM_GEN4 | MI_USE_GGTT) && rcs->ring[1] == 0 &&
              rcs->ring[2] == bottom - 4096u && rcs->ring[3] == 0x1915f157u &&
              rcs->ring[4] == MI_STORE_DWORD_INDEX && rcs->ring[7] == MI_USER_INTERRUPT);
    CHECK("light: logged, the interrupt latched with the processor's off",
          logged("first light: ok: rcs0 wrote 0x1915f157") && logged("user interrupt latched"));
    CHECK("light: the page given back, its entry on the scratch page",
          i915->gt.ggtt_bottom == bottom &&
              i915_ggtt_pte(i915, bottom - 4096u) == i915->ggtt.scratch_pte &&
              pages_live == TEST_GT_PAGES && rcs->irq_refs == 0 &&
              i915_model_peek(&m, GTIMR) == 0xffffffffu);

    /* The processor's interrupts on: the message the first raised is
     * taken, then first light again. */
    irq_on = 1;
    i915_delay_us(&i915->hw, 1);
    seen = rcs->irq_seen;
    log_text[0] = 0;
    CHECK("light: ok again, the interrupt taken", i915_firstlight(i915) == 0 &&
                                                      logged("user interrupt taken") && seen == 1 &&
                                                      rcs->irq_seen == seen + 1);
    irq_on = 0;

    i915_model_ring_state(&m, 0)->stuck = 1;
    CHECK("light: a stuck ring fails", i915_firstlight(i915) == -ETIME);
    CHECK("light: by name", logged("first light: no breadcrumb: head") &&
                                strcmp(i915->hw.last_timeout, "first light breadcrumb") == 0);
    i915_model_ring_state(&m, 0)->stuck = 0;
    i915_delay_us(&i915->hw, 1);
    CHECK("light: the page back after a failure too", pages_live == TEST_GT_PAGES);
    CHECK("light: the late write went to the scratch page",
          scratch_page[0] == 0x1915f157u && !i915_model_ring_state(&m, 0)->faulted);

    drop_phys = next_phys;
    log_text[0] = 0;
    CHECK("light: a write lost fails",
          i915_firstlight(i915) == -EIO && logged("first light: read back 0x00000000"));
    drop_phys = 0;

    m.on_read = no_gt_iir;
    log_text[0] = 0;
    CHECK("light: no interrupt fails",
          i915_firstlight(i915) == -ETIME && logged("first light: no user interrupt") &&
              strcmp(i915->hw.last_timeout, "first light interrupt") == 0);
    m.on_read = 0;

    i915_gt_fini(i915);
    log_text[0] = 0;
    CHECK("light: no ring, no light",
          i915_firstlight(i915) == -EIO && logged("the render ring is not running"));
    done(&m);
}

/** Nothing: one thread has no one to give way to. */
void sched_give_way(void)
{}

/** No thread holds a lock here: a clear of the GTT gives no way. */
int mutex_held(const struct mutex *m)
{
    (void)m;
    return 0;
}

int main(void)
{
    row_start();
    row_start_failures();
    row_irq();
    row_breadcrumbs();
    row_firstlight();
    if (fails) {
        printf("i915-ring-test: %d failures\n", fails);
        return 1;
    }
    printf("i915-ring-test: ok\n");
    return 0;
}
