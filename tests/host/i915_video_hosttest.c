/**
 * i915_video_hosttest.c: the Intel graphics driver's video ring and video
 * enhancement ring, i915_ring.c with i915_irq.c, against the host model of
 * the Lenovo's HD 4400, and their command parser tables,
 * i915_cmd_parser.c, judged as Linux 6.12's parser judges batches on
 * Haswell's vcs0 and vecs0.
 *
 * host-deps: rach2/sys/drm/i915/i915_ring.c rach2/sys/drm/i915/i915_irq.c
 * host-deps: rach2/sys/drm/i915/i915_firstlight.c
 * host-deps: rach2/sys/drm/i915/i915_ppgtt.c
 * host-deps: rach2/sys/drm/i915/i915_uncore.c rach2/sys/drm/i915/i915_power.c
 * host-deps: rach2/sys/drm/i915/i915_gtt.c rach2/sys/drm/i915/i915_hw.c
 * host-deps: tests/host/i915_model.c rach2/sys/lib/kstring.c
 * host-deps: rach2/sys/lib/kctypes.c tests/host/compat/spinlock_host.c
 *
 * The device is brought up as i915_ring_hosttest.c brings it up: a fresh
 * model, forcewake, the global GTT and the interrupts, then the rings.
 *
 * The rows, one for each of 78-14a's first two subtasks. 78-14a.1: vcs0
 * and vecs0 started as Linux's xcs_resume starts a ring, each with the
 * identity Linux's setup_vcs and setup_vecs give it; the video ring's user
 * interrupt in the GT's bank and the video enhancement ring's in power
 * management's, enabled and masked after the start; a breadcrumb on each
 * whose wait the user interrupt ends, through its own bank, the ring's own
 * mask unmasked before the bank's and both masked again after; a reset of
 * the media domain and of the video enhancement domain reaching its ring
 * alone; and both rings stopped. 78-14a.2: the parser's tables for both
 * sorted; every entry of each ring's tables judged as its action says; no
 * register allowed on either ring; the masked checks of MI_FLUSH_DW,
 * MI_STORE_DWORD_IMM and MI_CONDITIONAL_BATCH_BUFFER_END; MFX_WAIT one
 * dword on the video ring; the lengths of commands no table holds; and
 * gen7_exec_parse's basic-rejected on both rings.
 *
 * Returns 0 when every row passes, 1 otherwise.
 */

#include <stdarg.h>

#include "drm/i915/i915_cmd_parser.c"
#include "drm/i915/i915.h"
#include "drm/i915/i915_drm.h"
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
#include "i915_model_ring.h"

extern int printf(const char *format, ...);
extern int vsnprintf(char *s, unsigned long n, const char *format, va_list ap);
extern int posix_memalign(void **p, unsigned long align, unsigned long size);
extern void *calloc(unsigned long count, unsigned long size);
extern void *malloc(unsigned long size);
extern void free(void *p);
extern char *strstr(const char *s, const char *t);
extern int strcmp(const char *a, const char *b);

static int fails;

#define CHECK(what, cond)                                \
    do {                                                 \
        if (!(cond)) {                                   \
            printf("i915-video-test: fail: %s\n", what); \
            fails++;                                     \
        }                                                \
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

/* ---- the parser's kernel ---- */

/* The temporary mappings the parser holds: a physical address here is the
 * host address of a page of the test's. */
static int maps_live;

void *kmap_temp(uintptr_t phys)
{
    maps_live++;
    return (void *)phys;
}

void kunmap_temp(void *va)
{
    (void)va;
    maps_live--;
}

void arch_dma_flush(const volatile void *p, size_t len)
{
    (void)p;
    (void)len;
}

/* ---- 78-14a.1: the rings ---- */

/** Check that `e` was started as Linux's xcs_resume starts a ring: stopped,
 * emptied, its status page set and flushed, then started, valid and told
 * to go, every interrupt of the ring masked and none written to the status
 * page, and its pages mapped. */
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

    CHECK("start: running, empty", e->running && e->head == 0 && e->tail == 0);
    CHECK("start: stopped, emptied, then the status page, the flush, the start, RING_VALID and "
          "the go",
          stop < off && off < hws && hws < flush && flush < start && start < valid && valid < go &&
              go < (1L << 30));
    CHECK("start: the status page's address and the ring's start, size and head",
          i915_model_peek(m, e->hws_pga) == e->hws_ggtt &&
              i915_model_peek(m, RING_START(b)) == e->ring_ggtt &&
              i915_model_peek(m, RING_CTL(b)) == (0x3000u | RING_VALID) &&
              i915_model_peek(m, RING_HEAD(b)) == 0 && i915_model_peek(m, RING_TAIL(b)) == 0);
    CHECK("start: no interrupt written to the status page, every one masked",
          i915_model_peek(m, RING_HWSTAM(b)) == 0xffffffffu &&
              i915_model_peek(m, RING_IMR(b)) == 0xffffffffu);
    CHECK("start: the status page and the ring mapped",
          mapped(i915, e->hws, 1, e->hws_ggtt) && mapped(i915, e->ring, 4, e->ring_ggtt));
}

