#ifndef _PIT_H_
#define _PIT_H_

#include "isr.h"

#include "../../timer.h"

#include "../../lib/kint.h"
#include "../../kio.h"

/*
 * The PC's timer, the 8254 programmable interval timer, and the clock under
 * the tick that ../../timer.h declares. Channel 0 raises the tick on line 0
 * when the 8259 pair has the lines. With the I/O APIC the local APIC's
 * timer ticks, or channel 0 through the I/O APIC when that timer could not
 * be measured, and `timer_set_counter` puts the ticking timer's count under
 * the clock.
 */

/** Program channel 0 to raise line 0 `frequency` times a second, in mode 3,
 * the square wave, and record its reload in `pit_reload`. The caller
 * installs the tick's handler. */
void init_pit(uint32_t frequency);

/*
 * The reload value `init_pit` last programmed into channel 0.
 *
 * `udelay.c` measures against the same counter and needs to know where it
 * wraps, so it reads the reload here rather than computing the divisor a
 * second time, which a change to the tick rate could leave behind. It has
 * the value for TICK_HZ before `init_pit` runs, so that a wait during early
 * boot is measured.
 */
extern uint32_t pit_reload;

/**
 * Whether channel 0 counts: 1 when its count moves within a few thousand
 * readings after `init_pit`, 0 on a machine with no 8254, where the ports
 * read all ones.
 */
int pit_probe(void);

/*
 * The counter under the tick, which the clock of ../../timer.h reads: where
 * it is inside the tick's period, in microseconds and in nanoseconds, the
 * period, and whether the tick has been raised and not yet taken. It is the
 * 8254's channel 0 until the platform chooses another, the local APIC's timer
 * when that is the tick (apictimer.c).
 */
struct tick_counter {
    uint32_t (*phase_us)(void);
    uint32_t (*phase_ns)(void);
    uint32_t (*period_us)(void);
    int (*pending)(void);
};

/** Read the clock from `c`, or from channel 0 again when `c` is NULL. The
 * platform calls it with interrupts off, before the tick starts or when it
 * moves the tick to another timer. */
void timer_set_counter(const struct tick_counter *c);

/* Channel 0's own parts of a counter, for a platform that takes its tick
 * through another controller than the 8259 (apictimer.c). */

/** Where channel 0's counter is inside its period now, in microseconds,
 * from its count and status latched together. */
uint32_t pit_phase_us(void);

/** `pit_phase_us` in nanoseconds, at a microsecond's resolution. */
uint32_t pit_phase_ns(void);

/** Channel 0's period, two sweeps of its counter, in microseconds. */
uint32_t pit_period_us(void);

/* Channel 0's counts a second as it is read in mode 3, where the count
 * falls by two at each tick of the 1.193182 MHz input clock. */
#define PIT_COUNTS_PER_SEC (2u * 1193182u)

/**
 * Sample channel 0 until its count has fallen by `counts` at least, and
 * return the counts it fell from the first sample to the last, which a
 * measurement divides by PIT_COUNTS_PER_SEC. A sweep of the counter that
 * passes between two samples, as when the processor is stopped, is not
 * seen, so the counts returned are never more than the time that passed and
 * may be less. 0 when the counter stops. Defined in udelay.c.
 */
uint32_t pit_wait_counts(uint32_t counts);

/** The TSC's counts in a tick, as `timer_calibrate` measured them, or 0
 * when it was not measured: no invariant TSC, or `rach2.notsc`. */
uint32_t tsc_counts_per_tick(void);

/*
 * The wait `k_udelay` makes instead of counting channel 0, or NULL to count
 * it: set by the platform on a machine with no 8254, where it is the HPET's
 * or the PM timer's (refclock.c). Defined in udelay.c.
 */
extern void (*udelay_wait)(uint32_t usec);

#endif
