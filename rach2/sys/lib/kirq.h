#ifndef _KIRQ_H_
#define _KIRQ_H_

#include "kint.h"

/*
 * Interrupts off for a short region, and back to however they were.
 *
 * The pair is written this way rather than as `cli` and `sti` because a caller
 * does not always know whether interrupts were on when it was entered: an
 * interrupt handler that ended with `sti` would return to its interrupted code
 * with interrupts on that were off before, and the next fault would be a
 * puzzle. Saving the flags and putting them back makes the region safe to
 * enter from either side.
 *
 * The region between the two must be short and must not sleep. Nothing here
 * nests a counter: two brackets around the same region simply restore twice,
 * innermost first, which is what the flags say.
 */

/** Read the flags, then disable interrupts. */
uint32_t kirq_save(void);

/** Put the flags back as `kirq_save` found them. */
void kirq_restore(uint32_t flags);

/** Whether interrupts are on where the caller runs. */
int kirq_enabled(void);

#endif /* _KIRQ_H_ */
