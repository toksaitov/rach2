/**
 * i915_base_hosttest.c: the Intel graphics driver's base, forcewake and the
 * GT FIFO (i915_uncore.c), the display power well (i915_power.c), stolen
 * memory (i915_stolen.c) and the global GTT (i915_gtt.c), against the
 * host model of the Lenovo's HD 4400.
 *
 * host-deps: rach2/sys/drm/i915/i915_uncore.c rach2/sys/drm/i915/i915_power.c
 * host-deps: rach2/sys/drm/i915/i915_stolen.c rach2/sys/drm/i915/i915_gtt.c
 * host-deps: rach2/sys/drm/i915/i915_hw.c tests/host/i915_model.c
 * host-deps: rach2/sys/mm/frame.c rach2/sys/lib/kstring.c
 * host-deps: rach2/sys/lib/kctypes.c tests/host/compat/spinlock_host.c
 *
 * A row makes a fresh model, whose blocks give forcewake's acknowledgement,
 * the GT FIFO's free entries and the power well's state behaviour a test
 * can slow or stop, and a device record over it as the bind would. The
 * kernel around the driver is a set of records: the log keeps its last
 * line, the frame allocator hands out one frame and says which frames it
 * owns, and the IOMMU names the Lenovo's reserved region for the GPU.
 *
 * The rows: forcewake's request, acknowledgement and reference count, a
 * slow and a stuck acknowledgement, a stuck release, the GT's threads, and
 * the reads that take it; the GT FIFO's reserve, a slow and a stuck FIFO;
 * the power well taken over from the BIOS, turned off, kept on while the
 * firmware's pipe needs it, held on by another request, slow and stuck;
 * stolen memory reserved from the boot's map, found, checked against the
 * allocator, the RMRR and the firmware's reserved part; and the global
 * GTT: its size, the aperture's, the scratch page, the firmware's entries
 * kept, the entries' format, insert and clear and their refusals, and the
 * MMIO backend's write-combining map of the entries.
 *
 * Returns 0 when every row passes, 1 otherwise.
 */

#include <stdarg.h>

#include "drm/i915/i915.h"
#include "drm/i915/i915_reg.h"
#include "iommu.h"
#include "lib/kerrno.h"
#include "lib/kirq.h"
#include "mm/frame.h"
#include "mm/paging.h"
#include "udelay.h"
#include "i915_model.h"
#include "i915_lenovo.h"
#include "i915_model_base.h"

extern int printf(const char *format, ...);
extern int vsnprintf(char *s, unsigned long n, const char *format, va_list ap);
extern void *calloc(unsigned long count, unsigned long size);
extern void free(void *p);
extern int strcmp(const char *a, const char *b);
extern char *strstr(const char *s, const char *t);

static int fails;

#define CHECK(what, cond)                               \
    do {                                                \
        if (!(cond)) {                                  \
            printf("i915-base-test: fail: %s\n", what); \
            fails++;                                    \
        }                                               \
    } while (0)

#define MIB(x) ((uint64_t)(x) << 20)

/* ---- the kernel around the driver ---- */

static char last_log[256];
static int logs;

/** The boot log: the last line is kept, for the rows that read it. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    va_list ap;

    (void)level;
    (void)subsystem;
    va_start(ap, format);
    vsnprintf(last_log, sizeof(last_log), format, ap);
    va_end(ap);
    logs++;
}

/** Whether the last line of the log holds `text`. */
static int logged(const char *text)
{
    return strstr(last_log, text) != 0;
}

static uint32_t irq_flags;

uint32_t kirq_save(void)
{
    return irq_flags++;
}

void kirq_restore(uint32_t flags)
{
    (void)flags;
}

void k_udelay(uint32_t usec)
{
    (void)usec;
}

/* The frame allocator: one frame for the scratch page, which a row may
 * refuse, and the frames it owns, [owned_lo, owned_hi). */
static uintptr_t frame_next = 0x12345000u;
static int frames_out, frames_freed, frame_refuse, zero_refuse;
static uintptr_t freed_frame;
static uint64_t owned_lo, owned_hi;

uintptr_t alloc_frame(void)
{
    if (frame_refuse) {
        return 0;
    }
    frames_out++;
    return frame_next;
}

void free_frame(uintptr_t phys)
{
    frames_freed++;
    freed_frame = phys;
}

int paging_zero_frame(uintptr_t phys)
{
    (void)phys;
    return zero_refuse ? -1 : 0;
}

int paging_frame_is_device(uintptr_t phys)
{
    return !(phys >= owned_lo && phys < owned_hi);
}

/* The IOMMU: up or not, and the regions it names for the GPU. */
static int vtd_on = 1;
static struct iommu_region rmrr = {0xab800000ull, 0xaf9fffffull, IOMMU_RESV_DIRECT_RELAXABLE};
static int rmrr_named = 1;

int iommu_present(void)
{
    return vtd_on;
}

int iommu_unit_of(uint8_t bus, uint8_t devfn)
{
    return vtd_on && bus == 0 && devfn == 0x10 ? 0 : -1;
}

int iommu_reserved_regions(uint8_t bus, uint8_t devfn, struct iommu_region *out, int max)
{
    int n = 0;

    if (bus == 0 && devfn == 0x10 && rmrr_named && n < max) {
        out[n++] = rmrr;
    }
    if (n < max) {
        out[n].start = 0xfee00000ull;
        out[n].end = 0xfeefffffull;
        out[n].type = IOMMU_RESV_MSI;
        n++;
    }
    return n;
}

/* The MMIO backend's map: a buffer of the test's. */
static int wc = 1, maps;
static pte_t map_cache;
static size_t map_size;
static uint32_t *map_buffers[2];

int pat_write_combining(void)
{
    return wc;
}

uintptr_t map_mmio_typed(uintptr_t phys_addr, size_t size, pte_t cache)
{
    uint32_t *b = (uint32_t *)calloc(size / 4, sizeof(uint32_t));

    (void)phys_addr;
    if (maps < 2) {
        map_buffers[maps] = b;
    }
    maps++;
    map_cache = cache;
    map_size = size;
    return (uintptr_t)b;
}

