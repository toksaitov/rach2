#ifndef _TIMER_H_
#define _TIMER_H_

#include "lib/kint.h"

/* The kernel's tick, counted by its timer handler at the platform's rate. */
extern volatile uint32_t kernel_ticks;

/*
 * The clock under the tick: microseconds since the boot, the place within
 * the tick, and the ticks the timer did not deliver. The platform's timer
 * defines it (arch/x86/pit.c, the 8254's counter, with the time-stamp
 * counter where it runs at one rate); the tick itself is the kernel's
 * (kernel_ticks).
 */

/**
 * Microseconds since the boot, read from the tick and the counter under it.
 *
 * For measuring an interval shorter than the 10 ms tick, which is what the
 * USB interrupt path is. It wraps after about 71 minutes.
 */
uint32_t timer_now_us(void);

/** Whether the tick's interrupt has been raised and not yet taken: the
 * counter has wrapped into a tick the handler has not counted. */
int timer_tick_pending(void);

/** Nanoseconds within this tick. The caller must disable interrupts. */
uint32_t timer_subtick_ns(void);

/**
 * Count one period of the counter, from the timer handler and nowhere else.
 *
 * `timer_now_us` takes its periods from the tick, because two readings a whole
 * period apart are at the same place in the counter's sweep and the wrap
 * between them cannot be seen.
 */
void timer_tick_clock(void);

/**
 * Whether the clock's period accounting is right, on fixed readings: the
 * arithmetic `timer_now_us` and `timer_tick_clock` run, fed readings whose
 * answer is known, so a wrong accounting fails on every machine and a
 * machine a hypervisor stops cannot fail it, since nothing here is read from
 * the timer. The boot's timing record takes its level from this. Nonzero
 * when every answer is right.
 */
int timer_clock_check(void);

/**
 * Measure the processor's cycle counter against the timer, with interrupts
 * off for 20 ms, where the counter runs at one rate whatever the processor's
 * clock does and no hypervisor runs the machine (on a PC, what `cpuid`
 * leaves 0x80000007 and 1 say), so that a tick the timer misses can be made
 * up. Returns the counter's counts in one tick, and 0 otherwise, when no
 * missed tick is made up.
 */
uint32_t timer_calibrate(void);

/**
 * The ticks the timer did not deliver since its last interrupt, by the cycle
 * counter,
 * from the timer handler and nowhere else: the whole ticks of the interval,
 * rounded down, less the one this interrupt delivers, at most a minute's.
 * Zero without a calibration.
 */
uint32_t timer_lost_ticks(void);

#endif /* _TIMER_H_ */
