/*
 * apictimer.c: the local APIC's own inputs when the I/O APIC has the lines,
 * as apictimer.h describes: `LINT0` and `LINT1`, the timer as the tick, and
 * the timer's interrupts between ticks for the times `timer_wake_at` asks.
 *
 * The clock under the tick reads the timer's current count: periodic mode
 * reloads the initial count when the count reaches zero, at the moment the
 * vector is raised, so the counts gone from the initial count are the place
 * within the tick, as channel 0's are through the 8259.
 */

#include "apictimer.h"
#include "ioapic.h"
#include "lapic.h"
#include "refclock.h"

#include "../arch.h"
#include "../../irq.h"
#include "../../rtc.h"
#include "../../lib/kirq.h"
#include "../../klog.h"
#include "../../proc/percpu.h"
#include "../../timer.h"

#define CALIBRATE_US 20000u

/* A reference that has not counted 20 ms after this many reads has
 * stopped. */
#define CALIBRATE_READS 10000000u

/* The reads of the reference on either side of a read of the timer, a few
 * microseconds apart on time, are taken again when they are further apart
 * than this: the processor was stopped between them. */
#define SAMPLE_CLOSE_US 50u
#define SAMPLE_TRIES    16u

/* The 8254's measurements of the timer, of which the least is taken: a
 * processor stopped during one can only make it count too few of the 8254's
 * counts (pit_wait_counts). */
#define PIT_WINDOWS 3u

/* The tick rate `apic_timer_start` was given, which the counter's
 * arithmetic divides a second by. */
static uint32_t tick_hz = TICK_HZ;

/*
 * The timer between ticks, kept as Linux's hrtimer interrupt keeps its
 * clock event device. A processor's timer is periodic, raising
 * APIC_TIMER_VECTOR every tick, until `timer_wake_at` first asks it for a
 * time; from then on it is one-shot, and every interrupt counts it once to
 * the earlier of two absolute times of the monotonic clock: the processor's
 * next tick, raising APIC_TIMER_VECTOR, and the time asked for, raising
 * `hr_vector`. The tick's time moves on by whole ticks past the clock
 * (`hr_forward`), as tick_nohz_handler moves the tick's hrtimer by
 * hrtimer_forward (kernel/time/tick-sched.c:309, kernel/time/hrtimer.c:
 * 1046-1077), so the delay of an interrupt shifts no later tick; and an
 * interrupt at APIC_TIMER_VECTOR is the tick only once the clock has
 * reached the tick's time, as __hrtimer_run_queues runs a timer only once
 * the clock has reached its expiry (kernel/time/hrtimer.c:1814), so a count
 * that ran out while the timer was programmed again raises no second tick.
 */
struct hr_state {
    int64_t want;    /* the earliest time asked for and not yet served, in
                      * nanoseconds since boot, or 0 for none */
    int64_t tick;    /* with `oneshot`, the time of the next tick */
    uint8_t oneshot; /* the timer is one-shot */
    uint8_t lvt;     /* with `oneshot`, the vector its entry was last
                      * written with, 0 before the first */
    uint32_t hold;   /* the passes under way (`timer_wake_hold`): while
                      * nonzero, a time asked for only lowers `held` */
    int64_t held;    /* the earliest time asked for while held, 0 for none */
};

/* The time before a tick's time at which an interrupt still counts as the
 * tick: the timer's rate measured against the reference can make it run a
 * little fast, and an interrupt so early is the tick, not a second one. */
#define HR_SLACK_NS (TICK_NS / 16u)

/* The ticks a processor may fall behind and keep its tick's phase; past
 * them, as after a virtual machine stood still, its tick starts again a
 * tick from the clock. */
#define HR_BEHIND_MAX 4000u

static struct hr_state hr_cpu[NR_CPUS];
/* The vector of the timer's interrupts between ticks, 0 until
 * `apic_hr_init` takes it; the counts of a tick then. */
static int hr_vector;
static uint32_t hr_tick_counts;