/* Configuration space: the model's chip at 00:02.0, or no device. */
static struct i915_model *bus;

uint32_t pci_read_config32(uint8_t b, uint8_t d, uint8_t f, uint8_t offset)
{
    return bus && b == 0 && d == 2 && f == 0 ? i915_model_config(bus, offset, 4) : 0xffffffffu;
}

uint16_t pci_read_config16(uint8_t b, uint8_t d, uint8_t f, uint8_t offset)
{
    return bus && b == 0 && d == 2 && f == 0 ? (uint16_t)i915_model_config(bus, offset, 2)
                                             : 0xffffu;
}

/* The id table: the Lenovo's chip alone, which is all i915_stolen.c asks. */
static const struct i915_platform hsw = {"HASWELL", 7, 50, 7, 2, 0xf, 1, 1, 1, 0, 1, 31, 40};
static const struct i915_id lenovo_id = {0x0A16, 1, 0, &hsw};

const struct i915_id *i915_id_lookup(uint16_t device_id)
{
    return device_id == 0x0A16 ? &lenovo_id : 0;
}

/* ---- the device ---- */

static struct drm_i915_private dev;

/** A fresh model on the bus and a device record over it, as the bind
 * makes one; the kernel's records cleared. */
static struct drm_i915_private *fresh(struct i915_model *m)
{
    CHECK("the model is made", i915_model_init(m) == 0);
    bus = m;
    k_memset(&dev, 0, sizeof(dev));
    dev.pci = m->pci;
    dev.hw = m->hw;
    dev.mmio_phys = LENOVO_GPU_BAR0;
    dev.gmadr_phys = LENOVO_GPU_BAR2;
    dev.gmch_ctrl = (uint16_t)i915_model_config(m, SNB_GMCH_CTRL, 2);
    dev.bsm = i915_model_config(m, INTEL_BSM, 4);
    dev.msi_vector = -1;
    last_log[0] = 0;
    logs = 0;
    frames_out = frames_freed = frame_refuse = zero_refuse = 0;
    freed_frame = 0;
    owned_lo = owned_hi = 0;
    vtd_on = 1;
    rmrr_named = 1;
    rmrr.start = 0xab800000ull;
    rmrr.end = 0xaf9fffffull;
    return &dev;
}

/** Take the model off the bus. */
static void done(struct i915_model *m)
{
    i915_model_free(m);
    bus = 0;
}

/* ---- forcewake ---- */

/** The load: the firmware's errors cleared, every request released. */
static void row_uncore_init(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m);

    i915_model_poke(&m, FPGA_DBG, FPGA_DBG_RM_NOCLAIM);
    i915_model_poke(&m, GTFIFODBG, GT_FIFO_OVFERR);
    i915_uncore_init(i915);
    CHECK("init: the unclaimed access cleared",
          i915_model_find_write(&m, FPGA_DBG, FPGA_DBG_RM_NOCLAIM, 0) >= 0);
    CHECK("init: the FIFO's errors cleared",
          i915_model_find_write(&m, GTFIFODBG, GT_FIFO_OVFERR, 0) >= 0);
    CHECK("init: every request released, the firmware's too",
          i915_model_find_write(&m, FORCEWAKE_MT, 0xffff0000u, 0) >= 0);
    CHECK("init: no reference held", i915->uncore.wake_count == 0);
    CHECK("init: the FIFO's free entries read", i915->uncore.fifo_count == 64);
    done(&m);

    i915 = fresh(&m);
    i915_model_poke(&m, FPGA_DBG, 0);
    i915_uncore_init(i915);
    CHECK("init: a clean chip has nothing to clear",
          i915_model_count(&m, I915_MODEL_WRITE, FPGA_DBG) == 0 &&
              i915_model_count(&m, I915_MODEL_WRITE, GTFIFODBG) == 0);
    done(&m);
}

/** Get and put: the handshake, the reference count, the release. */
static void row_forcewake(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m);
    long clear, req;

    i915_uncore_init(i915);
    CHECK("get: woken", i915_forcewake_get(i915) == 0);
    req = i915_model_find_write(&m, FORCEWAKE_MT, 0x00010001u, 0);
    CHECK("get: the kernel's request set", req >= 0 && i915_model_forcewake(&m)->req == 1u);
    clear = -1;
    {
        uint32_t i;
        for (i = 0; i < m.nlog; i++) {
            if (m.log[i].op == I915_MODEL_READ && m.log[i].reg == FORCEWAKE_ACK_HSW) {
                clear = (long)i;
                break;
            }
        }
    }
    CHECK("get: the old acknowledgement read clear before the request", clear >= 0 && clear < req);
    CHECK("get: the GT's threads read",
          i915_model_count(&m, I915_MODEL_READ, GEN6_GT_THREAD_STATUS_REG) >= 1);
    CHECK("get: one reference", i915->uncore.wake_count == 1);
    CHECK("get: a second takes a reference and writes nothing",
          i915_forcewake_get(i915) == 0 && i915->uncore.wake_count == 2 &&
              i915_model_count(&m, I915_MODEL_WRITE, FORCEWAKE_MT) == 2);
    i915_forcewake_put(i915);
    CHECK("put: one reference left, the request kept",
          i915->uncore.wake_count == 1 && i915_model_forcewake(&m)->req == 1u);
    i915_forcewake_put(i915);
    CHECK("put: the last releases the request",
          i915->uncore.wake_count == 0 && i915_model_forcewake(&m)->req == 0 &&
              i915_model_find_write(&m, FORCEWAKE_MT, 0x00010000u, 0) >= 0);
    i915_forcewake_put(i915);
    CHECK("put: one too many is logged and changes nothing",
          i915->uncore.wake_count == 0 && logged("no reference") &&
              i915_model_count(&m, I915_MODEL_WRITE, FORCEWAKE_MT) == 3);
    CHECK("forcewake: no wait reached its bound", i915->hw.timeouts == 0);
    done(&m);
}

