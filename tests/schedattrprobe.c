/**
 * schedattrprobe: sched_setattr(2) and sched_getattr(2), the calls that set
 * and read scheduling policies on i386 and x86-64, with Linux 6.12's
 * refusals and EBUSY when no deadline bandwidth can be admitted.
 *
 * The probe, which the default suite runs as root, sets each policy with
 * sched_setattr and reads it back with sched_getattr, sched_getscheduler
 * and sched_getparam: SCHED_OTHER and SCHED_BATCH with a nice value,
 * SCHED_IDLE, SCHED_FIFO and SCHED_RR with a priority, and
 * SCHED_FLAG_RESET_ON_FORK beside one. An ordinary thread must read back
 * Linux's default slice for the processors started, or the one it asked
 * for, clamped to 0.1 to 100
 * ms. The structure's size rules must hold: E2BIG for a size below 48, past
 * 4096, or larger than 56 with a byte past 56 that is not 0, with 56
 * written back; and sched_getattr must write as many bytes as its size
 * holds, at most 56. SCHED_FLAG_KEEP_POLICY and SCHED_FLAG_KEEP_PARAMS must
 * keep what they name. Valid deadline requests must be refused with EBUSY,
 * as Linux's sched_dl_overflow refuses unavailable bandwidth, without
 * changing the caller's policy. On more than one processor a thread bound
 * to one must first get EPERM. A fork must keep the parent's policy, or
 * reset it with SCHED_RESET_ON_FORK.
 * sched_rr_get_interval must give 0 for SCHED_FIFO. A
 * child that has dropped root must be refused with EPERM what Linux refuses
 * without CAP_SYS_NICE. RLIMIT_NICE and RLIMIT_RTPRIO must read 0, and a
 * child that has raised them before dropping root must be given the nice
 * values and real-time priorities they allow, and no more. The other
 * refusals are EINVAL, EOPNOTSUPP for the utilization clamps, as from a
 * kernel built without them, and ESRCH.
 *
 * The probe writes etude's report records (probe_report.h) for the case
 * ETUDE_CASE names, an assertion for each check, as `assertion_id` lists
 * them; a failed check fails its assertion and leaves those after it not
 * run. Prints "SCHEDATTRPROBE OK" and exits 0, or "SCHEDATTRPROBE FAIL: "
 * and the reason and exits 1.
 */

#include "probe.h"
#include "probe_args.h"
#include "probe_report.h"

#if defined __x86_64__
#define SYS_SCHED_SETPARAM        142
#define SYS_SCHED_GETPARAM        143
#define SYS_SCHED_SETSCHEDULER    144
#define SYS_SCHED_GETSCHEDULER    145
#define SYS_SCHED_RR_GET_INTERVAL 148
#define SYS_SCHED_GETAFFINITY     204
#define SYS_SCHED_SETATTR         314
#define SYS_SCHED_GETATTR         315
#define SYS_SETPRIORITY           141
#define SYS_PRLIMIT64             302
#else
#define SYS_SCHED_SETPARAM        154
#define SYS_SCHED_GETPARAM        155
#define SYS_SCHED_SETSCHEDULER    156
#define SYS_SCHED_GETSCHEDULER    157
#define SYS_SCHED_RR_GET_INTERVAL 161
#define SYS_SCHED_GETAFFINITY     242
#define SYS_SCHED_SETATTR         351
#define SYS_SCHED_GETATTR         352
#define SYS_SETPRIORITY           97
#define SYS_PRLIMIT64             340
#endif

#define RLIMIT_NICE   13
#define RLIMIT_RTPRIO 14

#define SCHED_OTHER         0
#define SCHED_FIFO          1
#define SCHED_RR            2
#define SCHED_BATCH         3
#define SCHED_IDLE          5
#define SCHED_DEADLINE      6
#define SCHED_RESET_ON_FORK 0x40000000

#define SCHED_FLAG_RESET_ON_FORK  0x01
#define SCHED_FLAG_RECLAIM        0x02
#define SCHED_FLAG_DL_OVERRUN     0x04
#define SCHED_FLAG_KEEP_POLICY    0x08
#define SCHED_FLAG_KEEP_PARAMS    0x10
#define SCHED_FLAG_UTIL_CLAMP_MIN 0x20

