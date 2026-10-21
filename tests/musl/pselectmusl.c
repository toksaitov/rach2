/**
 * pselectmusl: check pselect6, its descriptor sets and its signal mask.
 *
 * musl's pselect is pselect6, whose sixth argument points at the mask's
 * address and its size; on i686 musl tries pselect6_time64 only for a
 * timeout past 32 bits of seconds, so every row here reaches pselect6.
 *
 * The rows: the three sets come back with only the ready descriptors set,
 * and the mask the call installed is gone after it; a timeout with nothing
 * ready returns 0 and clears the set; a timeout out of range, a negative
 * count and a mask whose size is not 8 are EINVAL. A signal pending and
 * blocked ends the wait at once when the call's mask unblocks it: the call
 * is EINTR, the handler runs under the call's mask, and the mask before the
 * call is back after the handler returns. A mask that blocks the pending
 * signal leaves it pending through a timeout, and it is delivered when the
 * process unblocks it. A signal sent while the call sleeps, once /proc shows
 * the probe asleep, ends the wait the same way.
 *
 * Prints `PSELECTMUSL OK` and exits 0 on success, and prints
 * `PSELECTMUSL FAIL (detail)` and exits 1 otherwise. etude's report
 * (probe_report.h), for the case ETUDE_CASE names, has an assertion for
 * each row in order: `sets-ready`, with the setup of the handler and the
 * pipes, `mask-restored`, `timeout-zero`, `einval-nsec`, `einval-negative`,
 * `einval-count`, `einval-mask-size`, `pending-eintr`, `handler-mask`,
 * `mask-after-handler`, `blocked-timeout`, `still-pending`,
 * `delivered-on-unblock`, `sleep-seen`, with the fork, and `sleep-eintr`.
 * The first failure fails its assertion and leaves the rest not run.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../probe_report.h"

static volatile sig_atomic_t hits; /* SIGUSR1's handler has run this often */
static sigset_t seen;              /* the mask the handler last ran under */

/** Count SIGUSR1 and record the mask it is handled under. */
static void on_usr1(int sig)
{
    (void)sig;
    sigprocmask(SIG_BLOCK, NULL, &seen);
    hits++;
}

/** Report one failed check and return 1, the probe's failure status. */
static int fail(const char *what)
{
    printf("PSELECTMUSL FAIL (%s)\n", what);
    report_stop(what);
    return 1;
}

/** Whether a call returned -1 with `err`. */
static int refused(long rc, int err)
{
    return rc == -1 && errno == err;
}

/** Set the process's mask to block `sig` alone, or nothing for 0. */
static void block_only(int sig)
{
    sigset_t set;

    sigemptyset(&set);
    if (sig != 0) {
        sigaddset(&set, sig);
    }
    sigprocmask(SIG_SETMASK, &set, NULL);
}

/** Whether the process's mask blocks `sig` alone, or nothing for 0. */
static int blocks_only(int sig)
{
    sigset_t now, want;

    sigprocmask(SIG_BLOCK, NULL, &now);
    sigemptyset(&want);
    if (sig != 0) {
        sigaddset(&want, sig);
    }
    return sigismember(&now, SIGUSR1) == sigismember(&want, SIGUSR1) &&
           sigismember(&now, SIGUSR2) == sigismember(&want, SIGUSR2);
}

/** The state letter of process `pid`, from the field after the name in
 * /proc/PID/stat, or 0 when it cannot be read. */
static char state_of(pid_t pid)
{
    char path[32], text[256];
    FILE *f;
    char *close_paren;
    size_t n;

    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    f = fopen(path, "r");
    if (f == NULL) {
        return 0;
    }
    n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = '\0';
    close_paren = strrchr(text, ')');
    return close_paren != NULL && close_paren[1] == ' ' ? close_paren[2] : 0;
}

/**
 * The sets: a readable pipe, an empty one, a writable end and an end with
 * no exceptional condition, under a mask that blocks SIGUSR2, which the
 * process does not. Returns 0, or 1 once the failure is reported.
 */
static int set_rows(int full[2], int empty[2])
{
    fd_set r, w, e;
    sigset_t mask;
    struct timespec ts = {0, 0};
    int top = full[0];

    if (full[1] > top) {
        top = full[1];
    }
    if (empty[0] > top) {
        top = empty[0];
    }
    FD_ZERO(&r);
    FD_ZERO(&w);
    FD_ZERO(&e);
    FD_SET(full[0], &r);
    FD_SET(empty[0], &r);
    FD_SET(full[1], &w);
    FD_SET(full[0], &e);
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR2);
    block_only(0);
    if (pselect(top + 1, &r, &w, &e, &ts, &mask) != 2 || !FD_ISSET(full[0], &r) ||
        FD_ISSET(empty[0], &r) || !FD_ISSET(full[1], &w) || FD_ISSET(full[0], &e)) {
        return fail("the sets come back with the ready descriptors alone");
    }
    report_held();
    if (!blocks_only(0)) {
        return fail("the call's mask is gone after a descriptor ends it");
    }
    report_held();

    FD_ZERO(&r);
    FD_SET(empty[0], &r);
    ts.tv_nsec = 30000000;
    if (pselect(empty[0] + 1, &r, NULL, NULL, &ts, NULL) != 0 || FD_ISSET(empty[0], &r)) {
        return fail("a timeout with nothing ready is 0 and clears the set");
    }
    report_held();
    return 0;
}

