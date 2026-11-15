/*
 * i915_rpm_hosttest.c: the Intel graphics driver's runtime suspend and
 * resume, the path of 76-5261's trial on the Lenovo, run whole against the
 * host model of its HD 4400 with the display off.
 *
 * host-deps: tests/host/compat/i915_time_host.c
 * host-deps: rach2/sys/drm/i915/i915_irq.c rach2/sys/drm/i915/i915_uncore.c
 * host-deps: rach2/sys/drm/i915/i915_power.c rach2/sys/drm/i915/i915_hw.c
 * host-deps: rach2/sys/drm/i915/i915_rps.c rach2/sys/drm/i915/i915_rc6.c
 * host-deps: tests/host/i915_model.c rach2/sys/pci.c
 * host-deps: rach2/sys/tick.c rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: rach2/sys/lib/kstd.c
 * host-deps: tests/host/compat/spinlock_host.c
 * host-deps: tests/host/compat/i915_gem_lock_host.c
 * host-deps: tests/host/compat/kheap_host.c
 *
 * i915_runtime_pm.c and i915_pc8.c are included whole; the interrupts,
 * forcewake, the display power well, the pcode, the PCI function's power
 * state and saved header are the real code. What the suspend calls and
 * every bind already runs on the Lenovo, the panel's VDD, hotplug's enables,
 * the fences and the clock gating, is a record of the calls here. The
 * model gives LCPLL, the PCH's sideband and D_COMP their behaviour through
 * its read hook: LCPLL loses its lock 200 us after it is disabled and wins
 * it back 500 us after it is enabled, its CD clock switches at once, and
 * the sideband answers each command from a table of the clock unit's
 * registers. Configuration space is the model's, with the Lenovo's power
 * management capability; a function in D3hot that the driver reaches
 * through BAR 0 is counted, as a real one would answer all ones.
 *
 * The rows: runtime PM enabled at the bind's end with Linux's defaults and
 * the power directory's files; the autosuspend work before its delay; the
 * suspend, in Linux's order, with no wait at its bound, every interrupt off
 * and every access to BAR 0 before D3hot; the registers read as all ones
 * and the chip untouched while it sleeps; the resume, D0 before any access
 * and every part of the suspend undone; the GTT's clears deferred and
 * replayed; a suspend refused while the GT is awake or forcewake held,
 * with the device as it was; control "on" and a negative delay holding the
 * device; and the unplug resuming it for good.
 *
 * Returns 0 when every row passes, 1 otherwise.
 */

#include <stdarg.h>

#include "drm/i915/i915_runtime_pm.c"
#include "drm/i915/i915_pc8.c"
#include "drm/i915/i915_reg.h"
#include "i915_model.h"
#include "i915_model_ring.h"
#include "i915_lenovo.h"

/* The PCI power management capability's control word, as pci.c has it. */
#define PCI_PM_CTRL       0x04
#define PCI_PM_STATE_MASK 0x0003u

/* The banks' enable and mask registers, in i915_irq.c's order. */
static const uint32_t bank_imr[I915_IRQ_BANKS] = {GTIMR, GEN6_PMIMR, DEIMR, SDEIMR};
static const uint32_t bank_ier[I915_IRQ_BANKS] = {GTIER, GEN6_PMIER, DEIER, SDEIER};

extern int printf(const char *format, ...);
extern int vsnprintf(char *s, unsigned long n, const char *format, va_list ap);
extern char *strstr(const char *s, const char *t);
extern int strcmp(const char *a, const char *b);

/* The boot processor's tick count, the clock of rtc_monotonic_ns. */
volatile uint64_t kernel_ticks;

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

struct percpu cpu_table[NR_CPUS];

uint32_t kirq_save(void)
{
    return 0;
}

void kirq_restore(uint32_t flags)
{
    (void)flags;
}

/* One thread: nothing else holds the transition, so no get spins. */
void arch_relax(void)
{
    printf("i915-rpm-test: fail: a get spun with one thread\n");
    fails++;
}

static struct i915_model *bus;
static uint64_t udelay_us;