#define E2BIG      7
#define EAGAIN     11
#define EINTR      4
#define EPERM      1
#define EACCES     13
#define ESRCH      3
#define EBUSY      16
#define EINVAL     22
#define EOPNOTSUPP 95

#define SIGKILL 9
#define MS      1000000ULL

/** Linux's struct sched_attr, the same on i386 and x86-64. */
struct sched_attr {
    unsigned int size;
    unsigned int sched_policy;
    unsigned long long sched_flags;
    int sched_nice;
    unsigned int sched_priority;
    unsigned long long sched_runtime;
    unsigned long long sched_deadline;
    unsigned long long sched_period;
    unsigned int sched_util_min;
    unsigned int sched_util_max;
};

/** The limit prlimit64 reads and writes, of 64-bit fields on both. */
struct rlimit64 {
    unsigned long long cur;
    unsigned long long max;
};

/** The timespec sched_rr_get_interval fills, of longs. */
struct timespec {
    long tv_sec;
    long tv_nsec;
};

/** End the calling process with `status`; it does not return. */
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

/* The one child alive, for failure cleanup; 0 in the child and after reap. */
static long child_pid;

/* The assertions, a check each, in the order the probe makes them. */
static const char *const assertion_id[] = {
    "size",         "einval",    "eopnotsupp", "esrch", "policies",    "slice", "keep",
    "getattr-size", "admission", "affinity",   "fork",  "rr-interval", "eperm", "rlimits"};

/** Print "SCHEDATTRPROBE FAIL: " and `why`, end the report at the
 * assertion that `why` fails, and exit 1. */
static void fail(const char *why)
{
    say("SCHEDATTRPROBE FAIL: ");
    say(why);
    say("\n");
    report_stop(why);
    if (child_pid > 0) {
        int status;

        syscall2(SYS_KILL, child_pid, 9);
        while (syscall3(SYS_WAITPID, child_pid, (long)&status, 0) == -EINTR) {
        }
    }
    sys_exit(1);
}

/** Fill `a` with the byte 0x63, as no value the kernel writes. */
static void fill(struct sched_attr *a)
{
    unsigned char *b = (unsigned char *)a;
    unsigned int i;

    for (i = 0; i < sizeof(*a); i++) {
        b[i] = 0x63;
    }
}

/** A structure of 56 bytes with the policy, flags, nice value and
 * priority given, and the rest 0. */
static struct sched_attr mk(unsigned int policy, unsigned long long flags, int nice,
                            unsigned int priority)
{
    struct sched_attr a;
    unsigned char *b = (unsigned char *)&a;
    unsigned int i;

    for (i = 0; i < sizeof(a); i++) {
        b[i] = 0;
    }
    a.size = sizeof(a);
    a.sched_policy = policy;
    a.sched_flags = flags;
    a.sched_nice = nice;
    a.sched_priority = priority;
    return a;
}

/** A structure of 56 bytes for SCHED_DEADLINE with the flags, runtime,
 * deadline and period given. */
static struct sched_attr mkdl(unsigned long long flags, unsigned long long runtime,
                              unsigned long long deadline, unsigned long long period)
{
    struct sched_attr a = mk(SCHED_DEADLINE, flags, 0, 0);

    a.sched_runtime = runtime;
    a.sched_deadline = deadline;
    a.sched_period = period;
    return a;
}

/** `a` with the runtime, an ordinary thread's slice, `runtime`. */
static struct sched_attr with_runtime(struct sched_attr a, unsigned long long runtime)
{
    a.sched_runtime = runtime;
    return a;
}

/** sched_setattr(pid, a, flags); 0, or a negated errno. */
static long setattr(long pid, void *a, long flags)
{
    return syscall3(SYS_SCHED_SETATTR, pid, (long)a, flags);
}

/** sched_getattr(pid, a, size, flags); 0, or a negated errno. */
static long getattr(long pid, void *a, long size, long flags)
{
    return syscall4(SYS_SCHED_GETATTR, pid, (long)a, size, flags);
}

/** sched_setattr of `a` on `pid`; 0, or a negated errno. */
static long set(long pid, struct sched_attr a)
{
    return setattr(pid, &a, 0);
}

