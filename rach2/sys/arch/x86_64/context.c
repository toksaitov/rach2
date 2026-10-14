/*
 * context.c: the x86-64's context switch, the stacks a thread's first switch
 * finds, the frame an exec starts a program from, and the entry to user
 * mode.
 *
 * The switch is C around one short routine, x86_64_switch (switch.s), which
 * saves the callee-saved registers on the old thread's kernel stack and
 * takes the new one's. Everything else a switch changes, the kernel stack
 * the next entry lands on, the FS and GS selectors and bases and the page
 * directory, is set here, before the stacks change, by the outgoing thread
 * for the incoming one; the record is read through the compiler's offsets,
 * and no assembly knows its layout.
 */

#include "percpu.h"

#include "../arch.h"

#include "../../mm/paging.h"
#include "../../proc/process.h"
#include "../../lib/kstring.h"

/* The common entry's return to user mode (isr_handler.s), where a forked
 * child's first switch lands. */
extern void isr_return(void);

extern void x86_64_switch(uint64_t *save_sp, uint64_t load_sp);
extern void x86_64_enter_user(uintptr_t pc, uintptr_t sp) K_NORETURN;

/* The registers x86_64_switch pushes: RBP, RBX and R12 to R15. */
#define SWITCH_SAVED 6

/* RFLAGS with interrupts on and bit 1, which the processor keeps set. */
#define RFLAGS_START 0x202u

/**
 * Nonzero when struct cpu's first fields are not at the offsets the entry
 * code reads (percpu.h); the record of a process has none of that kind on
 * x86-64.
 */
int arch_context_check(void)
{
    const struct cpu *c = &cpus[0];

    return (const char *)&c->self != (const char *)c ||
           (const char *)&c->kernel_rsp - (const char *)c != 8 ||
           (const char *)&c->user_rsp - (const char *)c != 16 ||
           (const char *)&c->id - (const char *)c != 24;
}

/**
 * Switch from `old` to `new_proc`.
 *
 * The incoming thread's kernel stack becomes the one the next entry from
 * user mode takes; its FS and GS selectors are loaded where they differ
 * from the outgoing thread's, then its FS base, its thread pointer, and its
 * GS base, which goes where `swapgs` will find it on the way out, since a
 * selector load changes the base too; the page directory
 * changes when the incoming thread has one and it is not the one loaded or
 * a shootdown passed it by: a record with none, an idle task or a kernel
 * thread, runs in whatever is loaded, whose kernel half is every
 * directory's, so a process that blocks and wakes again here finds its
 * directory still loaded (`smp_space_release` takes it back before it is
 * freed, and `smp_tlb_shootdown` marks it stale meanwhile). Then the stacks
 * change. The outgoing thread's selectors and bases are read back into its
 * record first (`arch_bases_save`): a program loads a selector into FS or
 * GS without a system call, and the load sets the base to the
 * descriptor's, 0; arch_prctl and clone write the record. The
 * directory is compared with the one this processor has loaded, never with
 * the outgoing record's: a thread that gave up its address space at its end
 * still runs in it until it switches away, and the directory may be freed
 * once it has.
 */
static void arch_bases_save(process_t *p)
{
    p->regs.fs_sel = (uint16_t)x86_64_fs_read();
    p->regs.gs_sel = (uint16_t)x86_64_gs_read();
    p->tls_base = msr_read(MSR_FS_BASE);
    p->regs.gs_base = msr_read(MSR_KERNEL_GS_BASE);
}

/** Load `p`'s selectors where they differ from the ones in FS and GS, then
 * its bases. */
static void arch_bases_load(const process_t *p)
{
    if (x86_64_fs_read() != p->regs.fs_sel) {
        x86_64_fs_load(p->regs.fs_sel);
    }
    if (x86_64_gs_read() != p->regs.gs_sel) {
        x86_64_gs_load(p->regs.gs_sel);
    }
    msr_write(MSR_FS_BASE, p->tls_base);
    msr_write(MSR_KERNEL_GS_BASE, p->regs.gs_base);
}

