/**
 * Reads one line from the terminal and exits, so the kernel's record of the
 * keystroke's path holds nothing but the kernel's own work.
 *
 * The stages the kernel stamps end at the reader's read. A program that
 * writes between its reads puts its own time into that number: the key
 * arrives while the program is still on the serial port with the last line,
 * and the read that follows is late for a reason that has nothing to do with
 * the interrupt. This one blocks on a single read and prints afterwards, so
 * the number in the record is the wake and the scheduler and nothing else.
 *
 * A child is forked first and left spinning in user mode, because the number
 * that matters is what happens to a reader that is not the context the
 * message interrupted. With the child on the processor, a wake that does not
 * reschedule leaves the reader waiting for the next tick, and the record says
 * so.
 *
 * Prints "KEYLAT OK" and exits 0. Freestanding; direct int 0x80.
 */

typedef unsigned int size_t;

#define STDIN_FILENO  0
#define STDOUT_FILENO 1

#define SYS_EXIT  1
#define SYS_FORK  2
#define SYS_READ  3
#define SYS_WRITE 4
#define SYS_KILL  37

#define SIGKILL 9

static char line[64];

static int syscall3(int n, int a, int b, int c)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}

static void sys_exit(int status)
{
    __asm__ __volatile__("int $0x80" : : "a"(SYS_EXIT), "b"(status));
    __builtin_unreachable();
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
    syscall3(SYS_WRITE, STDOUT_FILENO, (int)(size_t)s, (int)strlen(s));
}

void _start(void)
{
    int child;
    int n;

    child = syscall3(SYS_FORK, 0, 0, 0);
    if (child < 0) {
        /* Without the child there is nothing on the processor to be taken off
           it, and the number this program exists to produce would be the easy
           one. Better to fail than to report a fast read that proves
           nothing. */
        puts("KEYLAT FAIL: no child could be forked\n");
        sys_exit(1);
    }
    if (child == 0) {
        /* The processor's other tenant: it never sleeps, so the reader can
           only run if something takes the processor away from it. */
        for (;;) {
            __asm__ __volatile__("" ::: "memory");
        }
    }

    n = syscall3(SYS_READ, STDIN_FILENO, (int)(size_t)line, (int)sizeof(line));
    if (syscall3(SYS_KILL, child, SIGKILL, 0) != 0) {
        puts("KEYLAT FAIL: the child would not be killed\n");
        sys_exit(1);
    }

    if (n <= 0) {
        puts("KEYLAT FAIL: the read returned nothing\n");
        sys_exit(1);
    }
    puts("KEYLAT OK\n");
    sys_exit(0);
}
