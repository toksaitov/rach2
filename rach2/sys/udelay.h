/*
 * udelay.h: the short waits device bring-up sequences require, measured
 * against a hardware counter of known rate.
 */
#ifndef _UDELAY_H_
#define _UDELAY_H_

#include "lib/kint.h"

/**
 * Wait at least `usec` microseconds, spinning.
 *
 * The wait is measured against a counter, since a count of instructions
 * lasts as long as the processor's speed makes it. On a PC the counter is
 * the 8254's channel 0, latched and read without disturbing the tick, and on
 * a machine with no 8254 the HPET or the ACPI PM timer, which the platform
 * selects (`udelay_wait`, arch/x86/pit.h); arch/x86/udelay.c gives the mode
 * 3 counting rule its conversion depends on; once the TSC keeps the clock,
 * the TSC. It needs no interrupts and may run with them off. A wait on the
 * TSC that finds it stopped finishes on one of the others, a wait on the
 * 8254 whose counter stops or whose latch stays held finishes on the HPET or
 * the PM timer, and a counter that stops with none behind it ends the wait
 * early, so it returns on a machine whose counter is absent.
 */
void k_udelay(uint32_t usec);

/**
 * Wait as `k_udelay` does, and return the microseconds the counters
 * measured: `usec` when the wait lasted it, which it does while any counter
 * runs, since a wait on the TSC that finds it stopped finishes on the HPET
 * or the PM timer, and what that leaves on the 8254, and a wait on the 8254
 * whose counter stops or whose latch stays held finishes on the HPET or the
 * PM timer; less only when every counter it can read stopped, and then no
 * more than the time that passed. A caller that bounds a wait by time
 * counts this, not `usec`.
 */
uint32_t k_udelay_measured(uint32_t usec);

#endif /* _UDELAY_H_ */
