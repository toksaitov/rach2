/**
 * i915_power_hosttest.c: the Intel graphics driver's RC6 and frequency
 * control, i915_rc6.c and i915_rps.c, against the host model of the
 * Lenovo's HD 4400, whose RC6 counts residency while its GT is idle, whose
 * pcode answers its mailbox, and whose interrupt registers raise the up
 * and down interrupts.
 *
 * host-deps: rach2/sys/drm/i915/i915_rc6.c rach2/sys/drm/i915/i915_rps.c
 * host-deps: rach2/sys/drm/i915/i915_irq.c rach2/sys/drm/i915/i915_uncore.c
 * host-deps: rach2/sys/drm/i915/i915_hw.c tests/host/i915_model.c
 * host-deps: rach2/sys/tick.c rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/spinlock_host.c
 *
 * A row makes a fresh model and a device record over it, brought up as
 * the bind does: forcewake, the interrupts reset and installed, RC6 and
 * RPS read and sanitized, then RPS, the ring's frequency table and RC6
 * enabled and the GT parked. The kernel around the driver is a set of
 * records: the processor takes the device's messages by calling the
 * driver's handler, which runs the RPS work after it, and /sys is a table
 * of the files added, which a row reads and writes through their own
 * calls.
 *
 * The rows: RC6 sanitized and enabled with Linux's thresholds; RC6 entered
 * at once when the GT parks and after its threshold under the hardware's
 * timers, not while the GT is busy or forcewake is held, and its residency
 * in milliseconds across the counter's wrap; the pcode mailbox, slow,
 * stuck, busy and failing; RPS's limits from the Lenovo's chip, with and
 * without the pcode's answers; RPS enabled, its thresholds and the ring's
 * frequency table; the up and down interrupts stepping the frequency, the
 * park and unpark, and every request inside the softlimits; the
 * softlimits set and refused; the /sys files by Linux's names, read and
 * written; and RPS and RC6 turned off.
 *
 * Returns 0 when every row passes, 1 otherwise.
 */

#include <stdarg.h>

#include "drm/i915/i915.h"
#include "drm/i915/i915_reg.h"
#include "fs/sysfs.h"
#include "lib/kerrno.h"
#include "lib/kirq.h"
#include "mm/paging.h"
#include "udelay.h"
#include "i915_model.h"
#include "i915_lenovo.h"
#include "i915_model_rc6.h"
#include "i915_model_ring.h"

extern int printf(const char *format, ...);
extern int vsnprintf(char *s, unsigned long n, const char *format, va_list ap);
extern char *strstr(const char *s, const char *t);
extern int strcmp(const char *a, const char *b);
extern int memcmp(const void *a, const void *b, unsigned long n);

static int fails;

