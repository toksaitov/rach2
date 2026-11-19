/**
 * Host test of ptrace requests across a copy that sleeps and a listening stop.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: rach2/sys/proc/child_signal.c rach2/sys/proc/siginfo.c
 *
 * A register-set request holds the stop across its iovec and payload
 * copies. During either copy a sibling request cannot resume the tracee,
 * and a pending SIGKILL cannot wake it before the hold ends. `schedule`
 * records and ends a stop, so the status and siginfo of a single step over a
 * system call can be checked without a guest; the guest test ptracemusl
 * checks the system call's result and the next instruction as well. The
 * namespace stubs translate an id by adding a different amount for the
 * tracer and the tracee, so that a siginfo shows whose view it was given in.
 */
#include "proc/ptrace.c"

extern int printf(const char *format, ...);
extern void *malloc(unsigned long bytes);
extern void free(void *ptr);

static process_t self, tracee;
static struct tgroup self_group, tracee_group;
static struct mm space;
struct pidns init_pidns;
struct percpu cpu_table[NR_CPUS] = {{.current = &self}};
process_t *proc_list = &self;
uint32_t proc_dead_threads;
static int failures, resume_on_copy, kill_on_copy, register_reads, wakes;
static int stops, listen_interrupts, kill_on_resume, resume_signo;
static unsigned notifications_woken;
static uint32_t stop_status;
static struct k_siginfo stop_info, posted_info;
static struct userns self_ns, tracee_ns;
static const process_t *posted_to, *child_info_receiver;

/** Count a failure and print `row` when `ok` is 0. */
static void check(int ok, const char *row)
{
    if (!ok) {
        printf("ptrace-hosttest: fail: %s\n", row);
        failures++;
    }
}

/** The tracer or the tracee by `pid`, NULL for any other. */
process_t *get_process(uint32_t pid)
{
    return pid == self.pid ? &self : pid == tracee.pid ? &tracee : NULL;
}

/** The model puts all threads in the first PID namespace. */
struct pidns *proc_pidns(const process_t *p)
{
    (void)p;
    return &init_pidns;
}

/** The records are static, so a PID lookup needs no reference count. */
process_t *get_process_ref(uint32_t pid)
{
    return get_process(pid);
}

/** The records stay alive throughout a row. */
void proc_ref_get(process_t *p)
{
    (void)p;
}

/** The records stay alive throughout a row. */
void proc_ref_put(process_t *p)
{
    (void)p;
}

/** The model's process list is static. */
uint32_t tasklist_read_lock(void)
{
    return 0;
}

/** The model's process list is static. */
void tasklist_read_unlock(uint32_t flags)
{
    (void)flags;
}

/** No signaling can occur in the model while the signal state is locked. */
uint32_t signal_state_lock(void)
{
    return 0;
}

/** Restore the model's empty interrupt flags. */
void signal_state_unlock(uint32_t flags)
{
    (void)flags;
}

/** The model invokes the handler without the big lock. */
void bkl_enter(void)
{}

/** The model invokes the handler without the big lock. */
void bkl_exit(void)
{}

/** The model's stops run without the big lock, as a traced call's do. */
int bkl_held(void)
{
    return 0;
}

/** The model runs the tracee only during its own simulated stops. */
void sched_wait_off_cpu(const process_t *t)
{
    check(t->state == PROC_STOPPED && (t->ptrace & PT_FROZEN), "wait for frozen tracee");
}

/** Issue a ptrace request against the model's tracee. */
static long request_tracee(unsigned long request, uintptr_t addr, uintptr_t data)
{
    return ptrace_request(request, tracee.pid, addr, data);
}

/** Copy `len` bytes from the host address `src` and return 0. The copy
 * that brings `resume_on_copy` down to 0 tries to resume the tracee, as
 * another thread of the tracer could while it sleeps. With `kill_on_copy`,
 * it instead leaves SIGKILL pending behind the hold. */
int copy_from_user(void *dst, const void __user *src, size_t len)
{
    k_memcpy(dst, src, len);
    if (kill_on_copy && --kill_on_copy == 0) {
        check(tracee.state == PROC_STOPPED && (tracee.ptrace & PT_FROZEN),
              "SIGKILL during a frozen copy");
        tracee.sig_pending |= KSIG_BIT(SIGKILL);
    }
    if (resume_on_copy && --resume_on_copy == 0) {
        check(request_tracee(PTRACE_CONT, 0, 0) == -ESRCH && tracee.ptrace_status != 0,
              "a concurrent resume cannot take a frozen stop");
    }
    return 0;
}

