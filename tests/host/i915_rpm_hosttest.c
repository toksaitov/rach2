/**
 * i915_rpm_hosttest.c: the Intel graphics driver's runtime suspend and
 * resume on Haswell, i915_runtime_pm.c, against the host model of the
 * Lenovo's HD 4400, whose LCPLL locks and unlocks as its control bits
 * ask, whose sideband reaches the PCH's clock unit, and whose pcode writes
 * D_COMP.
 *
 * host-deps: tests/host/compat/i915_time_host.c
 * host-deps: rach2/sys/drm/i915/i915_hw.c tests/host/i915_model.c
 * host-deps: rach2/sys/drm/i915/i915_uncore.c
 * host-deps: rach2/sys/drm/i915/i915_power.c rach2/sys/drm/i915/i915_irq.c
 * host-deps: rach2/sys/drm/i915/i915_rc6.c rach2/sys/drm/i915/i915_rps.c
 * host-deps: rach2/sys/tick.c rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/spinlock_host.c
 * host-deps: tests/host/compat/mutex_none.c
 *
 * i915_runtime_pm.c is included whole, and a row calls its callbacks in
 * the order the runtime PM core calls them: `rpm_prepare`, then
 * `rpm_suspend`, and later `rpm_resume`. A row makes a fresh model and a
 * device record over it as the bind leaves it: forcewake up, the
 * interrupts reset and installed with a source unmasked in each bank, the
 * display power well taken over from the BIOS, and the display ready and
 * off, as a DPMS off leaves it. The model's backend is not the MMIO one,
 * so no row puts the function in D3hot. The GEM objects, the hotplug
 * detection, the fences and the clock gating are records of the calls
 * made to them.
 *
 * The rows: a suspend that writes Linux's sequence of hsw_enable_pc8 in
 * its order, every interrupt masked and disabled before it, and the device
 * then refusing every access; a resume that writes hsw_disable_pc8's under
 * forcewake and gives every bank its mask and enable back, twice over;
 * each of Linux's conditions of assert_can_disable_lcpll, forcewake held
 * and the well in use refusing the sleep before anything is written; a
 * GTT fault after the prepare refusing the suspend; and a sideband, an
 * LCPLL and a pcode that never answer, whose waits end at their bounds
 * with the suspend and resume carried through, as Linux carries them.
 *
 * Returns 0 when every row passes, 1 otherwise.
 */

#include <stdarg.h>

#include "drm/i915/i915_runtime_pm.c"
#include "mm/paging.h"
#include "udelay.h"
#include "i915_model.h"
#include "i915_model_base.h"
#include "i915_model_pc8.h"
#include "i915_model_rc6.h"
#include "i915_model_sink.h"

/* The boot processor's tick count, which forcewake's release timer reads. */
volatile uint64_t kernel_ticks;

extern int printf(const char *format, ...);
extern int vsnprintf(char *s, unsigned long n, const char *format, va_list ap);
extern char *strstr(const char *s, const char *t);
extern int strcmp(const char *a, const char *b);

static int fails;

#define CHECK(what, cond)                              \
    do {                                               \
        if (!(cond)) {                                 \
            printf("i915-rpm-test: fail: %s\n", what); \
            fails++;                                   \
        }                                              \
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

uint32_t kirq_save(void)
{
    return 0;
}

void kirq_restore(uint32_t flags)
{
    (void)flags;
}

void k_udelay(uint32_t usec)
{
    (void)usec;
}

/** Configuration space, which no row reaches: all ones, as an absent
 * function answers. */
uint16_t pci_read_config16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset)
{
    (void)bus;
    (void)device;
    (void)function;
    (void)offset;
    return 0xffffu;
}

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

/* ---- the calls outside the registers, recorded in order ---- */

static char calls[512];

