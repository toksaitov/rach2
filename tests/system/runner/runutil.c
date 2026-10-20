/**
 * runutil: the host helper of tests/run and the suites, for what POSIX shell
 * lacks: a monotonic clock and fractional sleeps, process groups, record
 * locks, and the supervision of a suite.
 *
 * tests/run compiles it with the host's cc, and the suites' make builds it
 * at build/tests/system/runutil. The first argument names the mode:
 *
 *   lock FILE OWNER COMMAND ...    hold the tree's lock through an exec
 *   ports DIR COUNT COMMAND ...    claim a block of host ports, then exec
 *   group COMMAND ...              exec in a new process group
 *   exec COMMAND ...               exec with the default signal actions
 *   lead COMMAND ...               lead a new group and pass it signals
 *   watch NAME IDLE LEDGER COMMAND ...
 *                                  supervise a command
 *   suite NAME IDLE LEDGER COMMAND ...
 *                                  supervise a suite
 *   record NAME IDLE LEDGER FILE COMMAND ...
 *                                  supervise a suite for etude, and
 *                                  record its end in FILE
 *   machine LEDGER COMMAND ...     record a machine and exec it
 *   state WHO LEDGER PID           print what a stalled machine is doing
 *   machines NAME LEDGER           stop the machines a suite left
 *   registered LEDGER              print the machines a ledger still has
 *   killgroup GROUP SIGNAL         signal a group other than init's or ours
 *   until FILE PATTERN SECS [COUNT]
 *                                  wait for lines of a file to match
 *   gone PID SECS                  wait for a process to end
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
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
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
#include <glob.h>
#include <regex.h>

/* A basic regular expression's alternation, `\|`, is an extension: GNU
   takes it as written, and macOS only with REG_ENHANCED. */
#ifdef REG_ENHANCED
#define RUNUTIL_REG_EXTRA REG_ENHANCED
#else
#define RUNUTIL_REG_EXTRA 0
#endif
#ifdef __APPLE__
#include <libproc.h>
#include <sys/proc.h>
#else
#include <dirent.h>
#endif

/* The stop signal this process has received, or 0; the waits end on it. */
static volatile sig_atomic_t interrupted;

/* The bound, in seconds before the deadline factor, on the wait of the
 * `record` mode for a suite's machines to end once the suite has. */
#define SETTLE_SECONDS 5.0

/* The lowest descriptor the `record` mode gives its command for etude's
 * records, above those a shell script redirects. */
#define CHILDREN_FD 60

/* The seconds a console relay goes on copying its machine's output after
 * INT, TERM or HUP, so that a machine stopped with it ends with its console
 * whole; fewer than the 15 the cleanup gives a recorded group to stop. */
#define RELAY_SECONDS 5.0

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

/** A completion bound multiplied by the set's deadline factor,
 * `RACH2_DEADLINE_FACTOR` under `RACH2_DEADLINES=scaled`, as the harness
 * multiplies its own; a process told nothing keeps the number as written. */
static double bound(double seconds)
{
    const char *mode = getenv("RACH2_DEADLINES");
    const char *factor = getenv("RACH2_DEADLINE_FACTOR");

    if (mode != NULL && !strcmp(mode, "scaled") && factor != NULL) {
        double f = atof(factor);

        if (f >= 1.0 && f <= 64.0)
            return seconds * f;
    }
    return seconds;
}

/** The number of lines of the file at `path` that match `re`, or 0 for a
 * file not there yet or one that cannot be read whole. The file is read
 * whole at each look, since a serial log grows at its end and a token may
 * straddle two reads. */
static int matching_lines(const char *path, const regex_t *re)
{
    FILE *f = fopen(path, "r");
    long len;
    char *text, *line, *next;
    int n = 0;

    if (f == NULL)
        return 0;
    if (fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) < 0 ||
        (text = malloc((size_t)len + 1)) == NULL) {
        fclose(f);
        return 0;
    }
    rewind(f);
    len = (long)fread(text, 1, (size_t)len, f);
    text[len] = 0;
    fclose(f);
    /* A NUL, as from a line the guest never finished, ends a line. */
    for (line = text; line < text + len; line++)
        if (*line == 0)
            *line = '\n';
    for (line = text; *line != 0; line = next) {
        next = strchr(line, '\n');
        if (next == NULL)
            next = line + strlen(line);
        else
            *next++ = 0;
        if (regexec(re, line, 0, NULL, 0) == 0)
            n++;
    }
    free(text);
    return n;
}

/**
 * `runutil until FILE PATTERN SECS [COUNT]`: the suites' wait for a token.
 * Return 0 as soon as `COUNT` lines of `FILE` (one, unsaid) match the basic
 * regular expression `PATTERN`, 1 when `SECS` seconds, a completion bound
 * under the set's factor, pass first, 2 for a pattern that does not
 * compile. The look every fifth of a second is the runner's interval, not
 * a suite's.
 */
static int until_token(const char *path, const char *pattern, double seconds, int count)
{
    regex_t re;
    double end;

    if (regcomp(&re, pattern, REG_NOSUB | REG_NEWLINE | RUNUTIL_REG_EXTRA) != 0) {
        fprintf(stderr, "runutil: until: bad pattern: %s\n", pattern);
        return 2;
    }
    end = now() + bound(seconds);
    for (;;) {
        if (matching_lines(path, &re) >= count) {
            regfree(&re);
            return 0;
        }
        if (now() >= end || interrupted) {
            regfree(&re);
            return 1;
        }
        pause_for(0.2);
    }
}

/** Whether `pid` is a process that has ended and waits to be reaped: a
 * child of the caller's stays so until the caller waits for it, and can
 * still be signalled, so `kill` alone would call it alive. The state is
 * read from POSIX ps; 0 when ps cannot be run. */
static int is_zombie(pid_t pid)
{
    char cmd[64], state[16];
    FILE *fp;
    int zombie = 0;

    snprintf(cmd, sizeof(cmd), "ps -o stat= -p %ld", (long)pid);
    fp = popen(cmd, "r");
    if (!fp)
        return 0;
    if (fgets(state, sizeof(state), fp)) {
        const char *s = state;
        while (*s == ' ')
            s++;
        zombie = *s == 'Z';
    }
    pclose(fp);
    return zombie;
}

/**
 * `runutil gone PID SECS`: the suites' wait for a process to end. Return 0
 * as soon as no process `PID` can be signalled, or the one that can is
 * ended and waiting to be reaped, 1 when `SECS` seconds, a completion
 * bound under the set's factor, pass first. A child of the caller's is
 * reported gone before the caller has waited for it, so the caller waits
 * afterwards for the status.
 */
