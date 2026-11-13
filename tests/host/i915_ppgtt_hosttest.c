/**
 * i915_ppgtt_hosttest.c: the Intel graphics driver's aliasing PPGTT,
 * i915_ppgtt.c, on the host model of the Lenovo's HD 4400, whose rings
 * walk the PPGTT from the directory their registers name and run batches
 * through it.
 *
 * host-deps: rach2/sys/drm/i915/i915_irq.c rach2/sys/drm/i915/i915_ppgtt.c
 * host-deps: rach2/sys/drm/i915/i915_uncore.c rach2/sys/drm/i915/i915_power.c
 * host-deps: rach2/sys/drm/i915/i915_gtt.c rach2/sys/drm/i915/i915_hw.c
 * host-deps: tests/host/i915_model.c rach2/sys/lib/kstring.c
 * host-deps: rach2/sys/lib/kctypes.c tests/host/compat/spinlock_host.c
 *
 * A row makes a fresh model and a device record over it, and brings it up
 * as the bind does: forcewake, the global GTT, the interrupts, then the
 * rings, whose start makes the PPGTT. The kernel around the driver is
 * i915_ring_hosttest.c's: the heap hands out pages whose physical
 * addresses are spread apart, which the model reaches back through the
 * test, and a sleep moves the model's clock. The test includes
 * i915_ring.c, so that a row restarts a ring as a reset does.
 *
 * The rows: the PPGTT made (its directory in the GT's space at a 64 KiB
 * boundary, every entry naming the scratch table, each ring's PP_DIR_DCLV,
 * PP_DIR_BASE and PPGTT bit written between its start and RING_VALID, the
 * PPGTT's caching) and taken down; bindings mirrored (the tables made as a
 * range first reaches them and named in the directory, the entries the
 * global GTT's, cleared to the scratch page with the tables kept, the
 * refusals and no memory); a batch's addresses resolved through the PPGTT
 * on both rings to the global GTT's pages, and the GT's own objects out of
 * its reach; and after a reset of the rings, the PPGTT on again and a
 * batch through it.
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
#include "i915_model_ring.h"

#include "drm/i915/i915_ring.c"

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

#define CHECK(what, cond)                                \
    do {                                                 \
        if (!(cond)) {                                   \
            printf("i915-ppgtt-test: fail: %s\n", what); \
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

/* The allocations the heap grants before it refuses, or -1 for no limit. */
static int pages_left = -1;

/* The pages the started GT holds: each ring's four and its status page,
 * and the PPGTT's scratch table. */
#define TEST_GT_PAGES 21
static uint64_t next_phys = 0x50000000ull;

