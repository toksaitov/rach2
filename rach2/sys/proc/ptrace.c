/*
 * ptrace.c: process tracing, as GDB and strace use it. See ptrace.Readme.md.
 */

#include "ptrace.h"

#include "ns.h"
#include "pidfd.h"
#include "sched.h"
#include "sigframe.h"
#include "tgroup.h"
#include "../fs/krffs.h"
#include "../fs/vfs.h"
#include "../lib/kerrno.h"
#include "../lib/kirq.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"
#include "../mm/mm.h"
#include "../mm/paging.h"
#include "../mm/usercopy.h"
#include "../syscall.h"
#include "../syscall_internal.h"

/* The requests. */
#define PTRACE_TRACEME          0
#define PTRACE_PEEKTEXT         1
#define PTRACE_PEEKDATA         2
#define PTRACE_POKETEXT         4
#define PTRACE_POKEDATA         5
#define PTRACE_CONT             7
#define PTRACE_KILL             8
#define PTRACE_SINGLESTEP       9
#define PTRACE_ATTACH           16
#define PTRACE_DETACH           17
#define PTRACE_SYSCALL          24
#define PTRACE_SETOPTIONS       0x4200
#define PTRACE_GETEVENTMSG      0x4201
#define PTRACE_GETSIGINFO       0x4202
#define PTRACE_SETSIGINFO       0x4203
#define PTRACE_GETREGSET        0x4204
#define PTRACE_SETREGSET        0x4205
#define PTRACE_SEIZE            0x4206
#define PTRACE_INTERRUPT        0x4207
#define PTRACE_LISTEN           0x4208
#define PTRACE_GET_SYSCALL_INFO 0x420E

/* The event message of a system call stop, at its entry and at its exit. */
#define PTRACE_EVENTMSG_SYSCALL_ENTRY 1u
#define PTRACE_EVENTMSG_SYSCALL_EXIT  2u

/* The options: an event's bit is 1 << the event, and the rest. */
#define PTRACE_O_TRACESYSGOOD 0x00000001u
#define PTRACE_O_TRACEEXEC    0x00000010u
#define PTRACE_O_EXITKILL     0x00100000u
#define PTRACE_O_MASK         0x001000FFu

/** Whether the current thread has SIGKILL pending, its own or its process's. */
static int kill_pending(void)
{
    return ((current_proc->sig_pending | current_proc->tg->pending) & KSIG_BIT(SIGKILL)) != 0;
}

/**
 * Stop the current thread for its tracer with the wait status `status` until
 * the tracer resumes it, or SIGKILL ends the stop, and return the signal the
 * resume gave, 0 for none or after SIGKILL.
 *
 * The tracer's wait reports the stop once; a SIGCHLD and a wake of the
 * tracer's child queue tell it of the stop. `info` is the siginfo
 * PTRACE_GETSIGINFO reads; NULL, for a group stop of a thread not seized,
 * leaves the stop without one, and the request fails with -EINVAL, as
 * Linux's do_jobctl_trap passes ptrace_stop no siginfo. A thread
 * whose tracer has gone does not stop: it is no longer traced, and gets the
 * stop's own signal back, as though the tracer had resumed it with that.
 */
static int ptrace_stop(uint32_t status, const struct k_siginfo *info)
{
    process_t *self = current_proc;
    process_t *tracer = get_process(self->tracer);
    struct k_siginfo chld;
    uint32_t flags;

    if (tracer == NULL || tracer->tg->live == 0) {
        self->ptrace = 0;
        self->tracer = 0;
        return (int)((status >> 8) & 0x7F);
    }
    if (info) {
        self->ptrace_info = *info;
        self->ptrace_info_signo = (int32_t)((status >> 8) & 0x7F);
        self->ptrace &= ~PT_NOINFO;
    } else {
        k_memset(&self->ptrace_info, 0, sizeof(self->ptrace_info));
        self->ptrace |= PT_NOINFO;
    }
    self->ptrace_signo = 0;
    self->ptrace_reported = 0;
    self->ptrace_status = status;
    k_memset(&chld, 0, sizeof(chld));
    chld.code = CLD_TRAPPED;
    chld.pid = pid_seen_by(tracer, self->pid);
    chld.uid = ns_uid_from_kernel(proc_userns(tracer), self->ruid);
    chld.status = (int32_t)((status >> 8) & 0xFF);
    signal_post_info(tracer, SIGCHLD, &chld);
    wq_wake_all(&tracer->tg->chld);

    flags = kirq_save();
    for (;;) {
        sched_set_current_state(PROC_STOPPED);
        if (self->ptrace_status == 0 || kill_pending()) {
            break;
        }
        schedule();
        if (current_proc->state == PROC_STOPPED) {
            idle_wait_irq_off();
        }
    }
    self->state = PROC_RUNNING;
    kirq_restore(flags);
    if (self->ptrace_status != 0) {
        self->ptrace_status = 0;
        return 0;
    }
    return self->ptrace_signo;
}

