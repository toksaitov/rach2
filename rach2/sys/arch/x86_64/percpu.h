#ifndef _X86_64_PERCPU_H_
#define _X86_64_PERCPU_H_

#include "../arch.h"

#include "../../proc/percpu.h"

/*
 * The x86-64's per-processor state: each processor's record, with its GDT
 * and TSS in it, reached through the GS base.
 *
 * In the kernel, GS's base is the running processor's `struct cpu`, whose
 * first word points at itself, so `cpu_self` is one load from %gs:0. In
 * user mode the base is the program's own, the one arch_prctl's ARCH_SET_GS
 * gives it. The processor keeps the other of the two in the MSR
 * IA32_KERNEL_GS_BASE, and `swapgs` exchanges them: every entry from user
 * mode swaps first and every return to it swaps last (isr_handler.s).
 *
 * The GDT is Linux's x86-64 layout, so a program sees the selectors it
 * would see there: the kernel's code and data at 0x10 and 0x18, the user's
 * data at 0x2B and code at 0x33, and the TSS, two entries long in long
 * mode, at 0x38. `syscall` and `sysret` take the selectors from IA32_STAR
 * and require this order: the kernel's data after its code, and the user's
 * code 16 bytes and its data 8 bytes after the base STAR names, 0x23. The
 * entry at 0x20, where a 32-bit user code segment would be, is empty: no
 * program can enter compatibility mode, and the `syscall` of one, whose
 * entry IA32_CSTAR would name, never happens.
 */

#define GDT_KERNEL_CODE 0x10u
#define GDT_KERNEL_DATA 0x18u
#define GDT_USER_DATA   0x2Bu
#define GDT_USER_CODE   0x33u
#define GDT_TSS         0x38u
#define GDT_ENTRIES     9

/*
 * The 64-bit TSS (Intel SDM Vol. 3A, 8.7): no task state, only the stacks
 * the processor switches to. `rsp0` is the one an interrupt or exception
 * from user mode lands on, the running thread's kernel stack; `ist` are the
 * interrupt stacks the gates of the double fault, the NMI, the machine
 * check and the debug exception name, whatever stack was in use. The I/O
 * map base past the segment's end means every port is closed to user mode.
 */
struct tss64 {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

/* The interrupt stacks, by their number in a gate (idt.c): 1 to 4. */
#define IST_DOUBLE_FAULT  1
#define IST_NMI           2
#define IST_MACHINE_CHECK 3
#define IST_DEBUG         4

/*
 * One processor's record. The first four fields are read at fixed offsets,
 * which percpu.c checks against the compiler's at build time and
 * `arch_context_check` (context.c) at boot: `self` at 0, `kernel_rsp` at 8
 * and `user_rsp` at 16 by the entry code (isr_handler.s), and `id` at 24 by
 * `arch_cpu_id` (cpu.h).
 */
struct cpu {
    struct cpu *self;    /* this record, which %gs:0 reads */
    uint64_t kernel_rsp; /* the top of the running thread's kernel stack */
    uint64_t user_rsp;   /* the user stack pointer at `syscall`, briefly */
    uint32_t id;         /* the processor's index in `cpus` */
    uint64_t gdt[GDT_ENTRIES];
    struct tss64 tss;
};

extern struct cpu cpus[NR_CPUS];

/** The running processor's record. */
static __inline__ struct cpu *cpu_self(void)
{
    struct cpu *c;

    __asm__ __volatile__("movq %%gs:0, %0" : "=r"(c));
    return c;
}

/* The model-specific registers of long mode (Intel SDM Vol. 4, 2.1). */
#define MSR_EFER           0xC0000080u
#define MSR_STAR           0xC0000081u
#define MSR_LSTAR          0xC0000082u
#define MSR_FMASK          0xC0000084u
#define MSR_FS_BASE        0xC0000100u
#define MSR_GS_BASE        0xC0000101u
#define MSR_KERNEL_GS_BASE 0xC0000102u

#define EFER_SCE 0x001u /* syscall and sysret */
#define EFER_LME 0x100u /* long mode */
#define EFER_NXE 0x800u /* the NX bit of page entries */

/** Read and write the model-specific register `msr` of the running
 * processor. */
uint64_t msr_read(uint32_t msr);
void msr_write(uint32_t msr, uint64_t value);

/*
 * Read the selector in FS or GS, and load `sel` into one (switch.s). A load
 * sets the register's base too, to the descriptor's or, for the null
 * selector, to a value that differs between processors, so the caller
 * writes the base's MSR after it. A load into GS leaves the kernel's GS
 * base in place and sets the program's, in IA32_KERNEL_GS_BASE.
 */
unsigned int x86_64_fs_read(void);
unsigned int x86_64_gs_read(void);
void x86_64_fs_load(unsigned int sel);
void x86_64_gs_load(unsigned int sel);

/* Read the selector in DS or ES, and load `sel` into one (switch.s). Long
 * mode ignores both registers' bases, so a load changes nothing else. */
unsigned int x86_64_ds_read(void);
unsigned int x86_64_es_read(void);
void x86_64_ds_load(unsigned int sel);
void x86_64_es_load(unsigned int sel);

/**
 * Fill the boot processor's record, with its GDT and its TSS, load the GDT,
 * and make the record GS's base, with 0 as the program's base in
 * IA32_KERNEL_GS_BASE. Boot only, before `init_tss`.
 */
void init_gdt(void);

/**
 * Make the kernel stack ending at `kernel_stack` the running processor's
 * (`cpu_set_kernel_stack`), load its task register, and turn `syscall` on:
 * IA32_STAR, IA32_LSTAR, IA32_FMASK and EFER.SCE. The processor's GS base
 * must be its record already, and its GDT loaded: the boot processor calls
 * this after `init_gdt`, and a started one from `arch_ap_tables`.
 */
void init_tss(uintptr_t kernel_stack);

/** Build the IDT, program the 8259 pair for vectors 32 to 47 (`init_pic`),
 * and load the IDT on the boot processor (idt.c). Boot only. */
void init_idt(void);

/**
 * Make `top` the kernel stack the next entry from user mode lands on, on
 * the running processor, by an interrupt (the TSS's `rsp0`) or by
 * `syscall` (`kernel_rsp`).
 */
void cpu_set_kernel_stack(uintptr_t top);

#endif /* _X86_64_PERCPU_H_ */
