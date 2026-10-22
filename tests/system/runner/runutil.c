/**
 * runutil: the host helper of tests/run and of the scripts that supervise a
 * command, for what POSIX shell lacks: a monotonic clock and fractional
 * sleeps, process groups, record locks, and the stop of a command that has
 * done nothing for too long.
 *
 * tests/run compiles it with the host's cc, and make builds it at
 * build/tests/system/runutil. The first argument names the mode:
 *
 *   lock FILE OWNER COMMAND ...    hold the tree's lock through an exec
 *   ports DIR COUNT COMMAND ...    claim a block of host ports, then exec
 *   group COMMAND ...              exec in a new process group
 *   lead COMMAND ...               lead a new group and pass it signals
 *   watch NAME IDLE COMMAND ...    supervise a command
 *   killgroup GROUP SIGNAL         signal a group other than init's or ours
 *   identity PID                   print pid, group and start time
 *   pgid PID                       print a process's group, or exit 1
 *   young FILE SECS                exit 0 when FILE changed within SECS
 *   now                            print the monotonic clock's seconds
 *   sleep SECS                     sleep, a fraction of a second allowed
 *
 * The function of each mode states its contract. Other arguments are
 * refused with status 2. The process table is read through libproc on
 * macOS and /proc on Linux.
 */
#define _POSIX_C_SOURCE 200809L
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
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

#ifdef __APPLE__
#include <libproc.h>
#include <sys/proc.h>
#endif

/* The stop signal this process has received, or 0; the waits end on it. */
static volatile sig_atomic_t interrupted;

/** Record the stop signal `sig` in `interrupted`. */
static void on_signal(int sig)
{
    interrupted = sig;
}

/** Seconds on `CLOCK_MONOTONIC`, which a change of the calendar clock does
 * not move; exits with status 2 when the clock cannot be read. */
static double now(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        perror("runutil: clock_gettime");
        exit(2);
    }
    return ts.tv_sec + ts.tv_nsec / 1000000000.0;
}

/** Sleep `seconds`, a fraction allowed, resuming after a signal that leaves
 * `interrupted` 0 and ending after one that sets it. */
static void pause_for(double seconds)
{
    struct timespec ts;
    ts.tv_sec = (time_t)seconds;
    ts.tv_nsec = (long)((seconds - ts.tv_sec) * 1000000000.0);
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR && !interrupted)
        ;
}

/** Write the `len` bytes of `buf` to `fd`, across interrupted and short
 * writes. Returns 0, or -1 when a write fails, as one to a closed pipe. */
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

/** Give INT, QUIT, TERM, HUP and PIPE their default actions before an exec;
 * a shell starts a background job with INT and QUIT ignored. */
static void reset_signals(void)
{
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);
}

/**
 * Exec `argv` with the default signal actions, first moving to a process
 * group of its own when `group` is set. A `setpgid` that fails is accepted
 * when the process already leads its group, as after the parent's call;
 * otherwise the process exits with status 2. An exec that fails exits with
 * status 127.
 */
static void execute(char **argv, int group)
{
    reset_signals();
    if (group && setpgid(0, 0) != 0) {
        int error = errno;
        pid_t pid = getpid(), actual = getpgrp();
        if (actual != pid) {
            fprintf(stderr, "runutil: setpgid: pid %ld, group %ld: %s\n", (long)pid, (long)actual,
                    strerror(error));
            exit(2);
        }
    }
    execvp(argv[0], argv);
    perror("runutil: exec");
    exit(127);
}

/* The group `lead_exec` started, which `forward_signal` signals. */
static volatile sig_atomic_t led_group;

/** Pass a signal to the process group `lead_exec` started. */
static void forward_signal(int sig)
{
    kill(-(pid_t)led_group, sig);
}

/** The `lead` mode: run a command as the leader of a new process group,
 * pass every INT, TERM and HUP this process receives to that whole group,
 * and return the command's status: its exit status, or 128 plus the number
 * of the signal that ended it, or 2 when the fork or the wait fails. A
 * signal this process ignored when it started stays ignored.
 *
 * The signals are blocked from before the fork until the child is in its
 * group, which whichever of the two `setpgid` calls runs first makes, and
 * `led_group` is set to it, so one that arrives in between is neither lost nor
 * sent to a group that does not exist yet; the child takes the default
 * actions before it unblocks them. Sending to the group, and not to the
 * child alone, ends a foreground command of a child shell at once, where
 * the shell would run its trap only after that command ended.
 */