#define CHECK(what, cond)                                \
    do {                                                 \
        if (!(cond)) {                                   \
            printf("i915-power-test: fail: %s\n", what); \
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

/** Configuration space, which i915_hw.c's words read and no row reaches:
 * all ones, as an absent function answers. */
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

/* ---- /sys: a table of the files added ---- */

#define TEST_CARD  1
#define TEST_FILES 16

static struct {
    int parent;
    char name[SYSFS_NAME_MAX];
    const struct sysfs_attr *attr;
    void *obj;
    int arg;
} files[TEST_FILES];
static int nfiles, card_dir, power_exists;

int drm_sysfs_card_dir(const struct drm_device *dev)
{
    (void)dev;
    return card_dir;
}

/** Add an entry below `parent`: its handle, which is its index plus 2. */
static int add_file(int parent, const char *name, const struct sysfs_attr *attr, void *obj, int arg)
{
    int i;

    for (i = 0; i < nfiles; i++) {
        if (files[i].parent == parent && strcmp(files[i].name, name) == 0) {
            return -EEXIST;
        }
    }
    if (nfiles == TEST_FILES) {
        return -ENOSPC;
    }
    files[nfiles].parent = parent;
    k_strncpy(files[nfiles].name, name, SYSFS_NAME_MAX);
    files[nfiles].attr = attr;
    files[nfiles].obj = obj;
    files[nfiles].arg = arg;
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

/* The function shown bound by sysfs_pci_bind: its key and driver, or -1
 * and NULL. */
static int bound_key = -1;
static const char *bound_driver;

int sysfs_pci_bind(int key, const char *driver)
{
    bound_key = key;
    bound_driver = driver;
    return 0;
}

void sysfs_pci_unbind(int key)
{
    if (key == bound_key) {
        bound_key = -1;
        bound_driver = 0;
    }
}

int sysfs_node_child(int dir, const char *name, int len)
{
    int i;

    for (i = 0; i < nfiles; i++) {
        if (files[i].parent == dir && (int)k_strlen(files[i].name) == len &&
            memcmp(files[i].name, name, (unsigned long)len) == 0) {
            return 2 + i;
        }
    }
    return 0;
}

/** The file `name` below the card, or below its power directory for a
 * name that starts "power/", or NULL. */
static int find_file(const char *name)
{
    int parent = TEST_CARD;

    if (strstr(name, "power/") == name) {
        parent = sysfs_node_child(TEST_CARD, "power", 5);
        name += 6;
    }
    return sysfs_node_child(parent, name, (int)k_strlen(name));
}

static char shown[64];

/** Read the file `name` into `shown`: whether it has a `show` that
 * answered. */
static int show(const char *name)
{
    int h = find_file(name), n;

    shown[0] = 0;
    if (h < 2 || !files[h - 2].attr || !files[h - 2].attr->show) {
        return 0;
    }
    n = files[h - 2].attr->show(files[h - 2].obj, files[h - 2].arg, shown, sizeof(shown) - 1);
    if (n < 0 || n >= (int)sizeof(shown)) {
        return 0;
    }
    shown[n] = 0;
    return 1;
}

/** Write `text` to the file `name`: its `store`'s answer, or -EACCES. */
static int32_t store(const char *name, const char *text)
{
    int h = find_file(name);

    if (h < 2 || !files[h - 2].attr || !files[h - 2].attr->store) {
        return -EACCES;
    }
    return files[h - 2].attr->store(files[h - 2].obj, files[h - 2].arg, text,
                                    (uint32_t)k_strlen(text));
}

/* The id table: the Lenovo's chip alone. */
static const struct i915_platform hsw = {"HASWELL", 7, 50, 7, 2, 0xf, 1, 1, 1, 0, 1, 31, 40};
static const struct i915_id lenovo_id = {0x0A16, 1, 0, &hsw};

/* ---- the device ---- */

static struct drm_i915_private dev;
static struct i915_model *bus;
static int irq_on;

/** Take every message the model has pending, as the processor calls the
 * handler. */
static void take_messages(struct i915_model *m)
{
    while (i915_model_take_message(m)) {
        i915_irq_handle(&dev);
    }
}

/** The model's clock moved: the processor takes its messages while its
 * interrupts are on. */
static void on_delay(struct i915_model *m)
{
    if (irq_on) {
        take_messages(m);
    }
}

/**
 * A fresh model and a device record over it, with forcewake up and the
 * interrupts reset and installed; with `bring_up`, RC6 and RPS as the bind
 * brings them up, the GT left parked. /sys empty, its card at handle 1.
 */
static struct drm_i915_private *fresh(struct i915_model *m, int bring_up)
{
    CHECK("the model is made", i915_model_init(m) == 0);
    bus = m;
    m->on_delay = on_delay;
    k_memset(&dev, 0, sizeof(dev));
    dev.pci = m->pci;
    dev.hw = m->hw;
    dev.id = &lenovo_id;
    dev.msi_vector = 48;
    log_text[0] = 0;
    irq_on = 0;
    nfiles = 0;
    card_dir = TEST_CARD;
    power_exists = 0;
    i915_uncore_init(&dev);
    i915_irq_reset(&dev);
    i915_irq_install(&dev);
    if (bring_up) {
        i915_rc6_init(&dev);
        i915_rps_init(&dev);
        i915_gt_pm_enable(&dev);
    }
    return &dev;
}

/** Take the model off the bus. */
static void done(struct i915_model *m)
{
    i915_model_free(m);
    bus = 0;
}

/** Let `ms` milliseconds pass on the model's clock. */
static void pass_ms(struct i915_model *m, uint32_t ms)
{
    uint32_t i;

    for (i = 0; i < ms; i++) {
        i915_delay_us(&m->hw, 1000);
    }
}

/** The log index of the first write of `value` to `reg`, or a large
 * number. */
static long at(const struct i915_model *m, uint32_t reg, uint32_t value)
{
    long i = i915_model_find_write(m, reg, value, 0);
    return i < 0 ? 1L << 30 : i;
}

/** The frequency the model was last asked for, in the chip's units. */
static int asked(struct i915_model *m)
{
    return (int)(i915_model_peek(m, GEN6_RPNSWREQ) >> HSW_FREQUENCY_SHIFT);
}

/** Raise the PM events `bits` and take the message they raise. */
static void pm_event(struct i915_model *m, uint32_t bits)
{
    i915_model_irq_event(m, I915_IRQ_PM, bits);
    take_messages(m);
}

/* ---- RC6 ---- */

/** RC6 sanitized at init and enabled with Linux's thresholds. */
static void row_rc6_enable(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m, 0);

    i915_model_poke(&m, GEN6_RC_CONTROL, 0x88040000u);
    i915_model_poke(&m, GEN6_RC_STATE, 0x40000u);
    m.nlog = 0;
    i915_rc6_init(i915);
    CHECK("rc6 init: supported, not enabled", i915->rc6.supported && !i915->rc6.enabled);
    CHECK("rc6 init: the firmware's RC6 turned off under forcewake",
          i915_model_peek(&m, GEN6_RC_CONTROL) == 0 && i915_model_peek(&m, GEN6_RC_STATE) == 0 &&
              at(&m, FORCEWAKE_MT, 0x00010001u) < at(&m, GEN6_RC_CONTROL, 0) &&
              i915->uncore.wake_count == 0);

    m.nlog = 0;
    i915_rc6_enable(i915);
    CHECK("rc6 enable: Linux's wake limits, interval and hysteresis",
          i915_model_peek(&m, GEN6_RC1_WAKE_RATE_LIMIT) == 1000u << 16 &&
              i915_model_peek(&m, GEN6_RC6_WAKE_RATE_LIMIT) == (40u << 16 | 30u) &&
              i915_model_peek(&m, GEN6_RC6pp_WAKE_RATE_LIMIT) == 30 &&
              i915_model_peek(&m, GEN6_RC_EVALUATION_INTERVAL) == 125000 &&
              i915_model_peek(&m, GEN6_RC_IDLE_HYSTERSIS) == 25);
    CHECK("rc6 enable: each of the four engines' idle limit",
          i915_model_count(&m, I915_MODEL_WRITE, RING_MAX_IDLE(RENDER_RING_BASE)) == 1 &&
              i915_model_peek(&m, RING_MAX_IDLE(BLT_RING_BASE)) == 10 &&
              i915_model_peek(&m, RING_MAX_IDLE(GEN6_BSD_RING_BASE)) == 10 &&
              i915_model_peek(&m, RING_MAX_IDLE(VEBOX_RING_BASE)) == 10);
    CHECK("rc6 enable: Linux's thresholds",
          i915_model_count(&m, I915_MODEL_WRITE, GEN6_RC_SLEEP) == 1 &&
              i915_model_peek(&m, GEN6_RC1e_THRESHOLD) == 1000 &&
              i915_model_peek(&m, GEN6_RC6_THRESHOLD) == 50000 &&
              i915_model_peek(&m, GEN6_RC6p_THRESHOLD) == 150000 &&
              i915_model_peek(&m, GEN6_RC6pp_THRESHOLD) == 64000);
    CHECK("rc6 enable: RC6 alone under the hardware's timers, sent at once when parked",
          i915->rc6.ctl_enable ==
                  (GEN6_RC_CTL_RC6_ENABLE | GEN6_RC_CTL_EI_MODE(1) | GEN6_RC_CTL_HW_ENABLE) &&
              i915->rc6.manual && i915->rc6.enabled);
    CHECK("rc6 enable: GEN6_RC_CONTROL left for the park",
          i915_model_count(&m, I915_MODEL_WRITE, GEN6_RC_CONTROL) == 0);
    CHECK("rc6 enable: the pcode's voltages read, under forcewake, released",
          i915_model_rc6(&m)->last_cmd == GEN6_PCODE_READ_RC6VIDS && i915->uncore.wake_count == 0 &&
              at(&m, FORCEWAKE_MT, 0x00010001u) < at(&m, GEN6_RC6_THRESHOLD, 50000));
    done(&m);

    i915 = fresh(&m, 0);
    dev.id = 0;
    i915_rc6_init(i915);
    i915_rc6_enable(i915);
    CHECK("rc6: a part without RC6 is left alone",
          !i915->rc6.supported && !i915->rc6.enabled && i915_rc6_residency_ms(i915) == 0);
    done(&m);
}

/** RC6 at idle: at once when parked, after its threshold under the
 * timers, never while busy or held awake; the residency in ms. */
static void row_rc6_idle(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m, 1);
    struct i915_model_rc6 *r = i915_model_rc6(&m);
    uint64_t before;

    CHECK("park: RC6's timers off and RC6 asked for at once",
          i915_model_peek(&m, GEN6_RC_CONTROL) == GEN6_RC_CTL_RC6_ENABLE &&
              i915_model_peek(&m, GEN6_RC_STATE) == 4u << RC_SW_TARGET_STATE_SHIFT);
    CHECK("park: no forcewake held", i915->uncore.wake_count == 0);
    CHECK("rc6: nothing yet", i915_rc6_residency_ms(i915) == 0);
    pass_ms(&m, 10000);
    CHECK("rc6: entered at idle", r->in_rc6);
    CHECK("rc6: 10 s idle reads 10000 ms, as Linux's 9960 at most",
          i915_rc6_residency_ms(i915) == 10000);

    i915_gt_unpark(i915);
    CHECK("unpark: RC6 under the hardware's timers",
          i915_model_peek(&m, GEN6_RC_CONTROL) == i915->rc6.ctl_enable);
    r->busy = 1;
    pass_ms(&m, 500);
    CHECK("rc6: not while the GT is busy", !r->in_rc6 && i915_rc6_residency_ms(i915) == 10000);
    r->busy = 0;
    pass_ms(&m, 1000);
    /* 50000 units of 1.28 us, 64 ms, before the timers take it in. */
    CHECK("rc6: under the timers, entered after the 64 ms threshold",
          r->in_rc6 && i915_rc6_residency_ms(i915) == 10936);

    i915_gt_park(i915);
    CHECK("forcewake: a reference taken", i915_forcewake_get(i915) == 0);
    before = i915_rc6_residency_ms(i915);
    pass_ms(&m, 300);
    CHECK("rc6: not while forcewake holds the GT awake",
          !r->in_rc6 && i915_rc6_residency_ms(i915) == before);
    i915_forcewake_put(i915);
    pass_ms(&m, 300);
    CHECK("rc6: again once it is released", i915_rc6_residency_ms(i915) == before + 300);
    done(&m);
}

/** The residency counter's wrap between two reads. */
static void row_rc6_wrap(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m, 1);
    struct i915_model_rc6 *r = i915_model_rc6(&m);
    uint64_t before;

    r->residency = 0xffffff00u;
    i915->rc6.prev_hw = 0xffffff00u;
    i915->rc6.cur = 0;
    before = i915_rc6_residency_ms(i915);
    pass_ms(&m, 2000);
    CHECK("wrap: the counter wrapped", r->residency < 0x10000000u);
    CHECK("wrap: the residency rose by the time idle",
          before == 0 && i915_rc6_residency_ms(i915) == 2000);
    done(&m);

    i915 = fresh(&m, 1);
    i915_model_rc6(&m)->residency = 390;
    CHECK("rounding: 390 units, 499.2 us, are 500 us rounded up, then 1 ms",
          i915_rc6_residency_ms(i915) == 1);
    done(&m);
}

