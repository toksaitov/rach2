/*
 * intel_pps.c: the eDP panel's power sequencer in the Lynx Point PCH,
 * after Linux 6.12's display/intel_pps.c for one panel on DDI: its delays
 * from the registers, the VBT and the eDP specification, the panel's VDD,
 * its power on and off, and the backlight's enable timed against them.
 */

#include "i915.h"
#include "i915_reg.h"

#include "../../klog.h"
#include "../../rtc.h"
#include "../../timer.h"
#include "../../proc/percpu.h"
#include "../../proc/sched.h"

/* How long the sequencer is given to finish, and how often it is read
 * meanwhile, Linux 6.18's wait_panel_status. 6.12 reads with a backoff
 * from 10 us to 1 ms; the sequencer's states take tens of milliseconds and
 * the wait sleeps, so a read every 10 ms costs nothing and frees the CPU. */
#define PPS_WAIT_MS 5000u
#define PPS_POLL_MS 10u

/* The states the sequencer is waited for, Linux's IDLE_*_MASK and
 * IDLE_*_VALUE: on and idle, off, and off with its cycle delay done. */
#define IDLE_ON_MASK     (PP_ON | PP_SEQUENCE_MASK | PP_SEQUENCE_STATE_MASK)
#define IDLE_ON_VALUE    (PP_ON | PP_SEQUENCE_NONE | PP_SEQUENCE_STATE_ON_IDLE)
#define IDLE_OFF_MASK    (PP_ON | PP_SEQUENCE_MASK)
#define IDLE_OFF_VALUE   PP_SEQUENCE_NONE
#define IDLE_CYCLE_MASK  (PP_ON | PP_SEQUENCE_MASK | PP_CYCLE_DELAY_ACTIVE | PP_SEQUENCE_STATE_MASK)
#define IDLE_CYCLE_VALUE (PP_SEQUENCE_NONE | PP_SEQUENCE_STATE_OFF_IDLE)

/* Linux keeps an unused VDD on for five power cycles before it lets it
 * go (edp_panel_vdd_schedule_off). */
#define VDD_OFF_CYCLES 5u

/** A register of the device. */
static uint32_t pps_rd(struct drm_i915_private *i915, uint32_t reg)
{
    return i915_read(&i915->hw, reg);
}

/** The time now in milliseconds. */
static uint64_t pps_now_ms(void)
{
    return drm_div64(rtc_ticks_now() * 1000u, TICK_HZ);
}

/** Whether the deadline at `arg`, in ticks, has passed. */
static int deadline_passed(void *arg)
{
    return rtc_ticks_now() >= *(const uint64_t *)arg;
}

void intel_display_msleep(struct drm_i915_private *i915, uint32_t ms)
{
    struct intel_pps *pps = &i915->display.dp.pps;
    uint64_t due, saved;

    if (ms == 0) {
        return;
    }
    if (!current_proc || !sched_is_enabled()) {
        i915_delay_us(&i915->hw, ms * 1000u);
        return;
    }
    /* A whole tick more, so that a sleep begun late in a tick is never
     * short. */
    due = rtc_ticks_now() + drm_div64((uint64_t)ms * TICK_HZ + 999u, 1000u) + 1u;
    saved = current_proc->wakeup_tick;
    current_proc->wakeup_tick = due;
    (void)wq_wait_event_nointr(&pps->sleep, deadline_passed, &due);
    current_proc->wakeup_tick = saved;
}

/** Sleep what is left of `delay` milliseconds since `since`, Linux's
 * wait_remaining_ms_from_jiffies. */
static void wait_remaining(struct drm_i915_private *i915, uint64_t since, uint32_t delay)
{
    uint64_t now = pps_now_ms();

    if (now < since + delay) {
        intel_display_msleep(i915, (uint32_t)(since + delay - now));
    }
}

/** PP_CONTROL with the key that unlocks its writes, Linux's
 * ilk_get_pp_control. */
static uint32_t pp_control(struct drm_i915_private *i915)
{
    return (pps_rd(i915, PP_CONTROL) & ~PANEL_UNLOCK_MASK) | PANEL_UNLOCK_REGS;
}

