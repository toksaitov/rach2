/**
 * tlsprobe: %gs thread-local storage lasts across system calls, interrupts
 * and context switches, and each process has its own.
 *
 * Calls the kernel's set_thread_area(2) as musl's __init_tls does: it
 * points a GDT TLS descriptor at a thread control block (TCB) and loads the
 * returned selector into %gs. It then writes sentinel words through %gs and
 * reads them again while the kernel is entered over and over:
 *
 *   1. Solo phase: thousands of system calls plus busy-waits, until the
 *      probe has seen its user time (times(2)) rise at SOLO_INTERRUPTS of
 *      its looks: each rise is at least one timer interrupt taken from user
 *      mode since the look before. Every %gs:off read must still equal the
 *      sentinel, so the entry and exit paths of both the system call and the
 *      interrupt must save and restore %gs; one that loaded %gs from the
 *      saved %ds fails at once.
 *
 *   2. Isolation phase: fork a child that points its own TLS descriptor at a
 *      different TCB with different sentinels. Parent and child then spin
 *      until the parent has seen the child's processor time, user and system
 *      (/proc/PID/stat), rise at ISO_SWITCHES of its looks: on one processor,
 *      each rise is a switch from the parent to the child and one back, both
 *      made by the scheduler while neither process waited. The system time
 *      counts too, since under a hypervisor such as WHPX most ticks can land
 *      in the kernel rather than in the spin.
 *      Then they hand a byte back and forth over two pipes, so that each
 *      round takes one switch to the child and one back; each must always
 *      read back its own sentinels. This holds only if the context switch
 *      reprograms the shared TLS GDT descriptor for each process. Both are
 *      bound to processor 0 first: on two processors each could keep one to
 *      itself, and no switch would go from one to the other.
 *
 * The counts are of events the kernel records, so a slow or loaded machine
 * spins longer and a fast one shorter, and the number of rounds decides
 * nothing beyond the least each side makes. The case's watchdog ends a
 * run in which the counts never rise.
 *
 * Prints "All TLS checks passed!" and exits 0 when every check passes.
 * Otherwise it prints a line that begins "FAIL: " and exits 1, printing
 * "tlsprobe: FAILED" before the exit unless the TLS, the binding to
 * processor 0, a pipe or the fork could not be set up.
 *
 * On Linux the same interface is set_thread_area(2) and the i386 %gs thread
 * pointer that glibc and musl, the stack-protector canary and
 * pthread_self() rely on. set_thread_area and the TLS descriptor in the GDT
 * are i386's, so tests/Makefile builds the probe for i686 alone; x86-64
 * keeps the thread pointer in the FS base, set by arch_prctl.
 *
 * The parent writes etude's report records (probe_report.h) for the case
 * ETUDE_CASE names, an assertion for each check, as `assertion_id` lists
 * them. A failed setup fails its assertion and leaves those after it not
 * run; a check the run stops before, such as the rounds of the byte after a
 * failure, is not run either. The child writes none, and its exit status
 * gives its assertions.
 */

#include "probe.h"
#include "probe_report.h"

#if defined __x86_64__
#error "tlsprobe tests i386's set_thread_area and %gs; it is built for i686 alone"
#endif

typedef unsigned int u32;
typedef unsigned char u8;

#define STDOUT 1

/* user_desc.flags: seg_32bit | limit_in_pages | useable (what musl passes). */
#define UD_FLAGS 0x51

/* Number of sentinel slots checked per TCB (offsets 0,4,...,28). */
#define NUM_SLOTS 8

/* The least rounds each phase makes: thousands of system calls in the solo
 * phase, thousands of busy waits on each side of the isolation phase. */
#define SOLO_ITERS  40000
#define ISO_ITERS   3000
#define BUSY_SPIN   60000
#define PING_ROUNDS 200

/* The witnessed events each phase waits for: looks at which the probe's own
 * user time had risen, each a timer interrupt from user mode, and looks at
 * which the child's processor time had, each a switch to the child and back. */
#define SOLO_INTERRUPTS 10
#define ISO_SWITCHES    20

#define POLLIN 0x001

struct pollfd {
    int fd;
    short events;
    short revents;
};

/** The write call: the count written, or a negated errno. */
static int sys_write(int fd, const void *buf, u32 n)
{
    return syscall3(SYS_WRITE, (long)fd, (long)buf, (long)n);
}

