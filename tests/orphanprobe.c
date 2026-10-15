/**
 * orphanprobe: the kernel gives orphans to init rather than leaking them.
 *
 * The probe runs as init: the test image boots into the kernel debug shell,
 * which `exec`s it as the init process. Each round forks a child that forks a
 * grandchild and exits at once, which leaves the grandchild an orphan while
 * the kernel still tracks it. The kernel must give that grandchild to init,
 * this probe, so the probe reaps both the child and the grandchild: two reaps
 * a round. A grandchild left with the dead child's pid as its parent would
 * never match the probe's `waitpid(-1)`, the round would reap only one, and
 * the grandchild would leak its frames, page directory and record.
 *
 * The reaps block, so each round's processes run and exit before the next
 * round begins. Prints "ORPHANPROBE OK (...)" and exits 0 when every round
 * reaped both; a failure prints "ORPHANPROBE FAIL (...)" and exits 1.
 * Freestanding; direct system calls (probe.h).
 *
 * Needs to be the kernel's designated init process. The debug shell's exec
 * gives it that role on either root type, and its pid need not be 1. An
 * ordinary shell or SSH child cannot reap init's orphans, so this probe runs
 * on a rescue boot, not in a login session.
 */

#include "probe.h"

#define STDOUT_FILENO 1

/* The rounds, each of which must reap two processes. */
#define ROUNDS 40

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

/** The length of the string `s`. */
static size_t strlen(const char *s)
{
    size_t len = 0;
    while (s[len]) {
        len++;
    }
    return len;
}

/** Write the string `s` and a newline to standard output. */
static void puts(const char *s)
{
    sys_write(STDOUT_FILENO, s, strlen(s));
    sys_write(STDOUT_FILENO, "\n", 1);
}

/** Write the string `s` to standard output. */
static void print(const char *s)
{
    sys_write(STDOUT_FILENO, s, strlen(s));
}

/** Write `n`, 0 or more, in decimal to standard output. */
static void print_num(int n)
{
    char buf[12];
    int i = 0;

    if (n == 0) {
        buf[i++] = '0';
    } else {
        while (n > 0) {
            buf[i++] = (char)('0' + (n % 10));
            n /= 10;
        }
    }
    while (i > 0) {
        sys_write(STDOUT_FILENO, &buf[--i], 1);
    }
}

/** The probe's entry: ROUNDS rounds of a child and an orphaned grandchild,
 * each reaped by this process. */
PROBE_START void _start(void)
{
    int i;
    int reaped = 0;
    int pid;
    int status;

    puts("Orphan-reaping test starting...");

    for (i = 0; i < ROUNDS; i++) {
        int got;

        pid = sys_fork();
        if (pid < 0) {
            print("ORPHANPROBE FAIL (fork failed at round ");
            print_num(i);
            puts("; the process table filled with leaked orphans)");
            sys_exit(1);
        }
        if (pid == 0) {
            int grandchild = sys_fork();
            if (grandchild == 0) {
                sys_exit(0); /* the orphan: exits to be reaped by init */
            }
            sys_exit(0); /* the child: exits, orphaning the grandchild */
        }

        /* The child, and the grandchild once the kernel gives it to init. */
        got = 0;
        while (got < 2 && sys_waitpid(-1, &status, 0) > 0) {
            got++;
            reaped++;
        }
        if (got != 2) {
            print("ORPHANPROBE FAIL (round ");
            print_num(i);
            puts(" reaped only the child; the orphan was not reparented to init)");
            sys_exit(1);
        }
    }

    print("ORPHANPROBE OK (");
    print_num(reaped);
    puts(" children and reparented orphans reaped, no leak)");
    sys_exit(0);
}