/** A slow acknowledgement is waited for; a stuck one fails by name. */
static void row_forcewake_slow_stuck(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m);
    struct i915_model_fw *fw;

    i915_uncore_init(i915);
    i915_model_forcewake(&m)->ack_delay_us = 3000;
    m.now_us = 0;
    CHECK("slow ack: woken", i915_forcewake_get(i915) == 0 && i915->uncore.wake_count == 1);
    CHECK("slow ack: after 3 ms, within a step of it", m.now_us >= 3000 && m.now_us < 4100);
    CHECK("slow ack: no timeout", i915->hw.timeouts == 0);
    m.now_us = 0;
    i915_forcewake_put(i915);
    CHECK("slow release: the request withdrawn at once, no wait", m.now_us == 0);
    m.now_us = 0;
    CHECK("slow release: the next get waits for the ack to clear first",
          i915_forcewake_get(i915) == 0 && m.now_us >= 3000);
    done(&m);

    i915 = fresh(&m);
    fw = i915_model_forcewake(&m);
    i915_uncore_init(i915);
    fw->ack_stuck = 1;
    m.now_us = 0;
    CHECK("stuck ack: fails", i915_forcewake_get(i915) == -ETIMEDOUT);
    CHECK("stuck ack: by name", i915->hw.last_timeout &&
                                    strcmp(i915->hw.last_timeout, "forcewake ack") == 0 &&
                                    logged("forcewake ack: timeout after 50000 us"));
    CHECK("stuck ack: at its bound of 50 ms", m.now_us == 50000);
    CHECK("stuck ack: no reference held, the request withdrawn",
          i915->uncore.wake_count == 0 && fw->req == 0);
    fw->ack_stuck = 0;
    CHECK("stuck ack: a later get once it answers",
          i915_forcewake_get(i915) == 0 && i915->uncore.wake_count == 1);
    done(&m);

    i915 = fresh(&m);
    i915_uncore_init(i915);
    i915_model_poke(&m, FORCEWAKE_ACK_HSW, FORCEWAKE_KERNEL);
    i915_model_forcewake(&m)->ack_stuck = 1;
    CHECK("stuck release: fails", i915_forcewake_get(i915) == -ETIMEDOUT);
    CHECK("stuck release: by name",
          i915->hw.last_timeout && strcmp(i915->hw.last_timeout, "forcewake ack clear") == 0);
    CHECK("stuck release: no request made",
          i915_model_find_write(&m, FORCEWAKE_MT, 0x00010001u, 0) < 0 &&
              i915->uncore.wake_count == 0);
    done(&m);

    i915 = fresh(&m);
    i915_uncore_init(i915);
    i915_model_poke(&m, GEN6_GT_THREAD_STATUS_REG, 1);
    CHECK("threads asleep: woken all the same, as Linux",
          i915_forcewake_get(i915) == 0 && i915->uncore.wake_count == 1);
    CHECK("threads asleep: counted and named",
          i915->uncore.thread_timeouts == 1 && strcmp(i915->hw.last_timeout, "gt thread c0") == 0);
    done(&m);
}

/** The reads: a GT register under forcewake, any other directly. */
static void row_uncore_read(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m);

    i915_uncore_init(i915);
    i915_model_poke(&m, RING_TIMESTAMP(RENDER_RING_BASE), 0x1234u);
    CHECK("read: a GT register's value",
          i915_uncore_read(i915, RING_TIMESTAMP(RENDER_RING_BASE)) == 0x1234u);
    CHECK("read: taken and released around it",
          i915_model_find_write(&m, FORCEWAKE_MT, 0x00010001u, 0) >= 0 &&
              i915_model_find_write(&m, FORCEWAKE_MT, 0x00010000u, 0) >= 0 &&
              i915->uncore.wake_count == 0);
    m.nlog = 0;
    i915_model_poke(&m, DEIMR, 0x55u);
    CHECK("read: a display register directly",
          i915_uncore_read(i915, DEIMR) == 0x55u &&
              i915_model_count(&m, I915_MODEL_WRITE, FORCEWAKE_MT) == 0);
    CHECK("read: a reference taken", i915_forcewake_get(i915) == 0);
    m.nlog = 0;
    CHECK("read: under a reference held, no handshake",
          i915_uncore_read(i915, RING_TIMESTAMP(RENDER_RING_BASE)) == 0x1234u &&
              i915_model_count(&m, I915_MODEL_WRITE, FORCEWAKE_MT) == 0);
    i915_forcewake_put(i915);
    i915_model_forcewake(&m)->ack_stuck = 1;
    CHECK("read: a GT register without forcewake reads all ones",
          i915_uncore_read(i915, RING_TIMESTAMP(RENDER_RING_BASE)) == 0xffffffffu);
    done(&m);
}

/** The GT FIFO: the reserve, a slow FIFO and a stuck one. */
static void row_fifo(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m);
    struct i915_model_fifo *f;
    uint32_t reads;

    i915_uncore_init(i915);
    f = i915_model_gt_fifo(&m);
    reads = i915_model_count(&m, I915_MODEL_READ, GTFIFOCTL);
    i915_uncore_write(i915, RING_TAIL(RENDER_RING_BASE), 0x40u);
    CHECK("fifo: a write counted against the entries",
          i915->uncore.fifo_count == 63 &&
              i915_model_peek(&m, RING_TAIL(RENDER_RING_BASE)) == 0x40u);
    CHECK("fifo: above the reserve, no read",
          i915_model_count(&m, I915_MODEL_READ, GTFIFOCTL) == reads);
    i915_uncore_write(i915, DEIMR, 0xffu);
    CHECK("fifo: a display register's write passes the FIFO by",
          i915->uncore.fifo_count == 63 && i915_model_peek(&m, DEIMR) == 0xffu);

    i915->uncore.fifo_count = 20;
    f->free = 5;
    f->refill_per_us = 1;
    m.now_us = 0;
    i915_uncore_write(i915, RING_TAIL(RENDER_RING_BASE), 0x80u);
    CHECK("slow fifo: waited until past the reserve",
          m.now_us >= 16 && i915->uncore.fifo_count >= 20 && i915->uncore.fifo_timeouts == 0);
    CHECK("slow fifo: then written", i915_model_peek(&m, RING_TAIL(RENDER_RING_BASE)) == 0x80u);

    i915->uncore.fifo_count = 3;
    f->free = 3;
    f->refill_per_us = 0;
    m.now_us = 0;
    i915_uncore_write(i915, RING_TAIL(RENDER_RING_BASE), 0xc0u);
    CHECK("stuck fifo: the wait ends at 10 ms", m.now_us == 10000 &&
                                                    i915->uncore.fifo_timeouts == 1 &&
                                                    strcmp(i915->hw.last_timeout, "gt fifo") == 0);
    CHECK("stuck fifo: the write goes ahead, as on Linux",
          i915_model_peek(&m, RING_TAIL(RENDER_RING_BASE)) == 0xc0u);
    done(&m);
}

