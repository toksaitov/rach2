/*
 * ptrace.c: process tracing, as GDB and strace use it. See ptrace.Readme.md.
 */

#include "ptrace.h"

#include "ns.h"
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
#include "../syscall_internal.h"

/* The requests. */
#define PTRACE_TRACEME     0
#define PTRACE_PEEKTEXT    1
#define PTRACE_PEEKDATA    2
#define PTRACE_POKETEXT    4
#define PTRACE_POKEDATA    5
#define PTRACE_CONT        7
#define PTRACE_KILL        8
#define PTRACE_SINGLESTEP  9
#define PTRACE_ATTACH      16
#define PTRACE_DETACH      17
#define PTRACE_SYSCALL     24
#define PTRACE_SETOPTIONS  0x4200
#define PTRACE_GETEVENTMSG 0x4201
#define PTRACE_GETSIGINFO  0x4202
#define PTRACE_SETSIGINFO  0x4203
#define PTRACE_GETREGSET   0x4204
#define PTRACE_SETREGSET   0x4205
#define PTRACE_SEIZE       0x4206
#define PTRACE_INTERRUPT   0x4207
#define PTRACE_LISTEN      0x4208

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
 * PTRACE_GETSIGINFO reads, none for a stop that is not a signal's. A thread
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
    } else {
        k_memset(&self->ptrace_info, 0, sizeof(self->ptrace_info));
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
    while (self->ptrace_status != 0 && !kill_pending()) {
        current_proc->state = PROC_STOPPED;
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
        (void)ptrace_stop(((uint32_t)PTRACE_EVENT_STOP << 16) | (signo << 8) | 0x7F, NULL);
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
    if (sig == signo) {
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
        status |= (uint32_t)PTRACE_EVENT_STOP << 16;
    }
    (void)ptrace_stop(status, NULL);
    ptrace_return_stops();
    return 1;
}

/**
 * Request a new PTRACE_EVENT_STOP of the seized thread `t`. A thread that
 * PTRACE_LISTEN keeps in its stop is let out of it at once, so that it
 * makes the new one.
 */
static void ptrace_restop(process_t *t)
{
    t->ptrace |= PT_INTERRUPT;
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
    (void)ptrace_stop(((uint32_t)event << 16) | ((uint32_t)SIGTRAP << 8) | 0x7F, NULL);
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
    self->orig_nr = (int32_t)nr;
    if (!leaving) {
        SC_RET(tf) = (unsigned long)-ENOSYS;
    }
    sig = ptrace_stop((signo << 8) | 0x7F, stepping ? &info : NULL);
    if (!leaving && ((self->sig_pending | self->tg->pending) & KSIG_BIT(SIGKILL))) {
        self->orig_nr = -1;
    }
    if (sig > 0 && sig <= KSIG_MAX) {
        tracer_info(&info);
        signal_post_thread(self, sig, &info);
    }
}

int ptrace_fork_attach(process_t *child, uint32_t flags)
{
    process_t *self = current_proc;
    int event;

    if (!(self->ptrace & PT_TRACED)) {
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
    signal_post_info(tracer, SIGCHLD, &info);
    wq_wake_all(&tracer->tg->chld);
}

/**
 * Tell the parent of the process `t` leads, which has ended and which its
 * tracer has let go, of the end, as the process's last thread tells the
 * parent of a process no other process traces: a SIGCHLD, and a wake of its
 * wait, which passed the process over while it was traced.
 */
static void ptrace_parent_notify(const process_t *t)
{
    process_t *parent = get_process(t->parent_pid);
    struct k_siginfo info;

    if (parent == NULL) {
        return;
    }
    proc_child_info(t, parent, &info);
    signal_post_info(parent, SIGCHLD, &info);
    wq_wake_all(&parent->tg->chld);
}

/**
 * Take `t` from its tracer, as PTRACE_DETACH, a tracer's end and its wait's
 * report of a process's end do: the step flag the kernel set and the debug
 * registers go, and a thread in a ptrace stop resumes with `sig`. A thread
 * that ended while traced, which its tracer would have freed, is left for the
 * next return to user mode to free. A process that has ended, which its
 * parent's wait passed over while another process traced it, is left to the
 * parent, which is told of the end (`ptrace_parent_notify`); a parent that
 * is the tracer is ending itself, and the process goes to init with its
 * other children (`reparent_children`).
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
    if (t->state == PROC_ZOMBIE && t->pid != t->tgid) {
        proc_dead_threads++;
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
    } else if (t->ptrace & PT_STEP) {
        arch_ptrace_step(t, 0);
        t->ptrace &= ~PT_STEP;
    }
    if (syscalls) {
        t->ptrace |= PT_SYSCALL;
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
 * PTRACE_PEEKTEXT and friends: read the word at `addr` of the stopped
 * thread `t`'s memory into `*word`, or with `write` write it there. The
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
static int32_t tracee_memory(process_t *t, uintptr_t addr, unsigned long *word, int write)
{
    struct mm *mm = t->mm;
    int32_t rc;

    if (!mm || (!proc_privileged(current_proc) && !mm->dumpable)) {
        return -EIO;
    }
    mm_get(mm);
    rc = paging_access(mm, addr, word, sizeof(*word), write);
    mm_put(mm);
    return rc;
}

/** PTRACE_GETSIGINFO and PTRACE_SETSIGINFO: the siginfo of the stop of `t`,
 * the thread `pid`, as the 128-byte `siginfo_t` at `data`. 0, -EFAULT, or
 * -ESRCH for a thread gone from its stop during the copy in. */
static int siginfo_access(uint32_t pid, process_t *t, uintptr_t data, int set)
{
    uint8_t buf[SIGINFO_SIZE];

    if (!set) {
        sigframe_fill_siginfo(buf, (int)((t->ptrace_status >> 8) & 0x7F), &t->ptrace_info);
        return copy_to_user((void __user *)data, buf, sizeof(buf)) == 0 ? 0 : -EFAULT;
    }
    if (copy_from_user(buf, (const void __user *)data, sizeof(buf)) != 0) {
        return -EFAULT;
    }
    t = ptrace_tracee(pid);
    if (!t) {
        return -ESRCH;
    }
    sigframe_take_siginfo(buf, &t->ptrace_info);
    return 0;
}

/**
 * PTRACE_GETREGSET and PTRACE_SETREGSET: the register set `type` of `t`, the
 * thread `pid`, to or from the buffer the `struct iovec` at `data` names,
 * as Linux's `ptrace_regset` moves it. The length must be a multiple of the
 * set's unit; it is cut to the set's size, and the iovec's length becomes
 * the bytes moved. The set goes through the heap, since an XSAVE image is
 * larger than a frame of the kernel's stack should be. The copy of the iovec
 * may sleep, and the thread may be reaped meanwhile, so both directions find
 * it again after their copies from the caller (`ptrace_tracee`). 0, or
 * -EFAULT, -EINVAL, -ENOMEM, -ESRCH or the architecture's error.
 */
static long regset_access(uint32_t pid, process_t *t, unsigned long type, uintptr_t data, int set)
{
    struct kernel_iovec iov;
    uint32_t unit = 1;
    uint32_t len;
    uint8_t *buf;
    long size;
    long rc = 0;

    if (copy_from_user(&iov, (const void __user *)data, sizeof(iov)) != 0) {
        return -EFAULT;
    }
    size = arch_regset_size(type, &unit);
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
        rc = t ? arch_regset_set(t, type, buf, len) : -ESRCH;
    }
    kfree(buf);
    iov.iov_len = len;
    if (rc == 0 && copy_to_user((void __user *)(data + sizeof(iov.iov_base)), &iov.iov_len,
                                sizeof(iov.iov_len)) != 0) {
        rc = -EFAULT;
    }
    return rc;
}

/**
 * The requests that need `t`, the thread `pid`, stopped for the caller, as
 * Linux has them. No request uses `t` after a copy that may sleep: one that
 * copies from the caller's memory finds the thread again after the copy
 * (`ptrace_tracee`), one that copies out takes what it copies first, and
 * the memory requests take a reference to the address space themselves
 * (`tracee_memory`).
 */
static long ptrace_stopped(process_t *t, uint32_t pid, long request, uintptr_t addr,
                           unsigned long data)
{
    unsigned long word;
    int32_t rc;

    switch (request) {
    case PTRACE_PEEKTEXT:
    case PTRACE_PEEKDATA:
        rc = tracee_memory(t, addr, &word, 0);
        if (rc == 0 && copy_to_user((void __user *)data, &word, sizeof(word)) != 0) {
            rc = -EFAULT;
        }
        return rc;
    case PTRACE_POKETEXT:
    case PTRACE_POKEDATA:
        return tracee_memory(t, addr, &data, 1);
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
        return copy_to_user((void __user *)data, &word, sizeof(word)) == 0 ? 0 : -EFAULT;
    case PTRACE_GETSIGINFO:
    case PTRACE_SETSIGINFO:
        return siginfo_access(pid, t, data, request == PTRACE_SETSIGINFO);
    case PTRACE_GETREGSET:
    case PTRACE_SETREGSET:
        return regset_access(pid, t, addr, data, request == PTRACE_SETREGSET);
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
            if (t->state == PROC_BLOCKED) {
                t->wakeup_tick = 0;
                sched_make_ready(t);
            }
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