static int until_gone(pid_t pid, double seconds)
{
    double end = now() + bound(seconds);

    while (kill(pid, 0) == 0 || errno == EPERM) {
        if (is_zombie(pid))
            return 0;
        if (now() >= end || interrupted)
            return 1;
        pause_for(0.2);
    }
    return 0;
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

/** Write all of `buf` to `fd` as `write_all` does, but give up when a stop
 * signal interrupts a write that cannot go on; -1 then or after an error. */
static int relay_write(int fd, const char *buf, size_t len)
{
    while (len) {
        ssize_t n = write(fd, buf, len);
        if (n < 0 && errno == EINTR && !interrupted)
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
 * being stopped or checked. */
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
 * cannot be run the list is empty, and the caller still signals the group
 * and the ledger's machines. Exits with status 2 when memory runs out.
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
 * whether it is a zombie, and its command's name. */
struct identity {
    pid_t group;
    unsigned long long start, fraction;
    int zombie;
    char command[64];
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
    snprintf(id->command, sizeof(id->command), "%.*s", (int)sizeof(info.pbi_name), info.pbi_name);
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
    word = strchr(text, '(');
    if (!end || !word || word >= end)
        return -1;
    *end = 0;
    snprintf(id->command, sizeof(id->command), "%.63s", word + 1);
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

/**
 * Append the identities of `machine` and `relay` to `LEDGER.identity`, a
 * line `PID GROUP START FRACTION` each. A process whose identity cannot be
 * read is reported and left out. Returns 0, or -1 when the file cannot be
 * opened or written.
 */
static int record_identity(const char *ledger, pid_t machine, pid_t relay)
{
    char *path = malloc(strlen(ledger) + 16);
    FILE *fp;
    pid_t pids[2] = {machine, relay};
    int i, result = 0;
    if (!path)
        return -1;
    sprintf(path, "%s.identity", ledger);
    fp = fopen(path, "a");
    free(path);
    if (!fp)
        return -1;
    for (i = 0; i < 2; i++) {
        struct identity id;
        if (process_identity(pids[i], &id) != 1) {
            fprintf(stderr, "runutil: machine identity unavailable for pid %ld\n", (long)pids[i]);
            continue;
        }
        if (fprintf(fp, "%ld %ld %llu %llu\n", (long)pids[i], (long)id.group, id.start,
                    id.fraction) < 0)
            result = -1;
    }
    if (fclose(fp) != 0)
        result = -1;
    return result;
}

/**
 * Whether `pid`, read from `ledger`, is still the machine recorded there: a
 * live process whose group and start time match its line of
 * `LEDGER.identity`. A ledger with no identity file is one of pids alone, as
 * the fixtures of the lock's recovery write, and its pids are taken as they
 * are; `machine_exec` always writes the file.
 */
static int registered_machine(const char *ledger, pid_t pid)
{
    char *path = malloc(strlen(ledger) + 16);
    FILE *fp;
    long saved_pid, group;
    unsigned long long start, fraction;
    struct identity id;
    int result = 0;
    if (!path)
        return 0;
    sprintf(path, "%s.identity", ledger);
    fp = fopen(path, "r");
    free(path);
    if (!fp)
        return errno == ENOENT;
    if (process_identity(pid, &id) == 1 && !id.zombie)
        while (fscanf(fp, "%ld %ld %llu %llu", &saved_pid, &group, &start, &fraction) == 4)
            if (saved_pid == pid && id.group == group && id.start == start &&
                id.fraction == fraction) {
                result = 1;
                break;
            }
    fclose(fp);
    return result;
}

/** The `registered` mode: print each pid of `ledger` that is still the
 * machine recorded there, for tests/run to stop at the end of a run. */
static void registered_pids(const char *ledger)
{
    FILE *fp = fopen(ledger, "r");
    long pid;
    if (!fp)
        return;
    while (fscanf(fp, "%ld", &pid) == 1)
        if (pid > 1 && registered_machine(ledger, (pid_t)pid))
            printf("%ld\n", pid);
    fclose(fp);
}

/**
 * The `machines` mode: stop the machines of suite `name` that still run when
 * it ends. Each live process that `LEDGER.identity` records, by pid, group
 * and start time, is sent SIGTERM, and one still there fifteen seconds later
 * is reported by pid, group, start time and command and sent SIGKILL. The
 * file keeps the machines and relays of every boot of the suite, and a
 * process that has ended, or a pid that is now another process's, is passed
 * over. Returns 1 when a machine had to be killed and 0 otherwise; a table
 * that could not be read whole is reported and fails nothing.
 */
static int check_machines(const char *name, const char *ledger)
{
    char *path = malloc(strlen(ledger) + 16);
    double started = now();
    int signalled = 0, failed = 0, unknown = 0;
    if (!path)
        return 0;
    sprintf(path, "%s.identity", ledger);
    for (;;) {
        FILE *fp = fopen(path, "r");
        long pid, group;
        unsigned long long start, fraction;
        int live = 0;
        if (!fp) {
            if (errno != ENOENT)
                unknown = 1;
            break;
        }
        while (fscanf(fp, "%ld %ld %llu %llu", &pid, &group, &start, &fraction) == 4) {
            struct identity id;
            int found = process_identity((pid_t)pid, &id);
            if (found < 0)
                unknown = 1;
            if (found != 1 || id.zombie || id.group != group || id.start != start ||
                id.fraction != fraction)
                continue;
            live = 1;
            if (!signalled)
                kill((pid_t)pid, SIGTERM);
            else if (now() - started >= 15) {
                fprintf(stderr,
                        "run: %s: machine survivor: pid %ld, group %ld, start %llu.%llu, %s\n",
                        name, pid, group, start, fraction, id.command);
                kill((pid_t)pid, SIGKILL);
                failed = 1;
            }
        }
        if (!feof(fp) || ferror(fp))
            unknown = 1;
        fclose(fp);
        if (!live || failed)
            break;
        if (!signalled) {
            fprintf(stderr, "run: %s: stopping registered machines (15 s grace)\n", name);
            started = now();
            signalled = 1;
        }
        pause_for(0.1);
    }
    if (unknown)
        fprintf(stderr, "run: %s: machine identity inspection incomplete\n", name);
    free(path);
    return failed;
}

/**
 * The machines and relays of `ledger` still running: each pid of the file
 * that is still the process `LEDGER.identity` records, and no zombie.
 * Returns their count, with the pids in `*pids` from malloc, or -1 when
 * memory ran out.
 */
static int live_machines(const char *ledger, pid_t **pids)
{
    FILE *fp = fopen(ledger, "r");
    struct identity id;
    pid_t *more;
    long pid;
    int n = 0, cap = 0;

    *pids = NULL;
    if (!fp)
        return 0;
    while (fscanf(fp, "%ld", &pid) == 1) {
        if (pid <= 1 || !registered_machine(ledger, (pid_t)pid) ||
            process_identity((pid_t)pid, &id) != 1 || id.zombie)
            continue;
        if (n == cap) {
            cap = cap * 2 + 8;
            more = realloc(*pids, (size_t)cap * sizeof(*more));
            if (!more) {
                fclose(fp);
                free(*pids);
                *pids = NULL;
                return -1;
            }
            *pids = more;
        }
        (*pids)[n++] = (pid_t)pid;
    }
    fclose(fp);
    return n;
}

/**
 * Wait up to `seconds` for the machines and relays of `ledger` to end, and
 * give those still running as `live_machines` does. A machine that its suite
 * signalled and did not wait for has this long to end by itself.
 */
static int settle(const char *ledger, double seconds, pid_t **pids)
{
    double until = now() + seconds;
    int n;

    while ((n = live_machines(ledger, pids)) > 0 && now() < until && !interrupted) {
        free(*pids);
        pause_for(0.05);
    }
    return n;
}

/** Whether `pid` is one of the `n` of `pids`. */
static int among(long pid, const pid_t *pids, int n)
{
    int i;

    for (i = 0; i < n; i++)
        if (pids[i] == pid)
            return 1;
    return 0;
}

/** Whether `LEDGER.closed` names `relay`, whose transcript then has the
 * whole of its machine's output. */
static int relay_closed(const char *ledger, long relay)
{
    char *path = malloc(strlen(ledger) + 16);
    FILE *fp;
    long pid;
    int found = 0;

    if (!path)
        return 0;
    sprintf(path, "%s.closed", ledger);
    fp = fopen(path, "r");
    free(path);
    if (!fp)
        return 0;
    while (!found && fscanf(fp, "%ld", &pid) == 1)
        found = pid == relay;
    fclose(fp);
    return found;
}

/**
 * Write to `fp` the lines of the `record` mode's record that `ledger` gives
 * once the suite has ended and its machines have settled: `left PID GROUP
 * COMMAND` for each of the `n` machines and relays of `left`, which still
 * run, and `cut MACHINE RELAY` for each other machine whose relay did not
 * copy the whole of its output to the transcript. The ledger has each
 * machine and its relay on two lines together. Returns the count of lines.
 */
static int end_lines(FILE *fp, const char *ledger, const pid_t *left, int n)
{
    FILE *in = fopen(ledger, "r");
    struct identity id;
    long machine, relay;
    char *c;
    int i, cut = 0;

    for (i = 0; i < n; i++) {
        if (process_identity(left[i], &id) != 1) {
            id.group = 0;
            strcpy(id.command, "?");
        }
        for (c = id.command; *c; c++)
            if (*c <= ' ' || *c > '~')
                *c = '?';
        fprintf(fp, "left %ld %ld %s\n", (long)left[i], (long)id.group, id.command);
    }
    if (!in)
        return n;
    while (fscanf(in, "%ld %ld", &machine, &relay) == 2)
        if (!among(machine, left, n) && !among(relay, left, n) && !relay_closed(ledger, relay)) {
            fprintf(fp, "cut %ld %ld\n", machine, relay);
            cut++;
        }
    fclose(in);
    return n + cut;
}

/**
 * Write etude's `child` record of process `pid` to the descriptor that
 * `RACH2_ETUDE_CHILDREN` names, when it names a pipe. The `record` mode
 * gives its command that descriptor, a copy of its own standard output,
 * which etude's executor reads, so that the executor knows each machine and
 * relay and ends them with the suite.
 */
static void report_child(pid_t pid)
{
    const char *text = getenv("RACH2_ETUDE_CHILDREN");
    struct stat st;
    char line[64];
    int fd, n;

    if (!text)
        return;
    fd = (int)strtol(text, NULL, 10);
    if (fd < CHILDREN_FD || fstat(fd, &st) != 0 || !S_ISFIFO(st.st_mode))
        return;
    n = snprintf(line, sizeof(line), "@etude 1 child %ld\n", (long)pid);
    write_all(fd, line, (size_t)n);
}

/** In the `record` mode's command, keep standard output, which etude's
 * executor reads, open at a descriptor of CHILDREN_FD or more, and name it
 * in `RACH2_ETUDE_CHILDREN` for `report_child`. */
static void keep_children_fd(void)
{
    char text[16];
    int fd = fcntl(STDOUT_FILENO, F_DUPFD, CHILDREN_FD);

    if (fd < 0) {
        unsetenv("RACH2_ETUDE_CHILDREN");
        return;
    }
    snprintf(text, sizeof(text), "%d", fd);
    setenv("RACH2_ETUDE_CHILDREN", text, 1);
}

/**
 * Begin the `record` mode's record, the file `record` with `.new` after it,
 * once the command has ended, and return it open, or NULL when it cannot be
 * written. When the command ended by itself, with `stopped` clear, the
 * machines of `ledger` have SETTLE_SECONDS to end, and the lines of
 * `end_lines` follow the first; `*ended` is set when there is one.
 * `finish_record` writes the rest.
 */
static FILE *start_record(const char *record, const char *ledger, int stopped, int *ended)
{
    char *tmp = malloc(strlen(record) + 8);
    FILE *fp = NULL;
    pid_t *left;
    int n;

    if (tmp) {
        sprintf(tmp, "%s.new", record);
        fp = fopen(tmp, "w");
        free(tmp);
    }
    if (!fp || fprintf(fp, "runutil-record 1\n") < 0)
        perror("runutil: record");
    if (!fp || stopped)
        return fp;
    n = settle(ledger, bound(SETTLE_SECONDS), &left);
    if (n < 0) {
        fclose(fp);
        return NULL;
    }
    *ended = end_lines(fp, ledger, left, n) > 0;
    free(left);
    return fp;
}

/**
 * Finish the `record` mode's record in `fp`, the file `record` with `.new`
 * after it, and rename it to `record` once it is whole, so that a reader
 * finds the record whole or not at all. Its first line is `runutil-record
 * 1`, and the lines after `end_lines` are the command's `status`, its exit
 * status or 128 plus the signal that ended it; `idle`, 1 when it was
 * stopped as hung; `stopped`, the signal this process received, or 0;
 * `leaked`, 1 when the liveness check failed; `survivor`, 1 when a machine
 * outlived its TERM; and `end`. Returns 0, or -1 when the record could not
 * be written.
 */
static int finish_record(FILE *fp, const char *record, int status, int idle, int leaked,
                         int survivor)
{
    char *tmp = malloc(strlen(record) + 8);
    int bad;

    if (!tmp) {
        fclose(fp);
        return -1;
    }
    sprintf(tmp, "%s.new", record);
    fprintf(fp, "status %d\nidle %d\nstopped %d\nleaked %d\nsurvivor %d\nend\n", status, idle,
            (int)interrupted, leaked, survivor);
    bad = fflush(fp) != 0 || fsync(fileno(fp)) != 0;
    bad = fclose(fp) != 0 || bad || rename(tmp, record) != 0;
    if (bad)
        perror("runutil: record");
    free(tmp);
    return bad ? -1 : 0;
}

/** Send `sig` to the group `root`, to each owned process of the `n` of
 * `list`, and to each machine `ledger` still records, and to the group such
 * a machine leads. This process is not sent it. */
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
            if (pid > 1 && pid != getpid() && registered_machine(ledger, (pid_t)pid)) {
                kill((pid_t)pid, sig);
                if (getpgid((pid_t)pid) == pid)
                    kill((pid_t)-pid, sig);
            }
        fclose(fp);
    }
}

/**
 * Whether process `pid` has a descriptor open on the pipe `token` describes,
 * matched by the pipe's inode and not by a descriptor number. A process that
 * has gone or cannot be read holds nothing.
 */
static int holds_token(pid_t pid, const struct stat *token)
{
#ifdef __APPLE__
    struct proc_fdinfo *fds;
    int size, bytes, i, found = 0;
    size = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, NULL, 0);
    if (size <= 0)
        return 0;
    size += 256 * (int)sizeof(*fds);
    fds = malloc((size_t)size);
    if (!fds)
        return 0;
    bytes = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, fds, size);
    for (i = 0; i < bytes / (int)sizeof(*fds); i++) {
        struct pipe_fdinfo info;
        if (fds[i].proc_fdtype == PROX_FDTYPE_PIPE &&
            proc_pidfdinfo(pid, fds[i].proc_fd, PROC_PIDFDPIPEINFO, &info, sizeof(info)) ==
                sizeof(info) &&
            info.pipeinfo.pipe_handle == (uint64_t)token->st_ino) {
            found = 1;
            break;
        }
    }
    free(fds);
    return found;
#else
    char path[128];
    struct dirent *entry;
    DIR *dir;
    int found = 0;
    snprintf(path, sizeof(path), "/proc/%ld/fd", (long)pid);
    dir = opendir(path);
    if (!dir)
        return 0;
    while ((entry = readdir(dir)) != NULL) {
        struct stat st;
        if (entry->d_name[0] == '.')
            continue;
        if (fstatat(dirfd(dir), entry->d_name, &st, 0) == 0 && S_ISFIFO(st.st_mode) &&
            st.st_dev == token->st_dev && st.st_ino == token->st_ino) {
            found = 1;
            break;
        }
    }
    closedir(dir);
    return found;
#endif
}

