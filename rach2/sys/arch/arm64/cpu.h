#ifndef _ARM64_CPU_H_
#define _ARM64_CPU_H_

#include "../../lib/kint.h"

/*
 * The 64-bit ARM's part of `arch.h`, for an AArch64 processor at EL1 with
 * 4 KiB pages and 48-bit virtual addresses, entered from EL0 by `svc` and
 * the exceptions. No code implements this architecture: the header exists
 * so that the shared code is compiled for it (`make check-arch`), and a
 * port begins from it.
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

/* A page table entry and the leaf table's count of them; the bits are the
 * kernel's names for what a port's MMU code makes of them. */
typedef uint64_t pte_t;
#define MMU_TABLE_ENTRIES 512u
#define PTE_P             0x001u
#define PTE_W             0x002u
#define PTE_U             0x004u
#define PTE_NOCACHE       0x008u
#define PTE_DIRTY         0x010u
#define PTE_COW           0x020u
#define PTE_NONE          0x040u
#define PTE_NOEXEC        0x080u
#define PTE_FLAGS         0xFFFu
#define PTE_ADDR(e)       ((e) & ~(pte_t)PTE_FLAGS)

/* The kernel's layout: what the port's MMU code chooses. */
#define HARD_MEMORY_LIMIT 0x2000000

/* Physical memory from the kernel (arch.h), as the port's MMU code maps
 * it. */
void *phys_to_virt(phys_addr_t p);
phys_addr_t virt_to_phys(const volatile void *v);

/* A device's register at the kernel address `a` (arch.h). A port defines
 * them with the barriers its memory model asks of a device access. */
uint8_t mmio_read8(uintptr_t a);
uint16_t mmio_read16(uintptr_t a);
uint32_t mmio_read32(uintptr_t a);
void mmio_write8(uintptr_t a, uint8_t v);
void mmio_write16(uintptr_t a, uint16_t v);
void mmio_write32(uintptr_t a, uint32_t v);

/* The kernel's regions, placed after the user range so that the shared code
 * compiles; a port's MMU code chooses where they are. */
#define KVA_BASE         USER_ADDR_MAX
#define KVA_HEAP         KVA_BASE
#define KVA_HEAP_SIZE    0x10000000u
#define KVA_PCWIN        (KVA_BASE + 0x10000000u)
#define KVA_MMIO         (KVA_BASE + 0x30000000u)
#define KVA_MMIO_SIZE    0x08000000u
#define KVA_TEMP         (KVA_BASE + 0x38000000u)
#define KVA_TEMP_SLOTS   1024u
#define KVA_MODULES      (KVA_BASE + 0x38400000u)
#define KVA_MODULES_SIZE 0x04000000u

/* The page fault: a data or instruction abort, by the number a port's entry
 * gives it, and what its syndrome in ESR_EL1 says: bit 6 (WnR) of a data
 * abort marks a write. */
#define PAGE_FAULT_VECTOR 3
#define FAULT_WRITE(p)    (((p)->error_code & 0x40u) != 0)
#define FAULT_USER(p)     FRAME_USER(p)
/* An instruction abort from EL0, ESR_EL1's class 0x20. */
#define FAULT_EXEC(p)     (((p)->error_code >> 26) == 0x20u)

#define USER_ADDR_MIN 0x0000000000010000UL
#define USER_ADDR_MAX 0x0001000000000000UL

/* A new image's layout: a position-independent program's base, the top
 * page of its stack, and the address below which a mapping without a hint
 * goes, above the heap, as Linux's mmap_base places it. */
#define ELF_ET_DYN_BASE 0x0000AAAAAAAAA000UL
#define USER_STACK_BASE 0x0000FFFFFFFFE000UL
#define USER_MMAP_BASE  0x0000FFFF00000000UL

#define ELF_CLASS   2u          /* ELFCLASS64 */
#define ELF_MACHINE 183u        /* EM_AARCH64 */
#define AUDIT_ARCH  0xC00000B7u /* AUDIT_ARCH_AARCH64 */
#define ARCH_NAME   "aarch64"

/* The calls seccomp's strict mode allows, by this ABI's numbers: read,
 * write, exit and rt_sigreturn, as Linux's `mode1_syscalls` has them. */
#define SECCOMP_NR_READ      63u
#define SECCOMP_NR_WRITE     64u
#define SECCOMP_NR_EXIT      93u
#define SECCOMP_NR_SIGRETURN 139u

/* The words of AArch64's `user_regs_struct`, which ptrace(2)'s register
 * set and a core file's NT_PRSTATUS note have, and the type of the user
 * and group ids in the core file's NT_PRPSINFO note, Linux's
 * `__kernel_uid_t`. */
#define ARCH_GREGS 34
typedef unsigned int core_uid_t;

/* The system call table's size, one more than the highest number of
 * Linux's generic table, 462; and clone's TLS argument and child tid
 * address, which AArch64 orders as i386 and ARM do, TLS first (Linux's
 * CLONE_BACKWARDS). */
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

#endif /* _ARM64_CPU_H_ */
