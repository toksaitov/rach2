#ifndef _TSS_H_
#define _TSS_H_

#include "../lib/kint.h"

/*
 * Task State Segment structure (104 bytes). The CPU loads esp0/ss0 from it
 * on a privilege change from ring 3 to ring 0 (a syscall or interrupt).
 */
struct tss {
    uint32_t prev_tss; /* Previous TSS link (unused for software switching) */
    uint32_t esp0;     /* Ring 0 stack pointer */
    uint32_t ss0;      /* Ring 0 stack segment */
    uint32_t esp1;     /* Ring 1 stack pointer (unused) */
    uint32_t ss1;      /* Ring 1 stack segment (unused) */
    uint32_t esp2;     /* Ring 2 stack pointer (unused) */
    uint32_t ss2;      /* Ring 2 stack segment (unused) */
    uint32_t cr3;      /* Page directory base (unused for our purposes) */
    uint32_t eip;      /* Instruction pointer (unused) */
    uint32_t eflags;   /* Flags register (unused) */
    uint32_t eax;      /* General registers (unused) */
    uint32_t ecx;
    uint32_t edx;
    uint32_t ebx;
    uint32_t esp;
    uint32_t ebp;
    uint32_t esi;
    uint32_t edi;
    uint32_t es; /* Segment selectors (unused) */
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

/* Kernel data segment selector, installed in ss0. */
#define KERNEL_DATA_SELECTOR 0x10

/* The single kernel TSS. */
extern tss_t kernel_tss;

/** Initialize the kernel TSS with the ring 0 stack top and load it. */
void init_tss(uint32_t kernel_stack);

/**
 * Update the ring 0 stack pointer in the kernel TSS.
 *
 * Called on each context switch so the next ring 3 to ring 0 crossing lands
 * on the incoming task's kernel stack.
 */
void tss_set_kernel_stack(uint32_t stack);

#endif
