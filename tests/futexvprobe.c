/**
 * futexvprobe: futex_waitv(2), the wait on several futexes at once that Wine
 * makes for Proton's fsync, on i386 and x86-64.
 *
 * A thread of the probe's own waits on 128 private words, and a wake of the
 * last by another thread must end the wait with its index, 127. A child that
 * shares an anonymous page with the probe wakes the second of four words in
 * it by a futex wake without FUTEX_PRIVATE_FLAG, and the probe's wait on
 * them, without FUTEX2_PRIVATE, must return 1. A wait whose second word does
 * not hold its value must fail with EAGAIN at once. Deadlines on
 * CLOCK_MONOTONIC and CLOCK_REALTIME, 20 ms ahead, must end the wait with
 * ETIMEDOUT no sooner than the clock reaches them, and a deadline that has
 * passed must end it at once. The arguments Linux refuses must be refused
 * with its errors: EINVAL for flags, counts, a NULL array, another clock and
 * an entry's size, flags, reserved word, value or alignment, and EFAULT for
 * an array, a deadline or a word the process cannot read.
 *
 * The probe writes etude's report records (probe_report.h) for the case
 * ETUDE_CASE names, an assertion for each check, as `assertion_id` lists
 * them; a failed check fails its assertion and leaves those after it not
 * run. Prints "FUTEXVPROBE OK" and exits 0, or "FUTEXVPROBE FAIL: " and the
 * reason and exits 1.
 */

#include "probe.h"
#include "probe_report.h"

#define SYS_FUTEX_WAITV 449

#define FUTEX_WAKE         1
#define FUTEX_PRIVATE_FLAG 128

#define FUTEX2_SIZE_U8  0x00
#define FUTEX2_SIZE_U32 0x02
#define FUTEX2_SIZE_U64 0x03
#define FUTEX2_NUMA     0x04
#define FUTEX2_PRIVATE  0x80

#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

#define EAGAIN    11
#define EFAULT    14
#define EINVAL    22
#define ETIMEDOUT 110

#define PROT_READ     0x1
#define PROT_WRITE    0x2
#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20

#define PAGE 4096

/* The thread's flags: Linux's CLONE_VM, CLONE_FS, CLONE_FILES,
 * CLONE_SIGHAND, CLONE_THREAD and CLONE_SYSVSEM. */
#define THREAD_FLAGS 0x50f00

/** Linux's struct futex_waitv, the same on i386 and x86-64. */
struct futex_waitv {
    unsigned long long val;
    unsigned long long uaddr;
    unsigned int flags;
    unsigned int reserved;
};

/** Linux's struct __kernel_timespec, of 64-bit fields on both. */
struct timespec64 {
    long long tv_sec;
    long long tv_nsec;
};

/** The timespec clock_gettime fills, of longs. */
struct timespec {
    long tv_sec;
    long tv_nsec;
};

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
    "private-wake",     "shared-wake",    "eagain", "timeout-monotonic",
    "timeout-realtime", "timeout-passed", "einval", "efault"};

/** Print "FUTEXVPROBE FAIL: " and `why`, end the report at the assertion
 * that `why` fails, and exit 1. */
static void fail(const char *why)
{
    say("FUTEXVPROBE FAIL: ");
    say(why);
    say("\n");
    report_stop(why);
    sys_exit(1);
}

/**
 * thread_spawn(fn, stack_top) starts a thread of the probe's process that
 * runs `fn` on the stack ending at `stack_top`, aligned to 16, and ends when
 * `fn` returns; the thread's id, or a negated errno. The function's address
 * goes on the new stack, since the child of clone comes back on it with no
 * other register the parent can set.
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

/** futex_waitv with its five arguments; the index woken, or a negated errno. */
static long futex_waitv(struct futex_waitv *w, long n, long flags, struct timespec64 *timeout,
                        long clockid)
{
    return syscall5(SYS_FUTEX_WAITV, (long)w, n, flags, (long)timeout, clockid);
}

/** A futex wake of up to one waiter on `word`, with `op`'s flag; the count
 * woken, or a negated errno. */
static long futex_wake(volatile unsigned int *word, long op)
{
    return syscall3(SYS_FUTEX, (long)word, FUTEX_WAKE | op, 1);
}

/** Fill `w` with `n` entries for the words `words`, each expecting 0, with
 * `flags`. */
