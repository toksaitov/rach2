/* bedkeep.c: the keeper of a load or a figure of bed-shm.sh, a child
 * subreaper that runs COMMAND and, once it ends or the keeper is told to
 * stop, ends and reaps every process it left (tests/kvm/Readme.md). */

#define _XOPEN_SOURCE 700

#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef PR_SET_PDEATHSIG
#define PR_SET_PDEATHSIG 1
#endif
#ifndef PR_SET_CHILD_SUBREAPER
#define PR_SET_CHILD_SUBREAPER 36
#endif

/* The end's bounds, in milliseconds from its start: TERM until TERM_MS,
 * as bed-shm.sh gave a load's group (the harness's own stop takes up to
 * 15 s), then KILL; once KILL_MS have passed, what is left is reported. */
#ifndef TERM_MS
#define TERM_MS 20000L
#endif
#ifndef KILL_MS
#define KILL_MS 30000L
#endif
#define MAX_KIDS 4096

static volatile sig_atomic_t stop_asked;

static pid_t kids[MAX_KIDS]; /* this keeper's children, from /proc */
static int nkids;
static pid_t termed[MAX_KIDS]; /* children sent TERM, until reaped */
static int ntermed;

/**
 * Note a request to stop, from SIGTERM, SIGINT or SIGHUP, or from the
 * parent's death through PR_SET_PDEATHSIG.
 */
static void on_stop(int sig)
{
    (void)sig;
    stop_asked = 1;
}

/**
 * Wake sigsuspend for a child's end; the main loop reaps.
 */
static void on_child(int sig)
{
    (void)sig;
}

/**
 * Give the milliseconds of the monotonic clock.
 */
static long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/**
 * Sleep MS milliseconds, with every signal the keeper takes blocked, so
 * that none cuts the turn short.
 */
