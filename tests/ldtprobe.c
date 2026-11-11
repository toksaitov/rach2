/**
 * ldtprobe: modify_ldt(2) and the 32-bit vDSO's __kernel_vsyscall, as an
 * i386 program uses them on Linux's 64-bit kernel.
 *
 * modify_ldt's read of the default LDT must give 128 zero bytes, and its
 * read before any write nothing. A write must refuse what Linux refuses: a
 * size other than a struct user_desc's and an entry past 8191 with EINVAL,
 * a present conforming code segment with EINVAL in both modes, an argument
 * it cannot read with EFAULT, and another function with ENOSYS; it must
 * take a not-present conforming one and a 16-bit segment. A 32-bit data
 * segment written to entry 3 must load into FS and read the block it
 * covers, and the read must give it back as Linux's `fill_ldt` makes it,
 * with the entries around it zero; the old mode must clear the descriptor's
 * AVL bit where the new mode keeps it, and clear the entry for a base and a
 * limit of 0. A far call through a code segment of the LDT must run there,
 * with that CS, make a system call and return.
 *
 * The LDT must hold across switches and fork: a child has a copy of it, in
 * which its own write changes nothing of the parent's, and the parent's FS
 * still reads its block after waiting for the child. A signal's return must
 * put back FS's LDT selector, which the handler cleared. Clearing an entry
 * must leave ES that held it null at once, and FS null after a sleep,
 * whose switch away and back loads FS again, as Linux's fault fixups load
 * it; a child whose far call clears
 * its own code segment must take SIGSEGV at the return, and the kernel
 * must go on.
 *
 * AT_SYSINFO must name __kernel_vsyscall, through which getpid must give
 * the pid and mmap2, with its page offset in EBP, the second page of a
 * memfd. The probe prints which entry __kernel_vsyscall takes, `sysenter`,
 * `syscall` or `int $0x80`, which the x86-64 kernel chooses by the
 * processor's vendor.
 *
 * The probe writes etude's report records (probe_report.h) for the case
 * ETUDE_CASE names, an assertion for each check, as `assertion_id` lists
 * them; a failed check fails its assertion and leaves those after it not
 * run. Prints "LDTPROBE OK" and exits 0, or "LDTPROBE FAIL: " and the
 * reason and exits 1. modify_ldt and __kernel_vsyscall are i386's
 * interfaces, so tests/Makefile builds the probe for i686 alone, and it
 * runs on the x86-64 kernel, which gives an i386 program both. The i686
 * kernel has neither: the probe reports the branch `kernel` of the kernel
 * that ETUDE_KERNEL names, or of the one whose modify_ldt answers where it
 * is unset, and on the i686 kernel checks only `absent`, that modify_ldt
 * gives ENOSYS, and stops with "LDTPROBE SKIP", exit 0.
 */

#include "probe.h"
#include "probe_report.h"

#if defined __x86_64__
#error "ldtprobe tests i386's modify_ldt and vDSO entry; it is built for i686 alone"
#endif

typedef unsigned int u32;

#define SYS_MODIFY_LDT    123
#define SYS_OLD_SETRLIMIT 75
#define SYS_MEMFD_CREATE  356

#define ENOSYS 38
#define EFAULT 14
#define EINVAL 22

#define SIGUSR1     10
#define SIGSEGV     11
#define SA_SIGINFO  0x00000004
#define SA_RESTORER 0x04000000

#define PROT_READ   0x1
#define MAP_PRIVATE 0x02
#define PAGE        4096

#define RLIMIT_CORE 4
#define AT_SYSINFO  32

/* struct user_desc's flags, in the order of Linux's bit-fields. */
#define UD_SEG_32BIT       0x01u
#define UD_CONTENTS_CODE   0x04u /* contents 2: code */
#define UD_CONTENTS_CONF   0x06u /* contents 3: conforming code */
#define UD_READ_EXEC_ONLY  0x08u
#define UD_LIMIT_IN_PAGES  0x10u
#define UD_SEG_NOT_PRESENT 0x20u
#define UD_USEABLE         0x40u

/* The entries the probe writes, and their selectors: TI set, RPL 3. */
#define E_DATA  3
#define E_OLD   5
#define E_CODE  6
#define E_ES    7
#define E_CONF  8
#define E_16BIT 9
#define SEL(e)  (((u32)(e) << 3) | 7u)

