/**
 * Host test of the work of a return to user mode, `isr_leave_work`
 * (arch/x86/leave.c), which i686's, x86-64's and the i386 entry's returns
 * on x86-64 all run.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 *
 * The loop is the kernel's; the interrupt flag, the big lock, the delivery
 * of signals and the rseq update are the test's. Its delivery takes
 * SIGKILL first and otherwise gives the lowest deliverable signal a handler
 * frame and blocks it, and it computes the signal-work flag (`sig_woken`)
 * after each call as the kernel's `sig_recalc` does. A post from another
 * processor sets a signal pending, the flag when the signal is not blocked,
 * and WORK_SIGNAL, as `sig_wake` does; a row makes it in the middle of a
 * frame's copy or of the rseq area's, which the kernel makes with
 * interrupts on and may sleep in.
 */
#include "arch/x86/leave.c"
#include "lib/kstring.h"

extern int printf(const char *format, ...);

uint32_t proc_dead_threads;
static process_t thread;
static isr_params_t frame;
static int failures;

/* The modeled interrupt flag, 1 when interrupts are on. */
static int irq_on;
/* The modeled big lock's depth. */
static int bkl_depth;

/* Where a row posts its signal from another processor: nowhere, during a
 * handler frame's copy, or during the rseq area's. */
enum post_at { POST_NONE, POST_FRAME, POST_RSEQ };
static enum post_at post_at;
static int post_signo;

/* What the model's delivery did: the calls, the frames by signal in the
 * order made, and whether SIGKILL ended the thread. */
static int deliveries;
static int frames[8];
static int nframes;
static int killed;
static int rseq_writes;

/** Record a failed row without ending the other independent rows. */
static void check(int ok, const char *row)
{
    if (!ok) {
        printf("leave-hosttest: fail: %s\n", row);
        failures++;
    }
}

/** Read the modeled flags and turn interrupts off. */
uint32_t kirq_save(void)
{
    uint32_t flags = irq_on ? 0x200u : 0u;

    irq_on = 0;
    return flags;
}

/** Put the modeled interrupt flag back as `flags` says. */
void kirq_restore(uint32_t flags)
{
    irq_on = (flags & 0x200u) != 0;
}

/** Take the modeled big lock. */
void bkl_enter(void)
{
    bkl_depth++;
}

/** Give the modeled big lock back. */
void bkl_exit(void)
{
    bkl_depth--;
}

/** No datagram waits in these rows. */
void syscall_loop_drain(void)
{}

/** No switch is owed in these rows. */
void sched_take_preempt(void)
{}

/** The debug registers are not the rows' concern. */
void debugreg_resume(void)
{}

/** No timed wake is recorded in these rows. */
void waketime_return(void)
{}

/** The signals deliverable to the thread now. */
static uint64_t deliverable(void)
{
    return thread.sig_pending & ~thread.sig_blocked;
}

/** Compute the signal-work flag as `sig_recalc` does, with no stop. */
static void recalc(void)
{
    thread.sig_woken = deliverable() != 0 || (thread.ptrace & PT_INTERRUPT);
    if (thread.sig_woken) {
        proc_work_set(&thread, WORK_SIGNAL);
    }
}

/** Post `signo` to the thread from another processor, as `sig_wake`. */
static void post(int signo)
{
    thread.sig_pending |= KSIG_BIT(signo);
    if (deliverable() & KSIG_BIT(signo)) {
        thread.sig_woken = 1;
    }
    if (thread.sig_woken) {
        proc_work_set(&thread, WORK_SIGNAL);
    }
}

/** Post the tick's signals, with interrupts off, as the kernel's does. */
void proc_tick_signals_own(void)
{
    check(!irq_on, "the tick's signals are posted with interrupts off");
    if (thread.tick_sigs) {
        thread.tick_sigs = 0;
        post(SIGALRM);
    }
}

/** Whether the return has work for the delivery, as the kernel's
 * `signal_return_work` reads it, with a tracer's attachment among it. */
int signal_return_work(const isr_params_t *tf)
{
    (void)tf;
    return thread.sig_woken || deliverable() != 0 || thread.ptrace != 0 || thread.tick_sigs;
}

/**
 * Deliver as the kernel's `deliver_signals` does, with interrupts on and
 * the lock held: SIGKILL ends the thread, which the model marks and stops
 * acting on; otherwise the lowest deliverable signal gets a frame, during
 * whose copy a row may post, and is blocked, and the flag is computed
 * again. A runaway loop is cut off at 16 calls.
 */
void deliver_signals(isr_params_t *tf)
{
    int signo;

    (void)tf;
    check(irq_on && bkl_depth > 0, "delivery runs with interrupts on and the lock held");
    if (++deliveries > 16) {
        check(0, "the return's loop ends");
        thread.sig_pending = 0;
        recalc();
        return;
    }
    if (deliverable() & KSIG_BIT(SIGKILL)) {
        killed = 1;
        thread.sig_pending = 0;
        thread.ptrace = 0;
        thread.rseq_notify = 0;
        recalc();
        return;
    }
    if (deliverable() == 0) {
        recalc();
        return;
    }
    for (signo = 1; !(deliverable() & KSIG_BIT(signo)); signo++) {
    }
    thread.sig_pending &= ~KSIG_BIT(signo);
    recalc();
    if (post_at == POST_FRAME) {
        post_at = POST_NONE;
        post(post_signo);
    }
    if (nframes < 8) {
        frames[nframes++] = signo;
    }
    thread.sig_blocked |= KSIG_BIT(signo);
    recalc();
}