/**
 * Stop the current thread for its tracer with the wait status `status`, for
 * a stop that no signal makes, with the siginfo Linux 6.12's
 * ptrace_do_notify gives it (kernel/signal.c): the stop's signal, the code
 * of the status's bits 8-23, `SIGTRAP | event << 8` for an event stop, and
 * the thread's own ID and real user ID as its namespaces see them, in a
 * union kept whole (`siginfo_pack`), where SIL_KILL places them: a plain
 * system call stop's code, SIGTRAP, is a fault's TRAP_UNK, so that a
 * native siginfo copies them as Linux copies its union whole, and the i386
 * one reads the union as a fault's address. Returns what `ptrace_stop`
 * returns.
 */
static int ptrace_notify(uint32_t status)
{
    process_t *self = current_proc;
    struct k_siginfo info;

    k_memset(&info, 0, sizeof(info));
    info.code = (int32_t)((status >> 8) & 0xFFFFu);
    info.pid = pid_seen_by(self, self->pid);
    info.uid = ns_uid_from_kernel(proc_userns(self), self->ruid);
    siginfo_pack(&info, SIL_KILL);
    return ptrace_stop(status, &info);
}

/** The flags are read again after each stop, since the tracer may request
 * another while the thread is stopped; a pending SIGKILL ends the loop, so
 * that signal delivery can end the thread. */
void ptrace_return_stops(void)
{
    process_t *self = current_proc;
    uint32_t signo;

    while ((self->ptrace & PT_TRACED) && (self->ptrace & (PT_START | PT_INTERRUPT)) &&
           !kill_pending()) {
        signo = self->tg->stop ? self->tg->stop : SIGTRAP;
        self->ptrace &= ~(PT_START | PT_INTERRUPT);
        (void)ptrace_notify(((uint32_t)PTRACE_EVENT_STOP << 16) | (signo << 8) | 0x7F);
    }
}

/**
 * Fill `info` as the siginfo of a signal the current thread's tracer gave it
 * on resuming it: SI_USER, with the tracer's process ID and real user ID as
 * the thread's namespaces see them, 0 for a tracer that has gone.
 */
static void tracer_info(struct k_siginfo *info)
{
    process_t *self = current_proc;
    const process_t *tracer = self->tracer ? get_process(self->tracer) : NULL;

    k_memset(info, 0, sizeof(*info));
    info->code = SI_USER;
    if (tracer) {
        info->pid = pid_seen_by(self, self->tracer);
        info->uid = ns_uid_from_kernel(proc_userns(self), tracer->ruid);
    }
}

int ptrace_signal_stop(int signo, struct k_siginfo *info)
{
    process_t *self = current_proc;
    int sig;

    if (!(self->ptrace & PT_TRACED) || signo == SIGKILL) {
        return signo;
    }
    sig = ptrace_stop(((uint32_t)signo << 8) | 0x7F, info);
    if (sig <= 0 || sig > KSIG_MAX) {
        return 0;
    }
    if (sig == self->ptrace_info_signo) {
        *info = self->ptrace_info;
    } else {
        tracer_info(info);
    }
    if (self->sig_blocked & KSIG_BIT(sig)) {
        signal_post_thread(self, sig, info);
        return 0;
    }
    return sig;
}

int ptrace_group_stop(int signo)
{
    process_t *self = current_proc;
    uint32_t status;

    if (!(self->ptrace & PT_TRACED) || self->stop_seen) {
        return 0;
    }
    self->stop_seen = 1;
    self->ptrace &= ~PT_INTERRUPT;
    status = ((uint32_t)signo << 8) | 0x7F;
    if (self->ptrace & PT_SEIZED) {
        (void)ptrace_notify(status | (uint32_t)PTRACE_EVENT_STOP << 16);
    } else {
        (void)ptrace_stop(status, NULL);
    }
    ptrace_return_stops();
    return 1;
}

/**
 * Mark signal work for the thread `t`, which has a stop to take: one its
 * tracer requested (`PT_INTERRUPT`), or its process's, which a detach
 * leaves it owing, and wake it if it sleeps, as Linux 6.12's
 * ptrace_signal_wake_up sets TIF_SIGPENDING before signal_wake_up_state's
 * wake (include/linux/sched/signal.h, kernel/signal.c:759-773). A
 * wait without the big lock ends only on the flag (`sig_woken`), and asks
 * `signal_pending`, which answers yes for the request, only once it is set;
 * a wake without it finds the wait's condition false and sleeps again. The
 * work's barrier comes between the flag and the read of the state, which a
 * sleeper sets before it tests the flag. The big lock is held.
 */
static void ptrace_signal_wake(process_t *t)
{
    t->sig_woken = 1;
    proc_work_set(t, WORK_SIGNAL);
    if (t->state == PROC_BLOCKED) {
        sched_wake_signal(t);
    }
}

/**
 * Request a new PTRACE_EVENT_STOP of the seized thread `t`, and wake it if
 * it sleeps, as Linux's ptrace_trap_notify does. A thread that
 * PTRACE_LISTEN keeps in its stop is let out of it at once, so that it
 * makes the new one.
 */
static void ptrace_restop(process_t *t)
{
    t->ptrace |= PT_INTERRUPT;
    ptrace_signal_wake(t);
    if ((t->ptrace & PT_LISTEN) && t->ptrace_status != 0) {
        t->ptrace &= ~PT_LISTEN;
        t->ptrace_signo = 0;
        t->ptrace_status = 0;
        if (t->state == PROC_STOPPED) {
            sched_make_ready(t);
        }
    }
}

