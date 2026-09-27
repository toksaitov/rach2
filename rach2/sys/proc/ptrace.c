/*
 * ptrace.c: process tracing, as GDB and strace use it. See ptrace.Readme.md.
 */

#include "ptrace.h"

#include "fpu.h"
#include "sched.h"
#include "sigframe.h"
#include "tgroup.h"
#include "tls.h"
#include "../lib/kerrno.h"
#include "../lib/kirq.h"
#include "../lib/kstring.h"
#include "../mm/mm.h"
#include "../mm/paging.h"
#include "../mm/usercopy.h"

/* The requests. */
#define PTRACE_TRACEME         0
#define PTRACE_PEEKTEXT        1
#define PTRACE_PEEKDATA        2
#define PTRACE_PEEKUSER        3
#define PTRACE_POKETEXT        4
#define PTRACE_POKEDATA        5
#define PTRACE_POKEUSER        6
#define PTRACE_CONT            7
#define PTRACE_KILL            8
#define PTRACE_SINGLESTEP      9
#define PTRACE_GETREGS         12
#define PTRACE_SETREGS         13
#define PTRACE_GETFPREGS       14
#define PTRACE_SETFPREGS       15
#define PTRACE_ATTACH          16
#define PTRACE_DETACH          17
#define PTRACE_GETFPXREGS      18
#define PTRACE_SETFPXREGS      19
#define PTRACE_SYSCALL         24
#define PTRACE_GET_THREAD_AREA 25
#define PTRACE_SET_THREAD_AREA 26
#define PTRACE_SETOPTIONS      0x4200
#define PTRACE_GETEVENTMSG     0x4201
#define PTRACE_GETSIGINFO      0x4202
#define PTRACE_SETSIGINFO      0x4203
#define PTRACE_SEIZE           0x4206
#define PTRACE_INTERRUPT       0x4207
#define PTRACE_LISTEN          0x4208

/* The options: an event's bit is 1 << the event, and the rest. */
#define PTRACE_O_TRACESYSGOOD 0x00000001u
#define PTRACE_O_TRACEEXEC    0x00000010u
#define PTRACE_O_EXITKILL     0x00100000u
#define PTRACE_O_MASK         0x0010007Fu

/* The i386 `struct user`: the 17 words of `user_regs_struct`, then the
 * floating-point and accounting fields, and the eight debug registers at
 * byte 252, 284 bytes in all. */
enum {
    R_EBX,
    R_ECX,
    R_EDX,
    R_ESI,
    R_EDI,
    R_EBP,
    R_EAX,
    R_DS,
    R_ES,
    R_FS,
    R_GS,
    R_ORIG_EAX,
    R_EIP,
    R_CS,
    R_EFL,
    R_UESP,
    R_SS,
    R_COUNT
};
#define USER_FPVALID  68u
#define USER_DEBUGREG 252u
#define USER_SIZE     284u

/* The user selectors: code, data, and the TLS entry's. */
#define SEL_UCODE 0x1Bu
#define SEL_UDATA 0x23u
#define SEL_UTLS  0x33u

/* The flags a tracer may change, Linux's FLAG_MASK: CF, PF, AF, ZF, SF, TF,
 * DF, OF, NT, RF and AC. */
#define FLAG_MASK 0x00054DD5u
#define EFLAGS_TF 0x00000100u

/* DR7: the enable bits of the four breakpoints, the bits the processor
 * keeps set, and GD, which a tracer may not set. */
#define DR7_ENABLES 0x000000FFu
#define DR7_ONE     0x00000400u
#define DR7_GD      0x00002000u

/* DR6: the four breakpoints' bits and the single-step bit. */
#define DR6_HITS 0x0000000Fu
#define DR6_BS   0x00004000u

/*
 * The debug registers, as bytes: TinyCC's assembler has no mnemonic for
 * them. `mov %drN, %eax` is 0F 21 /r and `mov %eax, %drN` 0F 23 /r, with
 * the register N in the ModRM byte's reg field and %eax in its r/m field.
 */
#define DR_READ(n, v)   __asm__ __volatile__(".byte 0x0f, 0x21, " n : "=a"(v))
#define DR_WRITE(n, v)  __asm__ __volatile__(".byte 0x0f, 0x23, " n : : "a"(v) : "memory")
#define DR0_MODRM       "0xc0"
#define DR1_MODRM       "0xc8"
#define DR2_MODRM       "0xd0"
#define DR3_MODRM       "0xd8"
#define DR6_MODRM       "0xf0"
#define DR7_MODRM       "0xf8"

