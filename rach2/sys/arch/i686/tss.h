#ifndef _TSS_H_
#define _TSS_H_

#include "../../lib/kint.h"

/*
 * Task State Segment structure (104 bytes). The CPU loads esp0/ss0 from it
 * on a privilege change from ring 3 to ring 0 (a syscall or interrupt). A
 * hardware task switch saves the fields from eip to gs into the TSS it
 * leaves and loads those from cr3 to ldt from the one it enters; the kernel
 * switches processes by software, and its one hardware task switch is the
 * double fault's.
 */
struct tss {
    uint32_t prev_tss; /* The TSS a task switch came from, which it writes */
    uint32_t esp0;     /* Ring 0 stack pointer */
    uint32_t ss0;      /* Ring 0 stack segment */
    uint32_t esp1;     /* Ring 1 stack pointer (unused) */
    uint32_t ss1;      /* Ring 1 stack segment (unused) */
    uint32_t esp2;     /* Ring 2 stack pointer (unused) */
    uint32_t ss2;      /* Ring 2 stack segment (unused) */
    uint32_t cr3;      /* Page directory base the task runs with */
    uint32_t eip;      /* Instruction pointer */
    uint32_t eflags;   /* Flags register */
    uint32_t eax;      /* General registers */
    uint32_t ecx;
    uint32_t edx;
    uint32_t ebx;
    uint32_t esp;
    uint32_t ebp;
    uint32_t esi;
    uint32_t edi;
    uint32_t es; /* Segment selectors */
    uint32_t cs;
    uint32_t ss;
    uint32_t ds;
    uint32_t fs;
    uint32_t gs;
    uint32_t ldt;        /* LDT selector (unused) */
    uint16_t trap;       /* Trap on task switch flag */
    uint16_t iomap_base; /* I/O map base address */
} __attribute__((packed));

typedef struct tss tss_t;

/* TSS selector in the GDT (index 5, so selector = 5 * 8 = 0x28). */
#define TSS_SELECTOR 0x28

/* The double fault's TSS selector in the GDT (index 7, 7 * 8 = 0x38). */
#define DOUBLE_FAULT_TSS_SELECTOR 0x38

/* Kernel code segment selector, installed in the double fault's cs. */
#define KERNEL_CODE_SELECTOR 0x08

/* Kernel data segment selector, installed in ss0. */
#define KERNEL_DATA_SELECTOR 0x10

/* The single kernel TSS. */
extern tss_t kernel_tss;

/*
 * The double fault's TSS, which the gate of vector 8 names: the processor
 * saves what was running into `kernel_tss` and starts `isr_double_fault`
 * from this one, on a stack of its own.
 */
extern tss_t double_fault_tss;

/** Initialize the kernel TSS with the ring 0 stack top and load it, and
 * set up the double fault's TSS. */
void init_tss(uint32_t kernel_stack);

/**
 * Update the ring 0 stack pointer in the kernel TSS.
 *
 * Called on each context switch so the next ring 3 to ring 0 crossing lands
 * on the incoming task's kernel stack.
 */
void tss_set_kernel_stack(uint32_t stack);

/**
 * Give the double fault's task the page directory at physical `dir`, which
 * the processor loads into CR3 when it switches to the task. `init_paging`
 * gives it the kernel's before it turns paging on.
 */
void tss_set_double_fault_dir(uint32_t dir);

#endif