void apic_lint_init(const struct acpi_info *a)
{
    uint32_t entry[2], uid = ACPI_NMI_ALL, id = lapic_this_id();
    int i;

    entry[0] = LAPIC_LVT_MASKED;
    entry[1] = LAPIC_LVT_MASKED;
    for (i = 0; i < a->ncpus; i++) {
        if (a->cpus[i].apic_id == id) {
            uid = a->cpus[i].uid;
        }
    }
    for (i = 0; i < a->nnmis; i++) {
        const struct acpi_nmi *n = &a->nmis[i];

        if (n->lint <= 1 && (n->uid == ACPI_NMI_ALL || n->uid == uid)) {
            entry[n->lint] =
                LAPIC_LVT_NMI |
                ((n->flags & ACPI_POLARITY_MASK) == ACPI_POLARITY_LOW ? LAPIC_LVT_ACTIVE_LOW : 0u);
        }
    }
    lapic_set_lint(0, entry[0]);
    lapic_set_lint(1, entry[1]);
}

/**
 * Read the timer between two reads of the reference, and return its count
 * with the reference midway between the two in `*ref`. The pair taken is
 * the first within SAMPLE_CLOSE_US of each other, or the closest of
 * SAMPLE_TRIES, so that a processor stopped between the reads of the
 * reference and of the timer cannot put one a stop's length from the other.
 */
static uint32_t apic_sample(uint32_t *ref)
{
    uint32_t close = refclock_scale(SAMPLE_CLOSE_US, refclock_hz(), 1000000u);
    uint32_t best = 0, count = 0, before, now, gap, i;

    for (i = 0; i < SAMPLE_TRIES && (i == 0 || best > close); i++) {
        before = refclock_read();
        now = lapic_timer_current();
        gap = (refclock_read() - before) & refclock_mask();
        if (i == 0 || gap < best) {
            best = gap;
            count = now;
            *ref = (before + gap / 2u) & refclock_mask();
        }
    }
    return count;
}

/** The timer's counts a second, measured over 20 ms of the reference, or 0
 * when the reference stopped or the timer did not count. */
static uint32_t apic_against_ref(void)
{
    uint32_t want = refclock_scale(CALIBRATE_US, refclock_hz(), 1000000u);
    uint32_t c0, c1, r0 = 0, r1 = 0, ref, spins;

    c0 = apic_sample(&r0);
    for (spins = 0; spins < CALIBRATE_READS; spins++) {
        if (((refclock_read() - r0) & refclock_mask()) >= want) {
            break;
        }
    }
    c1 = apic_sample(&r1);
    ref = (r1 - r0) & refclock_mask();
    if (ref < want || c1 >= c0) {
        return 0;
    }
    return refclock_scale(c0 - c1, refclock_hz(), ref);
}

/** The timer's fewest counts a second in PIT_WINDOWS measured waits of
 * 20 ms on the 8254, or 0 when it or the 8254 did not count. */
static uint32_t apic_against_pit(void)
{
    uint32_t c0, c1, seen, rate, least = 0, i;

    for (i = 0; i < PIT_WINDOWS; i++) {
        c0 = lapic_timer_current();
        seen = pit_wait_counts(PIT_COUNTS_PER_SEC / (1000000u / CALIBRATE_US));
        c1 = lapic_timer_current();
        if (seen != 0 && c1 < c0) {
            rate = refclock_scale(c0 - c1, PIT_COUNTS_PER_SEC, seen);
            if (least == 0 || rate < least) {
                least = rate;
            }
        }
    }
    return least;
}

/**
 * The reference's reads are taken in pairs around the timer's at each end
 * (`apic_sample`), and the 8254's waits are measured and repeated
 * (`apic_against_pit`), so that a processor stopped during the measurement,
 * as a hypervisor stops one, cannot change the result by more than a few
 * microseconds in 20 ms, so long as one of the 8254's waits had no stop. A
 * stop longer than the reference's wrap, 4.7 s for a 24-bit PM timer, is
 * not seen. A rate past 32 bits is no measurement.
 */
uint32_t apic_timer_calibrate(uint32_t hz, const char **against)
{
    uint32_t flags, rate;
    const char *name = refclock_name();

    *against = name ? name : "pit";
    if (hz == 0) {
        return 0;
    }
    flags = kirq_save();
    lapic_timer_set(APIC_TIMER_VECTOR, 0, 1, 0xFFFFFFFFu);
    rate = name ? apic_against_ref() : apic_against_pit();
    lapic_timer_stop();
    kirq_restore(flags);
    return rate == 0xFFFFFFFFu ? 0 : rate / hz;
}

void apic_timer_start(uint32_t counts, uint32_t hz)
{
    struct hr_state *c = &hr_cpu[arch_cpu_id()];

    tick_hz = hz;
    c->oneshot = 0;
    c->want = 0;
    lapic_timer_set(APIC_TIMER_VECTOR, 1, 0, counts);
}