static void entries(struct futex_waitv *w, volatile unsigned int *words, int n, unsigned flags)
{
    int i;

    for (i = 0; i < n; i++) {
        w[i].val = 0;
        w[i].uaddr = (unsigned long)&words[i];
        w[i].flags = flags;
        w[i].reserved = 0;
    }
}

/** The time on `clock` plus 20 ms, as a timespec64, in `*at`. */
static void deadline(long clock, struct timespec64 *at)
{
    struct timespec now;

    if (syscall2(SYS_CLOCK_GETTIME, clock, (long)&now) != 0) {
        fail("clock_gettime");
    }
    at->tv_sec = now.tv_sec;
    at->tv_nsec = now.tv_nsec + 20000000;
    if (at->tv_nsec >= 1000000000) {
        at->tv_nsec -= 1000000000;
        at->tv_sec++;
    }
}

/** Whether the time on `clock` has reached `at`. */
static int reached(long clock, const struct timespec64 *at)
{
    struct timespec now;

    if (syscall2(SYS_CLOCK_GETTIME, clock, (long)&now) != 0) {
        fail("clock_gettime");
    }
    return now.tv_sec > at->tv_sec || (now.tv_sec == at->tv_sec && now.tv_nsec >= at->tv_nsec);
}

/* The private wait's words, its entries, and the waker thread's state. */
static volatile unsigned int private_words[128];
static struct futex_waitv private_w[128];
static volatile int stop, woke, finished;
static unsigned char thread_stack[16384] __attribute__((aligned(16)));

/** The waker thread: wake the last private word until a waiter is woken, or
 * until the waiting thread says to stop. */
static void waker(void)
{
    while (!stop) {
        if (futex_wake(&private_words[127], FUTEX_PRIVATE_FLAG) == 1) {
            woke = 1;
            break;
        }
        syscall0(SYS_SCHED_YIELD);
    }
    finished = 1;
}

/** A thread's wake of one of 128 private words. */
static void run_private(void)
{
    long r;

    entries(private_w, private_words, 128, FUTEX2_SIZE_U32 | FUTEX2_PRIVATE);
    if (thread_spawn(waker, thread_stack + sizeof(thread_stack)) <= 0) {
        fail("clone of a thread");
    }
    r = futex_waitv(private_w, 128, 0, 0, 0);
    stop = 1;
    while (!finished) {
        syscall0(SYS_SCHED_YIELD);
    }
    if (r != 127 || !woke) {
        fail("a wake of the last of 128 private words did not end the wait with index 127");
    }
    report_held();
}