/** Check that `vcs` and `vecs` have the registers, interrupts, names,
 * classes, engine ids, reset domains and fault registers of Linux's
 * setup_vcs and setup_vecs on Haswell. */
static void check_identity(const struct i915_engine *vcs, const struct i915_engine *vecs)
{
    CHECK("rings: the video ring's registers, status page and interrupt",
          vcs->mmio_base == GEN6_BSD_RING_BASE && vcs->hws_pga == BSD_HWS_PGA_GEN7 &&
              vcs->irq_bit == GT_BSD_USER_INTERRUPT && vcs->irq_bank == I915_IRQ_GT);
    CHECK("rings: the video enhancement ring's",
          vecs->mmio_base == VEBOX_RING_BASE && vecs->hws_pga == VEBOX_HWS_PGA_GEN7 &&
              vecs->irq_bit == PM_VEBOX_USER_INTERRUPT && vecs->irq_bank == I915_IRQ_PM);
    CHECK("rings: Linux's names, classes and ids, vcs0 10 and vecs0 18",
          strcmp(vcs->name, "vcs0") == 0 && strcmp(vecs->name, "vecs0") == 0 &&
              vcs->uabi_class == I915_ENGINE_CLASS_VIDEO &&
              vecs->uabi_class == I915_ENGINE_CLASS_VIDEO_ENHANCE && vcs->linux_id == 10 &&
              vecs->linux_id == 18);
    CHECK("rings: the reset domains and fault registers",
          vcs->reset_domain == GEN6_GRDOM_MEDIA && vecs->reset_domain == GEN6_GRDOM_VECS &&
              vcs->fault_reg == RING_FAULT_REG_VCS && vecs->fault_reg == RING_FAULT_REG_VECS);
}

/** vcs0 and vecs0 started, with Linux's identities, their interrupts in
 * their own banks, and stopped. */
static void row_rings(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m);
    struct i915_engine *vcs = &i915->gt.engine[I915_VCS0], *vecs = &i915->gt.engine[I915_VECS0];

    m.nlog = 0;
    CHECK("rings: started", i915_gt_init(i915) == 0);
    check_started(&m, i915, vcs);
    check_started(&m, i915, vecs);
    check_identity(vcs, vecs);
    CHECK("rings: the video ring's user interrupt enabled in the GT's bank, the other's in "
          "power management's, both masked",
          (i915_model_peek(&m, GTIER) & GT_BSD_USER_INTERRUPT) &&
              i915_model_peek(&m, GEN6_PMIER) == PM_VEBOX_USER_INTERRUPT &&
              i915_model_peek(&m, GTIMR) == 0xffffffffu &&
              i915_model_peek(&m, GEN6_PMIMR) == 0xffffffffu);
    CHECK("rings: the handlers registered with their rings",
          i915->irq.fn[I915_IRQ_GT][12] && i915->irq.data[I915_IRQ_GT][12] == vcs &&
              i915->irq.fn[I915_IRQ_PM][10] && i915->irq.data[I915_IRQ_PM][10] == vecs);
    CHECK("rings: no wait reached its bound", i915->hw.timeouts == 0);

    i915_gt_fini(i915);
    CHECK("stop: both rings disabled and their interrupts off",
          i915_model_peek(&m, RING_CTL(GEN6_BSD_RING_BASE)) == 0 &&
              i915_model_peek(&m, RING_CTL(VEBOX_RING_BASE)) == 0 &&
              i915_model_peek(&m, GTIER) == 0 && i915_model_peek(&m, GEN6_PMIER) == 0 &&
              !vcs->running && !vecs->running);
    CHECK("stop: the handlers gone",
          !i915->irq.fn[I915_IRQ_GT][12] && !i915->irq.fn[I915_IRQ_PM][10]);
    CHECK("stop: the pages given back", pages_live == 0);
    done(&m);
}