/** Add `name` and a space to the record of calls. */
static void called(const char *name)
{
    unsigned long n = 0, k;

    while (calls[n]) {
        n++;
    }
    for (k = 0; name[k] && n + 2 < sizeof(calls); k++) {
        calls[n++] = name[k];
    }
    calls[n++] = ' ';
    calls[n] = 0;
}

/* The GEM objects: one, with a GTT map a program faulted in. */
const struct drm_gem_object_funcs i915_gem_object_funcs;
static struct drm_i915_gem_object test_obj;
static int gem_busy;

struct drm_gem_object *drm_gem_object_next(struct drm_device *dev, struct drm_gem_object *prev)
{
    (void)dev;
    return prev ? 0 : &test_obj.base;
}

int i915_gem_trylock(struct drm_i915_private *i915, int *taken)
{
    (void)i915;
    *taken = 1;
    return !gem_busy;
}

void i915_gem_unlock(struct drm_i915_private *i915, int taken)
{
    (void)i915;
    (void)taken;
}

void i915_gem_object_release_mmap_gtt(struct drm_i915_gem_object *obj)
{
    obj->userfault_count = 0;
    called("revoke");
}

void intel_hotplug_suspend(struct drm_i915_private *i915)
{
    CHECK("hotplug: suspended once every access is refused", i915->hw.asleep);
    called("hotplug-suspend");
}

void intel_hotplug_resume(struct drm_i915_private *i915)
{
    CHECK("hotplug: resumed with the interrupts back", !i915->irq.suspended);
    called("hotplug-resume");
}

/** No handler runs on another processor; the call is recorded, with
 * whether every source was already off. */
void i915_irq_sync(struct drm_i915_private *i915)
{
    called(i915->irq.suspended ? "irq-sync" : "irq-sync-early");
}

void i915_gem_restore_fences(struct drm_i915_private *i915)
{
    (void)i915;
    called("fences");
}

void hsw_display_clock_gating(struct drm_i915_private *i915)
{
    (void)i915;
    called("display-gating");
}

void i915_gt_init_clock_gating(struct drm_i915_private *i915)
{
    (void)i915;
    called("gt-gating");
}

/* The PCI core, which the model's backend never reaches: no capability. */
uint8_t pci_find_capability(uint8_t bus, uint8_t device, uint8_t function, uint8_t id)
{
    (void)bus;
    (void)device;
    (void)function;
    (void)id;
    return 0;
}

void pci_save_state(const pci_device_t *dev, struct pci_saved_state *st)
{
    (void)dev;
    (void)st;
    called("pci-save");
}

void pci_restore_state(const pci_device_t *dev, const struct pci_saved_state *st)
{
    (void)dev;
    (void)st;
    called("pci-restore");
}

int pci_set_power_state(const pci_device_t *dev, int state)
{
    (void)dev;
    (void)state;
    called("pci-power");
    return 0;
}

int pci_msi_enable(const pci_device_t *dev, uint8_t vector)
{
    (void)dev;
    (void)vector;
    return 0;
}

/* The runtime PM core, whose callbacks the rows call themselves in its
 * order, and /sys, which no row reads. */
void pm_runtime_init(struct pm_runtime *rpm, const struct pm_runtime_ops *ops, void *owner)
{
    rpm->ops = ops;
    rpm->owner = owner;
}

int pm_runtime_sysfs_add(struct pm_runtime *rpm, int parent)
{
    (void)rpm;
    return parent;
}

void pm_runtime_sysfs_remove(struct pm_runtime *rpm)
{
    (void)rpm;
}

void pm_runtime_enable(struct pm_runtime *rpm)
{
    (void)rpm;
}

int32_t pm_runtime_disable(struct pm_runtime *rpm)
{
    (void)rpm;
    return 0;
}

int32_t pm_runtime_get_sync(struct pm_runtime *rpm)
{
    (void)rpm;
    return 0;
}

int pm_runtime_get_if_active(struct pm_runtime *rpm)
{
    (void)rpm;
    return 1;
}

