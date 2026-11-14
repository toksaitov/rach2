/*
 * leave.c: the work of a return to user mode, the part of the return from a
 * router (`isr_leave`, isr.c) that its thread's work bits call for.
 */

#include "leave.h"
#include "debugreg.h"
#include "../../syscall.h"
#include "../../lib/kirq.h"
#include "../../proc/process.h"
#include "../../proc/ptrace.h"
#include "../../proc/rseq.h"
#include "../../proc/sched.h"
#include "../../proc/smp.h"

/**
 * The return to user mode's work, which the thread's work bits or threads
 * ended and not yet freed (`proc_dead_threads`) call for, with interrupts
 * off and the big lock held when `locked` says the entry took it, which
 * this gives back, as Linux's `exit_to_user_mode_loop` acts only on the
 * work its flags name (kernel/entry/common.c:90-127). The bits are cleared
 * before the work they stand for is tested, so a bit set meanwhile stays
 * for the next return. In order: the loopback backlog's datagrams
 * (`syscall_loop_drain`) and a switch the thread owes
 * (`sched_take_preempt`); then, under the big lock, taken here when the
 * entry ran without it, the signals of the tick the thread has waiting
 * (`proc_tick_signals_own`), a signal to deliver and the thread's rseq
 * area (`rseq_return`), when `signal_return_work` or `rseq_notify` says so;
 * then the debug registers (`debugreg_resume`) and a timed wake's record
 * (`waketime_return`).
 *
 * The user copies of signal and rseq work run with interrupts on, and may
 * sleep on a fault. When interrupts go off again, a migration, a tick's
 * signal or a signal to act on that came meanwhile (`sig_woken`, Linux's
 * TIF_SIGPENDING) makes the signal and rseq work run again, as Linux
 * rereads its flags with interrupts off and goes round again for
 * _TIF_SIGPENDING (include/linux/entry-common.h:65,
 * kernel/entry/common.c:122-132): so a SIGKILL posted while a handler's
 * frame or the rseq area is copied ends the thread before the handler
 * runs, and a second signal pending beside the one a frame was made for
 * gets its own frame, which its handler runs first. A tracer's attachment,
 * a stop the thread has taken and a mask a wait saved stay after a
 * delivery and send no work round. Then the bits are taken again, so that
 * a switch, a datagram or an rseq notice that came meanwhile sends the
 * whole work round once more.
 *
 * The work is marked in a call, as a user fault is, so a timer defers its
 * switch and cannot race the rseq event fetch and clear; a fault may still
 * sleep. Work that stays, a tracer's and a signal flag a delivery left set,
 * keeps its bits for the next return. Not inline in `isr_leave`, whose
 * return with no work is the common one.
 */
K_NOINLINE void isr_leave_work(isr_params_t *params, process_t *p, int locked)
{
    unsigned int work = proc_work_take(p, WORK_EXIT), keep = 0;
    uint32_t flags;
    uint32_t was_in_call;
    int signalled = 0;

    for (;;) {
        if (work & WORK_LOOP) {
            syscall_loop_drain();
        }
        if (work & WORK_RESCHED) {
            sched_take_preempt();
        }
        if (!signal_return_work(params) && !p->rseq_notify) {
            break;
        }
        if (!locked) {
            bkl_enter();
            locked = 1;
        }
        was_in_call = p->in_syscall;
        p->in_syscall = 1;
        flags = kirq_save();
        do {
            proc_tick_signals_own();
            kirq_restore(flags | 0x200u);
            deliver_signals(params);
            rseq_return(params);
            flags = kirq_save();
        } while (p->rseq_notify || p->tick_sigs || p->sig_woken);
        p->in_syscall = was_in_call;
        signalled = 1;
        work = (work & (WORK_DEBUGREG | WORK_WAKETIME)) |
               proc_work_take(p, WORK_EXIT & ~(WORK_SIGNAL | WORK_TRACE));
        if (!(work & (WORK_RESCHED | WORK_LOOP)) && !p->rseq_notify) {
            break;
        }
    }
    if (signalled && signal_return_work(params)) {
        keep |= WORK_SIGNAL;
    }
    if (signalled && (p->ptrace & (PT_SYSCALL | PT_STEP))) {
        keep |= WORK_TRACE;
    }
    if (work & WORK_DEBUGREG) {
        debugreg_resume();
    }
    if (work & WORK_WAKETIME) {
        waketime_return();
    }
    if (keep) {
        proc_work_set(p, keep);
    }
    if (locked) {
        bkl_exit();
    }
}