void ptrace_group_change(struct tgroup *tg)
{
    process_t *t;

    for (t = proc_list; t; t = t->next) {
        if (t->tg == tg && (t->ptrace & (PT_TRACED | PT_SEIZED)) == (PT_TRACED | PT_SEIZED)) {
            ptrace_restop(t);
        }
    }
}

void ptrace_event(int event, uint32_t msg)
{
    process_t *self = current_proc;

    if (!(self->ptrace & PT_TRACED) || !(self->ptrace_opts & (1u << event))) {
        return;
    }
    self->ptrace_msg = msg;
    (void)ptrace_notify(((uint32_t)event << 16) | ((uint32_t)SIGTRAP << 8) | 0x7F);
}

void ptrace_syscall_stop(isr_params_t *tf, uint32_t nr, int leaving)
{
    process_t *self = current_proc;
    uint32_t signo = SIGTRAP;
    struct k_siginfo info;
    int stepping = leaving && (self->ptrace & PT_STEP);
    int sig;

    if (!(self->ptrace & PT_TRACED) || (!(self->ptrace & PT_SYSCALL) && !stepping)) {
        return;
    }
    k_memset(&info, 0, sizeof(info));
    if (stepping) {
        info.code = TRAP_TRACE;
        info.addr = FRAME_PC(tf);
    } else if (self->ptrace_opts & PTRACE_O_TRACESYSGOOD) {
        signo |= 0x80u;
    }
    if (!leaving) {
        self->orig_nr = (int32_t)nr;
        SC_RET(tf) = (unsigned long)-ENOSYS;
    }
    if (!stepping) {
        self->ptrace_msg = leaving ? PTRACE_EVENTMSG_SYSCALL_EXIT : PTRACE_EVENTMSG_SYSCALL_ENTRY;
    }
    sig = stepping ? ptrace_stop((signo << 8) | 0x7F, &info) : ptrace_notify((signo << 8) | 0x7F);
    if (!stepping) {
        self->ptrace_msg = 0;
    }
    if (!leaving && ((self->sig_pending | self->tg->pending) & KSIG_BIT(SIGKILL))) {
        self->orig_nr = -1;
    }
    if (sig > 0 && sig <= KSIG_MAX) {
        if (stepping) {
            tracer_info(&info);
        } else {
            k_memset(&info, 0, sizeof(info));
            info.code = SI_KERNEL;
        }
        signal_post_thread(self, sig, &info);
    }
}

int ptrace_fork_attach(process_t *child, uint32_t flags)
{
    process_t *self = current_proc;
    int event;

    if (!(self->ptrace & PT_TRACED) || (flags & 0x00800000u)) { /* CLONE_UNTRACED */
        return 0;
    }
    if (flags & 0x00004000u) { /* CLONE_VFORK */
        event = PTRACE_EVENT_VFORK;
    } else if ((flags & 0xFFu) != SIGCHLD) {
        event = PTRACE_EVENT_CLONE;
    } else {
        event = PTRACE_EVENT_FORK;
    }
    if (!(self->ptrace_opts & (1u << event))) {
        return 0;
    }
    child->ptrace = self->ptrace & (PT_TRACED | PT_SEIZED);
    child->tracer = self->tracer;
    child->ptrace_opts = self->ptrace_opts;
    if (child->ptrace & PT_SEIZED) {
        child->ptrace |= PT_START;
    } else {
        child->sig_pending |= KSIG_BIT(SIGSTOP);
        k_memset(&child->sig_info[SIGSTOP], 0, sizeof(child->sig_info[SIGSTOP]));
        child->sig_info[SIGSTOP].code = SI_USER;
    }
    return event;
}

void ptrace_exec(uint32_t old_tid)
{
    process_t *self = current_proc;
    struct k_siginfo info;

    if (!(self->ptrace & PT_TRACED)) {
        return;
    }
    if (self->ptrace_opts & PTRACE_O_TRACEEXEC) {
        ptrace_event(PTRACE_EVENT_EXEC, old_tid);
    } else if (!(self->ptrace & PT_SEIZED)) {
        k_memset(&info, 0, sizeof(info));
        info.code = SI_USER;
        signal_post_thread(self, SIGTRAP, &info);
    }
}

void ptrace_exit_notify(process_t *p)
{
    process_t *tracer = get_process(p->tracer);
    struct k_siginfo info;

    if (!(p->ptrace & PT_TRACED) || tracer == NULL) {
        return;
    }
    proc_child_info(p, tracer, &info);
    if (p->pid != p->tgid || p->tg->live != 0 || p->parent_pid != p->tracer ||
        proc_child_signal(p, tracer)) {
        signal_post_info(tracer, SIGCHLD, &info);
    }
    wq_wake_all(&tracer->tg->chld);
}

/**
 * Tell the parent of the process `t` leads, which has ended and which its
 * tracer has let go, of the end, as the process's last thread tells the
 * parent of a process no other process traces: a SIGCHLD, and a wake of its
 * wait, which passed the process over while it was traced.
 */
static void ptrace_parent_notify(process_t *t)
{
    process_t *parent = get_process(t->parent_pid);
    struct k_siginfo info;

    if (parent == NULL) {
        return;
    }
    proc_child_info(t, parent, &info);
    if (proc_child_signal(t, parent)) {
        signal_post_info(parent, SIGCHLD, &info);
    }
    wq_wake_all(&parent->tg->chld);
}