/* ---- the power well ---- */

/** The well taken over from the BIOS, and turned off with nothing in it. */
static void row_power(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m);
    struct i915_model_pw *pw = i915_model_power(&m);

    CHECK("power: the firmware's request at power-on", pw->req[0] && pw->on);
    CHECK("power: taken over", i915_power_init(i915) == 0);
    CHECK("power: the BIOS's request is now the driver's",
          pw->req[0] == 0 && pw->req[1] != 0 && pw->on);
    CHECK("power: the init reference held", i915->power.count == 1 && i915->power.on);
    CHECK("power: the well reports on", i915_power_well_on(i915));
    i915_model_poke(&m, TRANS_DDI_FUNC_CTL(I915_TRANS_EDP),
                    TRANS_DDI_FUNC_ENABLE | TRANS_DDI_EDP_INPUT_A_ON);
    CHECK("power: the panel on pipe A always on needs no well", !i915_power_well_in_use(i915));
    i915_power_put(i915);
    CHECK("power: the last put turns it off",
          pw->req[1] == 0 && !pw->on && !i915->power.on && !i915->power.kept);
    CHECK("power: off reads off", !i915_power_well_on(i915) && !i915_power_well_in_use(i915));
    CHECK("power: on again", i915_power_get(i915) == 0 && pw->on && i915->power.count == 1);
    i915_power_put(i915);
    i915_power_put(i915);
    CHECK("power: a put too many is logged", logged("no reference") && i915->power.count == 0);
    done(&m);
}

/** Each way the firmware's pipe may need the well keeps it on. */
static void row_power_kept(void)
{
    static const struct {
        const char *what;
        uint32_t reg, value;
    } runs[] = {
        {"kept: the eDP transcoder through pipe A's panel fitter", 0,
         TRANS_DDI_FUNC_ENABLE | TRANS_DDI_EDP_INPUT_A_ONOFF},
        {"kept: the eDP transcoder from pipe B", 0,
         TRANS_DDI_FUNC_ENABLE | TRANS_DDI_EDP_INPUT_B_ONOFF},
        {"kept: transcoder A", 1, 0},
        {"kept: transcoder C", 2, 0},
        {"kept: pipe A's panel fitter", 3, 0},
        {"kept: pipe B's plane", 4, 0},
    };
    unsigned i;

    for (i = 0; i < sizeof(runs) / sizeof(runs[0]); i++) {
        struct i915_model m;
        struct drm_i915_private *i915 = fresh(&m);
        struct i915_model_pw *pw = i915_model_power(&m);

        switch (runs[i].reg) {
        case 0:
            i915_model_poke(&m, TRANS_DDI_FUNC_CTL(I915_TRANS_EDP), runs[i].value);
            break;
        case 1:
            i915_model_poke(&m, TRANSCONF(I915_PIPE_A), TRANSCONF_ENABLE);
            break;
        case 2:
            i915_model_poke(&m, TRANSCONF(I915_PIPE_C), TRANSCONF_ENABLE);
            break;
        case 3:
            i915_model_poke(&m, PF_CTL(0), PF_ENABLE);
            break;
        default:
            i915_model_poke(&m, DSPCNTR(I915_PIPE_B), DISP_ENABLE);
            break;
        }
        CHECK("kept: taken over", i915_power_init(i915) == 0);
        CHECK("kept: in use", i915_power_well_in_use(i915));
        i915_power_put(i915);
        CHECK(runs[i].what, pw->on && pw->req[1] != 0 && i915->power.kept && i915->power.on &&
                                i915->power.count == 0);
        CHECK("kept: logged", logged("kept on"));
        CHECK("kept: a get takes it back",
              i915_power_get(i915) == 0 && !i915->power.kept && i915->power.count == 1);
        done(&m);
    }
}

/** A well that is slow, stuck, or held on by another request. */
static void row_power_slow_stuck(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m);
    struct i915_model_pw *pw = i915_model_power(&m);

    pw->req[0] = 0;
    pw->on = 0;
    pw->delay_us = 300;
    m.now_us = 0;
    CHECK("slow well: on after its delay",
          i915_power_init(i915) == 0 && pw->on && m.now_us >= 300 && i915->hw.timeouts == 0);
    done(&m);

    i915 = fresh(&m);
    pw = i915_model_power(&m);
    pw->req[0] = 0;
    pw->on = 0;
    pw->stuck = 1;
    m.now_us = 0;
    CHECK("stuck well: fails", i915_power_init(i915) == -ETIMEDOUT);
    CHECK("stuck well: by name, at 1 ms",
          strcmp(i915->hw.last_timeout, "power well on") == 0 && m.now_us == 1000);
    CHECK("stuck well: no reference held", i915->power.count == 0);
    done(&m);

    i915 = fresh(&m);
    pw = i915_model_power(&m);
    CHECK("held: taken over", i915_power_init(i915) == 0);
    pw->req[2] = HSW_PWR_WELL_CTL_REQ(HSW_PW_CTL_IDX_GLOBAL);
    m.now_us = 0;
    i915_power_put(i915);
    CHECK("held: the driver's request withdrawn", pw->req[1] == 0);
    CHECK("held: no wait for a well the KVMR holds on", m.now_us == 0 && pw->on);
    CHECK("held: logged with its requester", logged("forced on: bios 0, kvmr 1, debug 0"));
    done(&m);

    i915 = fresh(&m);
    pw = i915_model_power(&m);
    CHECK("slow off: taken over", i915_power_init(i915) == 0);
    pw->delay_us = 200;
    m.now_us = 0;
    i915_power_put(i915);
    CHECK("slow off: waited for", !pw->on && m.now_us >= 200 && m.now_us < 1000);
    done(&m);
}