/** Return the caller to SCHED_OTHER at nice 0 with the default slice. */
static void ordinary(void)
{
    if (set(0, mk(SCHED_OTHER, 0, 0, 0)) != 0) {
        fail("sched_setattr could not return the probe to SCHED_OTHER");
    }
}

/** The caller's priority by sched_getparam. */
static long getparam(void)
{
    int p = -1;

    if (syscall2(SYS_SCHED_GETPARAM, 0, (long)&p) != 0) {
        fail("sched_getparam");
    }
    return p;
}

/* The processors started, from sched_getaffinity, and Linux's default
 * slice for them: 0.75 ms times one more than the base-2 logarithm of
 * their number, eight at most. */
static int cpus;
static unsigned long long base_slice;

/** Count the processors started and work out the default slice. */
static void count_cpus(void)
{
    unsigned long long mask = 0;
    int n, factor = 1;

    if (syscall3(SYS_SCHED_GETAFFINITY, 0, sizeof(mask), (long)&mask) <= 0) {
        fail("sched_getaffinity");
    }
    for (cpus = 0; mask; mask &= mask - 1) {
        cpus++;
    }
    for (n = cpus < 8 ? cpus : 8; n > 1; n >>= 1) {
        factor++;
    }
    base_slice = 750000ULL * (unsigned long long)factor;
}

/** The size rules of the structure sched_setattr reads. */
static void run_size(void)
{
    static unsigned long long buf[16];
    struct sched_attr *a = (struct sched_attr *)buf;
    unsigned int sizes[3];
    int i;

    sizes[0] = 47;
    sizes[1] = 4097;
    sizes[2] = 64;
    for (i = 0; i < 3; i++) {
        *a = mk(SCHED_OTHER, 0, 0, 0);
        ((unsigned char *)buf)[60] = 1;
        a->size = sizes[i];
        if (setattr(0, a, 0) != -E2BIG || a->size != 56) {
            fail("a size of 47 or 4097, or 64 with byte 60 not 0, did not fail with E2BIG and "
                 "write 56 back");
        }
    }
    *a = mk(SCHED_OTHER, 0, 0, 0);
    ((unsigned char *)buf)[60] = 0;
    a->size = 64;
    if (setattr(0, a, 0) != 0) {
        fail("a size of 64 whose bytes past 56 are 0 was refused");
    }
    a->size = 0;
    if (setattr(0, a, 0) != 0) {
        fail("a size of 0, which Linux takes as 48, was refused");
    }
    a->size = 48;
    if (setattr(0, a, 0) != 0) {
        fail("a size of 48 was refused");
    }
    report_held();
}

/** The arguments sched_setattr refuses with EINVAL. */
static void run_einval(void)
{
    struct sched_attr a;

    a = mk(SCHED_OTHER, 0, 0, 0);
    if (setattr(0, 0, 0) != -EINVAL || setattr(0, &a, 1) != -EINVAL ||
        setattr(-1, &a, 0) != -EINVAL) {
        fail("sched_setattr of no structure, flags 1 or pid -1 was not refused with EINVAL");
    }
    if (set(0, mk(SCHED_OTHER, 0x80, 0, 0)) != -EINVAL ||
        set(0, mk(SCHED_OTHER, 1ULL << 32, 0, 0)) != -EINVAL || set(0, mk(7, 0, 0, 0)) != -EINVAL ||
        set(0, mk(8, 0, 0, 0)) != -EINVAL || set(0, mk(0x80000000u, 0, 0, 0)) != -EINVAL) {
        fail("flag 0x80 or 1 << 32, or policy 7, 8 or 0x80000000, was not refused with EINVAL");
    }
    if (set(0, mk(SCHED_FIFO, 0, 0, 0)) != -EINVAL || set(0, mk(SCHED_OTHER, 0, 0, 1)) != -EINVAL ||
        set(0, mk(SCHED_FIFO, 0, 0, 100)) != -EINVAL) {
        fail("SCHED_FIFO at 0 or 100, or SCHED_OTHER at 1, was not refused with EINVAL");
    }
    if (set(0, mkdl(0, 1000, 30 * MS, 100 * MS)) != -EINVAL ||
        set(0, mkdl(0, 10 * MS, 200 * MS, 100 * MS)) != -EINVAL ||
        set(0, mkdl(0, 10000, 50000, 50000)) != -EINVAL ||
        set(0, mkdl(0, 10 * MS, 0, 100 * MS)) != -EINVAL ||
        set(0, mkdl(0, 40 * MS, 30 * MS, 100 * MS)) != -EINVAL ||
        set(0, mkdl(0, 10 * MS, 30 * MS, 5000000 * MS)) != -EINVAL) {
        fail("deadline parameters out of Linux's bounds were not refused with EINVAL");
    }
}