void pm_runtime_get_noresume(struct pm_runtime *rpm)
{
    (void)rpm;
}

void pm_runtime_put(struct pm_runtime *rpm)
{
    (void)rpm;
}

void pm_runtime_mark_busy(struct pm_runtime *rpm, uint32_t ms)
{
    (void)rpm;
    (void)ms;
}

void pm_runtime_set_delay(struct pm_runtime *rpm, int32_t ms)
{
    (void)rpm;
    (void)ms;
}

void pm_runtime_idle(struct pm_runtime *rpm)
{
    (void)rpm;
}

int32_t pm_runtime_usage(struct pm_runtime *rpm)
{
    (void)rpm;
    return 0;
}

void drm_dev_work_later_ms(struct drm_device *dev, struct drm_work *work, uint32_t ms)
{
    (void)dev;
    (void)work;
    (void)ms;
}

int drm_dev_work_cancel(struct drm_work *work)
{
    (void)work;
    return 0;
}

int drm_sysfs_card_dir(const struct drm_device *dev)
{
    (void)dev;
    return -ENOENT;
}

int sysfs_anchor(int node, int key)
{
    (void)node;
    (void)key;
    return -ENOENT;
}

int sysfs_add_dir(int parent, const char *name)
{
    (void)parent;
    (void)name;
    return -ENOSPC;
}

int sysfs_add_attr(int parent, const struct sysfs_attr *attr, void *obj, int arg)
{
    (void)parent;
    (void)attr;
    (void)obj;
    (void)arg;
    return -ENOSPC;
}

int sysfs_node_child(int dir, const char *name, int len)
{
    (void)dir;
    (void)name;
    (void)len;
    return 0;
}

/* ---- the device ---- */

/* The id table: the Lenovo's chip alone, a ULT part with a low-power PCH. */
static const struct i915_platform hsw = {"HASWELL", 7, 50, 7, 2, 0xf, 1, 1, 1, 0, 1, 31, 40};
static const struct i915_id lenovo_id = {0x0A16, 1, 0, &hsw};

static struct drm_i915_private dev;

/* Each bank's mask and enable registers, in i915_irq.c's order. */
static const uint32_t bank_imr[I915_IRQ_BANKS] = {GTIMR, GEN6_PMIMR, DEIMR, SDEIMR};
static const uint32_t bank_ier[I915_IRQ_BANKS] = {GTIER, GEN6_PMIER, DEIER, SDEIER};

/** A handler for the sources a row unmasks, which no row raises. */
static void no_handler(struct drm_i915_private *i915, uint32_t bits, void *data)
{
    (void)i915;
    (void)bits;
    (void)data;
}

/**
 * A fresh model and a device record over it as the bind leaves it:
 * forcewake up, the interrupts installed with one source of each bank
 * unmasked, the power well taken over and let go, the PCH's partition
 * level gating disabled as hsw_disable_pc8 leaves it, and the display
 * ready with the eDP transcoder, the panel and its backlight off and no
 * pipe active, as a DPMS off leaves them. The log of accesses starts
 * empty.
 */
static struct drm_i915_private *fresh(struct i915_model *m)
{
    int bank;

