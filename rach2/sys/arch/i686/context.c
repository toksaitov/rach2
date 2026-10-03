/*
 * context.c: what an i686's switch_context (switch.s) finds on a kernel
 * stack, built for a process's first switch, and the record's offsets it
 * reads.
 */

#include "../arch.h"

#include "../../proc/process.h"

#include "../../lib/kstring.h"

/* The common entry's return to user mode (isr_handler.s), where a forked
 * child's first switch lands. */
extern void isr_return(void);

/* The offsets switch.s reads from the process record, as `.set` values
 * there and as numbers here; a field added before one of them stops the
 * GNU build. TinyCC cannot fold an offset into an array size, so a kernel it
 * builds finds the change at boot, in `init_proc`, which checks every
 * build. A host test compiles the record with 64-bit pointers, where the
 * offsets differ, and skips the compile-time check. */
#define PROC_OFF(field)       ((uint32_t)&((process_t *)0)->field)
#define PROC_OFF_REGS_ESP     48u
#define PROC_OFF_SPACE_ROOT   52u
#define PROC_OFF_KERNEL_STACK 56u
#define PROC_OFF_TLS_BASE     80u
#define PROC_OFF_PAGE_DIR     720u
#if !defined(__TINYC__) && !defined(RACH2_HOSTTEST)
typedef char process_offsets_fit
    [(__builtin_offsetof(struct process, regs) + __builtin_offsetof(cpu_regs_t, esp) ==
          PROC_OFF_REGS_ESP &&
      __builtin_offsetof(struct process, space_root) == PROC_OFF_SPACE_ROOT &&
      __builtin_offsetof(struct process, kernel_stack) == PROC_OFF_KERNEL_STACK &&
      __builtin_offsetof(struct process, tls_base) == PROC_OFF_TLS_BASE &&
      __builtin_offsetof(struct process, page_dir) == PROC_OFF_PAGE_DIR)
         ? 1
         : -1];
#endif

int arch_context_check(void)
{
    return PROC_OFF(regs.esp) != PROC_OFF_REGS_ESP ||
           PROC_OFF(space_root) != PROC_OFF_SPACE_ROOT ||
           PROC_OFF(kernel_stack) != PROC_OFF_KERNEL_STACK ||
           PROC_OFF(tls_base) != PROC_OFF_TLS_BASE || PROC_OFF(page_dir) != PROC_OFF_PAGE_DIR;
}

/**
 * Prime a new process's kernel stack for its first context switch.
 *
 * `switch_context` restores callee-saved registers and rets, so it expects
 * the stack to look as if it had pushed them. The frame this builds, from
 * high to low address:
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
 * Lay out `child`'s kernel stack for its first switch, from the parent's
 * trap frame `params`: the frame, with 0 in EAX, fork's return, and the new
 * stack pointer `newsp` a clone asks for, when it asks; above it the return
 * address `isr_return` and the four registers `switch_context` pops. When
 * the child is first scheduled, switch_context pops EDI, ESI, EBX and EBP,
 * returns to isr_return, and the frame takes it to user mode.
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