/** A child's wake of the second of four words in a shared page. */
static void run_shared(void)
{
    volatile unsigned int *words;
    struct futex_waitv w[4];
    long page, pid, r;
    int status = 0;

    page = probe_mmap(0, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (sys_failed(page)) {
        fail("mmap of a shared page");
    }
    words = (volatile unsigned int *)page;
    entries(w, words, 4, FUTEX2_SIZE_U32);
    pid = syscall0(SYS_FORK);
    if (pid < 0) {
        fail("fork");
    }
    if (pid == 0) {
        while (!words[8]) {
            if (futex_wake(&words[1], 0) == 1) {
                sys_exit(0);
            }
            syscall0(SYS_SCHED_YIELD);
        }
        sys_exit(2);
    }
    r = futex_waitv(w, 4, 0, 0, 0);
    words[8] = 1;
    if (syscall3(SYS_WAITPID, pid, (long)&status, 0) != pid) {
        fail("the waking child could not be reaped");
    }
    if (r != 1 || status != 0) {
        fail("another process's wake of the second of four shared words did not end the wait "
             "with index 1");
    }
    syscall2(SYS_MUNMAP, page, PAGE);
    report_held();
}

/** The checks that need no second thread: a word that differs, the
 * deadlines, and the refusals. */
static void run_alone(void)
{
    static volatile unsigned int words[4];
    struct futex_waitv w[4];
    struct timespec64 at;
    long gone;

    entries(w, words, 3, FUTEX2_SIZE_U32 | FUTEX2_PRIVATE);
    w[1].val = 7;
    if (futex_waitv(w, 3, 0, 0, 0) != -EAGAIN) {
        fail("a wait whose second word did not hold its value did not fail with EAGAIN");
    }
    report_held();

    entries(w, words, 2, FUTEX2_SIZE_U32 | FUTEX2_PRIVATE);
    deadline(CLOCK_MONOTONIC, &at);
    if (futex_waitv(w, 2, 0, &at, CLOCK_MONOTONIC) != -ETIMEDOUT ||
        !reached(CLOCK_MONOTONIC, &at)) {
        fail("a deadline on CLOCK_MONOTONIC did not end the wait with ETIMEDOUT once reached");
    }
    report_held();
    deadline(CLOCK_REALTIME, &at);
    if (futex_waitv(w, 2, 0, &at, CLOCK_REALTIME) != -ETIMEDOUT || !reached(CLOCK_REALTIME, &at)) {
        fail("a deadline on CLOCK_REALTIME did not end the wait with ETIMEDOUT once reached");
    }
    report_held();
    at.tv_sec = 0;
    at.tv_nsec = 0;
    if (futex_waitv(w, 2, 0, &at, CLOCK_MONOTONIC) != -ETIMEDOUT ||
        futex_waitv(w, 2, 0, &at, CLOCK_REALTIME) != -ETIMEDOUT) {
        fail("a deadline that had passed did not end the wait with ETIMEDOUT");
    }
    report_held();

    if (futex_waitv(w, 2, 1, 0, 0) != -EINVAL || futex_waitv(w, 0, 0, 0, 0) != -EINVAL ||
        futex_waitv(private_w, 129, 0, 0, 0) != -EINVAL || futex_waitv(0, 1, 0, 0, 0) != -EINVAL ||
        futex_waitv(w, 2, 0, &at, 2) != -EINVAL) {
        fail("flags, a count of 0 or 129, a NULL array or clock 2 was not refused with EINVAL");
    }
    w[1].flags = FUTEX2_SIZE_U64 | FUTEX2_PRIVATE;
    if (futex_waitv(w, 2, 0, 0, 0) != -EINVAL) {
        fail("a 64-bit futex was not refused with EINVAL");
    }
    w[1].flags = FUTEX2_SIZE_U8 | FUTEX2_PRIVATE;
    if (futex_waitv(w, 2, 0, 0, 0) != -EINVAL) {
        fail("an 8-bit futex was not refused with EINVAL");
    }
    w[1].flags = FUTEX2_SIZE_U32 | FUTEX2_NUMA;
    if (futex_waitv(w, 2, 0, 0, 0) != -EINVAL) {
        fail("FUTEX2_NUMA was not refused with EINVAL");
    }
    w[1].flags = FUTEX2_SIZE_U32;
    w[1].reserved = 1;
    if (futex_waitv(w, 2, 0, 0, 0) != -EINVAL) {
        fail("a reserved word of 1 was not refused with EINVAL");
    }
    w[1].reserved = 0;
    w[1].val = 1ULL << 32;
    if (futex_waitv(w, 2, 0, 0, 0) != -EINVAL) {
        fail("a value past 32 bits was not refused with EINVAL");
    }
    w[1].val = 0;
    w[1].uaddr += 2;
    if (futex_waitv(w, 2, 0, 0, 0) != -EINVAL) {
        fail("a word not aligned to four bytes was not refused with EINVAL");
    }
    report_held();

    entries(w, words, 2, FUTEX2_SIZE_U32);
    gone = probe_mmap(0, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (sys_failed(gone) || syscall2(SYS_MUNMAP, gone, PAGE) != 0) {
        fail("mmap and munmap of a page");
    }
    if (futex_waitv((struct futex_waitv *)8, 1, 0, 0, 0) != -EFAULT ||
        futex_waitv(w, 2, 0, (struct timespec64 *)8, CLOCK_MONOTONIC) != -EFAULT) {
        fail("an array or a deadline at address 8 was not refused with EFAULT");
    }
    w[1].uaddr = (unsigned long)gone;
    if (futex_waitv(w, 2, 0, 0, 0) != -EFAULT) {
        fail("a word in an unmapped page was not refused with EFAULT");
    }
    report_held();
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/** The probe: the checks in order. */
void probe_main(unsigned long *sp)
{
    unsigned int i;

    report_begin(report_case((char **)(sp + 1) + sp[0] + 1, "guest.default.futexvprobe"));
    for (i = 0; i < sizeof(assertion_id) / sizeof(assertion_id[0]); i++) {
        report_plan(assertion_id[i]);
    }
    run_private();
    run_shared();
    run_alone();
    report_end();
    say("FUTEXVPROBE OK\n");
    sys_exit(0);
}
