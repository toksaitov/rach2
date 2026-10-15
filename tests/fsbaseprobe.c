/**
 * fsbaseprobe: the x86-64 thread's FS and GS bases, which arch_prctl sets
 * and gets, and its FS and GS selectors.
 *
 * Each base set by ARCH_SET_FS or ARCH_SET_GS must read back by ARCH_GET_FS
 * or ARCH_GET_GS and address the block it names. A forked child must start
 * with its parent's bases, and a child that sets its own FS base must keep
 * it, and the parent its own, across two thousand yields between them. A
 * program that loads a selector into FS or GS, which sets the base to the
 * descriptor's, 0, must read 0 back at once and after a sleep, which
 * switches it away and back, must still address memory by its absolute
 * address there, and must give 0 to a child it forks. A base at or above
 * USER_ADDR_MAX, the end of user space, is refused with EPERM by ARCH_SET_FS,
 * by ARCH_SET_GS and by clone with CLONE_SETTLS, which then makes no child,
 * and the bases set before stay; the last user address below it is taken,
 * and clone with CLONE_SETTLS gives its child the base it names. A forked
 * child must start with its parent's FS and GS selectors; when it loads
 * others, each process must keep its own across two thousand yields between
 * them on one processor. A tracer must read a stopped child's selectors
 * with PTRACE_PEEKUSER, and selectors it writes with PTRACE_POKEUSER must
 * be the child's when it runs again.
 *
 * arch_prctl is x86-64's alone; on i686 the probe says its rows did not run
 * and passes.
 *
 * Prints "FSBASEPROBE OK" and exits 0, or "FSBASEPROBE FAIL: " and the
 * reason and exits 1.
 */

#include "probe.h"

#define ARCH_SET_GS 0x1001
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
#define ARCH_GET_GS 0x1004

#define YIELDS 2000

/* USER_ADDR_MAX of x86-64's cpu.h, clone's flags, and the errors. */
#define USER_ADDR_MAX 0x00007FFFFFFFF000ul
#define SIGCHLD       17
#define CLONE_SETTLS  0x00080000
#define WNOHANG       1
#define EPERM         1
#define ECHILD        10

#define GDT_USER_DATA 0x2Bul
#define GDT_USER_CODE 0x33ul

#define SIGSTOP         19
#define PTRACE_TRACEME  0
#define PTRACE_PEEKUSER 3
#define PTRACE_POKEUSER 6
#define PTRACE_CONT     7

/* The byte offsets of FS and GS in Linux x86-64's `struct user`. */
#define USER_FS 200
#define USER_GS 208

/** End the process with `status`; it does not return. */
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

#if defined __x86_64__

struct timespec {
    long sec;
    long nsec;
};

/* Three blocks a base may name, each opening with its own word. */
static unsigned long block_a[4] = {0xA5A5A5A5A5A5A5A5ul};
static unsigned long block_b[4] = {0xB6B6B6B6B6B6B6B6ul};
static unsigned long block_c[4] = {0xC7C7C7C7C7C7C7C7ul};
static unsigned long absolute = 0xD8D8D8D8D8D8D8D8ul;

/** Print "FSBASEPROBE FAIL: " and `why`, and exit 1. */
static void fail(const char *why)
{
    say("FSBASEPROBE FAIL: ");
    say(why);
    say("\n");
    sys_exit(1);
}

/** The arch_prctl call: 0, or a negated errno. */
static long prctl(long code, unsigned long arg)
{
    return syscall2(SYS_ARCH_PRCTL, code, (long)arg);
}

/** The base ARCH_GET_FS or ARCH_GET_GS (`code`) reads; fail when the call
 * fails. */
static unsigned long base_of(long code)
{
    unsigned long base = 1;

    if (prctl(code, (unsigned long)&base) != 0) {
        fail("ARCH_GET_FS or ARCH_GET_GS failed");
    }
    return base;
}

