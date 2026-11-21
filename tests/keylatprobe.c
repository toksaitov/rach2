/**
 * Reads Enter from the terminal, one key typed for each read, and exits, so
 * the kernel's record of each keystroke's path holds nothing but the
 * kernel's own work.
 *
 * The terminal is put in noncanonical mode without ICRNL or echo for the
 * reads, and put back after them, so that each read returns the bytes of one
 * key as the console's keymap sends them: Enter must be one carriage
 * return, as on a terminal, and any other byte fails the probe.
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
 * The child runs under SCHED_IDLE, so that every wake of the reader asks for
 * the switch the records count. Beside a child of the reader's own policy and
 * weight, EEVDF decides: a reader whose last dequeue left it no lag is placed
 * at the queue's average with a deadline a whole slice on, the child keeps the
 * processor while its own deadline is earlier, and the reader waits up to a
 * base slice, as Linux's check_preempt_wakeup_fair and pick_eevdf decide
 * (kernel/sched/fair.c); whether its lag is zero depends on how long it ran
 * for the key before, which varies with the host's speed. A thread of a
 * normal policy always preempts a SCHED_IDLE one at its wake.
 *
 * Before `KEYLAT READING` the probe yields until /proc/stat counts at most
 * three processes running or ready: itself, the child and process 0, which
 * the debug shell leaves counted once it has started the probe. The kernel
 * threads that the probe's start wakes run their first passes meanwhile; a
 * key that came while one ran would wait for its next sleep or service point,
 * since a kernel thread is not switched in the middle of its work, as in
 * Linux without CONFIG_PREEMPTION.
 *
 * Usage: keylatprobe [spin | random] [LINES]. With `random` the child is
 * inside a large `getrandom` instead of spinning, which is the loop inside
 * one system call that the scheduler's service point must break. LINES, 1
 * to 99 and 1 when not given, is how many keys are read, each in a read of
 * its own; the kernel prints a record for each.
 *
 * Prints "KEYLAT OK" and exits 0. Freestanding; direct system calls (probe.h).
 */

#include "probe.h"

#define STDIN_FILENO  0
#define STDOUT_FILENO 1

#define GRND_INSECURE 0x0004u

#define SIGKILL 9

#define SCHED_IDLE 5

#if defined __x86_64__
#define SYS_SCHED_SETSCHEDULER 144
#else
#define SYS_SCHED_SETSCHEDULER 156
#endif

/* The terminal's requests and the flags the reads clear (<asm/termbits.h>). */
#define TCGETS 0x5401
#define TCSETS 0x5402
#define ICRNL  0000400
#define ICANON 0000002
#define ECHO   0000010
#define VTIME  5
#define VMIN   6

/* Linux's struct termios of i386 and x86-64, as TCGETS gives it. */
struct kernel_termios {
    unsigned int c_iflag;
    unsigned int c_oflag;
    unsigned int c_cflag;
    unsigned int c_lflag;
    unsigned char c_line;
    unsigned char c_cc[19];
};

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

/* /proc/stat, read whole; its `procs_running` line is near its end. */
static char stat_text[16384];

/**
 * The count `procs_running` of /proc/stat, the processes running or ready
 * to run, or -1 when the file cannot be read or has no such line. A `\0`
 * ends the text, so a file longer than the buffer is searched as far as it
 * was read.
 */
static int procs_running(void)
{
    static const char key[] = "\nprocs_running ";
    long fd = syscall3(SYS_OPEN, (long)"/proc/stat", 0, 0);
    long got = 0, n;
    int i, j, value = -1;

    if (fd < 0) {
        return -1;
    }
    while (got < (long)sizeof(stat_text) - 1 &&
           (n = syscall3(SYS_READ, fd, (long)(stat_text + got),
                         (long)sizeof(stat_text) - 1 - got)) > 0) {
        got += n;
    }
    syscall1(SYS_CLOSE, fd);
    stat_text[got] = '\0';
    for (i = 0; stat_text[i]; i++) {
        for (j = 0; key[j] && stat_text[i + j] == key[j]; j++) {
        }
        if (key[j] == '\0') {
            for (i += j, value = 0; stat_text[i] >= '0' && stat_text[i] <= '9'; i++) {
                value = value * 10 + (stat_text[i] - '0');
            }
            break;
        }
    }
    return value;
}

PROBE_SP_ENTRY;

/**
 * The child's loop, which never returns: a spin in user mode or, when
 * `random_mode` is set, a `getrandom` of eight mebibytes made again as soon
 * as it returns, its buffer touched first so that no page fault falls
 * inside the call's time. It writes a byte to `started`, a pipe's write end,
 * once it is in its loop, after the first page faults of its pages after the
 * fork, and in the `random` mode after it has printed KEYLAT HOG.
 */
static void child_loop(int random_mode, int started)
{
    unsigned off;
    int n;

    if (random_mode) {
        for (off = 0; off < sizeof(random_buf); off += 4096) {
            random_buf[off] = 1;
        }
        for (n = 0;; n++) {
            if (syscall3(SYS_GETRANDOM, (long)random_buf, (int)sizeof(random_buf), GRND_INSECURE) !=
                (int)sizeof(random_buf)) {
                puts("KEYLAT FAIL: getrandom did not fill the buffer\n");
                sys_exit(1);
            }
            if (n == 0) {
                puts("KEYLAT HOG\n");
                syscall3(SYS_WRITE, started, (long)"r", 1);
            }
        }
    }
    syscall3(SYS_WRITE, started, (long)"s", 1);
    for (;;) {
        __asm__ __volatile__("" ::: "memory");
    }
}