/** A busy wait: the model's clock moves, as the chip's does. */
void k_udelay(uint32_t usec)
{
    udelay_us += usec;
    if (bus) {
        bus->now_us += usec;
    }
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

/* pci.c's windows and messages, and the /sys of RC6 and RPS, which no row
 * reaches. */
uintptr_t mmio_map(uint64_t phys, size_t size, pte_t cache)
{
    (void)phys;
    (void)size;
    (void)cache;
    return 0;
}

void mmio_unmap(uintptr_t va, size_t size)
{
    (void)va;
    (void)size;
}

uint64_t pci_msi_cpus(void)
{
    return 1;
}

void pci_msi_message_cpu(uint8_t vector, int cpu, uint32_t *address, uint32_t *data,
                         uint32_t *destination)
{
    *address = 0xFEE00000u | (uint32_t)cpu << 12;
    *data = vector;
    *destination = (uint32_t)cpu;
}

int drm_sysfs_card_dir(const struct drm_device *d)
{
    (void)d;
    return -ENOENT;
}

int sysfs_node_child(int dir, const char *name, int len)
{
    (void)dir;
    (void)name;
    (void)len;
    return 0;
}

/* ---- configuration space: the model's ---- */

/* The model's log length at the function's last move into D3hot and out
 * of it, and the accesses to BAR 0 made while it was in D3hot. */
static uint32_t d3_at, d0_at, d3_accesses;
static int in_d3;

/** Whether (bus, device, function) is the model's chip. */
static int is_chip(uint8_t b, uint8_t d, uint8_t f)
{
    return bus && b == LENOVO_GPU_BUS && d == LENOVO_GPU_DEVICE && f == LENOVO_GPU_FUNCTION;
}

uint32_t pci_read_config32(uint8_t b, uint8_t d, uint8_t f, uint8_t offset)
{
    return is_chip(b, d, f) ? i915_model_config(bus, offset, 4) : 0xffffffffu;
}

/** The function's soft reset on leaving D3hot, its PMCSR having no
 * No_Soft_Reset: the command word, the BARs and the message's capability
 * back to 0. */
static void soft_reset(void)
{
    unsigned k;

    for (k = PCI_COMMAND; k < PCI_COMMAND + 2u; k++) {
        bus->config[k] = 0;
    }
    for (k = PCI_BAR0; k < PCI_BAR0 + 24u; k++) {
        bus->config[k] = 0;
    }
    for (k = LENOVO_GPU_CAP_MSI + 2u; k < LENOVO_GPU_CAP_MSI + 10u; k++) {
        bus->config[k] = 0;
    }
}

/** Note the function's moves into and out of D3hot by its PMCSR. */
static void pmcsr_written(unsigned at)
{
    unsigned pm = LENOVO_GPU_CAP_PM + PCI_PM_CTRL;
    int d3;

    if (at > pm + 1 || at + 4 <= pm) {
        return;
    }
    d3 = (bus->config[pm] & PCI_PM_STATE_MASK) == PCI_D3HOT;
    if (d3 && !in_d3) {
        d3_at = bus->nlog;
    } else if (!d3 && in_d3) {
        d0_at = bus->nlog;
        d3_accesses += bus->nlog - d3_at;
        soft_reset();
    }
    in_d3 = d3;
}

void pci_write_config32(uint8_t b, uint8_t d, uint8_t f, uint8_t offset, uint32_t value)
{
    unsigned k;

    if (is_chip(b, d, f)) {
        for (k = 0; k < 4; k++) {
            bus->config[offset + k] = (uint8_t)(value >> (8 * k));
        }
        pmcsr_written(offset);
    }
}

void pci_write_config16(uint8_t b, uint8_t d, uint8_t f, uint8_t offset, uint16_t value)
{
    if (is_chip(b, d, f)) {
        bus->config[offset] = (uint8_t)value;
        bus->config[offset + 1] = (uint8_t)(value >> 8);
        pmcsr_written(offset);
    }
}

void pci_write_config8(uint8_t b, uint8_t d, uint8_t f, uint8_t offset, uint8_t value)
{
    if (is_chip(b, d, f)) {
        bus->config[offset] = value;
        pmcsr_written(offset);
    }
}

/* ---- what every bind already runs: records of the calls ---- */

static int vdd_calls, vdd_err, hpd_suspends, hpd_resumes, fence_restores, invalidates;
static int gt_gating, display_gating, irq_syncs;

int32_t intel_pps_vdd_suspend(struct drm_i915_private *i915)
{
    (void)i915;
    vdd_calls++;
    return vdd_err;
}

void intel_hotplug_suspend(struct drm_i915_private *i915)
{
    (void)i915;
    hpd_suspends++;
}

void intel_hotplug_resume(struct drm_i915_private *i915)
{
    (void)i915;
    hpd_resumes++;
}

void i915_gem_restore_fences(struct drm_i915_private *i915)
{
    (void)i915;
    fence_restores++;
}

void i915_vma_revoke_mmap(struct i915_vma *vma)
{
    (void)vma;
}

void i915_vma_flush_writes(struct i915_vma *vma)
{
    (void)vma;
}

void i915_ggtt_invalidate(struct drm_i915_private *i915)
{
    (void)i915;
    invalidates++;
}

void i915_gt_init_clock_gating(struct drm_i915_private *i915)
{
    (void)i915;
    gt_gating++;
}

void intel_display_clock_gating(struct drm_i915_private *i915)
{
    (void)i915;
    display_gating++;
}

void i915_irq_sync(struct drm_i915_private *i915)
{
    (void)i915;
    irq_syncs++;
}

/* ---- the DRM core's worker: the one work item, run by the rows ---- */

static struct drm_work *queued;
static uint32_t queued_ms;
static int queues;

void drm_dev_work_later_ms(struct drm_device *dev, struct drm_work *work, uint32_t ms)
{
    work->dev = dev;
    queued = work;
    queued_ms = ms;
    queues++;
}

int drm_dev_work_cancel(struct drm_work *work)
{
    int was = queued == work;

    queued = 0;
    return was;
}

/* ---- /sys: the PCI function's directory and the files added ---- */

#define TEST_ANCHOR 1
#define TEST_FILES  16

static struct {
    int parent;
    char name[SYSFS_NAME_MAX];
    const struct sysfs_attr *attr;
    void *obj;
    int arg;
    int gone;
} files[TEST_FILES];
static int nfiles, anchor_key;

int sysfs_anchor(int kind, int key)
{
    if (kind != SYSFS_PCI_DEVICE) {
        return -ENOENT;
    }
    anchor_key = key;
    return TEST_ANCHOR;
}

/** Add an entry below `parent`: its handle, which is its index plus 2. */
static int add_file(int parent, const char *name, const struct sysfs_attr *attr, void *obj, int arg)
{
    if (nfiles == TEST_FILES) {
        return -ENOSPC;
    }
    files[nfiles].parent = parent;
    k_strncpy(files[nfiles].name, name, SYSFS_NAME_MAX);
    files[nfiles].attr = attr;
    files[nfiles].obj = obj;
    files[nfiles].arg = arg;
    files[nfiles].gone = 0;
    return 2 + nfiles++;
}

int sysfs_add_dir(int parent, const char *name)
{
    return add_file(parent, name, 0, 0, 0);
}

int sysfs_add_attr(int parent, const struct sysfs_attr *attr, void *obj, int arg)
{
    return add_file(parent, attr->name, attr, obj, arg);
}

/** Remove the entry `h` and everything below it. */
void sysfs_remove(int h)
{
    int i;

    if (h < 2 || h - 2 >= nfiles) {
        return;
    }
    files[h - 2].gone = 1;
    for (i = 0; i < nfiles; i++) {
        if (files[i].parent == h) {
            files[i].gone = 1;
        }
    }
}

/** The handle of power/`name` below the anchor, or 0. */
static int find_file(const char *name)
{
    int i, dir = 0;

    for (i = 0; i < nfiles; i++) {
        if (!files[i].gone && files[i].parent == TEST_ANCHOR && !strcmp(files[i].name, "power")) {
            dir = 2 + i;
        }
    }
    for (i = 0; dir && i < nfiles; i++) {
        if (!files[i].gone && files[i].parent == dir && !strcmp(files[i].name, name)) {
            return 2 + i;
        }
    }
    return 0;
}

static char shown[64];

/** Read power/`name` into `shown`, without its newline: whether it
 * answered. */
static int show(const char *name)
{
    int h = find_file(name), n;

    shown[0] = 0;
    if (h < 2 || !files[h - 2].attr || !files[h - 2].attr->show) {
        return 0;
    }
    n = files[h - 2].attr->show(files[h - 2].obj, files[h - 2].arg, shown, sizeof(shown) - 1);
    if (n < 1 || n >= (int)sizeof(shown)) {
        return 0;
    }
    shown[n - 1] = 0;
    return 1;
}

/** Whether power/`name` reads `text`. */
static int reads(const char *name, const char *text)
{
    return show(name) && !strcmp(shown, text);
}

/** Write `text` to power/`name`: its `store`'s answer, or -EACCES. */
static int32_t store(const char *name, const char *text)
{
    int h = find_file(name);

    if (h < 2 || !files[h - 2].attr || !files[h - 2].attr->store) {
        return -EACCES;
    }
    return files[h - 2].attr->store(files[h - 2].obj, files[h - 2].arg, text,
                                    (uint32_t)k_strlen(text));
}

/* ---- the chip's clocks: LCPLL, the sideband and D_COMP ---- */

/* The clock unit's registers that the sideband reaches, as the Lenovo's
 * firmware leaves them: CLKOUT_DP's buffer on, its SSC path enabled on the
 * main path. */
static uint32_t sbi_gen0, sbi_sscctl, sbi_other;
static int sbi_cmds;

/* LCPLL: whether it is disabled as last seen, and since when. */
static int lcpll_off;
static uint64_t lcpll_change_us;

/** The clock unit's register `reg`, or a scratch word for any other. */
static uint32_t *sbi_reg(uint32_t reg)
{
    return reg == SBI_GEN0 ? &sbi_gen0 : reg == SBI_SSCCTL ? &sbi_sscctl : &sbi_other;
}

/** LCPLL_CTL as the chip reads it: the lock 200 us after the PLL is
 * disabled lost, and 500 us after it is enabled won back; the CD clock's
 * switch done at once. */
static uint32_t lcpll_read(struct i915_model *m)
{
    uint32_t v = i915_model_peek(m, LCPLL_CTL);
    int off = (v & LCPLL_PLL_DISABLE) != 0;

    if (off != lcpll_off) {
        lcpll_off = off;
        lcpll_change_us = m->now_us;
    }
    v &= ~(LCPLL_PLL_LOCK | LCPLL_CD_SOURCE_FCLK_DONE);
    if (off ? m->now_us - lcpll_change_us < 200u : m->now_us - lcpll_change_us >= 500u) {
        v |= LCPLL_PLL_LOCK;
    }
    if (v & LCPLL_CD_SOURCE_FCLK) {
        v |= LCPLL_CD_SOURCE_FCLK_DONE;
    }
    i915_model_poke(m, LCPLL_CTL, v);
    return v;
}

/** SBI_CTL_STAT as the chip reads it: a command posted with the busy bit
 * is carried out and answered, its data in SBI_DATA. */
static uint32_t sbi_read(struct i915_model *m, uint32_t v)
{
    uint32_t *r;

    if (!(v & SBI_BUSY)) {
        return v;
    }
    sbi_cmds++;
    r = sbi_reg(i915_model_peek(m, SBI_ADDR) >> 16);
    if ((v & (0xfu << 8)) == (SBI_CTL_OP_CRRD | SBI_CTL_OP_WRITE)) {
        *r = i915_model_peek(m, SBI_DATA);
    } else {
        i915_model_poke(m, SBI_DATA, *r);
    }
    v &= ~(SBI_BUSY | SBI_RESPONSE_FAIL);
    i915_model_poke(m, SBI_CTL_STAT, v);
    return v;
}

/** The test's read hook: the clocks' behaviour, and BAR 0 in D3hot. */
static uint32_t on_read(struct i915_model *m, uint32_t reg, uint32_t value)
{
    if (in_d3) {
        return 0xffffffffu;
    }
    if (reg == LCPLL_CTL) {
        return lcpll_read(m);
    }
    if (reg == SBI_CTL_STAT) {
        return sbi_read(m, value);
    }
    return value;
}

/* ---- the device ---- */

/* The id table: the Lenovo's chip alone. */
static const struct i915_platform hsw = {"HASWELL", 7, 50, 7, 2, 0xf, 1, 1, 1, 0, 1, 31, 40};
static const struct i915_id lenovo_id = {0x0A16, 1, 0, &hsw};

static struct drm_i915_private dev;
static uint32_t ier_before[I915_IRQ_BANKS], imr_before[I915_IRQ_BANKS];

/** The banks' enable and mask registers as the chip holds them. */
static void read_banks(struct i915_model *m, uint32_t *ier, uint32_t *imr)
{
    int b;

    for (b = 0; b < I915_IRQ_BANKS; b++) {
        ier[b] = i915_model_peek(m, bank_ier[b]);
        imr[b] = i915_model_peek(m, bank_imr[b]);
    }
}

/** A handler for the rows' interrupt sources. */
static void ignore_irq(struct drm_i915_private *i915, uint32_t iir, void *data)
{
    (void)i915;
    (void)iir;
    (void)data;
}

/** Let `ms` milliseconds pass on the tick and the model's clock, which
 * runs its blocks, and run forcewake's release timer. */
static void pass_ms(struct i915_model *m, uint32_t ms)
{
    uint32_t i;

    for (i = 0; i < ms; i++) {
        i915_delay_us(&m->hw, 1000);
        kernel_ticks++;
        i915_uncore_tick(&dev);
    }
}

/**
 * A fresh model and a device record over it at the end of a bind with the
 * display off: forcewake up, the interrupts installed with the rings' user
 * interrupts and a display source enabled, the display well's references
 * put and its BIOS request gone, the panel turned off through PP_CONTROL
 * and its sequence run, the backlight and PLLs off, LCPLL
 * locked at 450 MHz, then runtime PM set up and enabled, which queues the
 * autosuspend. /sys empty.
 */
static struct drm_i915_private *fresh(struct i915_model *m)
{
    CHECK("the model is made", i915_model_init(m) == 0);
    bus = m;
    m->on_read = on_read;
    k_memset(&dev, 0, sizeof(dev));
    dev.pci = m->pci;
    dev.hw = m->hw;
    dev.hw.gtt_entries = (I915_MODEL_BAR0 - I915_GSM_OFFSET) / 4u;
    dev.id = &lenovo_id;
    dev.msi_vector = -1;
    dev.display.ready = 1;
    dev.drm.driver_priv = &dev;
    log_text[0] = 0;
    nfiles = 0;
    queued = 0;
    queues = 0;
    udelay_us = 0;
    in_d3 = 0;
    d3_at = d0_at = d3_accesses = 0;
    vdd_calls = vdd_err = hpd_suspends = hpd_resumes = fence_restores = invalidates = 0;
    gt_gating = display_gating = irq_syncs = 0;
    sbi_gen0 = SBI_GEN0_CFG_BUFFENABLE_DISABLE;
    sbi_sscctl = 0;
    sbi_other = 0;
    sbi_cmds = 0;
    lcpll_off = 0;
    lcpll_change_us = 0;
    kernel_ticks = 1000;
    i915_model_poke(m, LCPLL_CTL, LCPLL_PLL_LOCK | LCPLL_CLK_FREQ_450);
    i915_model_poke(m, BLC_PWM_CPU_CTL2, 0);
    i915_model_poke(m, BLC_PWM_PCH_CTL1, 0);
    i915_model_poke(m, HSW_PWR_WELL_CTL1, 0);

    i915_uncore_init(&dev);
    i915_irq_reset(&dev);
    i915_irq_install(&dev);
    i915_irq_register(&dev, I915_IRQ_GT, GT_RENDER_USER_INTERRUPT, ignore_irq, 0);
    i915_irq_unmask(&dev, I915_IRQ_GT, GT_RENDER_USER_INTERRUPT);
    i915_irq_register(&dev, I915_IRQ_DE, DE_PIPE_VBLANK_IVB(0), ignore_irq, 0);
    i915_irq_unmask(&dev, I915_IRQ_DE, DE_PIPE_VBLANK_IVB(0));
    CHECK("bind: the display well comes up", i915_power_init(&dev) == 0);
    i915_power_put(&dev);
    i915_write(&m->hw, PP_CONTROL, 0);
    pass_ms(m, 2000);
    i915_rpm_init(&dev);
    dev.runtime_pm.available = 1;
    i915_rpm_enable(&dev);
    return &dev;
}

/** Take the model off the bus. */
static void done(struct i915_model *m)
{
    i915_model_free(m);
    bus = 0;
}

/** Run the queued work, as the DRM core's worker would. */
static void run_work(void)
{
    struct drm_work *w = queued;

    queued = 0;
    if (w) {
        w->fn(w);
    }
}

/** The log index of the first write of `value` to `reg` from `from`, or a
 * large number. */
static long at(const struct i915_model *m, uint32_t reg, uint32_t value, uint32_t from)
{
    long i = i915_model_find_write(m, reg, value, from);
    return i < 0 ? 1L << 30 : i;
}

/** The index of the first write to `reg` from `from` whose value has every
 * bit of `bits`, or a large number. */
static long at_bits(const struct i915_model *m, uint32_t reg, uint32_t bits, uint32_t from)
{
    uint32_t i;

    for (i = from; i < m->nlog; i++) {
        if (m->log[i].op == I915_MODEL_WRITE && m->log[i].reg == reg &&
            (m->log[i].value & bits) == bits) {
            return (long)i;
        }
    }
    return 1L << 30;
}

/** The index of the first write to `reg` from `from` with none of the
 * bits `bits`, or a large number. */
static long at_clear(const struct i915_model *m, uint32_t reg, uint32_t bits, uint32_t from)
{
    uint32_t i;

    for (i = from; i < m->nlog; i++) {
        if (m->log[i].op == I915_MODEL_WRITE && m->log[i].reg == reg && !(m->log[i].value & bits)) {
            return (long)i;
        }
    }
    return 1L << 30;
}

/** The function's power state field. */
static int pci_state(struct i915_model *m)
{
    return (int)(i915_model_config(m, LENOVO_GPU_CAP_PM + PCI_PM_CTRL, 2) & PCI_PM_STATE_MASK);
}

/** Let the GT's forcewake go and the delay pass, then run the work: the
 * suspend. */
static void suspend(struct i915_model *m)
{
    pass_ms(m, 10000);
    run_work();
}

/* ---- the rows ---- */

/** Runtime PM at the bind's end: Linux's defaults, the files, and the
 * autosuspend queued for the delay. */
static void row_enable(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m);
    const pci_device_t *p = &i915->pci;

    CHECK("enable: the bind's wakeref put, enabled, active",
          i915->runtime_pm.usage == 0 && i915->runtime_pm.enabled &&
              i915->runtime_pm.status == I915_RPM_ACTIVE);
    CHECK("enable: the autosuspend queued for 10 s", queued && queued_ms == 10000);
    CHECK("enable: the power directory of the function",
          anchor_key == (p->bus << 8 | p->device << 3 | p->function));
    CHECK("enable: control auto", reads("control", "auto"));
    CHECK("enable: autosuspend_delay_ms 10000", reads("autosuspend_delay_ms", "10000"));
    CHECK("enable: runtime_status active", reads("runtime_status", "active"));
    CHECK("enable: runtime_enabled enabled", reads("runtime_enabled", "enabled"));
    CHECK("enable: runtime_usage 0", reads("runtime_usage", "0"));
    CHECK("enable: the function in D0", pci_state(&m) == PCI_D0);
    done(&m);
}

