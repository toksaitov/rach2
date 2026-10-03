#ifndef _I686_CPU_H_
#define _I686_CPU_H_

#include "../../lib/kint.h"

/*
 * The i686's part of `arch.h`: a 32-bit x86 processor in protected mode with
 * two-level paging, entered from user mode by `int 0x80` and the interrupts.
 */

/*
 * The registers the common entry (isr_handler.s) saves, in the order it
 * pushes them: the segment registers, `pushal`'s eight, the vector and the
 * error code it pushed, and the processor's own frame. `esp` is the one
 * `pushal` saw and is not restored; `user_esp` and `user_ss` are present
 * only on an entry from user mode.
 */
struct isr_params {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t vector;
    uint32_t error_code;
    uint32_t eip, cs, eflags, user_esp, user_ss;
};

typedef struct isr_params isr_params_t;
typedef uint32_t phys_addr_t;

/*
 * The process record's register block. Only `esp` is used: the kernel stack
 * pointer `switch_context` saved when the process last switched away, or the
 * one built for its first switch (context.c). The other fields are read by
 * nothing; they keep `esp` at the offset switch.s reads.
 */
struct cpu_regs {
    uint32_t edi;
    uint32_t esi;
    uint32_t ebp;
    uint32_t esp_dummy; /* Ignored by popal */
    uint32_t ebx;
    uint32_t edx;
    uint32_t ecx;
    uint32_t eax;
    uint32_t eip;
    uint32_t eflags;
    uint32_t esp; /* Kernel stack pointer for switch_context */
};

typedef struct cpu_regs cpu_regs_t;

/* Linux i386's system call convention: the number in EAX, the arguments in
 * EBX, ECX, EDX, ESI, EDI and EBP, the result in EAX. */
#define SC_NR(p)       ((p)->eax)
#define SC_RET(p)      ((p)->eax)
#define SC_ARG(p, n)   SC_ARG_N(p, n)
#define SC_ARG_N(p, n) SC_ARG_##n(p)
#define SC_ARG_0(p)    ((unsigned long)(p)->ebx)
#define SC_ARG_1(p)    ((unsigned long)(p)->ecx)
#define SC_ARG_2(p)    ((unsigned long)(p)->edx)
#define SC_ARG_3(p)    ((unsigned long)(p)->esi)
#define SC_ARG_4(p)    ((unsigned long)(p)->edi)
#define SC_ARG_5(p)    ((unsigned long)(p)->ebp)

/* The length of the instruction a system call is made by, `int $0x80`'s two
 * bytes, which a restart rewinds the return address over. */
#define SYSCALL_INSN_LEN 2u

#define FRAME_PC(p)   ((p)->eip)
#define FRAME_SP(p)   ((p)->user_esp)
#define FRAME_USER(p) (((p)->cs & 3u) == 3u)

#define PAGE_SHIFT 12
#define PAGE_SIZE  0x1000

/*
 * Two-level paging without PAE: a directory of 1024 entries, each naming a
 * table of 1024 page entries (arch/i686/mmu.c). An entry's bits are the
 * processor's (Intel SDM Vol. 3A, 4.3) and two the processor ignores, which
 * the kernel keeps its own marks in.
 */
typedef uint32_t pte_t;
#define MMU_TABLE_ENTRIES 1024u
#define PTE_P             0x001u /* present */
#define PTE_W             0x002u /* writable */
#define PTE_U             0x004u /* reachable from user mode */
#define PTE_NOCACHE       0x018u /* write-through, cache-disabled: PWT and PCD */
#define PTE_DIRTY         0x040u /* written since the bit was cleared */
/* Bit 9: a page shared copy-on-write. Present, user, read-only and PTE_COW
 * means "shared, copy on the next write"; the fault handler tells a
 * copy-on-write write from a write to a read-only region by this bit. */
#define PTE_COW           0x200u
/* Bit 10: a resident page of a PROT_NONE region. The page stays present,
 * with its frame and contents, but supervisor-only and read-only, so any
 * user access faults; fork and teardown own it as a user page by this bit,
 * and mprotect makes it a user page again. */
#define PTE_NONE          0x400u
#define PTE_FLAGS         0xFFFu
#define PTE_ADDR(e)       ((e) & ~(pte_t)PTE_FLAGS)

/* Top of the identity-mapped physical window (32 MiB): the kernel image,
 * the boot data, the page tables the boot carves and the DMA buffers that a
 * device is given a physical address of live below it, where the virtual
 * address equals the physical one. Nothing above it is reached through its
 * physical address; a frame there is mapped into the kernel's layout first,
 * and the modules and the page tables made later are reached that way. */
