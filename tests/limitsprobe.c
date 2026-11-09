/**
 * limitsprobe: calls Debian 13's graphics stack makes, answered as Linux
 * answers them: rseq, SCHED_BATCH and SCHED_IDLE for an ordinary user, guard
 * regions through madvise, get_mempolicy and process_vm_readv with
 * process_vm_writev. Each row's expected answer is Linux 6.12's, from its code,
 * but for the guard regions, which Linux 6.13 added and whose answers are
 * 6.13's.
 *
 * The rows, in order, each an assertion of etude's report (probe_report.h)
 * for the case ETUDE_CASE names:
 *
 * `rseq-auxv`: the auxiliary vector gives AT_RSEQ_FEATURE_SIZE 28 and
 * AT_RSEQ_ALIGN 32.
 * `rseq-register`: an area registered as glibc 2.44 registers it, cpu_id -1
 * before, its size the feature size raised to 32 and the signature
 * 0x53053053, reads its processor in cpu_id_start and cpu_id on the return,
 * the one getcpu gives, node 0, and an mm_cid below the processors' count.
 * `rseq-errors`: the same registration again is EBUSY, another signature
 * EPERM, another area or a flag other than RSEQ_FLAG_UNREGISTER EINVAL.
 * `rseq-migrate`: an affinity of another processor alone moves the caller
 * there, and the area says so; the machine needs two processors.
 * `rseq-abort`: a signal taken inside a critical section returns from its
 * handler to the section's abort handler, with rseq_cs cleared.
 * `rseq-fork`: a forked child has its parent's area, EBUSY to register again
 * and current on its processor, and a signal inside a section whose abort
 * handler has no signature before it kills the child with SIGSEGV.
 * `rseq-unregister`: unregistering with another signature is EPERM, with
 * another length EINVAL; with the registered ones it leaves cpu_id -1 and
 * cpu_id_start 0, and again is EINVAL; a new area of 16 bytes, or one not
 * aligned to 32, is EINVAL.
 * `sched-user`: an ordinary user sets SCHED_BATCH and SCHED_IDLE and reads
 * each back; leaving SCHED_IDLE is EPERM, as RLIMIT_NICE 0 allows no rise
 * from nice 20, and SCHED_IDLE with priority 1 EINVAL.
 * `guard-install`: MADV_GUARD_INSTALL over a locked page is EINVAL; over an
 * anonymous page it succeeds, its neighbours keep their contents, and a
 * write(2) from the guard is EFAULT.
 * `guard-fault`: a read of the guard in a child is SIGSEGV with SEGV_MAPERR
 * and the address.
 * `guard-remove`: MADV_GUARD_REMOVE ends the guard, and the page reads 0.
 * `mempolicy-default`: get_mempolicy with no flags gives MPOL_DEFAULT and
 * clears the 16 bytes of a mask of 128 nodes.
 * `mempolicy-allowed`: MPOL_F_MEMS_ALLOWED gives 0 and node 0 alone.
 * `mempolicy-node`: MPOL_F_NODE with MPOL_F_ADDR gives node 0 for a page of
 * the stack.
 * `mempolicy-errors`: MPOL_F_NODE alone, an address without MPOL_F_ADDR, a
 * mask with maxnode 0, MPOL_F_MEMS_ALLOWED with MPOL_F_NODE and an unknown
 * flag are EINVAL, and MPOL_F_ADDR of an unmapped page EFAULT.
 * `vm-read`: root reads a string a child wrote after the fork, into two
 * buffers of its own.
 * `vm-partial`: a remote range that runs from a page into a hole gives the
 * bytes before the hole; one inside the hole is EFAULT; flags are EINVAL.
 * `vm-write`: root writes into the child, which finds the bytes there.
 * `vm-refused`: a child of an ordinary user is refused its root parent's
 * memory with EPERM, and the reaped child's pid is ESRCH.
 *
 * Prints "LIMITSPROBE OK" and exits 0 when every row holds, and otherwise
 * "LIMITSPROBE FAIL: what" and exits 1 at the first failure. Runs as root,
 * with user ID 1000 for the ordinary user. Freestanding; direct system calls
 * (probe.h).
 */

