/*
 * platform.h: the machine's own devices around the processor, which the boot
 * brings up in an order the shared kernel decides (kernel.c).
 */
#ifndef _PLATFORM_H_
#define _PLATFORM_H_

#include "irq.h"

#include "arch/arch.h"
#include "lib/kint.h"

struct process;

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

struct frame_map;

/**
 * Take out of the boot's memory map `map`, before the frame allocator is
 * made from it, the memory the machine's devices use that the firmware's
 * map may not mark: on a PC the stolen memory of Intel's GPU, as Linux's
 * early quirks do. Configuration space is reached by mechanism #1 alone.
 */
void platform_memory_reserve(struct frame_map *map);

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
 * Bring up the interrupt controllers and start the periodic tick, TICK_HZ
 * times a second, with `tick` its handler, and check that it arrives.
 * Interrupts are on when this returns.
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

/**
 * Bring up the firmware's interpreter, on a PC ACPICA (sys/acpi), once the
 * heap, PCI, the tick and the scheduler are up.
 */
void platform_firmware_start(void);

/**
 * Make the platform's kernel threads, on a PC ACPICA's, the wireless
 * worker's and the PS/2 mouse's, once the first program has its pid, so
 * that init is process 1.
 */
void platform_threads_start(void);

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

/** Send processor `cpu` a non-maskable interrupt, which reaches it with its
 * interrupts off: the watchdog's backtrace request (watchdog.c) and the
 * panic's stop of a processor that did not take the interprocessor
 * interrupt (`smp_stop_others`). */
void platform_cpu_nmi(int cpu);

/*
 * The lockup detectors' part of the machine (watchdog.c), which
 * arch/x86/nmiwd.c defines for a PC: the hard-lockup detector's source of
 * NMIs, a performance counter of unhalted core cycles that raises one each
 * `watchdog_thresh` seconds, as Linux's perf event for the NMI watchdog
 * does, and what the reports read of the processor and its frames.
 */

/**
 * Find the counter, once, on the boot processor: an architectural
 * performance monitor of version 1 or later with the core-cycles event
 * (`cpuid` leaf 0xA), or AMD's core counters (leaf 0x80000001, ECX bit
 * 23), seen to count. 1 when there is one; 0 when there is none, as under
 * QEMU's emulator or a hypervisor that gives no PMU, and the detector falls
 * back to the buddy check. The result is logged.
 */
int platform_pmu_probe(void);

/**
 * Program this processor's counter to raise an NMI after each `cycles`
 * unhalted core cycles, its local APIC's counter entry an NMI; called on
 * the processor itself, with interrupts off. A period wider than the
 * counter's writable width is counted in parts (`platform_pmu_overflow`).
 */
void platform_pmu_enable(uint64_t cycles);

/** Stop this processor's counter and mask its APIC entry. */
void platform_pmu_disable(void);

/**
 * In the NMI handler: 1 when this processor's counter raised the NMI, which
 * it acknowledges, loads again and unmasks; 0 otherwise, for an NMI of
 * another source. `*due` is set to 1 when the whole period passed, 0 when a
 * part of a wide one did.
 */
int platform_pmu_overflow(int *due);

/** The processor's clock in cycles a second, as Linux's `cpu_khz` gives it
 * times 1000: the TSC's measured rate, which on Intel's processors since
 * Nehalem is the nominal clock, measured by `platform_pmu_probe` with or
 * without a counter; 0 before it. */
uint64_t platform_cpu_hz(void);

/**
 * Log the registers of the frame `p` at level `level` under `subsystem`, in
 * Linux's `show_regs` layout, and the return addresses of its frames: the
 * kernel has no symbol table, so the call trace gives addresses, which
 * `addr2line -e` of the kernel's image names.
 */
void platform_show_regs(const char *subsystem, int level, const isr_params_t *p);

/** Whether the frame `p` interrupted a processor halted in its idle wait,
 * at the instruction after the halt, as Linux's `cpu_in_idle` asks of the
 * interrupted address. */
int platform_frame_idle(const isr_params_t *p);

/** Log the return addresses of the caller's frames, from its frame pointer
 * up its thread's kernel stack, as Linux's `dump_stack` does where no
 * interrupted frame is at hand. */
void platform_show_stack(const char *subsystem, int level);

/** The instruction and frame pointer at which the blocked thread `t` is
 * stopped in its switch away, from what the switch saved on its stack. */
void platform_blocked_frame(const struct process *t, uintptr_t *pc, uintptr_t *fp);

#endif /* _PLATFORM_H_ */
