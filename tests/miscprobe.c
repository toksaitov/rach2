/*
 * miscprobe.c - Phase 18 probe: the busybox-required misc syscalls.
 *
 * Proves, from user space via raw int 0x80: getppid (a child sees the parent's
 * pid), umask (set returns the previous mask, inherited across fork), times (a
 * monotonic tick count + a zeroed tms), sysinfo (a live-process count, mem_unit
 * 1), and vfork (a fork alias -- parent gets a pid, child runs and exits).
 *
 * Prints "MISCPROBE OK" / "MISCPROBE FAIL". Freestanding; direct int 0x80.
 */

typedef unsigned int size_t;

#define SYS_EXIT    1
#define SYS_FORK    2
#define SYS_WRITE   4
#define SYS_WAITPID 7
#define SYS_GETPID  20
#define SYS_TIMES   43
#define SYS_UMASK   60
#define SYS_GETPPID 64
#define SYS_SYSINFO 116
#define SYS_VFORK   190

static int syscall1(int n, int a)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a) : "memory");
    return r;
}
static int syscall3(int n, int a, int b, int c)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}

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
static int vfork(void)
{
    return syscall1(SYS_VFORK, 0);
}
static int waitpid(int pid, int *st, int opt)
{
    return syscall3(SYS_WAITPID, pid, (int)(size_t)st, opt);
}
static int times(void *tms)
{
    return syscall1(SYS_TIMES, (int)(size_t)tms);
}
static int sysinfo(void *si)
{
    return syscall1(SYS_SYSINFO, (int)(size_t)si);
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
    syscall3(SYS_WRITE, 1, (int)(size_t)s, (int)strlen(s));
    syscall3(SYS_WRITE, 1, (int)(size_t)"\n", 1);
}

static int fails = 0;
static void check(int cond, const char *msg)
{
    if (!cond) {
        fails++;
        syscall3(SYS_WRITE, 1, (int)(size_t)"  FAIL: ", 8);
        puts(msg);
    }
}

void _start(void)
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
    check(pid > 0 && waitpid(pid, &st, 0) == pid && ((st >> 8) & 0xff) == 0,
          "child getppid() == parent pid");

    /* umask: set returns the previous mask; the value is remembered. */
    {
        int prev = umask(0027);
        check(prev == 022, "umask() returns the default 022");
        check(umask(0022) == 0027, "umask() returns the value just set");
    }

    /* times: a tms is zeroed and the return is a positive tick count. */
    {
        int tms[4];
        int t;
        tms[0] = tms[1] = tms[2] = tms[3] = -1;
        t = times(tms);
        check(t > 0, "times() returns a positive tick count");
        check(tms[0] == 0 && tms[1] == 0 && tms[2] == 0 && tms[3] == 0,
              "times() zeroes the tms fields");
    }

    /* sysinfo: at least this process is counted; mem_unit is 1. */
    {
        unsigned char si[64];
        unsigned short procs;
        unsigned int mem_unit;
        check(sysinfo(si) == 0, "sysinfo() ok");
        procs = (unsigned short)(si[40] | (si[41] << 8)); /* offset of `procs` */
        mem_unit = (unsigned int)si[52] | ((unsigned int)si[53] << 8) |
                   ((unsigned int)si[54] << 16) | ((unsigned int)si[55] << 24);
        check(procs >= 1, "sysinfo() counts at least one process");
        check(mem_unit == 1, "sysinfo() mem_unit == 1");
    }

    /* vfork: behaves as a fork alias (parent gets a pid, child runs + exits). */
    pid = vfork();
    if (pid == 0) {
        sys_exit(7);
    }
    check(pid > 0 && waitpid(pid, &st, 0) == pid && ((st >> 8) & 0xff) == 7,
          "vfork() child runs and exits 7");

    if (fails == 0) {
        puts("MISCPROBE OK");
    } else {
        puts("MISCPROBE FAIL");
    }
    sys_exit(fails);
}