/** Wait on a breadcrumb on `e`, whose user interrupt is `bit` in the bank
 * whose mask is `imr`, and check that the interrupt ended the wait. */
static void check_wait(struct i915_model *m, struct drm_i915_private *i915, struct i915_engine *e,
                       uint32_t imr, int ring)
{
    uint32_t seqno = i915_engine_emit_breadcrumb(i915, e);
    long own, bank;

    CHECK("wait: the first sequence number", seqno == 1);
    m->nlog = 0;
    m->log_lost = 0;
    wakes = 0;
    CHECK("wait: woken", i915_engine_wait(i915, e, seqno, 1000000u) == 0);
    CHECK("wait: by the ring's user interrupt",
          e->irq_seen == 1 && wakes == 1 && stuck_sleeps == 0);
    CHECK("wait: the breadcrumb in the status page and run by the chip",
          e->hws[I915_GEM_HWS_SEQNO] == 1u && i915_model_ring_state(m, ring)->executed > 0 &&
              !i915_model_ring_state(m, ring)->faulted);
    own = at(m, RING_IMR(e->mmio_base), ~e->irq_bit);
    bank = at(m, imr, ~e->irq_bit);
    CHECK("wait: the ring's own mask unmasked before its bank's", own < bank && bank < (1L << 30));
    CHECK("wait: both masked again after",
          e->irq_refs == 0 && i915_model_peek(m, imr) == 0xffffffffu &&
              i915_model_peek(m, RING_IMR(e->mmio_base)) == 0xffffffffu);
}

/** A breadcrumb on each ring, whose user interrupt ends its wait; and a
 * reset of each ring's domain reaching that ring alone. */
static void row_waits_resets(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m);
    struct i915_engine *vcs = &i915->gt.engine[I915_VCS0], *vecs = &i915->gt.engine[I915_VECS0];

    CHECK("waits: the rings start", i915_gt_init(i915) == 0);
    irq_on = 1;
    check_wait(&m, i915, vcs, GTIMR, 2);
    check_wait(&m, i915, vecs, GEN6_PMIMR, 3);
    CHECK("waits: no other ring woken",
          i915->gt.engine[I915_RCS0].irq_seen == 0 && i915->gt.engine[I915_BCS0].irq_seen == 0);

    i915_uncore_write(i915, GEN6_GDRST, vcs->reset_domain);
    CHECK("reset: the media domain resets the video ring alone",
          i915_model_peek(&m, RING_CTL(GEN6_BSD_RING_BASE)) == 0 &&
              i915_model_peek(&m, RING_CTL(VEBOX_RING_BASE)) == (0x3000u | RING_VALID) &&
              i915_model_peek(&m, RING_CTL(RENDER_RING_BASE)) == (0x3000u | RING_VALID) &&
              i915_model_peek(&m, RING_CTL(BLT_RING_BASE)) == (0x3000u | RING_VALID));
    i915_uncore_write(i915, GEN6_GDRST, vecs->reset_domain);
    CHECK("reset: the video enhancement domain resets its ring alone",
          i915_model_peek(&m, RING_CTL(VEBOX_RING_BASE)) == 0 &&
              i915_model_peek(&m, RING_CTL(RENDER_RING_BASE)) == (0x3000u | RING_VALID) &&
              i915_model_peek(&m, RING_CTL(BLT_RING_BASE)) == (0x3000u | RING_VALID));
    i915_gt_fini(i915);
    done(&m);
}

/* ---- 78-14a.2: the parser's tables ---- */

#define PPAGE 4096u
#define SCRUB 0xc5c5c5c5u
#define DIRT  0xa5a5a5a5u

/* The verdicts: the call's error, the copy run privileged, or the original
 * run unprivileged. */
enum { V_ERROR, V_PRIV, V_UNPRIV };

/* The engines, by their bases as the driver's are. */
static struct i915_engine p_vcs, p_vecs;
static int32_t last_ret;