#include "probe.h"
#include "probe_report.h"

#define EPERM  1
#define ESRCH  3
#define EFAULT 14
#define EBUSY  16
#define EINVAL 22

#define SIGUSR1 10
#define SIGSEGV 11

#define SA_SIGINFO  0x00000004u
#define SA_RESTORER 0x04000000u

#define SEGV_MAPERR 1

#define PROT_READ     0x1
#define PROT_WRITE    0x2
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20

#define MADV_GUARD_INSTALL 102
#define MADV_GUARD_REMOVE  103

#define SCHED_OTHER 0
#define SCHED_BATCH 3
#define SCHED_IDLE  5

#define MPOL_F_NODE         1
#define MPOL_F_ADDR         2
#define MPOL_F_MEMS_ALLOWED 4

#define AT_NULL              0
#define AT_RSEQ_FEATURE_SIZE 27
#define AT_RSEQ_ALIGN        28

#define RSEQ_FLAG_UNREGISTER 1
#define RSEQ_SIG             0x53053053u

/* The ordinary user the rows that need one become. */
#define USER_ID 1000

#define PAGE 4096L

#if defined __x86_64__
#define SYS_MADVISE            28
#define SYS_SCHED_SETSCHEDULER 144
#define SYS_SCHED_GETSCHEDULER 145
#define SYS_MLOCK              149
#define SYS_MUNLOCK            150
#define SYS_SCHED_GETAFFINITY  204
#define SYS_GET_MEMPOLICY      239
#define SYS_GETCPU             309
#define SYS_PROCESS_VM_READV   310
#define SYS_PROCESS_VM_WRITEV  311
#define SYS_RSEQ               334
/* The offset of si_addr in a siginfo. */
#define SI_ADDR_AT             16
#else
#define SYS_MLOCK              150
#define SYS_MUNLOCK            151
#define SYS_SCHED_SETSCHEDULER 156
#define SYS_SCHED_GETSCHEDULER 157
#define SYS_MADVISE            219
#define SYS_SCHED_GETAFFINITY  242
#define SYS_GET_MEMPOLICY      275
#define SYS_GETCPU             318
#define SYS_PROCESS_VM_READV   347
#define SYS_PROCESS_VM_WRITEV  348
#define SYS_RSEQ               386
#define SI_ADDR_AT             12
#endif

typedef unsigned int u32;
typedef unsigned long long u64;

/* Linux's struct rseq, 32 bytes aligned to 32. */
struct rseq_area {
    u32 cpu_id_start;
    u32 cpu_id;
    u64 rseq_cs;
    u32 flags;
    u32 node_id;
    u32 mm_cid;
    u32 pad;
} __attribute__((aligned(32)));

/* Linux's struct rseq_cs. */
struct rseq_cs {
    u32 version;
    u32 flags;
    u64 start_ip;
    u64 post_commit_offset;
    u64 abort_ip;
} __attribute__((aligned(32)));

struct k_sigaction {
    void (*handler)(void);
    unsigned long flags;
    void (*restorer)(void);
    unsigned mask[2];
};

struct iovec {
    void *base;
    unsigned long len;
};

static struct rseq_area area;
static u32 area_len;
static struct rseq_cs section, bad_section;
static volatile int usr1_taken;
static char vm_buf[64];
static char vm_target[64];

void lp_restorer(void);
PROBE_RT_RESTORER(lp_restorer);

/*
 * lp_cs_run(field, cs, pid): store `cs` in the 64-bit rseq_cs at `field`,
 * then inside the section from lp_cs_start to lp_cs_post send SIGUSR1 to
 * `pid`, and return 0; a section aborted by the signal returns 1 from
 * lp_cs_abort, which the signature precedes.
 */
