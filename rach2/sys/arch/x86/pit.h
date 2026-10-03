#ifndef _PIT_H_
#define _PIT_H_

#include "isr.h"

#include "../../timer.h"

#include "../../lib/kint.h"
#include "../../kio.h"

/*
 * The PC's timer, the 8254 programmable interval timer: channel 0 raises the
 * tick when the 8259 pair has the lines, and its counter under the tick is
 * then the clock ../../timer.h reads. With the I/O APIC the local APIC's
 * timer ticks, and `timer_set_counter` puts its count under the clock.
 *
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

/** Read the clock from `c`, or from channel 0 again when `c` is NULL.
 * Called before the tick starts. */
void timer_set_counter(const struct tick_counter *c);

/* Channel 0's own part of a counter, for a platform that takes its tick
 * through another controller than the 8259. */
uint32_t pit_phase_us(void);
uint32_t pit_phase_ns(void);
uint32_t pit_period_us(void);

/*
 * The wait `k_udelay` makes instead of counting channel 0, or NULL to count
 * it: set by the platform on a machine with no 8254, where it is the HPET's
 * or the PM timer's (refclock.c). Defined in udelay.c.
 */
extern void (*udelay_wait)(uint32_t usec);

#endif