/**
 * Move `c->tick` on past the tick an interrupt has just served: by whole
 * ticks until it is past `now`, as hrtimer_forward moves an expiry past the
 * clock by whole intervals (kernel/time/hrtimer.c:1046-1076), so that a
 * tick missed is skipped and the phase is kept; or by one tick when `now`
 * is short of it, since an interrupt up to HR_SLACK_NS early is that tick.
 * Only the ticks the clock has passed are skipped: the allowance for an
 * early interrupt moves no tick still to come. A processor more than
 * HR_BEHIND_MAX ticks behind has its next tick a tick from `now`.
 */
static void hr_forward(struct hr_state *c, int64_t now)
{
    int64_t gap = now - c->tick;

    if (gap < 0) {
        c->tick += TICK_NS;
        return;
    }
    if (gap >= (int64_t)HR_BEHIND_MAX * TICK_NS) {
        c->tick = now + TICK_NS;
        return;
    }
    c->tick += ((int64_t)((uint32_t)gap / TICK_NS) + 1) * TICK_NS;
}

/**
 * Count this processor's one-shot timer to the earlier of its next tick and
 * the time asked for, from the clock's reading `now`, at the vector of the
 * one it counts to. A time passed is one count away, so that its interrupt
 * comes at once; the counts are rounded up, so that the interrupt never
 * comes before its time by the rounding. The entry is written only when its
 * vector changes. Interrupts are off.
 */
static void hr_program(struct hr_state *c, int64_t now)
{
    int64_t at = c->tick, delta;
    uint8_t vector = APIC_TIMER_VECTOR;
    uint32_t counts;

    if (c->want != 0 && c->want < at) {
        at = c->want;
        vector = (uint8_t)hr_vector;
    }
    delta = at - now;
    if (delta <= 0) {
        counts = 1;
    } else if (delta >= 2 * (int64_t)TICK_NS) {
        counts = 2u * hr_tick_counts;
    } else {
        counts = refclock_scale((uint32_t)delta, hr_tick_counts, TICK_NS) + 1u;
    }
    if (c->lvt != vector) {
        lapic_timer_set(vector, 0, 0, counts);
        c->lvt = vector;
    } else {
        lapic_timer_count(counts);
    }
}

/**
 * Make this processor's periodic timer one-shot, its next tick at the time
 * the periodic count would have raised it, as Linux starts the emulated tick
 * at the tick's next period (kernel/time/tick-sched.c:1590): the counts
 * left, from the time the count was read. That time is the midpoint of two
 * readings of the clock around the read, and the pair taken is the first
 * within SAMPLE_CLOSE_US of each other, or the closest of SAMPLE_TRIES, so
 * that a processor stopped between a reading and the read, as a hypervisor
 * stops one, cannot move the tick's phase for good by the stop's length; it
 * moves it by at most half the pair's gap. A tick raised and not yet taken
 * when the count is read is one whose count reloaded before the read, so
 * the next tick's time is a tick earlier than the count says; the tick
 * taken then has reached it (`apic_hr_entry`) and moves it on. Returns the
 * clock's last reading. Interrupts are off.
 */
static int64_t hr_oneshot(struct hr_state *c)
{
    int64_t before, after = 0, gap, best = -1, at = 0;
    uint32_t cur, count = 0, i;
    int raised, late = 0;

    for (i = 0; i < SAMPLE_TRIES && (best < 0 || best > (int64_t)SAMPLE_CLOSE_US * 1000); i++) {
        before = rtc_monotonic_ns();
        cur = lapic_timer_current();
        raised = lapic_requested(APIC_TIMER_VECTOR);
        after = rtc_monotonic_ns();
        gap = after - before;
        if (best < 0 || gap < best) {
            best = gap;
            count = cur;
            late = raised;
            at = before + gap / 2;
        }
    }
    if (count > hr_tick_counts) {
        count = hr_tick_counts;
    }
    c->tick = at + refclock_scale(count, TICK_NS, hr_tick_counts);
    if (late) {
        c->tick -= TICK_NS;
    }
    c->oneshot = 1;
    c->lvt = 0;
    return after;
}

int timer_wakes_between(void)
{
    return hr_vector != 0 && timer_tick_shared();
}