#define HARD_MEMORY_LIMIT 0x2000000

/*
 * The kernel's virtual layout above user space, in the gigabyte from
 * `USER_ADDR_MAX`. Every page table for it exists at boot and is shared by
 * every page directory, so a mapping made in it after the first fork is
 * seen by every process. The regions:
 *
 *   0xC0000000  256 MiB  the heap (mm/kheap.c), grown a frame at a time
 *   0xD0000000  512 MiB  the page-cache window (mm/pagecache.h)
 *   0xF0000000  128 MiB  MMIO, a controller's BAR mapped wherever it is
 *   0xF8000000    4 MiB  temporary mappings, one page each, for the paths
 *                        that touch a user frame by its physical address
 *   0xF8400000   64 MiB  the boot modules, mapped wherever the loader put
 *                        them
 *   0xFC400000   ...     unused
 */
#define KVA_BASE         0xC0000000u
#define KVA_HEAP         0xC0000000u
#define KVA_HEAP_SIZE    0x10000000u
/* The page-cache window sat at USER_ADDR_MAX until the first real machine:
 * its firmware places device MMIO from 0xC0500000, and since the window is
 * sized down to the lowest BAR above its base, a window starting there was
 * five megabytes long, smaller than any disk worth mounting. So it starts
 * a quarter of the way up, past the gap where that firmware's
 * identity-mapped MMIO lives. */
#define KVA_PCWIN        0xD0000000u
#define KVA_MMIO         0xF0000000u
#define KVA_MMIO_SIZE    0x08000000u
#define KVA_TEMP         0xF8000000u
/* Policy: the temporary mappings held at once, one page each, the whole
 * of the region's 4 MiB; a caller that finds none free is told so. */
#define KVA_TEMP_SLOTS   1024u
#define KVA_MODULES      0xF8400000u
#define KVA_MODULES_SIZE 0x04000000u

/* The page fault's vector, and what its error code says (Intel SDM Vol.
 * 3A, 4.7): a write, and an access from user mode. */
#define PAGE_FAULT_VECTOR 14
#define FAULT_WRITE(p)    (((p)->error_code & 0x2u) != 0)
#define FAULT_USER(p)     (((p)->error_code & 0x4u) != 0)

/* A floating-point save area, the 512-byte `fxsave` image; `fnsave` uses
 * 108 bytes of it. */
#define FPU_STATE_SIZE 512

/* `rep movs` and `rep stos` move the words of k_memcpy and k_memset. */
#define ARCH_STRING_WORDS 1

/* User space: from 32 MiB, below which the kernel's identity window lies,
 * to 3 GiB, where the kernel's own layout begins. */
#define USER_ADDR_MIN 0x02000000UL
#define USER_ADDR_MAX 0xC0000000UL

/* A new image's layout: a position-independent program's base, the top
 * page of its stack, and the address below which a mapping without a hint
 * goes, above the heap, as Linux's mmap_base places it. */
#define ELF_ET_DYN_BASE 0x08000000UL
#define USER_STACK_BASE 0xBFFFF000UL
#define USER_MMAP_BASE  0xB0000000UL

#define ELF_CLASS   1u          /* ELFCLASS32 */
#define ELF_MACHINE 3u          /* EM_386 */
#define AUDIT_ARCH  0x40000003u /* AUDIT_ARCH_I386 */
#define ARCH_NAME   "i686"

/* The words of i386's `user_regs_struct`, which ptrace(2)'s register
 * set and a core file's NT_PRSTATUS note have, and the type of the user
 * and group ids in the core file's NT_PRPSINFO note, Linux's
 * `__kernel_uid_t`. */
#define ARCH_GREGS 17
typedef unsigned short core_uid_t;

/* The system call table's size, one more than Linux i386's highest number,
 * 462; and clone's TLS argument and child tid address, which i386 orders
 * TLS first, as Linux's CLONE_BACKWARDS has them. */
#define SYSCALL_TABLE_SIZE 463
#define SC_CLONE_TLS       3
#define SC_CLONE_CTID      4

/* The 8259 pair's sixteen lines arrive on vectors 32 to 47, and a device's
 * messages on 48 to 63. */
#define IRQ_VECTOR(line) (32 + (line))
#define ISR_DEVICE_FIRST 32
#define ISR_DEVICE_LAST  63

#endif /* _I686_CPU_H_ */