void switch_context(process_t *old, process_t *new_proc)
{
    struct percpu *cpu = this_cpu();

    arch_bases_save(old);
    cpu_set_kernel_stack(new_proc->kernel_stack);
    arch_bases_load(new_proc);

    if (new_proc->space_root != 0 && (new_proc->page_dir != cpu->space || cpu->space_stale)) {
        cpu->space = new_proc->page_dir;
        cpu->space_stale = 0;
        arch_space_load(new_proc->space_root);
    }

    x86_64_switch(&old->regs.rsp, new_proc->regs.rsp);
}

/**
 * Prime a new thread's kernel stack for its first switch, which returns
 * into `entry_point`. From high to low address: a zero where `entry_point`'s
 * own return address would be, which also leaves the stack as a call would,
 * 8 bytes past a 16-byte boundary; `entry_point`; and the six registers
 * x86_64_switch pops, all zero.
 */
void arch_context_init(process_t *proc, void (*entry_point)(void))
{
    uint64_t *sp = (uint64_t *)proc->kernel_stack;
    int i;

    *--sp = 0;
    *--sp = (uint64_t)(uintptr_t)entry_point;
    for (i = 0; i < SWITCH_SAVED; i++) {
        *--sp = 0;
    }
    proc->regs.rsp = (uint64_t)(uintptr_t)sp;
}

/**
 * Lay out `child`'s kernel stack for its first switch, from the parent's
 * frame `params`: the frame, with 0 in RAX, fork's result, and the stack
 * pointer `newsp` a clone asks for, when it asks; under it `isr_return` and
 * the six registers x86_64_switch pops. The child returns to user mode
 * through `iretq`, with RCX and R11 as the parent's `syscall` left them.
 * Its FS and GS selectors and its GS base are the parent's as they are
 * now, read from the registers, as its FS base is (`arch_tls_get`), unless
 * `CLONE_SETTLS` gives it a base of its own (`arch_clone_tls`).
 */
void arch_fork_frame(process_t *child, const isr_params_t *params, uintptr_t newsp)
{
    uint64_t *sp = (uint64_t *)child->kernel_stack;
    isr_params_t *frame;
    int i;

    sp -= sizeof(isr_params_t) / sizeof(uint64_t);
    frame = (isr_params_t *)sp;
    k_memcpy(frame, params, sizeof(isr_params_t));
    SC_RET(frame) = 0;
    if (newsp) {
        FRAME_SP(frame) = newsp;
    }
    *--sp = (uint64_t)(uintptr_t)isr_return;
    for (i = 0; i < SWITCH_SAVED; i++) {
        *--sp = 0;
    }
    child->regs.rsp = (uint64_t)(uintptr_t)sp;
    if (current_proc) {
        child->regs.fs_sel = (uint16_t)x86_64_fs_read();
        child->regs.gs_sel = (uint16_t)x86_64_gs_read();
        child->regs.gs_base = msr_read(MSR_KERNEL_GS_BASE);
    }
}

/**
 * Make the frame start a new program at `entry` with the stack pointer
 * `sp`: every general register zero, RDX among them, which the x86-64 ELF
 * ABI reads as a function for atexit when it is not; the user's selectors;
 * interrupts on and every other flag clear. FS and GS take the null
 * selector and the GS base goes back to 0; the caller sets the FS base,
 * after the load here.
 */
void arch_exec_frame(isr_params_t *p, uintptr_t entry, uintptr_t sp)
{
    k_memset(p, 0, (size_t)((char *)&p->vector - (char *)p));
    p->rip = entry;
    p->rsp = sp;
    p->cs = GDT_USER_CODE;
    p->ss = GDT_USER_DATA;
    p->rflags = RFLAGS_START;
    if (current_proc) {
        current_proc->regs.fs_sel = 0;
        current_proc->regs.gs_sel = 0;
        current_proc->regs.gs_base = 0;
    }
    x86_64_fs_load(0);
    x86_64_gs_load(0);
    msr_write(MSR_KERNEL_GS_BASE, 0);
}

/**
 * Enter user mode at `pc` with the stack pointer `sp`. The thread entering
 * was not switched to, so its kernel stack, selectors and bases are loaded
 * here, as `switch_context` would have loaded them.
 */
void arch_enter_user(uintptr_t pc, uintptr_t sp)
{
    if (current_proc) {
        cpu_set_kernel_stack(current_proc->kernel_stack);
        arch_bases_load(current_proc);
    }
    x86_64_enter_user(pc, sp);
}
