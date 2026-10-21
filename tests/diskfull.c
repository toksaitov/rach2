/**
 * diskfull: fill the file system, then see the space come back.
 *
 * A program writing a large file meets a full file system, and what it is
 * told decides whether it can do anything about it: `ENOSPC` says the disk is
 * full, and `EIO` says the write itself went wrong.
 *
 * The probe writes until a write is refused, requires the refusal to be
 * `ENOSPC` after some bytes were written, unlinks the file, and writes a
 * block again. The last step is what a file system that lost the space
 * instead of freeing it would fail.
 *
 * The probe writes to the mounted root until it is full, so its case,
 * guest.diskfull.diskfull, boots the test image for it alone. It writes the
 * report records of the fill only once it has removed the file, and removes
 * the file on every path but a failed unlink.
 *
 * Writes etude's report records (probe_report.h) for the case ETUDE_CASE
 * names, guest.diskfull.diskfull without it: `filled`, bytes written and then
 * a write refused; `enospc`, the refusal ENOSPC; and `space-returned`, a block
 * written again once the file is removed. Beside them it prints "DISKFULL OK
 * (...)" and exits 0, or "DISKFULL FAIL: " with the reason and exits 1.
 * Freestanding; direct system calls (probe.h).
 */

#include "probe.h"
#include "probe_report.h"

#define STDOUT_FILENO 1

#define O_WRONLY 1
#define O_CREAT  0100
#define O_TRUNC  01000

#define ENOSPC 28

#define CHUNK 4096
/* TRIES blocks of CHUNK, 32 MiB, are more than any image this runs on holds,
 * so the loop reaches the refusal rather than running out of tries. */
#define TRIES 8192

static char block[CHUNK];

/** exit(2); does not return. */
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

/** Close `fd` unless it is negative, remove the probe's file, report the
 * failure `why` of the planned assertion now checked, print "DISKFULL FAIL:
 * `why`" and exit 1. */
static void fail(const char *why, int fd)
{
    if (fd >= 0) {
        syscall3(SYS_CLOSE, fd, 0, 0);
    }
    syscall3(SYS_UNLINK, (long)"/diskfull.tmp", 0, 0);
    report_stop(why);
    print("DISKFULL FAIL: ");
    puts(why);
    sys_exit(1);
}

/** Write into `out`, of 64 bytes, "the refusal was -N, not -ENOSPC" for
 * the result `refused`, which is negative; return `out`. */
static const char *refusal(char *out, int refused)
{
    static const char head[] = "the refusal was -", tail[] = ", not -ENOSPC";
    char num[12];
    int n = 0, i;

    for (i = 0; head[i] != '\0'; i++) {
        out[n++] = head[i];
    }
    report_decimal(num, -refused);
    for (i = 0; num[i] != '\0'; i++) {
        out[n++] = num[i];
    }
    for (i = 0; tail[i] != '\0'; i++) {
        out[n++] = tail[i];
    }
    out[n] = '\0';
    return out;
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/** The probe, entered with `sp` at argc: fill, check the refusal, unlink,
 * and write again. */
void probe_main(unsigned long *sp)
{
    char **envp = (char **)(sp + 1) + sp[0] + 1;
    char why[64];
    int fd, n, i, refused, written, removed;

    report_begin(report_case(envp, "guest.diskfull.diskfull"));
    report_plan("filled");
    report_plan("enospc");
    report_plan("space-returned");

    for (i = 0; i < CHUNK; i++) {
        block[i] = (char)('a' + (i & 15));
    }

    fd = syscall3(SYS_OPEN, (long)"/diskfull.tmp", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fail("the file could not be created", -1);
    }

    refused = 0;
    written = 0;
    for (i = 0; i < TRIES; i++) {
        n = syscall3(SYS_WRITE, fd, (long)block, CHUNK);
        if (n < 0) {
            refused = n;
            break;
        }
        written += n;
    }

    if (refused == 0) {
        fail("the file system never refused a write", fd);
    }
    if (written == 0) {
        fail("nothing was written before the refusal", fd);
    }
    syscall3(SYS_CLOSE, fd, 0, 0);
    removed = syscall3(SYS_UNLINK, (long)"/diskfull.tmp", 0, 0) == 0;
    report_held();
    if (refused != -ENOSPC) {
        fail(refusal(why, refused), -1);
    }
    report_held();

    /* The space comes back once the file is removed. */
    if (!removed) {
        fail("the file could not be removed", -1);
    }
    fd = syscall3(SYS_OPEN, (long)"/diskfull.tmp", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        fail("the file could not be created again", -1);
    }
    n = syscall3(SYS_WRITE, fd, (long)block, CHUNK);
    if (n != CHUNK) {
        fail("the space a removed file held did not come back", fd);
    }
    syscall3(SYS_CLOSE, fd, 0, 0);
    syscall3(SYS_UNLINK, (long)"/diskfull.tmp", 0, 0);
    report_held();
    report_end();

    print("DISKFULL OK (");
    print_num(written / 1024);
    puts(" KiB written, refused with ENOSPC, the space came back)");
    sys_exit(0);
}