/** The structures, sizes and flags sched_getattr refuses with EINVAL, and
 * the refusals of EOPNOTSUPP and ESRCH. */
static void run_refusals(void)
{
    struct sched_attr a;
    int param = 0;

    a = mk(SCHED_OTHER, SCHED_FLAG_UTIL_CLAMP_MIN, 0, 0);
    a.size = 48;
    if (setattr(0, &a, 0) != -EINVAL) {
        fail("a utilization clamp in a structure of 48 bytes was not refused with EINVAL");
    }
    if (syscall3(SYS_SCHED_SETSCHEDULER, 0, SCHED_DEADLINE, (long)&param) != -EINVAL) {
        fail("sched_setscheduler of SCHED_DEADLINE was not refused with EINVAL");
    }
    if (getattr(0, &a, 47, 0) != -EINVAL || getattr(0, &a, 4097, 0) != -EINVAL ||
        getattr(0, &a, 56, 1) != -EINVAL || getattr(0, 0, 56, 0) != -EINVAL ||
        getattr(-1, &a, 56, 0) != -EINVAL) {
        fail("sched_getattr of size 47 or 4097, flags 1, no structure or pid -1 was not refused "
             "with EINVAL");
    }
    report_held();

    if (set(0, mk(SCHED_OTHER, SCHED_FLAG_UTIL_CLAMP_MIN, 0, 0)) != -EOPNOTSUPP ||
        set(0, mk(SCHED_OTHER, SCHED_FLAG_UTIL_CLAMP_MIN << 1, 0, 0)) != -EOPNOTSUPP) {
        fail("the utilization clamps were not refused with EOPNOTSUPP");
    }
    report_held();

    if (set(0x3fffffff, mk(SCHED_OTHER, 0, 0, 0)) != -ESRCH ||
        getattr(0x3fffffff, &a, 56, 0) != -ESRCH) {
        fail("sched_setattr or sched_getattr of no such process did not fail with ESRCH");
    }
    report_held();
}

/** Read the caller's scheduling back and check it against `want`, with
 * the policy sched_getscheduler must give. */
static void check(const struct sched_attr *want, long scheduler, const char *why)
{
    struct sched_attr got;

    fill(&got);
    if (getattr(0, &got, sizeof(got), 0) != 0 || got.size != 56 ||
        got.sched_policy != want->sched_policy || got.sched_flags != want->sched_flags ||
        got.sched_nice != want->sched_nice || got.sched_priority != want->sched_priority ||
        got.sched_runtime != want->sched_runtime || got.sched_deadline != want->sched_deadline ||
        got.sched_period != want->sched_period || got.sched_util_min != 0 ||
        got.sched_util_max != 0) {
        fail(why);
    }
    if (syscall1(SYS_SCHED_GETSCHEDULER, 0) != scheduler ||
        getparam() != (long)want->sched_priority) {
        fail(why);
    }
}

/** Set `in` with sched_setattr and read back `want`, with the policy
 * sched_getscheduler must give. */
static void set_and_check(struct sched_attr in, struct sched_attr want, long scheduler,
                          const char *why)
{
    if (set(0, in) != 0) {
        fail(why);
    }
    check(&want, scheduler, why);
}