/**
 * Take `t` from its tracer, as PTRACE_DETACH, a tracer's end and its wait's
 * report of a process's end do: the step flag the kernel set and the debug
 * registers go, at the tracee's next switch or return to user mode
 * (WORK_DEBUGREG), and a thread in a ptrace stop resumes with `sig`. A thread
 * that ended while traced, which its tracer would have freed, is left for the
 * next return to user mode to free, and its pidfds hang up now, as Linux's
 * __ptrace_detach releases it (`pidfd_id_gone`). A process that has ended,
 * which its parent's wait passed over while another process traced it, is
 * left to the parent, which is told of the end (`ptrace_parent_notify`); a
 * parent that is the tracer is ending itself, and the process goes to init
 * with its other children (`reparent_children`). A live thread whose process is
 * stopped owes the stop again, and is marked and woken to take it, as Linux
 * 6.12.115's __ptrace_unlink sets JOBCTL_STOP_PENDING and calls
 * ptrace_signal_wake_up (kernel/ptrace.c): a thread its tracer resumed
 * during the stop may sleep in a wait that ends only on its signal-work
 * flag, which nothing else would set.
 */
static void ptrace_detach(process_t *t, int sig)
{
    uint32_t tracer = t->tracer;

    if (t->ptrace & PT_STEP) {
        arch_ptrace_step(t, 0);
    }
    t->ptrace = 0;
    t->tracer = 0;
    t->ptrace_opts = 0;
    t->stop_seen = 0;
    k_memset(t->debugreg, 0, sizeof(t->debugreg));
    proc_work_set(t, WORK_DEBUGREG);
    if (t->state == PROC_ZOMBIE && t->pid != t->tgid) {
        proc_dead_threads++;
        pidfd_id_gone(t);
        return;
    }
    if (t->state == PROC_ZOMBIE && t->tg->live == 0) {
        if (t->parent_pid != tracer) {
            ptrace_parent_notify(t);
        }
        return;
    }
    if (t->ptrace_status != 0) {
        t->ptrace_signo = sig;
        t->ptrace_status = 0;
        if (t->state == PROC_STOPPED) {
            sched_make_ready(t);
        }
    }
    if (t->tg->stop && !t->exit_begun) {
        ptrace_signal_wake(t);
    }
}

void ptrace_exit_release(process_t *t)
{
    ptrace_detach(t, 0);
}

void ptrace_tracer_gone(uint32_t tgid)
{
    process_t *t;
    struct k_siginfo info;

    k_memset(&info, 0, sizeof(info));
    info.code = SI_KERNEL;
    for (t = proc_list; t; t = t->next) {
        if (!(t->ptrace & PT_TRACED) || t->tracer != tgid) {
            continue;
        }
        if (t->ptrace_opts & PTRACE_O_EXITKILL) {
            signal_post_thread(t, SIGKILL, &info);
        }
        ptrace_detach(t, 0);
    }
}

/** Resume the stopped thread `t` with the signal `sig`, stepping one
 * instruction with `step` and stopping at system calls with `syscalls`, and
 * clear PT_LISTEN; 0, or -EIO for a signal past KSIG_MAX. */
static int ptrace_resume(process_t *t, unsigned long sig, int step, int syscalls)
{
    if (sig > KSIG_MAX) {
        return -EIO;
    }
    if (step) {
        arch_ptrace_step(t, 1);
        t->ptrace |= PT_STEP;
        proc_work_set(t, WORK_SIGNAL | WORK_TRACE);
    } else if (t->ptrace & PT_STEP) {
        arch_ptrace_step(t, 0);
        t->ptrace &= ~PT_STEP;
    }
    if (syscalls) {
        t->ptrace |= PT_SYSCALL;
        proc_work_set(t, WORK_SIGNAL | WORK_TRACE);
    } else {
        t->ptrace &= ~PT_SYSCALL;
    }
    t->ptrace &= ~PT_LISTEN;
    t->ptrace_signo = (int32_t)sig;
    t->ptrace_status = 0;
    if (t->state == PROC_STOPPED) {
        sched_make_ready(t);
    }
    return 0;
}

/**
 * PTRACE_LISTEN: keep the seized thread `t`, stopped in a group stop's
 * PTRACE_EVENT_STOP, in its stop, as a stopped process is kept, until the
 * group stop begins again or ends (`ptrace_group_change`), when it makes a
 * new PTRACE_EVENT_STOP; at once, when such a change came before the
 * request. Any other stop, or a thread not seized, is -EIO, as on Linux.
 */
static long ptrace_listen(process_t *t)
{
    if (!(t->ptrace & PT_SEIZED) || (t->ptrace_status >> 16) != PTRACE_EVENT_STOP) {
        return -EIO;
    }
    if (t->ptrace & PT_INTERRUPT) {
        return ptrace_resume(t, 0, 0, (t->ptrace & PT_SYSCALL) != 0);
    }
    t->ptrace |= PT_LISTEN;
    return 0;
}

/**
 * Let the thread `t`, which a tracer has just attached to, out of the park
 * in which its process's stop keeps an untraced thread (`sig_park`): it goes
 * back through `deliver_signals`, where it makes the stop a ptrace stop,
 * reported as a group stop is, while its process stays stopped, as Linux's
 * attach makes a stopped task trap. A thread that is not parked is left.
 */
