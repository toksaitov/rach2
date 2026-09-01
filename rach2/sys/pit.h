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

/**
 * Count one period of the counter, from the timer handler and nowhere else.
 *
 * `pit_now_us` takes its periods from the tick, because two readings a whole
 * period apart are at the same place in the counter's sweep and the wrap
 * between them cannot be seen.
 */
void pit_tick_clock(void);

#endif