void *kalloc_pages(uint32_t count, uintptr_t *phys)
{
    void *block = 0;
    uint32_t i;

    if (pages_left == 0) {
        return 0;
    }
    if (pages_left > 0) {
        pages_left--;
    }
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

/* The GTT's scratch page. */
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

/* No boot option is given. */
int kcmdline_has(const char *flag)
{
    (void)flag;
    return 0;
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
    pages_left = -1;
    next_phys = 0x50000000ull;
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

/** The directory entry the model reads for the PPGTT's entry `pde`. */
static uint32_t pde_at(struct i915_model *m, struct drm_i915_private *i915, uint32_t pde)
{
    return i915_model_peek(m, I915_GSM_OFFSET + i915->gt.ppgtt.pd_ggtt / 4096u * 4u + pde * 4u);
}

/** Whether a directory entry names the table at `phys`. */
static int names(uint32_t pde, uint64_t phys)
{
    return pde == (GEN6_PDE_ADDR_ENCODE(phys) | GEN6_PDE_VALID);
}

/** Check that the ring of `e` was pointed at the PPGTT as Linux's
 * set_pp_dir does, between its start and RING_VALID. */
static void check_ring(struct i915_model *m, struct drm_i915_private *i915, struct i915_engine *e)
{
    uint32_t b = e->mmio_base;
    long start = at(m, RING_START(b), e->ring_ggtt);
    long dclv = at(m, RING_PP_DIR_DCLV(b), PP_DIR_DCLV_2G);
    long dir = at(m, RING_PP_DIR_BASE(b), i915->gt.ppgtt.pp_dir);
    long mode = at(m, RING_MODE_GEN7(b), I915_MASKED_ENABLE(GFX_PPGTT_ENABLE));
    long valid = at(m, RING_CTL(b), 0x3000u | RING_VALID);

    CHECK("init: the ring's PPGTT registers after its start, before RING_VALID",
          start < dclv && dclv < dir && dir < mode && mode < valid && valid < (1L << 30));
    CHECK("init: the whole 2 GiB valid, the directory named, the PPGTT on",
          i915_model_peek(m, RING_PP_DIR_DCLV(b)) == 0xffffffffu &&
              i915_model_peek(m, RING_PP_DIR_BASE(b)) == i915->gt.ppgtt.pp_dir &&
              (i915_model_peek(m, RING_MODE_GEN7(b)) & GFX_PPGTT_ENABLE));
}

/* ---- the PPGTT made and taken down ---- */

/** The PPGTT made with the rings and taken down with them, and a start
 * that finds no page for the scratch table. */
static void row_init(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m);
    struct i915_ppgtt *pp = &i915->gt.ppgtt;
    uint64_t rings_bottom = i915->ggtt.total - 4u * (4096u + I915_RING_SIZE);
    uint32_t i, ok = 1, pd_index;
    long eco, gam, rcs_dclv;

    m.nlog = 0;
    CHECK("init: the rings start", i915_gt_init(i915) == 0);
    CHECK("init: made, logged", pp->ready && logged("ppgtt: aliasing, 2048 MiB shared"));
    CHECK("init: the directory's 2 MiB below the rings, at a 64 KiB boundary",
          (pp->pd_ggtt & 0xffffu) == 0 && pp->pd_ggtt + I915_PPGTT_PD_BYTES <= rings_bottom &&
              rings_bottom - pp->pd_ggtt < I915_PPGTT_PD_BYTES + 0x10000u &&
              pp->pd_ggtt + pp->pd_take == rings_bottom && i915->gt.ggtt_bottom == pp->pd_ggtt);
    CHECK("init: PP_DIR_BASE counts the directory's offset among the entries in 64 bytes",
          pp->pp_dir >> 16 == pp->pd_ggtt / 4096u * 4u / 64u && (pp->pp_dir & 0xffffu) == 0);
    for (i = 0; i < I915_PPGTT_PDES; i++) {
        ok &= names(pde_at(&m, i915, i), pp->scratch_pt_phys);
    }
    CHECK("init: every directory entry names the scratch table", ok && pp->tables == 0);
    for (i = 0, ok = 1; i < I915_PPGTT_PTES; i++) {
        ok &= pp->scratch_pt[i] == i915->ggtt.scratch_pte;
    }
    CHECK("init: each of whose entries maps the scratch page", ok);
    pd_index = pp->pd_ggtt / 4096u;
    CHECK("init: the directory's entries read back, the GTT's cache flushed after",
          i915_model_find_write(&m, I915_GSM_OFFSET + (pd_index + 511u) * 4u,
                                GEN6_PDE_ADDR_ENCODE(pp->scratch_pt_phys) | GEN6_PDE_VALID,
                                0) >= 0 &&
              i915_model_find_write(&m, GFX_FLSH_CNTL_GEN6, GFX_FLSH_CNTL_EN,
                                    (uint32_t)i915_model_find_write(
                                        &m, I915_GSM_OFFSET + (pd_index + 511u) * 4u,
                                        GEN6_PDE_ADDR_ENCODE(pp->scratch_pt_phys) | GEN6_PDE_VALID,
                                        0)) >= 0);
    check_ring(&m, i915, &i915->gt.engine[I915_RCS0]);
    check_ring(&m, i915, &i915->gt.engine[I915_BCS0]);
    check_ring(&m, i915, &i915->gt.engine[I915_VCS0]);
    check_ring(&m, i915, &i915->gt.engine[I915_VECS0]);
    eco = last_write(&m, GAC_ECO_BITS);
    gam = last_write(&m, GAM_ECOCHK);
    rcs_dclv = at(&m, RING_PP_DIR_DCLV(RENDER_RING_BASE), PP_DIR_DCLV_2G);
    CHECK("init: the PPGTT's caching set before the render ring's directory",
          eco >= 0 && gam >= 0 && eco < rcs_dclv && gam < rcs_dclv &&
              (i915_model_peek(&m, GAC_ECO_BITS) & ECOBITS_PPGTT_CACHE64B) ==
                  ECOBITS_PPGTT_CACHE64B &&
              (i915_model_peek(&m, GAM_ECOCHK) & ECOCHK_PPGTT_WB_HSW) == ECOCHK_PPGTT_WB_HSW);
    CHECK("init: the GT's pages and the scratch table", pages_live == TEST_GT_PAGES);

    i915_gt_fini(i915);
    CHECK("fini: the directory's entries on the scratch page again",
          i915_ggtt_pte(i915, (uint64_t)pd_index * 4096u) == i915->ggtt.scratch_pte &&
              i915_ggtt_pte(i915, (uint64_t)(pd_index + 511u) * 4096u) == i915->ggtt.scratch_pte);
    CHECK("fini: every page given back, the GT's space too, the PPGTT gone",
          pages_live == 0 && i915->gt.ggtt_bottom == i915->gt.ggtt_top && !pp->ready &&
              !pp->scratch_pt);
    done(&m);

    /* Room for the rings' eight allocations, none for the scratch table. */
    i915 = fresh(&m);
    pages_left = 8;
    CHECK("init: no page for the scratch table fails the start",
          i915_gt_init(i915) == -ENOMEM && logged("ppgtt: no page for the scratch table"));
    CHECK("init: nothing left", pages_live == 0 && !pp->ready &&
                                    i915->gt.ggtt_bottom == i915->gt.ggtt_top &&
                                    !i915->gt.engine[0].running);
    done(&m);
}

/* ---- bindings mirrored ---- */

/** A buffer of `n` pages from the heap, their addresses in `phys`. */
static uint32_t *buffer(uint32_t n, uint64_t *phys)
{
    uint8_t *v = (uint8_t *)kalloc_pages(n, 0);
    uint32_t i;

    for (i = 0; v && i < n; i++) {
        phys[i] = paging_virt_to_phys((uintptr_t)(v + i * 4096u));
        k_memset(v + i * 4096u, 0, 4096u);
    }
    return (uint32_t *)(void *)v;
}

/** The refusals of a PPGTT that holds two tables: an offset not on a
 * page, a range past 2 GiB, and a table with no page for it. */
static void mirror_refusals(struct i915_model *m, struct drm_i915_private *i915,
                            const uint64_t *phys)
{
    struct i915_ppgtt *pp = &i915->gt.ppgtt;

    CHECK("mirror: an offset not on a page refused",
          i915_ppgtt_insert(i915, 0x800010u, phys, 1, I915_CACHE_LLC) == -EINVAL);
    CHECK("mirror: past 2 GiB refused",
          i915_ppgtt_insert(i915, I915_PPGTT_SIZE - 4096u, phys, 2, I915_CACHE_LLC) == -EINVAL &&
              pp->tables == 2);
    pages_refuse = 1;
    CHECK("mirror: no page for a table: ENOMEM, nothing written",
          i915_ppgtt_insert(i915, 0x2000000u, phys, 1, I915_CACHE_LLC) == -ENOMEM &&
              pp->tables == 2 && !pp->pt[8] && names(pde_at(m, i915, 8), pp->scratch_pt_phys) &&
              logged("ppgtt: no page for the table of 0x2000000"));
    pages_refuse = 0;
}

/** A binding mirrored into the PPGTT across a 4 MiB boundary, cleared and
 * mirrored again, and the refusals. */
static void row_mirror(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m);
    struct i915_ppgtt *pp = &i915->gt.ppgtt;
    uint64_t phys[3], at4 = 0xc00000u - 4096u; /* past the firmware's framebuffer */
    uint32_t *buf = buffer(3, phys), i, ok = 1;

    CHECK("mirror: before the PPGTT is made, nothing to do",
          i915_ppgtt_insert(i915, at4, phys, 3, I915_CACHE_LLC) == 0 && pp->tables == 0);
    CHECK("mirror: the rings start", i915_gt_init(i915) == 0);
    CHECK("mirror: a binding across 4 MiB in the global GTT",
          i915_ggtt_insert(i915, at4, phys, 3, I915_CACHE_LLC) == 0);
    CHECK("mirror: and in the PPGTT at the same offset",
          i915_ppgtt_insert(i915, at4, phys, 3, I915_CACHE_LLC) == 0);
    CHECK("mirror: the two tables it reaches made, named in the directory",
          pp->tables == 2 && pp->pt[2] && pp->pt[3] && !pp->pt[1] && !pp->pt[4] &&
              names(pde_at(&m, i915, 2), pp->pt_phys[2]) &&
              names(pde_at(&m, i915, 3), pp->pt_phys[3]) &&
              names(pde_at(&m, i915, 4), pp->scratch_pt_phys));
    CHECK("mirror: each entry the global GTT's",
          pp->pt[2][1023] == i915_ggtt_pte(i915, at4) &&
              pp->pt[3][0] == i915_ggtt_pte(i915, at4 + 4096u) &&
              pp->pt[3][1] == i915_ggtt_pte(i915, at4 + 8192u) &&
              pp->pt[3][1] == i915_ggtt_pte_encode(phys[2], I915_CACHE_LLC));
    for (i = 0; i < 1023u; i++) {
        ok &= pp->pt[2][i] == i915->ggtt.scratch_pte;
    }
    for (i = 2; i < 1024u; i++) {
        ok &= pp->pt[3][i] == i915->ggtt.scratch_pte;
    }
    CHECK("mirror: the tables' other entries on the scratch page", ok);

    i915_ppgtt_clear_range(i915, at4, 3u * 4096u);
    CHECK("mirror: cleared to the scratch page, the tables kept",
          pp->pt[2][1023] == i915->ggtt.scratch_pte && pp->pt[3][0] == i915->ggtt.scratch_pte &&
              pp->pt[3][1] == i915->ggtt.scratch_pte && pp->tables == 2 &&
              names(pde_at(&m, i915, 3), pp->pt_phys[3]));
    CHECK("mirror: again into the kept tables, none made",
          i915_ppgtt_insert(i915, at4, phys, 3, I915_CACHE_NONE) == 0 && pp->tables == 2 &&
              pp->pt[3][0] == i915_ggtt_pte_encode(phys[1], I915_CACHE_NONE));

    mirror_refusals(&m, i915, phys);
    i915_gt_fini(i915);
    kfree_pages(buf, 3);
    CHECK("mirror: the tables freed with the rings", pages_live == 0 && pp->tables == 0);
    done(&m);
}

