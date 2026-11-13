/* The local APIC timer: periodic ticks at boot, then one-shot ticks and
 * events on one vector, with scheduler ticks stopped while idle. */

#include "apictimer.h"
#include "ioapic.h"
#include "lapic.h"
#include "refclock.h"
#include "kcpu.h"
#include "tsc.h"

#include "../arch.h"
#include "../../irq.h"
#include "../../rtc.h"
#include "../../lib/kirq.h"
#include "../../klog.h"
#include "../../proc/percpu.h"
#include "../../timer.h"
#include "../../watchdog.h"

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
/* Scheduler periods are separate from the millisecond clock units. */

/* The one-shot timer keeps the next scheduler tick and the earliest event
 * on one vector. Its entry serves both when they coincide. Tick phase moves
 * by whole periods, including across idle, and an early event takes no
 * future tick. Each processor changes only its own state with IRQs off. */
struct hr_state {
    int64_t want;       /* the earliest time asked for and not yet served, in
                         * nanoseconds since boot, or 0 for none */
    int64_t tick;       /* with `oneshot`, the time of the next tick */
    int64_t due;        /* the time the last tick the clock passed was due,
                         * as the tick being taken found it, or 0 */
    uint64_t want_tick; /* whole-tick tag of want, zero for a direct time */
    uint64_t held_tick; /* the corresponding tag of held */
    int64_t programmed; /* device expiry, 0 when stopped, -1 before one-shot */
    uint8_t oneshot;    /* the timer is one-shot */
    uint8_t idle;       /* the scheduler tick is stopped during an idle wait */
    uint8_t lvt;        /* with `oneshot`, the vector its entry was last
                         * written with, 0 before the first */
    uint32_t hold;      /* the passes under way (`timer_wake_hold`): while
                         * nonzero, a time asked for only lowers `held` */
    int64_t held;       /* the earliest time asked for while held, 0 for none */
};

/* The time before a tick's time at which an interrupt still counts as the
 * tick: the timer's rate measured against the reference can make it run a
 * little fast, and an interrupt so early is the tick, not a second one. */
#define HR_SLACK_NS 62500u

/* The ticks a processor may fall behind and keep its tick's phase; past
 * them, as after a virtual machine stood still, its tick starts again a
 * common tick edge. */
#define HR_BEHIND_MAX (TICK_HZ * 4u)

static struct hr_state hr_cpu[NR_CPUS];
/* The vector of the timer's interrupts between ticks, 0 until
 * `apic_hr_init` takes it; the counts of a tick then. */
static int hr_vector;
static uint32_t hr_tick_counts;
static int hr_deadline;

/** Write this processor's TSC deadline, ordering the LVT's write before
 * the MSR as Intel's local APIC requires in xAPIC mode. Interrupts are off. */
static void hr_set_deadline(uint64_t at)
{
#ifndef RACH2_HOSTTEST
    __asm__ __volatile__("mfence" ::: "memory");
#endif
    kcpu_wrmsr(0x6E0u, (uint32_t)at, (uint32_t)(at >> 32));
}

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
    c->idle = 0;
    c->due = 0;
    c->lvt = 0;
    c->want = 0;
    c->want_tick = 0;
    c->held_tick = 0;
    c->programmed = -1;
    lapic_timer_set(APIC_TIMER_VECTOR, 1, 0, counts);
}

/** Return the first monotonic scheduler edge strictly after `now`,
 * a nonnegative nanosecond time. Every CPU uses this common tick phase. */
static int64_t hr_next_tick(int64_t now)
{
    uint64_t rem;

    (void)tick_div64((uint64_t)now, TICK_NS, &rem);
    return now + TICK_NS - (int64_t)rem;
}

/**
 * Move `c->tick` on past the tick an interrupt has just served: by whole
 * ticks until it is past `now`, as hrtimer_forward moves an expiry past the
 * clock by whole intervals (kernel/time/hrtimer.c:1046-1076), so that a
 * tick missed is skipped and the phase is kept; or by one tick when `now`
 * is short of it, since an interrupt up to HR_SLACK_NS early is that tick.
 * Only the ticks the clock has passed are skipped: the allowance for an
 * early interrupt moves no tick still to come. A processor more than
 * four seconds behind resumes at the next common scheduler edge.
 */