/** End the process with `status`; it does not return. */
static void sys_exit(int status)
{
    syscall1(SYS_EXIT, (long)status);
    __builtin_unreachable();
}

/** The read call: the count read, 0 at the end, or a negated errno. */
static int sys_read(int fd, void *buf, u32 n)
{
    return syscall3(SYS_READ, (long)fd, (long)buf, (long)n);
}

/** Close `fd`, ignoring the result. */
static void sys_close(int fd)
{
    (void)syscall1(SYS_CLOSE, (long)fd);
}

/** The fork call: the child's pid, 0 in the child, or a negated errno. */
static int sys_fork(void)
{
    return syscall0(SYS_FORK);
}

/** Wait for the child `pid`: the pid reaped, or a negated errno. */
static int sys_waitpid(int pid, int *status, int options)
{
    return syscall3(SYS_WAITPID, (long)pid, (long)status, (long)options);
}

/** A cheap system call with no side effect, to enter the kernel through
 * int 0x80. */
static void sys_gettimeofday(long *tv2)
{
    (void)syscall1(SYS_GETTIMEOFDAY, (long)tv2);
}

/** The times call: the user, system and children's ticks into `tms4`. */
static void sys_times(long *tms4)
{
    (void)syscall1(SYS_TIMES, (long)tms4);
}

/**
 * Program the TLS descriptor to `base` and return the GDT entry number the
 * kernel chose, or -1 on failure. Mirrors the user_desc musl builds
 * (entry_number = -1 to allocate, 4 GB limit in pages).
 */
static int set_thread_area(void *base)
{
    u32 desc[4];
    int r;

    desc[0] = (u32)-1;             /* entry_number: allocate */
    desc[1] = (unsigned long)base; /* base_addr */
    desc[2] = 0xfffff;             /* limit */
    desc[3] = UD_FLAGS;            /* flags */

    r = syscall1(SYS_SET_THREAD_AREA, (long)desc);
    if (r != 0) {
        return -1;
    }
    return (int)desc[0]; /* entry_number written back by the kernel */
}

/** Load the TLS selector (entry << 3) | 3 into %gs, as the C library does. */
static void load_gs(int entry)
{
    u32 sel = ((u32)entry << 3) | 3u;

    __asm__ __volatile__("movw %w0, %%gs" : : "r"(sel));
}

/** The 32-bit word at %gs:off, `off` a byte offset into the TCB. */
static u32 gs_read(u32 off)
{
    u32 v;

    __asm__ __volatile__("movl %%gs:(%1), %0" : "=r"(v) : "r"(off));
    return v;
}

/** Write the 32-bit word `val` at %gs:off. */
static void gs_write(u32 off, u32 val)
{
    __asm__ __volatile__("movl %0, %%gs:(%1)" : : "r"(val), "r"(off) : "memory");
}

/** The length of the string `s`. */
static u32 slen(const char *s)
{
    u32 n = 0;

    while (s[n]) {
        n++;
    }
    return n;
}

/** Write `s` to standard output, with no newline. */
static void puts_(const char *s)
{
    sys_write(STDOUT, s, slen(s));
}

/** Write `v` as "0x" and eight hex digits. */
static void put_u32(u32 v)
{
    static const char hex[] = "0123456789abcdef";
    char buf[11];
    int i;

    buf[0] = '0';
    buf[1] = 'x';
    for (i = 0; i < 8; i++) {
        buf[2 + i] = hex[(v >> ((7 - i) * 4)) & 0xF];
    }
    buf[10] = '\0';
    puts_(buf);
}

/* The two thread control blocks, at distinct virtual addresses (.bss). */
static u32 tcb_parent[NUM_SLOTS];
static u32 tcb_child[NUM_SLOTS];

/* The assertions, a check each, in the order the probe makes them. */
static const char *const assertion_id[] = {
    "set-thread-area", "gs-aliases-tcb", "solo-sentinels", "bind-processor0",
    "pipes",           "fork",           "child-runs",     "parent-switch-sentinels",
    "ping-returns",    "ping-sentinels", "child-reaped",   "child-inherits-base",
    "child-sets-tls",  "child-sentinels"};