static void nap(long ms)
{
    struct timespec ts;

    ts.tv_sec = ms / 1000L;
    ts.tv_nsec = (ms % 1000L) * 1000000L;
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

/**
 * Read the stat line of process PID into BUF, of SIZE bytes; give 0, or
 * -1 when it has gone.
 */
static int read_stat(pid_t pid, char *buf, size_t size)
{
    char path[64];
    FILE *f;
    size_t n;

    snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
    f = fopen(path, "r");
    if (f == NULL)
        return -1;
    n = fread(buf, 1, size - 1, f);
    fclose(f);
    buf[n] = '\0';
    return n > 0 ? 0 : -1;
}

/**
 * Fill kids with this keeper's children, read from each process's parent
 * in /proc, zombies among them. A child is never reaped but by this
 * process, which is single-threaded, so no pid read here can be given to
 * another process before the keeper itself reaps it.
 */
static void list_kids(void)
{
    DIR *d;
    struct dirent *e;
    char buf[512];
    char *p;
    long ppid;
    pid_t self = getpid();

    nkids = 0;
    d = opendir("/proc");
    if (d == NULL)
        return;
    while ((e = readdir(d)) != NULL && nkids < MAX_KIDS) {
        pid_t pid;
        char *end;

        pid = (pid_t)strtol(e->d_name, &end, 10);
        if (*end != '\0' || pid <= 1 || pid == self)
            continue;
        if (read_stat(pid, buf, sizeof(buf)) != 0)
            continue;
        /* The command name may hold spaces and parentheses: the fields
         * after it begin after the last ')'. */
        p = strrchr(buf, ')');
        if (p == NULL || sscanf(p + 1, " %*c %ld", &ppid) != 1)
            continue;
        if ((pid_t)ppid == self)
            kids[nkids++] = pid;
    }
    closedir(d);
}

/**
 * Forget that PID was sent TERM, once it is reaped and its pid may be
 * given again.
 */
static void forget(pid_t pid)
{
    int i;

    for (i = 0; i < ntermed; i++)
        if (termed[i] == pid) {
            termed[i] = termed[--ntermed];
            return;
        }
}

/**
 * Reap every child but MAIN that has ended, by its own pid, so that MAIN
 * is left a zombie: while it is, its pid and the process group it leads
 * cannot be given again. Give the number reaped.
 */
static int reap_others(pid_t main_pid)
{
    int i, status, n = 0;

    list_kids();
    for (i = 0; i < nkids; i++) {
        if (kids[i] == main_pid)
            continue;
        if (waitpid(kids[i], &status, WNOHANG) == kids[i]) {
            forget(kids[i]);
            n++;
        }
    }
    return n;
}

/**
 * Give whether MAIN has ended, leaving it unreaped.
 */
static int main_ended(pid_t main_pid)
{
    siginfo_t si;

    memset(&si, 0, sizeof(si));
    if (waitid(P_PID, (id_t)main_pid, &si, WEXITED | WNOHANG | WNOWAIT) != 0)
        return errno == ECHILD;
    return si.si_pid == main_pid;
}

/**
 * Send SIG to child PID and, when it leads its own process group, to the
 * group, which holds those of its descendants whose parents still run.
 * The child is unreaped, so neither its pid nor its group's can be another
 * process's.
 */
static void signal_kid(pid_t pid, int sig)
{
    kill(pid, sig);
    if (getpgid(pid) == pid)
        kill(-pid, sig);
}

/**
 * Print on stderr what child PID is and holds: its state, wait channel,
 * command and the devices it has open, for bed-shm.sh's line on a load
 * that outlived its end; nothing for a zombie.
 */
static void report(pid_t pid)
{
    char buf[512], path[320], link[256], wchan[64];
    const char *name = "gone";
    char state = '?';
    char *p, *q;
    FILE *f;
    DIR *d;
    struct dirent *e;
    ssize_t n;

    wchan[0] = '\0';
    if (read_stat(pid, buf, sizeof(buf)) == 0) {
        p = strchr(buf, '(');
        q = strrchr(buf, ')');
        if (p != NULL && q != NULL && q > p) {
            *q = '\0';
            name = p + 1;
            if (q[1] == ' ')
                state = q[2];
        }
    }
    if (state == 'Z')
        return; /* ended, holding nothing, to be reaped at the next turn */
    snprintf(path, sizeof(path), "/proc/%ld/wchan", (long)pid);
    f = fopen(path, "r");
    if (f != NULL) {
        if (fgets(wchan, sizeof(wchan), f) == NULL)
            wchan[0] = '\0';
        fclose(f);
    }
    fprintf(stderr, "bedkeep: left: process %ld: %c %s %s; devices:", (long)pid, state,
            wchan[0] != '\0' ? wchan : "-", name);
    snprintf(path, sizeof(path), "/proc/%ld/fd", (long)pid);
    d = opendir(path);
    if (d != NULL) {
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.')
                continue;
            snprintf(path, sizeof(path), "/proc/%ld/fd/%s", (long)pid, e->d_name);
            n = readlink(path, link, sizeof(link) - 1);
            if (n <= 0)
                continue;
            link[n] = '\0';
            if (strncmp(link, "/dev/", 5) == 0)
                fprintf(stderr, " %s", link);
        }
        closedir(d);
    }
    fputc('\n', stderr);
}

/**
 * End every process the command left: each child, and the group each one
 * leads, TERM and CONT once when first seen, until TERM_MS from the start
 * of the end, then KILL at each turn; orphans of the command come to the
 * keeper as their parents end and are signalled at the next turn. Return
 * once waitpid gives ECHILD, with MAIN's status; past KILL_MS, report each
 * child left once and go on, so that none of them is reaped elsewhere and
 * no pid of them is given again while it holds anything.
 */
