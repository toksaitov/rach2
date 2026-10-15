/**
 * Reads the mouse device and requires a movement to arrive.
 *
 * The device is the one Doom opens, by the bare name `mouse` rather than a
 * path under `/dev`, and it is the same queue the PS/2 mouse fills. That is
 * the point: a program reading movement does not learn which kind of mouse
 * produced it. The suite moves a USB mouse through the emulator's own
 * monitor while this runs, so what arrives here came in over USB and out of
 * the shared queue.
 *
 * With the argument `any`, for a person at a machine, any movement in any
 * direction passes and the movement seen is printed; without it the suite's
 * rule holds: right and up, and right by more.
 *
 * Prints "MOUSEPROBE OK" and exits 0. Freestanding; direct system calls
 * (probe.h).
 */

#include "probe.h"

#define STDOUT_FILENO 1

#define O_RDONLY 0

/* Up to 32 packets as the kernel hands them out, three bytes each: the
 * buttons, then two signed movements. */
static unsigned char packet[3 * 32];

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

/** Print "MOUSEPROBE FAIL: " and `why`, and exit 1. */
static void fail(const char *why)
{
    puts("MOUSEPROBE FAIL: ");
    puts(why);
    puts("\n");
    sys_exit(1);
}

/** Write `v` in decimal. */
static void put_int(int v)
{
    char digits[12];
    int i = 0;

    if (v < 0) {
        puts("-");
        v = -v;
    }
    do {
        digits[i++] = (char)('0' + v % 10);
        v /= 10;
    } while (v > 0);
    while (i > 0) {
        char one[2];

        one[0] = digits[--i];
        one[1] = 0;
        puts(one);
    }
}

PROBE_SP_ENTRY;

/**
 * Open the mouse, read until a movement arrives or fifteen seconds pass, and
 * judge the movement summed over the packets read.
 *
 * Opening the device empties the queue, so the movement has to arrive after
 * the open: the suite moves the mouse while this is already reading, and a
 * person at a machine is told to. The wait is fifteen seconds in turns of ten
 * milliseconds, long enough for a hand and short enough for a suite that
 * never moves the mouse. The suite moves the mouse right by 20 and up by 8,
 * so that axes swapped for each other or a sign turned the wrong way fail
 * rather than pass as some movement. Up is positive here and negative on the
 * wire, which is the turn the class driver makes (rach2/sys/usb/hid.c).
 */
void probe_main(unsigned long *sp)
{
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    int any = argc >= 2 && argv[1][0] == 'a';
    int fd;
    int n;
    int tries;
    int moved = 0;
    int total_x = 0;
    int total_y = 0;

    fd = syscall3(SYS_OPEN, (long)"mouse", O_RDONLY, 0);
    if (fd < 0) {
        fail("the mouse device did not open");
    }

    puts("MOUSEPROBE: move the mouse");
    for (tries = 0; tries < 1500 && !moved; tries++) {
        static const long pause[2] = {0, 10000000};

        n = syscall3(SYS_READ, fd, (long)packet, (int)sizeof(packet));
        if (n < 0) {
            syscall3(SYS_CLOSE, fd, 0, 0);
            fail("the mouse device did not read");
        }
        if (n >= 3) {
            int i;

            for (i = 0; i + 2 < n; i += 3) {
                total_x += (int)(signed char)packet[i + 1];
                total_y += (int)(signed char)packet[i + 2];
                if (packet[i + 1] != 0 || packet[i + 2] != 0) {
                    moved = 1;
                }
            }
        }
        if (!moved) {
            syscall3(SYS_NANOSLEEP, (long)pause, 0, 0);
        }
    }
    syscall3(SYS_CLOSE, fd, 0, 0);

    if (!moved) {
        fail("no movement arrived");
    }
    if (any) {
        puts("MOUSEPROBE: moved x ");
        put_int(total_x);
        puts(" y ");
        put_int(total_y);
        puts("\n");
        puts("MOUSEPROBE OK\n");
        sys_exit(0);
    }
    if (total_x <= 0) {
        fail("the movement did not go right");
    }
    if (total_y <= 0) {
        fail("the movement did not go up");
    }
    if (total_x <= total_y) {
        fail("the movement did not go right by more than it went up, so the axes are swapped");
    }
    puts("MOUSEPROBE OK\n");
    sys_exit(0);
}