/** Each policy set and read back. */
static void run_policies(void)
{
    struct sched_attr dl = mkdl(SCHED_FLAG_RECLAIM | SCHED_FLAG_DL_OVERRUN, 10 * MS, 30 * MS, 0);

    set_and_check(mk(SCHED_OTHER, 0, 5, 0), with_runtime(mk(SCHED_OTHER, 0, 5, 0), base_slice),
                  SCHED_OTHER,
                  "SCHED_OTHER at nice 5 did not read back as set, with the default slice");
    set_and_check(mk(SCHED_BATCH, 0, 3, 0), with_runtime(mk(SCHED_BATCH, 0, 3, 0), base_slice),
                  SCHED_BATCH, "SCHED_BATCH at nice 3 did not read back as set");
    set_and_check(mk(SCHED_IDLE, 0, 0, 0), with_runtime(mk(SCHED_IDLE, 0, 3, 0), base_slice),
                  SCHED_IDLE, "SCHED_IDLE changed the nice value or slice");
    set_and_check(mk(SCHED_FIFO, 0, 0, 10), mk(SCHED_FIFO, 0, 0, 10), SCHED_FIFO,
                  "SCHED_FIFO at 10 did not read back as set");
    set_and_check(mk(SCHED_RR, 0, 0, 20), mk(SCHED_RR, 0, 0, 20), SCHED_RR,
                  "SCHED_RR at 20 did not read back as set");
    if (set(0, mkdl(0, 10 * MS, 30 * MS, 100 * MS)) != -EBUSY || set(0, dl) != -EBUSY) {
        fail("a valid deadline request was not refused with EBUSY");
    }
    {
        struct sched_attr want = mk(SCHED_RR, 0, 0, 20);

        check(&want, SCHED_RR, "a refused deadline request changed the policy");
    }
    set_and_check(mk(SCHED_FIFO, SCHED_FLAG_RESET_ON_FORK, 0, 10),
                  mk(SCHED_FIFO, SCHED_FLAG_RESET_ON_FORK, 0, 10), SCHED_FIFO | SCHED_RESET_ON_FORK,
                  "SCHED_FIFO with SCHED_FLAG_RESET_ON_FORK did not read back as set");
    ordinary();
    report_held();
}

/** An ordinary thread's slice, asked for and clamped, and its nice value
 * clamped. */
static void run_slice(void)
{
    set_and_check(with_runtime(mk(SCHED_OTHER, 0, 0, 0), 2 * MS),
                  with_runtime(mk(SCHED_OTHER, 0, 0, 0), 2 * MS), SCHED_OTHER,
                  "a slice of 2 ms did not read back as asked");
    set_and_check(with_runtime(mk(SCHED_BATCH, 0, 0, 0), 50000),
                  with_runtime(mk(SCHED_BATCH, 0, 0, 0), MS / 10), SCHED_BATCH,
                  "a slice of 50 us did not read back as 0.1 ms");
    set_and_check(with_runtime(mk(SCHED_OTHER, 0, 0, 0), 200 * MS),
                  with_runtime(mk(SCHED_OTHER, 0, 0, 0), 100 * MS), SCHED_OTHER,
                  "a slice of 200 ms did not read back as 100 ms");
    set_and_check(mk(SCHED_OTHER, 0, -30, 0), with_runtime(mk(SCHED_OTHER, 0, -20, 0), base_slice),
                  SCHED_OTHER, "nice -30 did not read back as -20 with the default slice");
    ordinary();
    report_held();
}

/** SCHED_FLAG_KEEP_POLICY and SCHED_FLAG_KEEP_PARAMS, and the bytes
 * sched_getattr writes. */
