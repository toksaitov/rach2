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

/** Read the boot's memory map for whether the trampoline's pages are free
 * memory, which the firmware does not use. */
void smpboot_init(const struct boot_info *boot);

/** The local APIC's counts a tick, or 0 when the APIC does not tick
 * (platform.c). */
uint32_t x86_tick_counts(void);

/** A started processor's descriptor tables and stacks, the architecture's
 * part of its start: its own GDT and TSS, with `stack_top` the stack an
 * entry from user mode lands on, the IDT, and on x86-64 the kernel's page
 * tables, GS and `syscall`. */
void arch_ap_tables(int cpu, uintptr_t stack_top);

/** Load the IDT `init_idt` built, on a started processor. */
void idt_load(void);

/** The local APIC identity of processor `cpu`. */
uint32_t smpboot_apic_id(int cpu);

/** The trampoline's C entry, on the processor it started. */
void x86_ap_entry(int cpu);

#endif /* _SMPBOOT_H_ */