/** Write PP_CONTROL and read it back, so the write has landed. */
static void pp_write(struct drm_i915_private *i915, uint32_t pp)
{
    i915_write(&i915->hw, PP_CONTROL, pp);
    i915_posting_read(&i915->hw, PP_CONTROL);
}

/** Whether the panel's power is on, by the sequencer's status. */
static int have_power(struct drm_i915_private *i915)
{
    return (pps_rd(i915, PP_STATUS) & PP_ON) != 0;
}

/** Whether the panel's VDD is forced on. */
static int have_vdd(struct drm_i915_private *i915)
{
    return (pps_rd(i915, PP_CONTROL) & EDP_FORCE_VDD) != 0;
}

int intel_pps_have_power_or_vdd(struct drm_i915_private *i915)
{
    return have_power(i915) || have_vdd(i915);
}

/**
 * Wait until the status bits `mask` read `value`, Linux's
 * wait_panel_status: read every 10 ms for 5 s. A sequencer that does not
 * get there is logged with what it read, and the caller goes on, as on
 * Linux.
 */
static void wait_status(struct drm_i915_private *i915, const char *what, uint32_t mask,
                        uint32_t value)
{
    uint32_t waited;

    for (waited = 0;; waited += PPS_POLL_MS) {
        if ((pps_rd(i915, PP_STATUS) & mask) == value) {
            return;
        }
        if (waited >= PPS_WAIT_MS) {
            break;
        }
        intel_display_msleep(i915, PPS_POLL_MS);
    }
    i915->hw.timeouts++;
    i915->hw.last_timeout = what;
    klog(KLOG_ERR, "i915", "pps: %s: timeout after %u ms: status 0x%08x, control 0x%08x", what,
         (unsigned)PPS_WAIT_MS, (unsigned)pps_rd(i915, PP_STATUS),
         (unsigned)pps_rd(i915, PP_CONTROL));
}

/** Wait out the power cycle delay since the panel went off, then the
 * sequencer's own, Linux's wait_panel_power_cycle. */
static void wait_power_cycle(struct drm_i915_private *i915)
{
    struct intel_pps *pps = &i915->display.dp.pps;

    wait_remaining(i915, pps->power_off_time, pps->power_cycle_ms);
    wait_status(i915, "pps power cycle", IDLE_CYCLE_MASK, IDLE_CYCLE_VALUE);
}

/** 100 us units in milliseconds, rounded up. */
static uint32_t units_to_ms(uint32_t units)
{
    return (units + 9u) / 10u;
}

/** The larger of `a` and `b`. */
static uint32_t max_u32(uint32_t a, uint32_t b)
{
    return a > b ? a : b;
}

/**
 * Choose the delays, Linux 6.12's pps_init_delays: for each, the larger
 * of the firmware's registers and the VBT's, or the eDP 1.3 limit when
 * both are 0; then the backlight's two set to 1 in the registers, since
 * the driver times the backlight itself, and the power cycle rounded up
 * to the 100 ms the divisor counts in. The power cycle's field is read
 * and written as 6.18 does, as the delay in 100 ms plus one, which is how
 * the hardware counts it; 6.12 reads the field whole and adds 100 ms to
 * the VBT's and the specification's, and so waits 100 ms longer than the
 * panel needs, 600 ms for the Lenovo's 500.
 */
