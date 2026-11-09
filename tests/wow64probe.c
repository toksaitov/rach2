/**
 * wow64probe: 32-bit code inside a 64-bit process on the x86-64 kernel, and
 * the i386 system calls it makes by `int $0x80`.
 *
 * A far return to selector 0x23, the GDT's 32-bit user code segment, must
 * run the code it names in 32-bit mode, and a far jump to 0x33 must bring it
 * back to 64-bit mode. `int $0x80` must reach the compat table of i386 calls,
 * by i386 numbers, from 64-bit code and from 32-bit code alike, and a number
 * with no handler must return ENOSYS. set_thread_area with entry_number -1
 * must take the first free TLS entry, 12, whose selector, 0x63, loaded into
 * GS must address the block it names from 32-bit and from 64-bit code, and
 * keep addressing it after the process has waited for a child that set and
 * used an entry 12 of its own;
 * get_thread_area must read the entry back. A signal taken in 32-bit code
 * must run its 64-bit handler with the interrupted CS, 0x23, and SS in the
 * frame, UC_SIGCONTEXT_SS set and UC_STRICT_RESTORE_SS clear, and its return
 * must resume the 32-bit code. A seccomp filter that kills the i386 getpid,
 * judging the architecture and the number, must leave the x86-64 call of the
 * same number alone and kill the process that makes the i386 one with
 * SIGSYS. PTRACE_GET_SYSCALL_INFO must report the architecture of each call
 * a tracee makes, AUDIT_ARCH_I386 for `int $0x80`; like Linux, it reports a
 * system call stop only with PTRACE_O_TRACESYSGOOD.
 *
 * Everything the 32-bit code touches lies below 4 GiB: the probe is linked
 * at 0x400000, and its 32-bit stack and the buffers it passes to i386 calls
 * are static.
 *
 * The probe is built for x86-64 alone. It writes etude's report records
 * (probe_report.h) for the case ETUDE_CASE names, an assertion for each
 * check, as `assertion_id` lists them; a failed check fails its assertion
 * and leaves those after it not run. A child writes no record.
 *
 * Prints "WOW64PROBE OK" and exits 0, or "WOW64PROBE FAIL: " and the reason
 * and exits 1.
 */

#include "probe.h"
#include "probe_report.h"

#define GDT_USER_CODE32 0x23
#define GDT_USER_DATA   0x2B
#define GDT_USER_CODE   0x33
#define GDT_TLS_MIN     12
#define TLS_SELECTOR    ((GDT_TLS_MIN << 3) | 3)

/* The i386 numbers the probe makes by `int $0x80`. */
#define I386_GETPID          20
#define I386_KILL            37
#define I386_NO_CALL         222
#define I386_SET_THREAD_AREA 243
#define I386_GET_THREAD_AREA 244

#define SYS_PRCTL   157
#define SYS_SECCOMP 317

#define ENOSYS  38
#define SIGUSR1 10
#define SIGSTOP 19
#define SIGSYS  31

#define SA_SIGINFO  0x00000004ul
#define SA_RESTORER 0x04000000ul

/* uc_flags bits, and the offsets in Linux x86-64's ucontext of uc_flags,
 * the saved RIP and the word that holds CS in bits 0-15 and SS in 48-63. */
#define UC_SIGCONTEXT_SS     0x2
#define UC_STRICT_RESTORE_SS 0x4
#define UC_FLAGS             0
#define UC_RIP               168
#define UC_CSGSFS            184

#define PR_SET_NO_NEW_PRIVS      38
#define SECCOMP_SET_MODE_FILTER  1
#define SECCOMP_RET_KILL_PROCESS 0x80000000u
#define SECCOMP_RET_ALLOW        0x7fff0000u
#define AUDIT_ARCH_I386          0x40000003u
#define AUDIT_ARCH_X86_64        0xC000003Eu
#define BPF_LD_W_ABS             0x20
#define BPF_JEQ_K                0x15
#define BPF_RET_K                0x06
#define SECCOMP_DATA_NR          0
#define SECCOMP_DATA_ARCH        4