static void ptrace_unpark(process_t *t)
{
    uint32_t flags = kirq_save();

    if (t->state == PROC_STOPPED) {
        sched_make_ready(t);
    }
    kirq_restore(flags);
}

/**
 * Whether every live thread of the thread group `tg` is privileged. A
 * process traces as a whole, since each of its threads may make the
 * tracer's requests, so it is as privileged as its least privileged live
 * thread; a leader that has ended keeps its record and its credentials, and
 * does not count. A group with no live thread is not privileged.
 */
static int group_privileged(const struct tgroup *tg)
{
    const process_t *t;
    int live = 0;

    for (t = proc_list; t; t = t->next) {
        if (t->tg == tg && t->state != PROC_ZOMBIE && t->state != PROC_UNUSED) {
            if (!proc_privileged(t)) {
                return 0;
            }
            live = 1;
        }
    }
    return live;
}

int ptrace_tracer_privileged(const process_t *t)
{
    const process_t *tracer = get_process(t->tracer);

    return tracer != NULL && group_privileged(tracer->tg);
}

/**
 * PTRACE_ATTACH and PTRACE_SEIZE: make the caller's process `t`'s tracer.
 * The caller's process needs privilege (`group_privileged`), or the caller
 * its real user ID equal to each of `t`'s three user IDs and its real group
 * ID to each of `t`'s three group IDs, `t` dumpable and `t` not privileged,
 * as Linux's `__ptrace_may_access` requires when the caller lacks
 * CAP_SYS_PTRACE: a root thread that gave up its capabilities may not trace
 * one that has them. Such a caller may not attach to a thread inside
 * execve either, which sets the ids of its new image according to the
 * tracer the thread has (`exec_ids` in syscall_proc.c); Linux makes the
 * attach wait for the exec instead. No caller may attach to a thread of its
 * own process, a thread with no address space, init, or a thread already
 * traced. ATTACH sends the thread SIGSTOP; SEIZE takes the options in
 * `opts` and stops nothing. 0, -EPERM, or -EINVAL for an option SEIZE does
 * not have.
 */
static int ptrace_attach(process_t *t, int seize, unsigned long opts)
{
    struct k_siginfo info;

    if (t->tg == current_proc->tg || t->mm == NULL || t->tgid == 1 || (t->ptrace & PT_TRACED)) {
        return -EPERM;
    }
    if (!group_privileged(current_proc->tg) && t->exec_ws != NULL) {
        return -EPERM;
    }
    if (!group_privileged(current_proc->tg) &&
        (current_proc->ruid != t->ruid || current_proc->ruid != t->euid ||
         current_proc->ruid != t->suid || current_proc->rgid != t->rgid ||
         current_proc->rgid != t->egid || current_proc->rgid != t->sgid || !t->mm->dumpable ||
         proc_privileged(t))) {
        return -EPERM;
    }
    if (seize && (opts & ~(unsigned long)PTRACE_O_MASK)) {
        return -EINVAL;
    }
    t->ptrace = PT_TRACED | (seize ? PT_SEIZED : 0);
    proc_work_set(t, WORK_SIGNAL);
    t->tracer = current_proc->tgid;
    t->ptrace_opts = seize ? (uint32_t)opts : 0;
    t->ptrace_exit_seen = 0;
    if (!seize) {
        k_memset(&info, 0, sizeof(info));
        info.code = SI_USER;
        info.pid = pid_seen_by(t, current_proc->tgid);
        info.uid = ns_uid_from_kernel(proc_userns(t), current_proc->ruid);
        signal_post_thread(t, SIGSTOP, &info);
    }
    ptrace_unpark(t);
    return 0;
}

/**
 * PTRACE_TRACEME: make the caller's parent its tracer. A privileged caller
 * may not make a parent process that is not privileged (`group_privileged`)
 * its tracer, as Linux's `cap_ptrace_traceme` does not let a parent whose
 * capabilities are fewer trace. 0, or -EPERM for that, for a caller already
 * traced, or for a parent that has gone.
 */
static int ptrace_traceme(void)
{
    process_t *parent = get_process(current_proc->parent_pid);

    if ((current_proc->ptrace & PT_TRACED) || parent == NULL ||
        (proc_privileged(current_proc) && !group_privileged(parent->tg))) {
        return -EPERM;
    }
    current_proc->ptrace = PT_TRACED;
    proc_work_set(current_proc, WORK_SIGNAL);
    current_proc->tracer = parent->tgid;
    current_proc->ptrace_opts = 0;
    current_proc->ptrace_exit_seen = 0;
    return 0;
}

process_t *ptrace_tracee(uint32_t pid)
{
    process_t *t = get_process(pid);

    if (!t || t->state == PROC_UNUSED || t->state == PROC_ZOMBIE || !(t->ptrace & PT_TRACED) ||
        t->tracer != current_proc->tgid || t->ptrace_status == 0) {
        return NULL;
    }
    return t;
}

