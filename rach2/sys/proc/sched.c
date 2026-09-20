#include "sched.h"

#include "../lib/kirq.h"
#include "../network/ip/ip.h"

/** Nonzero once preemptive scheduling is enabled; schedule() is a no-op
 * until then. */
static volatile int _sched_enabled = 0;

/** The last ordinary selection; a wake preference does not move it. */
static process_t *_last_sched;

/** A preferred selection must be followed by an ordinary selection. */
static int _wake_turn;

/** Remove a departing record from the round-robin cursor. */
void sched_forget(process_t *p)
{
    if (_last_sched == p) {
        _last_sched = p->prev;
    }
}

void init_sched(void)
{
    _sched_enabled = 0;
    _last_sched = NULL;
    _wake_turn = 1;
}

void sched_enable(void)
{
    _sched_enabled = 1;
}

void sched_disable(void)
{
    _sched_enabled = 0;
}

int sched_is_enabled(void)
{
    return _sched_enabled;
}

/**
 * Find a READY process after the ordinary cursor, wrapping once.
 *
 * With `woken_only`, consider only wait-queue wakeups. Interrupts are off.
 */
static process_t *sched_find(int woken_only)
{
    process_t *start;
    process_t *p;

    start = _last_sched && _last_sched->next ? _last_sched->next : proc_list;
    p = start;
    if (!p) {
        return NULL;
    }
    do {
        if (p->state == PROC_READY && (!woken_only || p->woken)) {
            return p;
        }
        p = p->next ? p->next : proc_list;
    } while (p != start);
    return NULL;
}

/**
 * Alternate a wake preference with an ordinary round-robin selection.
 *
 * A preferred selection consumes its wake mark without moving the ordinary
 * cursor. At least every second selection advances that cursor, even when
 * sleepers wake repeatedly. A missing wake candidate takes an ordinary turn.
 * Selection and publication run with interrupts disabled so a wake cannot
 * race the mark's consumption or a timer re-enter partially updated state.
 * A current process that has already resumed after an idle wait loses its
 * mark before yielding, so that wake cannot prefer it again later.
 *
 * Does nothing if scheduling is disabled, there is no current process, or no
 * other process is READY. The switch runs with interrupts disabled: a timer
 * IRQ landing between publishing current_proc and the stack and CR3 swap
 * inside switch_context would see current_proc == next with next->in_syscall
 * == 0, pass the timer handler's reschedule test, and re-enter schedule() on
 * the old stack with the TLS descriptor already rewritten, corrupting
 * next->regs.esp. So schedule() saves the caller's IF, clears it across the
 * switch, and restores it afterward. Each process does this symmetrically
 * and resumes with its own IF; a process entered for the first time skips
 * the restore and takes its IF from iret instead.
 *
 * The floating-point registers are the one part of the processor
 * `switch_context` does not touch. They are saved into the outgoing record
 * and loaded from the incoming one here, inside the same masked window and
 * before the stack pointer changes; the kernel computes in none of them, so
 * nothing between the load and the switch can disturb the loaded state.
 */
void schedule(void)
{
    process_t *next = NULL;
    process_t *old;
    uint32_t saved_flags;

    if (!_sched_enabled) {
        return;
    }

    if (!current_proc) {
        return;
    }

    saved_flags = kirq_save();
    if (current_proc->state != PROC_READY) {
        current_proc->woken = 0;
    }
    if (_wake_turn) {
        next = sched_find(1);
    }
    if (next) {
        _wake_turn = 0;
    } else {
        next = sched_find(0);
        if (next) {
            _last_sched = next;
            _wake_turn = 1;
        }
    }

    if (!next) {
        kirq_restore(saved_flags);
        return;
    }

    next->woken = 0;
    if (next == current_proc) {
        next->state = PROC_RUNNING;
        kirq_restore(saved_flags);
        return;
    }

    old = current_proc;

    if (old->state == PROC_RUNNING) {
        old->state = PROC_READY;
    }

    next->state = PROC_RUNNING;

    fpu_save(old->fpu);
    fpu_restore(next->fpu);
    current_proc = next;

    switch_context(old, next);

    kirq_restore(saved_flags);
}

/**
 * Preempt from an interrupt handler.
 *
 * A process interrupted in user mode, the idle loop and a kernel thread are
 * switched at once. A process inside a system call is not: the call's exit
 * path and the scheduler itself run with interrupts on while their own
 * bookkeeping is half done, and a switch taken there strands the process
 * whose state they were in the middle of changing. The request is kept on
 * the process, and `sched_take_preempt` takes it at the call's exit, so a
 * process that is always inside a call still gives way once per call.
 */
