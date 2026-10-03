#ifndef _X86_64_PERCPU_H_
#define _X86_64_PERCPU_H_

#include "../arch.h"

/*
 * The x86-64's per-processor state: each processor's record, with its GDT
 * and TSS in it, reached through the GS base.
 *
 * In the kernel, GS's base is the running processor's `struct cpu`, whose
 * first word points at itself, so `this_cpu` is one load from %gs:0. In
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
 * interrupt stacks the gates of the double fault, the NMI and the machine
 * check name, which they get whatever stack was in use. The I/O map base
 * past the segment's end means every port is closed to user mode.
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

/* The interrupt stacks, by their number in a gate (idt.c): 1 to 3. */
#define IST_DOUBLE_FAULT  1
#define IST_NMI           2
#define IST_MACHINE_CHECK 3

/*
 * One processor's record. The first three fields are read by the entry
 * code at fixed offsets (isr_handler.s), which percpu.c checks against the
 * compiler's at build time: `self` at 0, `kernel_rsp` at 8, `user_rsp` at
 * 16.
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
static __inline__ struct cpu *this_cpu(void)
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

uint64_t msr_read(uint32_t msr);
void msr_write(uint32_t msr, uint64_t value);

/**
 * Make the boot processor's record, GDT and TSS, load them, and point GS at
 * the record: what `init_gdt` and `init_tss` do on i686, in one step here,
 * since the TSS is an entry of the record's own GDT.
 */
void init_gdt(void);

/** Give the boot processor's TSS its stacks, the kernel stack ending at
 * `kernel_stack` among them, load the task register, and turn `syscall`
 * on. */
void init_tss(uintptr_t kernel_stack);

/** Build the IDT and load it (idt.c). */
void init_idt(void);

/**
 * Make `top` the kernel stack the next entry from user mode lands on, by
 * an interrupt (the TSS's `rsp0`) or by `syscall` (`kernel_rsp`).
 */
void cpu_set_kernel_stack(uintptr_t top);

#endif /* _X86_64_PERCPU_H_ */
