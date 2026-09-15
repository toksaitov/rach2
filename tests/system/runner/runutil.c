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

/** Supervise a command, counting its output and its console transcript.
 * Console relays append independently, so an active guest keeps a silent
 * suite alive.
 * The suite's clock is never scaled. Its child group is stopped on silence
 * or a signal, and output from another suite cannot reset its clock.
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
    if (suite &&
        (pipe(life) != 0 || fstat(life[1], &token) != 0 ||
         fcntl(life[0], F_SETFD, FD_CLOEXEC) != 0 || fcntl(life[0], F_SETFL, O_NONBLOCK) != 0)) {
        perror("runutil: liveness pipe");
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
