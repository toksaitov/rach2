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
 * message interrupted. `KEYLAT READING` is printed before the read, and the
 * kernel, with `rach2.usblatency` on, says when the reader has parked; the
 * suite types on the kernel's line, so the key finds a parked reader and
 * not one still printing. With the child on the processor, a wake that does not
 * reschedule leaves the reader waiting for the next tick, and the record says
 * so.
 *
 * With the argument `random` the child is inside a large `getrandom`
 * instead of spinning, which is the loop inside one system call that the
 * scheduler's service point must break.
 *
 * Prints "KEYLAT OK" and exits 0. Freestanding; direct system calls (probe.h).
 */

#include "probe.h"

#define STDIN_FILENO  0
#define STDOUT_FILENO 1

#define GRND_INSECURE 0x0004u

#define SIGKILL 9

static char line[64];

static void sys_exit(int status)
{
    syscall1(SYS_EXIT, (long)status);
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
    syscall3(SYS_WRITE, STDOUT_FILENO, (long)s, (int)strlen(s));
}

/* The child's buffer in the `random` mode: a call that fills it is the
   loop inside one system call that the service point must break. */
static unsigned char random_buf[8u << 20];

__asm__(".global _start\n"
        "_start:\n"
        "   mov %esp, %eax\n"
        "   push %eax\n"
        "   call probe_main\n");

void probe_main(unsigned *sp)
{
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    int random_mode = argc > 1 && argv[1][0] == 'r';
    int child;
    int n;
    unsigned off;

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
        /* The child keeps the processor: it never sleeps, so the reader
           runs only if something takes the processor from it. In the
           `random` mode it is inside a system call nearly all the time, a
           `getrandom` of eight mebibytes made again as soon as it returns,
           touched first so no page fault is inside the call's time; a
           reader woken then runs only if the call gives way. */
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
                    /* Said once, so the row knows the tenant is in its loop
                       before the key is sent. */
                    puts("KEYLAT HOG\n");
                }
            }
        }
        for (;;) {
            __asm__ __volatile__("" ::: "memory");
        }
    }

    puts("KEYLAT READING\n");
    n = syscall3(SYS_READ, STDIN_FILENO, (long)line, (int)sizeof(line));
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