/** The word at offset `off` from the FS base. */
static unsigned long fs_word(unsigned long off)
{
    unsigned long v;

    __asm__ __volatile__("movq %%fs:(%1), %0" : "=r"(v) : "r"(off));
    return v;
}

/** The word at offset `off` from the GS base. */
static unsigned long gs_word(unsigned long off)
{
    unsigned long v;

    __asm__ __volatile__("movq %%gs:(%1), %0" : "=r"(v) : "r"(off));
    return v;
}

/** Sleep for `ms` milliseconds, under a second. */
static void sleep_ms(long ms)
{
    struct timespec ts;

    ts.sec = 0;
    ts.nsec = ms * 1000000L;
    syscall2(SYS_NANOSLEEP, (long)&ts, 0);
}

/** The selector in FS. */
static unsigned long fs_sel(void)
{
    unsigned long v = 0;

    __asm__ __volatile__("movw %%fs, %w0" : "+r"(v));
    return v;
}

/** The selector in GS. */
static unsigned long gs_sel(void)
{
    unsigned long v = 0;

    __asm__ __volatile__("movw %%gs, %w0" : "+r"(v));
    return v;
}

/** Load the selector `fs` into FS and `gs` into GS. */
static void load_sels(unsigned long fs, unsigned long gs)
{
    __asm__ __volatile__("movw %w0, %%fs" : : "r"(fs));
    __asm__ __volatile__("movw %w0, %%gs" : : "r"(gs));
}

/** The exit status of the child `pid`, or -1. */
static int child_status(long pid)
{
    int status = 0;

    if (syscall3(SYS_WAITPID, pid, (long)&status, 0) != pid || (status & 0x7F) != 0) {
        return -1;
    }
    return (status >> 8) & 0xFF;
}

/** Set the FS base to `block_a` and the GS base to `block_b`, and check that
 * each reads back and addresses its block. */
static void run_set_get(void)
{
    if (prctl(ARCH_SET_FS, (unsigned long)block_a) != 0 ||
        base_of(ARCH_GET_FS) != (unsigned long)block_a || fs_word(0) != block_a[0]) {
        fail("an FS base set does not read back or address its block");
    }
    if (prctl(ARCH_SET_GS, (unsigned long)block_b) != 0 ||
        base_of(ARCH_GET_GS) != (unsigned long)block_b || gs_word(0) != block_b[0]) {
        fail("a GS base set does not read back or address its block");
    }
}

/** A forked child starts with the bases `run_set_get` set, then moves its FS
 * base to `block_c`; each keeps its own across YIELDS yields. The child exits
 * 2 when its bases were not inherited and 3 when its base did not last. */
static void run_fork_and_switches(void)
{
    long pid = syscall0(SYS_FORK);
    int i;

    if (pid == 0) {
        if (fs_word(0) != block_a[0] || gs_word(0) != block_b[0]) {
            sys_exit(2);
        }
        prctl(ARCH_SET_FS, (unsigned long)block_c);
        for (i = 0; i < YIELDS; i++) {
            syscall0(SYS_SCHED_YIELD);
            if (fs_word(0) != block_c[0]) {
                sys_exit(3);
            }
        }
        sys_exit(0);
    }
    if (pid < 0) {
        fail("fork");
    }
    for (i = 0; i < YIELDS; i++) {
        syscall0(SYS_SCHED_YIELD);
        if (fs_word(0) != block_a[0]) {
            fail("the parent's FS base did not survive the switches");
        }
    }
    switch (child_status(pid)) {
    case 0:
        break;
    case 2:
        fail("a forked child did not start with its parent's bases");
        break;
    default:
        fail("a child's own FS base did not survive the switches");
    }
}

/** Load the stack's selector, the user data selector, into FS and GS: both
 * bases read 0 at once and after a sleep, address `absolute` at its own
 * address, and are 0 in a forked child. */