/* The descriptor's AVL bit, in its high word. */
#define DESC_HI_AVL 0x00100000u

struct user_desc {
    u32 entry_number;
    u32 base_addr;
    u32 limit;
    u32 flags;
};

/* The assertions, a check each, in the order the probe makes them. */
static const char *const assertion_id[] = {
    "default-read", "empty-read", "refusals",      "data-segment",  "read-back",  "old-mode",
    "far-call",     "fork-copy",  "signal-return", "cleared-entry", "bad-return", "vsyscall"};

/* The block entry E_DATA covers, and the one a child points it at. */
static u32 block[4] = {0x1DD7C0DEu, 0x5E6A4E47u, 0x0BADF00Du, 0x12345678u};
static u32 block2[4] = {0xC0FFEE01u, 0xC0FFEE02u, 0xC0FFEE03u, 0xC0FFEE04u};

/* __kernel_vsyscall, from AT_SYSINFO. */
static unsigned long vsyscall_at;

/* Set by the signal handler. */
static volatile int handled;

/** The length of `s`, which has no C library to count it. */
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

/** Exit with `status`. */
static void sys_exit(int status)
{
    syscall1(SYS_EXIT, status);
    __builtin_unreachable();
}

/** Print "LDTPROBE FAIL: " and `why`, end the report at the assertion that
 * `why` fails, and exit 1. */
static void fail(const char *why)
{
    say("LDTPROBE FAIL: ");
    say(why);
    say("\n");
    report_stop(why);
    sys_exit(1);
}

/** modify_ldt by `int $0x80`: its result, or -errno. */
static long modify_ldt(int func, void *ptr, unsigned long count)
{
    return syscall3(SYS_MODIFY_LDT, func, (long)ptr, (long)count);
}

/** Write entry `e` with `base`, `limit` and `flags` in mode `func`. */
static long ldt_set(int func, u32 e, u32 base, u32 limit, u32 flags)
{
    struct user_desc u;

    u.entry_number = e;
    u.base_addr = base;
    u.limit = limit;
    u.flags = flags;
    return modify_ldt(func, &u, sizeof(u));
}

/** Clear entry `e` with Linux's `LDT_empty` form. */
static long ldt_clear(u32 e)
{
    return ldt_set(0x11, e, 0, 0, UD_READ_EXEC_ONLY | UD_SEG_NOT_PRESENT);
}

/** The selector in FS. */
static u32 fs_read(void)
{
    u32 v = 0;

    __asm__ __volatile__("movw %%fs, %w0" : "+r"(v));
    return v;
}

/** Load `sel` into FS. */
static void fs_load(u32 sel)
{
    __asm__ __volatile__("movw %w0, %%fs" : : "r"(sel));
}

/** The selector in ES. */
static u32 es_read(void)
{
    u32 v = 0;

    __asm__ __volatile__("movw %%es, %w0" : "+r"(v));
    return v;
}

/** Load `sel` into ES. */
static void es_load(u32 sel)
{
    __asm__ __volatile__("movw %w0, %%es" : : "r"(sel));
}

/** The selector in DS. */
static u32 ds_read(void)
{
    u32 v = 0;

    __asm__ __volatile__("movw %%ds, %w0" : "+r"(v));
    return v;
}

/** The word at FS:`off`. */
static u32 fs_word(u32 off)
{
    u32 v;

    __asm__ __volatile__("movl %%fs:(%1), %0" : "=r"(v) : "r"(off));
    return v;
}

/** The two words of the descriptor `fill_ldt` makes of a present, writable
 * 32-bit data segment at `base` with the byte limit `limit`. */
static void data_desc(u32 base, u32 limit, u32 *lo, u32 *hi)
{
    *lo = (base << 16) | (limit & 0xFFFFu);
    *hi =
        ((base >> 16) & 0xFFu) | 0xF300u | (limit & 0xF0000u) | 0x00400000u | (base & 0xFF000000u);
}

/*
 * The code a far call reaches through E_CODE, whose base is 0, so an offset
 * is an address: it reads CS into ECX, makes getpid by `int $0x80`, and
 * returns far. ldt_clear_self makes the modify_ldt whose arguments the
 * caller put in EBX, ECX and EDX, which clears its own segment, and its
 * return from the kernel faults.
 */