/** Copy `len` bytes to the host address `dst` and return 0. */
int copy_to_user(void __user *dst, const void *src, size_t len)
{
    k_memcpy(dst, src, len);
    return 0;
}

/** The host's malloc, for a request's buffer of a register set. */
void *kmalloc(size_t bytes)
{
    return malloc(bytes);
}

/** The host's free. */
void kfree(void *p)
{
    free(p);
}

/** One word, the size and unit of every register set. */
long arch_regset_size(const process_t *t, unsigned long type, uint32_t *unit)
{
    (void)t;
    (void)type;
    *unit = sizeof(unsigned long);
    return sizeof(unsigned long);
}

/** Zero the set's word in `out` and count an access to the registers,
 * which must still have its stop held. */
void arch_regset_get(const process_t *t, unsigned long type, uint8_t *out)
{
    check(t->state == PROC_STOPPED && (t->ptrace & PT_FROZEN), "read frozen registers");
    (void)type;
    k_memset(out, 0, sizeof(unsigned long));
    register_reads++;
}

/** Count an access to the registers, as `arch_regset_get` does; 0. */
long arch_regset_set(process_t *t, unsigned long type, const uint8_t *in, uint32_t len)
{
    check(t->state == PROC_STOPPED && (t->ptrace & PT_FROZEN), "write frozen registers");
    (void)type;
    (void)in;
    (void)len;
    register_reads++;
    return 0;
}

/**
 * Record the current thread's stop status and siginfo, as the tracer would
 * read them, and act for the tracer while the tracee sleeps. While
 * `listen_interrupts` is above 0 the tracer puts the tracee in a listening
 * stop and sends PTRACE_INTERRUPT, which resumes it inside `ptrace_stop`,
 * and with `kill_on_resume` SIGKILL is then pending. Otherwise the stop
 * ends: its status is cleared and the thread runs.
 */
void schedule(void)
{
    isr_params_t request;

    stops++;
    stop_status = current_proc->ptrace_status;
    stop_info = current_proc->ptrace_info;
    if (listen_interrupts) {
        listen_interrupts--;
        check(ptrace_listen(&tracee) == 0, "listen during return stop");
        k_memset(&request, 0, sizeof(request));
        request.ebx = PTRACE_INTERRUPT;
        request.ecx = tracee.pid;
        cpu_table[0].current = &self;
        check(sys_ptrace(&request) == 0, "interrupt during return stop");
        cpu_table[0].current = &tracee;
        if (kill_on_resume) {
            tracee.sig_pending |= KSIG_BIT(SIGKILL);
        }
        return;
    }
    current_proc->ptrace_signo = resume_signo;
    current_proc->ptrace_status = 0;
    current_proc->state = PROC_RUNNING;
}

/** Make `t` ready and count the wakeup. */
void sched_make_ready(process_t *t)
{
    t->state = PROC_READY;
    wakes++;
}

/** A signal's wake: make `t` ready, clearing its deadline, if it is blocked,
 * and count the wakeup. */
void sched_wake_signal(process_t *t)
{
    if (t->state == PROC_BLOCKED) {
        t->wakeup_tick = 0;
        sched_make_ready(t);
    }
}

/** Reset the tracer, pid 10 with uid 5, as the current thread, and its
 * tracee, pid 11 with uid 7, seized and in a group stop. */
static void setup(void)
{
    k_memset(&self, 0, sizeof(self));
    k_memset(&tracee, 0, sizeof(tracee));
    k_memset(&self_group, 0, sizeof(self_group));
    k_memset(&tracee_group, 0, sizeof(tracee_group));
    self.pid = self.tgid = 10;
    self.ruid = 5;
    self.tg = &self_group;
    self.state = PROC_RUNNING;
    self_group.live = 1;
    self.next = &tracee;
    tracee.pid = tracee.tgid = 11;
    tracee.ruid = 7;
    tracee.tg = &tracee_group;
    tracee.mm = &space;
    tracee.state = PROC_STOPPED;
    tracee.ptrace = PT_TRACED | PT_SEIZED;
    tracee.tracer = self.tgid;
    tracee.ptrace_status = (PTRACE_EVENT_STOP << 16) | (SIGSTOP << 8) | 0x7f;
    cpu_table[0].current = &self;
    wakes = register_reads = resume_on_copy = kill_on_copy = 0;
    stops = listen_interrupts = kill_on_resume = resume_signo = 0;
    posted_to = child_info_receiver = NULL;
    notifications_woken = 0;
    k_memset(&posted_info, 0, sizeof(posted_info));
}

