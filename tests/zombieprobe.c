/**
 * zombieprobe: the teardown of processes killed by a signal, beside a writer
 * on the same file system.
 *
 * ROUNDS children in turn open and unlink files and die by SIGKILL while
 * another child writes, and each is reaped. This does not force a contended
 * close; exit_hosttest forces the waits of the cleanup and checks the state
 * it ends in.
 *
 * Every child rewrites its file in place, so the file system never fills: a
 * fast machine would fill a small image in the victim's first milliseconds,
 * and its creates would then fail for want of space, which is not what the
 * probe is about. A child whose open fails exits with the error's number, so
 * that the kernel's exit record says why. Needs a writable working
 * directory. Prints "ZOMBIEPROBE OK" after every child is reaped, or
 * "ZOMBIEPROBE FAIL: " and the step and exits 1. Writes etude's report
 * records (probe_report.h) for the case ETUDE_CASE names: `writer`, that the
 * writer was forked, and an assertion for each check of a round, which holds
 * when it held in every round: `pipe`, `fork`, `ready` (the victim filled and
 * unlinked its files), `kill`, `reap` and `sigkill`. A failed check stops
 * the rounds, so the other checks of a round are then not run.
 * Freestanding; direct system calls (probe.h).
 */

#include "probe.h"
#include "probe_report.h"

#define SEEK_SET 0

#define O_RDWR  2
#define O_CREAT 0100
#define O_TRUNC 01000

#define SIGKILL 9
#define ROUNDS  40
#define HOLDERS 6

/** The length of the string `s`. */
static size_t slen(const char *s)
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
    syscall3(SYS_WRITE, 1, (long)s, (int)slen(s));
}

static int wpid; /* the writer, stopped on a failure so the shell returns */
static int vpid; /* the current victim, killed and reaped on a failure */
static const char *names[HOLDERS] = {"zp0", "zp1", "zp2", "zp3", "zp4", "zp5"};

/* The checks: the writer's fork, then those each round makes. */
enum { WRITER, PIPE, FORK, READY, KILL, REAP, DIED, CHECKS };

static const char *const check_ids[CHECKS] = {"writer", "pipe", "fork",   "ready",
                                              "kill",   "reap", "sigkill"};

/** Report the check `check` failed with `what`, and the checks still to be
 * reported not run, and end the report. */
static void report_failed(int check, const char *what)
{
    int i;

    for (i = WRITER; i < CHECKS; i++) {
        if (i == check) {
            report_fail(check_ids[i], what);
        } else if (i != WRITER) {
            report_notrun(check_ids[i], check == WRITER ? "the writer was not started"
                                                        : "the rounds stopped at another check");
        }
    }
    report_end();
}

/** Print "ZOMBIEPROBE FAIL: `what`", report the check `check` failed with
 * it, kill and reap the children, and exit 1. */
static void fail(int check, const char *what)
{
    int status = 0, i;

    print("ZOMBIEPROBE FAIL: ");
    print(what);
    print("\n");
    report_failed(check, what);
    if (vpid > 0) {
        syscall2(SYS_KILL, vpid, SIGKILL);
        syscall3(SYS_WAITPID, vpid, (long)&status, 0);
    }
    if (wpid > 0) {
        syscall2(SYS_KILL, wpid, SIGKILL);
        syscall3(SYS_WAITPID, wpid, (long)&status, 0);
    }
    for (i = 0; i < HOLDERS; i++) {
        syscall1(SYS_UNLINK, (long)names[i]);
    }
    syscall1(SYS_UNLINK, (long)"zpw");
    syscall1(SYS_EXIT, 1);
}

/** The child that is killed. It fills each of its files and unlinks it while
 * it is open, so that its teardown has that many inodes to free, the work
 * that takes the mount's lock and can wait for a buffer the writer holds;
 * then it reports readiness on `ready` and rewrites the first file until
 * it dies. An incomplete setup exits without reporting readiness. */