__asm__(".pushsection .text\n"
        "ldt_target:\n"
        "    xorl %ecx, %ecx\n"
        "    movw %cs, %cx\n"
        "    movl $20, %eax\n"
        "    int $0x80\n"
        "    lret\n"
        "ldt_clear_self:\n"
        "    movl $123, %eax\n"
        "    int $0x80\n"
        "    lret\n"
        ".popsection\n");
extern char ldt_target[], ldt_clear_self[];

/** Call `fn` far through the selector `sel`: getpid's result, with the CS
 * the code saw in `cs`. */
static long far_call(void *fn, u32 sel, u32 *cs)
{
    u32 fp[2];
    long r;

    fp[0] = (u32)(unsigned long)fn;
    fp[1] = sel;
    __asm__ __volatile__("lcall *(%2)" : "=a"(r), "=c"(*cs) : "r"(fp) : "edx", "memory");
    return r;
}

/** The system call `n` with the six arguments `a` through __kernel_vsyscall,
 * the sixth in EBP, which is saved around it, as `syscall6` makes it by
 * `int $0x80`. */
static long vsys6(long n, const long a[6])
{
    long args[3];
    long r;

    args[0] = a[5];
    args[1] = n;
    args[2] = (long)vsyscall_at;
    __asm__ __volatile__("pushl %%ebp\n\t"
                         "movl (%%eax), %%ebp\n\t"
                         "pushl 8(%%eax)\n\t"
                         "movl 4(%%eax), %%eax\n\t"
                         "call *(%%esp)\n\t"
                         "addl $4, %%esp\n\t"
                         "popl %%ebp"
                         : "=a"(r)
                         : "0"(args), "b"(a[0]), "c"(a[1]), "d"(a[2]), "S"(a[3]), "D"(a[4])
                         : "memory");
    return r;
}

/** Fork a child that runs `fn`, and return its wait status, or -1. */
static int in_child(int (*fn)(void))
{
    int pid = (int)syscall0(SYS_FORK), status = 0;

    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        sys_exit(fn());
    }
    if (syscall3(SYS_WAITPID, pid, (long)&status, 0) != pid) {
        return -1;
    }
    return status;
}

/** The reads of the default LDT and of one never written. */
static void run_reads(void)
{
    unsigned char buf[256];
    unsigned int i;

    for (i = 0; i < sizeof(buf); i++) {
        buf[i] = 0xAA;
    }
    if (modify_ldt(2, buf, sizeof(buf)) != 128) {
        fail("the default LDT's read did not give 128 bytes");
    }
    for (i = 0; i < sizeof(buf); i++) {
        if (buf[i] != (i < 128 ? 0 : 0xAA)) {
            fail("the default LDT's read did not give 128 zero bytes alone");
        }
    }
    report_held();
    if (modify_ldt(0, buf, 64) != 0) {
        fail("the read of an LDT never written did not give 0");
    }
    report_held();
}

/** The writes Linux refuses, and the not-present conforming and 16-bit
 * segments it takes. */
static void run_refusals(void)
{
    struct user_desc u;

    u.entry_number = E_DATA;
    u.base_addr = 0;
    u.limit = 0;
    u.flags = UD_SEG_32BIT;
    if (modify_ldt(0x11, &u, sizeof(u) - 1) != -EINVAL) {
        fail("a write of 15 bytes was not refused with EINVAL");
    }
    if (ldt_set(0x11, 8192, 0, 0xFFFFF, UD_SEG_32BIT) != -EINVAL) {
        fail("entry 8192 was not refused with EINVAL");
    }
    if (ldt_set(0x11, E_CONF, 0, 0xFFFFF, UD_SEG_32BIT | UD_CONTENTS_CONF) != -EINVAL) {
        fail("a present conforming code segment was not refused with EINVAL");
    }
    if (ldt_set(1, E_CONF, 0, 0xFFFFF, UD_SEG_32BIT | UD_CONTENTS_CONF | UD_SEG_NOT_PRESENT) !=
        -EINVAL) {
        fail("the old mode's conforming code segment was not refused with EINVAL");
    }
    if (modify_ldt(0x11, (void *)0x10, sizeof(u)) != -EFAULT) {
        fail("an argument that cannot be read was not refused with EFAULT");
    }
    if (modify_ldt(7, &u, sizeof(u)) != -ENOSYS) {
        fail("function 7 was not refused with ENOSYS");
    }
    if (ldt_set(0x11, E_CONF, 0, 0xFFFFF, UD_SEG_32BIT | UD_CONTENTS_CONF | UD_SEG_NOT_PRESENT) !=
            0 ||
        ldt_set(0x11, E_16BIT, 0, 0xFFFF, 0) != 0) {
        fail("a not-present conforming segment or a 16-bit one was refused");
    }
    if (ldt_clear(E_CONF) != 0 || ldt_clear(E_16BIT) != 0) {
        fail("an entry could not be cleared");
    }
    report_held();
}

