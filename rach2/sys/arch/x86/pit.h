#ifndef _PIT_H_
#define _PIT_H_

#include "isr.h"

#include "../../timer.h"

#include "../../lib/kint.h"
#include "../../kio.h"

/*
 * The PC's timer, the 8254 programmable interval timer: channel 0 raises the
 * tick, and its counter under the tick is the clock ../../timer.h reads.
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

#endif
