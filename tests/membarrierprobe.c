/**
 * membarrierprobe: membarrier(2), the barrier on the processors that run a
 * process's threads, which user-space RCU, garbage collectors and the .NET
 * runtime issue, on i386 and x86-64, as Linux 6.12 answers it on x86.
 *
 * MEMBARRIER_CMD_QUERY must give 0x3FF, every command, and a new process
 * must have no registrations. Each private barrier must fail with EPERM
 * before its registration, while the global ones need none. A child that
 * registers for the sync-core barrier alone must read back that
 * registration and the plain private one, as Linux reports them, and still
 * be refused the plain private barrier. Each registration must read back
 * through MEMBARRIER_CMD_GET_REGISTRATIONS as it is made. With a second
 * thread spinning, every barrier must return 0, the rseq one also for one
 * processor and for a processor that does not exist, and the thread must
 * go on running. A fork's child must have its parent's registrations. An
 * unknown command, and flags other than MEMBARRIER_CMD_FLAG_CPU on the rseq
 * barrier, must fail with EINVAL.
 *
 * The probe writes etude's report records (probe_report.h) for the case
 * ETUDE_CASE names, an assertion for each check, as `assertion_id` lists
 * them; a failed check fails its assertion and leaves those after it not
 * run. Prints "MEMBARRIERPROBE OK" and exits 0, or "MEMBARRIERPROBE FAIL: "
 * and the reason and exits 1.
 */

#include "probe.h"
#include "probe_report.h"

#if defined __x86_64__
#define SYS_MEMBARRIER 324
#else
#define SYS_MEMBARRIER 375
#endif

#define MEMBARRIER_CMD_QUERY                                0
#define MEMBARRIER_CMD_GLOBAL                               (1 << 0)
#define MEMBARRIER_CMD_GLOBAL_EXPEDITED                     (1 << 1)
#define MEMBARRIER_CMD_REGISTER_GLOBAL_EXPEDITED            (1 << 2)
#define MEMBARRIER_CMD_PRIVATE_EXPEDITED                    (1 << 3)
#define MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED           (1 << 4)
#define MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE          (1 << 5)
#define MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_SYNC_CORE (1 << 6)
#define MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ               (1 << 7)
#define MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_RSEQ      (1 << 8)
#define MEMBARRIER_CMD_GET_REGISTRATIONS                    (1 << 9)
#define MEMBARRIER_CMD_FLAG_CPU                             1

#define EPERM  1
#define EINVAL 22

/** End the calling thread with `status`; it does not return. */
static void sys_exit(int status)
{
    syscall1(SYS_EXIT, status);
    __builtin_unreachable();
}

/** The length of the string `s`. */
static size_t strlen(const char *s)
{
    size_t n = 0;

    while (s[n]) {
        n++;
    }
    return n;
}

/** Write `s` to standard output. */
static void say(const char *s)
{
    syscall3(SYS_WRITE, 1, (long)s, (long)strlen(s));
}

/* The assertions, a check each, in the order the probe makes them. */
static const char *const assertion_id[] = {
    "query", "unregistered", "sync-core-alone", "register", "barriers", "inherit", "einval"};

/** Print "MEMBARRIERPROBE FAIL: " and `why`, end the report at the
 * assertion that `why` fails, and exit 1. */
static void fail(const char *why)
{
    say("MEMBARRIERPROBE FAIL: ");
    say(why);
    say("\n");
    report_stop(why);
    sys_exit(1);
}

/**
 * thread_spawn(fn, stack_top) starts a thread of the probe's process that
 * runs `fn` on the stack ending at `stack_top`, aligned to 16, and ends when
 * `fn` returns; the thread's id, or a negated errno. The flags are Linux's
 * CLONE_VM, CLONE_FS, CLONE_FILES, CLONE_SIGHAND, CLONE_THREAD and
 * CLONE_SYSVSEM, 0x50f00.
 */
long thread_spawn(void (*fn)(void), void *stack_top);

#if defined __x86_64__
__asm__(".text\n"
        ".globl thread_spawn\n"
        "thread_spawn:\n"
        "    subq $16, %rsi\n"
        "    movq %rdi, (%rsi)\n"
        "    movl $0x50f00, %edi\n"
        "    xorl %edx, %edx\n"
        "    xorl %r10d, %r10d\n"
        "    xorl %r8d, %r8d\n"
        "    movl $56, %eax\n"
        "    syscall\n"
        "    testq %rax, %rax\n"
        "    jnz 1f\n"
        "    popq %rax\n"
        "    andq $-16, %rsp\n"
        "    call *%rax\n"
        "    xorl %edi, %edi\n"
        "    movl $60, %eax\n"
        "    syscall\n"
        "1:  ret\n");
