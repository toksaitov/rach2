/**
 * Check ptrace requests across a sleeping copy and a listening stop.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 *
 * The iovec copy can resume the tracee before returning. Both register-set
 * directions must then answer ESRCH without reading its registers. The
 * scheduler reports a stop and resumes it, so the syscall step's status
 * and siginfo can be checked without a guest. Guest ptracemusl checks the
 * syscall result and the next instruction as well.
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
static uint32_t stop_status;
static struct k_siginfo stop_info;

/** Record each independent assertion. */
static void check(int ok, const char *row)
{
    if (!ok) {
        printf("ptrace-hosttest: fail: %s\n", row);
        failures++;
    }
}

/** Find one of the two modeled threads. */
process_t *get_process(uint32_t pid)
{
    return pid == self.pid ? &self : pid == tracee.pid ? &tracee : NULL;
}

/** Copy an iovec, then model a resume by another tracer thread. */
int copy_from_user(void *dst, const void __user *src, size_t len)
{
    k_memcpy(dst, src, len);
    if (resume_on_copy) {
        tracee.ptrace_status = 0;
        resume_on_copy = 0;
    }
    return 0;
}

/** Copy a result into the test's resident buffer. */
int copy_to_user(void __user *dst, const void *src, size_t len)
{
    k_memcpy(dst, src, len);
    return 0;
}

/** Allocate the real request's temporary register-set buffer. */
void *kmalloc(size_t bytes)
{
    return malloc(bytes);
}

/** Free the request's temporary buffer. */
void kfree(void *p)
{
    free(p);
}

/** Supply a single word as the architecture's register set. */
long arch_regset_size(unsigned long type, uint32_t *unit)
{
    (void)type;
    *unit = sizeof(unsigned long);
    return sizeof(unsigned long);
}

/** Count register reads; a resumed tracee must never reach this call. */
void arch_regset_get(const process_t *t, unsigned long type, uint8_t *out)
{
    (void)t;
    (void)type;
    k_memset(out, 0, sizeof(unsigned long));
    register_reads++;
}

/** Count register writes by the same rule as reads. */
long arch_regset_set(process_t *t, unsigned long type, const uint8_t *in, uint32_t len)
{
    (void)t;
    (void)type;
    (void)in;
    (void)len;
    register_reads++;
    return 0;
}

/** Record the stop before the modeled tracer resumes its thread. */
void schedule(void)
{
    stop_status = current_proc->ptrace_status;
    stop_info = current_proc->ptrace_info;
    current_proc->ptrace_status = 0;
    current_proc->state = PROC_RUNNING;
}

/** Count requests that make a listening tracee runnable. */
void sched_make_ready(process_t *t)
{
    t->state = PROC_READY;
    wakes++;
}

/** Initialize a privileged tracer and its stopped child. */
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
}

/** Exercise regset revalidation, listen interruption, options and stepping. */
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
    printf("ptrace-hosttest: result: %s\n", failures ? "fail" : "pass");
    return failures != 0;
}

/** Treat every modeled uid as belonging to the initial namespace. */
struct userns *proc_userns(const process_t *p)
{
    (void)p;
    return NULL;
}

/** Preserve a modeled uid. */
uint32_t ns_uid_from_kernel(const struct userns *ns, uint32_t id)
{
    (void)ns;
    return id;
}

/** Preserve a modeled process id. */
uint32_t pid_named(uint32_t pid)
{
    return pid;
}

/** Preserve a process id in the observer's namespace. */
uint32_t pid_seen_by(const process_t *p, uint32_t pid)
{
    (void)p;
    return pid;
}

/** Supply an empty child notification. */
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

/** A stopped thread must be resumed by the scheduler model. */
void idle_wait_irq_off(void)
{
    check(0, "unexpected idle wait");
}

/** The waiting tracer is represented by the recorded stop. */
void wq_wake_all(wait_queue_t *wq)
{
    (void)wq;
}

/** Process notifications need no delivery in this request model. */
void signal_post_info(process_t *p, int sig, const struct k_siginfo *info)
{
    (void)p;
    (void)sig;
    (void)info;
}

/** Thread notifications need no delivery in this request model. */
void signal_post_thread(process_t *p, int sig, const struct k_siginfo *info)
{
    (void)p;
    (void)sig;
    (void)info;
}

/** The guest test checks the real trap flag. */
void arch_ptrace_step(process_t *p, int on)
{
    (void)p;
    (void)on;
}

/** Architecture-specific requests are outside this model. */
long arch_ptrace(process_t *p, uint32_t pid, long request, uintptr_t addr, unsigned long data)
{
    (void)p;
    (void)pid;
    (void)request;
    (void)addr;
    (void)data;
    return -EIO;
}

/** Siginfo packing is outside these rows. */
void sigframe_fill_siginfo(uint8_t *buf, int sig, const struct k_siginfo *info)
{
    (void)buf;
    (void)sig;
    (void)info;
}

/** Siginfo unpacking is outside these rows. */
void sigframe_take_siginfo(const uint8_t *buf, struct k_siginfo *info)
{
    (void)buf;
    (void)info;
}

/** The modeled address space is static. */
void mm_get(struct mm *mm)
{
    (void)mm;
}

/** The modeled address space is static. */
void mm_put(struct mm *mm)
{
    (void)mm;
}

/** Tracee memory access is outside these rows. */
int32_t paging_access(struct mm *mm, uintptr_t addr, void *buf, uint32_t len, int write)
{
    (void)mm;
    (void)addr;
    (void)buf;
    (void)len;
    (void)write;
    return -EIO;
}