long lp_cs_run(u64 *field, struct rseq_cs *cs, long pid);
extern char lp_cs_start[], lp_cs_post[], lp_cs_abort[];
#if defined __x86_64__
__asm__(".text\n"
        ".globl lp_cs_run\n"
        "lp_cs_run:\n"
        "    movq %rsi, (%rdi)\n"
        ".globl lp_cs_start\n"
        "lp_cs_start:\n"
        "    movq %rdx, %rdi\n"
        "    movl $10, %esi\n"
        "    movl $62, %eax\n"
        "    syscall\n"
        "    xorl %eax, %eax\n"
        ".globl lp_cs_post\n"
        "lp_cs_post:\n"
        "    ret\n"
        "    .long 0x53053053\n"
        ".globl lp_cs_abort\n"
        "lp_cs_abort:\n"
        "    movl $1, %eax\n"
        "    ret\n");
#else
__asm__(".text\n"
        ".globl lp_cs_run\n"
        "lp_cs_run:\n"
        "    movl 4(%esp), %ecx\n"
        "    movl 8(%esp), %eax\n"
        "    movl %eax, (%ecx)\n"
        "    movl $0, 4(%ecx)\n"
        "    pushl %ebx\n"
        "    movl 16(%esp), %ebx\n"
        ".globl lp_cs_start\n"
        "lp_cs_start:\n"
        "    movl $10, %ecx\n"
        "    movl $37, %eax\n"
        "    int $0x80\n"
        "    xorl %eax, %eax\n"
        ".globl lp_cs_post\n"
        "lp_cs_post:\n"
        "    popl %ebx\n"
        "    ret\n"
        "    .long 0x53053053\n"
        ".globl lp_cs_abort\n"
        "lp_cs_abort:\n"
        "    popl %ebx\n"
        "    movl $1, %eax\n"
        "    ret\n");
#endif

/** End the process with `status`; it does not return. */
static void sys_exit(int status)
{
    syscall1(SYS_EXIT_GROUP, (long)status);
    __builtin_unreachable();
}

/** Say `what` failed, fail the current row with it, and exit 1. */
static void fail(const char *what)
{
    probe_line("LIMITSPROBE FAIL: ", what, 0);
    report_stop(what);
    sys_exit(1);
}

/** Fail with `what` unless `ok`. */
static void check(int ok, const char *what)
{
    if (!ok) {
        fail(what);
    }
}

/** Whether the `n` bytes at `a` and `b` are the same. */
static int same(const void *a, const void *b, unsigned long n)
{
    const unsigned char *x = a, *y = b;

    while (n-- > 0) {
        if (*x++ != *y++) {
            return 0;
        }
    }
    return 1;
}

/** Set the `n` bytes at `p` to `v`. */
static void fill(void *p, int v, unsigned long n)
{
    unsigned char *q = p;

    while (n-- > 0) {
        *q++ = (unsigned char)v;
    }
}

/** The processor the caller runs on, from getcpu. */
static long cpu_now(void)
{
    unsigned cpu = 0xFFFFFFFFu;

    check(syscall3(SYS_GETCPU, (long)&cpu, 0, 0) == 0, "getcpu");
    return (long)cpu;
}

/** Install `h`, a handler of the kind `extra`'s SA_SIGINFO says, for
 * `signo` with SA_RESTORER and the flags `extra`. */
static void install(int signo, void (*h)(void), unsigned long extra)
{
    struct k_sigaction ka;

    ka.handler = h;
    ka.flags = SA_RESTORER | extra;
    ka.restorer = lp_restorer;
    ka.mask[0] = 0;
    ka.mask[1] = 0;
    check(syscall4(SYS_RT_SIGACTION, signo, (long)&ka, 0, 8) == 0, "rt_sigaction");
}

/** Count a SIGUSR1. */
static void on_usr1(int signo)
{
    (void)signo;
    usr1_taken++;
}

/** Fork; the child's 0, or the parent's child pid. */
static long fork_child(void)
{
    long pid = syscall0(SYS_FORK);

    check(pid >= 0, "fork");
    return pid;
}

/** Reap `pid` and return its wait status. */
static int reap(long pid)
{
    int status = -1;

    check(syscall4(SYS_WAITPID, pid, (long)&status, 0, 0) == pid, "wait");
    return status;
}

/** Become the ordinary user, in a child. */
static void become_user(void)
{
    if (syscall1(SYS_SETUID, USER_ID) != 0) {
        sys_exit(100);
    }
}