static void run_keep(void)
{
    static unsigned char buf[64];
    struct sched_attr want, *a = (struct sched_attr *)buf;
    unsigned int i;

    if (set(0, mk(SCHED_FIFO, 0, 0, 10)) != 0 ||
        set(0, mk(SCHED_OTHER, SCHED_FLAG_KEEP_POLICY, 0, 20)) != 0) {
        fail("SCHED_FLAG_KEEP_POLICY with priority 20 on a SCHED_FIFO thread was refused");
    }
    want = mk(SCHED_FIFO, 0, 0, 20);
    check(&want, SCHED_FIFO, "SCHED_FLAG_KEEP_POLICY did not keep SCHED_FIFO and set priority 20");
    if (set(0, mk(SCHED_FIFO, SCHED_FLAG_KEEP_PARAMS | SCHED_FLAG_RESET_ON_FORK, 0, 0)) != 0) {
        fail("SCHED_FLAG_KEEP_PARAMS with priority 0 on a SCHED_FIFO thread was refused");
    }
    want = mk(SCHED_FIFO, SCHED_FLAG_RESET_ON_FORK, 0, 20);
    check(&want, SCHED_FIFO | SCHED_RESET_ON_FORK,
          "SCHED_FLAG_KEEP_PARAMS did not keep priority 20 and set the reset flag alone");
    ordinary();
    report_held();

    for (i = 0; i < sizeof(buf); i++) {
        buf[i] = 0xAA;
    }
    if (getattr(0, buf, 48, 0) != 0 || a->size != 48 || a->sched_policy != SCHED_OTHER ||
        buf[48] != 0xAA || buf[55] != 0xAA) {
        fail("sched_getattr of 48 bytes did not write 48 and give size 48");
    }
    if (getattr(0, buf, 64, 0) != 0 || a->size != 56 || a->sched_util_min != 0 || buf[56] != 0xAA ||
        buf[63] != 0xAA) {
        fail("sched_getattr of 64 bytes did not write 56 and give size 56");
    }
    report_held();
}

/** Linux's sched_dl_overflow returns EBUSY for unavailable bandwidth. */
static void run_admission(void)
{
    struct sched_attr want = with_runtime(mk(SCHED_OTHER, 0, 0, 0), base_slice);

    ordinary();
    if (set(0, mkdl(0, 1024, MS, MS)) != -EBUSY ||
        set(0, mkdl(0, 90 * MS, 100 * MS, 100 * MS)) != -EBUSY ||
        set(0, mkdl(SCHED_FLAG_RESET_ON_FORK, 10 * MS, 30 * MS, 100 * MS)) != -EBUSY) {
        fail("Linux's EBUSY path for unavailable deadline bandwidth was not taken");
    }
    check(&want, SCHED_OTHER, "deadline admission failure changed the caller");
    report_held();
}

/** A restricted affinity is refused before deadline admission. */
static void run_affinity(void)
{
    unsigned long long one = 1, all = 0;

    if (cpus > 1) {
        if (syscall3(SYS_SCHED_GETAFFINITY, 0, sizeof(all), (long)&all) <= 0) {
            fail("sched_getaffinity");
        }
        if (syscall3(SYS_SCHED_SETAFFINITY, 0, sizeof(one), (long)&one) != 0 ||
            set(0, mkdl(0, 10 * MS, 30 * MS, 100 * MS)) != -EPERM ||
            syscall3(SYS_SCHED_SETAFFINITY, 0, sizeof(all), (long)&all) != 0) {
            fail("SCHED_DEADLINE for a thread bound to one processor was not refused with EPERM");
        }
    }
    report_held();
}

/** Fork a child that exits 0 when its policy is `scheduler`, its priority
 * `priority` and its nice value `nice`, and 1 otherwise; the child's wait
 * status, or the negated errno of a failed fork. */
static long forked(long scheduler, unsigned int priority, int nice)
{
    struct sched_attr got;
    int status = 0;
    long pid = syscall0(SYS_FORK);

    if (pid < 0) {
        return pid;
    }
    if (pid == 0) {
        fill(&got);
        sys_exit(syscall1(SYS_SCHED_GETSCHEDULER, 0) == scheduler && getparam() == (long)priority &&
                         getattr(0, &got, sizeof(got), 0) == 0 && got.sched_nice == nice &&
                         got.sched_priority == priority
                     ? 0
                     : 1);
    }
    child_pid = pid;
    if (syscall3(SYS_WAITPID, pid, (long)&status, 0) != pid) {
        fail("a child could not be reaped");
    }
    child_pid = 0;
    return status;
}