static void run_selectors(void)
{
    unsigned long ss;
    long pid;

    __asm__ __volatile__("movq %%ss, %0" : "=r"(ss));
    __asm__ __volatile__("movw %w0, %%fs" : : "r"(ss));
    __asm__ __volatile__("movw %w0, %%gs" : : "r"(ss));
    if (base_of(ARCH_GET_FS) != 0 || base_of(ARCH_GET_GS) != 0) {
        fail("a selector loaded into FS or GS did not read back as base 0");
    }
    sleep_ms(20);
    if (base_of(ARCH_GET_FS) != 0 || base_of(ARCH_GET_GS) != 0) {
        fail("a switch gave back the FS or GS base a loaded selector replaced");
    }
    if (fs_word((unsigned long)&absolute) != absolute ||
        gs_word((unsigned long)&absolute) != absolute) {
        fail("FS or GS of base 0 does not address memory where it is");
    }
    pid = syscall0(SYS_FORK);
    if (pid == 0) {
        sys_exit(base_of(ARCH_GET_FS) == 0 && base_of(ARCH_GET_GS) == 0 ? 0 : 4);
    }
    if (pid < 0 || child_status(pid) != 0) {
        fail("a child forked after a selector load did not start with base 0");
    }
}

/** Clone a child with CLONE_SETTLS and the base `tls`, x86-64's argument
 * order: flags, stack, parent's and child's tid addresses, then the base. */
static long clone_tls(unsigned long tls)
{
    return syscall5(SYS_CLONE, SIGCHLD | CLONE_SETTLS, 0, 0, 0, (long)tls);
}

/** The bound on a base: USER_ADDR_MAX and above are -EPERM from both
 * arch_prctl sets and from clone, which then makes no child, and leave the
 * bases set before; USER_ADDR_MAX - 1 is taken, and clone with CLONE_SETTLS
 * gives its child the base. */
static void run_bounds(void)
{
    static const unsigned long refused[] = {USER_ADDR_MAX, USER_ADDR_MAX + 1, ~0ul};
    unsigned long i;
    long pid;

    if (prctl(ARCH_SET_FS, (unsigned long)block_a) != 0 ||
        prctl(ARCH_SET_GS, (unsigned long)block_b) != 0) {
        fail("the bases to keep could not be set");
    }
    for (i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        if (prctl(ARCH_SET_FS, refused[i]) != -EPERM || prctl(ARCH_SET_GS, refused[i]) != -EPERM) {
            fail("a base at or above USER_ADDR_MAX was not refused with EPERM");
        }
        if (clone_tls(refused[i]) != -EPERM) {
            fail("clone with CLONE_SETTLS took a base at or above USER_ADDR_MAX");
        }
    }
    if (base_of(ARCH_GET_FS) != (unsigned long)block_a ||
        base_of(ARCH_GET_GS) != (unsigned long)block_b || fs_word(0) != block_a[0]) {
        fail("a refused base changed the bases set before it");
    }
    if (syscall3(SYS_WAITPID, -1, 0, WNOHANG) != -ECHILD) {
        fail("a refused clone made a child");
    }
    if (prctl(ARCH_SET_GS, USER_ADDR_MAX - 1) != 0 || base_of(ARCH_GET_GS) != USER_ADDR_MAX - 1) {
        fail("the last user address below USER_ADDR_MAX was not taken as a base");
    }
    pid = clone_tls((unsigned long)block_c);
    if (pid == 0) {
        if (fs_word(0) != block_c[0] || base_of(ARCH_GET_FS) != (unsigned long)block_c) {
            sys_exit(5);
        }
        sys_exit(0);
    }
    if (pid < 0 || child_status(pid) != 0) {
        fail("clone with CLONE_SETTLS did not give its child the base");
    }
}

/**
 * The selectors across switches. The parent has the user data selector in
 * FS and GS from `run_selectors`. A forked child starts with the same, then
 * loads the null selector and the user code selector, and both yield to
 * each other on processor 0, where a selector the kernel did not keep for
 * each would pass from one to the other. The child exits 2 when it did not
 * start with its parent's selectors and 3 when its own did not last.
 */