/**
 * List the processes that hold the pipe `token`, but for init and this one,
 * each with its group, which is read between two checks of the descriptor so
 * that a process that exits meanwhile is left out. Returns the list, which
 * the caller frees, with its length in `*count`. The list can be incomplete,
 * as when the table cannot be read or memory runs out, so an empty one says
 * nothing of whether the pipe is still held.
 */
static struct process *token_holders(const struct stat *token, size_t *count)
{
    struct process *list = NULL;
    *count = 0;
#ifdef __APPLE__
    pid_t *pids;
    int size = proc_listpids(PROC_ALL_PIDS, 0, NULL, 0), bytes, i;
    if (size <= 0)
        return NULL;
    size += 4096 * (int)sizeof(*pids);
    pids = malloc((size_t)size);
    if (!pids)
        return NULL;
    bytes = proc_listpids(PROC_ALL_PIDS, 0, pids, size);
    for (i = 0; i < bytes / (int)sizeof(*pids); i++) {
        pid_t pid = pids[i];
#else
    struct dirent *entry;
    DIR *dir = opendir("/proc");
    if (!dir)
        return NULL;
    while ((entry = readdir(dir)) != NULL) {
        pid_t pid = (pid_t)strtol(entry->d_name, NULL, 10);
#endif
        pid_t group;
        struct process *next;
        if (pid <= 1 || pid == getpid() || !holds_token(pid, token))
            continue;
        group = getpgid(pid);
        if (group <= 1 || !holds_token(pid, token))
            continue;
        next = realloc(list, (*count + 1) * sizeof(*list));
        if (!next)
            break;
        list = next;
        list[*count].pid = pid;
        list[*count].group = group;
        list[*count].parent = 0;
        list[*count].owned = 0;
        (*count)++;
    }
#ifdef __APPLE__
    free(pids);
#else
    closedir(dir);
#endif
    return list;
}

/** Send `sig` to each process that holds the pipe `token` now, checking
 * its group and its descriptor again just before. */
static void signal_token(const struct stat *token, int sig)
{
    size_t count, i;
    struct process *list = token_holders(token, &count);
    for (i = 0; i < count; i++)
        if (getpgid(list[i].pid) == list[i].group && holds_token(list[i].pid, token))
            kill(list[i].pid, sig);
    free(list);
}

/**
 * Set `owned` on each of the `count` holders of `list` whose group is
 * `root`, the runner's own, which this process is in, or one recorded in a
 * `*.groups` file in the directory of `ledger`; every ledger there is of the
 * same run. Returns 0, or -1 when a file cannot be read, since then no
 * holder can be shown to be outside every recorded group.
 */
static int recorded_holders(struct process *list, size_t count, pid_t root, const char *ledger)
{
    char *pattern, *slash;
    glob_t paths;
    size_t i, j;
    int result;
    for (i = 0; i < count; i++)
        list[i].owned = list[i].group == root || list[i].group == getpgrp();
    pattern = malloc(strlen(ledger) + 16);
    if (!pattern)
        return -1;
    strcpy(pattern, ledger);
    slash = strrchr(pattern, '/');
    if (slash)
        strcpy(slash + 1, "*.groups");
    else
        strcpy(pattern, "*.groups");
    memset(&paths, 0, sizeof(paths));
    result = glob(pattern, 0, NULL, &paths);
    free(pattern);
    if (result != 0 && result != GLOB_NOMATCH) {
        globfree(&paths);
        return -1;
    }
    result = 0;
    for (i = 0; i < paths.gl_pathc; i++) {
        FILE *fp = fopen(paths.gl_pathv[i], "r");
        long group;
        if (!fp) {
            result = -1;
            break;
        }
        while (fscanf(fp, "%ld", &group) == 1)
            for (j = 0; j < count; j++)
                if (list[j].group == group)
                    list[j].owned = 1;
        if (!feof(fp) || ferror(fp))
            result = -1;
        fclose(fp);
    }
    globfree(&paths);
    return result;
}

/**
 * Stop the command whose group is `root`: send SIGTERM to its group, its
 * descendants, the machines `ledger` records and, with `token`, the holders
 * of that pipe, and SIGKILL to the same two seconds later. The descendants
 * are listed before the first signal, so a child that it leaves with a new
 * parent is still reached.
 */
static void stop_owned(pid_t root, const char *ledger, const struct stat *token)
{
    size_t n;
    struct process *list = descendants(root, &n);
    signal_owned(root, list, n, ledger, SIGTERM);
    if (token)
        signal_token(token, SIGTERM);
    pause_for(2);
    signal_owned(root, list, n, ledger, SIGKILL);
    if (token)
        signal_token(token, SIGKILL);
    free(list);
}

/** Read a byte of the non-blocking pipe `fd`. Returns 0 at its end, when no
 * writer holds it, 1 when a writer holds it and it is empty, and 2 when it
 * gave a byte or the read failed. */
static int token_open(int fd)
{
    char byte;
    ssize_t n;
    do {
        n = read(fd, &byte, 1);
    } while (n < 0 && errno == EINTR);
    if (!n)
        return 0;
    return n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) ? 1 : 2;
}