/** The value of the auxiliary vector's `type` on the stack `sp`, or -1. */
static long aux_value(unsigned long *sp, unsigned long type)
{
    unsigned long *p = sp + 1 + sp[0] + 1;

    while (*p != 0) {
        p++;
    }
    for (p++; p[0] != AT_NULL; p += 2) {
        if (p[0] == type) {
            return (long)p[1];
        }
    }
    return -1;
}

/** rseq-auxv: the area's size and alignment; sets `area_len` as glibc. */
static void rseq_auxv(unsigned long *sp)
{
    long size = aux_value(sp, AT_RSEQ_FEATURE_SIZE);

    check(size == 28 && aux_value(sp, AT_RSEQ_ALIGN) == 32, "AT_RSEQ_FEATURE_SIZE or ALIGN");
    area_len = size < 32 ? 32u : (u32)size;
    report_held();
}

/** rseq-register and rseq-errors. */
static void rseq_register(long ncpu)
{
    struct rseq_area other __attribute__((aligned(32)));
    long cpu;

    area.cpu_id = 0xFFFFFFFFu;
    area.cpu_id_start = 0;
    area.rseq_cs = 0;
    area.flags = 0;
    check(syscall4(SYS_RSEQ, (long)&area, area_len, 0, RSEQ_SIG) == 0, "rseq registration");
    cpu = cpu_now();
    check(area.cpu_id == (u32)cpu && area.cpu_id_start == (u32)cpu,
          "the area's processor is not getcpu's");
    check(area.node_id == 0 && (long)area.mm_cid < ncpu, "the area's node or mm_cid");
    report_held();
    check(syscall4(SYS_RSEQ, (long)&area, area_len, 0, RSEQ_SIG) == -EBUSY,
          "a second registration is not EBUSY");
    check(syscall4(SYS_RSEQ, (long)&area, area_len, 0, RSEQ_SIG + 1) == -EPERM,
          "another signature is not EPERM");
    check(syscall4(SYS_RSEQ, (long)&other, 32, 0, RSEQ_SIG) == -EINVAL,
          "another area is not EINVAL");
    check(syscall4(SYS_RSEQ, (long)&area, area_len, 2, RSEQ_SIG) == -EINVAL,
          "an unknown flag is not EINVAL");
    report_held();
}

/** rseq-migrate: move to each other processor the mask `all` has. */
static void rseq_migrate(const unsigned long *all, long ncpu)
{
    unsigned long one[2];
    long from = cpu_now(), to;
    int moved = 0;

    check(ncpu >= 2, "the case needs two processors");
    for (to = 0; to < (long)(8 * sizeof(one)); to++) {
        if (to == from || !((all[to / (8 * sizeof(long))] >> (to % (8 * sizeof(long)))) & 1ul)) {
            continue;
        }
        one[0] = 0;
        one[1] = 0;
        one[to / (8 * sizeof(long))] = 1ul << (to % (8 * sizeof(long)));
        check(syscall3(SYS_SCHED_SETAFFINITY, 0, sizeof(one), (long)one) == 0, "sched_setaffinity");
        check(cpu_now() == to, "the affinity did not move the caller");
        check(area.cpu_id == (u32)to && area.cpu_id_start == (u32)to,
              "the area did not follow the move");
        moved = 1;
        break;
    }
    check(moved, "no other processor in the affinity");
    check(syscall3(SYS_SCHED_SETAFFINITY, 0, 2 * sizeof(long), (long)all) == 0,
          "the affinity back");
    report_held();
}

/** Fill `cs` with the section of lp_cs_run and the abort handler `abort_ip`. */
static void set_section(struct rseq_cs *cs, char *abort_ip)
{
    cs->version = 0;
    cs->flags = 0;
    cs->start_ip = (unsigned long)lp_cs_start;
    cs->post_commit_offset = (unsigned long)(lp_cs_post - lp_cs_start);
    cs->abort_ip = (unsigned long)abort_ip;
}