static int end_all(pid_t main_pid)
{
    long start = now_ms();
    int reported = 0;
    int main_status = 0;
    int status, i, j, alone;
    pid_t pid;

    for (;;) {
        long t = now_ms() - start;
        int sig = t < TERM_MS ? SIGTERM : SIGKILL;

        reap_others(main_pid);
        list_kids();
        /* The main child, a zombie and alone, leads no group with a live
         * member that is not the keeper's child: a live descendant whose
         * parent has gone would be the keeper's. Reap it now. */
        alone = nkids == 1 && kids[0] == main_pid && main_ended(main_pid);
        if (nkids == 0 || alone) {
            pid = waitpid(main_pid, &status, WNOHANG);
            if (pid == main_pid)
                main_status = status;
            if (waitpid(-1, &status, WNOHANG) < 0 && errno == ECHILD)
                return main_status;
            nap(100L);
            continue;
        }
        for (i = 0; i < nkids; i++) {
            if (sig == SIGKILL) {
                signal_kid(kids[i], SIGKILL);
                continue;
            }
            for (j = 0; j < ntermed && termed[j] != kids[i]; j++) {
            }
            if (j < ntermed)
                continue;
            signal_kid(kids[i], SIGTERM);
            signal_kid(kids[i], SIGCONT);
            if (ntermed < MAX_KIDS)
                termed[ntermed++] = kids[i];
        }
        if (t >= KILL_MS && !reported) {
            for (i = 0; i < nkids; i++)
                report(kids[i]);
            reported = 1;
        }
        nap(t >= KILL_MS ? 1000L : 100L);
    }
}

/**
 * Give a shell's status for a wait status: the exit code, or 128 and the
 * signal's number.
 */
static int shell_status(int status)
{
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return 1;
}

int main(int argc, char **argv)
{
    static const int stops[] = {SIGTERM, SIGINT, SIGHUP};
    struct sigaction sa, old[3], oldchld;
    sigset_t block, saved, waitmask;
    pid_t parent, main_pid;
    int i, status;

    if (argc < 2) {
        fprintf(stderr, "usage: bedkeep COMMAND [ARGUMENT...]\n");
        return 2;
    }
    parent = getppid();
    if (prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) != 0) {
        fprintf(stderr, "bedkeep: failed: subreaper: %s\n", strerror(errno));
        return 125;
    }

    sigemptyset(&block);
    for (i = 0; i < 3; i++)
        sigaddset(&block, stops[i]);
    sigaddset(&block, SIGCHLD);
    sigprocmask(SIG_BLOCK, &block, &saved);
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = on_stop;
    for (i = 0; i < 3; i++)
        sigaction(stops[i], &sa, &old[i]);
    sa.sa_handler = on_child;
    sigaction(SIGCHLD, &sa, &oldchld);
    /* The script's death stops the load as its TERM does. Set after the
     * handler, and checked once, for a parent that died before it. */
    prctl(PR_SET_PDEATHSIG, SIGTERM, 0, 0, 0);
    if (getppid() != parent)
        stop_asked = 1;

    main_pid = fork();
    if (main_pid < 0) {
        fprintf(stderr, "bedkeep: failed: fork: %s\n", strerror(errno));
        return 125;
    }
    if (main_pid == 0) {
        /* The command, in a session and a group of its own, with the
         * signal state the keeper was given. */
        for (i = 0; i < 3; i++)
            sigaction(stops[i], &old[i], NULL);
        sigaction(SIGCHLD, &oldchld, NULL);
        sigprocmask(SIG_SETMASK, &saved, NULL);
        setsid();
        execvp(argv[1], argv + 1);
        fprintf(stderr, "bedkeep: failed: exec %s: %s\n", argv[1], strerror(errno));
        _exit(127);
    }

    waitmask = saved;
    for (i = 0; i < 3; i++)
        sigdelset(&waitmask, stops[i]);
    sigdelset(&waitmask, SIGCHLD);
    while (!stop_asked && !main_ended(main_pid)) {
        reap_others(main_pid);
        if (getppid() != parent)
            break;
        sigsuspend(&waitmask);
    }
    status = end_all(main_pid);
    return shell_status(status);
}