    CHECK("the model is made", i915_model_init(m) == 0);
    k_memset(&dev, 0, sizeof(dev));
    dev.pci = m->pci;
    dev.hw = m->hw;
    dev.id = &lenovo_id;
    dev.msi_vector = -1;
    log_text[0] = 0;
    calls[0] = 0;
    gem_busy = 0;
    k_memset(&test_obj, 0, sizeof(test_obj));
    test_obj.base.funcs = &i915_gem_object_funcs;
    test_obj.userfault_count = 1;
    i915_uncore_init(&dev);
    i915_irq_reset(&dev);
    i915_irq_install(&dev);
    for (bank = 0; bank < I915_IRQ_BANKS; bank++) {
        CHECK("a source registered", i915_irq_register(&dev, bank, 1u << 4, no_handler, 0) == 0);
        i915_irq_unmask(&dev, bank, 1u << 4);
    }
    CHECK("the well is taken over", i915_power_init(&dev) == 0);
    dev.power.init_ref = 0;
    i915_power_put(&dev);
    i915_model_poke(m, SOUTH_DSPCLK_GATE_D, PCH_LP_PARTITION_LEVEL_DISABLE);
    i915_model_poke(m, TRANSCONF(I915_TRANS_EDP), 0);
    i915_model_poke(m, PP_CONTROL, 0);
    i915_model_panel(m)->on = 0;
    i915_model_poke(m, BLC_PWM_CPU_CTL2, 0);
    i915_model_poke(m, BLC_PWM_PCH_CTL1, 0);
    dev.display.ready = 1;
    i915_rpm_init(&dev);
    m->nlog = 0;
    m->log_lost = 0;
    return &dev;
}

/** The index in the log of the first write to `reg` at or after `from`
 * whose value has every bit of `set` and none of `clear`, or -1. */
static long find_write(const struct i915_model *m, uint32_t reg, uint32_t set, uint32_t clear,
                       uint32_t from)
{
    uint32_t i;

    for (i = from; i < m->nlog; i++) {
        if (m->log[i].op == I915_MODEL_WRITE && m->log[i].reg == reg &&
            (m->log[i].value & set) == set && !(m->log[i].value & clear)) {
            return (long)i;
        }
    }
    return -1;
}

/** The index in the log of the first pcode command `cmd` posted at or
 * after `from`, or -1. */
static long find_pcode(const struct i915_model *m, uint32_t cmd, uint32_t from)
{
    uint32_t i;

    for (i = from; i < m->nlog; i++) {
        if (m->log[i].op == I915_MODEL_WRITE && m->log[i].reg == GEN6_PCODE_MAILBOX &&
            (m->log[i].value & GEN6_PCODE_READY) && (m->log[i].value & 0xffu) == cmd) {
            return (long)i;
        }
    }
    return -1;
}

/** How many writes to `reg` the log holds. */
static uint32_t writes_to(const struct i915_model *m, uint32_t reg)
{
    return i915_model_count(m, I915_MODEL_WRITE, reg);
}

/** Whether the sideband's transfer `i` was of `reg`, a write when `write`,
 * with `value`. */
static int sbi_was(const struct i915_model_pc8 *p, uint32_t i, uint32_t reg, int write,
                   uint32_t value)
{
    return i < p->nlog && p->log[i].reg == reg && p->log[i].write == write &&
           p->log[i].value == value;
}

/** Whether no wait of the driver reached its bound. */
static int no_timeout(void)
{
    return !logged("timeout");
}

/** Prepare and suspend as the core does: the two answers in `*prep` and
 * the return value. */
static int32_t sleep_once(struct drm_i915_private *i915, int32_t *prep)
{
    *prep = rpm_prepare(i915);
    return *prep == 0 ? rpm_suspend(i915) : *prep;
}

/* ---- the rows ---- */

/**
 * The sleep: the GTT map revoked; then every bank masked, disabled and
 * cleared, the partition level gating back on, CLKOUT_DP off through the
 * sideband, the CD clock on FCLK, LCPLL off, D_COMP's compensation off
 * through the pcode and LCPLL let power down, in Linux's order; then every
 * access refused and the hotplug detection suspended.
 */
