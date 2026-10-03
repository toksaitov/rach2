/**
 * Exercises the misc syscalls busybox needs, from user space.
 *
 * Proves getppid (a child sees the parent's pid), umask (set returns the
 * previous mask, inherited across fork), times (a monotonic tick count plus
 * the four CPU times, none negative and none past the ticks since boot),
 * sysinfo (a live-process count, mem_unit 1), and vfork (the child runs
 * and exits with 7, and the parent then gets its pid and that status).
 *
 * Prints "MISCPROBE OK" / "MISCPROBE FAIL". Freestanding; direct system calls
 * (probe.h).
 */

#include "probe.h"

/* Linux's struct sysinfo, of longs: its size, and the offsets of procs and
 * mem_unit, on i386 and on x86-64. */
#if defined __x86_64__
#define SI_SIZE     112
#define SI_PROCS    80
#define SI_MEM_UNIT 104
#else
#define SI_SIZE     64
#define SI_PROCS    40
#define SI_MEM_UNIT 52
#endif

static void sys_exit(int s)
{
    syscall1(SYS_EXIT, s);
    __builtin_unreachable();
}
static int getpid(void)
{
    return syscall1(SYS_GETPID, 0);
}
static int getppid(void)
{
    return syscall1(SYS_GETPPID, 0);
}
static int umask(int m)
{
    return syscall1(SYS_UMASK, m);
}
static int fork(void)
{
    return syscall1(SYS_FORK, 0);
}
/**
 * vfork, with the child's exit in the same instructions. The child runs on
 * the parent's stack until it exits, so it must not return through the
 * frames the parent returns through later, nor call a function, whose frame
 * would overwrite them; it exits with `code` from the instructions that made
 * the call. The parent gets the child's pid, or -errno.
 */
static int vfork_exit(int code)
{
    long r;
#if defined __x86_64__
    __asm__ __volatile__("syscall\n\t"
                         "testq %%rax, %%rax\n\t"
                         "jnz 1f\n\t"
                         "movq %2, %%rax\n\t"
                         "syscall\n"
                         "1:"
                         : "=a"(r)
                         : "0"((long)SYS_VFORK), "i"(SYS_EXIT), "D"((long)code)
                         : "rcx", "r11", "memory", "cc");
#else
    __asm__ __volatile__("int $0x80\n\t"
                         "testl %%eax, %%eax\n\t"
                         "jnz 1f\n\t"
                         "movl %2, %%eax\n\t"
                         "int $0x80\n"
                         "1:"
                         : "=a"(r)
                         : "0"(SYS_VFORK), "i"(SYS_EXIT), "b"(code)
                         : "memory", "cc");
#endif
    return (int)r;
}
static int waitpid(int pid, int *st, int opt)
{
    return syscall3(SYS_WAITPID, pid, (long)st, opt);
}
static int times(void *tms)
{
    return syscall1(SYS_TIMES, (long)tms);
}
static int sysinfo(void *si)
{
    return syscall1(SYS_SYSINFO, (long)si);
}

static size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}
static void puts(const char *s)
{
    syscall3(SYS_WRITE, 1, (long)s, (int)strlen(s));
    syscall3(SYS_WRITE, 1, (long)"\n", 1);
}

/** Whether a + b is at most t, for t > 0 and a and b not negative, by a
 * subtraction that cannot overflow where the sum could. */
static int sum_within(int a, int b, int t)
{
    return t > 0 && a >= 0 && b >= 0 && a <= t && b <= t - a;
}

static int fails = 0;
static void check(int cond, const char *msg)
{
    if (!cond) {
        fails++;
        syscall3(SYS_WRITE, 1, (long)"  FAIL: ", 8);
        puts(msg);
    }
}

PROBE_START void _start(void)
{
    int me = getpid();
    int st;
    int pid;

    puts("MISCPROBE START");

    /* getppid: a child reports our pid as its parent. */
    pid = fork();
    if (pid == 0) {
        sys_exit(getppid() == me ? 0 : 1);
    }
    check(pid > 0 && waitpid(pid, &st, 0) == pid && (st & 0x7f) == 0 && ((st >> 8) & 0xff) == 0,
          "child getppid() == parent pid");

    /* umask: set returns the previous mask; the value is remembered. */
    {
        int prev = umask(0027);
        check(prev == 022, "umask() returns the default 022");
        check(umask(0022) == 0027, "umask() returns the value just set");
    }

    /* times: the return is a positive tick count, and no CPU time in the
       tms is negative or more than the ticks since boot. */
    {
        long tms[4];
        int t;
        tms[0] = tms[1] = tms[2] = tms[3] = -1;
        t = times(tms);
        check(t > 0, "times() returns a positive tick count");
        check(tms[0] >= 0 && tms[1] >= 0 && tms[2] >= 0 && tms[3] >= 0,
              "times() fills the tms fields");
        check(sum_within((int)tms[0], (int)tms[1], t) && sum_within((int)tms[2], (int)tms[3], t),
              "times() reports no more CPU time than the ticks since boot");
    }

    /* sysinfo: at least this process is counted; mem_unit is 1. */
    {
        unsigned char si[SI_SIZE];
        unsigned short procs;
        unsigned int mem_unit;
        check(sysinfo(si) == 0, "sysinfo() ok");
        procs = (unsigned short)(si[SI_PROCS] | (si[SI_PROCS + 1] << 8));
        mem_unit = (unsigned int)si[SI_MEM_UNIT] | ((unsigned int)si[SI_MEM_UNIT + 1] << 8) |
                   ((unsigned int)si[SI_MEM_UNIT + 2] << 16) |
                   ((unsigned int)si[SI_MEM_UNIT + 3] << 24);
        check(procs >= 1, "sysinfo() counts at least one process");
        check(mem_unit == 1, "sysinfo() mem_unit == 1");
    }

    /* vfork: the child exits 7 from the call's own instructions. */
    pid = vfork_exit(7);
    check(pid > 0 && waitpid(pid, &st, 0) == pid && ((st >> 8) & 0xff) == 7,
          "vfork() child runs and exits 7");

    if (fails == 0) {
        puts("MISCPROBE OK");
    } else {
        puts("MISCPROBE FAIL");
    }
    sys_exit(fails);
}
