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
#include "../../udelay.h"

#define CALIBRATE_US 20000u

/* A reference that has not counted 20 ms after this many reads has
 * stopped. */
#define CALIBRATE_READS 10000000u

/* The tick rate `apic_timer_start` was given, which the counter's
 * arithmetic divides a second by. */
static uint32_t tick_hz = 100;

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

uint32_t apic_timer_calibrate(uint32_t hz, const char **against)
{
    uint32_t flags, c0, c1, r0, r1 = 0, ref, spins, want = 0;
    const char *name = refclock_name();

    *against = name ? name : "pit";
    flags = kirq_save();
    lapic_timer_set(APIC_TIMER_VECTOR, 0, 1, 0xFFFFFFFFu);
    if (name) {
        want = refclock_scale(CALIBRATE_US, refclock_hz(), 1000000u);
        r0 = refclock_read();
        c0 = lapic_timer_current();
        for (spins = 0; spins < CALIBRATE_READS; spins++) {
            r1 = refclock_read();
            if (((r1 - r0) & refclock_mask()) >= want) {
                break;
            }
        }
        c1 = lapic_timer_current();
        ref = (r1 - r0) & refclock_mask();
    } else {
        c0 = lapic_timer_current();
        k_udelay(CALIBRATE_US);
        c1 = lapic_timer_current();
        ref = 0;
    }
    lapic_timer_stop();
    kirq_restore(flags);
    if (c1 >= c0 || hz == 0) {
        return 0;
    }
    if (name) {
        if (ref < want) {
            return 0; /* the reference stopped */
        }
        /* The timer's counts a second, then a tick's. */
        return refclock_scale(c0 - c1, refclock_hz(), ref) / hz;
    }
    return refclock_scale(c0 - c1, 1000000u / hz, CALIBRATE_US);
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
