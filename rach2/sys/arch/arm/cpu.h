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

/* The process record's register block: the kernel stack pointer the context
 * switch saved when the process last switched away. */
struct cpu_regs {
    uint32_t sp;
};

typedef struct cpu_regs cpu_regs_t;
typedef uint32_t phys_addr_t;

/* Linux ARM EABI's system call convention: the number in r7, the arguments
 * in r0 to r5, the result in r0. */
#define SC_NR(p)     ((p)->r[7])
#define SC_RET(p)    ((p)->r[0])
#define SC_ARG(p, n) ((unsigned long)(p)->r[n])

/* The length of the instruction a system call is made by, `svc`'s four
 * bytes, which a restart rewinds the return address over. */
#define SYSCALL_INSN_LEN 4u

#define FRAME_PC(p)   ((p)->pc)
#define FRAME_SP(p)   ((p)->sp)
#define FRAME_USER(p) (((p)->cpsr & 0x1Fu) == 0x10u)

#define PAGE_SHIFT 12
#define PAGE_SIZE  0x1000

/* A page table entry and the leaf table's count of them; the bits are the
 * kernel's names for what a port's MMU code makes of them. */
typedef uint32_t pte_t;
#define MMU_TABLE_ENTRIES 256u
#define PTE_P             0x001u
#define PTE_W             0x002u
#define PTE_U             0x004u
#define PTE_NOCACHE       0x008u
#define PTE_DIRTY         0x010u
#define PTE_COW           0x020u
#define PTE_NONE          0x040u
#define PTE_FLAGS         0xFFFu
#define PTE_ADDR(e)       ((e) & ~(pte_t)PTE_FLAGS)

/* The kernel's layout: what the port's MMU code chooses. */
#define HARD_MEMORY_LIMIT 0x2000000
#define KVA_BASE          USER_ADDR_MAX
#define KVA_HEAP          KVA_BASE
#define KVA_HEAP_SIZE     0x10000000u
#define KVA_PCWIN         (KVA_BASE + 0x10000000u)
#define KVA_MMIO          (KVA_BASE + 0x30000000u)
#define KVA_MMIO_SIZE     0x08000000u
#define KVA_TEMP          (KVA_BASE + 0x38000000u)
#define KVA_TEMP_SLOTS    1024u
#define KVA_MODULES       (KVA_BASE + 0x38400000u)
#define KVA_MODULES_SIZE  0x04000000u

/* The page fault: the exception's number and what its syndrome says. */
#define PAGE_FAULT_VECTOR 3
#define FAULT_WRITE(p)    (((p)->error_code & 0x40u) != 0)
#define FAULT_USER(p)     FRAME_USER(p)

/* A floating-point save area: VFPv3's 32 doubleword registers and FPSCR. */
#define FPU_STATE_SIZE 264

#define USER_ADDR_MIN 0x00010000UL
#define USER_ADDR_MAX 0xC0000000UL

/* A new image's layout: a position-independent program's base, the top
 * page of its stack, and the address below which a mapping without a hint
 * goes, above the heap, as Linux's mmap_base places it. */
#define ELF_ET_DYN_BASE 0x00400000UL
#define USER_STACK_BASE 0xBEFFF000UL
#define USER_MMAP_BASE  0xB0000000UL

#define ELF_CLASS   1u          /* ELFCLASS32 */
#define ELF_MACHINE 40u         /* EM_ARM */
#define AUDIT_ARCH  0x40000028u /* AUDIT_ARCH_ARM */
#define ARCH_NAME   "armv7l"

/* The words of ARM's `user_regs_struct`, which ptrace(2)'s register
 * set and a core file's NT_PRSTATUS note have, and the type of the user
 * and group ids in the core file's NT_PRPSINFO note, Linux's
 * `__kernel_uid_t`. */
#define ARCH_GREGS 18
typedef unsigned short core_uid_t;

/* The system call table's size, one more than Linux ARM's highest number,
 * 462; and clone's TLS argument and child tid address, which ARM orders as
 * i386 does, TLS first. */
#define SYSCALL_TABLE_SIZE 463
#define SC_CLONE_TLS       3
#define SC_CLONE_CTID      4

/* A GIC's shared peripheral interrupts are numbered from 32. */
#define IRQ_VECTOR(line) (32 + (line))
#define ISR_DEVICE_FIRST 32
#define ISR_DEVICE_LAST  1019

/* The running processor's index in the per-processor table
 * (proc/percpu.h). A port that starts other processors reads it from a
 * register each has its own value of, as x86-64 does through GS. */
#define arch_cpu_id() 0u

#endif /* _ARM_CPU_H_ */
