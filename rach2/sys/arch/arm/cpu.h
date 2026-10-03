#ifndef _ARM_CPU_H_
#define _ARM_CPU_H_

#include "../../lib/kint.h"

/*
 * The 32-bit ARM's part of `arch.h`, for an ARMv7-A processor in SVC mode
 * with short-descriptor paging, entered from user mode by `svc` and the
 * exceptions. Nothing implements this architecture yet: the header exists
 * so that the shared code is compiled for it (`make check-arch`), and it is
 * what a port begins from.
 */

/* The registers an exception entry saves: r0 to r12, the user mode's
 * stack pointer and link register, the return address, the saved state,
 * and what the entry was. */
struct isr_params {
    uint32_t r[13];
    uint32_t sp, lr, pc, cpsr;
    uint32_t vector;
    uint32_t error_code; /* the fault status */
};

typedef struct isr_params isr_params_t;
typedef uint32_t phys_addr_t;

/* Linux ARM EABI's system call convention: the number in r7, the arguments
 * in r0 to r5, the result in r0. */
#define SC_NR(p)     ((p)->r[7])
#define SC_RET(p)    ((p)->r[0])
#define SC_ARG(p, n) ((unsigned long)(p)->r[n])

#define FRAME_PC(p)   ((p)->pc)
#define FRAME_SP(p)   ((p)->sp)
#define FRAME_USER(p) (((p)->cpsr & 0x1Fu) == 0x10u)

#define PAGE_SHIFT 12
#define PAGE_SIZE  0x1000

/* A floating-point save area: the 32 doubleword registers of VFPv3 and FPSCR. */
#define FPU_STATE_SIZE 264

#define USER_ADDR_MIN 0x00010000UL
#define USER_ADDR_MAX 0xC0000000UL

#define ELF_CLASS   1u          /* ELFCLASS32 */
#define ELF_MACHINE 40u         /* EM_ARM */
#define AUDIT_ARCH  0x40000028u /* AUDIT_ARCH_ARM */
#define ARCH_NAME   "armv7l"

/* A GIC's shared peripheral interrupts are numbered from 32. */
#define IRQ_VECTOR(line) (32 + (line))
#define ISR_DEVICE_FIRST 32
#define ISR_DEVICE_LAST  1019

#endif /* _ARM_CPU_H_ */
