/**
 * Fills a process's descriptor table, then proves a closed one is reusable.
 *
 * A process may hold a thousand and twenty-four descriptors, its table
 * growing from sixteen as it fills, and three are taken before it starts,
 * so a program that opens files in a loop meets the limit rather than being
 * told it. What matters is what happens there: the refusal has to be
 * `EMFILE`, the count has to be the limit less what was already open, and a
 * descriptor given back by `close` has to be usable again. A kernel that
 * leaked a slot per close would pass a test that only opened once.
 *
 * Prints "FDFULL OK" and exits 0. Freestanding; direct system calls (probe.h).
 */

#include "probe.h"

#define STDOUT_FILENO 1

#define O_RDONLY 0
#define EMFILE   24

/* More than the limit, so the loop below always reaches the refusal. */
#define TRIES 1040
#define LIMIT 1024

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

static int fds[TRIES];
static int n_fds;

static void close_all(void)
{
    int i;

    for (i = 0; i < n_fds; i++) {
        syscall3(SYS_CLOSE, fds[i], 0, 0);
    }
    n_fds = 0;
}

PROBE_START void _start(void)
{
    int fd, i, refused, opened;

    /* The same file every time: what is being filled is this process's table,
       not anything the file system holds. */
    refused = 0;
    for (i = 0; i < TRIES; i++) {
        fd = syscall3(SYS_OPEN, (long)"/hello", O_RDONLY, 0);
        if (fd < 0) {
            refused = fd;
            break;
        }
        fds[n_fds++] = fd;
    }

    if (refused == 0) {
        close_all();
        puts("FDFULL FAIL: the table never refused an open");
        sys_exit(1);
    }
    if (refused != -EMFILE) {
        print("FDFULL FAIL: the refusal was ");
        print_num(refused);
        puts(", not -EMFILE");
        close_all();
        sys_exit(1);
    }
    opened = n_fds;
    if (opened != LIMIT - 3) {
        close_all();
        puts("FDFULL FAIL: the count is not the limit less the three open at the start");
        sys_exit(1);
    }

    /* A closed descriptor is reusable at once. */
    if (syscall3(SYS_CLOSE, fds[0], 0, 0) != 0) {
        close_all();
        puts("FDFULL FAIL: a descriptor could not be closed");
        sys_exit(1);
    }
    fd = syscall3(SYS_OPEN, (long)"/hello", O_RDONLY, 0);
    if (fd < 0) {
        n_fds--;
        close_all();
        puts("FDFULL FAIL: a closed descriptor was not reusable");
        sys_exit(1);
    }
    fds[0] = fd;

    close_all();
    print("FDFULL OK (");
    print_num(opened);
    puts(" descriptors, refused with EMFILE, a closed one reused)");
    sys_exit(0);
}