/**
 * PTRACE_PEEKTEXT and friends: read the `len` bytes at `addr` of the stopped
 * thread `t`'s memory into `word`, or with `write` write them there: a word
 * of the caller's ABI, 4 bytes for an i386 tracer on x86-64. The
 * address space has a reference of the caller's while it is used, since
 * bringing a page in may sleep, and the thread may end and drop its own
 * meanwhile; nothing of `t` is read after the access.
 *
 * A caller that is not privileged (`proc_privileged`) gets -EIO, as from
 * Linux's `ptrace_access_vm`, for an address space that is not dumpable:
 * one that an exec of an image the caller may not read or with ids the
 * caller lacks, a change of the thread's ids, or PR_SET_DUMPABLE made so
 * after the attach. 0, or -EIO or -ENOMEM from `paging_access`.
 */
static int32_t tracee_memory(process_t *t, uintptr_t addr, void *word, uint32_t len, int write)
{
    struct mm *mm = t->mm;
    int32_t rc;

    if (!mm || (!proc_privileged(current_proc) && !mm->dumpable)) {
        return -EIO;
    }
    mm_get(mm);
    rc = paging_access(mm, addr, word, len, write);
    mm_put(mm);
    return rc;
}

/** PTRACE_GETSIGINFO and PTRACE_SETSIGINFO: the siginfo of the stop of `t`,
 * the thread `pid`, as the 128-byte `siginfo_t` at `data`, in the tracer's
 * layout, the i386 one for an i386 tracer on x86-64 whatever the tracee
 * runs, as Linux's `compat_ptrace_request` has it, with its own si_signo:
 * the stop's, or the one PTRACE_SETSIGINFO gave, by whose layout it read
 * the rest, as Linux keeps last_siginfo whole. 0, -EFAULT, -EINVAL for
 * a stop that has no siginfo (`PT_NOINFO`), as Linux's ptrace_getsiginfo
 * and ptrace_setsiginfo refuse a stop without last_siginfo, or -ESRCH
 * for a thread gone from its stop during the copy in. PTRACE_SETSIGINFO
 * copies and reads its siginfo before it looks for one to replace, so that
 * a bad pointer is -EFAULT, and a code of a layout Linux does not know with
 * bytes past the kept union -E2BIG (`siginfo_from_caller`), at any stop,
 * as Linux's copy_siginfo_from_user runs before ptrace_setsiginfo; a
 * native siginfo of a known layout is read only as far as the kept union,
 * so its extension may lie on a page the tracer cannot read. */
static int siginfo_access(uint32_t pid, process_t *t, uintptr_t data, int set)
{
    uint8_t buf[SIGINFO_SIZE];
    struct k_siginfo info;
    int32_t signo;
    int32_t rc;

    if (!set) {
        if (t->ptrace & PT_NOINFO) {
            return -EINVAL;
        }
        siginfo_to_caller(buf, t->ptrace_info_signo, &t->ptrace_info);
        return copy_to_user((void __user *)data, buf, sizeof(buf)) == 0 ? 0 : -EFAULT;
    }
    rc = siginfo_from_caller(data, 0, &signo, &info);
    if (rc != 0) {
        return rc;
    }
    t = ptrace_tracee(pid);
    if (!t) {
        return -ESRCH;
    }
    if (t->ptrace & PT_NOINFO) {
        return -EINVAL;
    }
    t->ptrace_info = info;
    t->ptrace_info_signo = signo;
    return 0;
}

/**
 * PTRACE_GETREGSET and PTRACE_SETREGSET: the register set `type` of `t`, the
 * thread `pid`, to or from the buffer the `struct iovec` at `data` names,
 * as Linux's `ptrace_regset` moves it: the iovec in the tracer's layout,
 * two 32-bit words from an i386 tracer on x86-64, and the set in the
 * layout of the tracee's code, which on x86-64 is i386's while it runs
 * 32-bit code (`arch_regset_size`). The length must be a multiple of the
 * set's unit; it is cut to the set's size, and the iovec's length becomes
 * the bytes moved. The set goes through the heap, since an XSAVE image is
 * larger than a frame of the kernel's stack should be. The copy of the iovec
 * may sleep, and the thread may be reaped meanwhile, so both directions find
 * it again after their copies from the caller (`ptrace_tracee`). 0, or
 * -EFAULT, -EINVAL, -ENOMEM, -ESRCH, -EIO if the set changed shape while
 * a copy slept, or the architecture's error.
 */