/** rseq-abort. */
static void rseq_abort(void)
{
    install(SIGUSR1, (void (*)(void))on_usr1, 0);
    set_section(&section, lp_cs_abort);
    usr1_taken = 0;
    check(lp_cs_run(&area.rseq_cs, &section, syscall0(SYS_GETPID)) == 1,
          "the section was not aborted by its signal");
    check(usr1_taken == 1, "the handler did not run once");
    check(area.rseq_cs == 0, "rseq_cs was not cleared");
    report_held();
}

/** rseq-fork. */
static void rseq_fork(void)
{
    long pid = fork_child();
    int status;

    if (pid == 0) {
        if (syscall4(SYS_RSEQ, (long)&area, area_len, 0, RSEQ_SIG) != -EBUSY) {
            sys_exit(2);
        }
        if (area.cpu_id != (u32)cpu_now()) {
            sys_exit(3);
        }
        set_section(&bad_section, lp_cs_post);
        lp_cs_run(&area.rseq_cs, &bad_section, syscall0(SYS_GETPID));
        sys_exit(4);
    }
    status = reap(pid);
    check(status != (2 << 8), "the child's registration is not its parent's");
    check(status != (3 << 8), "the child's area is not on its processor");
    check((status & 0x7F) == SIGSEGV, "an abort handler without the signature is not SIGSEGV");
    report_held();
}

/** rseq-unregister. */
static void rseq_unregister(void)
{
    check(syscall4(SYS_RSEQ, (long)&area, area_len, RSEQ_FLAG_UNREGISTER, RSEQ_SIG + 1) == -EPERM,
          "unregistering with another signature is not EPERM");
    check(syscall4(SYS_RSEQ, (long)&area, area_len + 32, RSEQ_FLAG_UNREGISTER, RSEQ_SIG) == -EINVAL,
          "unregistering with another length is not EINVAL");
    check(syscall4(SYS_RSEQ, (long)&area, area_len, RSEQ_FLAG_UNREGISTER, RSEQ_SIG) == 0,
          "unregistering");
    check(area.cpu_id == 0xFFFFFFFFu && area.cpu_id_start == 0,
          "the area does not read as unregistered");
    check(syscall4(SYS_RSEQ, (long)&area, area_len, RSEQ_FLAG_UNREGISTER, RSEQ_SIG) == -EINVAL,
          "unregistering again is not EINVAL");
    check(syscall4(SYS_RSEQ, (long)&area, 16, 0, RSEQ_SIG) == -EINVAL,
          "an area of 16 bytes is not EINVAL");
    check(syscall4(SYS_RSEQ, (long)&area + 16, 32, 0, RSEQ_SIG) == -EINVAL,
          "an area not aligned to 32 is not EINVAL");
    report_held();
}

/** The policy step of `sched_user`'s child: set `policy` at `priority`. */
static long set_policy(int policy, int priority)
{
    return syscall3(SYS_SCHED_SETSCHEDULER, 0, policy, (long)&priority);
}

/** sched-user. */
static void sched_user(void)
{
    long pid = fork_child();
    int status;

    if (pid == 0) {
        become_user();
        if (set_policy(SCHED_BATCH, 0) != 0 || syscall1(SYS_SCHED_GETSCHEDULER, 0) != SCHED_BATCH) {
            sys_exit(2);
        }
        if (set_policy(SCHED_IDLE, 0) != 0 || syscall1(SYS_SCHED_GETSCHEDULER, 0) != SCHED_IDLE) {
            sys_exit(3);
        }
        if (set_policy(SCHED_OTHER, 0) != -EPERM || set_policy(SCHED_BATCH, 0) != -EPERM) {
            sys_exit(4);
        }
        if (set_policy(SCHED_IDLE, 1) != -EINVAL ||
            syscall1(SYS_SCHED_GETSCHEDULER, 0) != SCHED_IDLE) {
            sys_exit(5);
        }
        sys_exit(0);
    }
    status = reap(pid);
    check(status != (100 << 8), "setuid");
    check(status != (2 << 8), "a user is refused SCHED_BATCH");
    check(status != (3 << 8), "a user is refused SCHED_IDLE");
    check(status != (4 << 8), "leaving SCHED_IDLE is not EPERM");
    check(status == 0, "SCHED_IDLE with priority 1 is not EINVAL");
    report_held();
}