/** The user-mode registers of the thread `t`, which is not running: the frame
 * its entry to the kernel pushed at the top of its kernel stack. */
static isr_params_t *user_frame(const process_t *t)
{
    return (isr_params_t *)(t->kernel_stack - sizeof(isr_params_t));
}

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
 * whose tracer has gone does not stop and is no longer traced.
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
    chld.pid = self->pid;
    chld.uid = self->ruid;
    chld.status = (int32_t)((status >> 8) & 0xFF);
    signal_post_info(tracer, SIGCHLD, &chld);
    wq_wake_all(&tracer->tg->chld);

    flags = kirq_save();
    while (self->ptrace_status != 0 && !kill_pending()) {
        self->state = PROC_STOPPED;
        schedule();
        if (self->state == PROC_STOPPED) {
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

void ptrace_return_stops(void)
{
    process_t *self = current_proc;
    uint32_t signo;

    if (!(self->ptrace & PT_TRACED) || !(self->ptrace & (PT_START | PT_INTERRUPT))) {
        return;
    }
    signo = (self->ptrace & PT_START) ? SIGSTOP : SIGTRAP;
    self->ptrace &= ~(PT_START | PT_INTERRUPT);
    (void)ptrace_stop(((uint32_t)PTRACE_EVENT_STOP << 16) | (signo << 8) | 0x7F, NULL);
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
        k_memset(info, 0, sizeof(*info));
        info->code = SI_USER;
        info->pid = self->tracer;
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
    status = ((uint32_t)signo << 8) | 0x7F;
    if (self->ptrace & PT_SEIZED) {
        status |= (uint32_t)PTRACE_EVENT_STOP << 16;
    }
    (void)ptrace_stop(status, NULL);
    return 1;
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
    int sig;

    if ((self->ptrace & (PT_TRACED | PT_SYSCALL)) != (PT_TRACED | PT_SYSCALL)) {
        return;
    }
    if (self->ptrace_opts & PTRACE_O_TRACESYSGOOD) {
        signo |= 0x80u;
    }
    self->orig_eax = (int32_t)nr;
    if (!leaving) {
        tf->eax = (uint32_t)-ENOSYS;
    }
    sig = ptrace_stop((signo << 8) | 0x7F, NULL);
    if (!leaving) {
        tf->eax = nr;
    }
    if (sig > 0 && sig <= KSIG_MAX) {
        k_memset(&info, 0, sizeof(info));
        info.code = SI_USER;
        info.pid = self->tracer;
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
    k_memset(&info, 0, sizeof(info));
    info.code = CLD_EXITED;
    info.pid = p->pid;
    info.uid = p->ruid;
    info.status = (p->exit_code >> 8) & 0xFF;
    signal_post_info(tracer, SIGCHLD, &info);
    wq_wake_all(&tracer->tg->chld);
}

/**
 * Take `t` from its tracer, as PTRACE_DETACH and a tracer's end do: the step
 * flag the kernel set and the debug registers go, and a thread in a ptrace
 * stop resumes with `sig`. A thread that ended while traced, which its tracer
 * would have freed, is left for the next return to user mode to free.
 */
static void ptrace_detach(process_t *t, int sig)
{
    if (t->ptrace & PT_STEP) {
        user_frame(t)->eflags &= ~EFLAGS_TF;
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
    if (t->ptrace_status != 0) {
        t->ptrace_signo = sig;
        t->ptrace_status = 0;
        if (t->state == PROC_STOPPED) {
            t->state = PROC_READY;
        }
    }
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

void ptrace_switch_debug(const process_t *old, const process_t *next)
{
    uint32_t v;

    if (next && (next->debugreg[7] & DR7_ENABLES)) {
        v = next->debugreg[0];
        DR_WRITE(DR0_MODRM, v);
        v = next->debugreg[1];
        DR_WRITE(DR1_MODRM, v);
        v = next->debugreg[2];
        DR_WRITE(DR2_MODRM, v);
        v = next->debugreg[3];
        DR_WRITE(DR3_MODRM, v);
        v = next->debugreg[7] | DR7_ONE;
        DR_WRITE(DR7_MODRM, v);
    } else if (old && (old->debugreg[7] & DR7_ENABLES)) {
        v = DR7_ONE;
        DR_WRITE(DR7_MODRM, v);
    }
}

int32_t ptrace_debug_status(void)
{
    uint32_t dr6;
    uint32_t zero = 0;

    DR_READ(DR6_MODRM, dr6);
    DR_WRITE(DR6_MODRM, zero);
    if (current_proc) {
        current_proc->debugreg[6] = dr6;
    }
    if (dr6 & DR6_HITS) {
        return TRAP_HWBKPT;
    }
    return (dr6 & DR6_BS) ? TRAP_TRACE : 0;
}

/**
 * Whether the DR7 value `v` sets what a tracer may set with the addresses
 * `t` has in DR0 to DR3: no general-detect bit, and for each breakpoint it
 * enables a user address aligned to its length, a length of 1, 2 or 4 bytes,
 * and an execute breakpoint of length 1; an I/O breakpoint, which needs
 * CR4.DE, is refused.
 */
static int dr7_valid(const process_t *t, uint32_t v)
{
    int i;

    if (v & DR7_GD) {
        return 0;
    }
    for (i = 0; i < 4; i++) {
        uint32_t rw = (v >> (16 + 4 * i)) & 3u;
        uint32_t len = (v >> (18 + 4 * i)) & 3u;
        uint32_t bytes = len == 3u ? 4u : len + 1u;

        if (!(v & (3u << (2 * i)))) {
            continue;
        }
        if (rw == 2u || len == 2u || (rw == 0u && len != 0u) ||
            (t->debugreg[i] & (bytes - 1u)) != 0 || t->debugreg[i] >= USER_ADDR_MAX - bytes) {
            return 0;
        }
    }
    return 1;
}

/** Fill `r` with the user registers of the stopped thread `t`, in the order
 * of `user_regs_struct`. */
static void regs_get(const process_t *t, uint32_t r[R_COUNT])
{
    const isr_params_t *tf = user_frame(t);

    r[R_EBX] = tf->ebx;
    r[R_ECX] = tf->ecx;
    r[R_EDX] = tf->edx;
    r[R_ESI] = tf->esi;
    r[R_EDI] = tf->edi;
    r[R_EBP] = tf->ebp;
    r[R_EAX] = tf->eax;
    r[R_DS] = tf->ds & 0xFFFFu;
    r[R_ES] = tf->es & 0xFFFFu;
    r[R_FS] = tf->fs & 0xFFFFu;
    r[R_GS] = tf->gs & 0xFFFFu;
    r[R_ORIG_EAX] = (uint32_t)t->orig_eax;
    r[R_EIP] = tf->eip;
    r[R_CS] = tf->cs & 0xFFFFu;
    r[R_EFL] = tf->eflags;
    r[R_UESP] = tf->user_esp;
    r[R_SS] = tf->user_ss & 0xFFFFu;
}

/**
 * Set the user register `i` of the stopped thread `t` to `v`, as Linux's
 * `putreg` does: the flags only where a program may change them, a data
 * segment register only to a selector this kernel has (null, user data, or
 * the TLS entry's), and CS and SS only to the ones they have, since a return
 * to user mode with any other would fault in the kernel. An orig_eax of -1
 * stops an interrupted system call from being restarted. 0, or -EIO.
 */
static int regs_set(process_t *t, int i, uint32_t v)
{
    isr_params_t *tf = user_frame(t);

    switch (i) {
    case R_EBX:
        tf->ebx = v;
        break;
    case R_ECX:
        tf->ecx = v;
        break;
    case R_EDX:
        tf->edx = v;
        break;
    case R_ESI:
        tf->esi = v;
        break;
    case R_EDI:
        tf->edi = v;
        break;
    case R_EBP:
        tf->ebp = v;
        break;
    case R_EAX:
        tf->eax = v;
        break;
    case R_DS:
    case R_ES:
    case R_FS:
    case R_GS:
        v &= 0xFFFFu;
        if (v != 0 && v != SEL_UDATA && v != SEL_UTLS) {
            return -EIO;
        }
        if (i == R_DS) {
            tf->ds = v;
        } else if (i == R_ES) {
            tf->es = v;
        } else if (i == R_FS) {
            tf->fs = v;
        } else {
            tf->gs = v;
        }
        break;
    case R_ORIG_EAX:
        t->orig_eax = (int32_t)v;
        if ((int32_t)v == -1) {
            t->restart_nr = 0;
        }
        break;
    case R_EIP:
        tf->eip = v;
        break;
    case R_CS:
        if ((v & 0xFFFFu) != SEL_UCODE) {
            return -EIO;
        }
        break;
    case R_EFL:
        tf->eflags = (tf->eflags & ~FLAG_MASK) | (v & FLAG_MASK);
        break;
    case R_UESP:
        tf->user_esp = v;
        break;
    case R_SS:
        if ((v & 0xFFFFu) != SEL_UDATA) {
            return -EIO;
        }
        break;
    default:
        return -EIO;
    }
    return 0;
}

/** PTRACE_PEEKUSER: the word at byte `off` of `struct user`, the registers,
 * the valid mark of the floating-point state, or a debug register, into
 * `*out`; the other fields read as 0. -EIO for an offset outside the
 * structure or not aligned to a word. */
static int user_peek(const process_t *t, uint32_t off, uint32_t *out)
{
    uint32_t r[R_COUNT];

    if ((off & 3u) != 0 || off >= USER_SIZE) {
        return -EIO;
    }
    *out = 0;
    if (off < R_COUNT * 4u) {
        regs_get(t, r);
        *out = r[off / 4u];
    } else if (off == USER_FPVALID) {
        *out = 1;
    } else if (off >= USER_DEBUGREG) {
        uint32_t n = (off - USER_DEBUGREG) / 4u;

        *out = n == 7 ? (t->debugreg[7] | DR7_ONE) : t->debugreg[n];
    }
    return 0;
}

/** PTRACE_POKEUSER: set a register or a debug register; DR4 and DR5 and the
 * other fields of `struct user` are -EIO, and a DR7 `dr7_valid` refuses, or
 * an address in an enabled breakpoint's register that DR7 would then refuse,
 * -EINVAL. */
static int user_poke(process_t *t, uint32_t off, uint32_t v)
{
    uint32_t n;

    if ((off & 3u) != 0 || off >= USER_SIZE) {
        return -EIO;
    }
    if (off < R_COUNT * 4u) {
        return regs_set(t, (int)(off / 4u), v);
    }
    if (off < USER_DEBUGREG) {
        return -EIO;
    }
    n = (off - USER_DEBUGREG) / 4u;
    if (n == 4 || n == 5) {
        return -EIO;
    }
    if (n < 4) {
        uint32_t old = t->debugreg[n];

        t->debugreg[n] = v;
        if (!dr7_valid(t, t->debugreg[7])) {
            t->debugreg[n] = old;
            return -EINVAL;
        }
    } else if (n == 6) {
        t->debugreg[6] = v;
    } else {
        v &= ~DR7_ONE;
        if (!dr7_valid(t, v)) {
            return -EINVAL;
        }
        t->debugreg[7] = v;
    }
    return 0;
}

/** Resume the stopped thread `t` with the signal `sig`, stepping one
 * instruction with `step` and stopping at system calls with `syscalls`. */
static int ptrace_resume(process_t *t, uint32_t sig, int step, int syscalls)
{
    isr_params_t *tf = user_frame(t);

    if (sig > KSIG_MAX) {
        return -EIO;
    }
    if (step) {
        tf->eflags |= EFLAGS_TF;
        t->ptrace |= PT_STEP;
    } else if (t->ptrace & PT_STEP) {
        tf->eflags &= ~EFLAGS_TF;
        t->ptrace &= ~PT_STEP;
    }
    if (syscalls) {
        t->ptrace |= PT_SYSCALL;
    } else {
        t->ptrace &= ~PT_SYSCALL;
    }
    t->ptrace_signo = (int32_t)sig;
    t->ptrace_status = 0;
    if (t->state == PROC_STOPPED) {
        t->state = PROC_READY;
    }
    return 0;
}

/**
 * PTRACE_ATTACH and PTRACE_SEIZE: make the caller's process `t`'s tracer.
 * The caller needs effective user ID 0, or its real user ID equal to each of
 * `t`'s three and `t` dumpable, as Linux's `__ptrace_may_access` asks when
 * the caller lacks CAP_SYS_PTRACE. A thread of the caller's own process, a
 * thread with no address space, init, and a thread already traced are
 * refused. ATTACH sends the thread SIGSTOP; SEIZE takes the options in
 * `opts` and stops nothing.
 */
static int ptrace_attach(process_t *t, int seize, uint32_t opts)
{
    struct k_siginfo info;

    if (t->tg == current_proc->tg || t->mm == NULL || t->tgid == 1 || (t->ptrace & PT_TRACED)) {
        return -EPERM;
    }
    if (current_proc->euid != 0 &&
        (current_proc->ruid != t->ruid || current_proc->ruid != t->euid ||
         current_proc->ruid != t->suid || current_proc->rgid != t->rgid ||
         current_proc->rgid != t->egid || current_proc->rgid != t->sgid || !t->dumpable)) {
        return -EPERM;
    }
    if (seize && (opts & ~PTRACE_O_MASK)) {
        return -EINVAL;
    }
    t->ptrace = PT_TRACED | (seize ? PT_SEIZED : 0);
    t->tracer = current_proc->tgid;
    t->ptrace_opts = seize ? opts : 0;
    t->ptrace_exit_seen = 0;
    if (!seize) {
        k_memset(&info, 0, sizeof(info));
        info.code = SI_USER;
        info.pid = current_proc->tgid;
        info.uid = current_proc->ruid;
        signal_post_thread(t, SIGSTOP, &info);
    }
    return 0;
}

/** PTRACE_TRACEME: make the caller's parent its tracer. */
static int ptrace_traceme(void)
{
    process_t *parent = get_process(current_proc->parent_pid);

    if ((current_proc->ptrace & PT_TRACED) || parent == NULL) {
        return -EPERM;
    }
    current_proc->ptrace = PT_TRACED;
    current_proc->tracer = parent->tgid;
    current_proc->ptrace_opts = 0;
    current_proc->ptrace_exit_seen = 0;
    return 0;
}

/** The 624-byte form of `t`'s floating-point state that `fpu_to_linux`
 * writes: the x87 part in the `fnsave` layout, then the `fxsave` image at
 * byte 112 unless the magic word at byte 110 is 0xFFFF. */
static void fp_linux(const process_t *t, uint8_t *buf)
{
    fpu_to_linux(t->fpu, buf);
}

/** PTRACE_GETFPREGS and friends: copy the floating-point state of `t` to or
 * from the user at `data`, as the 108-byte `user_i387_struct` or, with
 * `fx`, the 512-byte `fxsave` image. -EIO for the image on a processor
 * without FXSR, -EINVAL for one `fxrstor` would fault on. */
static int fp_access(process_t *t, uint32_t data, int fx, int set)
{
    uint8_t buf[FPU_LINUX_SIZE];
    uint8_t area[FPU_STATE_SIZE + 16];
    uint8_t *aligned = (uint8_t *)(((uintptr_t)area + 15u) & ~(uintptr_t)15u);
    uint32_t off = fx ? 112u : 0u;
    uint32_t len = fx ? (uint32_t)FPU_STATE_SIZE : 108u;

    fp_linux(t, buf);
    if (fx && buf[110] == 0xFF && buf[111] == 0xFF) {
        return -EIO;
    }
    if (!set) {
        return copy_to_user((void __user *)data, buf + off, len) == 0 ? 0 : -EFAULT;
    }
    if (copy_from_user(buf + off, (const void __user *)data, len) != 0) {
        return -EFAULT;
    }
    if (fx) {
        k_memcpy(aligned, buf + off, FPU_STATE_SIZE);
        fpu_to_linux(aligned, buf);
    }
    if (fpu_from_linux(buf, aligned) != 0) {
        return -EINVAL;
    }
    k_memcpy(t->fpu, aligned, FPU_STATE_SIZE);
    return 0;
}

/** PTRACE_GETSIGINFO and PTRACE_SETSIGINFO: the siginfo of `t`'s stop, as
 * the 128-byte i386 `siginfo_t`. */
static int siginfo_access(process_t *t, uint32_t data, int set)
{
    uint8_t buf[128];
    int32_t code;

    if (!set) {
        sigframe_fill_siginfo(buf, (int)((t->ptrace_status >> 8) & 0x7F), &t->ptrace_info);
        return copy_to_user((void __user *)data, buf, sizeof(buf)) == 0 ? 0 : -EFAULT;
    }
    if (copy_from_user(buf, (const void __user *)data, sizeof(buf)) != 0) {
        return -EFAULT;
    }
    k_memcpy(&code, buf + 8, 4);
    k_memset(&t->ptrace_info, 0, sizeof(t->ptrace_info));
    t->ptrace_info.code = code;
    k_memcpy(&t->ptrace_info.pid, buf + 12, 4);
    k_memcpy(&t->ptrace_info.uid, buf + 16, 4);
    k_memcpy(&t->ptrace_info.status, buf + 20, 4);
    k_memcpy(&t->ptrace_info.addr, buf + 12, 4);
    return 0;
}

/** PTRACE_GET_THREAD_AREA and PTRACE_SET_THREAD_AREA: the TLS entry `idx` of
 * `t`, whose one entry is GDT_TLS_ENTRY, as a `user_desc` at `data`. */
static int tls_access(process_t *t, uint32_t idx, uint32_t data, int set)
{
    user_desc_t desc;
    uint32_t base;

    if (idx != 6u) {
        return -EINVAL;
    }
    if (!set) {
        desc.entry_number = idx;
        desc.base_addr = t->tls_base;
        desc.limit = 0xFFFFFu;
        desc.flags = UD_SEG_32BIT | UD_LIMIT_IN_PAGES | UD_USEABLE;
        return copy_to_user((void __user *)data, &desc, sizeof(desc)) == 0 ? 0 : -EFAULT;
    }
    if (copy_from_user(&desc, (const void __user *)data, sizeof(desc)) != 0) {
        return -EFAULT;
    }
    if (tls_resolve_base(&desc, &base) != 0) {
        return -EINVAL;
    }
    t->tls_base = base;
    return 0;
}

/** The requests that need `t` stopped for the caller, as Linux has them. */
static int32_t ptrace_stopped(process_t *t, uint32_t request, uint32_t addr, uint32_t data)
{
    uint32_t word;
    uint32_t r[R_COUNT];
    int32_t rc;
    int i;

    switch (request) {
    case PTRACE_PEEKTEXT:
    case PTRACE_PEEKDATA:
        rc = paging_access(t->mm, addr, &word, sizeof(word), 0);
        if (rc == 0 && copy_to_user((void __user *)data, &word, sizeof(word)) != 0) {
            rc = -EFAULT;
        }
        return rc;
    case PTRACE_POKETEXT:
    case PTRACE_POKEDATA:
        return paging_access(t->mm, addr, &data, sizeof(data), 1);
    case PTRACE_PEEKUSER:
        rc = user_peek(t, addr, &word);
        if (rc == 0 && copy_to_user((void __user *)data, &word, sizeof(word)) != 0) {
            rc = -EFAULT;
        }
        return rc;
    case PTRACE_POKEUSER:
        return user_poke(t, addr, data);
    case PTRACE_GETREGS:
        regs_get(t, r);
        return copy_to_user((void __user *)data, r, sizeof(r)) == 0 ? 0 : -EFAULT;
    case PTRACE_SETREGS:
        if (copy_from_user(r, (const void __user *)data, sizeof(r)) != 0) {
            return -EFAULT;
        }
        for (i = 0; i < R_COUNT; i++) {
            rc = regs_set(t, i, r[i]);
            if (rc != 0) {
                return rc;
            }
        }
        return 0;
    case PTRACE_GETFPREGS:
    case PTRACE_SETFPREGS:
        return fp_access(t, data, 0, request == PTRACE_SETFPREGS);
    case PTRACE_GETFPXREGS:
    case PTRACE_SETFPXREGS:
        return fp_access(t, data, 1, request == PTRACE_SETFPXREGS);
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
        return (t->ptrace & PT_SEIZED) ? 0 : -EIO;
    case PTRACE_SETOPTIONS:
        if (data & ~PTRACE_O_MASK) {
            return -EINVAL;
        }
        t->ptrace_opts = data;
        return 0;
    case PTRACE_GETEVENTMSG:
        return copy_to_user((void __user *)data, &t->ptrace_msg, 4) == 0 ? 0 : -EFAULT;
    case PTRACE_GETSIGINFO:
    case PTRACE_SETSIGINFO:
        return siginfo_access(t, data, request == PTRACE_SETSIGINFO);
    case PTRACE_GET_THREAD_AREA:
    case PTRACE_SET_THREAD_AREA:
        return tls_access(t, addr, data, request == PTRACE_SET_THREAD_AREA);
    default:
        return -EIO;
    }
}

int32_t sys_ptrace(isr_params_t *params)
{
    uint32_t request = params->ebx;
    uint32_t addr = params->edx;
    uint32_t data = params->esi;
    process_t *t;
    struct k_siginfo info;

    if (!current_proc) {
        return -ESRCH;
    }
    if (request == PTRACE_TRACEME) {
        return ptrace_traceme();
    }
    t = get_process(params->ecx);
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
                t->state = PROC_READY;
            }
        }
        return 0;
    }
    if (t->ptrace_status == 0) {
        return -ESRCH;
    }
    return ptrace_stopped(t, request, addr, data);
}
