#ifndef _SMPBOOT_H_
#define _SMPBOOT_H_

#include "../../boot.h"
#include "../../lib/kint.h"

/*
 * smpboot: the start of the processors past the first on a PC.
 *
 * A processor the boot one starts begins in real mode at the address its
 * startup message names, a page below 1 MiB, which this kernel fixes at
 * TRAMPOLINE_PHYS. The architecture's trampoline.s is copied there: it
 * enters protected mode through a GDT of its own, takes the boot
 * processor's CR4, page tables, EFER and CR0, enters long mode on x86-64
 * through the PML4 at TRAMPOLINE_PML4_PHYS, and calls `x86_ap_entry` on the
 * stack the mailbox at its end names.
 */

#define TRAMPOLINE_PHYS      0x8000u
#define TRAMPOLINE_PML4_PHYS 0x9000u

/*
 * What the trampoline reads, at `ap_mailbox` in its copy: the boot
 * processor's control registers and EFER, the processor's index, its stack
 * and the entry. The layout is the assembly's, at fixed offsets, which
 * smpboot.c checks against the compiler's.
 */
struct ap_mailbox {
    uint32_t cr0;
    uint32_t cr3; /* the page tables until the entry loads the kernel's */
    uint32_t cr4;
    uint32_t efer_lo;
    uint32_t efer_hi;
    uint32_t cpu;
    uint64_t stack;
    uint64_t entry;
};

/* The trampoline (the architecture's trampoline.s): its first byte, one past
 * its last, and its mailbox. */
extern const char ap_trampoline[];
extern const char ap_trampoline_end[];
extern const char ap_mailbox[];

/* The BIOS restart (the architecture's trampoline.s): its first byte and
 * one past its last. */
extern const char bios_restart[];
extern const char bios_restart_end[];

/** Read the boot's memory map for whether the trampoline's pages lie in
 * memory the firmware gives as available and in nothing it gives as
 * anything else; without them `platform_cpu_count` keeps the machine on one
 * processor. Called from `platform_firmware_init`. */
void smpboot_init(const struct boot_info *boot);

/** Whether `smpboot_bios_restart` has the trampoline's page below 1 MiB,
 * which it needs: 1 when the firmware's map gives the page as available. */
int smpboot_bios_restart_ready(void);

/**
 * Restart the machine through the BIOS's reset entry, as Linux's
 * `machine_real_restart(MRR_BIOS)` does (arch/x86/kernel/reboot.c:97-131):
 * copy the architecture's `bios_restart` to TRAMPOLINE_PHYS, load the
 * trampoline's page tables, which map that page at its physical address,
 * and jump there, where the code leaves paging and protected mode and
 * jumps to F000:FFF0. Interrupts must be off and the other processors
 * stopped, and the caller writes CMOS's shutdown status first. Returns
 * only when the page is not free (`smpboot_bios_restart_ready`).
 */
void smpboot_bios_restart(void);

/** The local APIC's counts a tick, or 0 when the APIC does not tick
 * (platform.c). */
uint32_t x86_tick_counts(void);

/** Give processor `cpu` what it needs from the heap before it starts, which
 * the boot processor does from `platform_cpu_start`: on x86-64 its interrupt
 * stacks, which a second call keeps. 0, or -1 when the heap has none. */
int arch_ap_prepare(int cpu);

/** A started processor's descriptor tables and stacks, the architecture's
 * part of its start: its own GDT and TSS, with `stack_top` the stack an
 * entry from user mode lands on, the IDT, and on x86-64 the kernel's page
 * tables, GS and `syscall`. */
void arch_ap_tables(int cpu, uintptr_t stack_top);

/** Load the IDT `init_idt` built on the running processor: one the boot
 * processor started, and on x86-64 the boot processor too, from
 * `init_idt`. */
void idt_load(void);

/** The local APIC identity of processor `cpu`, as `platform_cpu_count`
 * numbered them. */
uint32_t smpboot_apic_id(int cpu);

/**
 * The trampoline's C entry, on the processor it started, with interrupts
 * off and the boot processor waiting in `platform_cpu_start`: the
 * processor's page attribute table, tables, floating point, APIC and tick,
 * then the entry `platform_cpu_start` was given. Does not return.
 */
void x86_ap_entry(int cpu);

#endif /* _SMPBOOT_H_ */