static long regset_access(uint32_t pid, process_t *t, unsigned long type, uintptr_t data, int set)
{
    struct kernel_iovec iov;
    uint32_t unit = 1;
    uint32_t copied_unit;
    uint32_t len;
    uint8_t *buf;
    long size;
    long rc = 0;
    int compat = in_compat_syscall();
    uint32_t iov32[2];

    if (compat) {
        if (copy_from_user(iov32, (const void __user *)data, sizeof(iov32)) != 0) {
            return -EFAULT;
        }
        iov.iov_base = iov32[0];
        iov.iov_len = iov32[1];
    } else if (copy_from_user(&iov, (const void __user *)data, sizeof(iov)) != 0) {
        return -EFAULT;
    }
    t = ptrace_tracee(pid);
    if (!t) {
        return -ESRCH;
    }
    size = arch_regset_size(t, type, &unit);
    if (size < 0) {
        return size;
    }
    if (iov.iov_len % unit != 0) {
        return -EINVAL;
    }
    len = iov.iov_len < (unsigned long)size ? (uint32_t)iov.iov_len : (uint32_t)size;
    buf = kmalloc((size_t)size);
    if (!buf) {
        return -ENOMEM;
    }
    if (!set) {
        t = ptrace_tracee(pid);
        if (!t) {
            rc = -ESRCH;
        } else if (arch_regset_size(t, type, &copied_unit) != size || copied_unit != unit) {
            rc = -EIO;
        } else {
            arch_regset_get(t, type, buf);
            if (copy_to_user((void __user *)iov.iov_base, buf, len) != 0) {
                rc = -EFAULT;
            }
        }
    } else if (copy_from_user(buf, (const void __user *)iov.iov_base, len) != 0) {
        rc = -EFAULT;
    } else {
        t = ptrace_tracee(pid);
        if (!t) {
            rc = -ESRCH;
        } else if (arch_regset_size(t, type, &copied_unit) != size || copied_unit != unit) {
            rc = -EIO;
        } else {
            rc = arch_regset_set(t, type, buf, len);
        }
    }
    kfree(buf);
    iov.iov_len = len;
    iov32[1] = len;
    if (rc == 0 && compat &&
        copy_to_user((void __user *)(data + sizeof(iov32[0])), &iov32[1], sizeof(iov32[1])) != 0) {
        rc = -EFAULT;
    } else if (rc == 0 && !compat &&
               copy_to_user((void __user *)(data + sizeof(iov.iov_base)), &iov.iov_len,
                            sizeof(iov.iov_len)) != 0) {
        rc = -EFAULT;
    }
    return rc;
}

/* What PTRACE_GET_SYSCALL_INFO reports, Linux's `struct
 * ptrace_syscall_info`, 88 bytes with its padding: the kind of stop, the
 * architecture, the instruction and stack pointers, then by the kind the
 * call's number and arguments, its result, or both and the data of the
 * seccomp filter's SECCOMP_RET_TRACE. */
struct syscall_info {
    uint8_t op;
    uint8_t pad[3];
    uint32_t arch;
    uint64_t instruction_pointer;
    uint64_t stack_pointer;
    uint64_t nr;      /* entry.nr, seccomp.nr; exit.rval at the same place */
    uint64_t args[6]; /* entry.args, seccomp.args; exit.is_error in args[0] */
    uint32_t ret_data;
    uint32_t pad2;
};

#define PTRACE_SYSCALL_INFO_NONE    0
#define PTRACE_SYSCALL_INFO_ENTRY   1
#define PTRACE_SYSCALL_INFO_EXIT    2
#define PTRACE_SYSCALL_INFO_SECCOMP 3

/* The sizes Linux returns for each kind: up to the union, through the
 * arguments, through `is_error`, through `ret_data`. */
#define SYSCALL_INFO_NONE_SIZE    24u
#define SYSCALL_INFO_ENTRY_SIZE   80u
#define SYSCALL_INFO_EXIT_SIZE    33u
#define SYSCALL_INFO_SECCOMP_SIZE 84u

/**
 * PTRACE_GET_SYSCALL_INFO: describe the stop of `t` into the `size` bytes
 * at the caller's `data`, as Linux's `ptrace_get_syscall_info`. The kind
 * comes from the stop: a system call stop reported as SIGTRAP | 0x80, that
 * is with PTRACE_O_TRACESYSGOOD, at the entry or the exit by its event
 * message, or a PTRACE_EVENT_SECCOMP stop; any other stop is
 * PTRACE_SYSCALL_INFO_NONE. The architecture is the call's, i386's for one
 * by `int $0x80` on x86-64 (SC_AUDIT_ARCH). An exit's result is an error
 * when it is -4095 to -1, taken in 32 bits for an i386 call, and is
 * otherwise the result register as it is. Returns the
 * size of the whole description, of which at most `size` bytes are
 * written, or -EFAULT.
 */
static long syscall_info(const process_t *t, uintptr_t size, uintptr_t data)
{
    const isr_params_t *tf = proc_user_frame(t);
    struct syscall_info info;
    uint32_t code = (t->ptrace_status >> 8) & 0xFFFFu, actual = SYSCALL_INFO_NONE_SIZE;
    long rval;

    k_memset(&info, 0, sizeof(info));
    info.op = PTRACE_SYSCALL_INFO_NONE;
    info.arch = SC_AUDIT_ARCH(tf);
    info.instruction_pointer = FRAME_PC(tf);
    info.stack_pointer = FRAME_SP(tf);
    if (code == ((uint32_t)SIGTRAP | 0x80u) && t->ptrace_msg == PTRACE_EVENTMSG_SYSCALL_EXIT) {
        info.op = PTRACE_SYSCALL_INFO_EXIT;
        rval = (long)SC_RET(tf);
        if (SC_COMPAT(tf)) {
            rval = (long)(int32_t)rval;
        }
        info.args[0] = rval < 0 && rval >= -4095;
        info.nr = info.args[0] ? (uint64_t)rval : (uint64_t)SC_RET(tf);
        actual = SYSCALL_INFO_EXIT_SIZE;
    } else if ((code == ((uint32_t)SIGTRAP | 0x80u) &&
                t->ptrace_msg == PTRACE_EVENTMSG_SYSCALL_ENTRY) ||
               code == ((uint32_t)SIGTRAP | ((uint32_t)PTRACE_EVENT_SECCOMP << 8))) {
        info.op = code == ((uint32_t)SIGTRAP | 0x80u) ? PTRACE_SYSCALL_INFO_ENTRY
                                                      : PTRACE_SYSCALL_INFO_SECCOMP;
        info.nr = (uint64_t)(int64_t)t->orig_nr;
        info.args[0] = SC_ARG(tf, 0);
        info.args[1] = SC_ARG(tf, 1);
        info.args[2] = SC_ARG(tf, 2);
        info.args[3] = SC_ARG(tf, 3);
        info.args[4] = SC_ARG(tf, 4);
        info.args[5] = SC_ARG(tf, 5);
        actual = SYSCALL_INFO_ENTRY_SIZE;
        if (info.op == PTRACE_SYSCALL_INFO_SECCOMP) {
            info.ret_data = t->ptrace_msg;
            actual = SYSCALL_INFO_SECCOMP_SIZE;
        }
    }
    if (size > actual) {
        size = actual;
    }
    if (copy_to_user((void __user *)data, &info, size) != 0) {
        return -EFAULT;
    }
    return (long)actual;
}