static void row_suspend(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m);
    struct i915_model_pc8 *p = i915_model_pc8(&m);
    long irq_off, gate, fclk, off, dcomp, allow;
    uint32_t reads, bank;
    int32_t prep, err = sleep_once(i915, &prep);

    CHECK("suspend: prepared", prep == 0);
    CHECK("suspend: done", err == 0);
    CHECK("suspend: the GTT map revoked first", strstr(calls, "revoke ") == calls);
    CHECK("suspend: the handlers waited for once every source is off",
          strstr(calls, "revoke irq-sync hotplug-suspend ") == calls);
    CHECK("suspend: no wait reached its bound", no_timeout());
    CHECK("suspend: the well is off", !i915_model_power(&m)->on);
    for (bank = 0; bank < I915_IRQ_BANKS; bank++) {
        CHECK("suspend: every bank masked", i915_model_peek(&m, bank_imr[bank]) == 0xffffffffu);
        CHECK("suspend: every bank disabled", i915_model_peek(&m, bank_ier[bank]) == 0);
    }
    irq_off = find_write(&m, SDEIER, 0, 0xffffffffu, 0);
    gate = find_write(&m, SOUTH_DSPCLK_GATE_D, 0, PCH_LP_PARTITION_LEVEL_DISABLE, 0);
    fclk = find_write(&m, LCPLL_CTL, LCPLL_CD_SOURCE_FCLK, LCPLL_PLL_DISABLE, 0);
    off = find_write(&m, LCPLL_CTL, LCPLL_CD_SOURCE_FCLK | LCPLL_PLL_DISABLE,
                     LCPLL_POWER_DOWN_ALLOW, 0);
    dcomp = find_pcode(&m, GEN6_PCODE_WRITE_D_COMP, 0);
    allow = find_write(&m, LCPLL_CTL, LCPLL_POWER_DOWN_ALLOW | LCPLL_PLL_DISABLE, 0, 0);
    CHECK("suspend: the interrupts off before package C8", irq_off >= 0 && irq_off < gate);
    CHECK("suspend: the partition level gating on", gate >= 0);
    CHECK("suspend: the CD clock on FCLK, then LCPLL off", fclk > gate && off > fclk);
    CHECK("suspend: D_COMP after LCPLL, then the power down allowed", dcomp > off && allow > dcomp);
    CHECK("suspend: the sideband's five transfers", p->transfers == 5);
    CHECK("suspend: SBI_GEN0's buffer read and disabled",
          sbi_was(p, 0, SBI_GEN0, 0, SBI_GEN0_CFG_BUFFENABLE_DISABLE) &&
              sbi_was(p, 1, SBI_GEN0, 1, 0));
    CHECK("suspend: SSC moved to its other path, then off",
          sbi_was(p, 2, SBI_SSCCTL, 0, 0) && sbi_was(p, 3, SBI_SSCCTL, 1, SBI_SSCCTL_PATHALT) &&
              sbi_was(p, 4, SBI_SSCCTL, 1, SBI_SSCCTL_PATHALT | SBI_SSCCTL_DISABLE));
    CHECK("suspend: LCPLL unlocked on FCLK, allowed to power down",
          i915_model_peek(&m, LCPLL_CTL) == (LCPLL_PLL_DISABLE | LCPLL_POWER_DOWN_ALLOW |
                                             LCPLL_CD_SOURCE_FCLK | LCPLL_CD_SOURCE_FCLK_DONE));
    CHECK("suspend: D_COMP's compensation off",
          i915_model_peek(&m, D_COMP_HSW) & D_COMP_COMP_DISABLE);
    CHECK("suspend: package C8 allowed", i915->rpm.pc8 == 1);
    CHECK("suspend: the device asleep", i915->hw.asleep == 1);
    CHECK("suspend: the hotplug detection suspended last",
          strstr(calls, "hotplug-suspend") != 0 && i915->rpm.d3 == 0);
    reads = m.reads;
    CHECK("suspend: a read refused", i915_read(&i915->hw, GTIMR) == 0xffffffffu);
    i915_write(&i915->hw, GTIMR, 0);
    CHECK("suspend: neither access reached the chip",
          m.reads == reads && i915_model_peek(&m, GTIMR) == 0xffffffffu);
    CHECK("suspend: the accesses counted and logged",
          i915->hw.asleep_accesses == 2 && logged("while suspended"));
    i915_model_free(&m);
}

