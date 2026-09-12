/**
 * Reads the mouse device and requires a movement to arrive.
 *
 * The device is the one Doom opens, by the bare name `mouse` rather than a
 * path under `/dev`, and it is the same queue the PS/2 mouse fills. That is
 * the point: a program reading movement does not learn which kind of mouse
 * produced it.
 * The suite moves a USB mouse through the emulator's own monitor before
 * running this, so what arrives here came in over USB and out of the shared
 * queue.
 *
 * With the argument `any`, for a person at a machine, any movement in any
 * direction passes and the movement seen is printed; without it the suite's
 * rule holds, right and up by different amounts.
 *
 * Prints "MOUSEPROBE OK" and exits 0. Freestanding; direct int 0x80.
 */

typedef unsigned int size_t;

#define STDOUT_FILENO 1

#define SYS_EXIT      1
#define SYS_READ      3
#define SYS_WRITE     4
#define SYS_OPEN      5
#define SYS_CLOSE     6
#define SYS_NANOSLEEP 162

#define O_RDONLY 0

/* One packet as the kernel hands it out: buttons, then two signed movements. */
static unsigned char packet[3 * 32];

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

static void fail(const char *why)
{
    puts("MOUSEPROBE FAIL: ");
    puts(why);
    puts("\n");
    sys_exit(1);
}

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

__asm__(".global _start\n"
        "_start:\n"
        "   mov %esp, %eax\n"
        "   push %eax\n"
        "   call probe_main\n");

void probe_main(unsigned int *sp)
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

    fd = syscall3(SYS_OPEN, (int)(size_t)"mouse", O_RDONLY, 0);
    if (fd < 0) {
        fail("the mouse device did not open");
    }

    /* Opening the device empties the queue, so the movement has to arrive
       after this point rather than before it: the suite moves the mouse while
       this is already reading, and a person at a machine is told to. The
       wait is fifteen seconds in turns of ten milliseconds, long enough for
       a hand and short enough for a suite that never moves the mouse. */
    puts("MOUSEPROBE: move the mouse");
    for (tries = 0; tries < 1500 && !moved; tries++) {
        static const int pause[2] = {0, 10000000};

        n = syscall3(SYS_READ, fd, (int)(size_t)packet, (int)sizeof(packet));
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
            syscall3(SYS_NANOSLEEP, (int)(size_t)pause, 0, 0);
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
    /* The suite moves the mouse right and up, by different amounts, so that
       axes swapped for each other or a sign turned the wrong way is a failure
       rather than a movement that still looks like one. Up is positive here
       and negative on the wire, which is the turn the class driver makes. */
    if (total_x <= 0) {
        fail("the movement did not go right");
    }
    if (total_y <= 0) {
        fail("the movement did not go up");
    }
    if (total_x == total_y) {
        fail("the two axes moved by the same amount, so neither is proved");
    }
    puts("MOUSEPROBE OK\n");
    sys_exit(0);
}