/* The index in `assertion_id` of each assertion the probe reports by name. */
enum { A_SET_THREAD_AREA, A_BIND = 3, A_PIPES, A_FORK, A_CHILD_REAPED = 10, A_COUNT = 14 };

/**
 * Report the assertion at index `at` failed with `why`, those after it not
 * run, and the report's end: a setup that failed, after which the probe
 * exits.
 */
static void report_setup_failed(int at, const char *why)
{
    report_fail(assertion_id[at], why);
    while (++at < A_COUNT) {
        report_notrun(assertion_id[at], "a setup failed");
    }
    report_end();
}

/** Report the assertion `id` held when `ok`, and failed with `why` if not. */
static void report_row(const char *id, int ok, const char *why)
{
    if (ok) {
        report_pass(id);
    } else {
        report_fail(id, why);
    }
}

/** Write magic^slot to every slot through %gs. */
static void tls_fill(u32 magic)
{
    int i;

    for (i = 0; i < NUM_SLOTS; i++) {
        gs_write((u32)(i * 4), magic ^ (u32)i);
    }
}

/**
 * Read every slot through %gs again and count those that no longer have
 * magic^slot; 0 when all are intact.
 */
static int tls_check(u32 magic)
{
    int i;
    int bad = 0;

    for (i = 0; i < NUM_SLOTS; i++) {
        if (gs_read((u32)(i * 4)) != (magic ^ (u32)i)) {
            bad++;
        }
    }
    return bad;
}

/** Spin in user mode, so that timer interrupts and, with a peer, switches
 * come. */
static void busy(u32 spins)
{
    volatile u32 x = 0;
    u32 i;

    for (i = 0; i < spins; i++) {
        x = x + 1;
    }
}

/** The ticks the timer has charged to this process in user mode, from
 * times(2): each tick that interrupted its user code adds one. */
static u32 own_user_ticks(void)
{
    long tms[4];

    tms[0] = 0;
    sys_times(tms);
    return (u32)tms[0];
}

/**
 * The state letter of the child `pid` from /proc/PID/stat, with its user and
 * system ticks, fields 14 and 15, summed in `*ticks`; 0 when the file cannot
 * be read or is short.
 */