/** The SIGSEGV handler of guard-fault's child: exit 0 for SEGV_MAPERR at
 * the guard, whose address the child left in vm_buf. */
static void on_segv(int signo, void *info, void *uc)
{
    const char *si = info;
    void *addr = *(void *const *)(si + SI_ADDR_AT);

    (void)signo;
    (void)uc;
    sys_exit(*(const int *)(si + 8) == SEGV_MAPERR && addr == *(void **)vm_buf ? 0 : 2);
}

/** guard-install, guard-fault and guard-remove. */
static void guard_rows(void)
{
    long map = probe_mmap(0, 3 * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    volatile char *page;
    int fds[2];
    char byte = 0;
    long pid;
    int status;

    check(!sys_failed(map), "mmap");
    page = (volatile char *)map;
    page[0] = 'a';
    page[PAGE] = 'b';
    page[2 * PAGE] = 'c';
    check(syscall2(SYS_MLOCK, map + 2 * PAGE, PAGE) == 0, "mlock");
    check(syscall3(SYS_MADVISE, map + 2 * PAGE, PAGE, MADV_GUARD_INSTALL) == -EINVAL,
          "a guard over a locked page is not EINVAL");
    check(syscall2(SYS_MUNLOCK, map + 2 * PAGE, PAGE) == 0, "munlock");
    check(syscall3(SYS_MADVISE, map + PAGE, PAGE, MADV_GUARD_INSTALL) == 0, "MADV_GUARD_INSTALL");
    check(page[0] == 'a' && page[2 * PAGE] == 'c', "a guard's neighbour lost its contents");
    check(syscall1(SYS_PIPE, (long)fds) == 0, "pipe");
    check(syscall3(SYS_WRITE, fds[1], map + PAGE, 1) == -EFAULT,
          "a write from a guard is not EFAULT");
    report_held();
    pid = fork_child();
    if (pid == 0) {
        *(void **)vm_buf = (void *)(map + PAGE);
        install(SIGSEGV, (void (*)(void))on_segv, SA_SIGINFO);
        byte = page[PAGE];
        sys_exit(3);
    }
    status = reap(pid);
    check(status != (2 << 8), "the guard's SIGSEGV is not SEGV_MAPERR at its address");
    check(status == 0, "a read of the guard was no SIGSEGV");
    report_held();
    check(syscall3(SYS_MADVISE, map + PAGE, PAGE, MADV_GUARD_REMOVE) == 0, "MADV_GUARD_REMOVE");
    check(page[PAGE] == 0, "a removed guard's page is not zero");
    check(syscall3(SYS_WRITE, fds[1], map + PAGE, 1) == 1 &&
              syscall3(SYS_READ, fds[0], (long)&byte, 1) == 1 && byte == 0,
          "a write from a removed guard");
    syscall1(SYS_CLOSE, fds[0]);
    syscall1(SYS_CLOSE, fds[1]);
    syscall2(SYS_MUNMAP, map, 3 * PAGE);
    report_held();
}

/** Whether the `n` bytes at `p` are 0 but byte 0, which is `first`. */
static int mask_is(const unsigned char *p, unsigned long n, unsigned char first)
{
    unsigned long i;

    if (p[0] != first) {
        return 0;
    }
    for (i = 1; i < n; i++) {
        if (p[i] != 0) {
            return 0;
        }
    }
    return 1;
}

/** mempolicy-default, -allowed, -node and -errors. */
static void mempolicy_rows(void)
{
    unsigned long mask[16 / sizeof(unsigned long)];
    long hole = probe_mmap(0, PAGE, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    int mode = -1, local = 7;

    check(!sys_failed(hole) && syscall2(SYS_MUNMAP, hole, PAGE) == 0, "a hole");
    fill(mask, 0xFF, sizeof(mask));
    check(syscall5(SYS_GET_MEMPOLICY, (long)&mode, (long)mask, 128, 0, 0) == 0, "get_mempolicy");
    check(mode == 0 && mask_is((unsigned char *)mask, sizeof(mask), 0),
          "the policy is not MPOL_DEFAULT with an empty mask");
    report_held();
    mode = -1;
    fill(mask, 0xFF, sizeof(mask));
    check(syscall5(SYS_GET_MEMPOLICY, (long)&mode, (long)mask, 128, 0, MPOL_F_MEMS_ALLOWED) == 0,
          "MPOL_F_MEMS_ALLOWED");
    check(mode == 0 && mask_is((unsigned char *)mask, sizeof(mask), 1),
          "the allowed nodes are not node 0 alone");
    report_held();
    mode = -1;
    check(syscall5(SYS_GET_MEMPOLICY, (long)&mode, 0, 0, (long)&local, MPOL_F_NODE | MPOL_F_ADDR) ==
                  0 &&
              mode == 0,
          "a stack page is not on node 0");
    report_held();
    check(syscall5(SYS_GET_MEMPOLICY, (long)&mode, 0, 0, 0, MPOL_F_NODE) == -EINVAL,
          "MPOL_F_NODE alone is not EINVAL");
    check(syscall5(SYS_GET_MEMPOLICY, (long)&mode, 0, 0, (long)&local, 0) == -EINVAL,
          "an address without MPOL_F_ADDR is not EINVAL");
    check(syscall5(SYS_GET_MEMPOLICY, (long)&mode, (long)mask, 0, 0, 0) == -EINVAL,
          "maxnode 0 is not EINVAL");
    check(syscall5(SYS_GET_MEMPOLICY, (long)&mode, 0, 0, 0, MPOL_F_MEMS_ALLOWED | MPOL_F_NODE) ==
              -EINVAL,
          "MPOL_F_MEMS_ALLOWED with MPOL_F_NODE is not EINVAL");
    check(syscall5(SYS_GET_MEMPOLICY, (long)&mode, 0, 0, 0, 8) == -EINVAL,
          "an unknown flag is not EINVAL");
    check(syscall5(SYS_GET_MEMPOLICY, (long)&mode, 0, 0, hole, MPOL_F_ADDR) == -EFAULT,
          "MPOL_F_ADDR of a hole is not EFAULT");
    report_held();
}

/** process_vm_readv or, with `write`, process_vm_writev of `pid`: `n` bytes
 * between `local` and `remote` in one buffer each. */
static long vm_rw(int write, long pid, void *local, void *remote, unsigned long n)
{
    struct iovec l, r;

    l.base = local;
    l.len = n;
    r.base = remote;
    r.len = n;
    return syscall6(write ? SYS_PROCESS_VM_WRITEV : SYS_PROCESS_VM_READV, pid, (long)&l, 1,
                    (long)&r, 1, 0);
}

/** The text the child of vm_rows writes after the fork, and the one root
 * writes into it. */
static const char child_words[32] = "words the child wrote";
static const char parent_words[32] = "words the parent wrote";

/** The child of vm_rows: write `child_words`, say so on `ready`, wait on
 * `go`, and exit 0 when `parent_words` arrived in vm_target. */
static void vm_child(int ready, int go)
{
    char c = 0;
    unsigned i;

    for (i = 0; i < sizeof(child_words); i++) {
        vm_buf[i] = child_words[i];
    }
    if (syscall3(SYS_WRITE, ready, (long)&c, 1) != 1 || syscall3(SYS_READ, go, (long)&c, 1) != 1) {
        sys_exit(2);
    }
    sys_exit(same(vm_target, parent_words, sizeof(parent_words)) ? 0 : 3);
}

/** vm-read, vm-partial, vm-write and vm-refused. */
static void vm_rows(void)
{
    long map = probe_mmap(0, 2 * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    struct iovec l[2], r;
    char got[32], c = 0;
    int ready[2], go[2];
    long pid, user;
    int status;

    check(!sys_failed(map) && syscall2(SYS_MUNMAP, map + PAGE, PAGE) == 0, "a page beside a hole");
    fill((void *)map, 'm', PAGE);
    check(syscall1(SYS_PIPE, (long)ready) == 0 && syscall1(SYS_PIPE, (long)go) == 0, "pipes");
    pid = fork_child();
    if (pid == 0) {
        vm_child(ready[1], go[0]);
    }
    check(syscall3(SYS_READ, ready[0], (long)&c, 1) == 1, "the child's word");
    fill(got, 0, sizeof(got));
    l[0].base = got;
    l[0].len = 10;
    l[1].base = got + 10;
    l[1].len = sizeof(got) - 10;
    r.base = vm_buf;
    r.len = sizeof(child_words);
    check(syscall6(SYS_PROCESS_VM_READV, pid, (long)l, 2, (long)&r, 1, 0) == sizeof(got),
          "process_vm_readv");
    check(same(got, child_words, sizeof(got)), "the bytes read are not the child's");
    report_held();
    check(vm_rw(0, pid, got, (char *)map + PAGE - 16, 32) == 16, "a read into a hole is not cut");
    check(vm_rw(0, pid, got, (char *)map + PAGE, 32) == -EFAULT, "a read of a hole is not EFAULT");
    check(syscall6(SYS_PROCESS_VM_READV, pid, (long)l, 1, (long)&r, 1, 1) == -EINVAL,
          "a flag is not EINVAL");
    report_held();
    check(vm_rw(1, pid, (void *)parent_words, vm_target, sizeof(parent_words)) ==
              sizeof(parent_words),
          "process_vm_writev");
    check(syscall3(SYS_WRITE, go[1], (long)&c, 1) == 1, "the parent's word");
    status = reap(pid);
    check(status == 0, "the child did not find the bytes written");
    report_held();
    user = fork_child();
    if (user == 0) {
        become_user();
        sys_exit(vm_rw(0, syscall0(SYS_GETPPID), got, vm_buf, 8) == -EPERM ? 0 : 2);
    }
    status = reap(user);
    check(status != (100 << 8), "setuid");
    check(status == 0, "a user is not refused root's memory with EPERM");
    check(vm_rw(0, pid, got, vm_buf, 8) == -ESRCH, "a reaped child's pid is not ESRCH");
    syscall1(SYS_CLOSE, ready[0]);
    syscall1(SYS_CLOSE, ready[1]);
    syscall1(SYS_CLOSE, go[0]);
    syscall1(SYS_CLOSE, go[1]);
    syscall2(SYS_MUNMAP, map, PAGE);
    report_held();
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/** Run the rows in order; `_start` calls it with the initial stack. */
void probe_main(unsigned long *sp)
{
    static const char *const ids[] = {
        "rseq-auxv",         "rseq-register",  "rseq-errors",      "rseq-migrate",
        "rseq-abort",        "rseq-fork",      "rseq-unregister",  "sched-user",
        "guard-install",     "guard-fault",    "guard-remove",     "mempolicy-default",
        "mempolicy-allowed", "mempolicy-node", "mempolicy-errors", "vm-read",
        "vm-partial",        "vm-write",       "vm-refused"};
    unsigned long all[2];
    long ncpu = 0, i;
    unsigned k;

    report_begin(report_case((char **)(sp + 1) + sp[0] + 1, "guest.limits.limitsprobe"));
    for (k = 0; k < sizeof(ids) / sizeof(ids[0]); k++) {
        report_plan(ids[k]);
    }
    fill(all, 0, sizeof(all));
    check(syscall3(SYS_SCHED_GETAFFINITY, 0, sizeof(all), (long)all) > 0, "sched_getaffinity");
    for (i = 0; i < (long)(8 * sizeof(all)); i++) {
        ncpu += (all[i / (8 * sizeof(long))] >> (i % (8 * sizeof(long)))) & 1ul;
    }
    rseq_auxv(sp);
    rseq_register(ncpu);
    rseq_migrate(all, ncpu);
    rseq_abort();
    rseq_fork();
    rseq_unregister();
    sched_user();
    guard_rows();
    mempolicy_rows();
    vm_rows();
    report_end();
    probe_line("LIMITSPROBE OK", 0, 0);
    sys_exit(0);
}