#define PTRACE_TRACEME            0
#define PTRACE_SYSCALL            24
#define PTRACE_SETOPTIONS         0x4200
#define PTRACE_O_TRACESYSGOOD     1
#define PTRACE_GET_SYSCALL_INFO   0x420E
#define PTRACE_SYSCALL_INFO_ENTRY 1

/* The x86-64 exit call, whose entry stop the tracer looks for. */
#define X86_64_EXIT 60

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

/* The assertions, a check each, in the order the probe makes them. */
static const char *const assertion_id[] = {"far-call",
                                           "int80-from-64",
                                           "int80-from-32",
                                           "int80-enosys",
                                           "set-thread-area",
                                           "tls-from-32",
                                           "tls-from-64",
                                           "tls-after-child",
                                           "get-thread-area",
                                           "signal-frame",
                                           "signal-return",
                                           "seccomp-native-kept",
                                           "seccomp-i386-killed",
                                           "ptrace-arch"};

/** Print "WOW64PROBE FAIL: " and `why`, end the report at the assertion that
 * `why` fails, and exit 1. */
static void fail(const char *why)
{
    say("WOW64PROBE FAIL: ");
    say(why);
    say("\n");
    report_stop(why);
    sys_exit(1);
}

/** Linux's `struct user_desc` of set_thread_area. */
struct user_desc {
    unsigned int entry_number;
    unsigned int base_addr;
    unsigned int limit;
    unsigned int flags; /* seg_32bit 0x01, limit_in_pages 0x10, useable 0x40 */
};

#define UD_FLAGS 0x51 /* 32-bit, limit in pages, useable, present */

struct k_sigaction {
    void (*handler)(int, void *, void *);
    unsigned long flags;
    void (*restorer)(void);
    unsigned mask[2];
};

/* A classic BPF instruction, as seccomp takes it. */
struct sock_filter {
    unsigned short code;
    unsigned char jt;
    unsigned char jf;
    unsigned int k;
};

struct sock_fprog {
    unsigned short len;
    struct sock_filter *filter;
};

/* The 32-bit code's stack, and what it passes to the i386 calls. */
static unsigned char stack32[16384] __attribute__((aligned(16)));
static struct user_desc desc;
static unsigned int tls_block[4] = {0x5A5A1234u};
static unsigned int child_block[4] = {0x6B6B5678u};

/* What the signal handler saw: uc_flags, the saved RIP and CS/SS word. */
static volatile unsigned long seen_flags, seen_rip, seen_csgsfs;
static volatile int handled;

/* The RSP of the 64-bit code while the 32-bit code runs. */
unsigned long saved_rsp;

/*
 * call32(entry, stack_top, arg) runs the 32-bit code at `entry` on the stack
 * at `stack_top`, with `arg` in EDX, and returns the EAX that code ends with.
 * It enters by a far return to GDT_USER_CODE32; the 32-bit code comes back by
 * a far jump to `back64` in GDT_USER_CODE, which restores the callee-saved
 * registers, whose upper halves 32-bit code may not keep.
 *
 * mode32 returns CS, with 0x10000 added when 0x48 decremented EAX, which it
 * does in 32-bit code and not in 64-bit code, where it is a REX prefix.
 * getpid32 and nocall32 make i386 calls; set32 makes set_thread_area with the
 * user_desc at EDX; tls32 loads the selector in EDX into GS and returns the
 * word at GS:0; kill32 sends SIGUSR1 to the process EDX names and returns
 * kill's result once the handler has returned to it.
 */
long call32(void *entry, void *stack_top, unsigned long arg);
extern char mode32[], getpid32[], nocall32[], set32[], tls32[], kill32[], kill32_int80[];