/** Update the rseq area as the kernel's `rseq_return` does, with
 * interrupts on, when a notice asks; a row may post during the copy. */
void rseq_return(isr_params_t *params)
{
    (void)params;
    check(irq_on, "the rseq area is written with interrupts on");
    if (!thread.rseq_notify) {
        return;
    }
    if (post_at == POST_RSEQ) {
        post_at = POST_NONE;
        post(post_signo);
    }
    thread.rseq_notify = 0;
    rseq_writes++;
}

/** Start a row: a thread with no work, interrupts off, no lock, and a
 * post from another processor of `signo` at `at`. */
static void setup(enum post_at at, int signo)
{
    k_memset(&thread, 0, sizeof(thread));
    k_memset(&frame, 0, sizeof(frame));
    irq_on = 0;
    bkl_depth = 0;
    post_at = at;
    post_signo = signo;
    deliveries = 0;
    nframes = 0;
    killed = 0;
    rseq_writes = 0;
}

/** Return to user mode as `isr_leave` does for an entry that ran without
 * the big lock, and check what every return leaves. */
static void leave(void)
{
    isr_leave_work(&frame, &thread, 0);
    check(!irq_on, "the return ends with interrupts off");
    check(bkl_depth == 0, "the return gives the lock back");
    check(thread.in_syscall == 0, "the return puts the call mark back");
}

/** A SIGKILL posted while a handler's frame is copied ends the thread
 * before the handler runs, and one posted while the rseq area is copied
 * ends it before user code resumes. */
static void kill_rows(void)
{
    setup(POST_FRAME, SIGKILL);
    post(SIGUSR1);
    leave();
    check(nframes == 1 && frames[0] == SIGUSR1, "the handler's frame is made");
    check(killed, "a SIGKILL posted during the frame's copy ends the thread before the return");
    check(!(thread.sig_pending & KSIG_BIT(SIGKILL)), "no SIGKILL is left for the handler");

    setup(POST_RSEQ, SIGKILL);
    thread.rseq_notify = 1;
    proc_work_set(&thread, WORK_RSEQ);
    leave();
    check(rseq_writes == 1 && nframes == 0, "the rseq area is written and no frame made");
    check(killed, "a SIGKILL posted during the rseq copy ends the thread before the return");

    setup(POST_RSEQ, SIGKILL);
    thread.rseq_notify = 1;
    post(SIGUSR2);
    leave();
    check(nframes == 1 && frames[0] == SIGUSR2 && killed,
          "a SIGKILL posted during the rseq copy after a frame ends the thread");
}

/** A second signal, pending beside the first or posted during the first's
 * frame, gets a frame above it before the return, as Linux; one that the
 * handler blocks waits for its sigreturn. */
static void frame_rows(void)
{
    setup(POST_NONE, 0);
    post(SIGUSR1);
    post(SIGUSR2);
    leave();
    check(nframes == 2 && frames[0] == SIGUSR1 && frames[1] == SIGUSR2 && !killed,
          "two pending signals each get a frame before the return");
    check(thread.sig_pending == 0 && !thread.sig_woken, "no signal work is left");

    setup(POST_FRAME, SIGUSR2);
    post(SIGUSR1);
    leave();
    check(nframes == 2 && frames[1] == SIGUSR2,
          "a signal posted during a frame's copy gets a frame");

    setup(POST_FRAME, SIGUSR1);
    post(SIGUSR1);
    leave();
    check(nframes == 1 && (thread.sig_pending & KSIG_BIT(SIGUSR1)) && !thread.sig_woken,
          "a signal the handler blocks stays pending for its sigreturn");
    check(deliveries == 1, "a blocked signal sends no work round");

    setup(POST_NONE, 0);
    thread.tick_sigs = 1;
    proc_work_set(&thread, WORK_SIGNAL);
    leave();
    check(nframes == 1 && frames[0] == SIGALRM, "a tick's signal gets its frame");
}

/** A tracer's attachment, which stays after a delivery, sends no work
 * round, and keeps WORK_SIGNAL and WORK_TRACE for the next return. */
static void trace_rows(void)
{
    setup(POST_NONE, 0);
    thread.ptrace = PT_TRACED | PT_SYSCALL;
    post(SIGUSR1);
    leave();
    check(nframes == 1 && deliveries == 1, "a traced thread's return delivers once");
    check((thread.work & (WORK_SIGNAL | WORK_TRACE)) == (WORK_SIGNAL | WORK_TRACE),
          "a tracer's bits stay for the next return");

    setup(POST_FRAME, SIGKILL);
    thread.ptrace = PT_TRACED | PT_SYSCALL;
    post(SIGUSR1);
    leave();
    check(killed && deliveries == 2, "a traced thread's SIGKILL posted during a copy ends it");
}

int main(void)
{
    kill_rows();
    frame_rows();
    trace_rows();
    if (failures) {
        printf("leave-hosttest: %d failures\n", failures);
        return 1;
    }
    printf("leave-hosttest: ok\n");
    return 0;
}