/** The work before the delay has passed: queued again for the rest, the
 * chip untouched. */
static void row_early(void)
{
    struct i915_model m;

    fresh(&m);
    pass_ms(&m, 4000);
    m.nlog = 0;
    run_work();
    CHECK("early: still active", dev.runtime_pm.status == I915_RPM_ACTIVE);
    CHECK("early: queued again for the 6 s left", queued && queued_ms == 6000);
    CHECK("early: no access to the chip", m.nlog == 0);
    done(&m);
}

/** The suspend, after the delay, in Linux's order. */
static void row_suspend(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m);
    uint32_t ier[I915_IRQ_BANKS], imr[I915_IRQ_BANKS], cmd, lc;
    long fclk, off, pda, dcomp, last_irq;
    int b, banks_off = 1;

    read_banks(&m, ier_before, imr_before);
    cmd = i915_model_config(&m, PCI_COMMAND, 2);
    pass_ms(&m, 10000);
    m.nlog = 0;
    run_work();
    read_banks(&m, ier, imr);
    for (b = 0; b < I915_IRQ_BANKS; b++) {
        banks_off &= ier[b] == 0 && imr[b] == 0xffffffffu;
    }
    lc = i915_model_peek(&m, LCPLL_CTL);
    fclk = at_bits(&m, LCPLL_CTL, LCPLL_CD_SOURCE_FCLK, 0);
    off = at_bits(&m, LCPLL_CTL, LCPLL_PLL_DISABLE, 0);
    pda = at_bits(&m, LCPLL_CTL, LCPLL_POWER_DOWN_ALLOW, 0);
    dcomp = at(&m, GEN6_PCODE_MAILBOX, GEN6_PCODE_READY | GEN6_PCODE_WRITE_D_COMP, 0);
    last_irq = at(&m, SERR_INT, 0xffffffffu, 0);

    CHECK("suspend: suspended, once", i915->runtime_pm.status == I915_RPM_SUSPENDED &&
                                          i915->runtime_pm.suspends == 1 &&
                                          reads("runtime_status", "suspended"));
    CHECK("suspend: no wait reached its bound", i915->hw.timeouts == 0);
    CHECK("suspend: the panel's VDD and hotplug told", vdd_calls == 1 && hpd_suspends == 1);
    CHECK("suspend: the display well's requests gone",
          i915_model_peek(&m, HSW_PWR_WELL_CTL1) == 0 &&
              i915_model_peek(&m, HSW_PWR_WELL_CTL2) == 0 && !i915->power.on);
    CHECK("suspend: every bank masked and disabled, the master enable off", banks_off);
    CHECK("suspend: a handler running elsewhere waited for", irq_syncs == 1);
    CHECK("suspend: the copies of the masks kept",
          (i915->irq.ier[I915_IRQ_GT] & GT_RENDER_USER_INTERRUPT) &&
              (i915->irq.ier[I915_IRQ_DE] & DE_PIPE_VBLANK_IVB(0)) && i915->irq.suspended);
    CHECK("suspend: forcewake released",
          i915->uncore.wake_count == 0 && (i915_model_peek(&m, FORCEWAKE_ACK_HSW) & 1u) == 0);
    CHECK("suspend: CLKOUT_DP's buffer off through the sideband",
          sbi_gen0 == 0 && (sbi_sscctl & (SBI_SSCCTL_PATHALT | SBI_SSCCTL_DISABLE)) ==
                               (SBI_SSCCTL_PATHALT | SBI_SSCCTL_DISABLE));
    CHECK("suspend: the interrupts off before LCPLL", last_irq < fclk);
    CHECK("suspend: the CD clock to FCLK, then LCPLL off, D_COMP off, power-down allowed",
          fclk < off && off < dcomp && dcomp < pda && pda < (1L << 30));
    CHECK("suspend: D_COMP disabled through the pcode",
          i915_model_peek(&m, D_COMP_HSW) & D_COMP_COMP_DISABLE);
    CHECK("suspend: LCPLL unlocked, disabled, on FCLK, power-down allowed",
          (lc &
           (LCPLL_PLL_LOCK | LCPLL_PLL_DISABLE | LCPLL_CD_SOURCE_FCLK | LCPLL_POWER_DOWN_ALLOW)) ==
              (LCPLL_PLL_DISABLE | LCPLL_CD_SOURCE_FCLK | LCPLL_POWER_DOWN_ALLOW));
    CHECK("suspend: nothing left on that PC8 needs off", !logged("pc8:"));
    CHECK("suspend: the function in D3hot, its header saved",
          pci_state(&m) == PCI_D3HOT && in_d3 &&
              (i915->runtime_pm.saved.header[1] & 0xffffu) == cmd);
    CHECK("suspend: every access to BAR 0 before D3hot", d3_at == m.nlog);
    CHECK("suspend: D3hot's 10 ms waited", udelay_us >= 10000u);
    CHECK("suspend: no access while asleep", i915->hw.asleep && i915->hw.asleep_accesses == 0);
    done(&m);
}