__asm__(".text\n"
        ".code64\n"
        ".globl call32\n"
        "call32:\n"
        "    pushq %rbx\n"
        "    pushq %rbp\n"
        "    pushq %r12\n"
        "    pushq %r13\n"
        "    pushq %r14\n"
        "    pushq %r15\n"
        "    movq %rsp, saved_rsp(%rip)\n"
        "    movq %rsi, %rsp\n"
        "    pushq $0x23\n"
        "    pushq %rdi\n"
        "    lretq\n"
        "back64:\n"
        "    movq saved_rsp(%rip), %rsp\n"
        "    movslq %eax, %rax\n"
        "    popq %r15\n"
        "    popq %r14\n"
        "    popq %r13\n"
        "    popq %r12\n"
        "    popq %rbp\n"
        "    popq %rbx\n"
        "    ret\n"
        ".code32\n"
        ".globl mode32\n"
        "mode32:\n"
        "    xorl %eax, %eax\n"
        "    .byte 0x48\n"
        "    nop\n"
        "    andl $0x10000, %eax\n"
        "    movl %cs, %ecx\n"
        "    movzwl %cx, %ecx\n"
        "    orl %ecx, %eax\n"
        "    ljmp $0x33, $back64\n"
        ".globl getpid32\n"
        "getpid32:\n"
        "    movl $20, %eax\n"
        "    int $0x80\n"
        "    ljmp $0x33, $back64\n"
        ".globl nocall32\n"
        "nocall32:\n"
        "    movl $222, %eax\n"
        "    int $0x80\n"
        "    ljmp $0x33, $back64\n"
        ".globl set32\n"
        "set32:\n"
        "    movl %edx, %ebx\n"
        "    movl $243, %eax\n"
        "    int $0x80\n"
        "    ljmp $0x33, $back64\n"
        ".globl tls32\n"
        "tls32:\n"
        "    movw %dx, %gs\n"
        "    movl %gs:0, %eax\n"
        "    ljmp $0x33, $back64\n"
        ".globl kill32\n"
        "kill32:\n"
        "    movl %edx, %ebx\n"
        "    movl $10, %ecx\n"
        "    movl $37, %eax\n"
        ".globl kill32_int80\n"
        "kill32_int80:\n"
        "    int $0x80\n"
        "    ljmp $0x33, $back64\n"
        ".code64\n");

/** The i386 call `nr` by `int $0x80` from 64-bit code, with three arguments,
 * each below 4 GiB; the result as the 32-bit EAX gives it. */
static int int80(long nr, unsigned long a, unsigned long b, unsigned long c)
{
    long r;

    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "a"(nr), "b"(a), "c"(b), "d"(c)
                         : "memory", "r8", "r9", "r10", "r11");
    return (int)r;
}

/** Run the 32-bit code at `entry` with `arg` in EDX. */
static long run32(void *entry, unsigned long arg)
{
    return call32(entry, stack32 + sizeof(stack32), arg);
}

/** The SIGUSR1 handler: records what its frame saved. */
static void on_usr1(int sig, void *info, void *ucontext)
{
    unsigned char *uc = ucontext;

    (void)sig;
    (void)info;
    seen_flags = *(unsigned long *)(uc + UC_FLAGS);
    seen_rip = *(unsigned long *)(uc + UC_RIP);
    seen_csgsfs = *(unsigned long *)(uc + UC_CSGSFS);
    handled++;
}

void sig_restorer(void);
PROBE_RT_RESTORER(sig_restorer);

/** The status of child `pid` once it ends; fail when it cannot be reaped. */
static int child_status(long pid)
{
    int status = 0;

    if (syscall3(SYS_WAITPID, pid, (long)&status, 0) != pid) {
        fail("a child could not be reaped");
    }
    return status;
}

/** The far call into 32-bit code, and the i386 calls by `int $0x80`. */
static void run_entry(void)
{
    long pid = syscall0(SYS_GETPID);

    if (run32(mode32, 0) != (0x10000 | GDT_USER_CODE32)) {
        fail("code entered by a far return to 0x23 did not run in 32-bit mode with CS 0x23");
    }
    report_held();
    if (int80(I386_GETPID, 0, 0, 0) != pid) {
        fail("int $0x80 with the i386 getpid number from 64-bit code did not return the pid");
    }
    report_held();
    if (run32(getpid32, 0) != pid) {
        fail("int $0x80 with the i386 getpid number from 32-bit code did not return the pid");
    }
    report_held();
    if (int80(I386_NO_CALL, 0, 0, 0) != -ENOSYS || run32(nocall32, 0) != -ENOSYS) {
        fail("an i386 number with no handler did not return ENOSYS");
    }
    report_held();
}