static void run_selector_switches(void)
{
    unsigned long processor0 = 1;
    long pid;
    int i;

    if (syscall3(SYS_SCHED_SETAFFINITY, 0, (long)sizeof(processor0), (long)&processor0) != 0) {
        fail("the probe could not be bound to processor 0");
    }
    pid = syscall0(SYS_FORK);
    if (pid == 0) {
        if (fs_sel() != GDT_USER_DATA || gs_sel() != GDT_USER_DATA) {
            sys_exit(2);
        }
        load_sels(0, GDT_USER_CODE);
        for (i = 0; i < YIELDS; i++) {
            syscall0(SYS_SCHED_YIELD);
            if (fs_sel() != 0 || gs_sel() != GDT_USER_CODE) {
                sys_exit(3);
            }
        }
        sys_exit(0);
    }
    if (pid < 0) {
        fail("fork");
    }
    for (i = 0; i < YIELDS; i++) {
        syscall0(SYS_SCHED_YIELD);
        if (fs_sel() != GDT_USER_DATA || gs_sel() != GDT_USER_DATA) {
            fail("the parent's FS or GS selector did not survive the switches");
        }
    }
    switch (child_status(pid)) {
    case 0:
        break;
    case 2:
        fail("a forked child did not start with its parent's selectors");
        break;
    default:
        fail("a child's own FS or GS selector did not survive the switches");
    }
}

/**
 * The selectors through ptrace. The child stops itself with the user data
 * selector in FS and GS; the parent reads them, writes the user code
 * selector to FS and a null selector of RPL 3 to GS, and lets it run on to
 * check them.
 */
static void run_selector_ptrace(void)
{
    unsigned long fs = 0, gs = 0;
    int status = 0;
    long pid = syscall0(SYS_FORK);

    if (pid == 0) {
        syscall4(SYS_PTRACE, PTRACE_TRACEME, 0, 0, 0);
        syscall2(SYS_KILL, syscall0(SYS_GETPID), SIGSTOP);
        sys_exit(fs_sel() == GDT_USER_CODE && gs_sel() == 3 ? 0 : 5);
    }
    if (pid < 0) {
        fail("fork");
    }
    if (syscall3(SYS_WAITPID, pid, (long)&status, 0) != pid || (status & 0xFF) != 0x7F) {
        fail("a traced child did not stop");
    }
    if (syscall4(SYS_PTRACE, PTRACE_PEEKUSER, pid, USER_FS, (long)&fs) != 0 ||
        syscall4(SYS_PTRACE, PTRACE_PEEKUSER, pid, USER_GS, (long)&gs) != 0 ||
        fs != GDT_USER_DATA || gs != GDT_USER_DATA) {
        fail("PTRACE_PEEKUSER did not read a stopped child's FS and GS selectors");
    }
    if (syscall4(SYS_PTRACE, PTRACE_POKEUSER, pid, USER_FS, (long)GDT_USER_CODE) != 0 ||
        syscall4(SYS_PTRACE, PTRACE_POKEUSER, pid, USER_GS, 3) != 0 ||
        syscall4(SYS_PTRACE, PTRACE_CONT, pid, 0, 0) != 0) {
        fail("PTRACE_POKEUSER refused an FS or GS selector");
    }
    if (child_status(pid) != 0) {
        fail("selectors written by PTRACE_POKEUSER were not the child's");
    }
}

#endif

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/** The probe: the rows in order on x86-64, and none on i686. */
void probe_main(unsigned long *sp)
{
    (void)sp;
#if defined __x86_64__
    run_set_get();
    run_fork_and_switches();
    run_bounds();
    run_selectors();
    run_selector_switches();
    run_selector_ptrace();
#else
    say("fsbaseprobe: not run: arch_prctl is x86-64's\n");
#endif
    say("FSBASEPROBE OK\n");
    sys_exit(0);
}