static int lead_exec(char **argv)
{
    static const int forwarded[] = {SIGINT, SIGTERM, SIGHUP};
    struct sigaction action, old;
    sigset_t stop, saved;
    pid_t pid;
    int i, status;

    sigemptyset(&stop);
    for (i = 0; i < 3; i++)
        sigaddset(&stop, forwarded[i]);
    sigprocmask(SIG_BLOCK, &stop, &saved);
    pid = fork();
    if (pid < 0) {
        perror("runutil: fork");
        return 2;
    }
    if (pid == 0) {
        reset_signals();
        sigprocmask(SIG_SETMASK, &saved, NULL);
        execute(argv, 1);
    }
    setpgid(pid, pid);
    led_group = pid;
    memset(&action, 0, sizeof(action));
    action.sa_handler = forward_signal;
    sigemptyset(&action.sa_mask);
    for (i = 0; i < 3; i++)
        if (sigaction(forwarded[i], NULL, &old) == 0 && old.sa_handler != SIG_IGN)
            sigaction(forwarded[i], &action, NULL);
    sigprocmask(SIG_SETMASK, &saved, NULL);
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR) {
            perror("runutil: waitpid");
            return 2;
        }
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return WEXITSTATUS(status);
}

/* A process of the table, with `owned` set when it belongs to the command
 * being watched or stopped. */
struct process {
    pid_t pid, parent, group;
    int owned;
    double cpu; /* cumulative processor time, in seconds */
};

/** Parse ps's cumulative processor time, `[[dd-]hh:]mm:ss`, into seconds. */
static double cpu_seconds(const char *text)
{
    long days = 0, hours = 0, mins = 0, secs = 0;
    const char *p = strchr(text, '-');
    if (p) {
        days = atol(text);
        text = p + 1;
    }
    if (sscanf(text, "%ld:%ld:%ld", &hours, &mins, &secs) != 3) {
        hours = 0;
        if (sscanf(text, "%ld:%ld", &mins, &secs) != 2)
            return 0;
    }
    return ((days * 24.0 + hours) * 60 + mins) * 60 + secs;
}

/** The processor time the children a process has already waited for used,
 * from Linux's /proc, in seconds; 0 where there is no such file. A shell
 * that runs one short command after another keeps no time of its own, so
 * the time its children took is what says it is working. The fields are
 * counted from the last ')', since the command name before it, in
 * parentheses, may have spaces. */
static double reaped_cpu(long pid)
{
    char path[64], text[1024];
    FILE *fp;
    const char *after;
    long cutime, cstime;
    size_t got;
    int field;

    snprintf(path, sizeof(path), "/proc/%ld/stat", pid);
    fp = fopen(path, "r");
    if (!fp)
        return 0;
    got = fread(text, 1, sizeof(text) - 1, fp);
    fclose(fp);
    text[got] = 0;
    after = strrchr(text, ')');
    if (!after)
        return 0;
    /* Fields 16 and 17 of /proc/PID/stat are cutime and cstime, in ticks. */
    for (field = 3; field < 17 && after; field++)
        after = strchr(after + 1, ' ');
    if (!after || sscanf(after, "%ld %ld", &cutime, &cstime) != 2)
        return 0;
    return (cutime + cstime) / (double)sysconf(_SC_CLK_TCK);
}

/**
 * List the process table, from POSIX ps, with `owned` set on `root`, the
 * members of its group, and every descendant of those, whatever its group.
 * The caller lists it before the first signal, since a process whose parent
 * then dies is given another parent and could no longer be traced. Returns
 * the list, which the caller frees, with its length in `*count`; when ps
 * cannot be run the list is empty, and the caller still signals the group.
 * Exits with status 2 when memory runs out.
 */
