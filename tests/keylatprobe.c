/**
 * Reads lines from the terminal, one key typed for each, and exits, so the
 * kernel's record of each keystroke's path holds nothing but the kernel's own
 * work.
 *
 * The stages the kernel stamps end at the reader's read. A program that
 * writes between its reads puts its own time into that number: the key
 * arrives while the program is still on the serial port with the last line,
 * and the read that follows is late for a reason that has nothing to do with
 * the interrupt. This one blocks in each read and prints only after the
 * last, so the number in each record is the wake and the scheduler and
 * nothing else.
 *
 * A child is forked first and left spinning in user mode, because the number
 * that matters is what happens to a reader that is not the context the
 * message interrupted. `KEYLAT READING` is printed before the read, and the
 * kernel, with `rach2.usblatency` on, says when the reader has parked; the
 * suite types on the kernel's line, so the key finds a parked reader and
 * not one still printing. With the child on the processor, a wake that does not
 * reschedule leaves the reader waiting for the next tick, and the record says
 * so. Both are bound to processor 0 first: the controller's interrupts go to
 * the processor that programmed its message address, the boot processor,
 * before the others start, and a reader woken on an idle processor would run
 * there without taking anything from the child.
 *
 * Usage: keylatprobe [spin | random] [LINES]. With `random` the child is
 * inside a large `getrandom` instead of spinning, which is the loop inside
 * one system call that the scheduler's service point must break. LINES, 1
 * to 99 and 1 when not given, is how many lines are read, each in a read of
 * its own; the kernel prints a record for each.
 *
 * Prints "KEYLAT OK" and exits 0. Freestanding; direct system calls (probe.h).
 */

#include "probe.h"

#define STDIN_FILENO  0
#define STDOUT_FILENO 1

#define GRND_INSECURE 0x0004u

#define SIGKILL 9

static char line[64];

/** exit(2) with `status`; does not return. */
static void sys_exit(int status)
{
    syscall1(SYS_EXIT, (long)status);
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
static void puts(const char *s)
{
    syscall3(SYS_WRITE, STDOUT_FILENO, (long)s, (int)strlen(s));
}

/** The decimal number `s`, from 1 to 99, or 0 when it is not one. */
static int lines_wanted(const char *s)
{
    int n = 0;

    while (*s >= '0' && *s <= '9' && n < 100) {
        n = n * 10 + (*s++ - '0');
    }
    return *s == '\0' && n >= 1 && n <= 99 ? n : 0;
}

/* The child's buffer in the `random` mode: a call that fills it is the
   loop inside one system call that the service point must break. */
static unsigned char random_buf[8u << 20];

PROBE_SP_ENTRY;

/**
 * Bind to processor 0, fork the child that keeps the processor, print
 * KEYLAT READING and read the lines; then kill the child and report.
 *
 * The child never sleeps, so the reader runs only if something takes the
 * processor from it. In the `random` mode the child is inside a system call
 * nearly all the time, a `getrandom` of eight mebibytes made again as soon as
 * it returns, its buffer touched first so that no page fault falls inside the
 * call's time; a reader woken then runs only if the call gives way. It
 * prints KEYLAT HOG once, so that the suite sends the key only once the
 * child is in its loop. A fork that fails fails the probe: without the child
 * nothing is on the processor to be taken off it, and a fast read would
 * prove nothing.
 */
void probe_main(unsigned long *sp)
{
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    int random_mode = argc > 1 && argv[1][0] == 'r';
    int lines = argc > 2 ? lines_wanted(argv[2]) : 1;
    static const unsigned long processor0 = 1;
    int child;
    int n;
    int i;
    unsigned off;

    if (lines == 0) {
        puts("KEYLAT FAIL: the count of lines is not a number from 1 to 99\n");
        sys_exit(1);
    }
    if (syscall3(SYS_SCHED_SETAFFINITY, 0, (long)sizeof(processor0), (long)&processor0) != 0) {
        puts("KEYLAT FAIL: the probe could not be bound to processor 0\n");
        sys_exit(1);
    }
    child = syscall3(SYS_FORK, 0, 0, 0);
    if (child < 0) {
        puts("KEYLAT FAIL: no child could be forked\n");
        sys_exit(1);
    }
    if (child == 0) {
        if (random_mode) {
            for (off = 0; off < sizeof(random_buf); off += 4096) {
                random_buf[off] = 1;
            }
            for (n = 0;; n++) {
                if (syscall3(SYS_GETRANDOM, (long)random_buf, (int)sizeof(random_buf),
                             GRND_INSECURE) != (int)sizeof(random_buf)) {
                    puts("KEYLAT FAIL: getrandom did not fill the buffer\n");
                    sys_exit(1);
                }
                if (n == 0) {
                    puts("KEYLAT HOG\n");
                }
            }
        }
        for (;;) {
            __asm__ __volatile__("" ::: "memory");
        }
    }

    puts("KEYLAT READING\n");
    n = 1;
    for (i = 0; i < lines && n > 0; i++) {
        n = syscall3(SYS_READ, STDIN_FILENO, (long)line, (int)sizeof(line));
    }
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
