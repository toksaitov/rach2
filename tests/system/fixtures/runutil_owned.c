/*
 * runutil_owned: the worker that runutil_owned.sh's cases leave behind, in
 * the process groups and sessions the runner must find and stop.
 */
#define _POSIX_C_SOURCE 200809L
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile sig_atomic_t released;

/** The handler of SIGUSR1, and of SIGTERM for a worker that obeys it: end
 * the worker's wait, so that it exits with status 0. */
static void release_worker(int sig)
{
    (void)sig;
    released = 1;
}

/**
 * Be the worker: write this process's PID to `path`, then exit after
 * `seconds`, or when SIGUSR1 arrives when `seconds` is 0. SIGTERM ends it
 * the same way when `term_exit` is set and is ignored otherwise, and an
 * alarm after `limit` seconds ends it in any case.
 *
 * Its standard descriptors are /dev/null, and every other descriptor it
 * inherited stays open, the runner's token among them. The PID file is for
 * runutil_owned.sh's checks and its cleanup; the runner never reads it. Does
 * not return; a failure exits 125.
 */
static void worker(const char *path, unsigned int limit, int seconds, int term_exit)
{
    FILE *fp;
    int nullfd = open("/dev/null", O_RDWR);
    if (nullfd < 0 || dup2(nullfd, 0) < 0 || dup2(nullfd, 1) < 0 || dup2(nullfd, 2) < 0)
        _exit(125);
    if (nullfd > 2)
        close(nullfd);
    signal(SIGTERM, term_exit ? release_worker : SIG_IGN);
    signal(SIGUSR1, release_worker);
    alarm(limit);
    fp = fopen(path, "w");
    if (!fp)
        _exit(125);
    fprintf(fp, "%ld\n", (long)getpid());
    if (fclose(fp) != 0)
        _exit(125);
    if (seconds) {
        struct timespec delay;
        delay.tv_sec = seconds;
        delay.tv_nsec = 0;
        while (nanosleep(&delay, &delay) != 0 && errno == EINTR)
            ;
        _exit(0);
    }
    while (!released) {
        struct timespec delay = {0, 10000000};
        nanosleep(&delay, NULL);
    }
    _exit(0);
}

/**
 * `runutil_owned CASE PIDFILE` starts a worker in the place CASE names and
 * returns once its PID file is written.
 *
 * `sentinel` is a worker itself, for 300 seconds. Otherwise a child becomes
 * the worker: in a group of its own for `group` and `recorded`, in this
 * process's group for `ingroup`, `draining` and `stubborn`, and in a session
 * of its own for the rest, where for `session` the child that made the
 * session forks the worker and exits. A `parting` worker exits after one
 * second and a `draining` one after six; an `ingroup` or `recorded` one obeys
 * SIGTERM. For `parting` and `recorded` this process appends the worker's
 * group to peer.groups beside $RACH2_QEMU_LEDGER, among the files of
 * recorded groups the runner reads; for `interrupted` it waits for a signal;
 * for `clean` it releases the worker and returns its status. Returns 0, or
 * 125 on a failure.
 */
int main(int argc, char **argv)
{
    pid_t pid;
    int status, i;
    FILE *fp = NULL;
    if (argc != 3)
        return 125;
    if (!strcmp(argv[1], "sentinel"))
        worker(argv[2], 300, 0, 0);
    pid = fork();
    if (pid < 0)
        return 125;
    if (!pid) {
        if (!strcmp(argv[1], "group") || !strcmp(argv[1], "recorded")) {
            if (setpgid(0, 0) != 0)
                _exit(125);
        } else if (strcmp(argv[1], "ingroup") && strcmp(argv[1], "draining") &&
                   strcmp(argv[1], "stubborn")) {
            if (setsid() < 0)
                _exit(125);
            if (!strcmp(argv[1], "session")) {
                pid = fork();
                if (pid < 0)
                    _exit(125);
                if (pid)
                    _exit(0);
            }
        }
        worker(argv[2], 60,
               !strcmp(argv[1], "draining") ? 6 : (!strcmp(argv[1], "parting") ? 1 : 0),
               !strcmp(argv[1], "ingroup") || !strcmp(argv[1], "recorded"));
    }
    for (i = 0; i < 200; i++) {
        struct timespec delay = {0, 10000000};
        fp = fopen(argv[2], "r");
        if (fp)
            break;
        nanosleep(&delay, NULL);
    }
    if (!fp)
        return 125;
    fclose(fp);
    if (!strcmp(argv[1], "interrupted"))
        for (;;)
            pause();
    if (!strcmp(argv[1], "parting") || !strcmp(argv[1], "recorded")) {
        const char *ledger = getenv("RACH2_QEMU_LEDGER");
        char *groups, *slash;
        if (!ledger)
            return 125;
        groups = malloc(strlen(ledger) + 20);
        if (!groups)
            return 125;
        strcpy(groups, ledger);
        slash = strrchr(groups, '/');
        if (!slash)
            return 125;
        strcpy(slash + 1, "peer.groups");
        fp = fopen(groups, "a");
        free(groups);
        if (!fp)
            return 125;
        fprintf(fp, "%ld\n", (long)getpgid(pid));
        return fclose(fp) == 0 ? 0 : 125;
    }

    if (strcmp(argv[1], "clean"))
        return 0;
    if (kill(pid, SIGUSR1) != 0)
        return 125;
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR)
            return 125;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 125;
}