void timer_wake_at(int64_t ns)
{
    struct hr_state *c;
    uint32_t flags;
    int64_t now;

    if (hr_vector == 0 || !timer_tick_shared() || ns <= 0) {
        return;
    }
    flags = kirq_save();
    c = &hr_cpu[arch_cpu_id()];
    if (c->hold != 0) {
        if (c->held == 0 || ns < c->held) {
            c->held = ns;
        }
    } else if (c->want == 0 || ns < c->want) {
        c->want = ns;
        if (!c->oneshot) {
            now = hr_oneshot(c);
            hr_program(c, now);
        } else if (ns < c->tick) {
            now = rtc_monotonic_ns();
            hr_program(c, now);
        }
    }
    kirq_restore(flags);
}

void timer_wake_hold(void)
{
    hr_cpu[arch_cpu_id()].hold++;
}

void timer_wake_release(void)
{
    struct hr_state *c = &hr_cpu[arch_cpu_id()];
    int64_t ns = c->held;

    if (c->hold == 0 || --c->hold != 0) {
        return;
    }
    c->held = 0;
    if (ns != 0) {
        timer_wake_at(ns);
    }
}

/** The interrupt between ticks, without the big lock (ISR_UNLOCKED): the
 * pass (`timer_hr_pass`), which arms the next times due and takes the lock
 * only by a try, to post the POSIX timers' signals. */
static void hr_interrupt(isr_params_t *params)
{
    (void)params;
    timer_hr_pass();
}

int apic_hr_entry(uint32_t vector)
{
    struct hr_state *c;
    int64_t now;
    int tick = 0;

    if (hr_vector == 0 || (vector != APIC_TIMER_VECTOR && vector != (uint32_t)hr_vector)) {
        return 1;
    }
    c = &hr_cpu[arch_cpu_id()];
    if (!c->oneshot) {
        return 1;
    }
    now = rtc_monotonic_ns();
    if (vector == APIC_TIMER_VECTOR && now + HR_SLACK_NS >= c->tick) {
        hr_forward(c, now);
        tick = 1;
    } else if (vector == (uint32_t)hr_vector && c->want != 0 && c->want <= now) {
        c->want = 0;
    }
    hr_program(c, now);
    return vector == APIC_TIMER_VECTOR ? tick : 1;
}

int apic_hr_vector(void)
{
    return hr_vector;
}

void apic_hr_init(uint32_t counts)
{
    int v = isr_alloc_vector();

    if (v < 0 || counts == 0) {
        return;
    }
    hr_tick_counts = counts;
    set_isr_handler((uint8_t)v, hr_interrupt, "hrtimer");
    isr_register_flags((uint8_t)v, hr_interrupt, ISR_UNLOCKED);
    hr_vector = v;
    klog(KLOG_INFO, "lapic", "timer: vector %d between ticks", v);
}

/** The counts gone since the timer last reloaded, and the initial count. */
static uint32_t apic_elapsed(uint32_t *initial)
{
    uint32_t cur = lapic_timer_current();

    *initial = lapic_timer_initial();
    if (cur > *initial) {
        cur = *initial;
    }
    return *initial - cur;
}

/** The counter's place within the tick, in microseconds: the counts gone
 * as a share of the initial count; 0 while the timer is stopped. */
static uint32_t apic_phase_us(void)
{
    uint32_t initial, gone = apic_elapsed(&initial);

    return initial ? refclock_scale(gone, 1000000u / tick_hz, initial) : 0;
}

/** The counter's place within the tick, in nanoseconds. */
static uint32_t apic_phase_ns(void)
{
    uint32_t initial, gone = apic_elapsed(&initial);

    return initial ? refclock_scale(gone, 1000000000u / tick_hz, initial) : 0;
}

/** The tick's period, in microseconds. */
static uint32_t apic_period_us(void)
{
    return 1000000u / tick_hz;
}

/** Whether the timer's vector has been raised at this processor and not
 * yet taken. */
static int apic_pending(void)
{
    return lapic_requested(APIC_TIMER_VECTOR);
}

/** The 8254's tick through the I/O APIC: raised at the I/O APIC's input or
 * at this processor, and not yet taken. */
static int apic_pit_pending(void)
{
    return lapic_requested(APIC_TIMER_VECTOR) || ioapic_line_pending(0);
}

const struct tick_counter apic_timer_counter = {apic_phase_us, apic_phase_ns, apic_period_us,
                                                apic_pending};
const struct tick_counter apic_pit_counter = {pit_phase_us, pit_phase_ns, pit_period_us,
                                              apic_pit_pending};
