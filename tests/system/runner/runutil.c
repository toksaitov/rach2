#define _POSIX_C_SOURCE 200809L
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile sig_atomic_t interrupted;

/** Remember a signal; the main loop stops and reaps the child. */
static void on_signal(int sig)
{
    interrupted = sig;
}

/** Seconds from a clock unaffected by adjustments to the calendar. */
static double now(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        perror("runutil: clock_gettime");
        exit(2);
    }
    return ts.tv_sec + ts.tv_nsec / 1000000000.0;
}

/** Sleep for a fraction of a second, resuming after an unrelated signal. */
static void pause_for(double seconds)
{
    struct timespec ts;
    ts.tv_sec = (time_t)seconds;
    ts.tv_nsec = (long)((seconds - ts.tv_sec) * 1000000000.0);
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR && !interrupted)
        ;
}

/** Write all bytes, returning failure for a closed destination. */
static int write_all(int fd, const char *buf, size_t len)
{
    while (len) {
        ssize_t n = write(fd, buf, len);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

/** Restore signals ignored by an asynchronous shell job before exec. */
static void reset_signals(void)
{
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);
}

/** Execute a command, optionally in a process group of its own.
 * After a failed setpgid, existing membership decides whether exec is safe.
 */
static void execute(char **argv, int group)
{
    reset_signals();
    if (group && setpgid(0, 0) != 0) {
        int error = errno;
        pid_t pid = getpid(), actual = getpgrp();
        if (actual != pid) {
            fprintf(stderr, "runutil: setpgid: pid %ld, group %ld: %s\n", (long)pid,
                    (long)actual, strerror(error));
            exit(2);
        }
    }
    execvp(argv[0], argv);
    perror("runutil: exec");
    exit(127);
}

struct process {
    pid_t pid, parent, group;
    int owned;
};

/** Save descendants before TERM reparents them, including separate groups.
 * POSIX ps supplies the same fields on macOS and Linux. The command group
 * and machine ledger remain usable if process inspection fails.
 */
static struct process *descendants(pid_t root, size_t *count)
{
    FILE *fp = popen("ps -A -o pid= -o ppid= -o pgid=", "r");
    struct process *list = NULL;
    long pid, parent, group;
    char line[256];
    size_t n = 0, i, j;
    int changed;
    if (fp) {
        while (fgets(line, sizeof(line), fp)) {
            struct process *next;
            if (sscanf(line, "%ld %ld %ld", &pid, &parent, &group) != 3)
                continue;
            next = realloc(list, (n + 1) * sizeof(*list));
            if (!next) {
                perror("runutil: realloc");
                exit(2);
            }
            list = next;
            list[n].pid = (pid_t)pid;
            list[n].parent = (pid_t)parent;
            list[n].group = (pid_t)group;
            list[n].owned = pid == root || group == root;
            n++;
        }
        if (pclose(fp) != 0)
            fprintf(stderr, "runutil: cannot inspect descendants of %ld\n", (long)root);
    }
    do {
        changed = 0;
        for (i = 0; i < n; i++) {
            if (list[i].owned)
                continue;
            for (j = 0; j < n; j++) {
                if (list[j].owned && list[i].parent == list[j].pid) {
                    list[i].owned = changed = 1;
                    break;
                }
            }
        }
    } while (changed);
    *count = n;
    return list;
}

/** Signal the command group, saved descendants, and registered machines. */
static void signal_owned(pid_t root, struct process *list, size_t n, const char *ledger, int sig)
{
    size_t i;
    FILE *fp;
    long pid;
    kill(-root, sig);
    for (i = 0; i < n; i++)
        if (list[i].owned && list[i].pid != getpid())
            kill(list[i].pid, sig);
    fp = ledger ? fopen(ledger, "r") : NULL;
    if (fp) {
        while (fscanf(fp, "%ld", &pid) == 1)
            if (pid > 1 && pid != getpid()) {
                kill((pid_t)pid, sig);
                if (getpgid((pid_t)pid) == pid)
                    kill((pid_t)-pid, sig);
            }
        fclose(fp);
    }
}

/** Stop a command, escalating after two seconds without losing its children. */
static void stop_owned(pid_t root, const char *ledger)
{
    size_t n;
    struct process *list = descendants(root, &n);
    signal_owned(root, list, n, ledger, SIGTERM);
    pause_for(2);
    signal_owned(root, list, n, ledger, SIGKILL);
    free(list);
}

/** Read a file's size; an absent file has no output yet. */
static off_t file_size(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? st.st_size : 0;
}

/** Print the last bytes without reading an entire build transcript. */
static void last_output(const char *path)
{
    char buf[4096];
    ssize_t n;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return;
    if (lseek(fd, -4096, SEEK_END) < 0)
        lseek(fd, 0, SEEK_SET);
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        write_all(STDERR_FILENO, buf, (size_t)n);
    close(fd);
    write_all(STDERR_FILENO, "\n", 1);
}

/** Supervise a command, counting its output and its console transcript.
 * Console relays append independently, so an active guest keeps a silent
 * suite alive.
 * The suite's clock is never scaled. Its child group is stopped on silence
 * or a signal, and output from another suite cannot reset its clock.
 * The parent tries to establish the group before cleanup can need it. The
 * child checks membership before exec; the parent's call may lose to exec.
 */
static int supervise(char **cmd, const char *name, double idle, const char *ledger)
{
    int pipes[2], status = 0, done = 0, eof = 0, timedout = 0;
    int progressfd = STDERR_FILENO;
    const char *progress = getenv("RACH2_PROGRESS_FD");
    pid_t child;
    char *console, buf[8192], recent[4096];
    size_t recent_len = 0;
    double started = now(), active = started, report = started;
    off_t previous = 0;
    struct sigaction sa;
    size_t len = strlen(ledger) + 16;
    if (progress && !strcmp(progress, "3") && fcntl(3, F_GETFD) >= 0)
        progressfd = 3;
    console = malloc(len);
    if (!console || pipe(pipes) != 0) {
        perror("runutil: pipe or allocation");
        return 2;
    }
    snprintf(console, len, "%s.console", ledger);
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    child = fork();
    if (child < 0) {
        perror("runutil: fork");
        return 2;
    }
    if (child == 0) {
        close(pipes[0]);
        dup2(pipes[1], STDOUT_FILENO);
        dup2(pipes[1], STDERR_FILENO);
        close(pipes[1]);
        execute(cmd, 1);
    }
    (void)setpgid(child, child);
    close(pipes[1]);
    fcntl(pipes[0], F_SETFL, O_NONBLOCK);
    {
        char *groups = malloc(len);
        FILE *fp;
        if (!groups)
            return 2;
        snprintf(groups, len, "%s.groups", ledger);
        fp = fopen(groups, "a");
        if (fp) {
            fprintf(fp, "%ld\n", (long)child);
            fclose(fp);
        } else {
            perror("runutil: group ledger");
            interrupted = SIGTERM;
        }
        free(groups);
    }
    while (!done || !eof) {
        struct pollfd pfd;
        ssize_t n;
        off_t size;
        double t;
        pfd.fd = pipes[0];
        pfd.events = POLLIN;
        pfd.revents = 0;
        poll(&pfd, 1, 100);
        n = read(pipes[0], buf, sizeof(buf));
        if (n > 0) {
            size_t keep = (size_t)n;
            active = now();
            if (keep >= sizeof(recent)) {
                memcpy(recent, buf + keep - sizeof(recent), sizeof(recent));
                recent_len = sizeof(recent);
            } else {
                if (recent_len + keep > sizeof(recent)) {
                    size_t drop = recent_len + keep - sizeof(recent);
                    memmove(recent, recent + drop, recent_len - drop);
                    recent_len -= drop;
                }
                memcpy(recent + recent_len, buf, keep);
                recent_len += keep;
            }
            if (write_all(STDOUT_FILENO, buf, (size_t)n) != 0)
                interrupted = SIGTERM;
        } else if (n == 0) {
            eof = 1;
            pause_for(0.05);
        }
        if (!done && waitpid(child, &status, WNOHANG) == child)
            done = 1;
        size = file_size(console);
        if (size != previous) {
            previous = size;
            active = now();
        }
        t = now();
        if (t - report >= 60) {
            dprintf(progressfd, "run: %s: working for %.0f s; last output %.0f s ago\n", name,
                    t - started, t - active);
            report = t;
        }
        if (!done && t - active >= idle) {
            fprintf(stderr, "run: %s: no output for %.0f s; stopping the suite\n", name, idle);
            fprintf(stderr, "run: %s: the suite's last output:\n", name);
            write_all(STDERR_FILENO, recent, recent_len);
            write_all(STDERR_FILENO, "\n", 1);
            fprintf(stderr, "run: %s: the console's last output:\n", name);
            last_output(console);
            timedout = 1;
        }
        if (interrupted || timedout) {
            stop_owned(child, ledger);
            break;
        }
        if (done) {
            size_t drained = 0;
            while (drained < 1048576 && (n = read(pipes[0], buf, sizeof(buf))) > 0) {
                write_all(STDOUT_FILENO, buf, (size_t)n);
                drained += (size_t)n;
            }
            break;
        }
    }
    if (!done)
        while (waitpid(child, &status, 0) < 0 && errno == EINTR)
            ;
    close(pipes[0]);
    free(console);
    if (timedout)
        return 124;
    if (interrupted)
        return 128 + interrupted;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

/** Keep the machine PID unchanged while a child relays its console.
 * Suites can signal that PID, including with KILL. The relay sees EOF when
 * QEMU closes its output, and both PIDs are recorded for final cleanup.
 */
static int machine_exec(const char *ledger, char **cmd)
{
    int pipes[2], logfd;
    pid_t relay, machine = getpid();
    size_t len = strlen(ledger) + 16;
    char *console = malloc(len);
    FILE *fp;

    if (!console || pipe(pipes) != 0) {
        perror("runutil: console pipe or allocation");
        return 2;
    }
    snprintf(console, len, "%s.console", ledger);
    logfd = open(console, O_WRONLY | O_CREAT | O_APPEND, 0600);
    free(console);
    if (logfd < 0) {
        perror("runutil: console transcript");
        return 2;
    }
    reset_signals();
    relay = fork();
    if (relay < 0) {
        perror("runutil: console relay");
        return 2;
    }
    if (relay == 0) {
        char buf[8192];
        ssize_t n;
        close(pipes[1]);
        close(STDIN_FILENO);
        signal(SIGPIPE, SIG_IGN);
        while ((n = read(pipes[0], buf, sizeof(buf))) != 0) {
            if (n < 0) {
                if (errno == EINTR)
                    continue;
                break;
            }
            if (write_all(logfd, buf, (size_t)n) != 0 ||
                write_all(STDOUT_FILENO, buf, (size_t)n) != 0) {
                kill(machine, SIGTERM);
                break;
            }
        }
        close(pipes[0]);
        close(logfd);
        _exit(0);
    }
    close(pipes[0]);
    close(logfd);
    fp = fopen(ledger, "a");
    if (!fp) {
        perror("runutil: machine ledger");
        close(pipes[1]);
        kill(relay, SIGTERM);
        waitpid(relay, NULL, 0);
        return 2;
    }
    fprintf(fp, "%ld\n%ld\n", (long)machine, (long)relay);
    fclose(fp);
    dup2(pipes[1], STDOUT_FILENO);
    dup2(pipes[1], STDERR_FILENO);
    close(pipes[1]);
    execute(cmd, 0);
    return 127;
}

/** Remove bootstrap scratch when an exec of the lock helper cannot continue. */
static void remove_work(void)
{
    const char *work = getenv("RACH2_RUN_WORK");
    pid_t child;
    if (!work || !*work)
        return;
    child = fork();
    if (child == 0) {
        execlp("rm", "rm", "-rf", work, (char *)NULL);
        _exit(1);
    }
    if (child > 0)
        while (waitpid(child, NULL, 0) < 0 && errno == EINTR)
            ;
}

/** Hold a POSIX record lock through exec. The file remains in the tree;
 * unlinking it would let two callers lock different inodes at the same path.
 */
static void lock_exec(const char *path, const char *owner, char **cmd)
{
    struct flock lock;
    int fd = open(path, O_RDWR | O_CREAT, 0600);
    if (fd < 0) {
        perror("runutil: lock file");
        remove_work();
        exit(1);
    }
    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    if (fcntl(fd, F_SETLK, &lock) < 0) {
        FILE *fp = fopen(owner, "r");
        char line[1024], pid[64] = "unknown", args[1024] = "unknown";
        if (fp) {
            while (fgets(line, sizeof(line), fp)) {
                line[strcspn(line, "\n")] = 0;
                if (!strncmp(line, "pid ", 4))
                    snprintf(pid, sizeof(pid), "%.63s", line + 4);
                if (!strncmp(line, "args ", 5))
                    snprintf(args, sizeof(args), "%s", line + 5);
            }
            fclose(fp);
        }
        fprintf(stderr, "run: another run holds this tree: %s, %s\n", pid, args);
        fprintf(stderr, "run: result: refused: the tree is not free\n");
        remove_work();
        exit(1);
    }
    execute(cmd, 0);
}

/** Small POSIX interfaces used by the shell runner on macOS and Linux. */
int main(int argc, char **argv)
{
    if (argc >= 3 && (!strcmp(argv[1], "group") || !strcmp(argv[1], "exec")))
        execute(argv + 2, !strcmp(argv[1], "group"));
    if (argc >= 5 && !strcmp(argv[1], "lock"))
        lock_exec(argv[2], argv[3], argv + 4);
    if (argc == 4 && !strcmp(argv[1], "killgroup")) {
        pid_t group = (pid_t)strtol(argv[2], NULL, 10);
        int sig = (int)strtol(argv[3], NULL, 10);
        if (group <= 1 || group == getpgrp())
            return 1;
        return kill(-group, sig) != 0;
    }
    if (argc == 2 && !strcmp(argv[1], "now")) {
        printf("%lld\n", (long long)now());
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "sleep")) {
        pause_for(strtod(argv[2], NULL));
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "pgid")) {
        pid_t group = getpgid((pid_t)strtol(argv[2], NULL, 10));
        if (group < 0)
            return 1;
        printf("%ld\n", (long)group);
        return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "young")) {
        struct stat st;
        if (stat(argv[2], &st) != 0)
            return 1;
        return difftime(time(NULL), st.st_mtime) < strtod(argv[3], NULL) ? 0 : 1;
    }
    if (argc >= 6 && !strcmp(argv[1], "watch"))
        return supervise(argv + 5, argv[2], strtod(argv[3], NULL), argv[4]);
    if (argc >= 4 && !strcmp(argv[1], "machine"))
        return machine_exec(argv[2], argv + 3);
    fprintf(stderr, "runutil: invalid arguments\n");
    return 2;
}