/* ---- the pcode mailbox ---- */

/** The mailbox: answers, a slow command, a stuck one, a busy mailbox, and
 * each status the pcode reports. */
static void row_pcode(void)
{
    static const struct {
        uint32_t status;
        int32_t err;
    } statuses[] = {{GEN6_PCODE_ILLEGAL_CMD, -ENXIO},
                    {GEN7_PCODE_TIMEOUT, -ETIMEDOUT},
                    {GEN7_PCODE_ILLEGAL_DATA, -EINVAL},
                    {GEN11_PCODE_ILLEGAL_SUBCOMMAND, -ENXIO},
                    {GEN11_PCODE_LOCKED, -EBUSY},
                    {GEN11_PCODE_REJECTED, -EACCES},
                    {GEN7_PCODE_MIN_FREQ_TABLE_GT_RATIO_OUT_OF_RANGE, -EOVERFLOW},
                    {0x7f, 0}};
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m, 0);
    struct i915_model_rc6 *r = i915_model_rc6(&m);
    uint32_t val = 0, val1 = 7;
    uint64_t t0;
    unsigned i;

    CHECK("pcode: the duty cycle read",
          i915_pcode_read(i915, HSW_PCODE_DYNAMIC_DUTY_CYCLE_CONTROL, &val, &val1) == 0 &&
              val == LENOVO_PCODE_DDCC);
    CHECK("pcode: the command posted with the ready bit, its data first",
          at(&m, GEN6_PCODE_DATA1, 7) <
              at(&m, GEN6_PCODE_MAILBOX, GEN6_PCODE_READY | HSW_PCODE_DYNAMIC_DUTY_CYCLE_CONTROL));
    CHECK("pcode: a write", i915_pcode_write(i915, GEN6_PCODE_WRITE_MIN_FREQ_TABLE, 0x50004) == 0 &&
                                r->ntable == 1 && r->table[0] == 0x50004);

    r->delay_us = 300;
    t0 = m.now_us;
    CHECK("pcode: a slow command waited for",
          i915_pcode_read(i915, GEN6_READ_OC_PARAMS, &val, 0) == 0 && val == 0 &&
              m.now_us - t0 >= 300 && i915->hw.timeouts == 0);
    r->delay_us = 0;

    r->stuck = 1;
    t0 = m.now_us;
    CHECK("pcode: a command never taken fails",
          i915_pcode_read(i915, GEN6_PCODE_READ_RC6VIDS, &val, 0) == -ETIMEDOUT);
    CHECK("pcode: by name, after 20.5 ms", strcmp(i915->hw.last_timeout, "pcode mailbox") == 0 &&
                                               m.now_us - t0 >= 20500 && m.now_us - t0 < 22000 &&
                                               logged("pcode mailbox: timeout after 20500 us"));
    CHECK("pcode: the next finds the mailbox busy",
          i915_pcode_read(i915, GEN6_PCODE_READ_RC6VIDS, &val, 0) == -EAGAIN);
    r->stuck = 0;
    i915_model_poke(&m, GEN6_PCODE_MAILBOX, 0);

    CHECK("pcode: a command it does not know",
          i915_pcode_write(i915, 0x42, 0) == -ENXIO && r->last_cmd == 0x42);
    for (i = 0; i < sizeof(statuses) / sizeof(statuses[0]); i++) {
        r->error = statuses[i].status;
        CHECK("pcode: each status as Linux maps it",
              i915_pcode_read(i915, GEN6_READ_OC_PARAMS, &val, 0) == statuses[i].err);
    }
    r->error = 0;
    done(&m);
}