static void victim(int ready)
{
    int fds[HOLDERS];
    static char buf[4096];
    int i, j;

    for (i = 0; i < (int)sizeof(buf); i++) {
        buf[i] = (char)(i & 0x5F);
    }
    for (i = 0; i < HOLDERS; i++) {
        fds[i] = syscall3(SYS_OPEN, (long)names[i], O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (fds[i] < 0) {
            syscall1(SYS_EXIT, -fds[i]);
        }
        for (j = 0; j < 16; j++) {
            if (syscall3(SYS_WRITE, fds[i], (long)buf, (int)sizeof(buf)) != (int)sizeof(buf)) {
                syscall1(SYS_EXIT, 1);
            }
        }
        if (syscall1(SYS_UNLINK, (long)names[i]) != 0) {
            syscall1(SYS_EXIT, 1);
        }
    }
    if (syscall3(SYS_WRITE, ready, (long)"r", 1) != 1) {
        syscall1(SYS_EXIT, 1);
    }
    syscall1(SYS_CLOSE, ready);
    for (;;) {
        for (j = 0; j < 16; j++) {
            syscall3(SYS_WRITE, fds[0], (long)buf, (int)sizeof(buf));
        }
        syscall3(SYS_LSEEK, fds[0], 0, SEEK_SET);
    }
}

/** The child that keeps the file system busy, so that the victim's closes
 * wait; it rewrites its file until it is killed. */
static void writer(void)
{
    int fd = syscall3(SYS_OPEN, (long)"zpw", O_RDWR | O_CREAT | O_TRUNC, 0644);
    static char buf[4096];
    int i;

    if (fd < 0) {
        syscall1(SYS_EXIT, -fd);
    }
    for (i = 0; i < (int)sizeof(buf); i++) {
        buf[i] = (char)(i & 0x7F);
    }
    for (;;) {
        for (i = 0; i < 64; i++) {
            syscall3(SYS_WRITE, fd, (long)buf, (int)sizeof(buf));
        }
        syscall3(SYS_LSEEK, fd, 0, SEEK_SET);
    }
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/** The probe's entry: the writer, then ROUNDS victims killed and reaped.
 * `_start` calls it with the initial stack, whose environment gives the
 * case's identity. */
void probe_main(unsigned long *sp)
{
    int status, round, ready[2], i;
    char ch;

    report_begin(report_case((char **)(sp + 1) + sp[0] + 1, "guest.default.zombieprobe"));
    wpid = syscall1(SYS_FORK, 0);
    if (wpid < 0) {
        fail(WRITER, "fork the writer");
    }
    if (wpid == 0) {
        writer();
    }
    report_pass(check_ids[WRITER]);

    for (round = 0; round < ROUNDS; round++) {
        ready[0] = -1;
        ready[1] = -1;
        if (syscall1(SYS_PIPE, (long)ready) != 0) {
            fail(PIPE, "pipe for the victim's readiness");
        }
        vpid = syscall1(SYS_FORK, 0);
        if (vpid < 0) {
            fail(FORK, "fork the victim");
        }
        if (vpid == 0) {
            syscall1(SYS_CLOSE, ready[0]);
            victim(ready[1]);
        }
        syscall1(SYS_CLOSE, ready[1]);
        if (syscall3(SYS_READ, ready[0], (long)&ch, 1) != 1 || ch != 'r') {
            fail(READY, "the victim did not fill and unlink its files");
        }
        syscall1(SYS_CLOSE, ready[0]);
        if (syscall2(SYS_KILL, vpid, SIGKILL) != 0) {
            fail(KILL, "kill the victim");
        }
        status = 0;
        if (syscall3(SYS_WAITPID, vpid, (long)&status, 0) != vpid) {
            fail(REAP, "reap the victim");
        }
        vpid = 0;
        if (status != SIGKILL) {
            fail(DIED, "the victim did not die by SIGKILL");
        }
    }

    syscall2(SYS_KILL, wpid, SIGKILL);
    status = 0;
    syscall3(SYS_WAITPID, wpid, (long)&status, 0);
    syscall1(SYS_UNLINK, (long)"zpw");

    for (i = PIPE; i < CHECKS; i++) {
        report_pass(check_ids[i]);
    }
    report_end();
    print("ZOMBIEPROBE OK\n");
    syscall1(SYS_EXIT, 0);
}
