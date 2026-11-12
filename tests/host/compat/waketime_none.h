/* Unmeasured timed wakes in models that test selection or timer ownership. */
#ifndef _HOST_WAKETIME_NONE_H_
#define _HOST_WAKETIME_NONE_H_

#include "proc/waketime.h"

/** These models have no clock from an interrupt to a user return. */
uint32_t waketime_clock(void)
{
    return 0;
}

/** Leave an expiry's ownership test independent of timing. */
void waketime_expire(struct process *p, uint64_t due)
{
    (void)p;
    (void)due;
}

/** The same for an expiry between ticks. */
void waketime_expire_late(struct process *p, int64_t late_ns)
{
    (void)p;
    (void)late_ns;
}

/** Leave queue selection independent of elapsed time. */
void waketime_state(struct process *p, unsigned int phase, int cpu)
{
    (void)p;
    (void)phase;
    (void)cpu;
}

/** No user-mode return is executed in these models. */
void waketime_return(void)
{}

/** The modeled lock has no elapsed wait. */
void waketime_lock(uint32_t waited)
{
    (void)waited;
}

#endif
