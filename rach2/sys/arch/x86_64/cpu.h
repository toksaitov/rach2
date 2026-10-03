#ifndef _X86_64_CPU_H_
#define _X86_64_CPU_H_

#include "../../lib/kint.h"

/*
 * The x86-64's part of `arch.h`: a 64-bit x86 processor in long mode with
 * four-level paging, entered from user mode by `syscall` and the interrupts.
 */

/*
 * The registers the common entry saves, in the order it pushes them: the
 * fifteen general registers, the vector and the error code, and the
 * processor's own frame, which in long mode always has the stack pointer
 * and segment.
 */
struct isr_params {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector;
    uint64_t error_code;
    uint64_t rip, cs, rflags, rsp, ss;
};

typedef struct isr_params isr_params_t;
typedef uint64_t phys_addr_t;

/* The process record's register block: the kernel stack pointer
 * `switch_context` saved when the process last switched away, or the one
 * built for its first switch. */
struct cpu_regs {
    uint64_t rsp;
};

typedef struct cpu_regs cpu_regs_t;

/* Linux x86-64's system call convention: the number in RAX, the arguments
 * in RDI, RSI, RDX, R10, R8 and R9, the result in RAX. */
#define SC_NR(p)     ((p)->rax)
#define SC_RET(p)    ((p)->rax)
#define SC_ARG(p, n) SC_ARG_##n(p)
#define SC_ARG_0(p)  ((unsigned long)(p)->rdi)
#define SC_ARG_1(p)  ((unsigned long)(p)->rsi)
#define SC_ARG_2(p)  ((unsigned long)(p)->rdx)
#define SC_ARG_3(p)  ((unsigned long)(p)->r10)
#define SC_ARG_4(p)  ((unsigned long)(p)->r8)
#define SC_ARG_5(p)  ((unsigned long)(p)->r9)

#define FRAME_PC(p)   ((p)->rip)
#define FRAME_SP(p)   ((p)->rsp)
#define FRAME_USER(p) (((p)->cs & 3u) == 3u)

#define PAGE_SHIFT 12
#define PAGE_SIZE  0x1000

/* A floating-point save area, the 512-byte `fxsave` image. */
#define FPU_STATE_SIZE 512

/* `rep movsq` and `rep stosq` move the words of k_memcpy and k_memset. */
#define ARCH_STRING_WORDS 1

/* User space: from 64 KiB, below which a null pointer's neighbours fault,
 * to the end of the lower half of the 48-bit address space. */
#define USER_ADDR_MIN 0x0000000000010000UL
#define USER_ADDR_MAX 0x00007FFFFFFFF000UL

#define ELF_CLASS   2u          /* ELFCLASS64 */
#define ELF_MACHINE 62u         /* EM_X86_64 */
#define AUDIT_ARCH  0xC000003Eu /* AUDIT_ARCH_X86_64 */
#define ARCH_NAME   "x86_64"

/* The 8259 pair's sixteen lines arrive on vectors 32 to 47, and a device's
 * messages on 48 to 63. */
#define IRQ_VECTOR(line) (32 + (line))
#define ISR_DEVICE_FIRST 32
#define ISR_DEVICE_LAST  63

#endif /* _X86_64_CPU_H_ */