/* ---- batches through the PPGTT ---- */

/* The buffers' offsets in the GTT, past the firmware's framebuffer: a
 * batch and its target, 4 MiB apart. */
#define BATCH_AT  0x1000000u
#define TARGET_AT 0x1400000u

/**
 * Run on `e`'s ring a batch at BATCH_AT, started with `start`'s flags,
 * that stores `value` at TARGET_AT + 8 and `value + 1` at `other`, then
 * wait for the ring's breadcrumb: 0, or -1 when it is not written.
 */
static int run_batch(struct drm_i915_private *i915, struct i915_engine *e, uint32_t *batch,
                     uint32_t start, uint32_t value, uint32_t other)
{
    uint32_t *cs, seqno;

    batch[0] = MI_STORE_DWORD_IMM_GEN4;
    batch[1] = 0;
    batch[2] = TARGET_AT + 8u;
    batch[3] = value;
    batch[4] = MI_STORE_DWORD_IMM_GEN4;
    batch[5] = 0;
    batch[6] = other;
    batch[7] = value + 1u;
    batch[8] = MI_NOOP;
    batch[9] = MI_BATCH_BUFFER_END;
    cs = i915_ring_begin(e, i915, 2);
    if (!cs) {
        return -1;
    }
    cs[0] = start;
    cs[1] = BATCH_AT;
    i915_ring_advance(e, i915);
    seqno = i915_engine_emit_breadcrumb(i915, e);
    return seqno && i915_engine_wait(i915, e, seqno, 100000u) == 0 ? 0 : -1;
}

