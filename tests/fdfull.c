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
 * Prints "FDFULL OK" and exits 0. Writes etude's report records
 * (probe_report.h) for the case ETUDE_CASE names, an assertion for each step:
 * `refused` (the table refused an open), `emfile` (with EMFILE), `count` (the
 * descriptors opened before it), `close-one`, `reuse` (the closed descriptor
 * is opened again) and `close-all`; a failure leaves the steps after it not
 * run. Freestanding; direct system calls (probe.h).
 */

#include "probe.h"
#include "probe_report.h"

#define STDOUT_FILENO 1

#define O_RDONLY 0
#define EMFILE   24

/* More than the limit, so the loop below always reaches the refusal. */
#define TRIES 1040
#define LIMIT 1024

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
static void print(const char *s)
{
    syscall3(SYS_WRITE, STDOUT_FILENO, (long)s, (int)strlen(s));
}

/** Write `s` and a newline to standard output. */
static void puts(const char *s)
{
    print(s);
    syscall3(SYS_WRITE, STDOUT_FILENO, (long)"\n", 1);
}

/** Write `v` in decimal. */
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

/** Close every descriptor in `fds`, and empty the list; the count of the
 * closes that failed. */
static int close_all(void)
{
    int i, failed = 0;

    for (i = 0; i < n_fds; i++) {
        failed += syscall3(SYS_CLOSE, fds[i], 0, 0) != 0;
    }
    n_fds = 0;
    return failed;
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/**
 * Open /hello until the table refuses, and require the refusal to be
 * -EMFILE after LIMIT less three opens; then close one descriptor and
 * require an open to take its place. The file is the same every time, since
 * what fills is this process's table and not the file system's. Every
 * descriptor is closed before the probe exits, and on success each close must
 * succeed. The probe exits 0 when each step held and 1 otherwise. `_start`
 * calls it with the initial stack, whose environment gives the case's
 * identity.
 */
void probe_main(unsigned long *sp)
{
    int fd, i, refused, opened;

    report_begin(report_case((char **)(sp + 1) + sp[0] + 1, "guest.default.fdfull"));
    report_plan("refused");
    report_plan("emfile");
    report_plan("count");
    report_plan("close-one");
    report_plan("reuse");
    report_plan("close-all");
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
        report_stop("the table never refused an open");
        sys_exit(1);
    }
    report_held();
    if (refused != -EMFILE) {
        print("FDFULL FAIL: the refusal was ");
        print_num(refused);
        puts(", not -EMFILE");
        report_stop("the refusal was not -EMFILE");
        close_all();
        sys_exit(1);
    }
    report_held();
    opened = n_fds;
    if (opened != LIMIT - 3) {
        close_all();
        puts("FDFULL FAIL: the count is not the limit less the three open at the start");
        report_stop("the count is not the limit less the three open at the start");
        sys_exit(1);
    }
    report_held();

    if (syscall3(SYS_CLOSE, fds[0], 0, 0) != 0) {
        close_all();
        puts("FDFULL FAIL: a descriptor could not be closed");
        report_stop("a descriptor could not be closed");
        sys_exit(1);
    }
    report_held();
    fd = syscall3(SYS_OPEN, (long)"/hello", O_RDONLY, 0);
    if (fd < 0) {
        fds[0] = fds[--n_fds];
        close_all();
        puts("FDFULL FAIL: a closed descriptor was not reusable");
        report_stop("a closed descriptor was not reusable");
        sys_exit(1);
    }
    fds[0] = fd;
    report_held();

    if (close_all() != 0) {
        puts("FDFULL FAIL: a descriptor could not be closed");
        report_stop("a descriptor could not be closed");
        sys_exit(1);
    }
    report_held();
    report_end();
    print("FDFULL OK (");
    print_num(opened);
    puts(" descriptors, refused with EMFILE, a closed one reused)");
    sys_exit(0);
}