/** What a fork's child takes of its parent's scheduling. */
static void run_fork(void)
{
    if (set(0, mk(SCHED_FIFO, 0, 0, 10)) != 0 || forked(SCHED_FIFO, 10, 0) != 0) {
        fail("the child of a SCHED_FIFO parent at 10 did not have SCHED_FIFO at 10");
    }
    if (set(0, mk(SCHED_FIFO, SCHED_FLAG_RESET_ON_FORK, 0, 10)) != 0 ||
        forked(SCHED_OTHER, 0, 0) != 0) {
        fail("the child of a SCHED_FIFO parent with the reset flag was not SCHED_OTHER at 0");
    }
    ordinary();
    if (set(0, mk(SCHED_OTHER, SCHED_FLAG_RESET_ON_FORK, -5, 0)) != 0 ||
        forked(SCHED_OTHER, 0, 0) != 0) {
        fail("the child of a parent at nice -5 with the reset flag was not at nice 0");
    }
    if (set(0, mk(SCHED_BATCH, SCHED_FLAG_RESET_ON_FORK, 4, 0)) != 0 ||
        forked(SCHED_BATCH, 0, 4) != 0) {
        fail("the child of a SCHED_BATCH parent at nice 4 with the reset flag did not keep both");
    }
    ordinary();
    if (set(0, mkdl(SCHED_FLAG_RESET_ON_FORK, 10 * MS, 30 * MS, 100 * MS)) != -EBUSY ||
        forked(SCHED_OTHER, 0, 0) != 0) {
        fail("a refused deadline request changed the policy inherited by a child");
    }
    report_held();
}

/** sched_rr_get_interval for SCHED_FIFO and SCHED_RR. */
static void run_rr_interval(void)
{
    struct timespec ts;

    ts.tv_sec = ts.tv_nsec = 7;
    if (set(0, mk(SCHED_FIFO, 0, 0, 10)) != 0 ||
        syscall2(SYS_SCHED_RR_GET_INTERVAL, 0, (long)&ts) != 0 || ts.tv_sec != 0 ||
        ts.tv_nsec != 0) {
        fail("sched_rr_get_interval of a SCHED_FIFO thread did not give 0");
    }
    ts.tv_sec = ts.tv_nsec = 0;
    if (set(0, mk(SCHED_RR, 0, 0, 10)) != 0 ||
        syscall2(SYS_SCHED_RR_GET_INTERVAL, 0, (long)&ts) != 0 ||
        (ts.tv_sec == 0 && ts.tv_nsec == 0)) {
        fail("sched_rr_get_interval of a SCHED_RR thread gave 0");
    }
    ordinary();
    report_held();
}

/**
 * The checks of a child without root, each step numbered: the exit status
 * is 0, or the step that went wrong.
 */
static int unprivileged(long parent)
{
    if (syscall3(SYS_SETRESUID, probe_id(1000), probe_id(1000), probe_id(1000)) != 0) {
        return 1;
    }
    if (set(0, mk(SCHED_OTHER, 0, 5, 0)) != 0) {
        return 2;
    }
    if (set(0, mk(SCHED_OTHER, 0, 4, 0)) != -EPERM) {
        return 3;
    }
    if (set(0, mk(SCHED_FIFO, 0, 0, 1)) != -EPERM) {
        return 4;
    }
    if (set(0, mkdl(0, 10 * MS, 30 * MS, 100 * MS)) != -EPERM) {
        return 5;
    }
    if (set(parent, mk(SCHED_OTHER, 0, 19, 0)) != -EPERM) {
        return 6;
    }
    if (set(0, mk(SCHED_OTHER, SCHED_FLAG_RESET_ON_FORK, 5, 0)) != 0 ||
        set(0, mk(SCHED_OTHER, 0, 5, 0)) != -EPERM) {
        return 7;
    }
    if (set(0, mk(SCHED_IDLE, SCHED_FLAG_RESET_ON_FORK, 5, 0)) != 0 ||
        set(0, mk(SCHED_OTHER, SCHED_FLAG_RESET_ON_FORK, 5, 0)) != -EPERM) {
        return 8;
    }
    return 0;
}

/** What a child that has dropped root is refused. */
static void run_eperm(void)
{
    long parent = syscall0(SYS_GETPID), pid;
    int status = 0;

    pid = syscall0(SYS_FORK);
    if (pid < 0) {
        fail("fork");
    }
    if (pid == 0) {
        sys_exit(unprivileged(parent));
    }
    child_pid = pid;
    if (syscall3(SYS_WAITPID, pid, (long)&status, 0) != pid) {
        fail("a child could not be reaped");
    }
    child_pid = 0;
    if (status != 0) {
        fail("a child without root was not refused with EPERM a lower nice value, a real-time or "
             "deadline policy, another user's thread, or clearing the reset flag or SCHED_IDLE");
    }
    report_held();
}