/** On the way back to user mode, a tracee whose listening stop
 * PTRACE_INTERRUPT ended stops again, as an event stop of SIGTRAP, for each
 * interrupt; SIGKILL ends the stops with PT_INTERRUPT still set. */
static void return_stops(void)
{
    setup();
    cpu_table[0].current = &tracee;
    tracee.ptrace |= PT_INTERRUPT;
    listen_interrupts = 2;
    ptrace_return_stops();
    check(stops == 3 && wakes == 2 && !(tracee.ptrace & PT_INTERRUPT) &&
              stop_status == ((PTRACE_EVENT_STOP << 16) | (SIGTRAP << 8) | 0x7f),
          "listening interrupts stop before user return");

    setup();
    cpu_table[0].current = &tracee;
    tracee.ptrace |= PT_INTERRUPT;
    listen_interrupts = kill_on_resume = 1;
    ptrace_return_stops();
    check(stops == 1 && (tracee.sig_pending & KSIG_BIT(SIGKILL)) && (tracee.ptrace & PT_INTERRUPT),
          "kill ends return stop sequence");
}

/**
 * The siginfo of a trace stop's SIGCHLD has the tracee's ids as the tracer
 * sees them; a signal the tracer resumes with in place of the stop's has the
 * tracer's ids as the tracee sees them at a signal-delivery stop, and at a
 * system-call stop is SI_KERNEL without ids, as Linux's send_sig posts it;
 * and the SIGCHLD of a tracee's end is made for the tracer, not for the
 * parent.
 */
static void namespace_ids(void)
{
    struct k_siginfo info;
    isr_params_t frame;

    setup();
    cpu_table[0].current = &tracee;
    k_memset(&info, 0, sizeof(info));
    check(ptrace_signal_stop(SIGUSR1, &info) == 0 && posted_to == &self &&
              posted_info.code == CLD_TRAPPED && posted_info.pid == 211 && posted_info.uid == 2007,
          "trap notice has tracee ids as tracer sees them");

    setup();
    cpu_table[0].current = &tracee;
    resume_signo = SIGUSR2;
    k_memset(&info, 0, sizeof(info));
    check(ptrace_signal_stop(SIGUSR1, &info) == SIGUSR2 && info.code == SI_USER &&
              info.pid == 110 && info.uid == 1005,
          "resumed signal has tracer ids as tracee sees them");

    setup();
    cpu_table[0].current = &tracee;
    tracee.ptrace |= PT_SYSCALL;
    resume_signo = SIGUSR2;
    k_memset(&frame, 0, sizeof(frame));
    ptrace_syscall_stop(&frame, 20, 1);
    check(posted_to == &tracee && posted_info.code == SI_KERNEL && posted_info.pid == 0 &&
              posted_info.uid == 0,
          "syscall stop signal is SI_KERNEL without ids");

    setup();
    tracee.parent_pid = 1;
    ptrace_exit_notify(&tracee);
    check(child_info_receiver == &self && posted_to == &self, "end notice is made for the tracer");
}

/* An untraced clone is not inherited by a tracer. A zero-signal child's
 * real parent still wakes, and a new parent image restores SIGCHLD. */
static void clone_notifications(void)
{
    setup();
    self.ptrace = PT_TRACED;
    self.tracer = 99;
    self.ptrace_opts = 1u << PTRACE_EVENT_CLONE;
    tracee.ptrace = 0;
    tracee.tracer = 0;
    check(ptrace_fork_attach(&tracee, 0x00800000u) == 0 && !tracee.ptrace && !tracee.tracer,
          "CLONE_UNTRACED prevents inherited tracing");
    check(ptrace_fork_attach(&tracee, 0) == PTRACE_EVENT_CLONE && tracee.tracer == 99,
          "ordinary zero-signal clone inherits requested tracing");

    setup();
    tracee.parent_pid = self.tgid;
    tracee.tg->no_child_signal = 1;
    ptrace_parent_notify(&tracee);
    check(!posted_to && notifications_woken == 1, "zero-signal child wakes without SIGCHLD");
    tracee.tg->live = 0;
    ptrace_exit_notify(&tracee);
    check(!posted_to && notifications_woken == 2,
          "tracer that is the real parent keeps zero-signal exit notification");
    self.tg->exec_generation = 1;
    ptrace_parent_notify(&tracee);
    check(posted_to == &self && notifications_woken == 3 && !tracee.tg->no_child_signal,
          "new parent image restores ordinary child notification");

    setup();
    tracee.parent_pid = self.tgid;
    tracee.tg->no_child_signal = 1;
    self.tg->exec_generation = (uint32_t)-1;
    tracee.tg->parent_exec_generation = (uint32_t)-1;
    proc_exec_generation(&self);
    check(self.tg->exec_generation == 1 && !tracee.tg->parent_exec_generation &&
              proc_child_signal(&tracee, &self),
          "exec generation wrap keeps old children distinct");
}

