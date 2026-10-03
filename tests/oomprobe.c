/**
 * Runs the machine out of physical frames, and proves it recovers.
 *
 * A process that touches more pages than there are frames cannot be served:
 * the fault handler has nothing to map, so it kills that process with
 * `SIGKILL`, as Linux's OOM killer does, and not with the SIGSEGV of a bad
 * address, which a handler could catch and return from to fault again. The
 * alternative, a kernel that panics or that hands out a frame it does not
 * have, is the failure this looks for.
 *
 * The child does the touching, so the parent is still there afterwards to
 * check the other half: the frames the child held come back, and the parent's
 * own allocation and touch succeed. A kernel that leaked the dead process's
 * frames would fail that second half while passing the first.
 *
 * Prints "OOMPROBE OK" and exits 0. Freestanding; direct system calls
 * (probe.h).
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

/* One mapping at a time, large enough that a handful of them outruns any
   machine this boots on, and small enough that the kernel's own limit on
   mappings per process is not what stops it. */
#define SPAN  (4u * 1024u * 1024u)
#define SPANS 64

static void *sys_mmap(size_t len)
{
    return (void *)probe_mmap(0, (long)len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1,
                              0);
}

static void sys_exit(int status)
{
    syscall1(SYS_EXIT, (long)status);
    __builtin_unreachable();
}

static int sys_fork(void)
{
    return syscall0(SYS_FORK);
}

static size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

static void print(const char *s)
{
    syscall3(SYS_WRITE, STDOUT_FILENO, (long)s, (int)strlen(s));
}

static void puts(const char *s)
{
    print(s);
    syscall3(SYS_WRITE, STDOUT_FILENO, (long)"\n", 1);
}

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

/* Map and touch until the machine cannot serve another page. Returns only if
   the memory never ran out, which is itself a failure. */
static void eat_memory(void)
{
    int i;
    unsigned int p;

    for (i = 0; i < SPANS; i++) {
        char *m = (char *)sys_mmap(SPAN);

        if ((size_t)m > (size_t)-4096) {
            /* The mapping itself was refused, which is the other way a
               machine can run out. Say so by exiting, not by faulting. */
            sys_exit(3);
        }
        for (p = 0; p < SPAN; p += PAGE) {
            m[p] = (char)i;
        }
    }
    sys_exit(4);
}

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

    /* The frames came back with the child. This is the half a kernel that
       leaked them would fail. */
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