/* ---- stolen memory ---- */

/** Put a usable range of the boot record's map at entry `n`, 24 bytes. */
static uint32_t put_entry(uint8_t *map, uint32_t at, uint64_t base, uint64_t len, uint32_t type)
{
    uint32_t size = 20, i;

    for (i = 0; i < 4; i++) {
        map[at + i] = (uint8_t)(size >> (8 * i));
    }
    for (i = 0; i < 8; i++) {
        map[at + 4 + i] = (uint8_t)(base >> (8 * i));
        map[at + 12 + i] = (uint8_t)(len >> (8 * i));
    }
    for (i = 0; i < 4; i++) {
        map[at + 20 + i] = (uint8_t)(type >> (8 * i));
    }
    return at + BOOT_MMAP_ENTRY;
}

/** The boot's reservation: from a map that lacks it, from the Lenovo's,
 * which has it, and none for another chip. */
static void row_stolen_early(void)
{
    static struct frame_map fm;
    struct i915_model m;
    uint8_t map[8 * BOOT_MMAP_ENTRY];
    uint32_t n;
    uint64_t usable;

    fresh(&m);
    n = put_entry(map, 0, 0, 0x9f000, FRAME_MAP_USABLE);
    n = put_entry(map, n, MIB(1), 0x100000000ull - MIB(1), FRAME_MAP_USABLE);
    CHECK("early: a map that offers the stolen memory",
          frame_map_build(&fm, map, n, 0, FRAME_REACH_32) == 0 &&
              frame_map_usable(&fm, LENOVO_STOLEN_BASE / 4096u));
    usable = frame_map_managed(&fm);
    CHECK("early: 64 MiB taken out", i915_stolen_reserve_early(&fm) == MIB(64));
    CHECK("early: no frame of it usable", !frame_map_usable(&fm, LENOVO_STOLEN_BASE / 4096u) &&
                                              !frame_map_usable(&fm, LENOVO_STOLEN_END / 4096u));
    CHECK("early: the frames around it still usable",
          frame_map_usable(&fm, LENOVO_STOLEN_BASE / 4096u - 1) &&
              frame_map_usable(&fm, LENOVO_STOLEN_END / 4096u + 1));
    CHECK("early: the managed bytes less 64 MiB", frame_map_managed(&fm) == usable - MIB(64));
    CHECK("early: logged as Linux's line", logged("reserving [mem 0xaba00000-0xaf9fffff]"));

    /* The Lenovo's own map (dev-log/70-wifi-chip/linux/dmesg.txt:5-23),
     * whose reserved entry covers the stolen memory already. */
    n = put_entry(map, 0, 0, 0x6f000, FRAME_MAP_USABLE);
    n = put_entry(map, n, 0x70000, 0x18000, FRAME_MAP_USABLE);
    n = put_entry(map, n, MIB(1), 0xa56b0000ull - MIB(1), FRAME_MAP_USABLE);
    n = put_entry(map, n, 0xa5eb0000ull, 0xaa6bf000ull - 0xa5eb0000ull, FRAME_MAP_USABLE);
    n = put_entry(map, n, 0xaafff000ull, 0x1000, FRAME_MAP_USABLE);
    n = put_entry(map, n, 0xab000000ull, 0xafa00000ull - 0xab000000ull, 2);
    n = put_entry(map, n, 0x100000000ull, 0x44f600000ull - 0x100000000ull, FRAME_MAP_USABLE);
    CHECK("early: the Lenovo's map", frame_map_build(&fm, map, n, 0, FRAME_REACH_32) == 0);
    usable = frame_map_managed(&fm);
    CHECK("early: on the Lenovo nothing usable is taken",
          i915_stolen_reserve_early(&fm) == 0 && frame_map_managed(&fm) == usable);

    m.config[PCI_VENDOR_ID] = 0x34;
    CHECK("early: another vendor's GPU is left alone", i915_stolen_reserve_early(&fm) == 0);
    m.config[PCI_VENDOR_ID] = 0x86;
    m.config[PCI_DEVICE_ID] = 0x06;
    CHECK("early: a chip the driver does not bind is left alone",
          i915_stolen_reserve_early(&fm) == 0);
    bus = 0;
    CHECK("early: no GPU, nothing", i915_stolen_reserve_early(&fm) == 0);
    bus = &m;
    done(&m);
}