/**
 * After suite `name` has ended, find what still holds its liveness pipe and
 * stop it. `fd` is the pipe's read end and `token` describes the pipe; the
 * suite's group is `root`; `stopped` says the suite was stopped already.
 * Every process of the suite inherits the write end, and keeps it through
 * exec, setsid, setpgid and a new parent; only one that closes the
 * descriptors it inherited escapes the check.
 *
 * A holder outside every recorded group fails the suite at once, and is
 * reported by pid and group. The recorded groups are sent SIGTERM, and a
 * holder still there fifteen seconds later fails the suite, which leaves
 * time for a QEMU that is shutting down on a loaded host. A pipe that is
 * held but by no process that can be found, or a group ledger that cannot be
 * read, is reported as inconclusive and fails nothing. After a failure, a
 * stop or a signal, the holders are sent SIGKILL for up to two seconds.
 * Returns 1 when the suite fails the check and 0 otherwise.
 */
static int check_token(int fd, const struct stat *token, pid_t root, const char *ledger,
                       const char *name, int stopped)
{
    double started = now();
    int signalled = stopped, failed = 0;
    while (token_open(fd)) {
        struct process *list;
        size_t count, i;
        int complete;
        list = token_holders(token, &count);
        if (!count) {
            free(list);
            if (token_open(fd))
                fprintf(stderr, "run: %s: token inspection found no current holder; inconclusive\n",
                        name);
            return 0;
        }
        complete = recorded_holders(list, count, root, ledger);
        if (complete != 0) {
            free(list);
            fprintf(stderr, "run: %s: group ledgers could not be read; token check inconclusive\n",
                    name);
            return 0;
        }
        for (i = 0; i < count; i++) {
            if (getpgid(list[i].pid) != list[i].group || !holds_token(list[i].pid, token))
                continue;
            if ((!list[i].owned && complete == 0) || (signalled && now() - started >= 15)) {
                fprintf(stderr, "run: %s: %s: pid %ld, group %ld\n", name,
                        list[i].owned ? "recorded group did not stop within 15 s"
                                      : "holder outside recorded groups",
                        (long)list[i].pid, (long)list[i].group);
                failed = 1;
            }
        }
        if (!signalled && !failed && !interrupted) {
            fprintf(stderr, "run: %s: stopping recorded groups (15 s grace)\n", name);
            signal_owned(root, NULL, 0, ledger, SIGTERM);
            for (i = 0; i < count; i++)
                if (list[i].owned && list[i].group != getpgrp() &&
                    getpgid(list[i].pid) == list[i].group && holds_token(list[i].pid, token))
                    kill(-list[i].group, SIGTERM);
            signal_token(token, SIGTERM);
            started = now();
            signalled = 1;
        }
        free(list);
        if (failed || interrupted) {
            stop_owned(root, ledger, token);
            break;
        }
        pause_for(0.1);
    }
    if (failed || stopped || interrupted) {
        started = now();
        while (token_open(fd) && now() - started < 2) {
            signal_token(token, SIGKILL);
            pause_for(0.02);
        }
        if (!token_open(fd))
            fprintf(stderr, "run: %s: all identified pipe holders stopped\n", name);
    }
    return failed;
}