/** A 32-bit data segment loaded into FS, and the LDT read back. */
static void run_data(void)
{
    u32 got[8 * 2], lo, hi;
    unsigned int i;

    if (ldt_set(0x11, E_DATA, (u32)(unsigned long)block, sizeof(block) - 1, UD_SEG_32BIT) != 0) {
        fail("the write of a 32-bit data segment failed");
    }
    fs_load(SEL(E_DATA));
    if (fs_read() != SEL(E_DATA) || fs_word(4) != block[1] || fs_word(12) != block[3]) {
        fail("FS on the LDT's data segment did not read its block");
    }
    report_held();
    if (modify_ldt(0, got, sizeof(got)) != (long)sizeof(got)) {
        fail("the read of 8 entries did not give 64 bytes");
    }
    data_desc((u32)(unsigned long)block, sizeof(block) - 1, &lo, &hi);
    for (i = 0; i < 8; i++) {
        u32 want_lo = i == E_DATA ? lo : 0, want_hi = i == E_DATA ? hi : 0;

        if (got[2 * i] != want_lo || got[2 * i + 1] != want_hi) {
            fail("the LDT read back is not fill_ldt's descriptors");
        }
    }
    report_held();
}

/** The AVL bit in both modes, and the old mode's clearing write. */
static void run_old_mode(void)
{
    u32 got[(E_OLD + 1) * 2];

    if (ldt_set(0x11, E_OLD, 0x1000, 0xFF, UD_SEG_32BIT | UD_USEABLE) != 0 ||
        modify_ldt(0, got, sizeof(got)) != (long)sizeof(got) ||
        !(got[2 * E_OLD + 1] & DESC_HI_AVL)) {
        fail("the new mode did not keep the AVL bit");
    }
    if (ldt_set(1, E_OLD, 0x1000, 0xFF, UD_SEG_32BIT | UD_USEABLE) != 0 ||
        modify_ldt(0, got, sizeof(got)) != (long)sizeof(got) || got[2 * E_OLD] == 0 ||
        (got[2 * E_OLD + 1] & DESC_HI_AVL)) {
        fail("the old mode did not clear the AVL bit");
    }
    if (ldt_set(1, E_OLD, 0, 0, UD_SEG_32BIT) != 0 ||
        modify_ldt(0, got, sizeof(got)) != (long)sizeof(got) || got[2 * E_OLD] != 0 ||
        got[2 * E_OLD + 1] != 0) {
        fail("the old mode did not clear an entry of base and limit 0");
    }
    report_held();
}

/** A far call through a code segment of the LDT. */
static void run_far_call(void)
{
    u32 cs = 0;

    if (ldt_set(0x11, E_CODE, 0, 0xFFFFF, UD_SEG_32BIT | UD_CONTENTS_CODE | UD_LIMIT_IN_PAGES) !=
        0) {
        fail("the write of a 32-bit code segment failed");
    }
    if (far_call(ldt_target, SEL(E_CODE), &cs) != syscall0(SYS_GETPID) || cs != SEL(E_CODE)) {
        fail("a far call through the LDT's code segment did not run there");
    }
    report_held();
}

/** The child of the fork check: its copy of entry E_DATA reads the block,
 * and its own write points the entry at block2. */
static int fork_child(void)
{
    if (fs_read() != SEL(E_DATA) || fs_word(4) != block[1]) {
        return 2;
    }
    if (ldt_set(0x11, E_DATA, (u32)(unsigned long)block2, sizeof(block2) - 1, UD_SEG_32BIT) != 0) {
        return 3;
    }
    fs_load(SEL(E_DATA));
    return fs_word(0) == block2[0] ? 0 : 4;
}

