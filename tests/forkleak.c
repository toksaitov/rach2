/**
 * Probes that fork()/exit()/wait() does not leak physical memory.
 *
 * Each fork gives the child a kernel stack. Before this milestone that stack
 * came from the never-freed bump allocator, so every fork/exit/reap cycle lost
 * four frames for good and the pool drained across the kernel's own respawn
 * churn (a steady, unbounded leak that eventually denies fork). The stacks now
 * come from a fixed arena the reap returns to the pool, so the free-frame count
 * is flat across churn. The probe reads sysinfo's freeram after a warm-up, runs
 * many fork/exit/wait cycles, and asserts the free count did not fall.
 *
 * Prints "FORKLEAK OK". Freestanding; direct system calls (probe.h).
 */

#include "probe.h"

#define STDOUT_FILENO 1

#define WARMUP 8
#define CYCLES 120
/* Tolerance (bytes): far below the ~1.6 MB a 120-cycle leak would cost, far
 * above any free-list jitter, so a real leak is unmistakable. */
#define SLACK  (64u * 1024u)

/* Linux's sysinfo struct, of longs, sized so the kernel's copy_to_user fits:
 * 64 bytes on i386 and 112 on x86-64. */
struct sysinfo {
    long uptime;
    unsigned long loads[3];
    unsigned long totalram;
    unsigned long freeram;
    unsigned long sharedram;
    unsigned long bufferram;
    unsigned long totalswap;
    unsigned long freeswap;
    unsigned short procs;
    unsigned short pad;
    unsigned long totalhigh;
    unsigned long freehigh;
    unsigned int mem_unit;
    char _f[20 - 2 * sizeof(long) - sizeof(int)];
};

static int sys_write(int fd, const void *buf, size_t count)
{
    return syscall3(SYS_WRITE, (long)fd, (long)buf, (long)count);
}

static void sys_exit(int status)
{
    syscall1(SYS_EXIT, (long)status);
    __builtin_unreachable();
}

static int sys_fork(void)
{
    return syscall0(SYS_FORK);
}

static int sys_waitpid(int pid, int *status, int options)
{
    return syscall3(SYS_WAITPID, (long)pid, (long)status, (long)options);
}

static int sys_sysinfo(struct sysinfo *si)
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
    sys_write(STDOUT_FILENO, s, strlen(s));
    sys_write(STDOUT_FILENO, "\n", 1);
}

static void fail(const char *why)
{
    sys_write(STDOUT_FILENO, "FORKLEAK FAIL: ", 15);
    puts(why);
    sys_exit(1);
}

/* One fork/exit(0)/wait cycle; fails the probe if fork ran out of memory. */
static void churn(void)
{
    int pid, status;

    pid = sys_fork();
    if (pid < 0) {
        fail("fork ran out of memory: the pool is leaking");
    }
    if (pid == 0) {
        sys_exit(0);
    }
    if (sys_waitpid(pid, &status, 0) != pid) {
        fail("waitpid");
    }
}

PROBE_START void _start(void)
{
    struct sysinfo si;
    unsigned long base_free, end_free;
    int i;

    /* Warm the page-table and page-directory free lists so the only frames that
     * move during the measured loop are the ones a leak would strand. */
    for (i = 0; i < WARMUP; i++) {
        churn();
    }

    if (sys_sysinfo(&si) != 0) {
        fail("sysinfo");
    }
    base_free = si.freeram;

    for (i = 0; i < CYCLES; i++) {
        churn();
    }

    if (sys_sysinfo(&si) != 0) {
        fail("sysinfo");
    }
    end_free = si.freeram;

    if (base_free > end_free && base_free - end_free > SLACK) {
        fail("free memory fell across fork churn: a per-fork resource leaks");
    }

    puts("FORKLEAK OK");
    sys_exit(0);
}