static char child_ticks(int pid, u32 *ticks)
{
    char path[32], text[256];
    char digits[12];
    int n = 0, at = 0, fd, len, i, state, field;
    u32 v = (u32)pid, sum = 0;
    const char *p;

    for (p = "/proc/"; *p; p++) {
        path[at++] = *p;
    }
    do {
        digits[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n) {
        path[at++] = digits[--n];
    }
    for (p = "/stat"; *p; p++) {
        path[at++] = *p;
    }
    path[at] = 0;
    fd = syscall3(SYS_OPEN, (long)path, 0, 0);
    if (fd < 0) {
        return 0;
    }
    len = sys_read(fd, text, sizeof(text) - 1);
    sys_close(fd);
    if (len <= 0) {
        return 0;
    }
    text[len] = 0;
    for (i = len - 2; i >= 0 && !(text[i] == ')' && text[i + 1] == ' '); i--) {
    }
    if (i < 0) {
        return 0;
    }
    state = i + 2; /* field 3 */
    for (i = state, field = 3; field < 14 && text[i]; i++) {
        if (text[i] == ' ') {
            field++;
        }
    }
    for (; field <= 15; field++) {
        if (text[i] < '0' || text[i] > '9') {
            return 0;
        }
        for (v = 0; text[i] >= '0' && text[i] <= '9'; i++) {
            v = v * 10 + (u32)(text[i] - '0');
        }
        sum += v;
        if (text[i] == ' ') {
            i++;
        }
    }
    *ticks = sum;
    return text[state];
}

/**
 * The solo phase: the sentinels written through %gs survive thousands of
 * system calls and the timer interrupts of busy waits. It runs SOLO_ITERS
 * rounds, and on until its user time has risen at SOLO_INTERRUPTS of the
 * looks taken after each busy wait, so that at least that many interrupts
 * from user mode came and each was followed by a check. `fails` is the
 * count of failures so far, and the new count is returned.
 */
static int solo_phase(int fails)
{
    long tv2[2];
    u32 last, now;
    int i, rises = 0, kept = 1;

    tls_fill(0xA5A50000u);
    last = own_user_ticks();
    for (i = 0; i < SOLO_ITERS || rises < SOLO_INTERRUPTS; i++) {
        sys_gettimeofday(tv2); /* int 0x80: the system call's %gs save */
        if ((i & 0x3FF) == 0) {
            busy(BUSY_SPIN); /* user code for the timer to interrupt */
            now = own_user_ticks();
            if (now != last) {
                rises++;
                last = now;
                if (rises <= SOLO_INTERRUPTS) {
                    report_progress("solo-rises", rises);
                }
            }
        }
        if (tls_check(0xA5A50000u) != 0) {
            fails++;
            kept = 0;
            if (fails <= 3) {
                puts_("FAIL: solo sentinel clobbered at i=");
                put_u32((u32)i);
                puts_("\n");
            }
            break;
        }
    }
    report_row("solo-sentinels", kept, "a sentinel read through %gs changed in the solo phase");
    if (fails == 0) {
        puts_("solo phase OK\n");
    }
    return fails;
}

/**
 * The child of the isolation phase: it switches to its own TCB and checks
 * only its own sentinels, across switches and across each round of the byte
 * passed over the pipes. It spins ISO_ITERS rounds, and on until the
 * parent's first byte is readable, so that it runs beside the parent for as
 * long as the parent spins. It exits 0 when all were intact, 8 when the
 * base it inherited did not read the parent's sentinels, 7 when it could
 * not set its own, and 1 on a mismatch or a failed poll. Does not return.
 *
 * Before it sets its own TLS, the child still has the base it inherited
 * across fork, which copies the TLS base and the %gs of the trap frame. Its
 * copy of tcb_parent has the parent's sentinels, so a read through the
 * inherited %gs returns them; a fork that dropped the base would make the
 * switch program base 0, and the read would not match.
 */
static void iso_child(const int to_child[2], const int to_parent[2])
{
    struct pollfd ready;
    long tv2[2];
    char token;
    int centry;
    int cbad = 0;
    int i;

    sys_close(to_child[1]);
    sys_close(to_parent[0]);

    /* The base inherited across fork reads the parent's sentinels. */
    if (tls_check(0xA5A50000u) != 0) {
        sys_exit(8);
    }

    centry = set_thread_area(tcb_child);
    if (centry < 0) {
        sys_exit(7);
    }
    load_gs(centry);
    tls_fill(0x5A5A0000u);

    ready.fd = to_child[0];
    ready.events = POLLIN;
    for (i = 0;; i++) {
        sys_gettimeofday(tv2);
        busy(BUSY_SPIN);
        if (tls_check(0x5A5A0000u) != 0) {
            cbad++;
            break;
        }
        ready.revents = 0;
        if (i + 1 >= ISO_ITERS) {
            int polled = syscall3(SYS_POLL, (long)&ready, 1, 0);

            if (polled < 0) {
                cbad++;
            }
            if (polled != 0) {
                break;
            }
        }
    }
    for (i = 0; i < PING_ROUNDS && cbad == 0; i++) {
        if (sys_read(to_child[0], &token, 1) != 1 || tls_check(0x5A5A0000u) != 0 ||
            sys_write(to_parent[1], &token, 1) != 1) {
            cbad++;
        }
    }
    sys_exit(cbad == 0 ? 0 : 1);
}

/**
 * Report the assertions of the byte's rounds: `returned` is 0 when the byte
 * did not come back at a round, `kept` 0 when a sentinel changed after one,
 * and `ran` 0 when the rounds did not run for an earlier failure. The rounds
 * stop at the first failure, which leaves the other assertion not run.
 */
static void report_rounds(int ran, int returned, int kept)
{
    if (!ran) {
        report_notrun("ping-returns", "an earlier check failed");
        report_notrun("ping-sentinels", "an earlier check failed");
    } else if (!returned) {
        report_fail("ping-returns", "the byte did not come back from the child");
        report_notrun("ping-sentinels", "the rounds stopped at the first failure");
    } else if (!kept) {
        report_notrun("ping-returns", "the rounds stopped at the first failure");
        report_fail("ping-sentinels", "a sentinel read through %gs changed after the child ran");
    } else {
        report_pass("ping-returns");
        report_pass("ping-sentinels");
    }
}

/**
 * Report the assertions of the spin beside the child: `clobbered` is 1 when
 * a sentinel of the parent changed, and `ended` 1 when the child's processor
 * time could not be read or the child ended first. Either stops the spin, which
 * leaves the other assertion not run.
 */
static void report_spin(int clobbered, int ended)
{
    if (ended) {
        report_fail("child-runs",
                    "the child's processor time could not be read, or it ended first");
        report_notrun("parent-switch-sentinels", "the spin stopped when the child ended");
    } else if (clobbered) {
        report_notrun("child-runs", "the spin stopped at a changed sentinel");
        report_fail("parent-switch-sentinels",
                    "a sentinel read through %gs changed after a switch");
    } else {
        report_pass("child-runs");
        report_pass("parent-switch-sentinels");
    }
}

/**
 * The parent's switches of the isolation phase: it keeps tcb_parent and
 * checks its sentinels across switches to the child, then across each
 * round of the byte it sends the child and gets back.
 *
 * The parent spins ISO_ITERS rounds, and on until the processor time of the
 * child `pid` has risen at ISO_SWITCHES of the looks it takes after each
 * round. Both are on processor 0 and the parent waits for nothing while it
 * spins, so a rise between two of its looks is the child having run between
 * them: a switch away from the parent and one back, after which the parent
 * checks. A child that has ended, or whose stat cannot be read, ends
 * the spin as a failure, since it can no longer run. `fails` is the count
 * of failures so far, and the new count is returned.
 */
static int iso_switches(int pid, const int to_child[2], const int to_parent[2], int fails)
{
    long tv2[2];
    char token, state;
    u32 last = 0, now = 0;
    int i, rises = 0, clobbered = 0, ended = 0, returned = 1, kept = 1, ran;

    if (child_ticks(pid, &last) == 0) {
        puts_("FAIL: the child's processor time cannot be read\n");
        report_spin(0, 1);
        report_rounds(0, 1, 1);
        return fails + 1;
    }
    for (i = 0; i < ISO_ITERS || rises < ISO_SWITCHES; i++) {
        sys_gettimeofday(tv2);
        busy(BUSY_SPIN);
        if (tls_check(0xA5A50000u) != 0) {
            fails++;
            clobbered = 1;
            if (fails <= 3) {
                puts_("FAIL: parent sentinel clobbered after switch at i=");
                put_u32((u32)i);
                puts_("\n");
            }
            break;
        }
        state = child_ticks(pid, &now);
        if (state == 0 || state == 'Z') {
            puts_("FAIL: the child ended before it ran beside the parent\n");
            fails++;
            ended = 1;
            break;
        }
        if (now != last) {
            rises++;
            last = now;
            if (rises <= ISO_SWITCHES) {
                report_progress("child-rises", rises);
            }
        }
    }
    report_spin(clobbered, ended);
    token = 'x';
    ran = fails == 0;
    for (i = 0; i < PING_ROUNDS && fails == 0; i++) {
        if (sys_write(to_child[1], &token, 1) != 1 || sys_read(to_parent[0], &token, 1) != 1) {
            puts_("FAIL: the byte did not come back from the child at round ");
            put_u32((u32)i);
            puts_("\n");
            fails++;
            returned = 0;
        } else if (tls_check(0xA5A50000u) != 0) {
            puts_("FAIL: parent sentinel clobbered after the child ran, at round ");
            put_u32((u32)i);
            puts_("\n");
            fails++;
            kept = 0;
        }
    }
    report_rounds(ran, returned, kept);
    return fails;
}

/**
 * Report the child's assertions from its wait: `got` is what waitpid
 * returned for `pid`, and `status` its status. The child exits 8 when the
 * base it inherited failed, 7 when it could not set its own, and 1 when its
 * sentinels changed or its poll failed.
 */
static void report_child(int got, int pid, int status)
{
    int code = (status & 0x7F) == 0 ? (status >> 8) & 0xFF : -1;
    int at;

    if (got != pid || code < 0) {
        report_fail(assertion_id[A_CHILD_REAPED],
                    got != pid ? "waitpid failed" : "the child was killed by a signal");
        for (at = A_CHILD_REAPED + 1; at < A_COUNT; at++) {
            report_notrun(assertion_id[at], "the child did not exit");
        }
        return;
    }
    report_pass(assertion_id[A_CHILD_REAPED]);
    report_row("child-inherits-base", code != 8,
               "the base the child inherited did not read the parent's sentinels");
    if (code == 8) {
        report_notrun("child-sets-tls", "an earlier check failed");
        report_notrun("child-sentinels", "an earlier check failed");
        return;
    }
    report_row("child-sets-tls", code != 7, "set_thread_area failed in the child");
    if (code == 7) {
        report_notrun("child-sentinels", "an earlier check failed");
        return;
    }
    report_row("child-sentinels", code == 0, "a sentinel of the child changed, or its poll failed");
}

/**
 * The parent of the isolation phase: its own checks across the switches,
 * then the child's verdict from its exit status. Each side closes the ends
 * it does not use, so that a side that stops early ends the other's read
 * with end of file. `fails` is the count of failures so far, and the new
 * count is returned.
 */
static int iso_parent(int pid, const int to_child[2], const int to_parent[2], int fails)
{
    int status = -1;
    int got;

    sys_close(to_child[0]);
    sys_close(to_parent[1]);
    fails = iso_switches(pid, to_child, to_parent, fails);
    sys_close(to_child[1]);
    got = sys_waitpid(pid, &status, 0);
    report_child(got, pid, status);
    if (got != pid) {
        puts_("FAIL: waitpid\n");
        fails++;
    } else if (status != 0) {
        puts_("FAIL: child reported a TLS mismatch (isolation broken)\n");
        fails++;
    }
    if (fails == 0) {
        puts_("isolation phase OK\n");
    }
    return fails;
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/** Set up the TLS, check that %gs aliases the TCB, then run the solo and
 * isolation phases and print the verdict. */
void probe_main(unsigned long *sp)
{
    static const unsigned long processor0 = 1;
    int entry;
    int pid;
    int fails = 0;
    int to_child[2], to_parent[2];

    report_begin(report_case((char **)(sp + 1) + sp[0] + 1, "guest.default.tlsprobe"));
    puts_("tlsprobe: starting\n");

    /* This process's TLS, at tcb_parent. */
    entry = set_thread_area(tcb_parent);
    if (entry < 0) {
        puts_("FAIL: set_thread_area\n");
        report_setup_failed(A_SET_THREAD_AREA, "set_thread_area failed");
        sys_exit(1);
    }
    report_pass(assertion_id[A_SET_THREAD_AREA]);
    puts_("set_thread_area entry=");
    put_u32((u32)entry);
    puts_("\n");
    load_gs(entry);

    /* %gs:0 is tcb_parent[0]. */
    gs_write(0, 0xdeadbeef);
    if (tcb_parent[0] != 0xdeadbeef) {
        puts_("FAIL: %gs does not alias the TCB\n");
        fails++;
    }
    report_row("gs-aliases-tcb", fails == 0, "a write through %gs did not reach the TCB");

    fails = solo_phase(fails);

    /* Isolation phase: a child with its own TCB, on the parent's processor. */
    if (syscall3(SYS_SCHED_SETAFFINITY, 0, (long)sizeof(processor0), (long)&processor0) != 0) {
        puts_("FAIL: the probe could not be bound to processor 0\n");
        report_setup_failed(A_BIND, "sched_setaffinity to processor 0 failed");
        sys_exit(1);
    }
    report_pass(assertion_id[A_BIND]);
    if (syscall1(SYS_PIPE, (long)to_child) != 0 || syscall1(SYS_PIPE, (long)to_parent) != 0) {
        puts_("FAIL: pipe\n");
        report_setup_failed(A_PIPES, "pipe failed");
        sys_exit(1);
    }
    report_pass(assertion_id[A_PIPES]);
    pid = sys_fork();
    if (pid < 0) {
        puts_("FAIL: fork\n");
        report_setup_failed(A_FORK, "fork failed");
        sys_exit(1);
    }
    if (pid == 0) {
        iso_child(to_child, to_parent);
    }
    report_pass(assertion_id[A_FORK]);
    fails = iso_parent(pid, to_child, to_parent, fails);
    report_end();

    if (fails == 0) {
        puts_("All TLS checks passed!\n");
    } else {
        puts_("tlsprobe: FAILED\n");
    }
    sys_exit(fails == 0 ? 0 : 1);
}