/**
 * The wake: LCPLL's power down taken back, D_COMP forced on through the
 * pcode, LCPLL on and locked and the CD clock off FCLK, under forcewake;
 * CLKOUT_DP left off; the clock gating written again; and every bank's
 * mask and enable from the driver's copies. A second sleep and wake does
 * the same.
 */
static void row_resume(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m);
    struct i915_model_pc8 *p = i915_model_pc8(&m);
    uint32_t bank, from;
    int round;
    int32_t prep;

    for (round = 0; round < 2; round++) {
        CHECK("resume: asleep", sleep_once(i915, &prep) == 0);
        from = m.nlog;
        p->nlog = 0;
        p->transfers = 0;
        calls[0] = 0;
        CHECK("resume: done", rpm_resume(i915) == 0);
        CHECK("resume: no wait reached its bound", no_timeout());
        CHECK("resume: awake", i915->hw.asleep == 0 && i915->rpm.pc8 == 0);
        CHECK("resume: no access refused while asleep", !logged("accesses while suspended"));
        {
            long fw = find_write(&m, FORCEWAKE_MT, 1u, 0, from);
            long down = find_write(&m, LCPLL_CTL, LCPLL_PLL_DISABLE, LCPLL_POWER_DOWN_ALLOW, from);
            long dcomp = find_pcode(&m, GEN6_PCODE_WRITE_D_COMP, from);
            long on =
                find_write(&m, LCPLL_CTL, 0, LCPLL_PLL_DISABLE | LCPLL_POWER_DOWN_ALLOW, from);
            long cd =
                find_write(&m, LCPLL_CTL, 0,
                           LCPLL_PLL_DISABLE | LCPLL_POWER_DOWN_ALLOW | LCPLL_CD_SOURCE_FCLK, from);

            CHECK("resume: forcewake taken first", fw >= 0 && fw < down);
            CHECK("resume: the power down taken back, then D_COMP", down >= 0 && dcomp > down);
            CHECK("resume: LCPLL on after D_COMP, then the CD clock off FCLK",
                  on > dcomp && cd > on);
        }
        CHECK("resume: D_COMP forced on",
              (i915_model_peek(&m, D_COMP_HSW) & (D_COMP_COMP_FORCE | D_COMP_COMP_DISABLE)) ==
                  D_COMP_COMP_FORCE);
        CHECK("resume: LCPLL locked on the CD clock's source",
              i915_model_peek(&m, LCPLL_CTL) == LCPLL_PLL_LOCK);
        CHECK("resume: CLKOUT_DP left off, as no PLL uses SSC",
              p->transfers == 3 && sbi_was(p, 1, SBI_GEN0, 1, 0) &&
                  sbi_was(p, 2, SBI_SSCCTL, 0, SBI_SSCCTL_PATHALT | SBI_SSCCTL_DISABLE));
        CHECK("resume: forcewake let go", i915->uncore.wake_count == 0);
        for (bank = 0; bank < I915_IRQ_BANKS; bank++) {
            uint32_t ier = i915->irq.ier[bank] | (bank == I915_IRQ_DE ? DE_MASTER_IRQ_CONTROL : 0);

            CHECK("resume: each bank's mask back",
                  i915_model_peek(&m, bank_imr[bank]) == i915->irq.imr[bank] &&
                      i915->irq.imr[bank] != 0xffffffffu);
            CHECK("resume: each bank's enable back", i915_model_peek(&m, bank_ier[bank]) == ier);
        }
        CHECK("resume: gating, then the interrupts, fences and hotplug",
              strcmp(calls, "display-gating gt-gating fences hotplug-resume ") == 0);
        test_obj.userfault_count = 1;
    }
    i915_model_free(&m);
}

/** Make the sleep wait on the state `what` sets up and check that it is
 * refused as `why` with nothing written to the PLL, the sideband or the
 * interrupts. */
