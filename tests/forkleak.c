/**
 * forkleak: fork, exit and wait do not leak physical memory.
 *
 * Each fork gives the child a kernel stack, which comes from the kernel heap
 * (`kstack_alloc` in syscall_proc.c), and page tables and a record, and the
 * child's reap gives them back. A kernel that kept any of them would lose
 * frames at every cycle, and the kernel's own respawning of processes would
 * drain memory until fork failed. The probe reads sysinfo's freeram after a
 * warm-up, runs CYCLES fork, exit and wait cycles, and requires the free
 * memory not to have fallen by more than SLACK.
 *
 * Prints "FORKLEAK OK" and exits 0, or "FORKLEAK FAIL: " with the reason and
 * exits 1. Freestanding; direct system calls (probe.h).
 */

#include "probe.h"

#define STDOUT_FILENO 1

#define WARMUP 8
#define CYCLES 120
/* The fall in free memory allowed, in bytes: well below the 480 KiB a leak
 * of a page a cycle would cost, and well above the jitter of the allocator. */
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

/** write(2). */
static int sys_write(int fd, const void *buf, size_t count)
{
    return syscall3(SYS_WRITE, (long)fd, (long)buf, (long)count);
}

/** exit(2); does not return. */
static void sys_exit(int status)
{
    syscall1(SYS_EXIT, (long)status);
    __builtin_unreachable();
}

/** fork(2). */
static int sys_fork(void)
{
    return syscall0(SYS_FORK);
}

/** waitpid(2). */
static int sys_waitpid(int pid, int *status, int options)
{
    return syscall3(SYS_WAITPID, (long)pid, (long)status, (long)options);
}

/** sysinfo(2). */
static int sys_sysinfo(struct sysinfo *si)
{
    return syscall1(SYS_SYSINFO, (long)si);
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

/** Write the string `s` and a newline to standard output. */
static void puts(const char *s)
{
    sys_write(STDOUT_FILENO, s, strlen(s));
    sys_write(STDOUT_FILENO, "\n", 1);
}

/** Print "FORKLEAK FAIL: `why`" and exit 1. */
static void fail(const char *why)
{
    sys_write(STDOUT_FILENO, "FORKLEAK FAIL: ", 15);
    puts(why);
    sys_exit(1);
}

/** One cycle of fork, exit(0) and wait; a fork that fails fails the probe. */
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

/** The probe's entry: the warm-up, the count, the cycles and the count. */
PROBE_START void _start(void)
{
    struct sysinfo si;
    unsigned long base_free, end_free;
    int i;

    /* What the first forks take for good is taken before the count. */
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