/** The bind's stolen memory with the Lenovo's values, and its checks. */
static void row_stolen(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m);
    struct i915_stolen *st = &i915->stolen;

    i915_stolen_init(i915);
    CHECK("stolen: Linux's 64 MiB at 0xaba00000",
          st->base == LENOVO_STOLEN_BASE && st->base + st->size - 1 == LENOVO_STOLEN_END);
    CHECK("stolen: inside the GPU's RMRR", st->in_rmrr);
    CHECK("stolen: all usable with no firmware part",
          st->usable_size == MIB(64) && st->reserved_size == 0);
    CHECK("stolen: no buffers in it with VT-d on, as Linux", !st->for_objects);
    CHECK("stolen: logged as Linux does", logged("DMAR active"));
    done(&m);

    i915 = fresh(&m);
    st = &i915->stolen;
    vtd_on = 0;
    rmrr_named = 0;
    i915_stolen_init(i915);
    CHECK("stolen: without VT-d, unchecked and for buffers",
          st->size == MIB(64) && !st->in_rmrr && st->for_objects);
    done(&m);

    i915 = fresh(&m);
    st = &i915->stolen;
    i915_model_poke(&m, GEN6_STOLEN_RESERVED, 0xaf900000u | GEN6_STOLEN_RESERVED_ENABLE);
    i915_stolen_init(i915);
    CHECK("stolen: the firmware's 1 MiB at the top kept out", st->reserved_base == 0xaf900000u &&
                                                                  st->reserved_size == MIB(1) &&
                                                                  st->usable_size == MIB(63));
    done(&m);

    i915 = fresh(&m);
    st = &i915->stolen;
    i915_model_poke(&m, GEN6_STOLEN_RESERVED,
                    0xaf9c0000u | GEN7_STOLEN_RESERVED_256K | GEN6_STOLEN_RESERVED_ENABLE);
    i915_stolen_init(i915);
    CHECK("stolen: the firmware's 256 KiB kept out",
          st->reserved_size == 0x40000u && st->usable_size == 0xaf9c0000u - LENOVO_STOLEN_BASE);
    done(&m);

    i915 = fresh(&m);
    st = &i915->stolen;
    i915_model_poke(&m, GEN6_STOLEN_RESERVED, 0xb0000000u | GEN6_STOLEN_RESERVED_ENABLE);
    i915_stolen_init(i915);
    CHECK("stolen: a firmware part outside it refuses it, as Linux",
          st->size == 0 && logged("outside stolen memory"));
    done(&m);

    i915 = fresh(&m);
    st = &i915->stolen;
    i915_model_poke(&m, GEN6_STOLEN_RESERVED, GEN6_STOLEN_RESERVED_ENABLE);
    i915_stolen_init(i915);
    CHECK("stolen: a firmware part at 0 is ignored, as Linux",
          st->size == MIB(64) && st->reserved_size == 0 && st->usable_size == MIB(64));
    done(&m);

    i915 = fresh(&m);
    st = &i915->stolen;
    owned_lo = LENOVO_STOLEN_BASE + 5 * 4096u;
    owned_hi = owned_lo + 4096u;
    i915_stolen_init(i915);
    CHECK("stolen: a frame the allocator owns refuses it",
          st->size == 0 && logged("allocator owns"));
    done(&m);

    i915 = fresh(&m);
    st = &i915->stolen;
    rmrr.start = 0xaae11000ull;
    rmrr.end = 0xaae30fffull;
    i915_stolen_init(i915);
    CHECK("stolen: outside the device's reserved region refused",
          st->size == 0 && logged("reserved regions"));
    done(&m);

    i915 = fresh(&m);
    st = &i915->stolen;
    i915->gmch_ctrl = 0x0201u;
    i915_stolen_init(i915);
    CHECK("stolen: none when the control word gives none", st->size == 0 && logged("none"));
    done(&m);
}

/* ---- the global GTT ---- */

/** The bind's GTT on the Lenovo: its sizes, the scratch page, the
 * firmware's framebuffer kept and every other entry cleared. */
static void row_ggtt_init(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m);
    struct i915_ggtt *g = &i915->ggtt;
    uint32_t i, kept_ok = 1, scratch_ok = 1;

    CHECK("gtt: the power well first", i915_power_init(i915) == 0);
    CHECK("gtt: up", i915_ggtt_init(i915) == 0);
    CHECK("gtt: 2 GiB of address space, 524288 entries",
          g->total == MIB(2048) && i915->hw.gtt_entries == 524288);
    CHECK("gtt: a 256 MiB aperture, from MSAC", g->mappable_end == MIB(256));
    CHECK("gtt: one scratch frame", frames_out == 1 && g->scratch == frame_next);
    CHECK("gtt: the scratch entry: valid, uncached",
          g->scratch_pte == ((uint32_t)frame_next | GEN6_PTE_VALID));
    CHECK("gtt: the firmware's framebuffer kept",
          g->nkept == 1 && g->kept[0].start == LENOVO_FW_FB_GGTT &&
              g->kept[0].end == LENOVO_FW_FB_GGTT + LENOVO_FW_FB_BYTES);
    for (i = 0; i < 524288u; i++) {
        uint32_t pte = i915_ggtt_pte(i915, (uint64_t)i * 4096u);
        if (i < LENOVO_FW_FB_BYTES / 4096u) {
            uint32_t want = HSW_GTT_ADDR_ENCODE(LENOVO_STOLEN_BASE + i * 4096u) | GEN6_PTE_VALID;
            kept_ok &= pte == want;
        } else {
            scratch_ok &= pte == g->scratch_pte;
        }
    }
    CHECK("gtt: the firmware's 1032 entries as they were", kept_ok);
    CHECK("gtt: every other entry the scratch page's", scratch_ok);
    CHECK("gtt: no write reached a kept entry",
          i915_model_count(&m, I915_MODEL_WRITE, I915_GSM_OFFSET) == 0 &&
              i915_model_count(&m, I915_MODEL_WRITE,
                               I915_GSM_OFFSET + (LENOVO_FW_FB_BYTES / 4096u - 1) * 4u) == 0);
    CHECK("gtt: its cache flushed",
          i915_model_find_write(&m, GFX_FLSH_CNTL_GEN6, GFX_FLSH_CNTL_EN, 0) >= 0 ||
              i915_model_peek(&m, GFX_FLSH_CNTL_GEN6) == GFX_FLSH_CNTL_EN);
    CHECK("gtt: logged", logged("2048 MiB, 256 MiB of it through the aperture"));
    CHECK("gtt: past its end reads all ones", i915_ggtt_pte(i915, g->total) == 0xffffffffu);
    i915_ggtt_fini(i915);
    CHECK("gtt: the scratch page given back",
          frames_freed == 1 && freed_frame == frame_next && g->scratch == 0);
    i915_ggtt_fini(i915);
    CHECK("gtt: given back once", frames_freed == 1);
    done(&m);
}

