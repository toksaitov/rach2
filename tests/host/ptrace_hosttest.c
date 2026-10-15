/**
 * Host test of ptrace requests across a copy that sleeps and a listening
 * stop.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 *
 * The copy of a register-set request's iovec can sleep, and the tracee can
 * be resumed meanwhile; the request, in either direction, must then answer
 * -ESRCH without reaching its registers. The test's `schedule` records a
 * stop and ends it, so that the status and siginfo of a single step over a
 * system call can be checked without a guest; the guest test ptracemusl
 * checks the system call's result and the next instruction as well.
 */
#include "proc/ptrace.c"

extern int printf(const char *format, ...);
extern void *malloc(unsigned long bytes);
extern void free(void *ptr);

static process_t self, tracee;
static struct tgroup self_group, tracee_group;
static struct mm space;
struct percpu cpu_table[NR_CPUS] = {{.current = &self}};
process_t *proc_list = &self;
uint32_t proc_dead_threads;
static int failures, resume_on_copy, register_reads, wakes;
static int stops, listen_interrupts, kill_on_resume;
static uint32_t stop_status;
static struct k_siginfo stop_info;

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

/** Copy `len` bytes from the host address `src` and return 0; with
 * `resume_on_copy` set, resume the tracee once, as another thread of the
 * tracer could while the copy sleeps. */
int copy_from_user(void *dst, const void __user *src, size_t len)
{
    k_memcpy(dst, src, len);
    if (resume_on_copy) {
        tracee.ptrace_status = 0;
        resume_on_copy = 0;
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
long arch_regset_size(unsigned long type, uint32_t *unit)
{
    (void)type;
    *unit = sizeof(unsigned long);
    return sizeof(unsigned long);
}

/** Zero the set's word in `out` and count an access to the registers,
 * which a resumed tracee must never reach. */
void arch_regset_get(const process_t *t, unsigned long type, uint8_t *out)
{
    (void)t;
    (void)type;
    k_memset(out, 0, sizeof(unsigned long));
    register_reads++;
}

/** Count an access to the registers, as `arch_regset_get` does; 0. */
long arch_regset_set(process_t *t, unsigned long type, const uint8_t *in, uint32_t len)
{
    (void)t;
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
    current_proc->ptrace_status = 0;
    current_proc->state = PROC_RUNNING;
}

/** Make `t` ready and count the wakeup. */
void sched_make_ready(process_t *t)
{
    t->state = PROC_READY;
    wakes++;
}

/** Reset the tracer, pid 10 with uid 0, as the current thread, and its
 * tracee, pid 11, seized and in a group stop. */
static void setup(void)
{
    k_memset(&self, 0, sizeof(self));
    k_memset(&tracee, 0, sizeof(tracee));
    k_memset(&self_group, 0, sizeof(self_group));
    k_memset(&tracee_group, 0, sizeof(tracee_group));
    self.pid = self.tgid = 10;
    self.tg = &self_group;
    self.state = PROC_RUNNING;
    self_group.live = 1;
    self.next = &tracee;
    tracee.pid = tracee.tgid = 11;
    tracee.tg = &tracee_group;
    tracee.mm = &space;
    tracee.state = PROC_STOPPED;
    tracee.ptrace = PT_TRACED | PT_SEIZED;
    tracee.tracer = self.tgid;
    tracee.ptrace_status = (PTRACE_EVENT_STOP << 16) | (SIGSTOP << 8) | 0x7f;
    cpu_table[0].current = &self;
    wakes = register_reads = resume_on_copy = 0;
    stops = listen_interrupts = kill_on_resume = 0;
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
 * Run every row; 0 when each passes, 1 otherwise. A register-set request
 * rechecks the tracee after its copy slept; PTRACE_INTERRUPT releases a
 * listening tracee; PTRACE_SETOPTIONS and PTRACE_SEIZE fail with -EINVAL
 * for an option above bit 31; a single step over a system call reports
 * TRAP_TRACE with no TRACESYSGOOD mark; and the stops on the way back to
 * user mode.
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
        check(regset_access(tracee.pid, &tracee, 1, (uintptr_t)&iov, set) == 0 &&
                  register_reads == 1,
              "stopped regset access");
        register_reads = 0;
        resume_on_copy = 1;
        check(regset_access(tracee.pid, &tracee, 1, (uintptr_t)&iov, set) == -ESRCH &&
                  register_reads == 0,
              "regset revalidates after sleeping copy");
    }
    setup();
    check(ptrace_listen(&tracee) == 0, "listen accepts group event");
    k_memset(&frame, 0, sizeof(frame));
    frame.ebx = PTRACE_INTERRUPT;
    frame.ecx = tracee.pid;
    check(sys_ptrace(&frame) == 0 && tracee.ptrace_status == 0 && (tracee.ptrace & PT_INTERRUPT) &&
              !(tracee.ptrace & PT_LISTEN) && wakes == 1,
          "interrupt releases listening tracee");
    setup();
    check(ptrace_stopped(&tracee, tracee.pid, PTRACE_SETOPTIONS, 0, 0x100000000UL) == -EINVAL,
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
    printf("ptrace-hosttest: result: %s\n", failures ? "fail" : "pass");
    return failures != 0;
}

/** NULL for every process; the stubs that take a namespace ignore it. */
struct userns *proc_userns(const process_t *p)
{
    (void)p;
    return NULL;
}

/** The kernel id `id` itself. */
uint32_t ns_uid_from_kernel(const struct userns *ns, uint32_t id)
{
    (void)ns;
    return id;
}

/** The PID `pid` itself. */
uint32_t pid_named(uint32_t pid)
{
    return pid;
}

/** The kernel id `pid` itself, whatever `p`. */
uint32_t pid_seen_by(const process_t *p, uint32_t pid)
{
    (void)p;
    return pid;
}

/** A zeroed siginfo for the notice of a child's change. */
void proc_child_info(const process_t *p, struct k_siginfo *info)
{
    (void)p;
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

/** Nothing: the tracer is told of a stop through `schedule`. */
void wq_wake_all(wait_queue_t *wq)
{
    (void)wq;
}

/** Nothing: no row checks a signal sent to a process. */
void signal_post_info(process_t *p, int sig, const struct k_siginfo *info)
{
    (void)p;
    (void)sig;
    (void)info;
}

/** Nothing: no row checks a signal sent to a thread. */
void signal_post_thread(process_t *p, int sig, const struct k_siginfo *info)
{
    (void)p;
    (void)sig;
    (void)info;
}

/** Nothing: the guest test ptracemusl checks the trap flag. */
void arch_ptrace_step(process_t *p, int on)
{
    (void)p;
    (void)on;
}

/** -EIO: no row makes a request of the architecture's own. */
long arch_ptrace(process_t *p, uint32_t pid, long request, uintptr_t addr, unsigned long data)
{
    (void)p;
    (void)pid;
    (void)request;
    (void)addr;
    (void)data;
    return -EIO;
}

/** Nothing: no row writes a siginfo to the user. */
void sigframe_fill_siginfo(uint8_t *buf, int sig, const struct k_siginfo *info)
{
    (void)buf;
    (void)sig;
    (void)info;
}

/** Nothing: no row reads a siginfo from the user. */
void sigframe_take_siginfo(const uint8_t *buf, struct k_siginfo *info)
{
    (void)buf;
    (void)info;
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
