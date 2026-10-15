/*
 * context.c: the i686 thread frames of ../arch.h: the kernel stack a new or
 * forked thread's first switch_context (switch.s) finds, an exec's user
 * frame, and the check of the record's offsets switch.s reads.
 */

#include "../arch.h"

#include "../../mm/paging.h"
#include "../../proc/process.h"

#include "../../lib/kstring.h"

/* The common entry's return to user mode (isr_handler.s), where a forked
 * child's first switch lands. */
extern void isr_return(void);

/* The offsets switch.s reads from the process record, as `.set` values
 * there and as numbers here; a field added before one of them stops the
 * GNU build. TinyCC cannot fold an offset into an array size, so a kernel it
 * builds finds the change at boot, in `init_proc` (proc/process.c), which
 * calls `arch_context_check` in every build. A host test compiles the
 * record with 64-bit pointers, where the offsets differ, and skips the
 * compile-time check. */
#define PROC_OFF(field)       ((uint32_t)&((process_t *)0)->field)
#define PROC_OFF_REGS_ESP     48u
#define PROC_OFF_KERNEL_STACK 56u
#define PROC_OFF_TLS_BASE     80u
#if !defined(__TINYC__) && !defined(RACH2_HOSTTEST)
typedef char process_offsets_fit[(__builtin_offsetof(struct process, regs) +
                                          __builtin_offsetof(cpu_regs_t, esp) ==
                                      PROC_OFF_REGS_ESP &&
                                  __builtin_offsetof(struct process, kernel_stack) ==
                                      PROC_OFF_KERNEL_STACK &&
                                  __builtin_offsetof(struct process, tls_base) == PROC_OFF_TLS_BASE)
                                     ? 1
                                     : -1];
#endif

int arch_context_check(void)
{
    return PROC_OFF(regs.esp) != PROC_OFF_REGS_ESP ||
           PROC_OFF(kernel_stack) != PROC_OFF_KERNEL_STACK ||
           PROC_OFF(tls_base) != PROC_OFF_TLS_BASE;
}

void switch_space(const process_t *next);

/**
 * Load the page directory `next` runs in, unless it is the one this
 * processor has loaded and no shootdown has marked it stale; switch.s calls
 * it. A
 * record with none, an idle task or a kernel thread, runs in whatever is
 * loaded, whose kernel half is every directory's, so a process that blocks
 * and wakes again on this processor finds its directory still loaded
 * (`smp_space_release` takes it back before it is freed, and
 * `smp_tlb_shootdown` marks it stale meanwhile). The comparison is with
 * what the processor has, never with the outgoing record: a thread that
 * gave up its address space at its end still runs in it until it switches
 * away.
 */
void switch_space(const process_t *next)
{
    struct percpu *cpu = this_cpu();

    if (next->space_root != 0 && (next->page_dir != cpu->space || cpu->space_stale)) {
        cpu->space = next->page_dir;
        cpu->space_stale = 0;
        arch_space_load(next->space_root);
    }
}

/**
 * `switch_context` pops the callee-saved registers and returns, so a new
 * thread's stack is laid out as if it had pushed them, from high to low
 * address:
 *
 *     [entry_point]  return address (where execution begins)
 *     [ebp]          saved EBP (0 for a new process)
 *     [ebx]          saved EBX (0)
 *     [esi]          saved ESI (0)
 *     [edi]          saved EDI (0)   <- regs.esp points here
 */
void arch_context_init(process_t *proc, void (*entry_point)(void))
{
    uint32_t *sp = (uint32_t *)proc->kernel_stack;

    *--sp = (uint32_t)entry_point;
    *--sp = 0; /* EBP */
    *--sp = 0; /* EBX */
    *--sp = 0; /* ESI */
    *--sp = 0; /* EDI */
    proc->regs.esp = (uint32_t)sp;
}

/**
 * The child's stack has a copy of the parent's frame at its top, with fork's
 * 0 in EAX and `newsp` as the user stack pointer when it is not 0, and below
 * it the return address `isr_return` (isr_handler.s) and the four registers
 * `switch_context` pops; its first switch returns to isr_return, which takes
 * the frame back to user mode.
 */
void arch_fork_frame(process_t *child, const isr_params_t *params, uintptr_t newsp)
{
    uint32_t *sp = (uint32_t *)child->kernel_stack;
    isr_params_t *frame;

    sp -= sizeof(isr_params_t) / sizeof(uint32_t);
    frame = (isr_params_t *)sp;
    k_memcpy(frame, params, sizeof(isr_params_t));
    SC_RET(frame) = 0;
    if (newsp) {
        FRAME_SP(frame) = (uint32_t)newsp;
    }
    *--sp = (uint32_t)isr_return;
    *--sp = 0; /* EBP */
    *--sp = 0; /* EBX */
    *--sp = 0; /* ESI */
    *--sp = 0; /* EDI */
    child->regs.esp = (uint32_t)sp;
}

void arch_exec_frame(isr_params_t *p, uintptr_t entry, uintptr_t sp)
{
    p->eip = (uint32_t)entry;
    p->user_esp = (uint32_t)sp;
    p->ebp = 0;
    p->edx = 0;
    p->ebx = 0;
    p->ecx = 0;
    p->esi = 0;
    p->edi = 0;
    p->gs = 0x23; /* the user data selector, GDT entry 4 at RPL 3 */
    p->fs = 0x23;
    p->es = 0x23;
    p->ds = 0x23;
}
