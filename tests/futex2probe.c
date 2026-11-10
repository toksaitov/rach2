/**
 * futex2probe: futex_wake(2), futex_wait(2) and futex_requeue(2), Linux
 * 6.7's futex2 calls, and futex(2)'s FUTEX_REQUEUE, FUTEX_CMP_REQUEUE and
 * FUTEX_WAKE_OP, on i386 and x86-64.
 *
 * A thread of the probe's own waits with FUTEX_WAIT and must be woken by a
 * futex_wake; another waits with futex_wait and must be woken by a
 * FUTEX_WAKE of 0, which wakes one as on Linux. A futex_wait whose mask is
 * 1 must stay asleep through a wake of mask 2 and wake at one of mask 1.
 * futex_wait must fail with EAGAIN on a word that differs, and end with
 * ETIMEDOUT at deadlines on CLOCK_MONOTONIC and CLOCK_REALTIME no sooner
 * than the clock reaches them. Two waiters moved by FUTEX_REQUEUE must not
 * be woken by a wake of their first word; futex_requeue must wake one and
 * move the other back, FUTEX_CMP_REQUEUE move that one again, and a wake of
 * the last word end its wait. FUTEX_WAKE_OP must change a word by each of
 * its operations and wake the second word's waiter only when its
 * comparison holds. The arguments Linux 6.12 refuses must be refused with
 * its errors: EINVAL for the sizes, FUTEX2_NUMA, an unknown flag, a mask of
 * 0, a value or mask past 32 bits, an unaligned word, a bad deadline or
 * clock, futex_requeue's flags, NULL array or negative count; EAGAIN for a
 * requeue whose word differs; ENOSYS for an unknown operation; EFAULT for a
 * word the process cannot reach; and 0 for a futex_wake of 0.
 *
 * The probe writes etude's report records (probe_report.h) for the case
 * ETUDE_CASE names, an assertion for each check, as `assertion_id` lists
 * them; a failed check fails its assertion and leaves those after it not
 * run. Prints "FUTEX2PROBE OK" and exits 0, or "FUTEX2PROBE FAIL: " and the
 * reason and exits 1.
 */

#include "probe.h"
#include "probe_report.h"

#define SYS_FUTEX_WAKE    454
#define SYS_FUTEX_WAIT    455
#define SYS_FUTEX_REQUEUE 456

#define FUTEX_WAIT         0
#define FUTEX_WAKE         1
#define FUTEX_REQUEUE      3
#define FUTEX_CMP_REQUEUE  4
#define FUTEX_WAKE_OP      5
#define FUTEX_PRIVATE_FLAG 128

#define FUTEX_OP_SET         0
#define FUTEX_OP_ADD         1
#define FUTEX_OP_OR          2
#define FUTEX_OP_ANDN        3
#define FUTEX_OP_XOR         4
#define FUTEX_OP_OPARG_SHIFT 8
#define FUTEX_OP_CMP_EQ      0
#define FUTEX_OP_CMP_NE      1

/* FUTEX_WAKE_OP's third word: the operation, the comparison, and their
 * signed 12-bit arguments. */
#define FUTEX_OP(op, oparg, cmp, cmparg)                                               \
    ((long)((((unsigned long)(op) & 15) << 28) | (((unsigned long)(cmp) & 15) << 24) | \
            (((unsigned long)(oparg) & 0xfff) << 12) | ((unsigned long)(cmparg) & 0xfff)))

#define FUTEX2_SIZE_U8  0x00
#define FUTEX2_SIZE_U16 0x01
#define FUTEX2_SIZE_U32 0x02
#define FUTEX2_SIZE_U64 0x03
#define FUTEX2_NUMA     0x04
#define FUTEX2_PRIVATE  0x80
#define F2              (FUTEX2_SIZE_U32 | FUTEX2_PRIVATE)
#define ALL             0xffffffffUL

#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

#define EAGAIN    11
#define EFAULT    14
#define EINVAL    22
#define ENOSYS    38
#define ETIMEDOUT 110

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
static const char *const assertion_id[] = {"wake-classic", "wait-futex2", "mask",        "eagain",
                                           "timeout",      "requeue",     "cmp-requeue", "wake-op",
                                           "wake-op-wake", "einval",      "efault",      "zero"};