/** Haswell's entry format: the address's high bits and the cache bits. */
static void row_pte(void)
{
    CHECK("pte: uncached", i915_ggtt_pte_encode(0xaba00000ull, I915_CACHE_NONE) == 0xaba00001u);
    CHECK("pte: LLC, write-back, age 3",
          i915_ggtt_pte_encode(0xaba00000ull, I915_CACHE_LLC) == (0xaba00001u | HSW_WB_LLC_AGE3));
    CHECK("pte: L3 and LLC as LLC on Haswell without eDRAM",
          i915_ggtt_pte_encode(0x1000ull, I915_CACHE_L3_LLC) == (0x1001u | HSW_WB_LLC_AGE3));
    CHECK("pte: write-through as LLC, as hsw_pte_encode",
          i915_ggtt_pte_encode(0x1000ull, I915_CACHE_WT) == (0x1001u | HSW_WB_LLC_AGE3));
    CHECK("pte: bits 38:32 in bits 10:4",
          i915_ggtt_pte_encode(0x44f5ff000ull, I915_CACHE_NONE) == 0x4f5ff041u);
    CHECK("pte: bit 38", i915_ggtt_pte_encode(0x4000000000ull, I915_CACHE_NONE) == 0x401u);
    CHECK("pte: age 3 write-back is bit 2", HSW_WB_LLC_AGE3 == 0x4u);
}

/** Insert and clear, and what they refuse. */
static void row_ggtt_insert(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m);
    uint64_t pages[3] = {0x1000ull, 0x44f5ff000ull, 0x2000ull};
    uint64_t bad[2] = {0x3000ull, 0x4001ull};
    uint64_t far[1] = {1ull << 39};
    uint64_t at = MIB(512);
    uint32_t flushes, writes;

    (void)i915_power_init(i915);
    CHECK("insert: the GTT up", i915_ggtt_init(i915) == 0);
    m.nlog = 0;
    m.log_lost = 0;
    flushes = 0;
    CHECK("insert: three pages", i915_ggtt_insert(i915, at, pages, 3, I915_CACHE_LLC) == 0);
    CHECK("insert: each entry",
          i915_ggtt_pte(i915, at) == i915_ggtt_pte_encode(0x1000ull, I915_CACHE_LLC) &&
              i915_ggtt_pte(i915, at + 4096u) == 0x4f5ff045u &&
              i915_ggtt_pte(i915, at + 8192u) == i915_ggtt_pte_encode(0x2000ull, I915_CACHE_LLC));
    CHECK("insert: the next entry untouched",
          i915_ggtt_pte(i915, at + 12288u) == i915->ggtt.scratch_pte);
    CHECK("insert: the GTT's cache flushed once",
          i915_model_count(&m, I915_MODEL_WRITE, GFX_FLSH_CNTL_GEN6) == flushes + 1);
    CHECK("clear: back to the scratch page",
          i915_ggtt_clear_range(i915, at, 3 * 4096u) == 0 &&
              i915_ggtt_pte(i915, at) == i915->ggtt.scratch_pte &&
              i915_ggtt_pte(i915, at + 8192u) == i915->ggtt.scratch_pte);
    CHECK("clear: flushed",
          i915_model_count(&m, I915_MODEL_WRITE, GFX_FLSH_CNTL_GEN6) == flushes + 2);

    CHECK("refuse: inside the firmware's framebuffer",
          i915_ggtt_insert(i915, 0x1000u, pages, 1, I915_CACHE_NONE) == -EBUSY);
    CHECK("refuse: across its end",
          i915_ggtt_insert(i915, LENOVO_FW_FB_BYTES - 4096u, pages, 2, I915_CACHE_NONE) == -EBUSY);
    CHECK("refuse: clearing it", i915_ggtt_clear_range(i915, 0, LENOVO_FW_FB_BYTES) == -EBUSY);
    CHECK("refuse: just past it is allowed",
          i915_ggtt_insert(i915, LENOVO_FW_FB_BYTES, pages, 1, I915_CACHE_NONE) == 0);
    writes = m.writes;
    CHECK("refuse: an offset not of whole pages",
          i915_ggtt_insert(i915, at + 8u, pages, 1, I915_CACHE_NONE) == -EINVAL);
    CHECK("refuse: a page not aligned, with nothing written",
          i915_ggtt_insert(i915, at, bad, 2, I915_CACHE_NONE) == -EINVAL && m.writes == writes &&
              i915_ggtt_pte(i915, at) == i915->ggtt.scratch_pte);
    CHECK("refuse: an address past 39 bits",
          i915_ggtt_insert(i915, at, far, 1, I915_CACHE_NONE) == -EINVAL);
    CHECK("refuse: past the GTT's end",
          i915_ggtt_insert(i915, i915->ggtt.total, pages, 1, I915_CACHE_NONE) == -EINVAL);
    CHECK("refuse: across the GTT's end",
          i915_ggtt_insert(i915, i915->ggtt.total - 4096u, pages, 2, I915_CACHE_NONE) == -EINVAL);
    CHECK("refuse: nothing at all",
          i915_ggtt_insert(i915, at, pages, 0, I915_CACHE_NONE) == -EINVAL);
    CHECK("refuse: a clear of no bytes", i915_ggtt_clear_range(i915, at, 0) == -EINVAL);
    CHECK("refuse: none of them wrote", m.writes == writes);
    i915_ggtt_fini(i915);
    done(&m);
}

