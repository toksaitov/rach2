/*
 * power.h: how the machine stops, by a halt, a power off or a restart. The
 * platform defines it (arch/x86/power.c on a PC).
 */
#ifndef _POWER_H_
#define _POWER_H_

/* The ways to stop. POWER_HALT is the one that cannot fail. */
#define POWER_HALT    0
#define POWER_OFF     1
#define POWER_RESTART 2

/**
 * Stop the machine in the way `how` gives. Does not return.
 *
 * Interrupts go off and the other processors are stopped first, and the
 * screen is set to show the records from `KLOG_INFO` up. A power off
 * requests the soft-off state: on a PC by ACPI's \_S5 sleep type, then
 * through the power controller the board profile found (arch/x86/board.c),
 * or else a PIIX4's. A restart requests a reset: on a PC by the FADT's
 * reset register, the Insyde firmware's own command, the chipset's reset
 * port 0xCF9 and the keyboard controller's pulse, in that order, where
 * `rach2.resetskip=N` leaves out the first N before the pulse. No such
 * request has a completion signal, so each is given a while to act before
 * the next. A machine still running after them all says so in the log; it,
 * and a machine asked to halt, logs `halt: system halted` and parks the
 * processor with interrupts off for good.
 *
 * Nothing here flushes a cache or unmounts a file system; the caller does
 * that before it decides the machine may stop.
 */
void power_stop(int how);

/* The table of the writes and readings the reset makes, defined and used in
 * arch/x86/power.c; no function outside it takes one. */
struct power_io;

#endif /* _POWER_H_ */
