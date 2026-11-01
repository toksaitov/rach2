/*
 * apictimer.c: the local APIC's own inputs when the I/O APIC has the lines,
 * as apictimer.h describes: `LINT0` and `LINT1`, and the timer as the tick.
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

#include "../../lib/kirq.h"
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
    tick_hz = hz;
    lapic_timer_set(APIC_TIMER_VECTOR, 1, 0, counts);
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
