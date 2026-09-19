/**
 * An init that exits with a runnable child returns to the debug shell.
 *
 * The probe forks a child that runs in user mode until the kernel ends it,
 * and exits while the child runs. The kernel ends the child and returns
 * to the shell: an exit that scheduled the child instead ran it and never
 * returned, since the exited init, a zombie, was never picked again, and a
 * return that left the child frozen would run it under the next program.
 *
 * Prints "SURVIVORPROBE OK" before exiting. The verdict is the kernel's
 * return record, which the runner requires after the probe's own exit.
 * Freestanding; direct int 0x80.
 */

typedef unsigned int size_t;

#define SYS_EXIT  1
#define SYS_FORK  2
#define SYS_WRITE 4

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
        for (;;) {
        }
    }
    print("SURVIVORPROBE OK\n");
    sys1(SYS_EXIT, 0);
}