/** The registers while the device sleeps: all ones, the chip untouched,
 * the access logged once. */
static void row_asleep(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m);
    uint32_t n;

    suspend(&m);
    n = m.nlog;
    CHECK("asleep: a read gives all ones", i915_read(&i915->hw, GEN6_RPNSWREQ) == 0xffffffffu);
    i915_write(&i915->hw, GEN6_RPNSWREQ, 1);
    CHECK("asleep: neither reaches the chip", m.nlog == n && d3_accesses == 0);
    CHECK("asleep: both counted and logged",
          i915->hw.asleep_accesses == 2 && logged("rpm: read of 0x"));
    CHECK("asleep: get_if_active refuses", !i915_rpm_get_if_active(i915) &&
                                               i915->runtime_pm.usage == 0 &&
                                               i915->runtime_pm.status == I915_RPM_SUSPENDED);
    done(&m);
}

/** The resume on a get: D0 before any access, then every part of the
 * suspend undone. */
static void row_resume(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m);
    uint32_t ier[I915_IRQ_BANKS], imr[I915_IRQ_BANKS], cmd, bar0, bar2, lc;
    long fw_on, pda_off, dcomp, enable;
    int b, banks_same = 1;

    i915->msi_vector = 48;
    read_banks(&m, ier_before, imr_before);
    cmd = i915_model_config(&m, PCI_COMMAND, 2);
    bar0 = i915_model_config(&m, PCI_BAR0, 4);
    bar2 = i915_model_config(&m, PCI_BAR0 + 8u, 4);
    suspend(&m);
    m.nlog = 0;
    d3_at = 0;
    i915->hw.timeouts = 0;
    i915_rpm_get(i915);
    read_banks(&m, ier, imr);
    for (b = 0; b < I915_IRQ_BANKS; b++) {
        banks_same &= ier[b] == ier_before[b] && imr[b] == imr_before[b];
    }
    lc = i915_model_peek(&m, LCPLL_CTL);
    fw_on = at(&m, FORCEWAKE_MT, 0x00010001u, 0);
    pda_off = at_clear(&m, LCPLL_CTL, LCPLL_POWER_DOWN_ALLOW, 0);
    dcomp = at(&m, GEN6_PCODE_MAILBOX, GEN6_PCODE_READY | GEN6_PCODE_WRITE_D_COMP, 0);
    enable = at_clear(&m, LCPLL_CTL, LCPLL_PLL_DISABLE, 0);

    CHECK("resume: active, once, with the wakeref",
          i915->runtime_pm.status == I915_RPM_ACTIVE && i915->runtime_pm.resumes == 1 &&
              i915->runtime_pm.usage == 1 && reads("runtime_status", "active"));
    CHECK("resume: no wait reached its bound", i915->hw.timeouts == 0);
    CHECK("resume: D0 before any access to BAR 0",
          pci_state(&m) == PCI_D0 && !in_d3 && d0_at == 0 && d3_accesses == 0);
    CHECK("resume: the command word and BARs restored after the soft reset",
          i915_model_config(&m, PCI_COMMAND, 2) == cmd &&
              i915_model_config(&m, PCI_BAR0, 4) == bar0 &&
              i915_model_config(&m, PCI_BAR0 + 8u, 4) == bar2);
    CHECK("resume: the message enabled again",
          (i915_model_config(&m, LENOVO_GPU_CAP_MSI + 2u, 2) & 1u) &&
              (i915_model_config(&m, LENOVO_GPU_CAP_MSI + 8u, 2) & 0xffu) == 48);
    CHECK("resume: no longer asleep", !i915->hw.asleep);
    CHECK("resume: forcewake held, then power-down disallowed, D_COMP on, LCPLL enabled",
          fw_on < pda_off && pda_off < dcomp && dcomp < enable && enable < (1L << 30));
    CHECK("resume: D_COMP forced on through the pcode",
          (i915_model_peek(&m, D_COMP_HSW) & (D_COMP_COMP_FORCE | D_COMP_COMP_DISABLE)) ==
              D_COMP_COMP_FORCE);
    CHECK("resume: LCPLL locked, the CD clock back on it",
          (lc & (LCPLL_PLL_LOCK | LCPLL_PLL_DISABLE | LCPLL_CD_SOURCE_FCLK |
                 LCPLL_POWER_DOWN_ALLOW | LCPLL_CD_SOURCE_FCLK_DONE)) == LCPLL_PLL_LOCK);
    CHECK("resume: the clock gating written again", gt_gating == 1 && display_gating == 1);
    CHECK("resume: every bank's enable and mask as before the suspend",
          banks_same && !i915->irq.suspended);
    CHECK("resume: the fences restored and hotplug back", fence_restores == 1 && hpd_resumes == 1);
    CHECK("resume: nothing logged amiss",
          !logged("pc8:") && !logged("rpm: the function") && !logged("rpm: read"));
    i915_rpm_put(i915);
    CHECK("resume: the last put queues the autosuspend again",
          i915->runtime_pm.usage == 0 && queued && queued_ms == 10000);
    suspend(&m);
    CHECK("resume: and the device suspends again",
          i915->runtime_pm.status == I915_RPM_SUSPENDED && i915->runtime_pm.suspends == 2);
    done(&m);
}

