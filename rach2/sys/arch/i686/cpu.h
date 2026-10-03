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
#define SC_NR(p)     ((p)->eax)
#define SC_RET(p)    ((p)->eax)
#define SC_ARG(p, n) SC_ARG_##n(p)
#define SC_ARG_0(p)  ((unsigned long)(p)->ebx)
#define SC_ARG_1(p)  ((unsigned long)(p)->ecx)
#define SC_ARG_2(p)  ((unsigned long)(p)->edx)
#define SC_ARG_3(p)  ((unsigned long)(p)->esi)
#define SC_ARG_4(p)  ((unsigned long)(p)->edi)
#define SC_ARG_5(p)  ((unsigned long)(p)->ebp)

/* The length of the instruction a system call is made by, `int $0x80`'s two
 * bytes, which a restart rewinds the return address over. */
#define SYSCALL_INSN_LEN 2u

#define FRAME_PC(p)   ((p)->eip)
#define FRAME_SP(p)   ((p)->user_esp)
#define FRAME_USER(p) (((p)->cs & 3u) == 3u)

#define PAGE_SHIFT 12
#define PAGE_SIZE  0x1000

/* A floating-point save area, the 512-byte `fxsave` image; `fnsave` uses
 * 108 bytes of it. */
#define FPU_STATE_SIZE 512

/* `rep movs` and `rep stos` move the words of k_memcpy and k_memset. */
#define ARCH_STRING_WORDS 1

/* User space: from 32 MiB, below which the kernel's identity window lies,
 * to 3 GiB, where the kernel's own layout begins. */
#define USER_ADDR_MIN 0x02000000UL
#define USER_ADDR_MAX 0xC0000000UL

#define ELF_CLASS   1u          /* ELFCLASS32 */
#define ELF_MACHINE 3u          /* EM_386 */
#define AUDIT_ARCH  0x40000003u /* AUDIT_ARCH_I386 */
#define ARCH_NAME   "i686"

/* The 8259 pair's sixteen lines arrive on vectors 32 to 47, and a device's
 * messages on 48 to 63. */
#define IRQ_VECTOR(line) (32 + (line))
#define ISR_DEVICE_FIRST 32
#define ISR_DEVICE_LAST  63

#endif /* _I686_CPU_H_ */
