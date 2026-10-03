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
 * and segment. The `syscall` entry builds the same frame, with the vector
 * SYSCALL_VECTOR, the user's RIP and RFLAGS from RCX and R11, and the user
 * selectors (isr_handler.s).
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

/*
 * The process record's register block: the kernel stack pointer
 * `switch_context` saved when the process last switched away, or the one
 * built for its first switch, and the user's GS base, which arch_prctl's
 * ARCH_SET_GS sets and the switch loads. The FS base, the thread pointer,
 * is the record's `tls_base`.
 */
struct cpu_regs {
    uint64_t rsp;
    uint64_t gs_base;
};

typedef struct cpu_regs cpu_regs_t;

/* Linux x86-64's system call convention: the number in RAX, the arguments
 * in RDI, RSI, RDX, R10, R8 and R9, the result in RAX. */
#define SC_NR(p)       ((p)->rax)
#define SC_RET(p)      ((p)->rax)
#define SC_ARG(p, n)   SC_ARG_N(p, n)
#define SC_ARG_N(p, n) SC_ARG_##n(p)
#define SC_ARG_0(p)    ((unsigned long)(p)->rdi)
#define SC_ARG_1(p)    ((unsigned long)(p)->rsi)
#define SC_ARG_2(p)    ((unsigned long)(p)->rdx)
#define SC_ARG_3(p)    ((unsigned long)(p)->r10)
#define SC_ARG_4(p)    ((unsigned long)(p)->r8)
#define SC_ARG_5(p)    ((unsigned long)(p)->r9)

/* The length of the instruction a system call is made by, `syscall`'s two
 * bytes, which a restart rewinds the return address over. */
#define SYSCALL_INSN_LEN 2u

#define FRAME_PC(p)   ((p)->rip)
#define FRAME_SP(p)   ((p)->rsp)
#define FRAME_USER(p) (((p)->cs & 3u) == 3u)

#define PAGE_SHIFT 12
#define PAGE_SIZE  0x1000

/*
 * Four-level paging: a PML4 of 512 entries, each naming a page directory
 * pointer table, then a page directory, then a page table of 512 page
 * entries (arch/x86_64/mmu.c). The low twelve bits of an entry are the
 * i686's, with the same two marks of the kernel's own in bits the processor
 * ignores; bit 63 forbids instruction fetches where EFER.NXE is set (Intel
 * SDM Vol. 3A, 4.5). The physical address is bits 12 to 51.
 */
typedef uint64_t pte_t;
#define MMU_TABLE_ENTRIES 512u
#define PTE_P             0x001UL /* present */
#define PTE_W             0x002UL /* writable */
#define PTE_U             0x004UL /* reachable from user mode */
#define PTE_NOCACHE       0x018UL /* write-through, cache-disabled: PWT and PCD */
#define PTE_DIRTY         0x040UL /* written since the bit was cleared */
#define PTE_PS            0x080UL /* in a PDPT or directory entry: a 1 GiB or 2 MiB page */
/* Bit 9: a page shared copy-on-write, as on i686. */
#define PTE_COW           0x200UL
/* Bit 10: a resident page of a PROT_NONE region, as on i686. */
#define PTE_NONE          0x400UL
/* Bit 63: no instruction fetch from the page. Set only where the processor
 * has the bit (`mmu_nx`, arch/x86_64/mmu.h): elsewhere it is reserved, and
 * an entry with it faults. */
#define PTE_NX            0x8000000000000000UL
#define PTE_FLAGS         0xFFF0000000000FFFUL
#define PTE_ADDR(e)       ((e) & ~(pte_t)PTE_FLAGS)

/*
 * The boot's window: the kernel image, the boot data, the page tables the
 * boot carves and the DMA buffers a device is given the physical address of
 * live below it. The kernel reaches it, as it reaches all physical memory,
 * through the direct map at KVA_DIRECT, never at its physical address,
 * since the lower half is user space.
 */
#define HARD_MEMORY_LIMIT 0x2000000