/** Store `value` at `p` and return what was there: one thread. */
unsigned int arch_atomic_xchg(volatile unsigned int *p, unsigned int value)
{
    unsigned int old = *p;

    *p = value;
    return old;
}

/**
 * Run every row; 0 when each passes, 1 otherwise. A register-set request
 * holds the tracee across the iovec and data copies. PTRACE_INTERRUPT
 * releases a listening tracee. PTRACE_SETOPTIONS and PTRACE_SEIZE fail with
 * -EINVAL for an option above bit 31. A single step over a system call reports
 * TRAP_TRACE with no TRACESYSGOOD mark; the stops on the way back to user
 * mode; and the ids of the siginfo records ptrace sends.
 */
int main(void)
{
    struct kernel_iovec iov;
    unsigned long word = 0;
    isr_params_t frame;
    int set;

    for (set = 0; set < 2; set++) {
        setup();
        iov.iov_base = (uintptr_t)&word;
        iov.iov_len = sizeof(word);
        check(request_tracee(set ? PTRACE_SETREGSET : PTRACE_GETREGSET, 1, (uintptr_t)&iov) == 0 &&
                  register_reads == 1 && !(tracee.ptrace & PT_FROZEN),
              "stopped regset access");
        register_reads = 0;
        resume_on_copy = 1;
        check(request_tracee(set ? PTRACE_SETREGSET : PTRACE_GETREGSET, 1, (uintptr_t)&iov) == 0 &&
                  resume_on_copy == 0 && register_reads == 1,
              "regset holds the stop through its iovec copy");
    }
    setup();
    iov.iov_base = (uintptr_t)&word;
    iov.iov_len = sizeof(word);
    resume_on_copy = 2;
    check(request_tracee(PTRACE_SETREGSET, 1, (uintptr_t)&iov) == 0 && resume_on_copy == 0 &&
              register_reads == 1,
          "regset holds the stop through its payload copy");
    setup();
    kill_on_copy = 2;
    check(request_tracee(PTRACE_SETREGSET, 1, (uintptr_t)&iov) == 0 && kill_on_copy == 0 &&
              register_reads == 1 && tracee.state == PROC_READY && wakes == 1,
          "SIGKILL wakes the tracee when the request releases the stop");
    setup();
    check(ptrace_listen(&tracee) == 0, "listen accepts group event");
    k_memset(&frame, 0, sizeof(frame));
    frame.ebx = PTRACE_INTERRUPT;
    frame.ecx = tracee.pid;
    check(sys_ptrace(&frame) == 0 && tracee.ptrace_status == 0 && (tracee.ptrace & PT_INTERRUPT) &&
              !(tracee.ptrace & PT_LISTEN) && wakes == 1,
          "interrupt releases listening tracee");
    setup();
    check(request_tracee(PTRACE_SETOPTIONS, 0, 0x100000000UL) == -EINVAL,
          "setoptions refuses upper bits");
    tracee.ptrace = 0;
    check(ptrace_attach(&tracee, 1, 0x100000000UL) == -EINVAL && tracee.ptrace == 0,
          "seize refuses upper bits without attaching");
    setup();
    cpu_table[0].current = &tracee;
    tracee.ptrace |= PT_STEP;
    tracee.ptrace_opts = PTRACE_O_TRACESYSGOOD;
    ptrace_syscall_stop(&frame, 20, 1);
    check(stop_status == ((SIGTRAP << 8) | 0x7f) && stop_info.code == TRAP_TRACE,
          "syscall single step reports TRAP_TRACE without syscall marker");
    return_stops();
    namespace_ids();
    clone_notifications();
    printf("ptrace-hosttest: result: %s\n", failures ? "fail" : "pass");
    return failures != 0;
}