/**
 * Parse the `n` dwords `cmds`, padded with MI_NOOP to a quadword, on `e`,
 * from a page of SCRUB into a dirty shadow of one page, and read the
 * verdict from the shadow as the parser test does: a batch that passes is
 * copied whole with MI_BATCH_BUFFER_END after it; a refused one has its
 * copy ended at once and the original started unprivileged after it.
 */
static int parse(struct i915_engine *e, const uint32_t *cmds, uint32_t n)
{
    struct i915_cmd_parse p;
    uintptr_t batch, copy;
    void *b = 0, *s = 0;
    uint32_t *bw, *sw, k, len = (4u * n + 7u) & ~7u, end = len / 4u;
    int v = V_ERROR;

    if (posix_memalign(&b, PPAGE, PPAGE) != 0 || posix_memalign(&s, PPAGE, PPAGE) != 0) {
        printf("i915-video-test: out of memory\n");
        fails++;
        free(b);
        return V_ERROR;
    }
    bw = (uint32_t *)b;
    sw = (uint32_t *)s;
    for (k = 0; k < PPAGE / 4u; k++) {
        bw[k] = SCRUB;
        sw[k] = DIRT;
    }
    for (k = 0; k < n; k++) {
        bw[k] = cmds[k];
    }
    if (end > n) {
        bw[n] = MI_NOOP;
    }
    batch = (uintptr_t)b;
    copy = (uintptr_t)s;
    p.batch = &batch;
    p.offset = 0;
    p.len = len;
    p.addr = 0x100000u;
    p.clflush = 0;
    p.shadow = &copy;
    p.shadow_size = PPAGE;
    last_ret = i915_cmd_parser(e, &p);
    CHECK("parse: no temporary mapping held", maps_live == 0);
    if (last_ret == 0 && sw[end] == MI_BATCH_BUFFER_END) {
        v = sw[0] == bw[0] ? V_PRIV : V_ERROR;
    } else if (last_ret == 0 && sw[0] == MI_BATCH_BUFFER_END &&
               sw[end] == (MI_BATCH_BUFFER_START | MI_BATCH_NON_SECURE_HSW) &&
               sw[end + 1u] == 0x100000u) {
        v = V_UNPRIV;
    }
    free(b);
    free(s);
    return v;
}

/** The verdict on one command of `ndw` dwords, its first `header` and the
 * rest `rest`, or zero for NULL, then MI_BATCH_BUFFER_END. */
static int parse_cmd(struct i915_engine *e, uint32_t header, uint32_t ndw, const uint32_t *rest)
{
    uint32_t b[300], k;

    b[0] = header;
    for (k = 1; k < ndw; k++) {
        b[k] = rest ? rest[k - 1u] : 0;
    }
    b[ndw] = MI_BATCH_BUFFER_END;
    return parse(e, b, ndw + 1u);
}

/** The tables of both rings sorted, which the search needs; both rings
 * parse; and every entry of each judged as its action says. */
static void row_tables(void)
{
    const struct cmd_engine *engines[2];
    struct i915_engine *rings[2];
    const struct cmd_desc *d;
    uint32_t e, i, j, prev, cur;
    int sorted = 1, bad = 0, v, want;

    engines[0] = cmd_engine_of(&p_vcs);
    engines[1] = cmd_engine_of(&p_vecs);
    rings[0] = &p_vcs;
    rings[1] = &p_vecs;
    CHECK("tables: Linux's gen7_video_cmd_table and hsw_vebox_cmd_table",
          engines[0] == &gen7_video_engine && engines[1] == &hsw_vebox_engine);
    CHECK("tables: both rings parse",
          i915_engine_using_cmd_parser(&p_vcs) && i915_engine_using_cmd_parser(&p_vecs));
    CHECK("tables: no allowlist on either ring", engines[0]->nregs == 0 && engines[1]->nregs == 0);
    for (e = 0; e < 2; e++) {
        for (i = 0; i < engines[e]->ncmds; i++) {
            prev = 0;
            for (j = 0; j < engines[e]->cmds[i].count; j++) {
                d = &engines[e]->cmds[i].desc[j];
                cur = d->value & d->mask;
                sorted &= cur >= prev;
                prev = cur;
            }
        }
    }
    CHECK("tables: every table sorted", sorted);

    /* Each entry, four dwords long unless its length is fixed, its fields
     * zero: a passed or masked command passes, and a refused one, one that
     * names a register, and MI_BATCH_BUFFER_START are refused. */
    for (e = 0; e < 2; e++) {
        for (i = 0; i < engines[e]->ncmds; i++) {
            for (j = 0; j < engines[e]->cmds[i].count; j++) {
                d = &engines[e]->cmds[i].desc[j];
                if (d->flags & CMD_DESC_FIXED) {
                    v = parse_cmd(rings[e], d->value, d->length, 0);
                } else {
                    v = parse_cmd(rings[e], d->value | 2u, 4u, 0);
                }
                if (d->flags & (CMD_DESC_REJECT | CMD_DESC_REGISTER) ||
                    d->value == MI_BATCH_BUFFER_START) {
                    want = V_UNPRIV;
                } else {
                    want = V_PRIV;
                }
                if (v != want) {
                    printf("i915-video-test: entry 0x%08x on %s: verdict %d, not %d\n",
                           (unsigned)d->value, rings[e]->name, v, want);
                    bad++;
                }
            }
        }
    }
    CHECK("tables: every entry of both rings judged by its action", bad == 0);
}

