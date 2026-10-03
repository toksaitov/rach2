#ifndef _ARM64_CPU_H_
#define _ARM64_CPU_H_

#include "../../lib/kint.h"

/*
 * The 64-bit ARM's part of `arch.h`, for an AArch64 processor at EL1 with
 * 4 KiB pages and 48-bit virtual addresses, entered from EL0 by `svc` and
 * the exceptions. Nothing implements this architecture yet: the header
 * exists so that the shared code is compiled for it (`make check-arch`),
 * and it is what a port begins from.
 */

/* The registers an exception entry saves: x0 to x30, the stack pointer
 * EL0 had, the return address, the saved state, and what the entry was. */
struct isr_params {
    uint64_t x[31];
    uint64_t sp, pc, pstate;
    uint64_t vector;
    uint64_t error_code; /* ESR_EL1 */
};

typedef struct isr_params isr_params_t;

/* The process record's register block: the kernel stack pointer the context
 * switch saved when the process last switched away. */
struct cpu_regs {
    uint64_t sp;
};

typedef struct cpu_regs cpu_regs_t;
typedef uint64_t phys_addr_t;

/* Linux AArch64's system call convention: the number in x8, the arguments
 * in x0 to x5, the result in x0. */
#define SC_NR(p)     ((p)->x[8])
#define SC_RET(p)    ((p)->x[0])
#define SC_ARG(p, n) ((unsigned long)(p)->x[n])

/* The length of the instruction a system call is made by, `svc`'s four
 * bytes, which a restart rewinds the return address over. */
#define SYSCALL_INSN_LEN 4u

#define FRAME_PC(p)   ((p)->pc)
#define FRAME_SP(p)   ((p)->sp)
#define FRAME_USER(p) (((p)->pstate & 0xFu) == 0u)

#define PAGE_SHIFT 12
#define PAGE_SIZE  0x1000

/* A floating-point save area: the 32 128-bit registers, FPSR and FPCR. */
#define FPU_STATE_SIZE 528

#define USER_ADDR_MIN 0x0000000000010000UL
#define USER_ADDR_MAX 0x0001000000000000UL

#define ELF_CLASS   2u          /* ELFCLASS64 */
#define ELF_MACHINE 183u        /* EM_AARCH64 */
#define AUDIT_ARCH  0xC00000B7u /* AUDIT_ARCH_AARCH64 */
#define ARCH_NAME   "aarch64"

/* A GIC's shared peripheral interrupts are numbered from 32. */
#define IRQ_VECTOR(line) (32 + (line))
#define ISR_DEVICE_FIRST 32
#define ISR_DEVICE_LAST  1019

#endif /* _ARM64_CPU_H_ */
