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

/** End a clean worker without making its normal exit a failed status. */
static void release_worker(int sig)
{
    (void)sig;
    released = 1;
}

/** Run without console output, retaining every unknown inherited descriptor.
 * The PID file is solely for the fixture's assertions and final cleanup.
 * It is never a runner ledger. An alarm bounds a broken test's worker.
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

/** Detach before any fixture handshake, or reap the same worker cleanly.
 * The session case loses an intermediate parent immediately after fork.
 * Neither case registers the worker or learns the liveness descriptor.
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