static void hr_forward(struct hr_state *c, int64_t now)
{
    int64_t gap = now - c->tick;

    if (gap < 0) {
        c->tick += TICK_NS;
        return;
    }
    if (gap >= (int64_t)HR_BEHIND_MAX * TICK_NS) {
        c->tick = hr_next_tick(now);
        return;
    }
    c->tick += ((int64_t)((uint32_t)gap / TICK_NS) + 1) * TICK_NS;
}

/**
 * Convert a nanosecond interval to counts at `rate` counts per tick,
 * rounded up, at least one and at most ten seconds. Whole ticks and a
 * fraction are converted apart so their product cannot overflow even at
 * the largest rate, and short intervals need only 32-bit division.
 */
static uint64_t hr_counts(int64_t delta, uint32_t rate)
{
    uint64_t ticks, rem;

    if (delta <= 0) {
        return 1;
    }
    if (delta > (int64_t)10 * 1000000000) {
        delta = (int64_t)10 * 1000000000;
    }
    if ((uint64_t)delta <= 0xFFFFFFFFu) {
        ticks = (uint32_t)delta / TICK_NS;
        rem = (uint32_t)delta % TICK_NS;
    } else {
        ticks = tick_div64((uint64_t)delta, TICK_NS, &rem);
    }
    return ticks * rate + refclock_scale((uint32_t)rem, rate, TICK_NS) + 1u;
}

/** Program the earlier event once, keeping the LVT until its vector changes.
 * The caller has interrupts off; a zero idle expiry stops the device.
 * Idle callbacks retain requests until final preparation (`idle_commit`)
 * or selection of a non-idle thread. The hardware cache is kept meanwhile. */
static void hr_program(struct hr_state *c, int64_t now, int idle_commit)
{
    int64_t at = c->idle ? 0 : c->tick, delta;
    uint8_t vector = APIC_TIMER_VECTOR;
    uint32_t counts, rate;
    uint64_t span;
    struct percpu *cpu = this_cpu();

    if (!idle_commit &&
        (c->idle || (cpu->current != NULL && cpu->current == cpu->idle && cpu->idle_wait))) {
        return;
    }
    if (c->want != 0 && (at == 0 || c->want < at)) {
        at = c->want;
    }
    if (at == 0) {
        if (c->programmed == 0) {
            return;
        }
        if (hr_deadline) {
            if (c->lvt == 0) {
                lapic_timer_deadline(APIC_TIMER_VECTOR);
                c->lvt = APIC_TIMER_VECTOR;
            }
            hr_set_deadline(0);
        } else {
            lapic_timer_count(0);
        }
        c->programmed = 0;
        return;
    }
    if (c->programmed == at && c->lvt == vector) {
        return;
    }
    c->programmed = at;
    delta = at - now;
    if (hr_deadline) {
        rate = timer_calibrate();
        span = hr_counts(delta, rate);
        if (c->lvt != vector) {
            lapic_timer_deadline(vector);
            c->lvt = vector;
        }
        hr_set_deadline(tsc_ordered() + span);
        return;
    }
    span = hr_counts(delta, hr_tick_counts);
    counts = span >= 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)span;
    if (c->lvt != vector) {
        lapic_timer_oneshot(vector, counts);
        c->lvt = vector;

    } else {
        lapic_timer_count(counts);
    }
}

/** Start one-shot mode on the common monotonic scheduler phase. A
 * pending periodic tick uses the preceding edge and is served once by the
 * entry. The pending bit is sampled between clock readings, taking the
 * first close pair or the closest available. IRQs are off; return the last
 * clock reading for programming the device. */
static int64_t hr_oneshot(struct hr_state *c)
{
    int64_t before, after = 0, gap, best = -1, at = 0;
    uint32_t i;
    int raised, late = 0;

    for (i = 0; i < SAMPLE_TRIES && (best < 0 || best > (int64_t)SAMPLE_CLOSE_US * 1000); i++) {
        before = rtc_monotonic_ns();
        raised = lapic_requested(APIC_TIMER_VECTOR);
        after = rtc_monotonic_ns();
        gap = after - before;
        if (best < 0 || gap < best) {
            best = gap;
            late = raised;
            at = before + gap / 2;
        }
    }
    c->tick = hr_next_tick(at);
    if (late) {
        c->tick -= TICK_NS;
    }
    c->oneshot = 1;
    c->lvt = 0;
    c->programmed = -1;
    return after;
}