/** The refusals. Returns 0, or 1 once the failure is reported. */
static int refusal_rows(void)
{
    struct timespec ts = {0, 1000000000};
    sigset_t mask;
    long sigarg[2];
    long raw_ts[2] = {0, 0}; /* the call's timespec, of longs on both */

    if (!refused(pselect(0, NULL, NULL, NULL, &ts, NULL), EINVAL)) {
        return fail("a timeout of 10^9 nanoseconds is EINVAL");
    }
    report_held();
    ts.tv_sec = -1;
    ts.tv_nsec = 0;
    if (!refused(pselect(0, NULL, NULL, NULL, &ts, NULL), EINVAL)) {
        return fail("a negative timeout is EINVAL");
    }
    report_held();
    ts.tv_sec = 0;
    if (!refused(pselect(-1, NULL, NULL, NULL, &ts, NULL), EINVAL)) {
        return fail("a negative count is EINVAL");
    }
    report_held();
    sigemptyset(&mask);
    sigarg[0] = (long)&mask;
    sigarg[1] = 4;
    if (!refused(syscall(SYS_pselect6, 0L, 0L, 0L, 0L, raw_ts, sigarg), EINVAL)) {
        return fail("a mask of 4 bytes is EINVAL");
    }
    report_held();
    return 0;
}

/**
 * The mask and a pending signal. The process blocks SIGUSR1 and sends it to
 * itself. A call whose mask blocks SIGUSR2 alone is EINTR at once, its
 * handler runs under that mask and its own signal, and the process's mask is
 * back after. A call whose mask blocks SIGUSR1 times out with it pending.
 * Returns 0, or 1 once the failure is reported.
 */
static int pending_rows(int empty[2])
{
    fd_set r;
    sigset_t mask, pending;
    struct timespec ts = {5, 0};

    block_only(SIGUSR1);
    hits = 0;
    kill(getpid(), SIGUSR1);
    FD_ZERO(&r);
    FD_SET(empty[0], &r);
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR2);
    if (!refused(pselect(empty[0] + 1, &r, NULL, NULL, &ts, &mask), EINTR) || hits != 1) {
        return fail("a pending signal the mask unblocks ends the wait");
    }
    report_held();
    if (!sigismember(&seen, SIGUSR2) || !sigismember(&seen, SIGUSR1)) {
        return fail("the handler runs under the call's mask");
    }
    report_held();
    if (!blocks_only(SIGUSR1)) {
        return fail("the mask before the call is back after the handler");
    }
    report_held();

    kill(getpid(), SIGUSR1);
    FD_ZERO(&r);
    FD_SET(empty[0], &r);
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    ts.tv_sec = 0;
    ts.tv_nsec = 30000000;
    if (pselect(empty[0] + 1, &r, NULL, NULL, &ts, &mask) != 0 || hits != 1) {
        return fail("a mask that blocks the pending signal keeps the wait");
    }
    report_held();
    sigpending(&pending);
    if (!sigismember(&pending, SIGUSR1) || !blocks_only(SIGUSR1)) {
        return fail("the signal stays pending and blocked after the timeout");
    }
    report_held();
    block_only(0);
    if (hits != 2) {
        return fail("the signal is delivered once the process unblocks it");
    }
    report_held();
    return 0;
}

/**
 * A signal sent while the call sleeps: a child sends SIGUSR1 once /proc
 * shows the probe asleep, and exits 1 if it never saw that, after sending it
 * anyway. The process blocks SIGUSR1 and the call's mask does not. Returns
 * 0, or 1 once the failure is reported.
 */
static int sleep_row(int empty[2])
{
    fd_set r;
    sigset_t mask;
    struct timespec ts = {30, 0};
    struct timespec pause_ts = {0, 10000000};
    pid_t parent = getpid(), pid;
    int status, tries, rc;

    block_only(SIGUSR1);
    hits = 0;
    pid = fork();
    if (pid < 0) {
        return fail("fork");
    }
    if (pid == 0) {
        for (tries = 0; tries < 3000 && state_of(parent) != 'S'; tries++) {
            nanosleep(&pause_ts, NULL);
        }
        kill(parent, SIGUSR1);
        _exit(tries < 3000 ? 0 : 1);
    }
    FD_ZERO(&r);
    FD_SET(empty[0], &r);
    sigemptyset(&mask);
    rc = pselect(empty[0] + 1, &r, NULL, NULL, &ts, &mask);
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return fail("the child saw the probe asleep in pselect");
    }
    report_held();
    if (rc != -1 || errno != EINTR || hits != 1 || !blocks_only(SIGUSR1)) {
        return fail("a signal sent during the wait ends it");
    }
    report_held();
    block_only(0);
    return 0;
}

/** Begin the report and plan its assertions, in the order they are
 * checked. */
static void plan(void)
{
    static const char *const ids[] = {"sets-ready",           "mask-restored",   "timeout-zero",
                                      "einval-nsec",          "einval-negative", "einval-count",
                                      "einval-mask-size",     "pending-eintr",   "handler-mask",
                                      "mask-after-handler",   "blocked-timeout", "still-pending",
                                      "delivered-on-unblock", "sleep-seen",      "sleep-eintr"};
    unsigned i;

    report_begin(report_case(environ, "guest.default.pselectmusl"));
    for (i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        report_plan(ids[i]);
    }
}

int main(void)
{
    struct sigaction sa;
    int full[2], empty[2];

    plan();
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_usr1;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGUSR1, &sa, NULL) != 0 || pipe(full) != 0 || pipe(empty) != 0 ||
        write(full[1], "x", 1) != 1) {
        return fail("set up the handler and the pipes");
    }
    if (set_rows(full, empty) != 0 || refusal_rows() != 0 || pending_rows(empty) != 0 ||
        sleep_row(empty) != 0) {
        return 1;
    }
    report_end();
    printf("PSELECTMUSL OK\n");
    return 0;
}
