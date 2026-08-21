/**
 * Proves the kernel reparents orphans onto init instead of leaking them.
 *
 * Run as init (the test image boots into the kernel debug shell, which `exec`s
 * this as the init process). Each round forks a child that forks a grandchild
 * and then exits immediately, orphaning the grandchild while it is still the
 * kernel's to track. The kernel must reparent that grandchild onto init (this
 * probe), so the probe reaps BOTH the child and the grandchild: two reaps per
 * round. Without reparenting the grandchild's parent names the dead child, the
 * probe's `waitpid(-1)` never matches it, the round reaps only one, and the
 * grandchild leaks its frames, page directory, and zombie slot.
 *
 * The reaps block, so each round's processes run and exit before the next
 * round, keeping the live set tiny; ROUNDS far exceeds MAX_PROCS, so a leak
 * would also wedge the table. Prints "ORPHANPROBE OK" only when every round
 * reaped both.
 *
 * Freestanding; direct int 0x80.
 */

typedef unsigned int size_t;

#define STDOUT_FILENO 1

#define SYS_EXIT    1
#define SYS_FORK    2
#define SYS_WRITE   4
#define SYS_WAITPID 7

/* Far past MAX_PROCS (16): a per-round orphan leak fills the table well inside
 * this count, on top of the per-round two-reaps check. */
#define ROUNDS 40

static int sys_write(int fd, const void *buf, size_t count)
{
    int result;
    __asm__ __volatile__("int $0x80"
                         : "=a"(result)
                         : "0"(SYS_WRITE), "b"(fd), "c"(buf), "d"(count)
                         : "memory");
    return result;
}

static void sys_exit(int status)
{
    __asm__ __volatile__("int $0x80" : : "a"(SYS_EXIT), "b"(status));
    __builtin_unreachable();
}

static int sys_fork(void)
{
    int result;
    __asm__ __volatile__("int $0x80" : "=a"(result) : "0"(SYS_FORK));
    return result;
}

static int sys_waitpid(int pid, int *status, int options)
{
    int result;
    __asm__ __volatile__("int $0x80"
                         : "=a"(result)
                         : "0"(SYS_WAITPID), "b"(pid), "c"(status), "d"(options)
                         : "memory");
    return result;
}

static size_t strlen(const char *s)
{
    size_t len = 0;
    while (s[len]) {
        len++;
    }
    return len;
}

static void puts(const char *s)
{
    sys_write(STDOUT_FILENO, s, strlen(s));
    sys_write(STDOUT_FILENO, "\n", 1);
}

static void print(const char *s)
{
    sys_write(STDOUT_FILENO, s, strlen(s));
}

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

void _start(void)
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

        /* Block-reap both this round's processes: the child directly, and the
         * grandchild only because the kernel reparented it onto us. The
         * blocking waits also yield the CPU so the round runs to completion
         * before the next fork, keeping the live set to a handful. */
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