/*
 * The kernel's virtual layout, the upper half of the 48-bit address space.
 * Each region has PML4 entries of its own, and every one of the upper
 * half's entries that the kernel uses exists from the boot and is copied
 * into every page directory, so a mapping made below one later is seen by
 * every process (mmu.c). The windows' eight entries, 4 TiB from KVA_HEAP,
 * exist whether a window uses them yet or not, and a window added later
 * goes there. The regions:
 *
 *   0xFFFF800000000000   64 TiB  all physical memory, the direct map, in the
 *                                largest pages the processor has
 *   0xFFFFC00000000000  256 MiB  the heap (mm/kheap.c), grown a frame at a
 *                                time; its PML4 entry has room for 512 GiB
 *   0xFFFFC08000000000  128 MiB  MMIO, a controller's BAR mapped wherever
 *                                it is
 *   0xFFFFC10000000000    4 MiB  temporary mappings, one page each
 *   0xFFFFC18000000000   64 MiB  the boot modules
 *   0xFFFFC20000000000  512 MiB  the page-cache window (mm/pagecache.h), at
 *                                most
 *   0xFFFFFFFF80000000    2 GiB  the kernel image, at its physical address
 *                                plus this base (-mcmodel=kernel)
 */
#define KVA_BASE         0xFFFF800000000000UL
#define KVA_DIRECT       0xFFFF800000000000UL
#define KVA_DIRECT_SIZE  0x0000400000000000UL
#define KVA_HEAP         0xFFFFC00000000000UL
#define KVA_HEAP_SIZE    0x10000000UL
#define KVA_MMIO         0xFFFFC08000000000UL
#define KVA_MMIO_SIZE    0x08000000UL
#define KVA_TEMP         0xFFFFC10000000000UL
/* Policy: the temporary mappings held at once, as on i686. */
#define KVA_TEMP_SLOTS   1024u
#define KVA_MODULES      0xFFFFC18000000000UL
#define KVA_MODULES_SIZE 0x04000000UL
#define KVA_PCWIN        0xFFFFC20000000000UL
#define KVA_IMAGE        0xFFFFFFFF80000000UL

/* The page fault's vector, and what its error code says (Intel SDM Vol.
 * 3A, 4.7): a write, and an access from user mode. */
#define PAGE_FAULT_VECTOR 14
#define FAULT_WRITE(p)    (((p)->error_code & 0x2u) != 0)
#define FAULT_USER(p)     (((p)->error_code & 0x4u) != 0)

/* A floating-point save area, the 512-byte `fxsave64` image. */
#define FPU_STATE_SIZE 512

/* `rep movsq` and `rep stosq` move the words of k_memcpy and k_memset. */
#define ARCH_STRING_WORDS 1

/* User space: from 64 KiB, below which a null pointer's neighbours fault,
 * to the end of the lower half of the 48-bit address space, less its last
 * page, which `sysret` must never return into (mmu.c). */
#define USER_ADDR_MIN 0x0000000000010000UL
#define USER_ADDR_MAX 0x00007FFFFFFFF000UL

/* A new image's layout: a position-independent program's base, the top
 * page of its stack, and the address below which a mapping without a hint
 * goes, above the heap, as Linux's mmap_base places it. */
#define ELF_ET_DYN_BASE 0x0000555555554000UL
#define USER_STACK_BASE 0x00007FFFFFFFE000UL
#define USER_MMAP_BASE  0x00007F0000000000UL

#define ELF_CLASS   2u          /* ELFCLASS64 */
#define ELF_MACHINE 62u         /* EM_X86_64 */
#define AUDIT_ARCH  0xC000003Eu /* AUDIT_ARCH_X86_64 */
#define ARCH_NAME   "x86_64"

/* The words of x86-64's `user_regs_struct`, which ptrace(2)'s register
 * set and a core file's NT_PRSTATUS note have, and the type of the user
 * and group ids in the core file's NT_PRPSINFO note, Linux's
 * `__kernel_uid_t`. */
#define ARCH_GREGS 27
typedef unsigned int core_uid_t;

/* The system call table's size, one more than Linux x86-64's highest
 * number, 462; and clone's TLS argument and child tid address, which x86-64
 * orders child tid first. */
#define SYSCALL_TABLE_SIZE 463
#define SC_CLONE_TLS       4
#define SC_CLONE_CTID      3

/* The 8259 pair's sixteen lines arrive on vectors 32 to 47, and a device's
 * messages on 48 to 63. */
#define IRQ_VECTOR(line) (32 + (line))
#define ISR_DEVICE_FIRST 32
#define ISR_DEVICE_LAST  63

/* The running processor's index in the per-processor table
 * (proc/percpu.h): the `id` of the processor's own record, which GS's base
 * points at in the kernel, at offset 24 (percpu.h). */
static __inline__ unsigned int arch_cpu_id(void)
{
    unsigned int id;

    __asm__ __volatile__("movl %%gs:24, %0" : "=r"(id));
    return id;
}

#endif /* _X86_64_CPU_H_ */
