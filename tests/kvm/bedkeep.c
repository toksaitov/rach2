/* bedkeep.c: the keeper of each background child of bed-shm.sh, a child
 * subreaper that runs COMMAND, signals it as the script asks through its
 * standard input and, once it ends or the keeper is told to stop, ends and
 * reaps every process it left (tests/kvm/Readme.md). */

#define _XOPEN_SOURCE 700

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/select.h>
#include <sys/stat.h>
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
/* How long a child of the command is held still, its parent stopped, while
 * the keeper checks it and signals it (signal_grandchild). */
#define HOLD_MS  5000L

static volatile sig_atomic_t stop_asked;
static int kill_now; /* the stop asked was `kill`: KILL from the first turn */

static const char *status_path; /* BASE.status, the lines bed-shm.sh reads */
static int end_noted;           /* whether the `ended` line is written */

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
 * Append one line, of FMT and its arguments, to the status file, opened
 * for each line and never inherited, so that the command holds nothing of
 * it.
 */
static void note(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    int fd, n;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n > (int)sizeof(line) - 2)
        n = (int)sizeof(line) - 2;
    line[n++] = '\n';
    fd = open(status_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0)
        return;
    if (write(fd, line, (size_t)n) != n) {
        /* nothing more to do: the script then waits out its bound */
    }
    close(fd);
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
 * Give whether MAIN has ended, leaving it unreaped, and write the status
 * file's `ended STATUS` line, a shell's status, the first time it has.
 */
