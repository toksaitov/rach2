#ifndef _APICTIMER_H_
#define _APICTIMER_H_

#include "acpi.h"
#include "pit.h"

#include "../../lib/kint.h"

/*
 * apictimer: the local APIC's own inputs when the I/O APIC takes the
 * machine's lines: `LINT0` and `LINT1`, and the timer as the tick.
 *
 * The timer counts at a rate no register states, a divided bus clock, so it
 * is measured at boot against a counter whose rate is known: the HPET, the
 * ACPI PM timer when there is no HPET, or the 8254 when there is neither.
 * Everything here works on the running processor's own local APIC, so each
 * processor sets up its own.
 */

/**
 * Write this processor's `LINT0` and `LINT1`: each masked, but where the
 * MADT gives it as this processor's NMI input, which it then delivers with
 * the polarity the MADT says. Masking `LINT0` ends the 8259's interrupts,
 * which the firmware sends through it as external interrupts.
 */
void apic_lint_init(const struct acpi_info *a);

/* The tick's vector, the one the 8254's line 0 has through the 8259. */
#define APIC_TIMER_VECTOR 32

/**
 * Measure this processor's timer over 20 ms of the reference counter
 * (refclock.h), or of the 8254 through `k_udelay` when there is none, with
 * interrupts off and the timer masked, and leave the timer stopped: its
 * counts in one tick at `hz` ticks a second, and the reference's name, or
 * "pit", in `*against`. 0 when the reference stopped, the timer did not
 * count, or `hz` is 0.
 */
uint32_t apic_timer_calibrate(uint32_t hz, const char **against);

/** Start this processor's timer, periodic, raising `APIC_TIMER_VECTOR`
 * every `counts` counts, which is a tick at `hz`. */
void apic_timer_start(uint32_t counts, uint32_t hz);

/* The clock's counters under a tick that is not the 8259's: the APIC timer
 * itself, and the 8254 through the I/O APIC. */
extern const struct tick_counter apic_timer_counter;
extern const struct tick_counter apic_pit_counter;

#endif /* _APICTIMER_H_ */