void sched_preempt(void)
{
    if (current_proc && current_proc->in_syscall) {
        current_proc->need_resched = 1;
        return;
    }
    schedule();
}

/**
 * Take the switch an interrupt asked for during the system call that is
 * ending. Called at the call's exit with interrupts off and the trapframe
 * whole; the request is cleared before the switch, so a request that
 * arrives during the switch is the next call's.
 */
void sched_take_preempt(void)
{
    if (current_proc && current_proc->need_resched) {
        current_proc->need_resched = 0;
        schedule();
    }
}

/**
 * Give way inside a system call that does not return for a while.
 *
 * A call that loops over a large buffer keeps the processor for the whole
 * loop, since the tick's switch is taken at the call's exit; a reader woken
 * by a keystroke then waits for the loop. Between two pieces of such a
 * loop, with nothing half done, the call takes the switch here as its exit
 * would: interrupts off, the request cleared before the switch, and the
 * process still inside its call.
 */
void sched_service_point(void)
{
    uint32_t flags = kirq_save();

    sched_take_preempt();
    kirq_restore(flags);
}

/* The idle marker and the two tick counters. `volatile` because the timer
 * handler reads the marker from an interrupt that arrives in the middle of the
 * halt it describes. The counters are 32-bit, which is one aligned store on
 * this machine, so a reader never sees half of one; at 100 Hz they wrap after
 * about 497 days, the bound `kernel_ticks` already carries. */
static volatile int _in_idle_wait = 0;
static volatile uint32_t _idle_ticks = 0;
static volatile uint32_t _busy_ticks = 0;

uint32_t sched_idle_ticks(void)
{
    return _idle_ticks;
}

uint32_t sched_busy_ticks(void)
{
    return _busy_ticks;
}

void sched_account_tick(void)
{
    if (_in_idle_wait) {
        ++_idle_ticks;
    } else {
        ++_busy_ticks;
    }
}

/**
 * Park the processor until an interrupt, and return with interrupts disabled.
 *
 * Every caller of this one enters with IF already clear, having checked its
 * condition there, and that is what makes the wait free of a lost wakeup:
 * `sti; hlt` issued from IF=0 is atomic against a wake that became pending
 * during the check, because the interrupt fires the instant after `hlt` and
 * never before it. The marker is set while interrupts are still off, so no
 * tick can be delivered between setting it and the halt it describes.
 *
 * Anything on the loopback ring is delivered before the processor is parked,
 * and this returns without halting so the caller checks its condition again.
 * Those datagrams are this machine's own, a delivery can be what makes a
 * process runnable, and nothing else is going to run to deliver them. A
 * system call's exit delivers only once the ring holds three or more, since
 * one or two may be the crossing SYNs of a simultaneous open; a single
 * datagram, which is what a window update or a bare acknowledgement is, would
 * otherwise wait for the timer, and a local connection would turn over one
 * window every ten milliseconds. Interrupts are off here, which is the
 * discipline the drain needs, and a caller that is about to halt holds nothing
 * and is between operations, which is the state the timer's drain relies on
 * too.
 */
void idle_wait_irq_off(void)
{
    if (ip_loop_pending()) {
        ip_loop_drain_from(IP_LOOP_FROM_IDLE);
        return;
    }
    _in_idle_wait = 1;
#ifndef RACH2_HOSTTEST
    __asm__ __volatile__("sti; hlt; cli");
#endif
    _in_idle_wait = 0;
}

/**
 * Park the processor until an interrupt, and return with interrupts enabled.
 *
 * The callers of this one are already running with IF set, so an interrupt can
 * arrive at any instruction and the `sti` changes nothing; a wake that lands
 * between the marker and the `hlt` costs one more tick of waiting, which is why
 * every such caller re-checks its condition in a loop rather than assuming the
 * halt ended for its own reason. That was true of the bare `sti; hlt` these
 * replaced and is not something the marker introduced. The tick the marker
 * catches in that window is counted idle, which is what the processor is about
 * to be; the error is at most one tick per wait.
 */
void idle_wait_irq_on(void)
{
    _in_idle_wait = 1;
#ifndef RACH2_HOSTTEST
    __asm__ __volatile__("sti; hlt");
#endif
    _in_idle_wait = 0;
}