/** Bind the batch and the target in the global GTT and the PPGTT, as a
 * buffer's binding is: 0, or -1. */
static int bind_both(struct drm_i915_private *i915, const uint64_t *phys)
{
    return i915_ggtt_insert(i915, BATCH_AT, &phys[0], 1, I915_CACHE_LLC) == 0 &&
                   i915_ppgtt_insert(i915, BATCH_AT, &phys[0], 1, I915_CACHE_LLC) == 0 &&
                   i915_ggtt_insert(i915, TARGET_AT, &phys[1], 1, I915_CACHE_LLC) == 0 &&
                   i915_ppgtt_insert(i915, TARGET_AT, &phys[1], 1, I915_CACHE_LLC) == 0
               ? 0
               : -1;
}

/* The flags each ring's batches start with, as i915_gem_execbuffer.c
 * gives an unprivileged batch: bit 8 is the PPGTT on both. */
#define RCS_START (MI_BATCH_BUFFER_START | MI_BATCH_PPGTT_HSW | MI_BATCH_NON_SECURE_HSW)
#define BCS_START (MI_BATCH_BUFFER_START | MI_BATCH_NON_SECURE_I965)

/** Batches through the PPGTT on both rings: their stores land on the
 * pages the global GTT maps, and the GT's own objects and a range cleared
 * from the PPGTT alone are out of their reach. */
