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
 * must resume the 32-bit code. A handler that the i386 rt_sigaction installs
 * with SA_ONSTACK, on an alternate stack that the i386 sigaltstack sets,
 * must run in 32-bit code on that stack with Linux's i386 rt frame: the
 * signal in EAX and on the stack, the stack 4 bytes past a 16-byte boundary,
 * the i386 siginfo and the ucontext with the interrupted EIP and CS, 0x23;
 * its return through the i386 rt_sigreturn must resume the 32-bit code and
 * leave the alternate stack set. A seccomp filter that kills the i386 getpid,
 * judging the architecture and the number, must leave the x86-64 call of the
 * same number alone and kill the process that makes the i386 one with
 * SIGSYS. PTRACE_GET_SYSCALL_INFO must report the architecture of each call
 * a tracee makes, AUDIT_ARCH_I386 for `int $0x80`; like Linux, it reports a
 * system call stop only with PTRACE_O_TRACESYSGOOD. A tracer must read -1
 * in orig_rax at the stops of an `int3` in 64-bit code and in 32-bit code
 * and of a write to address 0, which are no system call, and the call's
 * number at a system call's entry stop.
 *
 * An mmap with MAP_32BIT must place the mapping between 1 GiB and 2 GiB,
 * also when its hint is above 2 GiB, and must take a free hint below 2 GiB;
 * without the flag, a free hint below 4 GiB must be taken as it is. The
 * i386 futex_waitv, number 449, made by `int $0x80` with the same structure
 * and 64-bit deadline, must fail with EAGAIN for a word that differs and
 * with ETIMEDOUT for a deadline that has passed, and a child's wake of the
 * third of three words in a shared page that MAP_32BIT placed must end it
 * with index 2. The i386 ioctl's SIOCGSTAMP and SIOCGSTAMPNS, old, on a UDP
 * socket must fail with ENOENT before its first receive and give the time
 * of the datagram received after it in two 32-bit words, an i386
 * `old_timeval32` and `old_timespec32`, with the seconds of the 64-bit
 * SIOCGSTAMP_NEW.
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
#define I386_IOCTL           54
#define I386_KILL            37
#define I386_RT_SIGACTION    174
#define I386_SIGALTSTACK     186
#define I386_NO_CALL         222
#define I386_SET_THREAD_AREA 243
#define I386_GET_THREAD_AREA 244
#define I386_FUTEX_WAITV     449

#define SYS_PRCTL   157
#define SYS_SECCOMP 317

#define ENOSYS    38
#define EAGAIN    11
#define ETIMEDOUT 110
#define SIGUSR1   10
#define SIGSEGV   11
#define SIGTRAP   5
#define SIGSTOP   19
#define SIGSYS    31

#define SA_SIGINFO  0x00000004ul
#define SA_RESTORER 0x04000000ul
#define SA_ONSTACK  0x08000000ul

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
#define PTRACE_PEEKUSER           3
#define PTRACE_CONT               7
#define PTRACE_SYSCALL            24
#define PTRACE_SETOPTIONS         0x4200
#define PTRACE_O_TRACESYSGOOD     1
#define PTRACE_GET_SYSCALL_INFO   0x420E
#define PTRACE_SYSCALL_INFO_ENTRY 1

/* mmap's protections and flags, MAP_32BIT x86-64's, and where it places. */
#define PROT_READ     0x1
#define PROT_WRITE    0x2
#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20
#define MAP_32BIT     0x40
#define LOW_1G        0x40000000ul
#define LOW_2G        0x80000000ul
#define PAGE          4096ul

#define SIOCGSTAMP_OLD   0x8906ul
#define SIOCGSTAMPNS_OLD 0x8907ul
#define SIOCGSTAMP_NEW   0x80108906ul
#define ENOENT           2

#define FUTEX_WAKE      1
#define FUTEX2_SIZE_U32 0x02
#define CLOCK_MONOTONIC 1

/* The x86-64 exit call, whose entry stop the tracer looks for. */
#define X86_64_EXIT 60