static int main_ended(pid_t main_pid)
{
    siginfo_t si;

    memset(&si, 0, sizeof(si));
    if (waitid(P_PID, (id_t)main_pid, &si, WEXITED | WNOHANG | WNOWAIT) != 0)
        return errno == ECHILD;
    if (si.si_pid != main_pid)
        return 0;
    if (!end_noted) {
        note("ended %d", si.si_code == CLD_EXITED ? si.si_status : 128 + si.si_status);
        end_noted = 1;
    }
    return 1;
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
 * Print on stderr, and as a `left` line of the status file, what child PID
 * is and holds: its state, wait channel, command and the devices it has
 * open, for bed-shm.sh's line on a load that outlived its end; nothing for
 * a zombie.
 */
static void report(pid_t pid)
{
    char buf[512], path[320], link[256], wchan[64], line[1024];
    const char *name = "gone";
    char state = '?';
    char *p, *q;
    FILE *f;
    DIR *d;
    struct dirent *e;
    ssize_t n;
    int used;

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
    used = snprintf(line, sizeof(line), "process %ld: %c %s %s; devices:", (long)pid, state,
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
            if (strncmp(link, "/dev/", 5) == 0 && used >= 0 && (size_t)used < sizeof(line))
                used += snprintf(line + used, sizeof(line) - (size_t)used, " %s", link);
        }
        closedir(d);
    }
    fprintf(stderr, "bedkeep: left: %s\n", line);
    note("left %s", line);
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

/**
 * End every process the command left: each child, and the group each one
 * leads, TERM and CONT once when first seen, until TERM_MS from the start
 * of the end, then KILL at each turn, or KILL from the first turn when the
 * stop asked was `kill`; orphans of the command come to the
 * keeper as their parents end and are signalled at the next turn. Return
 * once waitpid gives ECHILD, with MAIN's status as a shell gives it; past
 * KILL_MS, report each child left once and go on, so that none of them is
 * reaped elsewhere and no pid of them is given again while it holds
 * anything.
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
        int sig = t < TERM_MS && !kill_now ? SIGTERM : SIGKILL;

        reap_others(main_pid);
        main_ended(main_pid);
        list_kids();
        /* The main child, a zombie and alone, leads no group with a live
         * member that is not the keeper's child: a live descendant whose
         * parent has gone would be the keeper's. Reap it now. */
        alone = nkids == 1 && kids[0] == main_pid && main_ended(main_pid);
        if (nkids == 0 || alone) {
            pid = waitpid(main_pid, &status, WNOHANG);
            if (pid == main_pid) {
                main_status = shell_status(status);
                if (!end_noted)
                    note("ended %d", main_status);
                end_noted = 1;
            }
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
 * Give the number of the signal NAME, written without its SIG, of those
 * the script sends; 0 for any other.
 */
static int signal_number(const char *name)
{
    static const struct {
        const char *name;
        int sig;
    } names[] = {{"TERM", SIGTERM}, {"KILL", SIGKILL}, {"STOP", SIGSTOP}, {"CONT", SIGCONT},
                 {"USR1", SIGUSR1}, {"USR2", SIGUSR2}, {"HUP", SIGHUP},   {"INT", SIGINT}};
    size_t i;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (strcmp(name, names[i].name) == 0)
            return names[i].sig;
    return 0;
}

/**
 * Read the state, the parent and the start time, in clock ticks since the
 * boot, of process PID from its stat line; give 0, or -1 when it has gone
 * or the line does not parse.
 */
static int read_proc(pid_t pid, char *state, long *ppid, unsigned long long *start)
{
    char buf[1024];
    char *p;

    if (read_stat(pid, buf, sizeof(buf)) != 0)
        return -1;
    p = strrchr(buf, ')');
    if (p == NULL || sscanf(p + 1,
                            " %c %ld %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s %*s"
                            " %*s %*s %*s %*s %llu",
                            state, ppid, start) != 3)
        return -1;
    return 0;
}

/**
 * Send SIG to PID, a child of the command, or of the keeper once the
 * command has ended, whose start time is START, as the script read it from
 * the process's stat line while the process ran. The command is stopped
 * first, unless it is stopped already, and continued after, so that it
 * cannot reap PID between the check and the signal; the keeper reaps
 * nothing meanwhile. So PID can be no other process's when it is signalled.
 * Give NULL once the signal is sent, or why it was not.
 */
static const char *signal_grandchild(pid_t main_pid, pid_t pid, unsigned long long start, int sig)
{
    const char *why = NULL;
    char state = '?';
    long ppid = 0, t0;
    unsigned long long st = 0;
    int stopped_here = 0;

    if (read_proc(main_pid, &state, &ppid, &st) != 0)
        return "the command is gone";
    if (state != 'T' && state != 'Z') {
        if (kill(main_pid, SIGSTOP) != 0)
            return "the command could not be stopped";
        stopped_here = 1;
        t0 = now_ms();
        for (;;) {
            if (read_proc(main_pid, &state, &ppid, &st) != 0 || state == 'T' || state == 'Z')
                break;
            if (now_ms() - t0 >= HOLD_MS) {
                why = "the command did not stop within 5 s";
                break;
            }
            nap(1L);
        }
    }
    if (why == NULL) {
        if (read_proc(pid, &state, &ppid, &st) != 0)
            why = "no such process";
        else if ((pid_t)ppid != main_pid && (pid_t)ppid != getpid())
            why = "not a child of the command";
        else if (st != start)
            why = "another process since";
        else if (state == 'Z')
            why = "ended";
        else if (kill(pid, sig) != 0)
            why = "kill failed";
    }
    if (stopped_here)
        kill(main_pid, SIGCONT);
    return why;
}

/**
 * Do one word of the script, LINE: `signal SEQ NAME` sends the signal NAME
 * to the command, which the keeper has not reaped, and `signal SEQ NAME
 * PID START` to a child of it (signal_grandchild); either answers in the
 * status file with `signalled SEQ`, or `unsignalled SEQ WHY`. `kill` asks
 * the keeper to stop and to KILL at once; any other word asks it to stop.
 */
static void do_word(pid_t main_pid, const char *line)
{
    char name[16];
    unsigned long seq;
    unsigned long long start;
    long pid;
    const char *why;
    int sig, n;

    n = sscanf(line, "signal %lu %15s %ld %llu", &seq, name, &pid, &start);
    if (n == 2 || n == 4) {
        sig = signal_number(name);
        if (sig == 0)
            why = "no such signal";
        else if (n == 2)
            why = kill(main_pid, sig) == 0 ? NULL : "kill failed";
        else if (pid <= 1 || (pid_t)pid == main_pid || (pid_t)pid == getpid())
            why = "not a child of the command";
        else
            why = signal_grandchild(main_pid, (pid_t)pid, start, sig);
        if (why == NULL)
            note("signalled %lu", seq);
        else
            note("unsignalled %lu %s", seq, why);
        return;
    }
    if (strcmp(line, "kill") == 0)
        kill_now = 1;
    stop_asked = 1;
}

/**
 * Wait for the script's words on standard input, the control FIFO, with
 * every stop signal and SIGCHLD let through only inside pselect, so that
 * none is lost between a look and the wait, and do each whole line read
 * (do_word); the end of file asks the keeper to stop.
 */
static void wait_word(pid_t main_pid, const sigset_t *waitmask)
{
    static char buf[512];
    static size_t used;
    fd_set rfds;
    ssize_t n;
    char *nl;

    FD_ZERO(&rfds);
    FD_SET(0, &rfds);
    if (pselect(1, &rfds, NULL, NULL, NULL, waitmask) <= 0 || !FD_ISSET(0, &rfds))
        return;
    n = read(0, buf + used, sizeof(buf) - 1 - used);
    if (n < 0) {
        if (errno != EINTR && errno != EAGAIN)
            stop_asked = 1;
        return;
    }
    if (n == 0) {
        stop_asked = 1;
        return;
    }
    used += (size_t)n;
    buf[used] = '\0';
    while (!stop_asked && (nl = strchr(buf, '\n')) != NULL) {
        *nl = '\0';
        do_word(main_pid, buf);
        used -= (size_t)(nl + 1 - buf);
        memmove(buf, nl + 1, used + 1);
    }
    if (used == sizeof(buf) - 1)
        stop_asked = 1; /* a line too long for any word */
}

/**
 * Run the command ARGV in the forked child, in a session and a group of
 * its own, with /dev/null as its standard input, as a shell gives a
 * command it runs in the background, in place of the FIFO, whose word is
 * the keeper's alone. Never returns.
 */
static void run_command(char **argv)
{
    int nul;

    nul = open("/dev/null", O_RDONLY);
    if (nul < 0 || dup2(nul, 0) < 0) {
        fprintf(stderr, "bedkeep: failed: /dev/null: %s\n", strerror(errno));
        _exit(127);
    }
    if (nul != 0)
        close(nul);
    setsid();
    execvp(argv[0], argv);
    fprintf(stderr, "bedkeep: failed: exec %s: %s\n", argv[0], strerror(errno));
    _exit(127);
}

/**
 * Read the keeper's arguments, PARENT BASE COMMAND..., into PARENT, the
 * path of BASE.stop into STOP_PATH, of STOP_SIZE bytes, and the status
 * file's path into status_path; give 0, or 2, a usage's status, with a line
 * on stderr.
 */
static int read_args(int argc, char **argv, pid_t *parent, char *stop_path, size_t stop_size)
{
    static char status_buf[4096];
    char *end;
    long arg;

    if (argc < 4) {
        fprintf(stderr, "usage: bedkeep PARENT BASE COMMAND [ARGUMENT...] <> FIFO\n");
        return 2;
    }
    arg = strtol(argv[1], &end, 10);
    if (*argv[1] == '\0' || *end != '\0' || arg <= 1) {
        fprintf(stderr, "bedkeep: failed: %s: not a parent's pid\n", argv[1]);
        return 2;
    }
    *parent = (pid_t)arg;
    if (snprintf(stop_path, stop_size, "%s.stop", argv[2]) >= (int)stop_size ||
        snprintf(status_buf, sizeof(status_buf), "%s.status", argv[2]) >= (int)sizeof(status_buf)) {
        fprintf(stderr, "bedkeep: failed: %s: name too long\n", argv[2]);
        return 2;
    }
    status_path = status_buf;
    return 0;
}

int main(int argc, char **argv)
{
    static const int stops[] = {SIGTERM, SIGINT, SIGHUP};
    struct sigaction sa, old[3], oldchld;
    sigset_t block, saved, waitmask;
    struct stat st;
    pid_t parent, main_pid;
    char stop_path[4096];
    int i;

    if (read_args(argc, argv, &parent, stop_path, sizeof(stop_path)) != 0)
        return 2;
    if (fstat(0, &st) != 0 || !S_ISFIFO(st.st_mode)) {
        fprintf(stderr, "bedkeep: failed: standard input is not a pipe\n");
        note("done 125");
        return 125;
    }
    if (prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) != 0) {
        fprintf(stderr, "bedkeep: failed: subreaper: %s\n", strerror(errno));
        note("done 125");
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
    /* The script's death stops the keeper as its word does. The signal is
     * asked for after its handler is set and before the parent is checked,
     * so that a script that dies at any point is seen: before the check, as
     * a parent other than PARENT, the script's pid, and after it, as the
     * signal. */
    if (prctl(PR_SET_PDEATHSIG, SIGTERM, 0, 0, 0) != 0) {
        fprintf(stderr, "bedkeep: failed: death signal: %s\n", strerror(errno));
        note("done 125");
        return 125;
    }
    if (getppid() != parent)
        stop_asked = 1;
    /* The script makes BASE.stop before it writes its word, which is lost
     * when the FIFO was not yet open here; the FIFO is open by now. */
    if (access(stop_path, F_OK) == 0)
        stop_asked = 1;
    if (stop_asked) {
        note("ended 143");
        note("done 143");
        return 143;
    }

    main_pid = fork();
    if (main_pid < 0) {
        fprintf(stderr, "bedkeep: failed: fork: %s\n", strerror(errno));
        note("done 125");
        return 125;
    }
    if (main_pid == 0) {
        for (i = 0; i < 3; i++)
            sigaction(stops[i], &old[i], NULL);
        sigaction(SIGCHLD, &oldchld, NULL);
        sigprocmask(SIG_SETMASK, &saved, NULL);
        run_command(argv + 3);
    }
    note("started %ld %ld", (long)getpid(), (long)main_pid);

    waitmask = saved;
    for (i = 0; i < 3; i++)
        sigdelset(&waitmask, stops[i]);
    sigdelset(&waitmask, SIGCHLD);
    while (!stop_asked && !main_ended(main_pid)) {
        reap_others(main_pid);
        if (getppid() != parent)
            break;
        wait_word(main_pid, &waitmask);
    }
    i = end_all(main_pid);
    note("done %d", i);
    return i;
}