/** Set the soft and hard limit `resource` of the caller to `value`; 0, or
 * a negated errno. */
static long limit(long resource, unsigned long long value)
{
    struct rlimit64 r;

    r.cur = r.max = value;
    return syscall4(SYS_PRLIMIT64, 0, resource, (long)&r, 0);
}

/**
 * The checks of a child without root whose RLIMIT_RTPRIO is 10 and whose
 * RLIMIT_NICE allows nice -5, each step numbered: the exit status is 0, or
 * the step that went wrong.
 */
static int limited(void)
{
    if (limit(RLIMIT_RTPRIO, 10) != 0 || limit(RLIMIT_NICE, 25) != 0 ||
        syscall3(SYS_SETRESUID, probe_id(1000), probe_id(1000), probe_id(1000)) != 0) {
        return 1;
    }
    if (set(0, mk(SCHED_FIFO, 0, 0, 5)) != 0 || set(0, mk(SCHED_FIFO, 0, 0, 11)) != -EPERM ||
        set(0, mk(SCHED_FIFO, 0, 0, 10)) != 0 || set(0, mk(SCHED_RR, 0, 0, 3)) != 0) {
        return 2;
    }
    if (set(0, mk(SCHED_OTHER, 0, 0, 0)) != 0 || set(0, mk(SCHED_OTHER, 0, -5, 0)) != 0 ||
        set(0, mk(SCHED_OTHER, 0, -6, 0)) != -EPERM) {
        return 3;
    }
    if (syscall3(SYS_SETPRIORITY, 0, 0, -6) != -EACCES ||
        syscall3(SYS_SETPRIORITY, 0, 0, -5) != 0) {
        return 4;
    }
    if (set(0, mk(SCHED_IDLE, 0, -5, 0)) != 0 || set(0, mk(SCHED_OTHER, 0, -5, 0)) != 0) {
        return 5;
    }
    return 0;
}

/** RLIMIT_NICE and RLIMIT_RTPRIO: 0 to begin with, and what they allow a
 * child without root. */
static void run_rlimits(void)
{
    struct rlimit64 nice, rtprio;
    long pid;
    int status = 0;

    nice.cur = nice.max = rtprio.cur = rtprio.max = 7;
    if (syscall4(SYS_PRLIMIT64, 0, RLIMIT_NICE, 0, (long)&nice) != 0 ||
        syscall4(SYS_PRLIMIT64, 0, RLIMIT_RTPRIO, 0, (long)&rtprio) != 0 || nice.cur != 0 ||
        nice.max != 0 || rtprio.cur != 0 || rtprio.max != 0) {
        fail("RLIMIT_NICE and RLIMIT_RTPRIO did not read as 0, Linux's defaults");
    }
    pid = syscall0(SYS_FORK);
    if (pid < 0) {
        fail("fork");
    }
    if (pid == 0) {
        sys_exit(limited());
    }
    child_pid = pid;
    if (syscall3(SYS_WAITPID, pid, (long)&status, 0) != pid) {
        fail("a child could not be reaped");
    }
    child_pid = 0;
    if (status != 0) {
        fail("a child without root did not have the real-time priorities up to RLIMIT_RTPRIO and "
             "the nice values RLIMIT_NICE allows, and no more");
    }
    report_held();
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/** The probe: the checks in order. */
void probe_main(unsigned long *sp)
{
    unsigned int i;

    probe_args((int)sp[0], (char **)(sp + 1));
    report_begin(report_case((char **)(sp + 1) + sp[0] + 1, "guest.default.schedattrprobe"));
    for (i = 0; i < sizeof(assertion_id) / sizeof(assertion_id[0]); i++) {
        report_plan(assertion_id[i]);
    }
    if (syscall0(SYS_GETUID) != 0) {
        fail("the probe needs root");
    }
    count_cpus();
    run_size();
    run_einval();
    run_refusals();
    run_policies();
    run_slice();
    run_keep();
    run_admission();
    run_affinity();
    run_fork();
    run_rr_interval();
    run_eperm();
    run_rlimits();
    report_end();
    say("SCHEDATTRPROBE OK\n");
    sys_exit(0);
}
