/**
 * Verify that %gs thread-local storage survives syscalls, IRQs, and context
 * switches, with per-process isolation.
 *
 * Drives the kernel's set_thread_area(2) exactly as musl's __init_tls does: it
 * points a GDT TLS descriptor at a thread control block (TCB) and loads the
 * returned selector into %gs. It then writes sentinel words through %gs and
 * re-reads them while hammering the kernel:
 *
 *   1. Solo phase: thousands of int 0x80 syscalls plus busy-waits across many
 *      100 Hz timer IRQs. Every %gs:off read must still equal the sentinel, so
 *      the syscall AND IRQ entry/exit paths must save and restore %gs (the old
 *      bug smeared %gs from the saved %ds, which this would catch immediately).
 *
 *   2. Isolation phase: fork a child that points its OWN TLS descriptor at a
 *      DIFFERENT TCB with different sentinels. Parent and child then spin so
 *      the round-robin scheduler switches between them hundreds of times; each
 *      must always read back its own sentinels. This only holds if the context
 *      switch reprograms the shared TLS GDT descriptor per process.
 *
 * Prints "All TLS checks passed!" and exits 0 only if every check passes.
 *
 * The Linux/BSD analogue is set_thread_area(2) plus the i386 %gs thread pointer
 * that glibc/musl, the stack-protector canary, and pthread_self() all rely on.
 *
 * Freestanding; direct int 0x80.
 */

typedef unsigned int u32;
typedef unsigned char u8;

#define SYS_EXIT            1
#define SYS_FORK            2
#define SYS_WRITE           4
#define SYS_WAITPID         7
#define SYS_GETTIMEOFDAY    78
#define SYS_SET_THREAD_AREA 243
#define STDOUT              1

/* user_desc.flags: seg_32bit | limit_in_pages | useable (what musl passes). */
#define UD_FLAGS 0x51

/* Number of sentinel slots checked per TCB (offsets 0,4,...,28). */
#define NUM_SLOTS 8

/* Iteration counts: thousands of syscalls solo, hundreds of switches in iso. */
#define SOLO_ITERS 40000
#define ISO_ITERS  3000
#define BUSY_SPIN  60000

static int sys_write(int fd, const void *buf, u32 n)
{
    int r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "0"(SYS_WRITE), "b"(fd), "c"(buf), "d"(n)
                         : "memory");
    return r;
}

static void sys_exit(int status)
{
    __asm__ __volatile__("int $0x80" : : "a"(SYS_EXIT), "b"(status));
    __builtin_unreachable();
}

static int sys_fork(void)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(SYS_FORK));
    return r;
}

static int sys_waitpid(int pid, int *status, int options)
{
    int r;
    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "0"(SYS_WAITPID), "b"(pid), "c"(status), "d"(options)
                         : "memory");
    return r;
}

/** A cheap, side-effect-free syscall used to exercise the int 0x80 path. */
static void sys_gettimeofday(u32 *tv2)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(SYS_GETTIMEOFDAY), "b"(tv2) : "memory");
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

    desc[0] = (u32)-1;   /* entry_number: allocate */
    desc[1] = (u32)base; /* base_addr */
    desc[2] = 0xfffff;   /* limit */
    desc[3] = UD_FLAGS;  /* flags */

    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(SYS_SET_THREAD_AREA), "b"(desc) : "memory");
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

/** Read/write a 32-bit word at %gs:off (off is a byte offset into the TCB). */
static u32 gs_read(u32 off)
{
    u32 v;

    __asm__ __volatile__("movl %%gs:(%1), %0" : "=r"(v) : "r"(off));
    return v;
}

static void gs_write(u32 off, u32 val)
{
    __asm__ __volatile__("movl %0, %%gs:(%1)" : : "r"(val), "r"(off) : "memory");
}

static u32 slen(const char *s)
{
    u32 n = 0;

    while (s[n]) {
        n++;
    }
    return n;
}

static void puts_(const char *s)
{
    sys_write(STDOUT, s, slen(s));
}

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
 * Re-read every slot through %gs and confirm it still holds magic^slot.
 * Returns the number of mismatches (0 == intact).
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

/** Burn cycles in user mode so timer IRQs (and, with a peer, switches) fire. */
static void busy(u32 spins)
{
    volatile u32 x = 0;
    u32 i;

    for (i = 0; i < spins; i++) {
        x = x + 1;
    }
}

void probe_main(u32 *sp);

__asm__(".global _start\n"
        "_start:\n"
        "   mov %esp, %eax\n"
        "   push %eax\n"
        "   call probe_main\n");

void probe_main(u32 *sp)
{
    int entry;
    int pid;
    int i;
    int fails = 0;
    u32 tv2[2];

    (void)sp;

    puts_("tlsprobe: starting\n");

    /* --- Establish this process's TLS pointing at tcb_parent. --- */
    entry = set_thread_area(tcb_parent);
    if (entry < 0) {
        puts_("FAIL: set_thread_area\n");
        sys_exit(1);
    }
    puts_("set_thread_area entry=");
    put_u32((u32)entry);
    puts_("\n");
    load_gs(entry);

    /* Sanity: %gs:0 must alias tcb_parent[0]. */
    gs_write(0, 0xdeadbeef);
    if (tcb_parent[0] != 0xdeadbeef) {
        puts_("FAIL: %gs does not alias the TCB\n");
        fails++;
    }

    /* --- Solo phase: sentinels survive syscalls + timer IRQs. --- */
    tls_fill(0xA5A50000u);
    for (i = 0; i < SOLO_ITERS; i++) {
        sys_gettimeofday(tv2); /* int 0x80: tests syscall %gs save */
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

    /* --- Isolation phase: a second process with a DIFFERENT TCB. --- */
    pid = sys_fork();
    if (pid < 0) {
        puts_("FAIL: fork\n");
        sys_exit(1);
    }

    if (pid == 0) {
        /* Child: switch to its own TCB and verify only its own sentinels. */
        int centry;
        int cbad = 0;

        /* Before re-arming TLS, the child must still see the base it INHERITED
         * from the parent across fork (fork copies tls_base and the trapframe
         * %gs). Its own copy of tcb_parent holds the parent's sentinels, so a
         * read through the inherited %gs must return them. If fork dropped
         * tls_base the switch would reprogram base 0 and this would
         * mismatch. */
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
        /* Exit status carries the child verdict to the parent (0 == clean). */
        sys_exit(cbad == 0 ? 0 : 1);
    } else {
        /* Parent: keep tcb_parent and verify its sentinels across switches. */
        int status = -1;
        int got;

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
    }

    if (fails == 0) {
        puts_("All TLS checks passed!\n");
    } else {
        puts_("tlsprobe: FAILED\n");
    }
    sys_exit(fails == 0 ? 0 : 1);
}