/** The size of the file at `path`, or 0 when there is none yet. */
static off_t file_size(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? st.st_size : 0;
}

/** Copy the last 4096 bytes of the file at `path`, or the whole of a
 * shorter one, to standard error, and a newline after them. */
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

/** The last 4096 bytes a supervised command wrote, for the report of a hung
 * suite. */
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

/** Open the `suite` mode's liveness pipe in `life`, with the identity of its
 * write end in `token` and its read end non-blocking and closed on exec.
 * Returns 0, or -1 with errno set. */
static int open_liveness(int life[2], struct stat *token)
{
    if (pipe(life) != 0 || fstat(life[1], token) != 0 || fcntl(life[0], F_SETFD, FD_CLOEXEC) != 0 ||
        fcntl(life[0], F_SETFL, O_NONBLOCK) != 0)
        return -1;
    return 0;
}

/** The status a shell gives a command whose wait status is `status`: its
 * exit status, or 128 plus the signal that ended it. */
static int exit_code(int status)
{
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

/**
 * Write to `path`, of `size` bytes, the monitor socket `machine_exec` gives
 * machine `pid` of `ledger`, `LEDGER.PID.monitor`. Returns 0, or -1 when the
 * path does not fit a socket's address or has a comma, which QEMU's option
 * would read as the end of the path.
 */
static int monitor_path(char *path, size_t size, const char *ledger, pid_t pid)
{
    struct sockaddr_un addr;
    int n = snprintf(path, size, "%s.%ld.monitor", ledger, (long)pid);

    return n > 0 && (size_t)n < size && (size_t)n < sizeof(addr.sun_path) && !strchr(path, ',')
               ? 0
               : -1;
}

/**
 * Print each thread of process `pid`, labelled `label`, with its state
 * letter and the kernel function it sleeps in, Linux's `wchan`, which is
 * `0` for a thread that runs: a thread in state D waits for the host's disk
 * or memory, and one asleep in a pipe's write waits for a reader. Nothing is
 * printed where there is no /proc.
 */
static void task_states(const char *who, const char *label, pid_t pid)
{
#ifndef __APPLE__
    char path[96], text[512], wchan[128];
    struct dirent *entry;
    DIR *dir;
    FILE *fp;
    size_t got;

    snprintf(path, sizeof(path), "/proc/%ld/task", (long)pid);
    dir = opendir(path);
    if (!dir)
        return;
    while ((entry = readdir(dir)) != NULL) {
        char *name, *paren;

        if (entry->d_name[0] < '0' || entry->d_name[0] > '9')
            continue;
        snprintf(path, sizeof(path), "/proc/%ld/task/%.16s/stat", (long)pid, entry->d_name);
        fp = fopen(path, "r");
        if (!fp)
            continue;
        got = fread(text, 1, sizeof(text) - 1, fp);
        fclose(fp);
        text[got] = 0;
        name = strchr(text, '(');
        paren = strrchr(text, ')');
        if (!name || !paren || paren < name || paren[1] != ' ' || !paren[2])
            continue;
        *paren = 0;
        snprintf(path, sizeof(path), "/proc/%ld/task/%.16s/wchan", (long)pid, entry->d_name);
        wchan[0] = 0;
        fp = fopen(path, "r");
        if (fp) {
            got = fread(wchan, 1, sizeof(wchan) - 1, fp);
            fclose(fp);
            wchan[got] = 0;
            wchan[strcspn(wchan, "\n")] = 0;
        }
        if (!wchan[0] || !strcmp(wchan, "0"))
            fprintf(stderr, "%s: %s %ld: thread %s (%s): state %c\n", who, label, (long)pid,
                    entry->d_name, name + 1, paren[2]);
        else
            fprintf(stderr, "%s: %s %ld: thread %s (%s): state %c in %s\n", who, label, (long)pid,
                    entry->d_name, name + 1, paren[2], wchan);
    }
    closedir(dir);
#else
    (void)who;
    (void)label;
    (void)pid;
#endif
}

/** Print the host's load averages and its pressure stall lines for the disk
 * and for memory, the share of time tasks waited on each, where Linux keeps
 * them. */
static void host_pressure(const char *who)
{
    static const char *const files[] = {"/proc/loadavg", "/proc/pressure/io",
                                        "/proc/pressure/memory"};
    static const char *const names[] = {"load", "disk pressure", "memory pressure"};
    char line[256];
    size_t i;

    for (i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        FILE *fp = fopen(files[i], "r");

        if (!fp)
            continue;
        while (fgets(line, sizeof(line), fp)) {
            line[strcspn(line, "\n")] = 0;
            fprintf(stderr, "%s: host %s: %s\n", who, names[i], line);
        }
        fclose(fp);
    }
}

/**
 * Read from the monitor `fd` into `buf`, of `size` bytes, until its prompt
 * comes after the first `skip` newlines, or `seconds` pass. A command's echo
 * is its first line, so `skip` 1 waits past it to the answer. Returns the
 * bytes read, or -1 when the prompt did not come.
 */
static long monitor_read(int fd, char *buf, size_t size, int skip, double seconds)
{
    double until = now() + seconds;
    size_t len = 0;

    buf[0] = 0;
    while (now() < until && len + 1 < size) {
        struct pollfd pfd;
        const char *from = buf;
        ssize_t n;
        int lines;

        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (poll(&pfd, 1, 100) <= 0)
            continue;
        n = read(fd, buf + len, size - 1 - len);
        if (n <= 0)
            return -1;
        len += (size_t)n;
        buf[len] = 0;
        for (lines = 0; lines < skip && from; lines++) {
            from = strchr(from, '\n');
            if (from)
                from++;
        }
        if (from && strstr(from, "(qemu)"))
            return (long)len;
    }
    return -1;
}

/**
 * Print each emulated processor's instruction pointer, flags and halt state
 * of machine `pid`, from `info registers -a` on the monitor `machine_exec`
 * gave it: the `CPU#N` line and the `EIP=` or `RIP=` line under it, whose
 * `HLT=1` is a halted processor and whose flags' 0x200 bit is the interrupt
 * flag. A monitor that gives no prompt is reported: QEMU's main loop is
 * blocked, or another client holds the monitor.
 */
static void guest_cpus(const char *who, const char *ledger, pid_t pid)
{
    static const char command[] = "info registers -a\n";
    static char reply[65536];
    char path[256];
    struct sockaddr_un addr;
    char *line, *next;
    int fd;

    if (monitor_path(path, sizeof(path), ledger, pid) != 0 || access(path, F_OK) != 0)
        return;
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path, strlen(path) + 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "%s: machine %ld: monitor: cannot connect: %s\n", who, (long)pid,
                strerror(errno));
        close(fd);
        return;
    }
    if (monitor_read(fd, reply, sizeof(reply), 0, 3) < 0) {
        fprintf(stderr, "%s: machine %ld: monitor: no prompt in 3 s\n", who, (long)pid);
        close(fd);
        return;
    }
    if (write_all(fd, command, sizeof(command) - 1) != 0 ||
        monitor_read(fd, reply, sizeof(reply), 1, 5) < 0) {
        fprintf(stderr, "%s: machine %ld: monitor: no answer to info registers in 5 s\n", who,
                (long)pid);
        close(fd);
        return;
    }
    close(fd);
    for (line = reply; line; line = next) {
        next = strchr(line, '\n');
        if (next)
            *next++ = 0;
        line[strcspn(line, "\r")] = 0;
        if (!strncmp(line, "CPU#", 4) || !strncmp(line, "EIP=", 4) || !strncmp(line, "RIP=", 4))
            fprintf(stderr, "%s: machine %ld: %s\n", who, (long)pid, line);
    }
}