/** No register on either ring, the masked checks, and the refused MI
 * commands of the video tables. */
static void row_checks(void)
{
    struct i915_engine *rings[2];
    uint32_t r[4] = {0, 0, 0, 0}, e;
    int bad = 0;

    rings[0] = &p_vcs;
    rings[1] = &p_vecs;
    for (e = 0; e < 2; e++) {
        r[0] = RING_TIMESTAMP(rings[e]->mmio_base);
        bad += parse_cmd(rings[e], MI_LOAD_REGISTER_IMM(1), 3, r) != V_UNPRIV;
        bad += parse_cmd(rings[e], MI_STORE_REGISTER_MEM, 3, r) != V_UNPRIV;
        bad += parse_cmd(rings[e], MI_LOAD_REGISTER_MEM, 3, r) != V_UNPRIV;
        r[0] = GEN7_SO_WRITE_OFFSET(0);
        bad += parse_cmd(rings[e], MI_LOAD_REGISTER_IMM(1), 3, r) != V_UNPRIV;
        r[0] = 0;
        bad += parse_cmd(rings[e], MI_STORE_DWORD_IMM_GEN4, 4, r) != V_PRIV;
        bad += parse_cmd(rings[e], MI_STORE_DWORD_IMM_GEN4 | MI_USE_GGTT, 4, r) != V_UNPRIV;
        bad += parse_cmd(rings[e], MI_CONDITIONAL_BATCH_BUFFER_END, 2, r) != V_PRIV;
        bad += parse_cmd(rings[e], MI_CONDITIONAL_BATCH_BUFFER_END | MI_USE_GGTT, 2, r) != V_UNPRIV;
        bad += parse_cmd(rings[e], MI_FLUSH_DW, 3, r) != V_PRIV;
        bad += parse_cmd(rings[e], MI_FLUSH_DW | MI_FLUSH_DW_NOTIFY, 3, r) != V_UNPRIV;
        bad += parse_cmd(rings[e], MI_FLUSH_DW | MI_FLUSH_DW_STORE_INDEX, 3, r) != V_PRIV;
        bad += parse_cmd(rings[e], MI_FLUSH_DW | MI_FLUSH_DW_STORE_INDEX | MI_FLUSH_DW_OP_STOREDW,
                         3, r) != V_UNPRIV;
        r[0] = MI_FLUSH_DW_USE_GTT;
        bad += parse_cmd(rings[e], MI_FLUSH_DW, 3, r) != V_PRIV;
        bad += parse_cmd(rings[e], MI_FLUSH_DW | MI_FLUSH_DW_OP_STOREDW, 3, r) != V_UNPRIV;
        r[0] = 0;
        bad += parse_cmd(rings[e], MI_USER_INTERRUPT, 1, 0) != V_UNPRIV;
        bad += parse_cmd(rings[e], MI_ARB_ON_OFF, 1, 0) != V_UNPRIV;
        bad += parse_cmd(rings[e], MI_UPDATE_GTT | 2u, 4, 0) != V_UNPRIV;
        bad += parse_cmd(rings[e], MI_STORE_DWORD_INDEX, 3, 0) != V_UNPRIV;
        bad += parse_cmd(rings[e], MI_SET_APPID, 1, 0) != V_PRIV;
        if (bad) {
            printf("i915-video-test: %u checks wrong on %s\n", (unsigned)bad, rings[e]->name);
            fails++;
            bad = 0;
        }
    }
}