/** GTT clears while the device sleeps: noted, then written at the resume
 * with the GTT flushed. */
static void row_clears(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m);
    uint32_t i, n = 0;

    i915->ggtt.scratch_pte = 0x12345001u;
    suspend(&m);
    CHECK("clears: refused while asleep, noted",
          !i915_rpm_get_or_defer_clear(i915, 0x10000, 3 * I915_GTT_PAGE_SIZE) &&
              i915->runtime_pm.nclears == 1 && i915->runtime_pm.usage == 0);
    m.nlog = 0;
    i915_rpm_get(i915);
    for (i = 0; i < m.nlog; i++) {
        n += m.log[i].op == I915_MODEL_WRITE && m.log[i].value == 0x12345001u;
    }
    CHECK("clears: the three entries pointed at scratch, the GTT flushed",
          n == 3 && invalidates == 1 && i915->runtime_pm.nclears == 0);
    CHECK("clears: taken at once while awake",
          i915_rpm_get_or_defer_clear(i915, 0, I915_GTT_PAGE_SIZE) && i915->runtime_pm.usage == 2);
    done(&m);
}

/** A suspend refused, by the GT awake and by forcewake held: the device
 * as it was, and the work again after a second. */
static void row_refused(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m);
    uint32_t ier[I915_IRQ_BANKS], imr[I915_IRQ_BANKS];
    int b, same = 1;

    i915->gt.awake = 1;
    pass_ms(&m, 10000);
    m.nlog = 0;
    run_work();
    CHECK("refused: the GT awake keeps the device active, the chip untouched",
          i915->runtime_pm.status == I915_RPM_ACTIVE && i915->runtime_pm.failures == 1 &&
              m.nlog == 0 && queued && queued_ms == I915_RPM_RETRY_MS);
    i915->gt.awake = 0;

    read_banks(&m, ier_before, imr_before);
    CHECK("refused: forcewake taken", i915_forcewake_get(i915) == 0);
    pass_ms(&m, 1000);
    run_work();
    read_banks(&m, ier, imr);
    for (b = 0; b < I915_IRQ_BANKS; b++) {
        same &= ier[b] == ier_before[b] && imr[b] == imr_before[b];
    }
    CHECK("refused: forcewake held keeps the device active",
          i915->runtime_pm.status == I915_RPM_ACTIVE && i915->runtime_pm.failures == 2 && queued &&
              queued_ms == I915_RPM_RETRY_MS);
    CHECK("refused: the interrupts back as they were", same && !i915->irq.suspended);
    CHECK("refused: neither PC8 nor D3hot", pci_state(&m) == PCI_D0 &&
                                                (i915_model_peek(&m, LCPLL_CTL) & LCPLL_PLL_LOCK) &&
                                                !i915->hw.asleep);
    i915_forcewake_put(i915);
    pass_ms(&m, 1000);
    run_work();
    CHECK("refused: suspended once forcewake is let go",
          i915->runtime_pm.status == I915_RPM_SUSPENDED);
    done(&m);
}

