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

/** The lines of the file at `path` that match `re`, or 0 for a file not
 * there yet, which is one with nothing in it. The file is read whole at
 * each look, since a serial log grows at its end and a token may straddle
 * two reads. */
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
    /* A NUL in a serial log, a line the guest never finished, ends a line
       and not the text. */
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
 * still be signalled, so `kill` alone would call it alive. POSIX ps says. */
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
            fprintf(stderr, "runutil: setpgid: pid %ld, group %ld: %s\n", (long)pid, (long)actual,
                    strerror(error));
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
 * the time its children took is what says it is working. */
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
    /* The command name, in parentheses, may hold spaces; the fields are
       counted from after its closing one. */
    after = strrchr(text, ')');
    if (!after)
        return 0;
    /* Fields 16 and 17 of /proc/PID/stat are cutime and cstime, the
       reaped children's user and system time; the separator found on the
       loop's last pass is the one before field 16. */
    for (field = 3; field < 17 && after; field++)
        after = strchr(after + 1, ' ');
    if (!after || sscanf(after, "%ld %ld", &cutime, &cstime) != 2)
        return 0;
    return (cutime + cstime) / (double)sysconf(_SC_CLK_TCK);
}

/** The processor time the command's group has used, root and every process
 * in its group, from POSIX ps, with the time of the children each has
 * already reaped where the system reports it, plus the number of processes
 * in the group into `count`; -1 when ps cannot be read. A suite whose
 * machines still burn processor time is working, however silent, and so is
 * one whose processes come and go. */
static double group_cpu(pid_t root, int *count)
{
    FILE *fp = popen("ps -A -o pid= -o pgid= -o time=", "r");
    char line[256], timetext[64];
    long pid, group;
    double total = 0;
    *count = 0;
    if (!fp)
        return -1;
    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "%ld %ld %63s", &pid, &group, timetext) != 3)
            continue;
        if (pid == root || group == root) {
            total += cpu_seconds(timetext) + reaped_cpu(pid);
            (*count)++;
        }
    }
    pclose(fp);
    return total;
}

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

struct identity {
    pid_t group;
    unsigned long long start, fraction;
    int zombie;
    char command[64];
};

/** Read a PID's start time and state without treating a zombie as live.
 * A PID alone can identify an unrelated process after reuse. Zero means
 * gone; minus one means inspection was unavailable, not a live process.
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

/** Record identities beside the PID ledger before the machine execs. */
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

/** Match a registered process before signalling a retained ledger PID.
 * A missing identity file permits legacy PID-only ledgers used by the
 * lock recovery fixtures. Every new machine writes an identity file.
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

/** List current registered PIDs for the outer runner's cleanup. */
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

/** Check current identities after TERM, allowing fifteen seconds to stop.
 * Old machine and relay PIDs remain in the ledger across many boots.
 * Exited processes and reused PIDs cannot fail or be signalled by this
 * check. A failure prints the matching PID, group, start time and command.
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
            if (pid > 1 && pid != getpid() && registered_machine(ledger, (pid_t)pid)) {
                kill((pid_t)pid, sig);
                if (getpgid((pid_t)pid) == pid)
                    kill((pid_t)-pid, sig);
            }
        fclose(fp);
    }
}

/** Match a pipe writer by its kernel identity, not its descriptor number.
 * An open pipe asks for inspection. Only identified live holders can
 * affect the verdict; a vanished or unreadable process cannot fail it.
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

/** Collect current pipe holders with valid process groups.
 * The group is read between two descriptor checks. An exiting process
 * whose group or descriptors disappeared is not a named live holder.
 * Inspection can be incomplete; the caller must not infer a failure
 * from an empty result, even when the pipe is still open.
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

/** Signal newly inspected holders, rechecking identity before each kill. */
static void signal_token(const struct stat *token, int sig)
{
    size_t count, i;
    struct process *list = token_holders(token, &count);
    for (i = 0; i < count; i++)
        if (getpgid(list[i].pid) == list[i].group && holds_token(list[i].pid, token))
            kill(list[i].pid, sig);
    free(list);
}

