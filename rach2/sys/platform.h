#ifndef _PLATFORM_H_
#define _PLATFORM_H_

#include "irq.h"

#include "lib/kint.h"

/*
 * The platform: the machine's own devices around the processor, which the
 * boot brings up in an order the shared kernel decides and the platform
 * carries out. arch/x86/platform.c defines it for a PC; a port to another
 * machine defines these and the devices behind them.
 */

/**
 * Make the processor ready to take interrupts and exceptions: on a PC the
 * descriptor tables, the task state segment, whose stack an entry from user
 * mode switches to, with the boot stack ending at `boot_stack_top`, and the
 * interrupt table's handlers.
 */
void platform_cpu_init(uintptr_t boot_stack_top);

/** Report what the firmware left in the machine's devices, before the
 * drivers change it: `rach2.diag`'s full report and the interrupt
 * controller's state. */
void platform_report(void);

/** Show `rach2.diag`'s report once more and wait, before init takes the
 * console. */
void platform_report_hold(void);

/**
 * Start the periodic tick, 100 times a second, with `tick` its handler, and
 * check that it arrives. Interrupts are on when this returns.
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

#endif /* _PLATFORM_H_ */