/* ---- RPS ---- */

/** RPS's limits from the Lenovo's chip and the pcode. */
static void row_rps_init(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m, 0);
    struct i915_rps *rps = &i915->rps;

    i915_rps_init(i915);
    CHECK("rps init: RP0, RP1 and RPn from RP_STATE_CAP",
          rps->rp0_freq == 20 && rps->rp1_freq == 4 && rps->min_freq == 4);
    CHECK("rps init: RPe 500 MHz from the pcode", rps->efficient_freq == 10);
    CHECK("rps init: no overclocking: the limits and boost at RP0, idle at RPn",
          rps->max_freq == 20 && rps->max_freq_softlimit == 20 && rps->min_freq_softlimit == 4 &&
              rps->boost_freq == 20 && rps->idle_freq == 4);
    CHECK("rps init: Linux's thresholds and start",
          rps->up_threshold == 95 && rps->down_threshold == 85 && rps->cur_freq == 10 &&
              rps->last_freq == -1 && rps->power_mode == -1);
    CHECK("rps init: the up interval's expiry never masked",
          rps->pm_intrmsk_mbz == GEN6_PM_RP_UP_EI_EXPIRED &&
              i915_model_peek(&m, GEN6_PMINTRMSK) == ~GEN6_PM_RP_UP_EI_EXPIRED);
    CHECK("rps init: the interrupts off", i915_model_peek(&m, GEN6_PMIER) == 0 &&
                                              i915_model_peek(&m, GEN6_PMIMR) == 0xffffffffu &&
                                              i915->irq.deferred == i915_rps_work);
    CHECK("rps init: logged",
          logged("rps: RPn 200 MHz, RP1 200 MHz, RP0 1000 MHz, RPe 500 MHz, max 1000 MHz"));
    done(&m);

    i915 = fresh(&m, 0);
    i915_model_rc6(&m)->oc_params = 0x80000016u;
    i915_model_rc6(&m)->ddcc = 30u << 8;
    i915_rps_init(i915);
    CHECK("rps init: the overclocking limit raises max and boost, not the softlimit",
          i915->rps.max_freq == 22 && i915->rps.boost_freq == 22 &&
              i915->rps.max_freq_softlimit == 20);
    CHECK("rps init: RPe held within the limits", i915->rps.efficient_freq == 20);
    done(&m);

    i915 = fresh(&m, 0);
    i915_model_rc6(&m)->error = GEN6_PCODE_ILLEGAL_CMD;
    i915_rps_init(i915);
    CHECK("rps init: without the pcode, RPe is RP1",
          i915->rps.efficient_freq == 4 && i915->rps.max_freq == 20);
    done(&m);
}