/** The aperture's codes, the firmware's other planes, and the failures. */
static void row_ggtt_cases(void)
{
    struct i915_model m;
    struct drm_i915_private *i915;
    static const struct {
        uint8_t msac;
        uint64_t size;
    } codes[] = {{0x00, MIB(128)}, {0x02, MIB(256)}, {0x06, MIB(512)}, {0x04, MIB(128)}};
    unsigned i;

    for (i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
        i915 = fresh(&m);
        m.config[HSW_MSAC] = codes[i].msac;
        CHECK("aperture: up", i915_ggtt_init(i915) == 0);
        CHECK("aperture: its size by MSAC's code", i915->ggtt.mappable_end == codes[i].size);
        i915_ggtt_fini(i915);
        done(&m);
    }

    i915 = fresh(&m);
    i915_model_poke(&m, DSPCNTR(I915_PIPE_B), DISP_ENABLE | DISP_TILED);
    i915_model_poke(&m, DSPSURF(I915_PIPE_B), 0x01000000u);
    i915_model_poke(&m, DSPSTRIDE(I915_PIPE_B), 8192u);
    i915_model_poke(&m, PIPESRC(I915_PIPE_B), (1919u << 16) | 1079u);
    CHECK("planes: up with the well on", i915_ggtt_init(i915) == 0);
    CHECK("planes: pipe B's tiled surface kept too, in whole tile rows",
          i915->ggtt.nkept == 2 && i915->ggtt.kept[1].start == 0x01000000u &&
              i915->ggtt.kept[1].end == 0x01000000u + 8192u * 1080u);
    i915_ggtt_fini(i915);
    done(&m);

    i915 = fresh(&m);
    i915_model_power(&m)->req[0] = 0;
    i915_model_power(&m)->on = 0;
    i915_model_poke(&m, DSPCNTR(I915_PIPE_B), DISP_ENABLE);
    i915_model_poke(&m, DSPSTRIDE(I915_PIPE_B), 8192u);
    CHECK("planes: up with the well off", i915_ggtt_init(i915) == 0);
    CHECK("planes: pipe B not read while its well is off",
          i915->ggtt.nkept == 1 &&
              i915_model_count(&m, I915_MODEL_READ, DSPCNTR(I915_PIPE_B)) == 0);
    i915_ggtt_fini(i915);
    done(&m);

    i915 = fresh(&m);
    i915_model_poke(&m, PIPESRC(I915_PIPE_A), (1365u << 16) | 764u);
    i915_model_poke(&m, DSPCNTR(I915_PIPE_A), DISP_ENABLE | DISP_TILED);
    CHECK("tiled: up", i915_ggtt_init(i915) == 0);
    CHECK("tiled: 765 rows kept as 768",
          i915->ggtt.kept[0].end == ((uint64_t)LENOVO_FW_FB_STRIDE * 768u + 4095u) / 4096u * 4096u);
    i915_ggtt_fini(i915);
    done(&m);

    i915 = fresh(&m);
    i915_model_poke(&m, DSPCNTR(I915_PIPE_A), 0);
    CHECK("no plane: up", i915_ggtt_init(i915) == 0);
    CHECK("no plane: nothing kept, every entry cleared",
          i915->ggtt.nkept == 0 && i915_ggtt_pte(i915, 0) == i915->ggtt.scratch_pte);
    i915_ggtt_fini(i915);
    done(&m);

    i915 = fresh(&m);
    i915->gmch_ctrl = (uint16_t)(i915->gmch_ctrl & ~(SNB_GMCH_GGMS_MASK << SNB_GMCH_GGMS_SHIFT));
    CHECK("no GTT: refused", i915_ggtt_init(i915) == -ENODEV && frames_out == 0);
    done(&m);

    i915 = fresh(&m);
    frame_refuse = 1;
    CHECK("no frame: refused", i915_ggtt_init(i915) == -ENOMEM && logged("scratch"));
    done(&m);

    i915 = fresh(&m);
    zero_refuse = 1;
    CHECK("no clear: refused, the frame given back",
          i915_ggtt_init(i915) == -ENOMEM && frames_freed == 1 && i915->ggtt.scratch == 0);
    done(&m);

    i915 = fresh(&m);
    i915->hw.size = I915_MMIO_SIZE;
    CHECK("short backend: the entries out of its reach",
          i915_ggtt_init(i915) == -ENXIO && frames_out == 0);
    done(&m);
}

/** The MMIO backend maps the entries write-combining, uncached without a
 * page attribute table, and writes them there. */
static void row_mmio_gtt(void)
{
    struct i915_hw hw;
    uint32_t *regs;

    maps = 0;
    wc = 1;
    CHECK("mmio: the registers mapped",
          i915_hw_mmio_init(&hw, LENOVO_GPU_BAR0, I915_MMIO_SIZE) == 0 && maps == 1);
    regs = map_buffers[0];
    CHECK("mmio: no entries before the GTT's init",
          hw.gtt_entries == 0 && hw.gsm == 0 && i915_gtt_read(&hw, 0) == 0xffffffffu);
    CHECK("mmio: the entries mapped",
          i915_hw_gtt_init(&hw, LENOVO_GPU_BAR0 + I915_GSM_OFFSET, 0x200000u) == 0 && maps == 2);
    CHECK("mmio: write-combining", map_cache == PTE_WC && map_size == 0x200000u);
    CHECK("mmio: 524288 entries", hw.gtt_entries == 524288 && hw.gsm == (void *)map_buffers[1]);
    i915_gtt_write(&hw, 7, 0xabcd1001u);
    CHECK("mmio: an entry written through the map, not the registers'",
          map_buffers[1][7] == 0xabcd1001u && i915_gtt_read(&hw, 7) == 0xabcd1001u && regs[7] == 0);
    i915_gtt_write(&hw, 524288, 1);
    CHECK("mmio: past the entries nothing", i915_gtt_read(&hw, 524288) == 0xffffffffu);
    free(map_buffers[0]);
    free(map_buffers[1]);

    maps = 0;
    wc = 0;
    CHECK("mmio: no PAT",
          i915_hw_mmio_init(&hw, LENOVO_GPU_BAR0, I915_MMIO_SIZE) == 0 &&
              i915_hw_gtt_init(&hw, LENOVO_GPU_BAR0 + I915_GSM_OFFSET, 0x200000u) == 0);
    CHECK("mmio: uncached without a page attribute table", map_cache == PTE_NOCACHE);
    free(map_buffers[0]);
    free(map_buffers[1]);
    wc = 1;
}

/** Run every row; 0 when all pass. */
int main(void)
{
    row_uncore_init();
    row_forcewake();
    row_forcewake_slow_stuck();
    row_uncore_read();
    row_fifo();
    row_power();
    row_power_kept();
    row_power_slow_stuck();
    row_stolen_early();
    row_stolen();
    row_pte();
    row_ggtt_init();
    row_ggtt_insert();
    row_ggtt_cases();
    row_mmio_gtt();
    if (fails) {
        printf("i915-base-test: %d failed\n", fails);
        return 1;
    }
    printf("i915-base-test: ok\n");
    return 0;
}