static void refused(const char *what, void (*set)(struct i915_model *, struct drm_i915_private *),
                    const char *why)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m);
    int32_t prep, err;

    set(&m, i915);
    m.nlog = 0;
    err = sleep_once(i915, &prep);
    if (err != -EBUSY || prep != -EBUSY) {
        printf("i915-rpm-test: fail: %s: prepare %d, suspend %d\n", what, (int)prep, (int)err);
        fails++;
    }
    if (!i915->rpm.refused || strcmp(i915->rpm.refused, why) != 0) {
        printf("i915-rpm-test: fail: %s: refused as '%s'\n", what,
               i915->rpm.refused ? i915->rpm.refused : "");
        fails++;
    }
    CHECK("refused: nothing written to LCPLL, the sideband or the interrupts",
          writes_to(&m, LCPLL_CTL) == 0 && writes_to(&m, SBI_CTL_STAT) == 0 &&
              writes_to(&m, GTIMR) == 0 && writes_to(&m, SDEIER) == 0);
    CHECK("refused: awake, the map kept", !i915->hw.asleep && test_obj.userfault_count == 1);
    i915_model_free(&m);
}

/** The display still the firmware's, before the driver read it. */
static void set_firmware(struct i915_model *m, struct drm_i915_private *i915)
{
    (void)m;
    i915->display.ready = 0;
}

/** Forcewake held by the driver. */
static void set_forcewake(struct i915_model *m, struct drm_i915_private *i915)
{
    (void)m;
    CHECK("forcewake taken", i915_forcewake_get(i915) == 0);
}

/** The display well held by the driver. */
static void set_well(struct i915_model *m, struct drm_i915_private *i915)
{
    (void)m;
    CHECK("the well taken", i915_power_get(i915) == 0);
}

/** Pipe B active. */
static void set_pipe(struct i915_model *m, struct drm_i915_private *i915)
{
    (void)m;
    i915->display.crtc[1].cfg.active = 1;
}

/** The SPLL running. */
static void set_spll(struct i915_model *m, struct drm_i915_private *i915)
{
    (void)i915;
    i915_model_poke(m, SPLL_CTL, SPLL_PLL_ENABLE);
}

/** The second WRPLL running. */
static void set_wrpll(struct i915_model *m, struct drm_i915_private *i915)
{
    (void)i915;
    i915_model_poke(m, WRPLL_CTL(1), WRPLL_PLL_ENABLE);
}

/** The CPU's backlight PWM on. */
static void set_pwm(struct i915_model *m, struct drm_i915_private *i915)
{
    (void)i915;
    i915_model_poke(m, BLC_PWM_CPU_CTL2, BLM_PWM_ENABLE);
}

/** The PCH's backlight PWM on. */
static void set_pch_pwm(struct i915_model *m, struct drm_i915_private *i915)
{
    (void)i915;
    i915_model_poke(m, BLC_PWM_PCH_CTL1, BLM_PCH_PWM_ENABLE);
}

/** The utility pin a PWM. */
static void set_util(struct i915_model *m, struct drm_i915_private *i915)
{
    (void)i915;
    i915_model_poke(m, UTIL_PIN_CTL, UTIL_PIN_ENABLE | UTIL_PIN_MODE_PWM);
}

/** The PCH's time code on. */
static void set_gtc(struct i915_model *m, struct drm_i915_private *i915)
{
    (void)i915;
    i915_model_poke(m, PCH_GTC_CTL, PCH_GTC_ENABLE);
}

/** Each condition of Linux's assert_can_disable_lcpll, and forcewake held
 * and the well in use, refuse the sleep before anything is written. */