/** End the process with `status`; it does not return. */
static void sys_exit(int status) __attribute__((noreturn));
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
                                           "ia32-altstack-frame",
                                           "ia32-altstack-return",
                                           "sleep-remainder",
                                           "seccomp-native-kept",
                                           "seccomp-i386-killed",
                                           "ptrace-arch",
                                           "orig-rax",
                                           "map32bit",
                                           "map32bit-hint",
                                           "low-hint",
                                           "futexv-int80",
                                           "sockstamp-int80",
                                           "directory-copy",
                                           "pselect-null-mask",
                                           "vector-flags",
                                           "range-sync"};

/** Print "WOW64PROBE FAIL: " and `why`, end the report at the assertion that
 * `why` fails, and exit 1. */
static void fail(const char *why) __attribute__((noreturn));
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

/* The i386 handler's alternate stack, and what it saw, by the indexes of
 * H32_*: EAX at entry, the signal on its stack, its ESP, its CS, and from
 * the frame uc_stack's flags, the saved EIP and CS and the siginfo's signal
 * and pid. Not static, since the 32-bit code stores to it by name. */
static unsigned char alt32[16384] __attribute__((aligned(16)));
volatile unsigned int h32[9];
enum { H32_EAX, H32_SIG, H32_ESP, H32_CS, H32_SSFLAGS, H32_EIP, H32_UCCS, H32_SIGNO, H32_PID };

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
 *
 * int3_32 executes `int3` in 32-bit code. handler32 is an i386 SA_SIGINFO
 * handler, which records into h32 what it
 * finds and returns to its restorer, restorer32, which makes the i386
 * rt_sigreturn.
 */
long call32(void *entry, void *stack_top, unsigned long arg);
extern char mode32[], getpid32[], nocall32[], set32[], tls32[], kill32[], kill32_int80[];
extern char handler32[], restorer32[], int3_32[];

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
        ".globl handler32\n"
        "handler32:\n"
        "    movl %eax, h32\n"
        "    movl 4(%esp), %eax\n"
        "    movl %eax, h32+4\n"
        "    movl %esp, h32+8\n"
        "    movl %cs, %eax\n"
        "    movzwl %ax, %eax\n"
        "    movl %eax, h32+12\n"
        "    movl 12(%esp), %eax\n"
        "    movl 12(%eax), %ecx\n"
        "    movl %ecx, h32+16\n"
        "    movl 76(%eax), %ecx\n"
        "    movl %ecx, h32+20\n"
        "    movl 80(%eax), %ecx\n"
        "    movl %ecx, h32+24\n"
        "    movl 8(%esp), %eax\n"
        "    movl (%eax), %ecx\n"
        "    movl %ecx, h32+28\n"
        "    movl 12(%eax), %ecx\n"
        "    movl %ecx, h32+32\n"
        "    ret\n"
        ".globl int3_32\n"
        "int3_32:\n"
        "    int3\n"
        "    ljmp $0x33, $back64\n"
        ".globl restorer32\n"
        "restorer32:\n"
        "    movl $173, %eax\n"
        "    int $0x80\n"
        "    hlt\n"
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

/** The i386 call `nr` by `int $0x80` from 64-bit code, with five
 * arguments, each below 4 GiB; the result as the 32-bit EAX gives it. */
static int int80_5(long nr, unsigned long a, unsigned long b, unsigned long c, unsigned long d,
                   unsigned long e)
{
    long r;

    __asm__ __volatile__("int $0x80"
                         : "=a"(r)
                         : "a"(nr), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e)
                         : "memory", "r8", "r9", "r10", "r11");
    return (int)r;
}