static void row_batch(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m);
    struct i915_engine *rcs, *bcs;
    struct i915_model_ring *rr = i915_model_ring_state(&m, 0);
    struct i915_model_ring *br = i915_model_ring_state(&m, 1);
    uint64_t phys[2];
    uint32_t *pages2 = buffer(2, phys), *batch = pages2, *target = pages2 + 1024;
    uint32_t hws_before;

    CHECK("batch: the rings start", i915_gt_init(i915) == 0);
    rcs = &i915->gt.engine[I915_RCS0];
    bcs = &i915->gt.engine[I915_BCS0];
    CHECK("batch: bound in both", bind_both(i915, phys) == 0);

    hws_before = rcs->hws[200];
    scratch_page[200] = 0;
    CHECK("batch: the render ring runs it through the PPGTT",
          run_batch(i915, rcs, batch, RCS_START, 0x1915aa00u, rcs->hws_ggtt + 800u) == 0 &&
              rr->batches == 1 && !rr->faulted);
    CHECK("batch: its store reached the target's page, as the global GTT maps it",
          target[2] == 0x1915aa00u &&
              i915_ggtt_pte(i915, TARGET_AT) == i915_ggtt_pte_encode(phys[1], I915_CACHE_LLC));
    CHECK("batch: the status page, the GT's own, is out of its reach: the scratch page took it",
          rcs->hws[200] == hws_before && scratch_page[200] == 0x1915aa01u);

    target[2] = 0;
    CHECK("batch: the blitter runs it through the PPGTT",
          run_batch(i915, bcs, batch, BCS_START, 0x1915bb00u, bcs->ring_ggtt) == 0 &&
              br->batches == 1 && !br->faulted && target[2] == 0x1915bb00u &&
              bcs->ring[0] != 0x1915bb01u);

    /* Taken out of the PPGTT alone, the target is gone from the batch's
     * view while the global GTT still maps it. */
    i915_ppgtt_clear_range(i915, TARGET_AT, 4096u);
    target[2] = 0;
    CHECK("batch: a range cleared from the PPGTT is out of reach",
          run_batch(i915, rcs, batch, RCS_START, 0x1915cc00u, TARGET_AT + 16u) == 0 &&
              target[2] == 0 && scratch_page[2] == 0x1915cc00u &&
              i915_ggtt_pte(i915, TARGET_AT) == i915_ggtt_pte_encode(phys[1], I915_CACHE_LLC));

    i915_gt_fini(i915);
    kfree_pages(pages2, 2);
    done(&m);
}