/**
 * Print what machine `pid` of `ledger` is doing, each line headed `who`:
 * its threads, its console relay and its parent, as `task_states` gives
 * them, the host's load and pressure, and its processors from the monitor.
 * The relay is the pid after the machine's in the ledger. A machine that
 * stops printing and takes no processor time is either waiting in the host,
 * which its threads' states say, or halted in the guest, which its
 * processors say.
 */
static void machine_state(const char *who, const char *ledger, pid_t pid)
{
    FILE *fp = fopen(ledger, "r");
    long seen, relay = 0, parent = 0;
    char path[64], text[512];
    const char *paren;
    size_t got;

    if (fp) {
        while (fscanf(fp, "%ld", &seen) == 1)
            if (seen == (long)pid && fscanf(fp, "%ld", &relay) != 1)
                relay = 0;
        fclose(fp);
    }
    snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
    fp = fopen(path, "r");
    if (fp) {
        got = fread(text, 1, sizeof(text) - 1, fp);
        fclose(fp);
        text[got] = 0;
        paren = strrchr(text, ')');
        if (!paren || sscanf(paren + 1, " %*c %ld", &parent) != 1)
            parent = 0;
    }
    fprintf(stderr, "%s: the state of machine %ld:\n", who, (long)pid);
    task_states(who, "machine", pid);
    if (relay > 1)
        task_states(who, "relay", (pid_t)relay);
    if (parent > 1)
        task_states(who, "parent", (pid_t)parent);
    host_pressure(who);
    guest_cpus(who, ledger, pid);
}

/** Print `machine_state` for each live QEMU machine `ledger` records, those
 * whose command begins `qemu-system`. */
