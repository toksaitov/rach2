/*
 * calibprobe: fixed work for the runner's calibration of the emulator's
 * rate, timed by the host's clock around each part.
 *
 * * `compute`: a 32-bit xorshift generator run for a fixed count, the
 *   last value printed so the work is not optimised away; the same
 *   instructions every time, so a slower run is the emulator's rate.
 * * `storage`: 32 MiB read raw from `/dev/disk0` in 32 KiB reads, the
 *   whole-disk node the boot publishes under `rach2.diskdiag`.
 *
 * Each part prints `CALIB PART DONE` and `CALIBPROBE OK`, or `CALIBPROBE
 * FAIL: reason`. Freestanding; direct system calls (probe.h).
 */

#include "probe.h"

#define O_RDONLY 0

static void put(const char *s)
{
    int n = 0;

    while (s[n]) {
        n++;
    }
    syscall3(SYS_WRITE, 1, (long)s, n);
}

static void putnum(unsigned v)
{
    char buf[12];
    int i = 11;

    buf[i] = 0;
    do {
        buf[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v != 0);
    put(&buf[i]);
}

static void fail(const char *why)
{
    put("CALIBPROBE FAIL: ");
    put(why);
    put("\n");
    syscall1(SYS_EXIT, 1);
}

static int same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* The count is what an idle emulator runs in about a second on the
   reference host; the host's clock around the run is the measurement. */
#define COMPUTE_ROUNDS 100000000u

static void part_compute(void)
{
    unsigned x = 2463534242u;
    unsigned i;

    for (i = 0; i < COMPUTE_ROUNDS; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
    }
    put("CALIB COMPUTE DONE ");
    putnum(x);
    put("\n");
}

static char big[32768];

static void part_storage(void)
{
    int fd = syscall3(SYS_OPEN, (long)"/dev/disk0", O_RDONLY, 0);
    unsigned total = 0;

    if (fd < 0) {
        fail("/dev/disk0 would not open; is rach2.diskdiag on?");
    }
    while (total < 32u * 1024u * 1024u) {
        int n = syscall3(SYS_READ, fd, (long)big, (int)sizeof(big));

        if (n <= 0) {
            fail("a read of /dev/disk0 failed before 32 MiB");
        }
        total += (unsigned)n;
    }
    syscall1(SYS_CLOSE, fd);
    put("CALIB STORAGE DONE\n");
}

void probe_main(unsigned int *sp);

__asm__(".global _start\n"
        "_start:\n"
        "   mov %esp, %eax\n"
        "   push %eax\n"
        "   call probe_main\n");

void probe_main(unsigned int *sp)
{
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    const char *what = argc > 1 ? argv[1] : "";

    if (same(what, "compute")) {
        part_compute();
    } else if (same(what, "storage")) {
        part_storage();
    } else {
        fail("no such part; compute or storage");
    }
    put("CALIBPROBE OK\n");
    syscall1(SYS_EXIT, 0);
}