/** set_thread_area's entry, loaded into GS from both modes, and read back. */
static void run_tls(void)
{
    unsigned int word;
    long pid;
    static struct user_desc back, child_desc;

    desc.entry_number = (unsigned int)-1;
    desc.base_addr = (unsigned int)(unsigned long)tls_block;
    desc.limit = 0xFFFFF;
    desc.flags = UD_FLAGS;
    if (run32(set32, (unsigned long)&desc) != 0 || desc.entry_number != GDT_TLS_MIN) {
        fail("set_thread_area with entry_number -1 did not take entry 12");
    }
    report_held();
    if ((unsigned int)run32(tls32, TLS_SELECTOR) != tls_block[0]) {
        fail("32-bit code did not read the block through selector 0x63 in GS");
    }
    report_held();
    __asm__ __volatile__("movw %w1, %%gs\n\tmovl %%gs:0, %0"
                         : "=r"(word)
                         : "r"(TLS_SELECTOR)
                         : "memory");
    if (word != tls_block[0]) {
        fail("64-bit code did not read the block through selector 0x63 in GS");
    }
    report_held();
    pid = syscall0(SYS_FORK);
    if (pid < 0) {
        fail("fork");
    }
    if (pid == 0) {
        child_desc.entry_number = GDT_TLS_MIN;
        child_desc.base_addr = (unsigned int)(unsigned long)child_block;
        child_desc.limit = 0xFFFFF;
        child_desc.flags = UD_FLAGS;
        sys_exit(run32(set32, (unsigned long)&child_desc) != 0 ||
                 (unsigned int)run32(tls32, TLS_SELECTOR) != child_block[0]);
    }
    if (child_status(pid) != 0) {
        fail("a child did not read its own block through its entry 12");
    }
    __asm__ __volatile__("movl %%gs:0, %0" : "=r"(word) : : "memory");
    if (word != tls_block[0] || (unsigned int)run32(tls32, TLS_SELECTOR) != tls_block[0]) {
        fail("selector 0x63 no longer addressed the block after a child set its own entry 12");
    }
    report_held();
    back.entry_number = GDT_TLS_MIN;
    if (int80(I386_GET_THREAD_AREA, (unsigned long)&back, 0, 0) != 0 ||
        back.base_addr != desc.base_addr || back.limit != desc.limit || back.flags != UD_FLAGS) {
        fail("get_thread_area did not read entry 12 back");
    }
    report_held();
}

/** A signal taken in 32-bit code, its frame, and its return there. */
static void run_signal(void)
{
    struct k_sigaction ka;
    long r;

    ka.handler = on_usr1;
    ka.flags = SA_SIGINFO | SA_RESTORER;
    ka.restorer = sig_restorer;
    ka.mask[0] = 0;
    ka.mask[1] = 0;
    if (syscall4(SYS_RT_SIGACTION, SIGUSR1, (long)&ka, 0, 8) != 0) {
        fail("rt_sigaction");
    }
    r = run32(kill32, (unsigned long)syscall0(SYS_GETPID));
    if (handled != 1 || (seen_csgsfs & 0xFFFF) != GDT_USER_CODE32 ||
        (seen_csgsfs >> 48) != GDT_USER_DATA || !(seen_flags & UC_SIGCONTEXT_SS) ||
        (seen_flags & UC_STRICT_RESTORE_SS) || seen_rip != (unsigned long)kill32_int80 + 2) {
        fail("the frame of a signal taken in 32-bit code did not save CS 0x23, SS and its RIP "
             "with UC_SIGCONTEXT_SS alone");
    }
    report_held();
    if (r != 0) {
        fail("the return from the handler did not resume the 32-bit code");
    }
    report_held();
}