static void machine_states(const char *who, const char *ledger)
{
    FILE *fp = fopen(ledger, "r");
    long pid;

    if (!fp)
        return;
    while (fscanf(fp, "%ld", &pid) == 1) {
        struct identity id;

        if (pid > 1 && registered_machine(ledger, (pid_t)pid) &&
            process_identity((pid_t)pid, &id) == 1 && !strncmp(id.command, "qemu-system", 11))
            machine_state(who, ledger, (pid_t)pid);
    }
    fclose(fp);
}

/**
 * Say that the command `name` has done nothing for `idle` seconds, and print
 * its last output, the `recent_len` bytes of `recent`, the last output of
 * its machines' transcript `console`, and the state of each machine of
 * `ledger` that still runs (`machine_states`).
 */
static void report_hang(const char *name, double idle, const char *recent, size_t recent_len,
                        const char *console, const char *ledger)
{
    char who[128];

    fprintf(stderr,
            "run: %s: no output for %.0f s and next to no processor time; stopping the suite\n",
            name, idle);
    fprintf(stderr, "run: %s: the suite's last output:\n", name);
    write_all(STDERR_FILENO, recent, recent_len);
    write_all(STDERR_FILENO, "\n", 1);
    fprintf(stderr, "run: %s: the console's last output:\n", name);
    last_output(console);
    snprintf(who, sizeof(who), "run: %s", name);
    machine_states(who, ledger);
}

/**
 * The `watch` and `suite` modes: run `cmd` as the leader of a new process
 * group, copy its output to standard output, and stop it as hung when it has
 * done nothing for `idle` seconds. Returns the command's exit status, or 128
 * plus N when signal N ended it; 124 when it was stopped as hung; 128 plus N
 * when this process received signal N; 1 when `suite` is set and the
 * liveness check fails; and 2 when the command cannot be started.
 *
 * The command is working while it writes output, while `LEDGER.console`, the
 * transcript its machines' relays append to, grows, and while its group or
 * descendants use processor time: more than a tenth of a processor over the
 * span since it was last seen working, or a process started or ended, sampled
 * every ten seconds or twice within a shorter limit. A slow machine can write
 * nothing for minutes, and a halted one takes only a few percent for its timer,
 * under either accelerator, which is not working. When none of the three has
 * happened for `idle` seconds, which are never multiplied by the deadline
 * factor, `report_hang` says so and the command is stopped with
 * `stop_owned`, as it is when this process receives INT, TERM or HUP. A
 * line of progress goes each minute to standard error, or to descriptor 3
 * when `RACH2_PROGRESS_FD` is 3 and it is open.
 *
 * The group is recorded in `LEDGER.groups` before the wait. With `suite`
 * set, the command's processes inherit a liveness pipe, which `check_token`
 * reads when the command has ended. Both the parent and the child call
 * `setpgid`, so the group exists before any cleanup can need it, whichever
 * runs first; `execute` checks the child's call.
 *
 * With `record`, the `record` mode, `suite` is set too. The command's output
 * goes to standard error, and its processes keep standard output, which
 * etude's executor reads, for the `child` records of their machines
 * (`report_child`). When the command has ended by itself, the machines of
 * `ledger` have SETTLE_SECONDS to end, and those still running, and each
 * console a relay did not copy whole, go into the record (`start_record`).
 * The liveness check and `check_machines` then stop what is left, and the
 * record is finished (`finish_record`). The mode returns as `suite` does,
 * and 1 also when a machine was left running, outlived its TERM or had its
 * console cut; a record that cannot be written leaves none.
 */
static int supervise(char **cmd, const char *name, double idle, const char *ledger, int suite,
                     const char *record)
{
    int pipes[2], status = 0, done = 0, eof = 0, timedout = 0;
    int life[2] = {-1, -1}, leaked = 0, survivor = 0, ended = 0;
    int outfd = record ? STDERR_FILENO : STDOUT_FILENO;
    FILE *end = NULL;
    struct stat token;
    int progressfd = STDERR_FILENO;
    const char *progress = getenv("RACH2_PROGRESS_FD");
    pid_t child;
    char *console, buf[8192];
    struct recent_output recent;
    double started = now(), active = started, report = started;
    double period = idle < 20 ? (idle < 2 ? 1 : idle / 2) : 10;
    double busy = started, sampled = started - period, cpu_at_busy = -1;
    int count_last = -1;
    off_t previous = 0;
    struct sigaction sa;
    size_t len = strlen(ledger) + 16;
    recent.len = 0;
    if (progress && !strcmp(progress, "3") && fcntl(3, F_GETFD) >= 0)
        progressfd = 3;
    console = malloc(len);
    if (!console || pipe(pipes) != 0) {
        perror("runutil: pipe or allocation");
        free(console);
        return 2;
    }
    snprintf(console, len, "%s.console", ledger);
    if (suite && open_liveness(life, &token) != 0) {
        perror("runutil: liveness pipe");
        free(console);
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
        free(console);
        return 2;
    }
    if (child == 0) {
        if (suite)
            close(life[0]);
        close(pipes[0]);
        if (record)
            keep_children_fd();
        dup2(pipes[1], STDOUT_FILENO);
        dup2(pipes[1], STDERR_FILENO);
        close(pipes[1]);
        execute(cmd, 1);
    }
    (void)setpgid(child, child);
    if (suite)
        close(life[1]);
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
            active = now();
            keep_recent(&recent, buf, (size_t)n);
            if (write_all(outfd, buf, (size_t)n) != 0)
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
            report_hang(name, idle, recent.text, recent.len, console, ledger);
            timedout = 1;
        }
        if (interrupted || timedout) {
            stop_owned(child, ledger, suite ? &token : NULL);
            break;
        }
        if (done) {
            drain_output(pipes[0], outfd);
            break;
        }
    }
    if (!done)
        while (waitpid(child, &status, 0) < 0 && errno == EINTR)
            ;
    if (record)
        end = start_record(record, ledger, interrupted || timedout, &ended);
    if (suite) {
        leaked = check_token(life[0], &token, child, ledger, name, interrupted || timedout);
        close(life[0]);
    }
    if (record)
        survivor = check_machines(name, ledger);
    close(pipes[0]);
    free(console);
    status = exit_code(status);
    if (end)
        finish_record(end, record, status, timedout, leaked, survivor);
    if (timedout)
        return 124;
    if (interrupted)
        return 128 + interrupted;
    if (leaked || survivor || ended)
        return 1;
    return status;
}

/** Add this process's pid to `LEDGER.closed`: the relay's transcript has the
 * whole of its machine's output. */
static void note_closed(const char *ledger)
{
    char *path = malloc(strlen(ledger) + 16), line[32];
    int fd, n;

    if (!path)
        return;
    sprintf(path, "%s.closed", ledger);
    fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    free(path);
    if (fd < 0)
        return;
    n = snprintf(line, sizeof(line), "%ld\n", (long)getpid());
    write_all(fd, line, (size_t)n);
    close(fd);
}

