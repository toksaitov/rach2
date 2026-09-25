/**
 * deadline: run a command under a wall-clock limit, killing its whole
 * process group when the limit passes.
 *
 * Usage: `deadline SECONDS COMMAND [ARG ...]`. The command runs in a new
 * process group. When it exits in time, its exit status (or 128 plus its
 * fatal signal) is returned. When the limit passes, the group receives
 * `SIGTERM`, then `SIGKILL` two seconds later if anything survives, and the
 * status is 124, the same contract as timeout(1). Killing the group rather
 * than the child matters because a test that hangs often hangs in a
 * descendant (a QEMU it started, a shell it spawned), and a signal to the
 * child alone would leave the descendant holding the port or the disk.
 *
 * A `SIGTERM`, `SIGINT` or `SIGHUP` sent to deadline itself is passed on to
 * the group, which gets `SIGKILL` two seconds later if anything survives,
 * and deadline then ends by that signal. The group is not the terminal's
 * foreground group, so a Ctrl-C at the terminal, or a script that stops the
 * job it started, reaches the command only through deadline. A signal
 * deadline was started with ignored stays ignored, as a shell ignores
 * `SIGINT` for a job it starts in the background. The ones deadline catches
 * are blocked while it decides whether to return the command's status, and
 * one pending then is taken as caught, so a signal that comes before that
 * decision is passed on; one that comes after it is not, as one sent after
 * deadline has ended is not. Once a signal is passed on or the limit passes,
 * deadline watches the group until it is empty, kills what is left of it two
 * seconds after the signal, whether or not the command itself has ended by
 * then, and returns when the group is gone or five seconds after the kill.
 *
 * This replaces coreutils' gtimeout on macOS, so the suites need no
 * coreutils package. The wait loop polls at 100 ms with `waitpid`; the
 * limit is read from `CLOCK_MONOTONIC`, so a clock step cannot stretch or
 * cut the budget.
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static const int stops[] = {SIGTERM, SIGINT, SIGHUP};

static volatile sig_atomic_t caught;

/** Note a stop signal sent to deadline; the wait loop passes it on. */
static void note_signal(int sig)
{
    caught = sig;
}

static double now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/**
 * Catch each stop signal deadline was not started with ignored, and put the
 * ones caught in `set`, which the wait loop blocks while it decides.
 */
static void catch_stops(sigset_t *set)
{
    struct sigaction action, old;
    size_t i;

    action.sa_handler = note_signal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    sigemptyset(set);
    for (i = 0; i < sizeof(stops) / sizeof(stops[0]); i++) {
        if (sigaction(stops[i], NULL, &old) == 0 && old.sa_handler != SIG_IGN &&
            sigaction(stops[i], &action, NULL) == 0) {
            sigaddset(set, stops[i]);
        }
    }
}

/** The first signal of `set` that is pending, or 0. */
static int pending_stop(const sigset_t *set)
{
    sigset_t pending;
    size_t i;

    if (sigpending(&pending) != 0) {
        return 0;
    }
    for (i = 0; i < sizeof(stops) / sizeof(stops[0]); i++) {
        if (sigismember(set, stops[i]) && sigismember(&pending, stops[i])) {
            return stops[i];
        }
    }
    return 0;
}

/**
 * Kill what is left of the group two seconds after `term_at`, once, and say
 * whether deadline is done with it: its leader reaped, and the group gone or
 * five seconds past the kill.
 */
static int group_done(pid_t child, int reaped, double term_at, double *kill_at)
{
    int gone = kill(-child, 0) != 0 && errno == ESRCH;

    if (!gone && *kill_at == 0 && now() - term_at >= 2.0) {
        kill(-child, SIGKILL);
        *kill_at = now();
    }
    return reaped && (gone || (*kill_at != 0 && now() - *kill_at >= 5.0));
}

/** End deadline by `sig`, the signal it passed on to its command. */
static int end_by(int sig)
{
    signal(sig, SIG_DFL);
    raise(sig);
    return 128 + sig;
}

/**
 * Wait for `child`, the leader of its own group, under `limit` seconds, and
 * return deadline's status: the command's, 124 after the limit, or death by
 * a stop signal passed on to the group. The signals of `blocked` are blocked
 * while the command's status is judged.
 */
static int supervise(pid_t child, long limit, const sigset_t *blocked)
{
    double start = now(), term_at = 0, kill_at = 0;
    int killed = 0, passed = 0, reaped = 0, status = 0;

    for (;;) {
        if (!reaped) {
            pid_t got = waitpid(child, &status, WNOHANG);

            if (got == child) {
                reaped = 1;
            } else if (got < 0 && errno != EINTR) {
                perror("deadline: waitpid");
                return 125;
            }
        }
        sigprocmask(SIG_BLOCK, blocked, NULL);
        if (!caught) {
            caught = pending_stop(blocked);
        }
        if (caught && !passed && !killed) {
            passed = caught;
            kill(-child, passed);
            term_at = now();
        }
        if (reaped && !passed && !killed) {
            return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        }
        sigprocmask(SIG_UNBLOCK, blocked, NULL);
        if (!killed && !passed && now() - start >= (double)limit) {
            fprintf(stderr, "deadline: %lds passed; stopping the group\n", limit);
            kill(-child, SIGTERM);
            killed = 1;
            term_at = now();
        }
        if ((killed || passed) && group_done(child, reaped, term_at, &kill_at)) {
            return passed ? end_by(passed) : 124;
        }
        {
            struct timespec nap = {0, 100000000L};

            nanosleep(&nap, NULL);
        }
    }
}

int main(int argc, char **argv)
{
    long limit;
    char *end;
    pid_t child;
    sigset_t caught_set;

    if (argc < 3) {
        fprintf(stderr, "usage: deadline SECONDS COMMAND [ARG ...]\n");
        return 125;
    }
    limit = strtol(argv[1], &end, 10);
    if (*end != '\0' || limit <= 0) {
        fprintf(stderr, "deadline: '%s' is not a positive second count\n", argv[1]);
        return 125;
    }
    catch_stops(&caught_set);

    child = fork();
    if (child < 0) {
        perror("deadline: fork");
        return 125;
    }
    if (child == 0) {
        setpgid(0, 0);
        execvp(argv[2], argv + 2);
        fprintf(stderr, "deadline: cannot run %s\n", argv[2]);
        _exit(127);
    }
    setpgid(child, child);
    return supervise(child, limit, &caught_set);
}
