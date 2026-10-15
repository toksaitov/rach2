/**
 * tlsprobe: %gs thread-local storage lasts across system calls, interrupts
 * and context switches, and each process has its own.
 *
 * Calls the kernel's set_thread_area(2) as musl's __init_tls does: it
 * points a GDT TLS descriptor at a thread control block (TCB) and loads the
 * returned selector into %gs. It then writes sentinel words through %gs and
 * reads them again while the kernel is entered over and over:
 *
 *   1. Solo phase: thousands of system calls plus busy-waits across many
 *      100 Hz timer interrupts. Every %gs:off read must still equal the
 *      sentinel, so the entry and exit paths of both the system call and the
 *      interrupt must save and restore %gs; one that loaded %gs from the
 *      saved %ds fails at once.
 *
 *   2. Isolation phase: fork a child that points its own TLS descriptor at a
 *      different TCB with different sentinels. Parent and child then spin so
 *      the scheduler switches between them hundreds of times, and then hand
 *      a byte back and forth over two pipes, so that each round takes one
 *      switch to the child and one back; each must always read back its own
 *      sentinels. This holds only if the context switch reprograms the
 *      shared TLS GDT descriptor for each process. Both are bound to
 *      processor 0 first: on two processors each could keep one to itself,
 *      and no switch would go from one to the other.
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
 */

#include "probe.h"

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

/* Iteration counts: thousands of system calls in the solo phase, hundreds of
 * switches in the isolation phase. */
#define SOLO_ITERS  40000
#define ISO_ITERS   3000
#define BUSY_SPIN   60000
#define PING_ROUNDS 200

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

/**
 * The solo phase: the sentinels written through %gs survive thousands of
 * system calls and the timer interrupts of busy waits. `fails` is the count
 * of failures so far, and the new count is returned.
 */
static int solo_phase(int fails)
{
    long tv2[2];
    int i;

    tls_fill(0xA5A50000u);
    for (i = 0; i < SOLO_ITERS; i++) {
        sys_gettimeofday(tv2); /* int 0x80: the system call's %gs save */
        if ((i & 0x3FF) == 0) {
            busy(BUSY_SPIN); /* span several timer IRQs */
        }
        if (tls_check(0xA5A50000u) != 0) {
            fails++;
            if (fails <= 3) {
                puts_("FAIL: solo sentinel clobbered at i=");
                put_u32((u32)i);
                puts_("\n");
            }
            break;
        }
    }
    if (fails == 0) {
        puts_("solo phase OK\n");
    }
    return fails;
}

/**
 * The child of the isolation phase: it switches to its own TCB and checks
 * only its own sentinels, across switches and across each round of the byte
 * passed over the pipes. It exits 0 when all were intact, 8 when the base
 * it inherited did not read the parent's sentinels, 7 when it could not
 * set its own, and 1 on a mismatch. Does not return.
 *
 * Before it sets its own TLS, the child still has the base it inherited
 * across fork, which copies the TLS base and the %gs of the trap frame. Its
 * copy of tcb_parent has the parent's sentinels, so a read through the
 * inherited %gs returns them; a fork that dropped the base would make the
 * switch program base 0, and the read would not match.
 */
static void iso_child(const int to_child[2], const int to_parent[2])
{
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

    for (i = 0; i < ISO_ITERS; i++) {
        sys_gettimeofday(tv2);
        busy(BUSY_SPIN);
        if (tls_check(0x5A5A0000u) != 0) {
            cbad++;
            break;
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
 * The parent's switches of the isolation phase: it keeps tcb_parent and
 * checks its sentinels across switches to the child, then across each
 * round of the byte it sends the child and gets back. `fails` is the count
 * of failures so far, and the new count is returned.
 */
static int iso_switches(const int to_child[2], const int to_parent[2], int fails)
{
    long tv2[2];
    char token;
    int i;

    for (i = 0; i < ISO_ITERS; i++) {
        sys_gettimeofday(tv2);
        busy(BUSY_SPIN);
        if (tls_check(0xA5A50000u) != 0) {
            fails++;
            if (fails <= 3) {
                puts_("FAIL: parent sentinel clobbered after switch at i=");
                put_u32((u32)i);
                puts_("\n");
            }
            break;
        }
    }
    token = 'x';
    for (i = 0; i < PING_ROUNDS && fails == 0; i++) {
        if (sys_write(to_child[1], &token, 1) != 1 || sys_read(to_parent[0], &token, 1) != 1) {
            puts_("FAIL: the byte did not come back from the child at round ");
            put_u32((u32)i);
            puts_("\n");
            fails++;
        } else if (tls_check(0xA5A50000u) != 0) {
            puts_("FAIL: parent sentinel clobbered after the child ran, at round ");
            put_u32((u32)i);
            puts_("\n");
            fails++;
        }
    }
    return fails;
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
    fails = iso_switches(to_child, to_parent, fails);
    sys_close(to_child[1]);
    got = sys_waitpid(pid, &status, 0);
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

    (void)sp;

    puts_("tlsprobe: starting\n");

    /* This process's TLS, at tcb_parent. */
    entry = set_thread_area(tcb_parent);
    if (entry < 0) {
        puts_("FAIL: set_thread_area\n");
        sys_exit(1);
    }
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

    fails = solo_phase(fails);

    /* Isolation phase: a child with its own TCB, on the parent's processor. */
    if (syscall3(SYS_SCHED_SETAFFINITY, 0, (long)sizeof(processor0), (long)&processor0) != 0) {
        puts_("FAIL: the probe could not be bound to processor 0\n");
        sys_exit(1);
    }
    if (syscall1(SYS_PIPE, (long)to_child) != 0 || syscall1(SYS_PIPE, (long)to_parent) != 0) {
        puts_("FAIL: pipe\n");
        sys_exit(1);
    }
    pid = sys_fork();
    if (pid < 0) {
        puts_("FAIL: fork\n");
        sys_exit(1);
    }
    if (pid == 0) {
        iso_child(to_child, to_parent);
    }
    fails = iso_parent(pid, to_child, to_parent, fails);

    if (fails == 0) {
        puts_("All TLS checks passed!\n");
    } else {
        puts_("tlsprobe: FAILED\n");
    }
    sys_exit(fails == 0 ? 0 : 1);
}