/* ---- after a reset ---- */

/** Reset the model's block named `name` to its power-on state, as a
 * reset of the chip's part does. */
static void reset_block(struct i915_model *m, const char *name)
{
    int i;

    for (i = 0; i < i915_model_nblocks(); i++) {
        const struct i915_model_block *b = i915_model_block_at(i);

        if (strcmp(b->name, name) == 0 && b->reset) {
            b->reset(m, m->priv[i]);
        }
    }
}

/** Both rings reset to power-on and started again, as a reset of the
 * whole GPU does: the PPGTT on again, and a batch through it. */
static void row_reset(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m);
    struct i915_engine *rcs, *bcs;
    uint64_t phys[2];
    uint32_t *pages2 = buffer(2, phys), *batch = pages2, *target = pages2 + 1024;
    int32_t ra, rb;

    CHECK("reset: the rings start", i915_gt_init(i915) == 0);
    rcs = &i915->gt.engine[I915_RCS0];
    bcs = &i915->gt.engine[I915_BCS0];
    CHECK("reset: bound in both", bind_both(i915, phys) == 0);

    /* The whole GPU reset: both rings at power-on, the PPGTT off, the
     * caching bits gone. */
    reset_block(&m, "render ring");
    reset_block(&m, "blitter ring");
    i915_model_poke(&m, GAC_ECO_BITS, 0);
    i915_model_poke(&m, GAM_ECOCHK, 0);
    CHECK("reset: the rings' PPGTT registers cleared",
          i915_model_peek(&m, RING_PP_DIR_BASE(RENDER_RING_BASE)) == 0 &&
              !(i915_model_peek(&m, RING_MODE_GEN7(BLT_RING_BASE)) & GFX_PPGTT_ENABLE));

    m.nlog = 0;
    CHECK("reset: forcewake", i915_forcewake_get(i915) == 0);
    ra = i915_ring_start(i915, rcs);
    rb = i915_ring_start(i915, bcs);
    i915_forcewake_put(i915);
    CHECK("reset: both rings start again", ra == 0 && rb == 0);
    check_ring(&m, i915, rcs);
    check_ring(&m, i915, bcs);
    CHECK("reset: the PPGTT's caching set again",
          (i915_model_peek(&m, GAC_ECO_BITS) & ECOBITS_PPGTT_CACHE64B) == ECOBITS_PPGTT_CACHE64B &&
              (i915_model_peek(&m, GAM_ECOCHK) & ECOCHK_PPGTT_WB_HSW) == ECOCHK_PPGTT_WB_HSW);
    CHECK("reset: the render ring runs a batch through the PPGTT",
          run_batch(i915, rcs, batch, RCS_START, 0x1915dd00u, TARGET_AT + 16u) == 0 &&
              target[2] == 0x1915dd00u && target[4] == 0x1915dd01u &&
              !i915_model_ring_state(&m, 0)->faulted);
    CHECK("reset: so does the blitter",
          run_batch(i915, bcs, batch, BCS_START, 0x1915ee00u, TARGET_AT + 16u) == 0 &&
              target[2] == 0x1915ee00u && !i915_model_ring_state(&m, 1)->faulted);

    i915_gt_fini(i915);
    kfree_pages(pages2, 2);
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
    row_init();
    row_mirror();
    row_batch();
    row_reset();
    if (fails) {
        printf("i915-ppgtt-test: %d failures\n", fails);
        return 1;
    }
    printf("i915-ppgtt-test: ok\n");
    return 0;
}