static void init_delays(struct drm_i915_private *i915)
{
    struct intel_pps *pps = &i915->display.dp.pps;
    const struct intel_edp_power_seq *vbt = &i915->display.dp.vbt.edp.pps;
    uint32_t on = pps_rd(i915, PP_ON_DELAYS), off = pps_rd(i915, PP_OFF_DELAYS);
    uint32_t div = pps_rd(i915, PP_DIVISOR) & PANEL_POWER_CYCLE_DELAY_MASK;
    uint32_t cur[5], v[5], spec[5], fin[5];
    int i;

    cur[0] = (on >> PANEL_POWER_UP_DELAY_SHIFT) & PANEL_DELAY_FIELD_MASK;
    cur[1] = on & PANEL_DELAY_FIELD_MASK;
    cur[2] = off & PANEL_DELAY_FIELD_MASK;
    cur[3] = (off >> PANEL_POWER_DOWN_DELAY_SHIFT) & PANEL_DELAY_FIELD_MASK;
    cur[4] = div ? (div - 1u) * 1000u : 0; /* the divisor counts 100 ms plus one */
    v[0] = vbt->t1_t3;
    v[1] = vbt->t8;
    v[2] = vbt->t9;
    v[3] = vbt->t10;
    v[4] = vbt->t11_t12;
    spec[0] = (10u + 200u) * 10u; /* T1 + T3 */
    spec[1] = 50u * 10u;          /* T8 has no limit; T7's */
    spec[2] = 50u * 10u;          /* T9, as T8 */
    spec[3] = 500u * 10u;         /* T10 */
    spec[4] = (10u + 500u) * 10u; /* T11 + T12 */
    for (i = 0; i < 5; i++) {
        fin[i] = max_u32(cur[i], v[i]) == 0 ? spec[i] : max_u32(cur[i], v[i]);
    }
    pps->power_up_ms = units_to_ms(fin[0]);
    pps->backlight_on_ms = units_to_ms(fin[1]);
    pps->backlight_off_ms = units_to_ms(fin[2]);
    pps->power_down_ms = units_to_ms(fin[3]);
    pps->power_cycle_ms = units_to_ms(fin[4]);
    pps->power_up = fin[0];
    pps->backlight_on = 1;
    pps->backlight_off = 1;
    pps->power_down = fin[3];
    pps->power_cycle = (fin[4] + 999u) / 1000u * 1000u;
}

/** Write the delays and the divisor, Linux's pps_init_registers: the
 * reference divider counts 100 us of the PCH's raw clock, and the power
 * cycle field 100 ms plus one. */
static void init_registers(struct drm_i915_private *i915)
{
    struct intel_pps *pps = &i915->display.dp.pps;
    uint32_t mhz = pps_rd(i915, PCH_RAWCLK_FREQ) & RAWCLK_FREQ_MASK;

    i915_write(&i915->hw, PP_ON_DELAYS,
               pps->power_up << PANEL_POWER_UP_DELAY_SHIFT | pps->backlight_on);
    i915_write(&i915->hw, PP_OFF_DELAYS,
               pps->power_down << PANEL_POWER_DOWN_DELAY_SHIFT | pps->backlight_off);
    i915_write(&i915->hw, PP_DIVISOR,
               ((100u * mhz) / 2u - 1u) << PP_REFERENCE_DIVIDER_SHIFT |
                   ((pps->power_cycle + 999u) / 1000u + 1u));
}

void intel_pps_init(struct drm_i915_private *i915)
{
    struct intel_pps *pps = &i915->display.dp.pps;
    uint64_t now = pps_now_ms();

    wait_queue_init(&pps->sleep);
    /* Linux's pps_init_timestamps: the panel's last power off taken as
     * long past, since only a driver that waited out its cycle turned it
     * off before this one. */
    pps->power_off_time = 0;
    pps->last_power_on = now;
    pps->last_backlight_off = now;
    init_delays(i915);
    init_registers(i915);
    pps->ready = 1;
    klog(KLOG_INFO, "i915",
         "pps: power up %u ms, down %u ms, cycle %u ms, backlight on %u ms, off %u ms",
         (unsigned)pps->power_up_ms, (unsigned)pps->power_down_ms, (unsigned)pps->power_cycle_ms,
         (unsigned)pps->backlight_on_ms, (unsigned)pps->backlight_off_ms);
    if (have_vdd(i915)) {
        /* The firmware's VDD, let go as an unused one is. */
        pps->vdd_off_due = now + VDD_OFF_CYCLES * pps->power_cycle_ms;
    }
}

void intel_pps_init_late(struct drm_i915_private *i915)
{
    struct intel_pps *pps = &i915->display.dp.pps;

    init_delays(i915);
    init_registers(i915);
    if (have_vdd(i915) && !pps->want_vdd) {
        pps->vdd_off_due = pps_now_ms() + VDD_OFF_CYCLES * pps->power_cycle_ms;
    }
}

