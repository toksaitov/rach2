#ifndef _PIT_H_
#define _PIT_H_

#include "isr.h"

#include "lib/kint.h"
#include "kio.h"

/*
 * Initializes the Programmable Interval Timer (PIT) with a specific
 * update frequency.
 */
void init_pit(uint32_t frequency);

/*
 * The reload value `init_pit` last programmed into channel 0.
 *
 * Published because `udelay.c` measures against that same counter and has to
 * know where it wraps, and computing the divisor a second time there would be
 * a copy that a change to the tick rate would silently leave behind. It has
 * the 100 Hz value before `init_pit` runs, so a wait taken during early boot
 * is measured, not undefined.
 */
extern uint32_t pit_reload;

/**
 * Microseconds since the boot, read from the tick and the counter under it.
 *
 * For measuring an interval shorter than the 10 ms tick, which is what the
 * USB interrupt path is. It wraps after about 71 minutes.
 */
uint32_t pit_now_us(void);

/** Nanoseconds within this tick. The caller must disable interrupts. */
uint32_t pit_subtick_ns(void);

/**
 * Count one period of the counter, from the timer handler and nowhere else.
 *
 * `pit_now_us` takes its periods from the tick, because two readings a whole
 * period apart are at the same place in the counter's sweep and the wrap
 * between them cannot be seen.
 */
void pit_tick_clock(void);

/**
 * Whether the clock's period accounting is right, on fixed readings.
 *
 * The same arithmetic `pit_now_us` and `pit_tick_clock` run: the count and
 * status of the 8254 converted to a phase, the period of two sweeps against
 * the reload and the crystal, and the period accounting fed a sequence of
 * phases and ticks whose answer is known. A sweep counted as a period, a
 * period counted twice, or a second sweep read as the first fails it on
 * every machine, and a machine a hypervisor stops cannot fail it, since
 * nothing here is read from the counter. The boot's timing record takes
 * its level from this. Nonzero when every answer is right.
 */
int pit_clock_check(void);

/**
 * Measure the time-stamp counter against the counter of the 8254, with
 * interrupts off for 20 ms, when the processor says its TSC runs at one rate
 * whatever its clock does, which `cpuid` leaf 0x80000007 says in EDX bit 8,
 * and no hypervisor runs the machine, which leaf 1 says in ECX bit 31.
 * Returns the TSC's counts in one tick, and 0 otherwise, when no tick the
 * timer misses is made up.
 */
uint32_t pit_tsc_calibrate(void);

/**
 * The ticks the timer did not deliver since its last interrupt, by the TSC,
 * from the timer handler and nowhere else: the whole ticks of the interval,
 * rounded down, less the one this interrupt delivers, at most a minute's.
 * Zero without a calibration.
 */
uint32_t pit_lost_ticks(void);

#endif
