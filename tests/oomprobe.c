/**
 * oomprobe: run the machine out of physical frames, and see it recover.
 *
 * A process that touches more pages than there are frames cannot be served:
 * the fault handler has nothing to map, so it kills that process with
 * `SIGKILL`, as Linux's OOM killer does, and not with the SIGSEGV of a bad
 * address, which a handler could catch and return from to fault again. A
 * kernel that panicked, or that handed out a frame it does not have, fails.
 *
 * A child does the touching, so that the parent is there afterwards to
 * check the other half: the frames the child held come back, and the
 * parent's own mapping and touch succeed. A kernel that leaked the dead
 * process's frames would fail that half while passing the first.
 *
 * Prints "OOMPROBE OK (...)" and exits 0, or "OOMPROBE FAIL: " with the
 * reason and exits 1. Freestanding; direct system calls (probe.h).
 */

#include "probe.h"

#define STDOUT_FILENO 1

#define PROT_READ     0x1
#define PROT_WRITE    0x2
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20

#define PAGE    4096u
#define SIGKILL 9

#define WIFEXITED(s)   (((s) & 0x7F) == 0)
#define WIFSIGNALED(s) (((s) & 0x7F) != 0 && ((s) & 0x7F) != 0x7F)
#define WTERMSIG(s)    ((s) & 0x7F)

/* SPANS mappings of SPAN, 256 MiB in all, more than the machines the probe
 * boots on have; each is large enough that their number stays well inside
 * the kernel's limit on mappings per process. */
#define SPAN  (4u * 1024u * 1024u)
#define SPANS 64

/** An anonymous private mapping of `len` bytes: its address, or a negated
 * errno cast to a pointer. */
static void *sys_mmap(size_t len)
{
    return (void *)probe_mmap(0, (long)len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1,
                              0);
}

/** exit(2); does not return. */
static void sys_exit(int status)
{
    syscall1(SYS_EXIT, (long)status);
    __builtin_unreachable();
}

/** fork(2). */
static int sys_fork(void)
{
    return syscall0(SYS_FORK);
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

/** Write the string `s` to standard output. */
static void print(const char *s)
{
    syscall3(SYS_WRITE, STDOUT_FILENO, (long)s, (int)strlen(s));
}

/** Write the string `s` and a newline to standard output. */
static void puts(const char *s)
{
    print(s);
    syscall3(SYS_WRITE, STDOUT_FILENO, (long)"\n", 1);
}

/** Write `v` in decimal to standard output. */
static void print_num(int v)
{
    char buf[12];
    int i = 0;

    if (v < 0) {
        print("-");
        v = -v;
    }
    if (v == 0) {
        buf[i++] = '0';
    }
    while (v > 0) {
        buf[i++] = (char)('0' + v % 10);
        v /= 10;
    }
    while (i > 0) {
        syscall3(SYS_WRITE, STDOUT_FILENO, (long)&buf[--i], 1);
    }
}

/** Map and touch until the machine cannot serve another page, which kills
 * the process. Exits 3 when a mapping is refused, the other way a machine
 * can run out, and 4 when the memory never ran out; does not return. */
static void eat_memory(void)
{
    int i;
    unsigned int p;

    for (i = 0; i < SPANS; i++) {
        char *m = (char *)sys_mmap(SPAN);

        if ((size_t)m > (size_t)-4096) {
            sys_exit(3);
        }
        for (p = 0; p < SPAN; p += PAGE) {
            m[p] = (char)i;
        }
    }
    sys_exit(4);
}

/** The probe's entry: the child runs out of frames, then the parent maps
 * and touches SPAN bytes of its own. */
PROBE_START void _start(void)
{
    int pid, status;
    char *m;
    unsigned int p;

    pid = sys_fork();
    if (pid == 0) {
        eat_memory();
    }
    if (pid < 0) {
        puts("OOMPROBE FAIL: the child could not be started");
        sys_exit(1);
    }
    if (syscall3(SYS_WAITPID, pid, (long)&status, 0) != pid) {
        puts("OOMPROBE FAIL: the child could not be reaped");
        sys_exit(1);
    }
    if (WIFEXITED(status)) {
        print("OOMPROBE FAIL: the child exited ");
        print_num((int)((status >> 8) & 0xFF));
        puts(" (3 the mapping was refused, 4 the memory never ran out)");
        sys_exit(1);
    }
    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL) {
        print("OOMPROBE FAIL: the child died of signal ");
        print_num((int)WTERMSIG(status));
        puts(", not SIGKILL");
        sys_exit(1);
    }

    /* The frames came back with the child. */
    m = (char *)sys_mmap(SPAN);
    if ((size_t)m > (size_t)-4096) {
        puts("OOMPROBE FAIL: no mapping after the child died");
        sys_exit(1);
    }
    for (p = 0; p < SPAN; p += PAGE) {
        m[p] = 'x';
    }
    for (p = 0; p < SPAN; p += PAGE) {
        if (m[p] != 'x') {
            puts("OOMPROBE FAIL: a page written after the child did not read back");
            sys_exit(1);
        }
    }

    puts("OOMPROBE OK (the child was killed with SIGKILL and its frames came back)");
    sys_exit(0);
}