/**
 * Wait until `child` says on the pipe `ready` that it is in its loop, then
 * yield until /proc/stat counts at most three processes running or ready:
 * this one, the child and process 0. Returns 0, or -1 with the reason
 * printed, the child killed.
 */
static int settle(int child, const int ready[2])
{
    char started;
    int running;

    syscall1(SYS_CLOSE, ready[1]);
    if (syscall3(SYS_READ, ready[0], (long)&started, 1) != 1) {
        syscall3(SYS_KILL, child, SIGKILL, 0);
        puts("KEYLAT FAIL: the child did not say it had started\n");
        return -1;
    }
    syscall1(SYS_CLOSE, ready[0]);
    while ((running = procs_running()) > 3) {
        syscall0(SYS_SCHED_YIELD);
    }
    if (running < 0) {
        syscall3(SYS_KILL, child, SIGKILL, 0);
        puts("KEYLAT FAIL: /proc/stat gives no count of running processes\n");
        return -1;
    }
    return 0;
}

/**
 * Bind to processor 0, fork the child that keeps the processor and put it
 * under SCHED_IDLE, wait for it to start and for the other work to settle,
 * put the terminal in noncanonical mode, print KEYLAT READING and read the
 * keys; then put the terminal back, kill the child and report.
 *
 * The child never sleeps, so the reader runs only if something takes the
 * processor from it. In the `random` mode the child is inside a system call
 * nearly all the time; a reader woken then runs only if the call gives way.
 * The child prints KEYLAT HOG once, so that the suite sends the key only
 * once the child is in its loop.
 *
 * The parent waits for the child's byte on a pipe: a page fault takes a
 * switch only at its end, and the child's first faults after the fork, the
 * copies of the pages it writes, would otherwise come with the first key.
 *
 * A pipe, a fork or a change of policy that fails fails the probe: without
 * the child nothing is on the processor to be taken off it, and a fast read
 * would prove nothing; without SCHED_IDLE, a slow one would not show a fault.
 */
void probe_main(unsigned long *sp)
{
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    int random_mode = argc > 1 && argv[1][0] == 'r';
    int lines = argc > 2 ? lines_wanted(argv[2]) : 1;
    static const unsigned long processor0 = 1;
    static const int idle_param = 0; /* struct sched_param, priority 0 */
    struct kernel_termios saved, raw;
    int not_cr = 0;
    int ready[2];
    int child;
    int n;
    int i;

    if (lines == 0) {
        puts("KEYLAT FAIL: the count of lines is not a number from 1 to 99\n");
        sys_exit(1);
    }
    if (syscall3(SYS_SCHED_SETAFFINITY, 0, (long)sizeof(processor0), (long)&processor0) != 0) {
        puts("KEYLAT FAIL: the probe could not be bound to processor 0\n");
        sys_exit(1);
    }
    if (syscall1(SYS_PIPE, (long)ready) != 0) {
        puts("KEYLAT FAIL: no pipe could be made\n");
        sys_exit(1);
    }
    child = syscall3(SYS_FORK, 0, 0, 0);
    if (child < 0) {
        puts("KEYLAT FAIL: no child could be forked\n");
        sys_exit(1);
    }
    if (child == 0) {
        child_loop(random_mode, ready[1]);
    }
    if (syscall3(SYS_SCHED_SETSCHEDULER, child, SCHED_IDLE, (long)&idle_param) != 0) {
        syscall3(SYS_KILL, child, SIGKILL, 0);
        puts("KEYLAT FAIL: the child could not be put under SCHED_IDLE\n");
        sys_exit(1);
    }
    if (settle(child, ready) != 0) {
        sys_exit(1);
    }

    if (syscall3(SYS_IOCTL, STDIN_FILENO, TCGETS, (long)&saved) != 0) {
        syscall3(SYS_KILL, child, SIGKILL, 0);
        puts("KEYLAT FAIL: standard input is not a terminal\n");
        sys_exit(1);
    }
    raw = saved;
    raw.c_iflag &= ~(unsigned int)ICRNL;
    raw.c_lflag &= ~(unsigned int)(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    syscall3(SYS_IOCTL, STDIN_FILENO, TCSETS, (long)&raw);

    puts("KEYLAT READING\n");
    n = 1;
    for (i = 0; i < lines && n > 0; i++) {
        n = syscall3(SYS_READ, STDIN_FILENO, (long)line, (int)sizeof(line));
        if (n > 0 && (n != 1 || line[0] != '\r')) {
            not_cr = 1;
        }
    }
    syscall3(SYS_IOCTL, STDIN_FILENO, TCSETS, (long)&saved);
    if (syscall3(SYS_KILL, child, SIGKILL, 0) != 0) {
        puts("KEYLAT FAIL: the child would not be killed\n");
        sys_exit(1);
    }

    if (n <= 0) {
        puts("KEYLAT FAIL: the read returned nothing\n");
        sys_exit(1);
    }
    if (not_cr) {
        puts("KEYLAT FAIL: a key read was not one carriage return\n");
        sys_exit(1);
    }
    puts("KEYLAT OK\n");
    sys_exit(0);
}
