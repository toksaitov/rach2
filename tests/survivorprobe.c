/**
 * An init that exits with a runnable child returns to the debug shell.
 *
 * The probe forks a child that spins for a bounded count and then exits,
 * and exits itself while the child is still runnable. The kernel must
 * return to the debug shell at once: an exit that first scheduled would
 * give the child the processor, and the exited init, a zombie, would never
 * be picked again to make the return, so the shell never came back and the
 * runner scored a hang. The child is frozen while the shell has the
 * machine, finishes under the next program the shell starts, and stays a
 * zombie, since its parent is gone; one record of the process table, on a
 * machine that is discarded after the run.
 *
 * Prints "SURVIVORPROBE OK" before exiting. The verdict is the kernel's
 * return record, which the runner requires after the probe's own exit.
 * Freestanding; direct int 0x80.
 */

typedef unsigned int size_t;

#define SYS_EXIT  1
#define SYS_FORK  2
#define SYS_WRITE 4

/* Spins of the child: long enough for the parent to exit meanwhile on any
   machine, short enough to end under the next program. */
#define SPINS 20000000u

static int sys1(int n, int a)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a));
    return r;
}

static int sys3(int n, int a, int b, int c)
{
    int r;
    __asm__ __volatile__("int $0x80" : "=a"(r) : "0"(n), "b"(a), "c"(b), "d"(c));
    return r;
}

static size_t slen(const char *s)
{
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

static void print(const char *s)
{
    sys3(SYS_WRITE, 1, (int)(size_t)s, (int)slen(s));
}

void _start(void)
{
    int pid = sys1(SYS_FORK, 0);

    if (pid < 0) {
        print("SURVIVORPROBE FAIL: fork\n");
        sys1(SYS_EXIT, 1);
    }
    if (pid == 0) {
        volatile unsigned i;

        for (i = 0; i < SPINS; i++) {
        }
        sys1(SYS_EXIT, 0);
    }
    print("SURVIVORPROBE OK\n");
    sys1(SYS_EXIT, 0);
}