/** The child's copy of the LDT, and the parent's across its write. */
static void run_fork(void)
{
    int i;

    if (in_child(fork_child) != 0) {
        fail("the child's copy of the LDT was not the parent's or not its own");
    }
    for (i = 0; i < 100; i++) {
        syscall0(SYS_SCHED_YIELD);
    }
    if (fs_read() != SEL(E_DATA) || fs_word(4) != block[1]) {
        fail("the parent's FS lost its block across the child's write and the switches");
    }
    fs_load(SEL(E_DATA));
    if (fs_word(0) != block[0]) {
        fail("the child's write changed the parent's LDT");
    }
    report_held();
}

PROBE_RT_RESTORER(ldt_restorer);
extern void ldt_restorer(void);

/** The handler of the signal check: clears FS, which the return must put
 * back. */
static void on_usr1(int sig, void *info, void *uc)
{
    (void)sig;
    (void)info;
    (void)uc;
    fs_load(0);
    handled = 1;
}

/** FS's LDT selector across a signal's handler and return. */
static void run_signal(void)
{
    struct {
        void (*handler)(int, void *, void *);
        unsigned long flags;
        void (*restorer)(void);
        u32 mask[2];
    } sa;

    sa.handler = on_usr1;
    sa.flags = SA_SIGINFO | SA_RESTORER;
    sa.restorer = ldt_restorer;
    sa.mask[0] = 0;
    sa.mask[1] = 0;
    if (syscall4(SYS_RT_SIGACTION, SIGUSR1, (long)&sa, 0, 8) != 0) {
        fail("rt_sigaction failed");
    }
    fs_load(SEL(E_DATA));
    syscall2(SYS_KILL, syscall0(SYS_GETPID), SIGUSR1);
    if (!handled || fs_read() != SEL(E_DATA) || fs_word(8) != block[2]) {
        fail("the signal's return did not put back FS's LDT selector");
    }
    report_held();
}

/** Sleep 10 ms, which takes the processor from the probe and gives it
 * back, through a switch each way. */
static void nap(void)
{
    long ts[2];

    ts[0] = 0;
    ts[1] = 10000000;
    /* interval: a nap of 10 ms switches the probe out and back in */
    syscall2(SYS_NANOSLEEP, (long)ts, 0);
}

/** ES and FS after their entries are cleared, at once and after a
 * switch. */
static void run_cleared(void)
{
    u32 ds = ds_read();

    if (ldt_set(0x11, E_ES, (u32)(unsigned long)block, sizeof(block) - 1, UD_SEG_32BIT) != 0) {
        fail("the write of ES's data segment failed");
    }
    es_load(SEL(E_ES));
    if (ldt_clear(E_ES) != 0) {
        fail("ES's entry could not be cleared");
    }
    if (es_read() != 0) {
        es_load(ds);
        fail("ES still held the selector of its cleared entry");
    }
    es_load(ds);
    fs_load(SEL(E_DATA));
    if (ldt_clear(E_DATA) != 0) {
        fail("FS's entry could not be cleared");
    }
    nap();
    if (fs_read() != 0) {
        fail("FS still held the selector of its cleared entry after a sleep");
    }
    report_held();
}

/** The child of the bad return: no core file, and a far call whose code
 * clears its own segment through `int $0x80`, whose return faults. */
static int bad_child(void)
{
    u32 lim[2] = {0, 0};
    struct user_desc u;
    u32 fp[2];

    syscall2(SYS_OLD_SETRLIMIT, RLIMIT_CORE, (long)lim);
    u.entry_number = E_CODE;
    u.base_addr = 0;
    u.limit = 0;
    u.flags = UD_READ_EXEC_ONLY | UD_SEG_NOT_PRESENT;
    fp[0] = (u32)(unsigned long)ldt_clear_self;
    fp[1] = SEL(E_CODE);
    __asm__ __volatile__("lcall *(%0)"
                         :
                         : "r"(fp), "b"(0x11), "c"(&u), "d"(sizeof(u))
                         : "eax", "memory");
    return 5;
}

/** A child's return to the code segment it cleared ends in SIGSEGV. */
static void run_bad_return(void)
{
    int status = in_child(bad_child);

    if (status < 0 || (status & 0x7F) != SIGSEGV) {
        fail("the return to a cleared code segment did not end the child with SIGSEGV");
    }
    report_held();
}