int timer_wakes_between(void)
{
    return hr_vector != 0 && timer_tick_shared();
}

/** Keep an absolute request and its whole-tick tag, zero for a direct
 * time. Each winning pair moves together.
 * The caller has interrupts off. */
static void hr_wake_at(int64_t ns, uint64_t tick)
{
    struct hr_state *c = &hr_cpu[arch_cpu_id()];
    int64_t now;

    if (c->hold != 0) {
        if (c->held == 0 || ns < c->held) {
            c->held = ns;
            c->held_tick = tick;
        }
    } else if (c->want == 0 || ns < c->want) {
        c->want = ns;
        c->want_tick = tick;
        if (!c->oneshot) {
            now = hr_oneshot(c);
            hr_program(c, now, 0);
        } else if (c->idle || ns < c->tick) {
            now = rtc_monotonic_ns();
            hr_program(c, now, 0);
        }
    }
}

void timer_wake_at(int64_t ns)
{
    uint32_t flags;

    if (!timer_wakes_between() || ns <= 0) {
        return;
    }
    flags = kirq_save();
    hr_wake_at(ns, 0);
    kirq_restore(flags);
}

/** Convert `tick` from a phase sampled between clock readings, returning
 * whether it is already passed in `past`. The closest pair bounds the
 * error if the host stops the CPU between its readings. IRQs are off. */
static int64_t hr_tick_when(uint64_t tick, int *past)
{
    uint32_t phase, i;
    uint64_t now, left;
    int64_t before, after, gap, best = -1, at = 0;

    for (i = 0; i < SAMPLE_TRIES && (best < 0 || best > (int64_t)SAMPLE_CLOSE_US * 1000); i++) {
        before = rtc_monotonic_ns();
        rtc_tick_phase(&now, &phase);
        after = rtc_monotonic_ns();
        gap = after - before;
        if (best < 0 || gap < best) {
            best = gap;
            left = tick > now ? tick - now : 0;
            if (left > (uint64_t)TICK_HZ * 60u) {
                left = (uint64_t)TICK_HZ * 60u;
            }
            *past = left == 0;
            at = before + gap / 2 + (int64_t)left * TICK_NS - phase;
        }
    }
    return at;
}

void timer_wake_tick(uint64_t tick)
{
    struct hr_state *c;
    uint32_t flags;
    int64_t at;
    int past = 0;

    if (tick == 0 || !timer_wakes_between()) {
        return;
    }
    flags = kirq_save();
    c = &hr_cpu[arch_cpu_id()];
    if ((c->want != 0 && c->want_tick == tick) ||
        (c->hold != 0 && c->held != 0 && c->held_tick == tick)) {
        kirq_restore(flags);
        return;
    }
    at = hr_tick_when(tick, &past);
    if (past && tick > tick_count()) {
        int pending;
        uint32_t raw_phase, seq;

        do {
            seq = tick_read_begin();
            timer_phase(&pending, &raw_phase);
        } while (tick_read_retry(seq));
        if (pending == 0) {
            at = rtc_monotonic_ns() + (int64_t)rtc_slewed(TICK_NS - raw_phase, rtc_mono_rate()) + 1;
        }
    }
    hr_wake_at(at > 0 ? at : 1, tick);
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
    uint64_t tick = c->held_tick;

    if (c->hold == 0 || --c->hold != 0) {
        return;
    }
    c->held = 0;
    c->held_tick = 0;
    if (ns != 0) {
        hr_wake_at(ns, tick);
    }
}

/** The interrupt between ticks, without the big lock (ISR_UNLOCKED): the
 * pass (`timer_hr_pass`), which arms the next times due and takes the lock
 * only by a try, to post the POSIX timers' signals. */