/** Mark holders in the runner's group or any of its recorded groups.
 * All ledgers beside the suite's machine ledger belong to the same run.
 * A failed ledger read prevents an outside-group verdict: the checker
 * cannot establish that such a group was never recorded.
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

/** Stop a command, escalating after two seconds without losing its children. */
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

/** Return zero for EOF, one for a writer, or two for data or a read error. */
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

/** Inspect a completed suite without mistaking shutdown for detachment.
 * The inherited writer survives exec, setsid, setpgid and reparenting.
 * Closing inherited descriptors escapes this check. An open pipe with
 * no identifiable holder is inconclusive and cannot fail the suite.
 * A holder outside every recorded group fails immediately, with its PID
 * and group. Recorded groups receive TERM and fifteen seconds to stop,
 * including a QEMU still shutting down under the twenty-worker pool.
 * Only an identified survivor fails when that grace expires.
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

/** Supervise a command, counting its output, its console transcript and
 * its group's processor time. Console relays append independently, so an
 * active guest keeps a silent suite alive, and so does a group that still
 * burns processor time: a suite is stopped as hung only when it has been
 * silent and idle for the whole limit, since a slow machine writes for
 * minutes without a word and an idle one burns nothing. Working is more
 * than a tenth of a processor over the span since the group was last seen
 * working, or a process come or gone, sampled every ten seconds or twice
 * within a shorter limit: a halted machine still takes a few percent for
 * its timer, under either accelerator, and is not working.
 * The suite's clock is never scaled. Its child group is stopped on a signal
 * too, and output from another suite cannot reset its clock.
 * The parent tries to establish the group before cleanup can need it. The
 * child checks membership before exec; the parent's call may lose to exec.
 */