/** RPS enabled: its writes, the ring's frequency table, inactive until the
 * GT unparks; and a chip with no range. */
static void row_rps_enable(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m, 1);
    struct i915_model_rc6 *r = i915_model_rc6(&m);
    struct i915_rps *rps = &i915->rps;
    uint32_t i, ok = 1;

    CHECK("rps enable: enabled with interrupts, inactive",
          rps->enabled && rps->interrupts && !rps->active);
    CHECK("rps enable: the down timeout of 50 ms and the idle hysteresis",
          i915_model_peek(&m, GEN6_RP_DOWN_TIMEOUT) == 50000 &&
              i915_model_peek(&m, GEN6_RP_IDLE_HYSTERSIS) == 10);
    CHECK("rps enable: the up threshold, down threshold and down timeout",
          rps->pm_events ==
              (GEN6_PM_RP_UP_THRESHOLD | GEN6_PM_RP_DOWN_THRESHOLD | GEN6_PM_RP_DOWN_TIMEOUT));
    CHECK("rps enable: reset to RPn", asked(&m) == 4 && rps->cur_freq == 4 && rps->last_freq == 4 &&
                                          i915_rps_read_actual_mhz(i915) == 200);
    CHECK("rps enable: low power's intervals and thresholds in PM units",
          rps->power_mode == I915_RPS_LOW_POWER && i915_model_peek(&m, GEN6_RP_UP_EI) == 12500 &&
              i915_model_peek(&m, GEN6_RP_UP_THRESHOLD) == 11875 &&
              i915_model_peek(&m, GEN6_RP_DOWN_EI) == 25000 &&
              i915_model_peek(&m, GEN6_RP_DOWN_THRESHOLD) == 21250);
    CHECK("rps enable: RPS's hardware on, busy and idle averaged",
          i915_model_peek(&m, GEN6_RP_CONTROL) ==
              (GEN6_RP_MEDIA_TURBO | GEN6_RP_MEDIA_HW_NORMAL_MODE | GEN6_RP_MEDIA_IS_GFX |
               GEN6_RP_ENABLE | GEN6_RP_UP_BUSY_AVG | GEN6_RP_DOWN_IDLE_AVG));
    CHECK("rps enable: the ring's table, a frequency each from RP0 to RPn", r->ntable == 17);
    for (i = 0; i < r->ntable; i++) {
        uint32_t gpu = 20 - i;
        ok = ok && r->table[i] == ((gpu * 5 / 4) << 16 | gpu);
    }
    CHECK("rps enable: the ring at 5/4 of the GPU, the IA left to cpufreq", ok);
    CHECK("rps enable: parked, its interrupts off",
          i915_model_peek(&m, GEN6_PMIER) == 0 && i915_model_peek(&m, GEN6_PMIMR) == 0xffffffffu);
    done(&m);

    i915 = fresh(&m, 0);
    i915_model_poke(&m, DCLK, 6);
    i915_rc6_init(i915);
    i915_rps_init(i915);
    i915_gt_pm_enable(i915);
    r = i915_model_rc6(&m);
    CHECK("rps enable: the ring never below DDR's 16",
          r->ntable == 17 && r->table[0] == (25u << 16 | 20u) && r->table[16] == (16u << 16 | 4u) &&
              r->table[8] == (16u << 16 | 12u));
    done(&m);

    i915 = fresh(&m, 0);
    i915_model_poke(&m, GEN6_RP_STATE_CAP, 0x00040404u);
    i915_rc6_init(i915);
    i915_rps_init(i915);
    i915_gt_pm_enable(i915);
    CHECK("rps enable: no range, no RPS and no table",
          !i915->rps.enabled && i915_model_rc6(&m)->ntable == 0 &&
              logged("rps: off: no range from 200 MHz to 200 MHz") && i915->rc6.enabled);
    i915_gt_unpark(i915);
    CHECK("rps: a disabled RPS stays inactive", !i915->rps.active);
    done(&m);
}

