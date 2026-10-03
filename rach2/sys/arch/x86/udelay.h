#ifndef _UDELAY_H_
#define _UDELAY_H_

#include "../../lib/kint.h"

/*
 * udelay: a short wait measured against the timer chip, not counted.
 *
 * Device bring-up sequences ask for waits in microseconds and milliseconds,
 * and a wait that is too short is the kind of defect that appears only on one
 * machine at one clock speed. Counting instructions cannot promise a duration
 * on an unknown CPU, so this reads the 8254's own counter, which runs at a
 * fixed 1.193182 MHz whatever the processor does.
 *
 * The counter is read with a latch command, which samples it without
 * disturbing the count, so this neither perturbs the 100 Hz tick nor depends
 * on interrupts being enabled. Long waits are made of repeated shorter ones,
 * for the arithmetic's sake, not the counter's; udelay.c states the
 * mode 3 counting rule the conversion depends on.
 */

/** Wait at least `usec` microseconds. Bounded, and safe with interrupts off. */
void k_udelay(uint32_t usec);

#endif /* _UDELAY_H_ */