/** The i386 call with six arguments, preserving the native frame pointer. */
static int int80_6(long nr, unsigned long a, unsigned long b, unsigned long c, unsigned long d,
                   unsigned long e, unsigned long f)
{
    long r;

    __asm__ __volatile__("sub $128, %%rsp; push %%rbp; mov %[six], %%rbp; int $0x80; "
                         "pop %%rbp; add $128, %%rsp"
                         : "=a"(r)
                         : "a"(nr), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e), [six] "r"(f)
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

/** Linux's `compat_sigaction` and `compat_stack_t`, as the i386 calls take
 * them. */
struct sigaction32 {
    unsigned int handler, flags, restorer, mask[2];
};

struct stack32 {
    unsigned int sp;
    int flags;
    unsigned int size;
};

/** A signal taken in 32-bit code by an i386 handler on an alternate stack,
 * with the i386 rt frame, and its return there. */
static void run_ia32_signal(void)
{
    static struct sigaction32 ka;
    static struct stack32 ss, old;
    unsigned long base = (unsigned long)alt32, pid = (unsigned long)syscall0(SYS_GETPID);
    long r;

    ss.sp = (unsigned int)base;
    ss.flags = 0;
    ss.size = sizeof(alt32);
    ka.handler = (unsigned int)(unsigned long)handler32;
    ka.flags = (unsigned int)(SA_SIGINFO | SA_RESTORER | SA_ONSTACK);
    ka.restorer = (unsigned int)(unsigned long)restorer32;
    if (int80(I386_SIGALTSTACK, (unsigned long)&ss, 0, 0) != 0 ||
        int80_5(I386_RT_SIGACTION, SIGUSR1, (unsigned long)&ka, 0, 8, 0) != 0) {
        fail("the i386 sigaltstack or rt_sigaction failed");
    }
    r = run32(kill32, pid);
    if (h32[H32_EAX] != SIGUSR1 || h32[H32_SIG] != SIGUSR1 || h32[H32_CS] != GDT_USER_CODE32 ||
        h32[H32_ESP] <= base || h32[H32_ESP] >= base + sizeof(alt32) ||
        ((h32[H32_ESP] + 4) & 15) != 0 || h32[H32_SSFLAGS] != 0 ||
        h32[H32_EIP] != (unsigned int)(unsigned long)kill32_int80 + 2 ||
        h32[H32_UCCS] != GDT_USER_CODE32 || h32[H32_SIGNO] != SIGUSR1 || h32[H32_PID] != pid) {
        fail("an i386 handler on the alternate stack did not run in 32-bit code with the i386 "
             "rt frame");
    }
    report_held();
    if (r != 0 || int80(I386_SIGALTSTACK, 0, (unsigned long)&old, 0) != 0 ||
        old.sp != (unsigned int)base || old.size != sizeof(alt32) || old.flags != 0) {
        fail("the i386 rt_sigreturn did not resume the 32-bit code with the alternate stack set");
    }
    report_held();
}

/** Ignore SIGALRM so each interval can interrupt the sleeping call. */
static void on_alarm(int sig, void *info, void *ucontext)
{
    (void)sig;
    (void)info;
    (void)ucontext;
}

/** Each compat sleep reports EFAULT when its interrupted remainder is lost. */
static void run_sleep_remainder(void)
{
    static const unsigned int req32[2] = {100000, 0};
    static const unsigned int req64[4] = {100000, 0, 0, 0};
    struct k_sigaction ka;
    const unsigned long timer[4] = {0, 10000, 0, 10000};
    const unsigned long off[4] = {0, 0, 0, 0};
    long rc[3];

    ka.handler = on_alarm;
    ka.flags = SA_SIGINFO | SA_RESTORER;
    ka.restorer = sig_restorer;
    ka.mask[0] = ka.mask[1] = 0;
    if (syscall4(SYS_RT_SIGACTION, 14, (long)&ka, 0, 8) != 0 ||
        syscall3(SYS_SETITIMER, 0, (long)timer, 0) != 0) {
        fail("sleep interrupt timer");
    }
    rc[0] = int80(162, (unsigned long)req32, 1, 0);
    rc[1] = int80_5(267, 1, 0, (unsigned long)req32, 1, 0);
    rc[2] = int80_5(407, 1, 0, (unsigned long)req64, 1, 0);
    if (syscall3(SYS_SETITIMER, 0, (long)off, 0) != 0 || rc[0] != -14 || rc[1] != -14 ||
        rc[2] != -14) {
        fail("an interrupted compat sleep hid a failed remainder copy");
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
    long pid, got;
    int status, pipefd[2];
    char marker = 0;

    if (syscall1(SYS_PIPE, (long)pipefd) != 0) {
        fail("seccomp pipe");
    }
    pid = syscall0(SYS_FORK);
    if (pid < 0) {
        fail("fork");
    }
    if (pid == 0) {
        unsigned long iov[2];

        syscall1(SYS_CLOSE, pipefd[0]);
        iov[0] = (unsigned long)"x";
        iov[1] = 1;
        prog.len = sizeof(code) / sizeof(code[0]);
        prog.filter = code;
        if (syscall5(SYS_PRCTL, PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 ||
            syscall3(SYS_SECCOMP, SECCOMP_SET_MODE_FILTER, 0, (long)&prog) != 0) {
            sys_exit(2);
        }
        /* x86-64's 20 is writev; its byte proves the call returned. */
        if (syscall3(SYS_WRITEV, pipefd[1], (long)iov, 1) != 1) {
            sys_exit(3);
        }
        int80(I386_GETPID, 0, 0, 0);
        sys_exit(4);
    }
    syscall1(SYS_CLOSE, pipefd[1]);
    got = syscall3(SYS_READ, pipefd[0], (long)&marker, 1);
    syscall1(SYS_CLOSE, pipefd[0]);
    status = child_status(pid);
    if (got != 1 || marker != 'x' || status == (2 << 8) || status == (3 << 8)) {
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

/* The offsets in x86-64's user_regs_struct of orig_rax and cs. */
#define USER_ORIG_RAX 120
#define USER_CS       136

/** Resume the stopped tracee `pid` with `request` and wait for its next
 * stop; that stop's signal, or -1 when it did not stop. */
static int resume(long pid, long request)
{
    int status = 0;

    if (syscall4(SYS_PTRACE, request, pid, 0, 0) != 0 ||
        syscall3(SYS_WAITPID, pid, (long)&status, 0) != pid || (status & 0xFF) != 0x7F) {
        return -1;
    }
    return (status >> 8) & 0xFF;
}

/** The word at byte `off` of the stopped tracee `pid`'s user area, or
 * 0x5A5A when it cannot be read. */
static long peekuser(long pid, long off)
{
    long word = 0x5A5A;

    if (syscall4(SYS_PTRACE, PTRACE_PEEKUSER, pid, off, (long)&word) != 0) {
        return 0x5A5A;
    }
    return word;
}

/** orig_rax as a tracer reads it: -1 at the stops of an int3 in 64-bit and
 * in 32-bit code and of a fault, and the number at a system call's entry. */
static void run_orig(void)
{
    int status = 0, ok;
    long pid = syscall0(SYS_FORK);

    if (pid < 0) {
        fail("fork");
    }
    if (pid == 0) {
        syscall4(SYS_PTRACE, PTRACE_TRACEME, 0, 0, 0);
        syscall2(SYS_KILL, syscall0(SYS_GETPID), SIGSTOP);
        __asm__ __volatile__("int3");
        run32(int3_32, 0);
        syscall0(SYS_GETPID);
        /* Fault at address 0 without undefined C pointer dereferencing. */
        __asm__ __volatile__("movl $1, (%0)" : : "r"(0UL) : "memory");
        sys_exit(0);
    }
    if (syscall3(SYS_WAITPID, pid, (long)&status, 0) != pid || (status & 0xFF) != 0x7F) {
        fail("a traced child did not stop");
    }
    ok = resume(pid, PTRACE_CONT) == SIGTRAP && peekuser(pid, USER_ORIG_RAX) == -1 &&
         peekuser(pid, USER_CS) == GDT_USER_CODE;
    ok = ok && resume(pid, PTRACE_CONT) == SIGTRAP && peekuser(pid, USER_ORIG_RAX) == -1 &&
         peekuser(pid, USER_CS) == GDT_USER_CODE32;
    ok = ok && resume(pid, PTRACE_SYSCALL) == SIGTRAP && peekuser(pid, USER_ORIG_RAX) == SYS_GETPID;
    ok = ok && resume(pid, PTRACE_CONT) == SIGSEGV && peekuser(pid, USER_ORIG_RAX) == -1;
    syscall2(SYS_KILL, pid, 9);
    child_status(pid);
    if (!ok) {
        fail("orig_rax was not -1 at an int3 in 64-bit or 32-bit code or at a fault, or not the "
             "number at a system call's entry");
    }
    report_held();
}

/** An anonymous mapping of `len` bytes at the hint `addr` with `flags`
 * beside MAP_ANONYMOUS; its address, or a negated errno. */
static unsigned long map(unsigned long addr, unsigned long len, long flags)
{
    return (unsigned long)syscall6(SYS_MMAP, (long)addr, (long)len, PROT_READ | PROT_WRITE,
                                   flags | MAP_ANONYMOUS, -1, 0);
}

/** Whether [a, a + len) lies from 1 GiB to 2 GiB. */
static int in_low_window(unsigned long a, unsigned long len)
{
    return a >= LOW_1G && a <= LOW_2G - len;
}

/** MAP_32BIT's window and hints, and a hint below 4 GiB without it. */
static void run_map32(void)
{
    volatile unsigned char *m;
    unsigned long a;

    a = map(0, 2 * PAGE, MAP_PRIVATE | MAP_32BIT);
    if (sys_failed((long)a) || !in_low_window(a, 2 * PAGE)) {
        fail("an mmap with MAP_32BIT was not placed from 1 GiB to 2 GiB");
    }
    m = (volatile unsigned char *)a;
    m[0] = 0x5A;
    m[2 * PAGE - 1] = 0xA5;
    if (m[0] != 0x5A || m[2 * PAGE - 1] != 0xA5) {
        fail("the MAP_32BIT mapping did not keep its bytes");
    }
    syscall2(SYS_MUNMAP, (long)a, 2 * PAGE);
    report_held();
    a = map(0x90000000ul, PAGE, MAP_PRIVATE | MAP_32BIT);
    if (sys_failed((long)a) || !in_low_window(a, PAGE)) {
        fail("an mmap with MAP_32BIT and a hint above 2 GiB was not placed from 1 GiB to 2 GiB");
    }
    syscall2(SYS_MUNMAP, (long)a, PAGE);
    a = map(0x31000000ul, PAGE, MAP_PRIVATE | MAP_32BIT);
    if (a != 0x31000000ul) {
        fail("an mmap with MAP_32BIT did not take the free hint 0x31000000");
    }
    syscall2(SYS_MUNMAP, (long)a, PAGE);
    report_held();
    a = map(0xC0000000ul, PAGE, MAP_PRIVATE);
    if (a != 0xC0000000ul) {
        fail("an mmap did not take the free hint 0xC0000000, below 4 GiB");
    }
    syscall2(SYS_MUNMAP, (long)a, PAGE);
    report_held();
}

/** Linux's struct futex_waitv and struct __kernel_timespec, alike in both
 * ABIs. */
struct futex_waitv {
    unsigned long long val;
    unsigned long long uaddr;
    unsigned int flags;
    unsigned int reserved;
};

struct timespec64 {
    long long tv_sec;
    long long tv_nsec;
};

/** futex_waitv by `int $0x80`: a word that differs, a deadline that has
 * passed, and a wake by a child of a word in a shared page. */
static void run_futexv(void)
{
    static struct futex_waitv w[3];
    static struct timespec64 past;
    volatile unsigned int *words;
    unsigned long page;
    long pid;
    int i, r;

    page = map(0, PAGE, MAP_SHARED | MAP_32BIT);
    if (sys_failed((long)page)) {
        fail("an mmap of a shared page with MAP_32BIT failed");
    }
    words = (volatile unsigned int *)page;
    for (i = 0; i < 3; i++) {
        w[i].val = 0;
        w[i].uaddr = (unsigned long)&words[i];
        w[i].flags = FUTEX2_SIZE_U32;
        w[i].reserved = 0;
    }
    w[1].val = 9;
    if (int80_5(I386_FUTEX_WAITV, (unsigned long)w, 3, 0, 0, 0) != -EAGAIN) {
        fail("the i386 futex_waitv did not fail with EAGAIN for a word that differs");
    }
    w[1].val = 0;
    if (int80_5(I386_FUTEX_WAITV, (unsigned long)w, 3, 0, (unsigned long)&past, CLOCK_MONOTONIC) !=
        -ETIMEDOUT) {
        fail("the i386 futex_waitv did not fail with ETIMEDOUT at a deadline that had passed");
    }
    pid = syscall0(SYS_FORK);
    if (pid < 0) {
        fail("fork");
    }
    if (pid == 0) {
        while (!words[8]) {
            if (syscall3(SYS_FUTEX, (long)&words[2], FUTEX_WAKE, 1) == 1) {
                sys_exit(0);
            }
            syscall0(SYS_SCHED_YIELD);
        }
        sys_exit(2);
    }
    r = int80_5(I386_FUTEX_WAITV, (unsigned long)w, 3, 0, 0, 0);
    words[8] = 1;
    if (child_status(pid) != 0 || r != 2) {
        fail("a child's wake of the third shared word did not end the i386 futex_waitv with "
             "index 2");
    }
    syscall2(SYS_MUNMAP, (long)page, PAGE);
    report_held();
}

/** SIOCGSTAMP and SIOCGSTAMPNS by the i386 ioctl on a UDP socket that sends
 * a datagram to itself on the loopback, before and after its receive. */
static void run_sockstamp(void)
{
    static unsigned int tv[4], ts[4];
    static volatile unsigned int wide[4];
    static unsigned long sa[2];
    static unsigned int salen = 16;
    unsigned long args[6];
    char c;
    long u;
    int i;

    args[0] = 2; /* AF_INET */
    args[1] = 2; /* SOCK_DGRAM */
    args[2] = 0;
    u = sock_call(SC_SOCKET, args);
    if (u < 0) {
        fail("a UDP socket could not be made");
    }
    if (int80(I386_IOCTL, (unsigned long)u, SIOCGSTAMP_OLD, (unsigned long)tv) != -ENOENT) {
        fail("the i386 SIOCGSTAMP before any receive did not fail with ENOENT");
    }
    ((unsigned short *)sa)[0] = 2;
    ((unsigned int *)sa)[1] = 0x0100007Fu; /* 127.0.0.1 in network order */
    args[0] = (unsigned long)u;
    args[1] = (unsigned long)sa;
    args[2] = 16;
    if (sock_call(SC_BIND, args) != 0) {
        fail("the UDP socket could not be bound to the loopback");
    }
    args[2] = (unsigned long)&salen;
    if (sock_call(SC_GETSOCKNAME, args) != 0) {
        fail("getsockname");
    }
    args[1] = (unsigned long)"x";
    args[2] = 1;
    args[3] = 0;
    args[4] = (unsigned long)sa;
    args[5] = 16;
    if (sock_call(SC_SENDTO, args) != 1) {
        fail("the UDP socket could not send to itself");
    }
    args[1] = (unsigned long)&c;
    args[4] = 0;
    args[5] = 0;
    if (sock_call(SC_RECVFROM, args) != 1) {
        fail("the UDP socket did not receive its datagram");
    }
    for (i = 0; i < 4; i++) {
        tv[i] = ts[i] = wide[i] = 0xFFFFFFFFu;
    }
    if (int80(I386_IOCTL, (unsigned long)u, SIOCGSTAMP_OLD, (unsigned long)tv) != 0 ||
        int80(I386_IOCTL, (unsigned long)u, SIOCGSTAMPNS_OLD, (unsigned long)ts) != 0 ||
        syscall3(SYS_IOCTL, u, (long)SIOCGSTAMP_NEW, (long)wide) != 0) {
        fail("SIOCGSTAMP or SIOCGSTAMPNS after a receive failed");
    }
    if (tv[0] == 0 || tv[0] != wide[0] || tv[1] >= 1000000u || tv[2] != 0xFFFFFFFFu ||
        tv[3] != 0xFFFFFFFFu || ts[0] != tv[0] || ts[1] >= 1000000000u || ts[2] != 0xFFFFFFFFu ||
        ts[3] != 0xFFFFFFFFu) {
        fail("the i386 SIOCGSTAMP or SIOCGSTAMPNS did not give the receive's time in two "
             "32-bit words");
    }
    syscall1(SYS_CLOSE, u);
    report_held();
}

/** Failed and partial compat directory copies preserve the next entry. */
static void run_directory_copy(void)
{
    static unsigned char reference[1024], retry[1024], old[266];
    unsigned char *edge;
    unsigned long mapping;
    unsigned int first, i;
    long fd = syscall3(SYS_OPEN, (long)"/proc", 0, 0);
    int n;

    if (fd < 0 || (n = int80(141, fd, (unsigned long)reference, sizeof(reference))) <= 0) {
        fail("compat directory reference");
    }
    first = reference[8] | (unsigned int)reference[9] << 8;
    syscall3(SYS_LSEEK, fd, 0, 0);
    if (int80(141, fd, 1, sizeof(reference)) != -14 || syscall3(SYS_LSEEK, fd, 0, 1) != 0 ||
        int80(141, fd, (unsigned long)retry, sizeof(retry)) != n) {
        fail("getdents consumed entries after a failed copy");
    }
    for (i = 0; i < first; i++) {
        if (retry[i] != reference[i]) {
            fail("getdents retry skipped its first entry");
        }
    }
    mapping = map(0, 8192, MAP_PRIVATE | MAP_32BIT);
    if ((long)mapping < 0 || syscall3(SYS_MPROTECT, mapping + 4096, 4096, 0) != 0) {
        fail("directory copy fault mapping");
    }
    edge = (unsigned char *)(mapping + 4096 - first);
    syscall3(SYS_LSEEK, fd, 0, 0);
    if (int80(141, fd, (unsigned long)edge, sizeof(reference)) != (int)first ||
        int80(141, fd, (unsigned long)retry, sizeof(retry)) <= 0) {
        fail("getdents partial copy did not retain its delivered entry");
    }
    for (i = 0; i < first; i++) {
        if (edge[i] != reference[i]) {
            fail("getdents partial copy changed its first entry");
        }
    }
    if (retry[10] != reference[first + 10] || retry[11] != reference[first + 11]) {
        fail("getdents partial retry skipped the next entry");
    }
    syscall2(SYS_MUNMAP, mapping, 8192);
    syscall3(SYS_LSEEK, fd, 0, 0);
    if (int80(89, fd, (unsigned long)old, 0) != 1) {
        fail("old readdir reference");
    }
    syscall3(SYS_LSEEK, fd, 0, 0);
    if (int80(89, fd, 1, 0) != -14 || syscall3(SYS_LSEEK, fd, 0, 1) != 0 ||
        int80(89, fd, (unsigned long)retry, 0) != 1) {
        fail("old readdir consumed an entry after a failed copy");
    }
    for (i = 0; i < 10u + old[8] + ((unsigned int)old[9] << 8) + 1u; i++) {
        if (old[i] != retry[i]) {
            fail("old readdir retry skipped its entry");
        }
    }
    syscall1(SYS_CLOSE, fd);
    report_held();
}

/** A missing pselect mask still selects a timespec in each time ABI. */
static void run_pselect_null(void)
{
    static unsigned int bits[32], ts32[2] = {0, 1000000};
    static unsigned long ts64[2] = {0, 1000000000};
    int pipefd[2], rc32, rc64;

    if (syscall1(SYS_PIPE, (long)pipefd) != 0 || (unsigned int)pipefd[0] >= 1024u ||
        syscall3(SYS_WRITE, pipefd[1], (long)"x", 1) != 1) {
        fail("pselect ready pipe");
    }
    bits[pipefd[0] / 32] = 1u << (pipefd[0] % 32);
    rc32 = int80_6(308, pipefd[0] + 1, (unsigned long)bits, 0, 0, (unsigned long)ts32, 0);
    bits[pipefd[0] / 32] = 1u << (pipefd[0] % 32);
    rc64 = int80_6(413, pipefd[0] + 1, (unsigned long)bits, 0, 0, (unsigned long)ts64, 0);
    syscall1(SYS_CLOSE, pipefd[0]);
    syscall1(SYS_CLOSE, pipefd[1]);
    if (rc32 != 1 || rc64 != -22) {
        fail("pselect with a null mask used a timeval");
    }
    report_held();
}

/** Native and compat vector flags append without changing O_APPEND. */
static void run_vector_flags(void)
{
    static char data[] = "CDEF", readback[8];
    static unsigned long native[4];
    static unsigned int compat_vec[4];
    long fd = syscall2(319, (long)"wow64-vector", 0);
    unsigned int i;

    native[0] = (unsigned long)data;
    native[1] = 1;
    native[2] = (unsigned long)data + 1;
    native[3] = 1;
    compat_vec[0] = (unsigned int)(unsigned long)(data + 2);
    compat_vec[1] = 1;
    compat_vec[2] = (unsigned int)(unsigned long)(data + 3);
    compat_vec[3] = 1;
    if (fd < 0 || syscall3(SYS_WRITE, fd, (long)"ab", 2) != 2 ||
        syscall6(328, fd, (long)native, 2, 0, 0, 0x10) != 2 || syscall3(SYS_LSEEK, fd, 0, 1) != 2 ||
        int80_6(379, fd, (unsigned long)compat_vec, 2, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x10) != 2 ||
        syscall3(SYS_LSEEK, fd, 0, 1) != 6 || (syscall3(SYS_FCNTL, fd, 3, 0) & 02000) != 0) {
        fail("RWF_APPEND changed the wrong offset or the shared append flag");
    }
    native[0] = (unsigned long)readback;
    native[1] = 6;
    if (syscall6(327, fd, (long)native, 1, 0, 0, 0) != 6) {
        fail("native preadv2 is absent or cannot read the vector");
    }
    for (i = 0; i < 6; i++) {
        if (readback[i] != "abCDEF"[i]) {
            fail("RWF_APPEND overwrote existing data");
        }
    }
    compat_vec[0] = (unsigned int)(unsigned long)readback;
    compat_vec[1] = 6;
    syscall3(SYS_LSEEK, fd, 0, 0);
    if (int80_6(378, fd, (unsigned long)compat_vec, 1, 0xFFFFFFFFu, 0xFFFFFFFFu, 0) != 6 ||
        syscall3(SYS_LSEEK, fd, 0, 1) != 6) {
        fail("compat preadv2 did not use the shared offset at -1");
    }
    if (syscall4(277, fd, 0, 6, 0) != 0 || int80_6(314, fd, 0, 0, 6, 0, 0) != 0) {
        fail("sync_file_range is absent from the native table");
    }
    syscall1(SYS_CLOSE, fd);
    report_held();
}

/** A character device is not a sync_file_range file in either ABI. */
static void run_range_sync(void)
{
    long fd = syscall3(SYS_OPEN, (long)"/dev/null", 2, 0);
    long native, compat_result;

    if (fd < 0) {
        fail("range sync character device");
    }
    native = syscall4(277, fd, 0, 0, 0);
    compat_result = int80_6(314, fd, 0, 0, 0, 0, 0);
    syscall1(SYS_CLOSE, fd);
    if (native != -29 || compat_result != -29) {
        fail("sync_file_range accepted a character device");
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
    run_ia32_signal();
    run_sleep_remainder();
    run_seccomp();
    run_ptrace();
    run_orig();
    run_map32();
    run_futexv();
    run_sockstamp();
    run_directory_copy();
    run_pselect_null();
    run_vector_flags();
    run_range_sync();
    report_end();
    say("WOW64PROBE OK\n");
    sys_exit(0);
}