/** A seccomp filter that kills the i386 getpid and allows the rest. */
static void run_seccomp(void)
{
    static struct sock_filter code[] = {
        {BPF_LD_W_ABS, 0, 0, SECCOMP_DATA_ARCH},     {BPF_JEQ_K, 0, 3, AUDIT_ARCH_I386},
        {BPF_LD_W_ABS, 0, 0, SECCOMP_DATA_NR},       {BPF_JEQ_K, 0, 1, I386_GETPID},
        {BPF_RET_K, 0, 0, SECCOMP_RET_KILL_PROCESS}, {BPF_RET_K, 0, 0, SECCOMP_RET_ALLOW}};
    struct sock_fprog prog;
    long pid;
    int status;

    pid = syscall0(SYS_FORK);
    if (pid < 0) {
        fail("fork");
    }
    if (pid == 0) {
        prog.len = sizeof(code) / sizeof(code[0]);
        prog.filter = code;
        if (syscall5(SYS_PRCTL, PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 ||
            syscall3(SYS_SECCOMP, SECCOMP_SET_MODE_FILTER, 0, (long)&prog) != 0) {
            sys_exit(2);
        }
        /* x86-64's 20 is writev; with no vectors it writes nothing. */
        if (syscall3(SYS_WRITEV, 1, 0, 0) != 0) {
            sys_exit(3);
        }
        int80(I386_GETPID, 0, 0, 0);
        sys_exit(4);
    }
    status = child_status(pid);
    if (status == (2 << 8) || status == (3 << 8)) {
        fail("the filter was not installed, or killed the x86-64 call of number 20");
    }
    report_held();
    if ((status & 0x7F) != SIGSYS) {
        fail("the filter did not kill the process at the i386 getpid with SIGSYS");
    }
    report_held();
}

/** PTRACE_GET_SYSCALL_INFO's architecture for an i386 and an x86-64 call. */
static void run_ptrace(void)
{
    static unsigned char info[88];
    int saw_i386 = 0, saw_x86_64 = 0, status, i;
    long pid;

    pid = syscall0(SYS_FORK);
    if (pid < 0) {
        fail("fork");
    }
    if (pid == 0) {
        syscall4(SYS_PTRACE, PTRACE_TRACEME, 0, 0, 0);
        syscall2(SYS_KILL, syscall0(SYS_GETPID), SIGSTOP);
        int80(I386_GETPID, 0, 0, 0);
        sys_exit(0);
    }
    status = 0;
    if (syscall3(SYS_WAITPID, pid, (long)&status, 0) != pid || (status & 0xFF) != 0x7F ||
        syscall4(SYS_PTRACE, PTRACE_SETOPTIONS, pid, 0, PTRACE_O_TRACESYSGOOD) != 0) {
        fail("a traced child did not stop, or did not take PTRACE_O_TRACESYSGOOD");
    }
    for (i = 0; i < 16; i++) {
        unsigned int arch;
        unsigned long nr;

        if (syscall4(SYS_PTRACE, PTRACE_SYSCALL, pid, 0, 0) != 0 ||
            syscall3(SYS_WAITPID, pid, (long)&status, 0) != pid || (status & 0xFF) != 0x7F) {
            break;
        }
        if (syscall4(SYS_PTRACE, PTRACE_GET_SYSCALL_INFO, pid, sizeof(info), (long)info) <= 0 ||
            info[0] != PTRACE_SYSCALL_INFO_ENTRY) {
            continue;
        }
        arch = *(unsigned int *)(info + 4);
        nr = *(unsigned long *)(info + 24);
        if (arch == AUDIT_ARCH_I386 && nr == I386_GETPID) {
            saw_i386 = 1;
        }
        if (arch == AUDIT_ARCH_X86_64 && nr == X86_64_EXIT) {
            saw_x86_64 = 1;
        }
    }
    if ((status & 0xFF) == 0x7F) {
        syscall2(SYS_KILL, pid, 9);
        child_status(pid);
    }
    if (!saw_i386 || !saw_x86_64) {
        fail("PTRACE_GET_SYSCALL_INFO did not report AUDIT_ARCH_I386 for int $0x80 and "
             "AUDIT_ARCH_X86_64 for syscall");
    }
    report_held();
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/** The probe: the rows in order. */
void probe_main(unsigned long *sp)
{
    unsigned int i;

    report_begin(report_case((char **)(sp + 1) + sp[0] + 1, "guest.default.wow64probe"));
    for (i = 0; i < sizeof(assertion_id) / sizeof(assertion_id[0]); i++) {
        report_plan(assertion_id[i]);
    }
    run_entry();
    run_tls();
    run_signal();
    run_seccomp();
    run_ptrace();
    report_end();
    say("WOW64PROBE OK\n");
    sys_exit(0);
}
