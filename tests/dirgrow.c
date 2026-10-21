/**
 * dirgrow: a file keeps its directory when a write moves it.
 *
 * KRFFS grows a file in place into the free node after it, and when that
 * node is taken moves the file to a new allocation, copying its header and
 * data (`krffs_resize_at`). The copy must keep the file's parent_id, or the
 * moved file would leave its directory for the root and the tree on the disk
 * would be wrong. The probe makes /dg/f, then /dg/g right after it so that f
 * has no room beside it, grows f past its slack so that it moves, and checks
 * that f still resolves under /dg with its first bytes, and not as /f.
 *
 * The names are relative to the directory the argument `dir=PATH`
 * (probe_args.h) gives, which a native run gives as a KRFFS volume of its
 * own; without it they are relative to `/`, the test image's KRFFS root.
 *
 * Prints "DIRGROW OK" and exits 0, or "DIRGROW FAIL: " with the reason and
 * exits 1. Writes etude's report records (probe_report.h) for the case
 * ETUDE_CASE names, an assertion for each step: `setup` (the directory and
 * the two files, written), `grow` (the growing write), `in-dir` (f resolves
 * under dg), `head-bytes` (f keeps its first bytes), `not-root` (no f in the
 * directory of the files) and `cleanup` (the directory is removed); a
 * failure leaves the steps after it not run. Freestanding; direct system
 * calls (probe.h).
 */

#include "probe.h"
#include "probe_args.h"
#include "probe_report.h"

#define STDOUT_FILENO 1

#define O_RDONLY 0
#define O_RDWR   2
#define O_CREAT  0100

#define GROW 2048

static int fixture_created;

/** Remove the fixtures; return the directory removal result. */
static int cleanup(void)
{
    int rc = 0;

    if (fixture_created) {
        syscall1(SYS_UNLINK, (long)"dg/f");
        syscall1(SYS_UNLINK, (long)"dg/g");
        rc = syscall1(SYS_RMDIR, (long)"dg");
        fixture_created = 0;
    }
    return rc;
}

/** Remove the fixtures and exit; does not return. */
static void sys_exit(int status)
{
    if (cleanup() != 0) {
        status = 1;
    }
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

/** Write the string `s` and a newline to standard output. */
static void puts(const char *s)
{
    syscall3(SYS_WRITE, STDOUT_FILENO, (long)s, (int)strlen(s));
    syscall3(SYS_WRITE, STDOUT_FILENO, (long)"\n", 1);
}

/** Print "DIRGROW FAIL: `why`", fail the step under way with `why`, and
 * exit 1. */
static void fail(const char *why)
{
    syscall3(SYS_WRITE, STDOUT_FILENO, (long)"DIRGROW FAIL: ", 14);
    puts(why);
    report_stop(why);
    sys_exit(1);
}

static char big[GROW];

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/** The probe, entered with `sp` at argc: make the two files, grow the first,
 * and look for it. */
void probe_main(unsigned long *sp)
{
    int fd_f, fd_g, fd;
    int i;
    char buf[4];

    probe_args((int)sp[0], (char **)(sp + 1));
    report_begin(report_case((char **)(sp + 1) + sp[0] + 1, "guest.default.dirgrow"));
    report_plan("setup");
    report_plan("grow");
    report_plan("in-dir");
    report_plan("head-bytes");
    report_plan("not-root");
    report_plan("cleanup");
    if (syscall1(SYS_CHDIR, (long)probe_arg("dir", "/")) != 0) {
        fail("chdir to the directory of the files");
    }
    if (syscall2(SYS_MKDIR, (long)"dg", 0755) != 0) {
        fail("mkdir /dg");
    }
    fixture_created = 1;

    fd_f = syscall3(SYS_OPEN, (long)"dg/f", O_CREAT | O_RDWR, 0644);
    if (fd_f < 0) {
        fail("create /dg/f");
    }
    if (syscall3(SYS_WRITE, fd_f, (long)"FF", 2) != 2) {
        fail("write /dg/f");
    }

    /* A second file right after f leaves it no adjacent room to grow into. */
    fd_g = syscall3(SYS_OPEN, (long)"dg/g", O_CREAT | O_RDWR, 0644);
    if (fd_g < 0) {
        fail("create /dg/g");
    }
    if (syscall3(SYS_WRITE, fd_g, (long)"GG", 2) != 2) {
        fail("write /dg/g");
    }
    report_held();

    /* Grow f past its slack: with g adjacent, this relocates f. */
    for (i = 0; i < GROW; i++) {
        big[i] = (char)('a' + (i & 31));
    }
    if (syscall3(SYS_WRITE, fd_f, (long)big, GROW) != GROW) {
        fail("grow /dg/f");
    }
    report_held();

    syscall2(SYS_CLOSE, fd_f, 0);
    syscall2(SYS_CLOSE, fd_g, 0);

    /* The moved f is still under /dg. */
    fd = syscall3(SYS_OPEN, (long)"dg/f", O_RDONLY, 0);
    if (fd < 0) {
        fail("/dg/f vanished from its directory after a relocating grow");
    }
    report_held();
    if (syscall3(SYS_READ, fd, (long)buf, 2) != 2 || buf[0] != 'F' || buf[1] != 'F') {
        fail("/dg/f lost its head bytes across the relocation");
    }
    syscall2(SYS_CLOSE, fd, 0);
    report_held();

    /* And not at the root as /f. */
    fd = syscall3(SYS_OPEN, (long)"f", O_RDONLY, 0);
    if (fd >= 0) {
        fail("the relocated file leaked to the root as /f");
    }
    report_held();

    if (cleanup() != 0) {
        fail("remove fixture directory");
    }
    report_held();
    report_end();
    puts("DIRGROW OK");
    sys_exit(0);
}