/** The up and down interrupts step the frequency; the park and unpark. */
static void row_rps_interrupts(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m, 1);
    struct i915_rps *rps = &i915->rps;
    static const int ups[] = {5, 7, 11, 19, 20, 20};
    static const int downs[] = {10, 8, 4, 4};
    unsigned i;
    int ok = 1;

    i915_gt_unpark(i915);
    CHECK("unpark: active at the frequency asked for", rps->active && asked(&m) == 4);
    CHECK("unpark: the interrupt limits and mask at RPn",
          i915_model_peek(&m, GEN6_RP_INTERRUPT_LIMITS) == (20u << 24 | 4u << 16) &&
              i915_model_peek(&m, GEN6_PMINTRMSK) ==
                  ~(GEN6_PM_RP_UP_THRESHOLD | GEN6_PM_RP_UP_EI_EXPIRED));
    CHECK("unpark: the up and down interrupts enabled and unmasked",
          i915_model_peek(&m, GEN6_PMIER) == rps->pm_events &&
              i915_model_peek(&m, GEN6_PMIMR) == ~rps->pm_events);

    for (i = 0; i < sizeof(ups) / sizeof(ups[0]); i++) {
        pm_event(&m, GEN6_PM_RP_UP_THRESHOLD);
        ok = ok && asked(&m) == ups[i] && rps->cur_freq == ups[i];
    }
    CHECK("up: steps of 1, 2, 4 and 8, held at RP0", ok && rps->work_runs == 6);
    CHECK("up: the GT runs at 1000 MHz", i915_rps_read_actual_mhz(i915) == 1000);
    CHECK("up: high power's intervals at RP0",
          rps->power_mode == I915_RPS_HIGH_POWER && i915_model_peek(&m, GEN6_RP_UP_EI) == 7813);
    CHECK("up: the events unmasked again after each",
          i915_model_peek(&m, GEN6_PMIMR) == ~rps->pm_events && rps->pm_iir == 0);
    CHECK(
        "up: at RP0 only the down events stay unmasked in PMINTRMSK",
        i915_model_peek(&m, GEN6_PMINTRMSK) ==
                ~(GEN6_PM_RP_UP_EI_EXPIRED | GEN6_PM_RP_DOWN_THRESHOLD | GEN6_PM_RP_DOWN_TIMEOUT) &&
            i915_model_peek(&m, GEN6_RP_INTERRUPT_LIMITS) == 20u << 24);

    pm_event(&m, GEN6_PM_RP_DOWN_TIMEOUT);
    CHECK("down timeout: to RPe", asked(&m) == 10);
    pm_event(&m, GEN6_PM_RP_DOWN_TIMEOUT);
    CHECK("down timeout: then to RPn", asked(&m) == 4);
    pm_event(&m, GEN6_PM_RP_UP_THRESHOLD);
    pm_event(&m, GEN6_PM_RP_UP_THRESHOLD);
    pm_event(&m, GEN6_PM_RP_UP_THRESHOLD);
    CHECK("up again: 5, 7, 11", asked(&m) == 11);
    ok = 1;
    for (i = 0; i < sizeof(downs) / sizeof(downs[0]); i++) {
        pm_event(&m, GEN6_PM_RP_DOWN_THRESHOLD);
        ok = ok && asked(&m) == downs[i];
    }
    CHECK("down: steps of 1, 2 and 4, held at RPn", ok);

    i = rps->work_runs;
    i915_model_irq_event(&m, I915_IRQ_PM, GEN6_PM_RP_UP_EI_EXPIRED);
    take_messages(&m);
    CHECK("an event RPS does not take stays masked in the bank",
          asked(&m) == 4 && rps->work_runs == i && i915->irq.unclaimed == 0 &&
              i915->irq.messages > 0);

    pm_event(&m, GEN6_PM_RP_UP_THRESHOLD);
    pm_event(&m, GEN6_PM_RP_UP_THRESHOLD);
    pm_event(&m, GEN6_PM_RP_UP_THRESHOLD);
    pm_event(&m, GEN6_PM_RP_UP_THRESHOLD);
    pm_event(&m, GEN6_PM_RP_UP_THRESHOLD);
    CHECK("up to RP0 again", asked(&m) == 20);
    m.nlog = 0;
    i915_gt_park(i915);
    CHECK("park: inactive, the interrupts off and cleared",
          !rps->active && i915_model_peek(&m, GEN6_PMIER) == 0 &&
              i915_model_peek(&m, GEN6_PMIMR) == 0xffffffffu &&
              i915_model_peek(&m, GEN6_PMINTRMSK) == ~GEN6_PM_RP_UP_EI_EXPIRED &&
              i915_model_count(&m, I915_MODEL_WRITE, GEN6_PMIIR) == 2);
    CHECK("park: the idle frequency asked for under forcewake",
          asked(&m) == 4 &&
              at(&m, FORCEWAKE_MT, 0x00010001u) < at(&m, GEN6_RPNSWREQ, HSW_FREQUENCY(4)) &&
              i915->uncore.wake_count == 0);
    CHECK("park: the next start a step down", rps->cur_freq == 18 && rps->last_adj == -2);
    pm_event(&m, GEN6_PM_RP_UP_THRESHOLD);
    CHECK("parked: an up event does nothing", asked(&m) == 4 && rps->cur_freq == 18);
    i915_gt_unpark(i915);
    CHECK("unpark: from where the park left it", asked(&m) == 18);
    i915_gt_park(i915);
    i915_gt_unpark(i915);
    CHECK("unpark: the steps down double, to RPe at least", asked(&m) == 14);
    i915_gt_park(i915);
    i915_gt_unpark(i915);
    i915_gt_park(i915);
    i915_gt_unpark(i915);
    CHECK("unpark: never below RPe", asked(&m) == 10 && rps->last_adj == 0);
    done(&m);
}

/** Whether every request written to RPNSWREQ in `m`'s log lies within
 * `lo` and `hi`. */
static int requests_within(const struct i915_model *m, int lo, int hi)
{
    uint32_t k;
    int ok = 1;

    for (k = 0; k < m->nlog; k++) {
        if (m->log[k].op == I915_MODEL_WRITE && m->log[k].reg == GEN6_RPNSWREQ) {
            int f = (int)(m->log[k].value >> HSW_FREQUENCY_SHIFT);
            ok = ok && f >= lo && f <= hi;
        }
    }
    return ok;
}

/** 400 random up and down events, the softlimits widened half way, as
 * 300 to 650 MHz and then 400 to 900; whether every request stayed within
 * the softlimits of its time. */