/** The tracee's user namespace for the tracee, the tracer's for any other. */
struct userns *proc_userns(const process_t *p)
{
    return p == &tracee ? &tracee_ns : &self_ns;
}

/** The kernel id `id` plus 1000 in the tracee's user namespace, plus 2000 in
 * the tracer's. */
uint32_t ns_uid_from_kernel(const struct userns *ns, uint32_t id)
{
    return id + (ns == &tracee_ns ? 1000 : 2000);
}

/** Make `tgid` the tracer of `p`; the model keeps no index. */
void proc_set_tracer(process_t *p, uint32_t tgid)
{
    p->tracer = tgid;
}

/** The tracee, when `tgid` traces it, as the index by tracer gives it. */
process_t *proc_tracees(uint32_t tgid)
{
    return tgid != 0 && tracee.tracer == tgid ? &tracee : NULL;
}

/** The process the PID `pid` names, its kernel id in the model. */
process_t *proc_named(uint32_t pid)
{
    return get_process(pid);
}

/** The kernel id `pid` plus 100 as the tracee sees it, plus 200 as any
 * other process does. */
uint32_t pid_seen_by(const process_t *p, uint32_t pid)
{
    return pid + (p == &tracee ? 100 : 200);
}

/** A zeroed siginfo for the notice of a child's change; the receiver it is
 * made for is recorded. */
void proc_child_info(const process_t *p, const process_t *receiver, struct k_siginfo *info)
{
    (void)p;
    child_info_receiver = receiver;
    k_memset(info, 0, sizeof(*info));
}

/** The host model has no interrupt flag to save. */
uint32_t kirq_save(void)
{
    return 0;
}

/** The host model has no interrupt flag to restore. */
void kirq_restore(uint32_t flags)
{
    (void)flags;
}

/** A failure: the test's `schedule` ends every stop. */
void idle_wait_irq_off(void)
{
    check(0, "unexpected idle wait");
}

/** No row opens a pidfd, so a detached zombie thread has none to wake. */
void pidfd_id_gone(process_t *p)
{
    (void)p;
}

/** Count child-state wakes independently of the model's scheduled stops. */
void wq_wake_all(wait_queue_t *wq)
{
    notifications_woken++;
    (void)wq;
}

/** Record the receiver and the siginfo of a signal sent to a process. */
void signal_post_info(process_t *p, int sig, const struct k_siginfo *info)
{
    (void)sig;
    posted_to = p;
    posted_info = *info;
}

/** Record the receiver and the siginfo of a signal sent to a thread. */
void signal_post_thread(process_t *p, int sig, const struct k_siginfo *info)
{
    (void)sig;
    posted_to = p;
    posted_info = *info;
}

/** Nothing: the guest test ptracemusl checks the trap flag. */
void arch_ptrace_step(process_t *p, int on)
{
    (void)p;
    (void)on;
}

/** -EIO: no row makes a request of the architecture's own. */
long arch_ptrace(process_t *p, long request, uintptr_t addr, unsigned long data)
{
    (void)p;
    (void)request;
    (void)addr;
    (void)data;
    return -EIO;
}

/** 0: every row's tracer is a native caller. */
int in_compat_syscall(void)
{
    return 0;
}

/** Nothing: no row writes a siginfo to the user. */
void siginfo_to_caller(uint8_t *buf, int sig, const struct k_siginfo *info)
{
    (void)buf;
    (void)sig;
    (void)info;
}

/** 0: no row reads a siginfo from the user. */
int32_t siginfo_from_caller(uintptr_t from, int sig, int32_t *given, struct k_siginfo *info)
{
    (void)from;
    (void)sig;
    (void)given;
    (void)info;
    return 0;
}

/** Nothing: the tracee's address space is static. */
void mm_get(struct mm *mm)
{
    (void)mm;
}

/** Nothing: the tracee's address space is static. */
void mm_put(struct mm *mm)
{
    (void)mm;
}

/** -EIO: no row reads or writes the tracee's memory. */
int32_t paging_access(struct mm *mm, uintptr_t addr, void *buf, uint32_t len, int write)
{
    (void)mm;
    (void)addr;
    (void)buf;
    (void)len;
    (void)write;
    return -EIO;
}

/** A zeroed frame: no row asks for a stopped thread's registers. */
isr_params_t *proc_user_frame(const process_t *t)
{
    static isr_params_t frame;

    (void)t;
    return &frame;
}