static void row_refused(void)
{
    refused("firmware's display", set_firmware, "the display is the firmware's");
    refused("forcewake", set_forcewake, "forcewake is held");
    refused("well", set_well, "the display well is in use");
    refused("pipe", set_pipe, "a pipe is active");
    refused("spll", set_spll, "a PLL runs");
    refused("wrpll", set_wrpll, "a PLL runs");
    refused("cpu pwm", set_pwm, "a backlight PWM is on");
    refused("pch pwm", set_pch_pwm, "a backlight PWM is on");
    refused("utility pin", set_util, "the utility pin is a PWM");
    refused("gtc", set_gtc, "the PCH's time code is on");
}

/** A GTT fault after the prepare, or the GEM lock held at it, refuses the
 * sleep before anything is written, and the next try sleeps. */
static void row_fault(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m);
    int32_t prep;

    gem_busy = 1;
    CHECK("fault: the GEM lock held refuses the prepare", rpm_prepare(i915) == -EBUSY);
    gem_busy = 0;
    CHECK("fault: prepared", rpm_prepare(i915) == 0);
    i915->rpm.faults++;
    CHECK("fault: the suspend refused", rpm_suspend(i915) == -EAGAIN);
    CHECK("fault: nothing written",
          writes_to(&m, LCPLL_CTL) == 0 && writes_to(&m, GTIMR) == 0 && !i915->hw.asleep);
    test_obj.userfault_count = 1;
    CHECK("fault: the next try sleeps", sleep_once(i915, &prep) == 0 && i915->hw.asleep);
    CHECK("fault: and wakes", rpm_resume(i915) == 0 && !i915->hw.asleep);
    i915_model_free(&m);
}

/**
 * A sideband that stays busy, an LCPLL that neither unlocks nor locks and
 * a pcode that never answers: each wait ends at its bound and is logged,
 * the sleep and the wake are carried through as Linux's are, and the two
 * take less than 450 ms of the model's clock, the sum of the bounds: four
 * sideband waits of 100 ms, two on each side, LCPLL's unlock of 1 ms and
 * lock of 5 ms, the pcode's 20.5 ms and D_COMP's 1 ms.
 */
static void row_stuck(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m);
    uint64_t start = m.now_us;
    int32_t prep;

    i915_model_pc8(&m)->sbi_stuck = 1;
    i915_model_pc8(&m)->lcpll_stuck = 1;
    i915_model_rc6(&m)->stuck = 1;
    i915_model_poke(&m, SBI_CTL_STAT, SBI_BUSY);
    CHECK("stuck: asleep", sleep_once(i915, &prep) == 0 && i915->hw.asleep && i915->rpm.pc8);
    CHECK("stuck: the sideband's waits logged", logged("sbi ready: timeout"));
    CHECK("stuck: LCPLL's waits logged",
          logged("lcpll fclk: timeout") && logged("lcpll unlock: timeout"));
    CHECK("stuck: D_COMP's write logged", logged("D_COMP not written"));
    CHECK("stuck: LCPLL let power down", i915_model_peek(&m, LCPLL_CTL) & LCPLL_POWER_DOWN_ALLOW);
    /* LCPLL unlocked at last, and stuck so. */
    i915_model_poke(&m, LCPLL_CTL, i915_model_peek(&m, LCPLL_CTL) & ~LCPLL_PLL_LOCK);
    CHECK("stuck: awake", rpm_resume(i915) == 0 && !i915->hw.asleep && !i915->rpm.pc8);
    CHECK("stuck: LCPLL's lock wait logged", logged("lcpll lock: timeout"));
    CHECK("stuck: the power down taken back",
          !(i915_model_peek(&m, LCPLL_CTL) & LCPLL_POWER_DOWN_ALLOW));
    CHECK("stuck: within the sum of Linux's bounds", m.now_us - start < 450000u);
    i915_model_free(&m);
}

int main(void)
{
    row_suspend();
    row_resume();
    row_refused();
    row_fault();
    row_stuck();
    if (fails != 0) {
        printf("i915-rpm-test: %d failures\n", fails);
        return 1;
    }
    printf("i915-rpm-test: ok\n");
    return 0;
}