/** getpid and mmap2 through __kernel_vsyscall, and the entry it takes. */
static void run_vsyscall(void)
{
    const unsigned char *entry;
    long fd, page, args[6] = {0, 0, 0, 0, 0, 0};
    u32 buf[PAGE / 4];
    unsigned int i;

    if (vsyscall_at == 0) {
        fail("the auxiliary vector has no AT_SYSINFO");
    }
    entry = (const unsigned char *)vsyscall_at;
    if (entry[5] == 0x0F && entry[6] == 0x34) {
        say("ldtprobe: __kernel_vsyscall enters by sysenter\n");
    } else if (entry[5] == 0x0F && entry[6] == 0x05) {
        say("ldtprobe: __kernel_vsyscall enters by syscall\n");
    } else {
        say("ldtprobe: __kernel_vsyscall enters by int $0x80\n");
    }
    if (vsys6(SYS_GETPID, args) != syscall0(SYS_GETPID)) {
        fail("getpid through __kernel_vsyscall did not give the pid");
    }
    fd = syscall2(SYS_MEMFD_CREATE, (long)"ldtprobe", 0);
    if (fd < 0) {
        fail("memfd_create failed");
    }
    for (i = 0; i < PAGE / 4; i++) {
        buf[i] = 0x11110000u + i;
    }
    if (syscall3(SYS_WRITE, fd, (long)buf, PAGE) != PAGE) {
        fail("the memfd's first page could not be written");
    }
    for (i = 0; i < PAGE / 4; i++) {
        buf[i] = 0x22220000u + i;
    }
    if (syscall3(SYS_WRITE, fd, (long)buf, PAGE) != PAGE) {
        fail("the memfd's second page could not be written");
    }
    args[1] = PAGE;
    args[2] = PROT_READ;
    args[3] = MAP_PRIVATE;
    args[4] = fd;
    args[5] = 1;
    page = vsys6(SYS_MMAP2, args);
    if (sys_failed(page) || ((u32 *)page)[0] != 0x22220000u || ((u32 *)page)[5] != 0x22220005u) {
        fail("mmap2 through __kernel_vsyscall did not take its page offset from EBP");
    }
    syscall2(SYS_MUNMAP, page, PAGE);
    syscall1(SYS_CLOSE, fd);
    report_held();
}

/** Whether the kernel is x86-64's: as ETUDE_KERNEL in the environment
 * `envp` names it, or where it is unset, whether modify_ldt answers. */
static int kernel_x86_64(char **envp)
{
    static const char name[] = "ETUDE_KERNEL=";
    unsigned char buf[16];
    int i, n;

    for (i = 0; envp[i] != 0; i++) {
        for (n = 0; name[n] != '\0' && envp[i][n] == name[n]; n++) {
        }
        if (name[n] == '\0') {
            return envp[i][n] == 'x';
        }
    }
    return modify_ldt(2, buf, sizeof(buf)) != -ENOSYS;
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/** The probe: AT_SYSINFO from the initial stack, then the checks in
 * order. */
void probe_main(unsigned long *sp)
{
    char **envp = (char **)(sp + 1) + sp[0] + 1;
    unsigned char none[16];
    unsigned long *aux;
    unsigned int i;

    for (aux = (unsigned long *)envp; *aux; aux++) {
    }
    for (aux++; aux[0] != 0; aux += 2) {
        if (aux[0] == AT_SYSINFO) {
            vsyscall_at = aux[1];
        }
    }
    report_begin(report_case(envp, "guest.ia32.ldtprobe"));
    if (!kernel_x86_64(envp)) {
        report_branch("kernel", "i686");
        report_plan("absent");
        if (modify_ldt(2, none, sizeof(none)) != -ENOSYS) {
            fail("the i686 kernel's modify_ldt did not fail with ENOSYS");
        }
        report_held();
        report_end();
        say("LDTPROBE SKIP: the kernel has no modify_ldt\n");
        sys_exit(0);
    }
    report_branch("kernel", "x86-64");
    for (i = 0; i < sizeof(assertion_id) / sizeof(assertion_id[0]); i++) {
        report_plan(assertion_id[i]);
    }
    run_reads();
    run_refusals();
    run_data();
    run_old_mode();
    run_far_call();
    run_fork();
    run_signal();
    run_cleared();
    run_bad_return();
    run_vsyscall();
    report_end();
    say("LDTPROBE OK\n");
    sys_exit(0);
}