static int supervise(char **cmd, const char *name, double idle, const char *ledger, int suite)
{
    int pipes[2], status = 0, done = 0, eof = 0, timedout = 0;
    int life[2] = {-1, -1}, leaked = 0;
    struct stat token;
    int progressfd = STDERR_FILENO;
    const char *progress = getenv("RACH2_PROGRESS_FD");
    pid_t child;
    char *console, buf[8192], recent[4096];
    size_t recent_len = 0;
    double started = now(), active = started, report = started;
    double period = idle < 20 ? (idle < 2 ? 1 : idle / 2) : 10;
    double busy = started, sampled = started - period, cpu_at_busy = -1;
    int count_last = -1;
    off_t previous = 0;
    struct sigaction sa;
    size_t len = strlen(ledger) + 16;
    if (progress && !strcmp(progress, "3") && fcntl(3, F_GETFD) >= 0)
        progressfd = 3;
    console = malloc(len);
    if (!console || pipe(pipes) != 0) {
        perror("runutil: pipe or allocation");
        free(console);
        return 2;
    }
    snprintf(console, len, "%s.console", ledger);
    if (suite &&
        (pipe(life) != 0 || fstat(life[1], &token) != 0 ||
         fcntl(life[0], F_SETFD, FD_CLOEXEC) != 0 || fcntl(life[0], F_SETFL, O_NONBLOCK) != 0)) {
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
        if (!done && t - sampled >= period) {
            int count;
            double cpu = group_cpu(child, &count);

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
            fprintf(
                stderr,
                "run: %s: no output for %.0f s and next to no processor time; stopping the suite\n",
                name, idle);
            fprintf(stderr, "run: %s: the suite's last output:\n", name);
            write_all(STDERR_FILENO, recent, recent_len);
            write_all(STDERR_FILENO, "\n", 1);
            fprintf(stderr, "run: %s: the console's last output:\n", name);
            last_output(console);
            timedout = 1;
        }
        if (interrupted || timedout) {
            stop_owned(child, ledger, suite ? &token : NULL);
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
    if (suite) {
        leaked = check_token(life[0], &token, child, ledger, name, interrupted || timedout);
        close(life[0]);
    }
    close(pipes[0]);
    free(console);
    if (timedout)
        return 124;
    if (interrupted)
        return 128 + interrupted;
    if (leaked)
        return 1;
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
    dup2(pipes[1], STDOUT_FILENO);
    dup2(pipes[1], STDERR_FILENO);
    close(pipes[1]);
    execute(cmd, 0);
    return 127;
}

/** Remove the directory the environment variable `name` gives, if it is set. */
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
 * only for that claim.
 */
static void remove_work(void)
{
    remove_named("RACH2_RUN_WORK");
    remove_named("RACH2_RUN_TREE_LOCK");
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

/** Open a block's lock file, which every account on the host must be able to
 * lock. The file is made mode 0666 by its first opener. An existing file is
 * opened without O_CREAT, which Linux refuses on another account's file in a
 * sticky directory such as /tmp.
 */
static int open_shared(const char *path)
{
    int fd = open(path, O_RDWR);
    if (fd >= 0 || errno != ENOENT)
        return fd;
    fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0666);
    if (fd >= 0) {
        fchmod(fd, 0666);
        return fd;
    }
    return errno == EEXIST ? open(path, O_RDWR) : -1;
}

/** End the port claim with 128 plus the number of a signal it has received.
 * The signals in `stop` are blocked except while the claim waits, and here,
 * where any that arrived during a pass over the blocks are delivered.
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

/** Run the command that has claimed a block. The caller has called
 * `stop_if_interrupted` with the signals in `stop` still blocked, and the
 * handlers become the defaults before they are unblocked, so a signal that
 * arrives after that check ends the process instead of being lost when exec
 * replaces it. The run's work directory is removed first if one is pending.
 */
static void ports_start(const sigset_t *stop, char **cmd)
{
    sigset_t pending;

    reset_signals();
    if (sigpending(&pending) == 0 &&
        (sigismember(&pending, SIGINT) || sigismember(&pending, SIGTERM) ||
         sigismember(&pending, SIGHUP)))
        remove_work();
    sigprocmask(SIG_UNBLOCK, stop, NULL);
    execute(cmd, 0);
}

/** Claim a block of host ports and hold it through exec, as `lock` holds the
 * tree. Block N is `DIR/rach2-ports.N`, locked with a POSIX record lock, and
 * the command runs with `RACH2_PORT_OFFSET` set to N * 1000, which
 * tests/system/suites.mk adds to every port it forwards. The lowest free
 * block is taken; while every block is held, the claim waits. An interrupt,
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

/** Small POSIX interfaces used by the shell runner on macOS and Linux. */
int main(int argc, char **argv)
{
    if (argc >= 3 && (!strcmp(argv[1], "group") || !strcmp(argv[1], "exec")))
        execute(argv + 2, !strcmp(argv[1], "group"));
    if (argc >= 5 && !strcmp(argv[1], "lock"))
        lock_exec(argv[2], argv[3], argv + 4);
    if (argc >= 5 && !strcmp(argv[1], "ports"))
        ports_exec(argv[2], (int)strtol(argv[3], NULL, 10), argv + 4);
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
    if ((argc == 5 || argc == 6) && !strcmp(argv[1], "until"))
        return until_token(argv[2], argv[3], strtod(argv[4], NULL),
                           argc == 6 ? (int)strtol(argv[5], NULL, 10) : 1);
    if (argc == 4 && !strcmp(argv[1], "gone"))
        return until_gone((pid_t)strtol(argv[2], NULL, 10), strtod(argv[3], NULL));
    if (argc == 3 && !strcmp(argv[1], "pgid")) {
        pid_t group = getpgid((pid_t)strtol(argv[2], NULL, 10));
        if (group < 0)
            return 1;
        printf("%ld\n", (long)group);
        return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "young"))
        return !is_young(argv[2], strtod(argv[3], NULL));
    if (argc >= 6 && (!strcmp(argv[1], "watch") || !strcmp(argv[1], "suite")))
        return supervise(argv + 5, argv[2], strtod(argv[3], NULL), argv[4],
                         !strcmp(argv[1], "suite"));
    if (argc == 3 && !strcmp(argv[1], "registered")) {
        registered_pids(argv[2]);
        return 0;
    }
    if (argc == 4 && !strcmp(argv[1], "machines"))
        return check_machines(argv[2], argv[3]);
    if (argc >= 4 && !strcmp(argv[1], "machine"))
        return machine_exec(argv[2], argv + 3);
    fprintf(stderr, "runutil: invalid arguments\n");
    return 2;
}