/**
 * The console relay of `machine_exec`, in its child: copy the machine's
 * output from `in` to the transcript `logfd` and to standard output until
 * it ends, and exit. A relay that can no longer write sends the machine
 * SIGTERM. INT, TERM and HUP, which a suite that stops its machine by its
 * command line also sends the relay, leave it RELAY_SECONDS to copy the
 * rest, and end the copy at once when they interrupt a write to standard
 * output that cannot finish. The relay waits for output in `poll`, a second
 * at a time, so that a signal that comes just before a read cannot leave it
 * waiting past that bound. Once the transcript has the whole of the
 * machine's output, at its end or when the suite stops reading it, the
 * relay notes it in `LEDGER.closed` (`note_closed`); a relay that ends
 * otherwise has cut the console.
 */
static void relay_console(int in, int logfd, pid_t machine, const char *ledger)
{
    char buf[8192];
    ssize_t n = -1;
    int whole = 0;
    double t, until = 0;
    struct pollfd pfd;
    struct sigaction sa;

    close(STDIN_FILENO);
    signal(SIGPIPE, SIG_IGN);
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    pfd.fd = in;
    pfd.events = POLLIN;
    for (;;) {
        t = now();
        if (interrupted && until == 0)
            until = t + RELAY_SECONDS;
        if (until != 0 && t >= until)
            break;
        pfd.revents = 0;
        if (poll(&pfd, 1, until != 0 ? (int)((until - t) * 1000) + 1 : 1000) <= 0)
            continue;
        n = read(in, buf, sizeof(buf));
        if (n == 0)
            break;
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (write_all(logfd, buf, (size_t)n) != 0) {
            kill(machine, SIGTERM);
            break;
        }
        if (relay_write(STDOUT_FILENO, buf, (size_t)n) != 0) {
            kill(machine, SIGTERM);
            whole = 1;
            break;
        }
    }
    close(in);
    close(logfd);
    if (n == 0 || whole)
        note_closed(ledger);
    _exit(0);
}

/**
 * The `machine` mode: record this process as a machine in `ledger` and exec
 * `cmd` in it, with its output going through a relay child
 * (`relay_console`) to standard output and to `LEDGER.console`. The machine
 * keeps this process's pid, so a suite that signals the pid it started
 * signals the machine, SIGKILL too. Both pids go in the ledger, and their
 * identities in `LEDGER.identity`, for the cleanup at the suite's end, and
 * each goes as a `child` record to etude's executor under the `record` mode
 * (`report_child`). A QEMU, a command whose name begins `qemu-system`, is
 * given a monitor on the socket `monitor_path` names as well, which
 * `machine_state` reads when the machine stalls. Returns 2 when the pipe,
 * the transcript, the relay or the ledger cannot be made; an exec that fails
 * exits with status 127.
 */
static int machine_exec(const char *ledger, char **cmd)
{
    int pipes[2], logfd;
    pid_t relay, machine = getpid();
    size_t len = strlen(ledger) + 16, words;
    char *console = malloc(len), **argv, path[256], monitor[300];
    const char *base = strrchr(cmd[0], '/');
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
        close(pipes[1]);
        relay_console(pipes[0], logfd, machine, ledger);
    }
    close(pipes[0]);
    close(logfd);
    fp = fopen(ledger, "a");
    if (!fp || record_identity(ledger, machine, relay) != 0) {
        perror("runutil: machine ledger");
        if (fp)
            fclose(fp);
        close(pipes[1]);
        kill(relay, SIGTERM);
        waitpid(relay, NULL, 0);
        return 2;
    }
    fprintf(fp, "%ld\n%ld\n", (long)machine, (long)relay);
    fclose(fp);
    report_child(machine);
    report_child(relay);
    dup2(pipes[1], STDOUT_FILENO);
    dup2(pipes[1], STDERR_FILENO);
    close(pipes[1]);
    base = base ? base + 1 : cmd[0];
    for (words = 0; cmd[words]; words++)
        ;
    argv = malloc((words + 3) * sizeof(*argv));
    if (argv && !strncmp(base, "qemu-system", 11) &&
        monitor_path(path, sizeof(path), ledger, machine) == 0) {
        unlink(path);
        snprintf(monitor, sizeof(monitor), "unix:%s,server=on,wait=off", path);
        argv[0] = cmd[0];
        argv[1] = "-monitor";
        argv[2] = monitor;
        memcpy(argv + 3, cmd + 1, words * sizeof(*argv));
        cmd = argv;
    }
    execute(cmd, 0);
    return 127;
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

/** Start a command for the modes that run one, `group`, `exec` and `lead`;
 * return for any other.
 */
static void start_command(int argc, char **argv)
{
    if (argc < 3)
        return;
    if (!strcmp(argv[1], "group") || !strcmp(argv[1], "exec"))
        execute(argv + 2, !strcmp(argv[1], "group"));
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
    if ((argc == 5 || argc == 6) && !strcmp(argv[1], "until"))
        return until_token(argv[2], argv[3], strtod(argv[4], NULL),
                           argc == 6 ? (int)strtol(argv[5], NULL, 10) : 1);
    if (argc == 4 && !strcmp(argv[1], "gone"))
        return until_gone((pid_t)strtol(argv[2], NULL, 10), strtod(argv[3], NULL));
    if (argc == 3 && !strcmp(argv[1], "identity"))
        return print_identity((pid_t)strtol(argv[2], NULL, 10));
    if (argc == 3 && !strcmp(argv[1], "pgid"))
        return print_group((pid_t)strtol(argv[2], NULL, 10));
    if (argc == 4 && !strcmp(argv[1], "young"))
        return !is_young(argv[2], strtod(argv[3], NULL));
    if (argc >= 6 && (!strcmp(argv[1], "watch") || !strcmp(argv[1], "suite")))
        return supervise(argv + 5, argv[2], strtod(argv[3], NULL), argv[4],
                         !strcmp(argv[1], "suite"), NULL);
    if (argc >= 7 && !strcmp(argv[1], "record"))
        return supervise(argv + 6, argv[2], strtod(argv[3], NULL), argv[4], 1, argv[5]);
    if (argc == 3 && !strcmp(argv[1], "registered")) {
        registered_pids(argv[2]);
        return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "machines"))
        return check_machines(argv[2], argv[3]);
    if (argc >= 4 && !strcmp(argv[1], "machine"))
        return machine_exec(argv[2], argv + 3);
    if (argc == 5 && !strcmp(argv[1], "state")) {
        machine_state(argv[2], argv[3], (pid_t)strtol(argv[4], NULL, 10));
        return 0;
    }
    fprintf(stderr, "runutil: invalid arguments\n");
    return 2;
}