#else
__asm__(".text\n"
        ".globl thread_spawn\n"
        "thread_spawn:\n"
        "    pushl %ebx\n"
        "    pushl %esi\n"
        "    pushl %edi\n"
        "    movl 20(%esp), %ecx\n"
        "    subl $16, %ecx\n"
        "    movl 16(%esp), %eax\n"
        "    movl %eax, (%ecx)\n"
        "    movl $0x50f00, %ebx\n"
        "    xorl %edx, %edx\n"
        "    xorl %esi, %esi\n"
        "    xorl %edi, %edi\n"
        "    movl $120, %eax\n"
        "    int $0x80\n"
        "    testl %eax, %eax\n"
        "    jnz 1f\n"
        "    popl %eax\n"
        "    call *%eax\n"
        "    xorl %ebx, %ebx\n"
        "    movl $1, %eax\n"
        "    int $0x80\n"
        "1:  popl %edi\n"
        "    popl %esi\n"
        "    popl %ebx\n"
        "    ret\n");
#endif

/** membarrier(cmd, flags, cpu_id); the call's result, or a negated errno. */
static long membarrier(long cmd, long flags, long cpu_id)
{
    return syscall3(SYS_MEMBARRIER, cmd, flags, cpu_id);
}

/** The registrations the process has made. */
static long registrations(void)
{
    return membarrier(MEMBARRIER_CMD_GET_REGISTRATIONS, 0, 0);
}

/** Fork a child that runs `fn` and exits with its result; the child's wait
 * status. */
static int in_child(int (*fn)(void))
{
    int status = 0;
    long pid = syscall0(SYS_FORK);

    if (pid < 0) {
        fail("fork");
    }
    if (pid == 0) {
        sys_exit(fn());
    }
    if (syscall3(SYS_WAITPID, pid, (long)&status, 0) != pid) {
        fail("a child could not be reaped");
    }
    return status;
}

/** QUERY, and the barriers of a process with no registrations. */
static void run_unregistered(void)
{
    if (membarrier(MEMBARRIER_CMD_QUERY, 0, 0) != 0x3FF || registrations() != 0) {
        fail("MEMBARRIER_CMD_QUERY did not give 0x3FF, or a new process had registrations");
    }
    report_held();

    if (membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0, 0) != -EPERM ||
        membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE, 0, 0) != -EPERM ||
        membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ, 0, 0) != -EPERM ||
        membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ, MEMBARRIER_CMD_FLAG_CPU, 0) != -EPERM) {
        fail("a private barrier before its registration did not fail with EPERM");
    }
    if (membarrier(MEMBARRIER_CMD_GLOBAL, 0, 0) != 0 ||
        membarrier(MEMBARRIER_CMD_GLOBAL_EXPEDITED, 0, 0) != 0) {
        fail("a global barrier without registration did not return 0");
    }
    report_held();
}

/** A child's sync-core registration alone; 0, or the step that failed. */
static int sync_core_alone(void)
{
    if (membarrier(MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_SYNC_CORE, 0, 0) != 0) {
        return 1;
    }
    if (registrations() != (MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED |
                            MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_SYNC_CORE)) {
        return 2;
    }
    if (membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0, 0) != -EPERM ||
        membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ, 0, 0) != -EPERM) {
        return 3;
    }
    if (membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE, 0, 0) != 0) {
        return 4;
    }
    return 0;
}

/** The registrations, each read back as it is made. */
static void run_register(void)
{
    static const long reg[4] = {MEMBARRIER_CMD_REGISTER_GLOBAL_EXPEDITED,
                                MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED,
                                MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_SYNC_CORE,
                                MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_RSEQ};
    long want = 0;
    int i;

    if (in_child(sync_core_alone) != 0) {
        fail("a child registered for the sync-core barrier alone did not read back it and the "
             "private registration, or was not refused the plain private barrier");
    }
    report_held();

    for (i = 0; i < 4; i++) {
        want |= reg[i];
        if (membarrier(reg[i], 0, 0) != 0 || membarrier(reg[i], 0, 0) != 0 ||
            registrations() != want) {
            fail("a registration, made twice, did not return 0 or read back as made");
        }
    }
    report_held();
}

