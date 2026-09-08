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
 *
 * Which chipset the power-off talks to is `board.c`'s answer, decided once at
 * boot and refused unless four checks pass; a machine no profile matches keeps
 * the PIIX4 path this file has always had, and failing that, the halt.
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

/*
 * The reset's own writes, so a host test can see which ports the decision
 * reaches. The machine's binding is the real one; NULL restores it.
 */
struct power_io;
void power_set_io(const struct power_io *ops);

#endif /* _POWER_H_ */
