/*
 * power.h: how the machine stops, by a halt, a power off or a restart. The
 * platform defines it (arch/x86/power.c on a PC).
 */
#ifndef _POWER_H_
#define _POWER_H_

#include "lib/kdef.h"

/* The ways to stop. POWER_HALT is the one that cannot fail. */
#define POWER_HALT    0
#define POWER_OFF     1
#define POWER_RESTART 2

/**
 * Prepare to stop in the way `how` gives, while processes still run: on a
 * PC with ACPICA up, a power off runs the firmware's `\_PTS(5)`, whose
 * methods may wait. The caller then stops the machine with `power_stop`.
 */
void power_prepare(int how);

/**
 * Stop the machine in the way `how` gives. Does not return.
 *
 * Interrupts go off and the other processors are stopped first, and the
 * screen is set to show the records from `KLOG_INFO` up. A power off
 * requests the soft-off state: on a PC through ACPICA where
 * `power_prepare` prepared it, then by writing ACPI's \_S5 sleep type
 * itself, then through the power controller the board profile found
 * (arch/x86/board.c).
 * A restart requests a reset: on a PC with `reboot=` on the boot line, by
 * the sequence `power_emergency_restart` runs, as Linux's restart runs it;
 * without, by the FADT's reset register, the Insyde firmware's own command,
 * the chipset's reset port 0xCF9 and the keyboard controller's pulse, in
 * that order, where `rach2.resetskip=N` leaves out the first N before the
 * pulse. No such request has a completion signal, so each is given a while
 * to act before the next. A machine still running after them all says so in
 * the log; it, and a machine asked to halt, logs `halt: system halted` and
 * parks the processor with interrupts off for good.
 *
 * Nothing here flushes a cache or unmounts a file system; the caller does
 * that before it decides the machine may stop.
 */
void power_stop(int how);

/**
 * Reset the machine at once, with nothing shut down, as Linux's
 * `emergency_restart` does on a PC (arch/x86/kernel/reboot.c,
 * `native_machine_emergency_restart`). Does not return. For a panic, which
 * has stopped the other processors and turned interrupts off already.
 *
 * The methods run in Linux's order, from the one `reboot=` names, `a` when
 * it names none: the FADT's reset register (a), the keyboard controller's
 * pulse ten times (k), the register and the pulse again when the first was
 * the register, EFI's ResetSystem (e), where the kernel booted by UEFI
 * firmware of its own word size, the BIOS's reset entry in real mode (b),
 * port 0xCF9 (p), and a triple fault (t), after which the pulse comes round
 * again, for ever. `reboot=w` or `reboot=panic_warm` asks for a warm reset,
 * which the BIOS flag at 0x472, ResetSystem's reset type and the value
 * written to 0xCF9 carry, and any other mode a cold one.
 * Each method logs `reboot: emergency: METHOD` on its first try.
 */
void power_emergency_restart(void) K_NORETURN;

#endif /* _POWER_H_ */