/* The spinning thread's count, the word that stops it, whether it has
 * stopped, and its stack. */
static volatile unsigned long spins;
static volatile int stop, finished;
static unsigned char thread_stack[16384] __attribute__((aligned(16)));

/** The second thread: spin until told to stop. */
static void spinner(void)
{
    while (!stop) {
        spins++;
    }
    finished = 1;
}

/** Every barrier, with a second thread running. */
static void run_barriers(void)
{
    static const long cmd[4] = {MEMBARRIER_CMD_PRIVATE_EXPEDITED,
                                MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE,
                                MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ, MEMBARRIER_CMD_GLOBAL};
    unsigned long before;
    int round, i;

    if (thread_spawn(spinner, thread_stack + sizeof(thread_stack)) <= 0) {
        fail("clone of a thread");
    }
    while (spins == 0) {
        syscall0(SYS_SCHED_YIELD);
    }
    for (round = 0; round < 100; round++) {
        for (i = 0; i < 4; i++) {
            if (membarrier(cmd[i], 0, 0) != 0) {
                fail("a barrier with a second thread running did not return 0");
            }
        }
        if (membarrier(MEMBARRIER_CMD_GLOBAL_EXPEDITED, 0, 0) != 0 ||
            membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ, MEMBARRIER_CMD_FLAG_CPU, 0) != 0 ||
            membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ, MEMBARRIER_CMD_FLAG_CPU, 1) != 0 ||
            membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ, MEMBARRIER_CMD_FLAG_CPU, 1000) != 0 ||
            membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ, MEMBARRIER_CMD_FLAG_CPU, -1) != 0) {
            fail("the expedited global barrier, or the rseq barrier on processor 0, 1, 1000 or "
                 "-1, did not return 0");
        }
    }
    before = spins;
    while (spins == before) {
        syscall0(SYS_SCHED_YIELD);
    }
    stop = 1;
    while (!finished) {
        syscall0(SYS_SCHED_YIELD);
    }
    report_held();
}

/** A fork's child's registrations; 0, or the step that failed. */
static int inherited(void)
{
    if (registrations() !=
        (MEMBARRIER_CMD_REGISTER_GLOBAL_EXPEDITED | MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED |
         MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_SYNC_CORE |
         MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_RSEQ)) {
        return 1;
    }
    if (membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0, 0) != 0) {
        return 2;
    }
    return 0;
}

/** What Linux refuses with EINVAL. */
static void run_einval(void)
{
    if (in_child(inherited) != 0) {
        fail("a fork's child did not have its parent's registrations");
    }
    report_held();

    if (membarrier(1 << 10, 0, 0) != -EINVAL || membarrier(-1, 0, 0) != -EINVAL ||
        membarrier(3, 0, 0) != -EINVAL || membarrier((long)(1UL << 31), 0, 0) != -EINVAL) {
        fail("command 1 << 10, -1, 3 or 1 << 31 was not refused with EINVAL");
    }
    if (membarrier(MEMBARRIER_CMD_QUERY, 1, 0) != -EINVAL ||
        membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED, MEMBARRIER_CMD_FLAG_CPU, 0) != -EINVAL ||
        membarrier(MEMBARRIER_CMD_GET_REGISTRATIONS, 1, 0) != -EINVAL ||
        membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ, 2, 0) != -EINVAL ||
        membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ, 3, 0) != -EINVAL) {
        fail("flags other than MEMBARRIER_CMD_FLAG_CPU on the rseq barrier, or any on another "
             "command, were not refused with EINVAL");
    }
    report_held();
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/** The probe: the checks in order. */
void probe_main(unsigned long *sp)
{
    unsigned int i;

    report_begin(report_case((char **)(sp + 1) + sp[0] + 1, "guest.default.membarrierprobe"));
    for (i = 0; i < sizeof(assertion_id) / sizeof(assertion_id[0]); i++) {
        report_plan(assertion_id[i]);
    }
    run_unregistered();
    run_register();
    run_barriers();
    run_einval();
    report_end();
    say("MEMBARRIERPROBE OK\n");
    sys_exit(0);
}