/** control "on" and a negative delay each hold the device; "auto" and a
 * delay again let it go. */
static void row_control(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m);

    suspend(&m);
    CHECK("control: \"on\" taken", store("control", "on\n") == 3);
    CHECK("control: \"on\" resumes and holds",
          i915->runtime_pm.status == I915_RPM_ACTIVE && i915->runtime_pm.usage == 1 &&
              reads("control", "on") && reads("runtime_enabled", "forbidden"));
    CHECK("control: another word refused", store("control", "off") == -EINVAL);
    store("autosuspend_delay_ms", "-1");
    CHECK("control: a negative delay holds a second wakeref",
          i915->runtime_pm.usage == 2 && reads("autosuspend_delay_ms", "-1"));
    store("control", "auto");
    queued = 0;
    store("autosuspend_delay_ms", "0");
    CHECK("control: both let go, the work queued at once",
          i915->runtime_pm.usage == 0 && queued && queued_ms == 0);
    pass_ms(&m, 1);
    run_work();
    CHECK("control: suspended with no delay", i915->runtime_pm.status == I915_RPM_SUSPENDED);
    CHECK("control: a delay that is no number refused",
          store("autosuspend_delay_ms", "soon") == -EINVAL);
    done(&m);
}

/** The unplug: the device resumed and held for good, the work cancelled,
 * the files gone. */
static void row_disable(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m);

    suspend(&m);
    queued = &i915->runtime_pm.work;
    i915_rpm_disable(i915);
    CHECK("disable: resumed and held", i915->runtime_pm.status == I915_RPM_ACTIVE &&
                                           i915->runtime_pm.usage == 1 && pci_state(&m) == PCI_D0);
    CHECK("disable: not enabled, the work cancelled, the files gone",
          !i915->runtime_pm.enabled && !queued && find_file("control") == 0);
    i915_rpm_put(i915);
    CHECK("disable: a put queues nothing", !queued);
    done(&m);
}

int main(void)
{
    row_enable();
    row_early();
    row_suspend();
    row_asleep();
    row_resume();
    row_clears();
    row_refused();
    row_control();
    row_disable();
    if (fails) {
        printf("i915-rpm-test: %d failed\n", fails);
        return 1;
    }
    printf("i915-rpm-test: ok\n");
    return 0;
}