/**
 * The requests that need `t`, the thread `pid`, stopped for the caller, as
 * Linux has them. No request uses `t` after a copy that may sleep: one that
 * copies from the caller's memory finds the thread again after the copy
 * (`ptrace_tracee`), one that copies out takes what it copies first, and
 * the memory requests take a reference to the address space themselves
 * (`tracee_memory`). A word of memory or of the event message is the
 * tracer's, 4 bytes for an i386 tracer on x86-64, whose word moves the low
 * half of `data`, as Linux's `compat_ptrace_request` moves it.
 */
static long ptrace_stopped(process_t *t, uint32_t pid, long request, uintptr_t addr,
                           unsigned long data)
{
    unsigned long word = 0;
    uint32_t wlen = (uint32_t)sizeof(word);
    int32_t rc;

    if (in_compat_syscall()) {
        wlen = 4u;
    }
    switch (request) {
    case PTRACE_PEEKTEXT:
    case PTRACE_PEEKDATA:
        rc = tracee_memory(t, addr, &word, wlen, 0);
        if (rc == 0 && copy_to_user((void __user *)data, &word, wlen) != 0) {
            rc = -EFAULT;
        }
        return rc;
    case PTRACE_POKETEXT:
    case PTRACE_POKEDATA:
        return tracee_memory(t, addr, &data, wlen, 1);
    case PTRACE_CONT:
        return ptrace_resume(t, data, 0, 0);
    case PTRACE_SYSCALL:
        return ptrace_resume(t, data, 0, 1);
    case PTRACE_SINGLESTEP:
        return ptrace_resume(t, data, 1, 0);
    case PTRACE_DETACH:
        if (data > KSIG_MAX) {
            return -EIO;
        }
        ptrace_detach(t, (int)data);
        return 0;
    case PTRACE_LISTEN:
        return ptrace_listen(t);
    case PTRACE_SETOPTIONS:
        if (data & ~(unsigned long)PTRACE_O_MASK) {
            return -EINVAL;
        }
        t->ptrace_opts = (uint32_t)data;
        return 0;
    case PTRACE_GETEVENTMSG:
        word = t->ptrace_msg;
        return copy_to_user((void __user *)data, &word, wlen) == 0 ? 0 : -EFAULT;
    case PTRACE_GETSIGINFO:
    case PTRACE_SETSIGINFO:
        return siginfo_access(pid, t, data, request == PTRACE_SETSIGINFO);
    case PTRACE_GETREGSET:
    case PTRACE_SETREGSET:
        return regset_access(pid, t, addr, data, request == PTRACE_SETREGSET);
    case PTRACE_GET_SYSCALL_INFO:
        return syscall_info(t, addr, data);
    default:
        return arch_ptrace(t, pid, request, addr, data);
    }
}

long sys_ptrace(isr_params_t *params)
{
    long request = (long)SC_ARG(params, 0);
    uintptr_t addr = SC_ARG(params, 2);
    unsigned long data = SC_ARG(params, 3);
    process_t *t;
    struct k_siginfo info;

    if (!current_proc) {
        return -ESRCH;
    }
    if (request == PTRACE_TRACEME) {
        return ptrace_traceme();
    }
    t = proc_named((uint32_t)SC_ARG(params, 1));
    if (!t || t->state == PROC_UNUSED || t->state == PROC_ZOMBIE) {
        return -ESRCH;
    }
    if (request == PTRACE_ATTACH || request == PTRACE_SEIZE) {
        return ptrace_attach(t, request == PTRACE_SEIZE, data);
    }
    if (!(t->ptrace & PT_TRACED) || t->tracer != current_proc->tgid) {
        return -ESRCH;
    }
    if (request == PTRACE_KILL) {
        k_memset(&info, 0, sizeof(info));
        info.code = SI_KERNEL;
        signal_post_thread(t, SIGKILL, &info);
        return 0;
    }
    if (request == PTRACE_INTERRUPT) {
        if (!(t->ptrace & PT_SEIZED)) {
            return -EIO;
        }
        if (t->ptrace_status == 0) {
            t->ptrace |= PT_INTERRUPT;
            ptrace_signal_wake(t);
        } else if (t->ptrace & PT_LISTEN) {
            ptrace_restop(t);
        }
        return 0;
    }
    if (t->ptrace_status == 0) {
        return -ESRCH;
    }
    return ptrace_stopped(t, t->pid, request, addr, data);
}