void apic_hr_interrupt(isr_params_t *params)
{
    if (hr_cpu[arch_cpu_id()].idle) {
        watchdog_event(params);
    }
    timer_hr_pass();
    hr_program(&hr_cpu[arch_cpu_id()], rtc_monotonic_ns(), 0);
}

int apic_hr_entry(uint32_t vector)
{
    struct hr_state *c;
    int64_t now, programmed;
    int result = 0;

    if (hr_vector == 0 || vector != APIC_TIMER_VECTOR) {
        return APIC_HR_TICK;
    }
    c = &hr_cpu[arch_cpu_id()];
    if (!c->oneshot) {
        return APIC_HR_TICK;
    }
    programmed = c->programmed;
    c->programmed = 0;
    now = rtc_monotonic_ns();
    if (!c->idle && (now >= c->tick || (programmed == c->tick && now + HR_SLACK_NS >= c->tick))) {
        c->due = c->tick;
        hr_forward(c, now);
        if (c->tick - (int64_t)TICK_NS > c->due) {
            c->due = c->tick - (int64_t)TICK_NS;
        }
        result = APIC_HR_TICK;
    }
    if (c->want != 0 && c->want <= now) {
        c->want = 0;
        c->want_tick = 0;
        result |= APIC_HR_EVENT;
    }
    if ((result & APIC_HR_TICK) != 0 || (c->idle && c->want == 0 && result == 0)) {
        hr_program(c, now, 0);
    } else {
        result |= APIC_HR_EVENT;
    }
    return result;
}

/** Return the current scheduler period in kernel tick units. */
uint32_t timer_tick_period(void)
{
    return 1u;
}

int64_t timer_tick_due(void)
{
    return hr_vector != 0 ? hr_cpu[arch_cpu_id()].due : 0;
}

int apic_hr_vector(void)
{
    return hr_vector;
}

/** Prepare an idle event once after its queries, or keep the current
 * state when ready work makes the caller return straight to selection. */
int timer_idle_enter(void)
{
    struct hr_state *c = &hr_cpu[arch_cpu_id()];
    int64_t now;
    int prepared;

    if (!timer_wakes_between()) {
        timer_idle_exit();
        return 0;
    }
    timer_wake_hold();
    prepared = timer_idle_prepare();
    if (prepared <= 0) {
        timer_wake_release();
        if (prepared == 0) {
            timer_idle_exit();
        }
        return c->idle;
    }
    now = c->oneshot ? rtc_monotonic_ns() : hr_oneshot(c);
    c->idle = 1;
    timer_wake_release();
    hr_program(c, now, 1);
    return 1;
}

void timer_idle_exit(void)
{
    struct hr_state *c = &hr_cpu[arch_cpu_id()];
    int64_t now;

    if (!c->oneshot) {
        return;
    }
    now = rtc_monotonic_ns();
    c->idle = 0;
    if (c->tick <= now) {
        hr_forward(c, now);
    }
    hr_program(c, now, 1);
}

/** Whether desired hardware differs from the installed local event.
 * Interrupts are off. */
int timer_idle_pending(void)
{
    struct hr_state *c = &hr_cpu[arch_cpu_id()];
    int64_t at = c->idle ? 0 : c->tick;

    if (!c->oneshot) {
        return 0;
    }
    if (c->want != 0 && (at == 0 || c->want < at)) {
        at = c->want;
    }
    return c->programmed != at || (at != 0 && c->lvt != APIC_TIMER_VECTOR);
}

/** Commit the retained idle timer before halting, with IRQs off. */
void timer_idle_retain(void)
{
    struct hr_state *c = &hr_cpu[arch_cpu_id()];

    if (c->oneshot) {
        hr_program(c, rtc_monotonic_ns(), 1);
    }
}

void apic_hr_init(uint32_t counts)
{
    uint32_t a, b, c, d;

    if (counts == 0) {
        return;
    }
    hr_tick_counts = counts;
    kcpu_id(1, &a, &b, &c, &d);
    hr_deadline = (c & (1u << 24)) != 0 && timer_calibrate() != 0;
    hr_vector = APIC_TIMER_VECTOR;
    klog(KLOG_INFO, "lapic", "timer: vector %d for ticks and events", hr_vector);
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