int intel_pps_vdd_on_unlocked(struct drm_i915_private *i915)
{
    struct intel_pps *pps = &i915->display.dp.pps;
    int need_off = !pps->want_vdd;

    if (!pps->ready) {
        return 0;
    }
    pps->vdd_off_due = 0;
    pps->want_vdd = 1;
    if (have_vdd(i915)) {
        return need_off;
    }
    if (!have_power(i915)) {
        wait_power_cycle(i915);
    }
    pp_write(i915, pp_control(i915) | EDP_FORCE_VDD);
    if (!have_power(i915)) {
        /* The panel's logic needs its power up delay before the AUX
         * channel reaches it. */
        intel_display_msleep(i915, pps->power_up_ms);
    }
    return need_off;
}

/** Let VDD go now, Linux's intel_pps_vdd_off_sync_unlocked; a panel then
 * without power starts its power cycle. */
static void vdd_off_sync(struct drm_i915_private *i915)
{
    struct intel_pps *pps = &i915->display.dp.pps;
    uint32_t pp;

    pps->vdd_off_due = 0;
    if (!have_vdd(i915)) {
        return;
    }
    pp = pp_control(i915) & ~EDP_FORCE_VDD;
    pp_write(i915, pp);
    if (!(pp & PANEL_POWER_ON)) {
        pps->power_off_time = pps_now_ms();
    }
}

void intel_pps_vdd_off_unlocked(struct drm_i915_private *i915, int sync)
{
    struct intel_pps *pps = &i915->display.dp.pps;

    if (!pps->ready) {
        return;
    }
    pps->want_vdd = 0;
    if (sync) {
        vdd_off_sync(i915);
    } else {
        pps->vdd_off_due = pps_now_ms() + VDD_OFF_CYCLES * pps->power_cycle_ms;
    }
}

void intel_pps_vdd_work(struct drm_i915_private *i915)
{
    struct intel_pps *pps = &i915->display.dp.pps;

    if (pps->ready && !pps->want_vdd && pps->vdd_off_due != 0 && pps_now_ms() >= pps->vdd_off_due) {
        vdd_off_sync(i915);
    }
}

void intel_pps_on(struct drm_i915_private *i915)
{
    struct intel_pps *pps = &i915->display.dp.pps;

    if (!pps->ready) {
        return;
    }
    if (have_power(i915)) {
        klog(KLOG_WARN, "i915", "pps: the panel's power is already on");
        return;
    }
    wait_power_cycle(i915);
    pp_write(i915, pp_control(i915) | PANEL_POWER_ON | PANEL_POWER_RESET);
    wait_status(i915, "pps power on", IDLE_ON_MASK, IDLE_ON_VALUE);
    pps->last_power_on = pps_now_ms();
}

void intel_pps_off(struct drm_i915_private *i915)
{
    struct intel_pps *pps = &i915->display.dp.pps;
    uint32_t pp;

    if (!pps->ready) {
        return;
    }
    if (!pps->want_vdd) {
        klog(KLOG_WARN, "i915", "pps: the panel goes off without VDD wanted");
    }
    /* Panel power and forced VDD go off together, as some panels need. */
    pp = pp_control(i915) & ~(PANEL_POWER_ON | PANEL_POWER_RESET | EDP_FORCE_VDD | EDP_BLC_ENABLE);
    pps->want_vdd = 0;
    pps->vdd_off_due = 0;
    pp_write(i915, pp);
    wait_status(i915, "pps power off", IDLE_OFF_MASK, IDLE_OFF_VALUE);
    pps->power_off_time = pps_now_ms();
}

void intel_pps_backlight_on(struct drm_i915_private *i915)
{
    struct intel_pps *pps = &i915->display.dp.pps;

    /* The image steadies on the link before the backlight shows it. */
    wait_remaining(i915, pps->last_power_on, pps->backlight_on_ms);
    intel_backlight_pipe(i915, 1);
}

void intel_pps_backlight_off(struct drm_i915_private *i915)
{
    struct intel_pps *pps = &i915->display.dp.pps;

    intel_backlight_pipe(i915, 0);
    pps->last_backlight_off = pps_now_ms();
    wait_remaining(i915, pps->last_backlight_off, pps->backlight_off_ms);
}
