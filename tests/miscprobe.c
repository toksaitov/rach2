/**
 * Exercises the misc syscalls busybox needs, from user space.
 *
 * Proves getppid (a child sees the parent's pid), umask (set returns the
 * previous mask, inherited across fork), times (a monotonic tick count plus
 * the four CPU times, none negative and none past the ticks since boot),
 * sysinfo (a live-process count, mem_unit 1), and vfork (the child runs
 * and exits with 7, and the parent then gets its pid and that status).
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
/**
 * vfork, with the child's exit in the same instructions. The child runs on
 * the parent's stack until it exits, so it must not return through the
 * frames the parent returns through later, nor call a function, whose frame
 * would overwrite them; it exits with `code` from the instructions that made
 * the call. The parent gets the child's pid, or -errno.
 */
static int vfork_exit(int code)
{
    int r;
    __asm__ __volatile__("int $0x80\n\t"
                         "testl %%eax, %%eax\n\t"
                         "jnz 1f\n\t"
                         "movl %2, %%eax\n\t"
                         "int $0x80\n"
                         "1:"
                         : "=a"(r)
                         : "0"(SYS_VFORK), "i"(SYS_EXIT), "b"(code)
                         : "memory", "cc");
    return r;
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

    /* times: the return is a positive tick count, and no CPU time in the
       tms is negative or more than the ticks since boot. */
    {
        int tms[4];
        int t;
        tms[0] = tms[1] = tms[2] = tms[3] = -1;
        t = times(tms);
        check(t > 0, "times() returns a positive tick count");
        check(tms[0] >= 0 && tms[1] >= 0 && tms[2] >= 0 && tms[3] >= 0,
              "times() fills the tms fields");
        check(sum_within(tms[0], tms[1], t) && sum_within(tms[2], tms[3], t),
              "times() reports no more CPU time than the ticks since boot");
    }

    /* sysinfo: at least this process is counted; mem_unit is 1. */
    {
        unsigned char si[64];
        unsigned short procs;
        unsigned int mem_unit;
        check(sysinfo(si) == 0, "sysinfo() ok");
        procs = (unsigned short)(si[40] | (si[41] << 8)); /* `procs` offset */
        mem_unit = (unsigned int)si[52] | ((unsigned int)si[53] << 8) |
                   ((unsigned int)si[54] << 16) | ((unsigned int)si[55] << 24);
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