static int random_events(struct i915_model *m, struct drm_i915_private *i915)
{
    static const uint32_t events[] = {GEN6_PM_RP_UP_THRESHOLD, GEN6_PM_RP_DOWN_THRESHOLD,
                                      GEN6_PM_RP_DOWN_TIMEOUT,
                                      GEN6_PM_RP_UP_THRESHOLD | GEN6_PM_RP_DOWN_THRESHOLD};
    uint32_t i, seed = 12345;
    int ok = 1, lo = 6, hi = 13;

    for (i = 0; i < 400; i++) {
        seed = seed * 1103515245u + 12345u;
        m->nlog = 0;
        if (i == 200) {
            ok = ok && i915_rps_set_max_mhz(i915, 900) == 0 && i915_rps_set_min_mhz(i915, 400) == 0;
            hi = 18;
        }
        pm_event(m, events[(seed >> 16) % 4u]);
        ok = ok && requests_within(m, lo, hi);
        if (i == 200) {
            lo = 8; /* the raise to the new min came with the first event */
        }
    }
    return ok;
}

/** The softlimits set and refused; every request within them. */
static void row_rps_limits(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m, 1);
    struct i915_rps *rps = &i915->rps;
    int ok;

    i915_gt_unpark(i915);
    CHECK("max: 600 MHz", i915_rps_set_max_mhz(i915, 600) == 0 && rps->max_freq_softlimit == 12);
    CHECK("min: 300 MHz", i915_rps_set_min_mhz(i915, 300) == 0 && rps->min_freq_softlimit == 6);
    CHECK("min: the frequency raised into the limits", asked(&m) == 6 && rps->cur_freq == 6);
    CHECK("min: the interrupt limits follow",
          i915_model_peek(&m, GEN6_RP_INTERRUPT_LIMITS) == (12u << 24 | 6u << 16));
    CHECK("max: 625 MHz rounds to 650",
          i915_rps_set_max_mhz(i915, 625) == 0 && rps->max_freq_softlimit == 13);
    CHECK("max: refused below the softlimit min",
          i915_rps_set_max_mhz(i915, 250) == -EINVAL && rps->max_freq_softlimit == 13);
    CHECK("max: refused past the overclocking limit", i915_rps_set_max_mhz(i915, 1050) == -EINVAL);
    CHECK("min: refused past the softlimit max", i915_rps_set_min_mhz(i915, 700) == -EINVAL);
    CHECK("min: refused below RPn", i915_rps_set_min_mhz(i915, 100) == -EINVAL);
    CHECK("boost: within the chip's limits", i915_rps_set_boost_mhz(i915, 800) == 0 &&
                                                 rps->boost_freq == 16 &&
                                                 i915_rps_set_boost_mhz(i915, 1100) == -EINVAL &&
                                                 i915_rps_set_boost_mhz(i915, 150) == -EINVAL);
    ok = random_events(&m, i915);
    CHECK("limits: 400 events, every request within the softlimits of its time",
          ok && rps->work_runs == 400 && m.log_lost == 0 && asked(&m) >= 8 && asked(&m) <= 18);
    done(&m);
}

/* ---- /sys ---- */

/** The /sys files by Linux's names and modes. */
static void sysfs_names(void)
{
    static const char *const names[] = {"gt_act_freq_mhz", "gt_cur_freq_mhz", "gt_boost_freq_mhz",
                                        "gt_max_freq_mhz", "gt_min_freq_mhz", "gt_RP0_freq_mhz",
                                        "gt_RP1_freq_mhz", "gt_RPn_freq_mhz", "power",
                                        "rc6_enable",      "rc6_residency_ms"};
    int i, ok = 1;

    CHECK("sysfs: eleven entries in Linux's order", nfiles == 11);
    for (i = 0; i < nfiles && i < 11; i++) {
        ok = ok && strcmp(files[i].name, names[i]) == 0 &&
             files[i].parent == (i < 9 ? TEST_CARD : 10);
    }
    CHECK("sysfs: the names, the frequencies on the card and RC6 in power/", ok);
    CHECK("sysfs: boost, max and min written by root, the rest read",
          files[2].attr->mode == 0644u && files[3].attr->mode == 0644u &&
              files[4].attr->mode == 0644u && files[0].attr->mode == 0444u &&
              files[7].attr->mode == 0444u && files[9].attr->mode == 0444u &&
              !files[0].attr->store && !files[10].attr->store);
}

/** The values the /sys files of `m`'s driver show, 10 s idle included. */
static void sysfs_values(struct i915_model *m)
{
    CHECK("sysfs: RP0 1000", show("gt_RP0_freq_mhz") && strcmp(shown, "1000\n") == 0);
    CHECK("sysfs: RP1 200", show("gt_RP1_freq_mhz") && strcmp(shown, "200\n") == 0);
    CHECK("sysfs: RPn 200", show("gt_RPn_freq_mhz") && strcmp(shown, "200\n") == 0);
    CHECK("sysfs: max 1000", show("gt_max_freq_mhz") && strcmp(shown, "1000\n") == 0);
    CHECK("sysfs: min 200", show("gt_min_freq_mhz") && strcmp(shown, "200\n") == 0);
    CHECK("sysfs: boost 1000", show("gt_boost_freq_mhz") && strcmp(shown, "1000\n") == 0);
    CHECK("sysfs: cur 200 when idle", show("gt_cur_freq_mhz") && strcmp(shown, "200\n") == 0);
    CHECK("sysfs: act 200", show("gt_act_freq_mhz") && strcmp(shown, "200\n") == 0);
    CHECK("sysfs: rc6_enable 1", show("power/rc6_enable") && strcmp(shown, "1\n") == 0);
    pass_ms(m, 10000);
    CHECK("sysfs: rc6_residency_ms after 10 s idle",
          show("power/rc6_residency_ms") && strcmp(shown, "10000\n") == 0);
}