/** The lengths of commands no table holds, Linux's
 * gen7_bsd_get_cmd_length_mask, MFX_WAIT's on each ring, and
 * gen7_exec_parse's basic-rejected. */
static void row_lengths(void)
{
    uint32_t media =
        (INSTR_RC_CLIENT << INSTR_CLIENT_SHIFT) | (INSTR_MEDIA_SUBCLIENT << INSTR_SUBCLIENT_SHIFT);
    uint32_t threed = INSTR_RC_CLIENT << INSTR_CLIENT_SHIFT;
    uint32_t invalid[2];

    invalid[0] = 7u << INSTR_CLIENT_SHIFT;
    invalid[1] = MI_BATCH_BUFFER_END;
    CHECK("lengths: a media command of opcode 6 has 16 bits of length",
          parse_cmd(&p_vcs, media | (6u << 24) | 0x10u, 18, 0) == V_PRIV &&
              parse_cmd(&p_vcs, media | (6u << 24) | 0x1010u, 18, 0) == V_ERROR &&
              last_ret == -EINVAL);
    CHECK("lengths: another media command 12",
          parse_cmd(&p_vcs, media | (5u << 24) | 0x1010u, 18, 0) == V_PRIV &&
              parse_cmd(&p_vecs, media | (3u << 24) | 0x1010u, 18, 0) == V_PRIV &&
              parse_cmd(&p_vcs, media | (5u << 24) | 0x110u, 18, 0) == V_ERROR);
    CHECK("lengths: another render client command 8",
          parse_cmd(&p_vcs, threed | (1u << 24) | 0x110u, 18, 0) == V_PRIV &&
              parse_cmd(&p_vecs, threed | (1u << 24) | 0x110u, 18, 0) == V_PRIV);
    CHECK("lengths: an MI command 6, refused as unknown on neither",
          parse_cmd(&p_vcs, MI_INSTR(0x2f, 0) | 0x40u, 2, 0) == V_PRIV &&
              parse_cmd(&p_vecs, MI_INSTR(0x2f, 0) | 0x40u, 2, 0) == V_PRIV);
    CHECK("lengths: a blitter command EINVAL on both",
          parse_cmd(&p_vcs, (INSTR_BC_CLIENT << INSTR_CLIENT_SHIFT) | 2u, 4, 0) == V_ERROR &&
              last_ret == -EINVAL &&
              parse_cmd(&p_vecs, (INSTR_BC_CLIENT << INSTR_CLIENT_SHIFT) | 2u, 4, 0) == V_ERROR &&
              last_ret == -EINVAL);
    CHECK("mfx: MFX_WAIT one dword on the video ring, whatever its length field",
          parse_cmd(&p_vcs, MFX_WAIT, 1, 0) == V_PRIV &&
              parse_cmd(&p_vcs, MFX_WAIT | 5u, 1, 0) == V_PRIV);
    CHECK("mfx: on the video enhancement ring, a render client command of 8 bits",
          parse_cmd(&p_vecs, MFX_WAIT | 5u, 7, 0) == V_PRIV &&
              parse_cmd(&p_vecs, MFX_WAIT | 5u, 1, 0) == V_ERROR);
    CHECK("igt: basic-rejected, an invalid client on the video ring EINVAL",
          parse(&p_vcs, invalid, 2) == V_ERROR && last_ret == -EINVAL);
    CHECK("igt: basic-rejected, an invalid client on the video enhancement ring EINVAL",
          parse(&p_vecs, invalid, 2) == V_ERROR && last_ret == -EINVAL);
}

int main(void)
{
    p_vcs.name = "vcs0";
    p_vcs.mmio_base = GEN6_BSD_RING_BASE;
    p_vecs.name = "vecs0";
    p_vecs.mmio_base = VEBOX_RING_BASE;
    row_rings();
    row_waits_resets();
    row_tables();
    row_checks();
    row_lengths();
    if (fails) {
        printf("i915-video-test: %d failures\n", fails);
        return 1;
    }
    printf("i915-video-test: ok\n");
    return 0;
}
