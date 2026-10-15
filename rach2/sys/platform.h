/*
 * platform.h: the machine's own devices around the processor, which the boot
 * brings up in an order the shared kernel decides (kernel.c).
 */
#ifndef _PLATFORM_H_
#define _PLATFORM_H_

#include "irq.h"

#include "lib/kint.h"

/*
 * arch/x86/platform.c defines these for a PC, with the processors' start in
 * arch/x86/smpboot.c; a port to another machine defines these and the
 * devices behind them.
 */

/**
 * Make the processor ready to take interrupts and exceptions, and turn
 * interrupts on: on a PC the descriptor tables, the task state segment,
 * whose stack an entry from user mode switches to, with the boot stack
 * ending at `boot_stack_top`, and the interrupt table's handlers. On a PC it
 * first sets up the page attribute table, which gives the framebuffer's
 * mapping write-combining (arch/x86/pat.h).
 */
void platform_cpu_init(uintptr_t boot_stack_top);

struct boot_info;

/**
 * Read the firmware's description of the machine, once paging is on and
 * before the interrupt controllers and the timer start: on a PC, ACPI's
 * tables, from the RSDP `boot` gives or from the BIOS area, SMBIOS's, the
 * counter the timer is measured against, PCI configuration through ECAM
 * where the tables give a window for it, and whether the pages the other
 * processors start in are free memory.
 */
void platform_firmware_init(const struct boot_info *boot);

/** Report what the firmware left in the machine's devices, before the
 * drivers change it: `rach2.diag`'s full report and the interrupt
 * controller's state. */
void platform_report(void);

/** Show `rach2.diag`'s report once more and wait for a key or a byte on the
 * serial line, before init takes the console. */
void platform_report_hold(void);

/**
 * Bring up the interrupt controllers and start the periodic tick, 100 times
 * a second, with `tick` its handler, and check that it arrives. Interrupts
 * are on when this returns.
 */
void platform_timer_start(isr_handler_t tick);

/** The ticks a second `platform_timer_start` set. */
uint32_t platform_timer_hz(void);

/** Probe the disk controllers only this platform has, after the shared
 * ones (on a PC, the ATA channels, after AHCI). */
void platform_disks_probe(void);

struct audio_driver;

/**
 * The sound device only this platform has, which the boot tries after HD
 * Audio and before the speaker (on a PC, the Sound Blaster 16), with its
 * name for the boot's status in `*name`; NULL when it has none.
 */
struct audio_driver *platform_audio_driver(const char **name);

/** Find the machine's power controller, for the power off. */
void platform_board_init(void);

/** Take keystrokes from the platform's own keyboard, which a USB keyboard
 * joins later. */
void platform_keyboard_init(void);

/** Probe the platform's own mouse, unless `rach2.nomouse` says not to. */
void platform_mouse_init(void);

/** Have the platform's mouse start its next packet afresh, when the queue is
 * emptied. */
void platform_mouse_resync(void);

/** The platform's devices' work at each tick, from the timer handler. */
void platform_tick(void);

/**
 * The processors the firmware lists that this kernel may start, the boot
 * processor first and counted, at most `limit`: on a PC, the MADT's enabled
 * processors whose APIC identities the xAPIC reaches, when the local APIC
 * gives each its tick, a reference counter (the HPET or the PM timer) lets
 * the others read the clock, and the start's pages are free; 1 otherwise.
 */
int platform_cpu_count(int limit);

/**
 * Start processor `cpu`, from 1 to one less than `platform_cpu_count`, on
 * the stack ending at `stack_top`. It takes the boot processor's page
 * attribute table and floating-point format, loads its own descriptor
 * tables, enables its local interrupt controller and starts its tick, then
 * calls `entry(cpu)` with interrupts off, which does not return. 0 once it
 * has reached `entry`; -1 when it did not answer in time, and is held reset.
 */
int platform_cpu_start(int cpu, uintptr_t stack_top, void (*entry)(int cpu));

/** Send processor `cpu` the interprocessor interrupt, whose handler is
 * `smp_ipi` (proc/smp.h). */
void platform_cpu_ipi(int cpu);

#endif /* _PLATFORM_H_ */
