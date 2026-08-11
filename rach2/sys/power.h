/*
 * power.h - how this machine stops.
 *
 * One question with three answers that degrade into each other: park the CPU,
 * ask the platform to remove power, ask the chipset to reset. Only the first
 * always works, so the other two fall back to it and say on the serial line
 * that they did.
 *
 * Nothing here flushes anything or knows what a file system is. The caller
 * decides the machine may stop; this decides how.
 */
#ifndef _POWER_H_
#define _POWER_H_

/* What to ask for. POWER_HALT is the one that cannot fail. */
#define POWER_HALT    0
#define POWER_OFF     1
#define POWER_RESTART 2

/**
 * Stop the machine the way `how` asks. Does not return.
 */
void power_stop(int how);

#endif /* _POWER_H_ */