static struct process *descendants(pid_t root, size_t *count)
{
    FILE *fp = popen("ps -A -o pid= -o ppid= -o pgid= -o time=", "r");
    struct process *list = NULL;
    long pid, parent, group;
    char line[256], timetext[64];
    size_t n = 0, i, j;
    int changed;
    if (fp) {
        while (fgets(line, sizeof(line), fp)) {
            struct process *next;
            if (sscanf(line, "%ld %ld %ld %63s", &pid, &parent, &group, timetext) != 4)
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
            list[n].cpu = cpu_seconds(timetext);
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

/** The seconds of processor time used by `root`, its group and their
 * descendants, including those in other groups, with reaped children's time
 * where /proc reports it. The number of owned processes goes in `*count`.
 * Returns -1 when the process table is empty or unavailable. */
static double owned_cpu(pid_t root, int *count)
{
    size_t n, i;
    struct process *list = descendants(root, &n);
    double total = 0;

    *count = 0;
    for (i = 0; i < n; i++) {
        if (list[i].owned) {
            total += list[i].cpu + reaped_cpu(list[i].pid);
            (*count)++;
        }
    }
    free(list);
    return n ? total : -1;
}

/* A process's identity: its group, its start time, in seconds and
 * microseconds on macOS and in ticks since boot with `fraction` 0 on Linux,
 * and whether it is a zombie. */
struct identity {
    pid_t group;
    unsigned long long start, fraction;
    int zombie;
};

/**
 * Read the identity of process `pid` into `*id`. A pid alone can name an
 * unrelated process once it is reused, and the start time tells the two
 * apart. Returns 1, with `zombie` set for a process that has ended and waits
 * to be reaped; 0 when there is no such process; and -1 when the table could
 * not be read, which says nothing of whether the process lives.
 */
static int process_identity(pid_t pid, struct identity *id)
{
    memset(id, 0, sizeof(*id));
#ifdef __APPLE__
    struct proc_bsdinfo info;
    int bytes;
    errno = 0;
    bytes = proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof(info));
    if (bytes != sizeof(info))
        return errno == ESRCH ? 0 : -1;
    id->group = (pid_t)info.pbi_pgid;
    id->start = info.pbi_start_tvsec;
    id->fraction = info.pbi_start_tvusec;
    id->zombie = info.pbi_status == SZOMB;
#else
    char path[64], text[4096], *end, *word, *saved;
    FILE *fp;
    size_t bytes;
    int field;
    snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
    fp = fopen(path, "r");
    if (!fp)
        return errno == ENOENT ? 0 : -1;
    bytes = fread(text, 1, sizeof(text) - 1, fp);
    fclose(fp);
    text[bytes] = 0;
    end = strrchr(text, ')');
    if (!end)
        return -1;
    for (field = 3, word = strtok_r(end + 1, " ", &saved); word;
         field++, word = strtok_r(NULL, " ", &saved)) {
        if (field == 3)
            id->zombie = *word == 'Z' || *word == 'X';
        else if (field == 5)
            id->group = (pid_t)strtol(word, NULL, 10);
        else if (field == 22) {
            id->start = strtoull(word, NULL, 10);
            return 1;
        }
    }
    return -1;
#endif
#ifdef __APPLE__
    return 1;
#endif
}

/** Send `sig` to the group `root` and to each owned process of the `n` of
 * `list`. This process is not sent it. */
static void signal_owned(pid_t root, struct process *list, size_t n, int sig)
{
    size_t i;
    kill(-root, sig);
    for (i = 0; i < n; i++)
        if (list[i].owned && list[i].pid != getpid())
            kill(list[i].pid, sig);
}

/**
 * Stop the command whose group is `root`: send SIGTERM to its group and its
 * descendants, and SIGKILL to the same two seconds later. The descendants
 * are listed before the first signal, so a child that it leaves with a new
 * parent is still reached.
 */
static void stop_owned(pid_t root)
{
    size_t n;
    struct process *list = descendants(root, &n);
    signal_owned(root, list, n, SIGTERM);
    pause_for(2);
    signal_owned(root, list, n, SIGKILL);
    free(list);
}

/** The last 4096 bytes a supervised command wrote, for the report of a hung
 * command. */
struct recent_output {
    char text[4096];
    size_t len;
};

/** Add the `n` bytes of `buf` to the end of `recent`, dropping its oldest. */
static void keep_recent(struct recent_output *recent, const char *buf, size_t n)
{
    if (n >= sizeof(recent->text)) {
        memcpy(recent->text, buf + n - sizeof(recent->text), sizeof(recent->text));
        recent->len = sizeof(recent->text);
        return;
    }
    if (recent->len + n > sizeof(recent->text)) {
        size_t drop = recent->len + n - sizeof(recent->text);
        memmove(recent->text, recent->text + drop, recent->len - drop);
        recent->len -= drop;
    }
    memcpy(recent->text + recent->len, buf, n);
    recent->len += n;
}

/** Copy to `outfd` what the output pipe `fd` still holds once the command
 * has ended, up to 1 MiB, so that a descendant that goes on writing does
 * not delay the end. */
static void drain_output(int fd, int outfd)
{
    char buf[8192];
    size_t drained = 0;
    ssize_t n;

    while (drained < 1048576 && (n = read(fd, buf, sizeof(buf))) > 0) {
        write_all(outfd, buf, (size_t)n);
        drained += (size_t)n;
    }
}

/** The status a shell gives a command whose wait status is `status`: its
 * exit status, or 128 plus the signal that ended it. */
static int exit_code(int status)
{
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

/**
 * Say that the command `name` has done nothing for `idle` seconds, and print
 * its last output, the `recent_len` bytes of `recent`.
 */
static void report_hang(const char *name, double idle, const char *recent, size_t recent_len)
{
    fprintf(stderr, "run: %s: no output for %.0f s and next to no processor time; stopping it\n",
            name, idle);
    fprintf(stderr, "run: %s: its last output:\n", name);
    write_all(STDERR_FILENO, recent, recent_len);
    write_all(STDERR_FILENO, "\n", 1);
}

/**
 * The `watch` mode: run `cmd` as the leader of a new process group, copy its
 * output to standard output, and stop it as hung when it has done nothing
 * for `idle` seconds. Returns the command's exit status, or 128 plus N when
 * signal N ended it; 124 when it was stopped as hung; 128 plus N when this
 * process received signal N; and 2 when the command cannot be started.
 *
 * The command is working while it writes output, and while its group or
 * descendants use processor time: more than a tenth of a processor over the
 * span since it was last seen working, or a process started or ended,
 * sampled every ten seconds or twice within a shorter limit. When neither
 * has happened for `idle` seconds, `report_hang` says so and the command is
 * stopped with `stop_owned`, as it is when this process receives INT, TERM
 * or HUP. A line of progress goes each minute to standard error, or to
 * descriptor 3 when `RACH2_PROGRESS_FD` is 3 and it is open.
 *
 * Both the parent and the child call `setpgid`, so the group exists before
 * any cleanup can need it, whichever runs first; `execute` checks the
 * child's call.
 */
static int supervise(char **cmd, const char *name, double idle)
{
    int pipes[2], status = 0, done = 0, eof = 0, timedout = 0;
    int progressfd = STDERR_FILENO;
    const char *progress = getenv("RACH2_PROGRESS_FD");
    pid_t child;
    char buf[8192];
    struct recent_output recent;
    double started = now(), active = started, report = started;
    double period = idle < 20 ? (idle < 2 ? 1 : idle / 2) : 10;
    double busy = started, sampled = started - period, cpu_at_busy = -1;
    int count_last = -1;
    struct sigaction sa;
    recent.len = 0;
    if (progress && !strcmp(progress, "3") && fcntl(3, F_GETFD) >= 0)
        progressfd = 3;
    if (pipe(pipes) != 0) {
        perror("runutil: pipe");
        return 2;
    }
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
    while (!done || !eof) {
        struct pollfd pfd;
        ssize_t n;
        double t;
        pfd.fd = pipes[0];
        pfd.events = POLLIN;
        pfd.revents = 0;
        poll(&pfd, 1, 100);
        n = read(pipes[0], buf, sizeof(buf));
        if (n > 0) {
            active = now();
            keep_recent(&recent, buf, (size_t)n);
            if (write_all(STDOUT_FILENO, buf, (size_t)n) != 0)
                interrupted = SIGTERM;
        } else if (n == 0) {
            eof = 1;
            pause_for(0.05);
        }
        if (!done && waitpid(child, &status, WNOHANG) == child)
            done = 1;
        t = now();
        if (!done && t - sampled >= period) {
            int count;
            double cpu = owned_cpu(child, &count);

            if (cpu < 0 || cpu_at_busy < 0 || count != count_last ||
                cpu - cpu_at_busy > 0.1 * (t - busy)) {
                busy = t;
                cpu_at_busy = cpu;
            }
            count_last = count;
            sampled = t;
        }
        if (t - report >= 60) {
            dprintf(progressfd,
                    "run: %s: working for %.0f s; last output %.0f s ago; busy %.0f s ago\n", name,
                    t - started, t - active, t - busy);
            report = t;
        }
        if (!done && t - active >= idle && t - busy >= idle) {
            report_hang(name, idle, recent.text, recent.len);
            timedout = 1;
        }
        if (interrupted || timedout) {
            stop_owned(child);
            break;
        }
        if (done) {
            drain_output(pipes[0], STDOUT_FILENO);
            break;
        }
    }
    if (!done)
        while (waitpid(child, &status, 0) < 0 && errno == EINTR)
            ;
    close(pipes[0]);
    status = exit_code(status);
    if (timedout)
        return 124;
    if (interrupted)
        return 128 + interrupted;
    return status;
}

/** Remove, with `rm -rf`, the directory the environment variable `name`
 * gives, when it is set and not empty. */
static void remove_named(const char *name)
{
    const char *path = getenv(name);
    pid_t child;
    if (!path || !*path)
        return;
    child = fork();
    if (child == 0) {
        execlp("rm", "rm", "-rf", path, (char *)NULL);
        _exit(1);
    }
    if (child > 0)
        while (waitpid(child, NULL, 0) < 0 && errno == EINTR)
            ;
}

/** Remove what a run has made when an exec of the lock or port helper cannot
 * continue: its scratch, `RACH2_RUN_WORK`, and the tree lock directory it
 * took before claiming its ports, `RACH2_RUN_TREE_LOCK`, which tests/run sets
 * only for that claim, and its status record, `RACH2_RUN_STATUS`.
 */
static void remove_work(void)
{
    remove_named("RACH2_RUN_STATUS");
    remove_named("RACH2_RUN_WORK");
    remove_named("RACH2_RUN_TREE_LOCK");
}

/**
 * The `lock` mode: take a POSIX record lock on the file `path` and exec `cmd`
 * holding it, so that one run at a time works on the tree. A lock that
 * another run holds refuses this one with status 1, naming the pid and the
 * arguments the `pid ` and `args ` lines of the file `owner` give. The lock
 * file is never removed, since a run that unlinked it would let two others
 * lock two files at one path.
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

/** Open a block's lock file, which every account on the host must be able to
 * lock. The first opener creates the file mode 0666 with the umask cleared,
 * so that no other account finds it with a narrower mode, even when the
 * creator dies at once; the helper is single-threaded, so nothing else sees
 * the cleared umask. Every opener then sets the mode 0666, which repairs a
 * narrower file of its own account and fails harmlessly on another's. An
 * existing file is opened without O_CREAT, which Linux refuses on another
 * account's file in a sticky directory such as /tmp.
 */
static int open_shared(const char *path)
{
    mode_t mask;
    int fd = open(path, O_RDWR);

    if (fd < 0 && errno == ENOENT) {
        mask = umask(0);
        fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0666);
        umask(mask);
        if (fd < 0 && errno == EEXIST)
            fd = open(path, O_RDWR);
    }
    if (fd >= 0)
        fchmod(fd, 0666);
    return fd;
}

/** End the port claim with 128 plus the number of a signal it has received.
 * The signals in `stop` are blocked except while the claim waits, and here,
 * where any that arrived during a try of the host lock or a pass over the
 * blocks are delivered.
 */
static void stop_if_interrupted(const sigset_t *stop)
{
    sigprocmask(SIG_UNBLOCK, stop, NULL);
    sigprocmask(SIG_BLOCK, stop, NULL);
    if (interrupted) {
        remove_work();
        exit(128 + interrupted);
    }
}

/** End the process with the signal `sig` after removing the run's work
 * directory and tree lock. It is the handler for the last moments of the port
 * claim, between the unblocking of the signals and the exec, where
 * `on_signal` would lose a signal to the exec and the default action would
 * leave the work behind. The exec makes the action the default again, and the
 * run's first line traps the signals itself.
 */
static void on_handoff_signal(int sig)
{
    remove_work();
    signal(sig, SIG_DFL);
    raise(sig);
}

/** Run the command that has claimed a block. The caller has called
 * `stop_if_interrupted` with the signals in `stop` still blocked. A signal
 * that is pending or arrives before the exec goes to `on_handoff_signal`.
 */
static void ports_start(const sigset_t *stop, char **cmd)
{
    reset_signals();
    signal(SIGINT, on_handoff_signal);
    signal(SIGTERM, on_handoff_signal);
    signal(SIGHUP, on_handoff_signal);
    sigprocmask(SIG_UNBLOCK, stop, NULL);
    execute(cmd, 0);
}

/** Try to take a POSIX record lock of `type`, F_RDLCK or F_WRLCK, on the
 * whole of the file `fd` has open. Return 0 when it is taken, 1 when another
 * process has a lock that conflicts with it, and -1 on any other failure.
 */
static int try_lock(int fd, short type)
{
    struct flock lock;

    memset(&lock, 0, sizeof(lock));
    lock.l_type = type;
    lock.l_whence = SEEK_SET;
    if (fcntl(fd, F_SETLK, &lock) == 0)
        return 0;
    return errno == EACCES || errno == EAGAIN ? 1 : -1;
}

/** Open `DIR/NAME`, one of the two files of the host lock, as `open_shared`
 * opens a block's, or refuse the run when it cannot be opened.
 */
static int open_host_file(const char *dir, const char *name)
{
    char path[4096];
    int fd;

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    fd = open_shared(path);
    if (fd < 0) {
        fprintf(stderr, "run: result: refused: the host lock %s cannot be opened: %s\n", path,
                strerror(errno));
        remove_work();
        exit(1);
    }
    return fd;
}

/** Publish a host or port wait in the run's scratch directory. `path` is
 * one host lock, or the directory of `count` port blocks. A zero `since`
 * removes the record before the claim starts its command. The record is
 * advisory; failure to write it does not change the claim.
 */
static void claim_status(const char *reason, const char *path, short type, int count, time_t since)
{
    const char *work = getenv("RACH2_RUN_WORK");
    char file[4096], temp[4096];
    FILE *fp;
    int i;

    if (!work || !*work)
        return;
    snprintf(file, sizeof(file), "%s/claim.wait", work);
    if (!since) {
        unlink(file);
        return;
    }
    snprintf(temp, sizeof(temp), "%s/claim.wait.new", work);
    fp = fopen(temp, "w");
    if (!fp)
        return;
    fprintf(fp, "reason %s\nsince %lld\n", reason, (long long)since);
    if (count)
        for (i = 0; i < count; i++)
            fprintf(fp, "lock WRITE %s/rach2-ports.%d\n", path, i);
    else
        fprintf(fp, "lock %s %s\n", type == F_WRLCK ? "WRITE" : "READ", path);
    if (fclose(fp) == 0)
        rename(temp, file);
}

/** Take a lock of `type` on the file `fd` has open, trying once a second, as
 * the claim of a block does, while another process has a lock that conflicts
 * with it. The signals in `stop` are unblocked only between the tries. The
 * first try that fails prints `waiting`, unless `*said` says a line has been
 * printed already. Return 0 when the lock is taken and -1 when it fails for
 * another reason, with `errno` set.
 */
static int wait_for_lock(int fd, short type, const sigset_t *stop, const char *waiting, int *said,
                         const char *path)
{
    int held, saved;
    time_t since = 0;

    while ((held = try_lock(fd, type)) == 1) {
        if (!since) {
            since = time(NULL);
            claim_status("host", path, type, 0, since);
        }
        if (!*said) {
            printf("%s\n", waiting);
            fflush(stdout);
            *said = 1;
        }
        sigprocmask(SIG_UNBLOCK, stop, NULL);
        pause_for(1.0);
        stop_if_interrupted(stop);
    }
    saved = errno;
    claim_status(NULL, NULL, 0, 0, 0);
    errno = saved;
    return held;
}

/**
 * Take the host lock that `RACH2_HOST_LOCK` asks for, `shared` or `alone`,
 * and keep it through exec; with the variable unset, take none. A shared run
 * takes a read lock on `DIR/rach2-host.lock`, which any number of runs have
 * at once, and a run that needs the host alone takes a write lock on it,
 * which it gets only when no other run has a lock there.
 *
 * A write lock that only waited for the read locks to go could wait for
 * ever, since a run that starts meanwhile takes a read lock of its own. So a
 * run first locks `DIR/rach2-host.gate`: one that needs the host alone takes
 * a write lock there and keeps it while it waits and while it runs, and a
 * shared run takes a read lock there, then the host lock, and then gives the
 * read lock back. A shared run that starts while another waits for the host
 * alone waits behind it. A wait prints one line, once, and a signal ends it
 * as it ends the wait for a block. The command is not given the variable,
 * and a value other than the two refuses the run with status 2.
 */
static void host_claim(const char *dir, const sigset_t *stop)
{
    static const char ahead[] = "run: host: waiting for the runs in progress to end";
    static const char behind[] = "run: host: waiting for a run that needs the host alone";
    const char *mode = getenv("RACH2_HOST_LOCK");
    int gate, host, alone, failed, said = 0;
    char gatepath[4096], hostpath[4096];

    if (mode == NULL)
        return;
    alone = !strcmp(mode, "alone");
    if (!alone && strcmp(mode, "shared") != 0) {
        fprintf(stderr, "run: result: refused: RACH2_HOST_LOCK must be shared or alone\n");
        remove_work();
        exit(2);
    }
    snprintf(gatepath, sizeof(gatepath), "%s/rach2-host.gate", dir);
    snprintf(hostpath, sizeof(hostpath), "%s/rach2-host.lock", dir);
    gate = open_host_file(dir, "rach2-host.gate");
    host = open_host_file(dir, "rach2-host.lock");
    if (alone)
        failed = wait_for_lock(gate, F_WRLCK, stop, ahead, &said, gatepath) != 0 ||
                 wait_for_lock(host, F_WRLCK, stop, ahead, &said, hostpath) != 0;
    else
        failed = wait_for_lock(gate, F_RDLCK, stop, behind, &said, gatepath) != 0 ||
                 wait_for_lock(host, F_RDLCK, stop, behind, &said, hostpath) != 0;
    if (failed) {
        fprintf(stderr, "run: result: refused: the host lock under %s cannot be taken: %s\n", dir,
                strerror(errno));
        remove_work();
        exit(1);
    }
    if (!alone)
        close(gate);
    unsetenv("RACH2_HOST_LOCK");
}

/** Claim a block of host ports and hold it through exec, as `lock` holds the
 * tree. Block N is `DIR/rach2-ports.N`, locked with a POSIX record lock, and
 * the command runs with `RACH2_PORT_OFFSET` set to N * 1000, which
 * tests/system/suites.mk adds to every port it forwards. The lowest free
 * block is taken; while every block is held, the claim waits. Before the
 * block, `host_claim` takes the host lock `RACH2_HOST_LOCK` asks for, in the
 * same directory, and that lock too is held through exec. An interrupt,
 * hangup or termination at any point of the claim ends it with 128 plus the
 * signal's number, and the command does not start.
 */
static void ports_exec(const char *dir, int count, char **cmd)
{
    char path[4096], offset[32];
    struct flock lock;
    sigset_t stop;
    int slot, fd, opened, said = 0;

    sigemptyset(&stop);
    sigaddset(&stop, SIGINT);
    sigaddset(&stop, SIGTERM);
    sigaddset(&stop, SIGHUP);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP, on_signal);
    sigprocmask(SIG_BLOCK, &stop, NULL);
    host_claim(dir, &stop);
    for (;;) {
        opened = 0;
        for (slot = 0; slot < count; slot++) {
            snprintf(path, sizeof(path), "%s/rach2-ports.%d", dir, slot);
            if ((fd = open_shared(path)) < 0)
                continue;
            opened++;
            memset(&lock, 0, sizeof(lock));
            lock.l_type = F_WRLCK;
            lock.l_whence = SEEK_SET;
            if (fcntl(fd, F_SETLK, &lock) == 0) {
                stop_if_interrupted(&stop);
                snprintf(offset, sizeof(offset), "%d", slot * 1000);
                setenv("RACH2_PORT_OFFSET", offset, 1);
                if (slot > 0)
                    printf("run: ports: block %d, %s above the table's\n", slot, offset);
                fflush(stdout);
                claim_status(NULL, NULL, 0, 0, 0);
                ports_start(&stop, cmd);
            }
            close(fd);
        }
        if (opened == 0) {
            fprintf(stderr, "run: result: refused: no port block under %s can be opened\n", dir);
            remove_work();
            exit(1);
        }
        if (!said) {
            claim_status("ports", dir, F_WRLCK, count, time(NULL));
            printf("run: ports: all %d blocks are held by other runs; waiting\n", count);
            fflush(stdout);
            said = 1;
        }
        sigprocmask(SIG_UNBLOCK, &stop, NULL);
        pause_for(1.0);
        stop_if_interrupted(&stop);
    }
}

/** Whether `path` was modified less than `seconds` ago. */
static int is_young(const char *path, double seconds)
{
    struct stat st;
    return stat(path, &st) == 0 && difftime(time(NULL), st.st_mtime) < seconds;
}

/** Start a command for the modes that run one at once, `group` and `lead`;
 * return for any other.
 */
static void start_command(int argc, char **argv)
{
    if (argc < 3)
        return;
    if (!strcmp(argv[1], "group"))
        execute(argv + 2, 1);
    if (!strcmp(argv[1], "lead"))
        exit(lead_exec(argv + 2));
}

/** Print the process identity used by the manager's status reader. Return
 * 1 when `pid` is absent, a zombie, or cannot be inspected.
 */
static int print_identity(pid_t pid)
{
    struct identity id;

    if (process_identity(pid, &id) != 1 || id.zombie)
        return 1;
    printf("%ld %ld %llu %llu\n", (long)pid, (long)id.group, id.start, id.fraction);
    return 0;
}

/** The `pgid` mode: print the process group of `pid`. Return 1 when there
 * is no such process. */
static int print_group(pid_t pid)
{
    pid_t group = getpgid(pid);

    if (group < 0)
        return 1;
    printf("%ld\n", (long)group);
    return 0;
}

/** The `killgroup` mode: send `sig` to `group`, which is neither init's nor
 * this process's. Returns 0 when it was sent and 1 otherwise. */
static int kill_group(pid_t group, int sig)
{
    if (group <= 1 || group == getpgrp())
        return 1;
    return kill(-group, sig) != 0;
}

/** Run the mode the first argument names, as the opening comment lists. */
int main(int argc, char **argv)
{
    start_command(argc, argv);
    if (argc >= 5 && !strcmp(argv[1], "lock"))
        lock_exec(argv[2], argv[3], argv + 4);
    if (argc >= 5 && !strcmp(argv[1], "ports"))
        ports_exec(argv[2], (int)strtol(argv[3], NULL, 10), argv + 4);
    if (argc == 4 && !strcmp(argv[1], "killgroup"))
        return kill_group((pid_t)strtol(argv[2], NULL, 10), (int)strtol(argv[3], NULL, 10));
    if (argc == 2 && !strcmp(argv[1], "now")) {
        printf("%lld\n", (long long)now());
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "sleep")) {
        pause_for(strtod(argv[2], NULL));
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "identity"))
        return print_identity((pid_t)strtol(argv[2], NULL, 10));
    if (argc == 3 && !strcmp(argv[1], "pgid"))
        return print_group((pid_t)strtol(argv[2], NULL, 10));
    if (argc == 4 && !strcmp(argv[1], "young"))
        return !is_young(argv[2], strtod(argv[3], NULL));
    if (argc >= 5 && !strcmp(argv[1], "watch"))
        return supervise(argv + 4, argv[2], strtod(argv[3], NULL));
    fprintf(stderr, "runutil: invalid arguments\n");
    return 2;
}