/** Print "FUTEX2PROBE FAIL: " and `why`, end the report at the assertion
 * that `why` fails, and exit 1. */
static void fail(const char *why)
{
    say("FUTEX2PROBE FAIL: ");
    say(why);
    say("\n");
    report_stop(why);
    syscall1(SYS_EXIT_GROUP, 1);
    __builtin_unreachable();
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

/** futex_wake(2); the count woken, or a negated errno. */
static long futex_wake(volatile unsigned int *word, unsigned long mask, long nr, long flags)
{
    return syscall4(SYS_FUTEX_WAKE, (long)word, (long)mask, nr, flags);
}

/** futex_wait(2); 0, or a negated errno. */
static long futex_wait(volatile unsigned int *word, unsigned long val, unsigned long mask,
                       long flags, struct timespec64 *timeout, long clockid)
{
    return syscall6(SYS_FUTEX_WAIT, (long)word, (long)val, (long)mask, flags, (long)timeout,
                    clockid);
}

/** futex(2) with its six arguments; the fourth is a count for the
 * operations here. */
static long futex(volatile unsigned int *word, long op, long val, long val2,
                  volatile unsigned int *word2, long val3)
{
    return syscall6(SYS_FUTEX, (long)word, op | FUTEX_PRIVATE_FLAG, val, val2, (long)word2, val3);
}

/** futex_requeue(2) from the first of `w` to the second. */
static long futex_requeue(struct futex_waitv *w, long flags, long nr_wake, long nr_requeue)
{
    return syscall4(SYS_FUTEX_REQUEUE, (long)w, flags, nr_wake, nr_requeue);
}

/** Fill `w` for the word `word`, expecting `val`, with `flags`. */
static void entry(struct futex_waitv *w, volatile unsigned int *word, unsigned int val,
                  unsigned int flags)
{
    w->val = val;
    w->uaddr = (unsigned long)word;
    w->flags = flags;
    w->reserved = 0;
}

/** Yield the processor. */
static void yield(void)
{
    syscall0(SYS_SCHED_YIELD);
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

/* The words the threads wait on, how many threads have ended their wait,
 * the first failed result of a wait, and the threads' stacks, one for each
 * thread the probe starts, since a thread that has counted its wait as
 * ended still runs on its stack until it exits. */
#define THREADS 6
static volatile unsigned int word_a, word_b, word_x, word_y;
static volatile int done;
static volatile long bad;
static unsigned char stacks[THREADS][16384] __attribute__((aligned(16)));
static int stacks_used;

/** Count a thread's wait as ended, keeping a result that was not 0. */
static void ended(long r)
{
    if (r != 0) {
        bad = r;
    }
    __sync_fetch_and_add(&done, 1);
}

/** A thread that waits on word_a with FUTEX_WAIT. */
static void wait_classic(void)
{
    ended(futex(&word_a, FUTEX_WAIT, 0, 0, 0, 0));
}

/** A thread that waits on word_a with futex_wait, any mask. */
static void wait_futex2(void)
{
    ended(futex_wait(&word_a, 0, ALL, F2, 0, 0));
}

/** A thread that waits on word_a with futex_wait, mask 1. */
static void wait_mask1(void)
{
    ended(futex_wait(&word_a, 0, 1, F2, 0, 0));
}

/** A thread that waits on word_y with FUTEX_WAIT. */
static void wait_y(void)
{
    ended(futex(&word_y, FUTEX_WAIT, 0, 0, 0, 0));
}

/** Start a thread that runs `fn`, on a stack of its own. */
static void spawn(void (*fn)(void))
{
    if (stacks_used == THREADS || thread_spawn(fn, stacks[stacks_used] + sizeof(stacks[0])) <= 0) {
        fail("clone of a thread");
    }
    stacks_used++;
}

/** Wait until `n` threads have ended their waits, each with 0. */
static void join(int n, const char *why)
{
    while (done < n) {
        if (bad != 0) {
            fail(why);
        }
        yield();
    }
    if (bad != 0) {
        fail(why);
    }
}

/** The wakes of one waiter across the two interfaces and by mask. */
static void run_wakes(void)
{
    long r;

    done = 0;
    spawn(wait_classic);
    while ((r = futex_wake(&word_a, ALL, 1, F2)) == 0) {
        if (done != 0 || bad != 0) {
            fail("the classic waiter ended before its wake");
        }
        yield();
    }
    if (r != 1) {
        fail("futex_wake of a FUTEX_WAIT waiter did not return 1");
    }
    join(1, "a FUTEX_WAIT woken by futex_wake did not return 0");
    report_held();

    done = 0;
    spawn(wait_futex2);
    while ((r = futex(&word_a, FUTEX_WAKE, 0, 0, 0, 0)) == 0) {
        if (done != 0 || bad != 0) {
            fail("the futex2 waiter ended before its wake");
        }
        yield();
    }
    if (r != 1) {
        fail("FUTEX_WAKE of 0 did not wake the one futex_wait waiter");
    }
    join(1, "a futex_wait woken by FUTEX_WAKE did not return 0");
    report_held();

    done = 0;
    spawn(wait_mask1);
    for (;;) {
        if (futex_wake(&word_a, 2, 1, F2) != 0) {
            fail("a futex_wake of mask 2 woke a futex_wait of mask 1");
        }
        r = futex_wake(&word_a, 1, 1, F2);
        if (r != 0) {
            break;
        }
        if (done != 0 || bad != 0) {
            fail("the waiter ended before its wake");
        }
        yield();
    }
    if (r != 1) {
        fail("a futex_wake of mask 1 did not wake the futex_wait of mask 1");
    }
    join(1, "a futex_wait of mask 1 did not return 0 when woken");
    report_held();
}

/** futex_wait's EAGAIN and its deadlines. */
static void run_wait_alone(void)
{
    struct timespec64 at;

    word_a = 5;
    if (futex_wait(&word_a, 4, ALL, F2, 0, 0) != -EAGAIN ||
        futex_wait(&word_a, 4, ALL, FUTEX2_SIZE_U32, 0, 0) != -EAGAIN) {
        fail("a futex_wait on a word that did not hold its value did not fail with EAGAIN");
    }
    word_a = 0;
    report_held();

    deadline(CLOCK_MONOTONIC, &at);
    if (futex_wait(&word_a, 0, ALL, F2, &at, CLOCK_MONOTONIC) != -ETIMEDOUT ||
        !reached(CLOCK_MONOTONIC, &at)) {
        fail("a futex_wait deadline on CLOCK_MONOTONIC did not end with ETIMEDOUT once reached");
    }
    deadline(CLOCK_REALTIME, &at);
    if (futex_wait(&word_a, 0, ALL, FUTEX2_SIZE_U32, &at, CLOCK_REALTIME) != -ETIMEDOUT ||
        !reached(CLOCK_REALTIME, &at)) {
        fail("a futex_wait deadline on CLOCK_REALTIME did not end with ETIMEDOUT once reached");
    }
    at.tv_sec = 0;
    at.tv_nsec = 0;
    if (futex_wait(&word_a, 0, ALL, F2, &at, CLOCK_MONOTONIC) != -ETIMEDOUT) {
        fail("a futex_wait deadline that had passed did not end with ETIMEDOUT");
    }
    report_held();
}

/** Move two waiters with FUTEX_REQUEUE, futex_requeue and
 * FUTEX_CMP_REQUEUE, and wake them on the word they were moved to. */
static void run_requeue(void)
{
    struct futex_waitv w[2];
    long moved = 0, r;

    done = 0;
    spawn(wait_classic);
    spawn(wait_classic);
    while (moved < 2) {
        r = futex(&word_a, FUTEX_REQUEUE, 0, 2, &word_b, 0);
        if (r < 0 || moved + r > 2) {
            fail("FUTEX_REQUEUE of two waiters did not move them");
        }
        moved += r;
        if (done != 0 || bad != 0) {
            fail("a waiter ended before its requeue");
        }
        yield();
    }
    if (futex_wake(&word_a, ALL, 10, F2) != 0 || done != 0) {
        fail("a wake of the first word woke waiters FUTEX_REQUEUE had moved");
    }
    entry(&w[0], &word_b, 0, F2);
    entry(&w[1], &word_a, 0, F2);
    if (futex_requeue(w, 0, 1, 1) != 2) {
        fail("futex_requeue of two waiters, waking one and moving one, did not return 2");
    }
    join(1, "a waiter futex_requeue woke did not return 0");
    if (futex_wake(&word_b, ALL, 10, F2) != 0 || done != 1) {
        fail("futex_requeue left a waiter on the word it moved from");
    }
    report_held();

    if (futex(&word_a, FUTEX_CMP_REQUEUE, 0, 1, &word_b, 1) != -EAGAIN) {
        fail("FUTEX_CMP_REQUEUE whose word did not hold val3 did not fail with EAGAIN");
    }
    entry(&w[0], &word_a, 1, F2);
    entry(&w[1], &word_b, 0, F2);
    if (futex_requeue(w, 0, 0, 1) != -EAGAIN) {
        fail("futex_requeue whose first word did not hold its value did not fail with EAGAIN");
    }
    if (futex(&word_a, FUTEX_CMP_REQUEUE, 0, 1, &word_b, 0) != 1) {
        fail("FUTEX_CMP_REQUEUE did not move the waiter futex_requeue had moved");
    }
    if (futex(&word_b, FUTEX_WAKE, 1, 0, 0, 0) != 1) {
        fail("a wake of the word FUTEX_CMP_REQUEUE moved to did not wake its waiter");
    }
    join(2, "a waiter FUTEX_CMP_REQUEUE moved did not return 0 when woken");
    report_held();
}

/** Run FUTEX_WAKE_OP on word_y with `op`, expecting it to leave `want`. */
static void wake_op(long op, unsigned int want, const char *why)
{
    if (futex(&word_x, FUTEX_WAKE_OP, 1, 1, &word_y, op) != 0 || word_y != want) {
        fail(why);
    }
}

/** FUTEX_WAKE_OP's operations, and its wake of the second word's waiter. */
static void run_wake_op(void)
{
    long r;

    word_y = 5;
    wake_op(FUTEX_OP(FUTEX_OP_SET, 7, FUTEX_OP_CMP_EQ, 5), 7, "FUTEX_OP_SET did not set 7");
    wake_op(FUTEX_OP(FUTEX_OP_ADD, 3, FUTEX_OP_CMP_EQ, 0), 10, "FUTEX_OP_ADD did not add 3");
    wake_op(FUTEX_OP(FUTEX_OP_ADD, -1, FUTEX_OP_CMP_EQ, 0), 9,
            "FUTEX_OP_ADD of -1 did not take its argument as signed");
    wake_op(FUTEX_OP(FUTEX_OP_OR, 0x10, FUTEX_OP_CMP_EQ, 0), 0x19, "FUTEX_OP_OR did not set 0x10");
    wake_op(FUTEX_OP(FUTEX_OP_ANDN, 0x8, FUTEX_OP_CMP_EQ, 0), 0x11,
            "FUTEX_OP_ANDN did not clear 0x8");
    wake_op(FUTEX_OP(FUTEX_OP_XOR, 0xff, FUTEX_OP_CMP_EQ, 0), 0xee,
            "FUTEX_OP_XOR did not flip 0xff");
    wake_op(FUTEX_OP(FUTEX_OP_SET | FUTEX_OP_OPARG_SHIFT, 4, FUTEX_OP_CMP_EQ, 0), 16,
            "FUTEX_OP_OPARG_SHIFT did not set 1 << 4");
    if (futex(&word_x, FUTEX_WAKE_OP, 1, 1, &word_y, FUTEX_OP(5, 1, 0, 0)) != -ENOSYS ||
        word_y != 16) {
        fail("FUTEX_WAKE_OP of operation 5 did not fail with ENOSYS and leave the word");
    }
    if (futex(&word_x, FUTEX_WAKE_OP, 1, 1, &word_y, FUTEX_OP(FUTEX_OP_SET, 3, 6, 0)) != -ENOSYS ||
        word_y != 3) {
        fail("FUTEX_WAKE_OP of comparison 6 did not fail with ENOSYS after setting the word");
    }
    report_held();

    word_y = 0;
    done = 0;
    spawn(wait_y);
    for (;;) {
        if (futex(&word_x, FUTEX_WAKE_OP, 1, 1, &word_y,
                  FUTEX_OP(FUTEX_OP_ADD, 0, FUTEX_OP_CMP_NE, 0)) != 0) {
            fail("FUTEX_WAKE_OP whose comparison failed woke the second word's waiter");
        }
        r = futex(&word_x, FUTEX_WAKE_OP, 1, 1, &word_y,
                  FUTEX_OP(FUTEX_OP_ADD, 0, FUTEX_OP_CMP_EQ, 0));
        if (r != 0) {
            break;
        }
        if (done != 0 || bad != 0) {
            fail("the waiter ended before its wake");
        }
        yield();
    }
    if (r != 1) {
        fail("FUTEX_WAKE_OP whose comparison held did not wake the second word's waiter");
    }
    join(1, "the waiter FUTEX_WAKE_OP woke did not return 0");
    report_held();
}

/** The flags, masks, values, deadlines and counts Linux 6.12 refuses. */
static void run_einval(void)
{
    struct futex_waitv w[2];
    struct timespec64 at;
    unsigned int bad_flags[5];
    int i;

    bad_flags[0] = FUTEX2_SIZE_U8;
    bad_flags[1] = FUTEX2_SIZE_U16 | FUTEX2_PRIVATE;
    bad_flags[2] = FUTEX2_SIZE_U64;
    bad_flags[3] = FUTEX2_SIZE_U32 | FUTEX2_NUMA;
    bad_flags[4] = F2 | 0x100;
    for (i = 0; i < 5; i++) {
        entry(&w[0], &word_a, 0, bad_flags[i]);
        entry(&w[1], &word_b, 0, F2);
        if (futex_wake(&word_a, ALL, 1, bad_flags[i]) != -EINVAL ||
            futex_wait(&word_a, 1, ALL, bad_flags[i], 0, 0) != -EINVAL ||
            futex_requeue(w, 0, 1, 1) != -EINVAL) {
            fail(
                "a size other than 32 bits, FUTEX2_NUMA or flag 0x100 was not refused with EINVAL");
        }
    }
    if (futex_wake(&word_a, 0, 1, F2) != -EINVAL ||
        futex_wait(&word_a, 0, 0, F2, 0, 0) != -EINVAL) {
        fail("a mask of 0 was not refused with EINVAL");
    }
#if defined __x86_64__
    if (futex_wake(&word_a, 1UL << 32, 1, F2) != -EINVAL ||
        futex_wait(&word_a, 1UL << 32, ALL, F2, 0, 0) != -EINVAL ||
        futex_wait(&word_a, 0, 1UL << 32, F2, 0, 0) != -EINVAL) {
        fail("a value or mask past 32 bits was not refused with EINVAL");
    }
#endif
    entry(&w[0], &word_a, 0, F2);
    entry(&w[1], &word_b, 0, F2);
    w[1].val = 1ULL << 32;
    if (futex_requeue(w, 0, 1, 1) != -EINVAL) {
        fail("a futex_requeue entry's value past 32 bits was not refused with EINVAL");
    }
    w[1].val = 0;
    w[1].reserved = 1;
    if (futex_requeue(w, 0, 1, 1) != -EINVAL) {
        fail("a futex_requeue entry's reserved word of 1 was not refused with EINVAL");
    }
    w[1].reserved = 0;
    w[1].flags = FUTEX2_SIZE_U32;
    if (futex_requeue(w, 0, 0, 0) != -EINVAL) {
        fail("unequal futex_requeue entry flags were not refused with EINVAL");
    }
    w[0].flags = FUTEX2_SIZE_U32;
    w[1].flags = F2;
    if (futex_requeue(w, 0, 0, 0) != -EINVAL) {
        fail("unequal futex_requeue entry flags in reverse were not refused with EINVAL");
    }
    w[0].flags = F2;
    if (futex_requeue(w, 1, 1, 1) != -EINVAL || futex_requeue(0, 0, 1, 1) != -EINVAL ||
        futex_requeue(w, 0, -1, 1) != -EINVAL || futex_requeue(w, 0, 1, -1) != -EINVAL ||
        futex(&word_a, FUTEX_CMP_REQUEUE, 1, -1, &word_b, 0) != -EINVAL) {
        fail("futex_requeue's flags, NULL array or a negative count was not refused with EINVAL");
    }
    if (futex_wake((volatile unsigned int *)((char *)&word_a + 2), ALL, 1, F2) != -EINVAL ||
        futex_wait((volatile unsigned int *)((char *)&word_a + 2), 0, ALL, F2, 0, 0) != -EINVAL) {
        fail("a word not aligned to four bytes was not refused with EINVAL");
    }
    at.tv_sec = 0;
    at.tv_nsec = 1000000000;
    if (futex_wait(&word_a, 0, ALL, F2, &at, CLOCK_MONOTONIC) != -EINVAL) {
        fail("a deadline of 1000000000 ns was not refused with EINVAL");
    }
    at.tv_sec = -1;
    at.tv_nsec = 0;
    if (futex_wait(&word_a, 0, ALL, F2, &at, CLOCK_MONOTONIC) != -EINVAL) {
        fail("a deadline of -1 s was not refused with EINVAL");
    }
    at.tv_sec = 0;
    if (futex_wait(&word_a, 0, ALL, F2, &at, 2) != -EINVAL) {
        fail("a deadline on clock 2 was not refused with EINVAL");
    }
    report_held();
}

/** The addresses the process cannot reach, and a futex_wake of 0. */
static void run_efault(void)
{
    struct futex_waitv w[2];
    unsigned long unmapped = 8;
    long gone;

    gone = probe_mmap(0, 4096, 0x3, 0x22, -1, 0);
    if (sys_failed(gone) || syscall2(SYS_MUNMAP, gone, 4096) != 0) {
        fail("mmap and munmap of a page");
    }
    if (futex_wait((volatile unsigned int *)gone, 0, ALL, FUTEX2_SIZE_U32, 0, 0) != -EFAULT ||
        futex_wait(&word_a, 0, ALL, F2, (struct timespec64 *)unmapped, CLOCK_MONOTONIC) !=
            -EFAULT ||
        futex_requeue((struct futex_waitv *)unmapped, 0, 1, 1) != -EFAULT) {
        fail("an unmapped word, deadline or array was not refused with EFAULT");
    }
    entry(&w[0], (volatile unsigned int *)gone, 0, F2);
    entry(&w[1], &word_b, 0, F2);
    if (futex_requeue(w, 0, 1, 1) != -EFAULT ||
        futex(&word_x, FUTEX_WAKE_OP, 1, 1, (volatile unsigned int *)gone, 0) != -EFAULT) {
        fail("futex_requeue or FUTEX_WAKE_OP of an unmapped word was not refused with EFAULT");
    }
    report_held();

    if (futex_wake(&word_a, ALL, 0, F2) != 0) {
        fail("a futex_wake of 0 did not return 0");
    }
    report_held();
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/** The probe: the checks in order. */
void probe_main(unsigned long *sp)
{
    unsigned int i;

    report_begin(report_case((char **)(sp + 1) + sp[0] + 1, "guest.default.futex2probe"));
    for (i = 0; i < sizeof(assertion_id) / sizeof(assertion_id[0]); i++) {
        report_plan(assertion_id[i]);
    }
    run_wakes();
    run_wait_alone();
    run_requeue();
    run_wake_op();
    run_einval();
    run_efault();
    report_end();
    say("FUTEX2PROBE OK\n");
    sys_exit(0);
}