/** Writes to the /sys files of `i915`: Linux's kstrtou32 and its
 * refusals, and the limits'. */
static void sysfs_stores(struct drm_i915_private *i915)
{
    CHECK("sysfs: max takes 500 with a newline", store("gt_max_freq_mhz", "500\n") == 4 &&
                                                     show("gt_max_freq_mhz") &&
                                                     strcmp(shown, "500\n") == 0);
    CHECK("sysfs: hexadecimal", store("gt_max_freq_mhz", "0x190") == 5 && show("gt_max_freq_mhz") &&
                                    strcmp(shown, "400\n") == 0);
    CHECK("sysfs: octal",
          store("gt_min_freq_mhz", "0454") == 4 && i915->rps.min_freq_softlimit == 6);
    CHECK("sysfs: a plus sign", store("gt_min_freq_mhz", "+200\n") == 5);
    CHECK("sysfs: refused: no number", store("gt_max_freq_mhz", "fast") == -EINVAL &&
                                           store("gt_max_freq_mhz", "") == -EINVAL &&
                                           store("gt_max_freq_mhz", "\n") == -EINVAL &&
                                           store("gt_max_freq_mhz", "08") == -EINVAL &&
                                           store("gt_max_freq_mhz", "500\n\n") == -EINVAL &&
                                           store("gt_max_freq_mhz", " 500") == -EINVAL);
    CHECK("sysfs: refused: past 32 bits", store("gt_max_freq_mhz", "4294967296") == -ERANGE &&
                                              store("gt_boost_freq_mhz", "4294967295") == -EINVAL);
    CHECK("sysfs: refused: outside the chip's range",
          store("gt_min_freq_mhz", "100") == -EINVAL &&
              store("gt_max_freq_mhz", "2000") == -EINVAL);
    CHECK("sysfs: boost written", store("gt_boost_freq_mhz", "750") == 3 &&
                                      show("gt_boost_freq_mhz") && strcmp(shown, "750\n") == 0);
    CHECK("sysfs: a read-only file takes no write", store("gt_RP0_freq_mhz", "300") == -EACCES);
}

/** The files of a bound driver, then a card without a directory and one
 * whose power/ is there already. */
static void row_sysfs(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m, 1);

    i915_sysfs_register(i915);
    CHECK("sysfs: the function shown bound to i915",
          bound_key == (m.pci.bus << 8 | m.pci.device << 3 | m.pci.function) && bound_driver &&
              strcmp(bound_driver, "i915") == 0);
    sysfs_names();
    sysfs_values(&m);
    sysfs_stores(i915);
    i915_sysfs_unregister(i915);
    CHECK("sysfs: and no longer once unregistered", bound_key == -1 && !bound_driver);
    done(&m);

    i915 = fresh(&m, 1);
    card_dir = 0;
    i915_sysfs_register(i915);
    CHECK("sysfs: no card, no files", nfiles == 0);
    card_dir = TEST_CARD;
    CHECK("sysfs: an existing power directory is used", sysfs_add_dir(TEST_CARD, "power") == 2);
    i915_sysfs_register(i915);
    CHECK("sysfs: its files below it", nfiles == 11 && files[9].parent == 2 &&
                                           files[10].parent == 2 && show("power/rc6_enable"));
    done(&m);
}

/* ---- off ---- */

/** RPS and RC6 turned off, from parked and from active. */
static void row_disable(void)
{
    struct i915_model m;
    struct drm_i915_private *i915 = fresh(&m, 1);

    i915_gt_unpark(i915);
    i915_gt_pm_disable(i915);
    CHECK("off: RPS's hardware and RC6 off",
          i915_model_peek(&m, GEN6_RP_CONTROL) == 0 && i915_model_peek(&m, GEN6_RC_CONTROL) == 0 &&
              i915_model_peek(&m, GEN6_RC_STATE) == 0 && !i915->rps.enabled && !i915->rps.active &&
              !i915->rc6.enabled);
    CHECK("off: the up and down interrupts off",
          i915_model_peek(&m, GEN6_PMIER) == 0 && i915_model_peek(&m, GEN6_PMIMR) == 0xffffffffu);
    CHECK("off: no forcewake left", i915->uncore.wake_count == 0);
    pass_ms(&m, 100);
    CHECK("off: no RC6", !i915_model_rc6(&m)->in_rc6);
    i915_gt_pm_disable(i915);
    i915_gt_park(i915);
    i915_gt_unpark(i915);
    CHECK("off: a second disable and a park or unpark change nothing",
          i915_model_peek(&m, GEN6_RC_CONTROL) == 0 && !i915->rps.active);
    done(&m);
}

int main(void)
{
    row_rc6_enable();
    row_rc6_idle();
    row_rc6_wrap();
    row_pcode();
    row_rps_init();
    row_rps_enable();
    row_rps_interrupts();
    row_rps_limits();
    row_sysfs();
    row_disable();
    if (fails) {
        printf("i915-power-test: %d failures\n", fails);
        return 1;
    }
    printf("i915-power-test: ok\n");
    return 0;
}
