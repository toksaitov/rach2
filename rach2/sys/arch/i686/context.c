/*
 * context.c: what an i686's switch_context (switch.s) finds on a kernel
 * stack, built for a process's first switch, and the record's offsets it
 * reads.
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
 * builds finds the change at boot, in `init_proc`, which checks every
 * build. A host test compiles the record with 64-bit pointers, where the
 * offsets differ, and skips the compile-time check. */
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
 * processor has loaded and no shootdown passed it by; switch.s calls it. A
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
    p->gs = 0x23; /* the user data segment, GDT entry 4 at ring 3 */
    p->fs = 0x23;
    p->es = 0x23;
    p->ds = 0x23;
}
