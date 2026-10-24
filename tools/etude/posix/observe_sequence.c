/* Native ownership of observation, preparation and collection on POSIX. */
#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "etude/exec.h"
#include "posix.h"
#include "../observe_sequence.h"
#include "../routine_direct.h"

extern char **environ;
static volatile sig_atomic_t stopped;

int etude_sequence_caller_identity(struct etude_pid *identity)
{
    return etude_sequence_parent_identity(getppid(), identity);
}

int etude_posix_observe_parent_main(int argc, char **argv)
{
    struct etude_pid identity;
    (void)argv;
    if (argc != 2 || etude_process_state(getppid(), &identity) != 1 || !identity.start_known)
        return 2;
    return printf("%ld %llu\n", identity.pid, (unsigned long long)identity.start) < 0 ? 1 : 0;
}

struct sequence_phase {
    const char *name;
    struct etude_proc *process;
    size_t out, err;
    int done, status;
    struct etude_reading started;
};

struct sequence {
    const char *run, *trace, *stop, *program, *prep_path, *runtime_path;
    char **prep_argv, **run_argv;
    struct etude_exec *executor;
    struct etude_pid parent, owner;
    struct sequence_phase phases[3];
    FILE *record, *log;
    int observer_input, error, cancelled, cleanup, watch_input;
};

static void stop(int signal_number)
{
    stopped = signal_number;
}

static char **vector(int argc, char **argv, int *position, const char *program)
{
    char *end;
    if (*position + 2 > argc || strcmp(argv[(*position)++], program))
        return NULL;
    const char *number = argv[(*position)++];
    if (*number < '1' || *number > '9')
        return NULL;
    errno = 0;
    unsigned long count = strtoul(number, &end, 10);
    if (errno || *end || count < 2 || count > (unsigned long)(argc - *position))
        return NULL;
    char **result = calloc(count + 1, sizeof(*result));
    if (result) {
        memcpy(result, argv + *position, count * sizeof(*result));
        *position += (int)count;
    }
    return result;
}

static int arguments(struct sequence *s, int argc, char **argv)
{
    if (argc < 17)
        return -1;
    s->run = argv[3];
    s->trace = argv[4];
    s->stop = argv[5];
    s->program = argv[6];
    s->prep_path = argv[7];
    s->runtime_path = argv[8];
    size_t n = strlen(s->run);
    const char *alphabet = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._:/-";
    if (!n || n > 255 || strspn(s->run, alphabet) != n || s->program[0] != '/')
        return -1;
    int position = 9;
    s->prep_argv = vector(argc, argv, &position, s->program);
    s->run_argv = vector(argc, argv, &position, s->program);
    if (!s->prep_argv || !s->run_argv || position != argc)
        return -1;
    return strcmp(s->prep_argv[0], s->program) || strcmp(s->run_argv[0], s->program) ||
                   strcmp(s->prep_argv[1], "admit") || strcmp(s->run_argv[1], "run")
               ? -1
               : 0;
}

static int stop_matches(const struct sequence *s)
{
    FILE *f = fopen(s->stop, "rb");
    char bytes[257];
    if (!f)
        return errno == ENOENT ? 0 : -1;
    size_t n = fread(bytes, 1, sizeof(bytes), f);
    int match = !ferror(f) && n == strlen(s->run) + 1 && !memcmp(bytes, s->run, n - 1) &&
                bytes[n - 1] == '\n';
    if (fclose(f))
        match = 0;
    return match ? 1 : -1;
}

static int request_stop(struct sequence *s)
{
    int match = stop_matches(s);
    if (match)
        return match == 1 ? 0 : -1;
    char part[4096], text[257];
    int n = snprintf(part, sizeof(part), "%s.owner-%ld.tmp", s->stop, (long)getpid());
    if (n < 0 || (size_t)n >= sizeof(part))
        return -1;
    n = snprintf(text, sizeof(text), "%s\n", s->run);
    int fd = open(part, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0)
        return -1;
    int error = write(fd, text, (size_t)n) != n || fsync(fd);
    if (close(fd))
        error = 1;
    if (!error && link(part, s->stop))
        error = 1;
    if (unlink(part))
        error = 1;
    return error ? -1 : 0;
}

/* An identity is recorded before the executor lets its program run. */
static int registered(void *context, const char *attempt, const struct etude_pid *keeper,
                      const struct etude_pid *process)
{
    struct sequence *s = context;
    if (fprintf(s->record, "phase-child %s %ld %llu keeper %ld %llu\n", attempt, process->pid,
                (unsigned long long)process->start, keeper->pid,
                (unsigned long long)keeper->start) < 0 ||
        fflush(s->record))
        return EIO;
    return stopped ? ECANCELED : 0;
}

static int begin(struct sequence *s, unsigned index, char **argv, int observer)
{
    struct sequence_phase *p = &s->phases[index];
    if (setenv("PATH", index == 1 ? s->prep_path : s->runtime_path, 1))
        return -1;
    if (etude_history_now(&p->started))
        return -1;
    struct etude_exec_spec spec = {0};
    spec.attempt_id = p->name;
    spec.argv = argv;
    spec.envp = environ;
    spec.stdin_path = observer ? "-" : NULL;
    spec.output_max = 64u << 20;
    spec.grace_us = 2000000;
    spec.reg = registered;
    spec.reg_ctx = s;
    if (fprintf(s->record, "phase-begin %s %s %llu\n", s->run, p->name,
                (unsigned long long)posix_now_us()) < 0 ||
        fflush(s->record))
        return -1;
    return etude_exec_start(s->executor, &spec, &p->process);
}

/* Only the owner retains the write end of the observer's input pipe. */
static int begin_observer(struct sequence *s)
{
    int pipe_fds[2], saved = dup(STDIN_FILENO);
    if (saved < 0 || pipe(pipe_fds)) {
        if (saved >= 0)
            close(saved);
        return -1;
    }
    int error = fcntl(pipe_fds[0], F_SETFD, FD_CLOEXEC) < 0 ||
                fcntl(pipe_fds[1], F_SETFD, FD_CLOEXEC) < 0 || dup2(pipe_fds[0], STDIN_FILENO) < 0;
    char pid[32], creation[32];
    snprintf(pid, sizeof(pid), "%ld", s->owner.pid);
    snprintf(creation, sizeof(creation), "%llu", (unsigned long long)s->owner.start);
    char *argv[] = {(char *)s->program,
                    "observe",
                    "-o",
                    (char *)s->trace,
                    "-p",
                    pid,
                    "-c",
                    creation,
                    "-r",
                    (char *)s->run,
                    "-s",
                    (char *)s->stop,
                    NULL};
    if (!error)
        error = begin(s, 0, argv, 1);
    if (dup2(saved, STDIN_FILENO) < 0)
        error = -1;
    close(saved);
    close(pipe_fds[0]);
    s->observer_input = pipe_fds[1];
    return error;
}

static int output(const struct etude_output *o, size_t *seen, FILE *stream, FILE *log)
{
    if (o->kept > *seen) {
        size_t n = o->kept - *seen;
        if (fwrite(o->data + *seen, 1, n, stream) != n || fflush(stream))
            return -1;
        if (log && (fwrite(o->data + *seen, 1, n, log) != n || fflush(log)))
            return -1;
        *seen = o->kept;
    }
    return o->total > o->kept ? -1 : 0;
}

static void cancel(struct sequence *s)
{
    for (unsigned i = 0; i < 3; i++)
        if (s->phases[i].process && !s->phases[i].done)
            etude_exec_cancel(s->phases[i].process);
}

static void parent_check(struct sequence *s)
{
    struct etude_pid current;
    struct pollfd input = {STDIN_FILENO, POLLIN, 0};
    int input_gone = s->watch_input && poll(&input, 1, 0) > 0 && input.revents;
    if (stopped || input_gone || etude_process_state(s->parent.pid, &current) != 1 ||
        !current.start_known || current.start != s->parent.start) {
        s->cancelled = 1;
        cancel(s);
    }
}

/* Poll all owned attempts even while the preparation command is active. */
static int poll_sequence(struct sequence *s)
{
    parent_check(s);
    struct etude_proc *done = etude_exec_wait(s->executor, 100);
    for (unsigned i = 0; i < 3; i++) {
        struct sequence_phase *p = &s->phases[i];
        if (!p->process)
            continue;
        if (p->process == done)
            p->done = 1;
        const struct etude_exec_result *r = etude_proc_result(p->process);
        if (output(&r->out, &p->out, stdout, s->log) || output(&r->err, &p->err, stderr, s->log) ||
            r->output_error)
            s->error = EIO;
    }
    if (s->error)
        cancel(s);
    return s->error || s->cancelled ? -1 : 0;
}

static int end_phase(struct sequence *s, unsigned index)
{
    struct sequence_phase *p = &s->phases[index];
    const struct etude_exec_result *r = etude_proc_result(p->process);
    p->status = r->end == ETUDE_END_EXITED ? r->value : 125;
    if (r->cleanup != ETUDE_CLEANUP_CONFIRMED) {
        s->cleanup = 0;
        s->error = EBUSY;
    }
    if (fprintf(s->record, "phase-end %s %s %llu status %d cleanup %s cpu_us %llu\n", s->run,
                p->name, (unsigned long long)posix_now_us(), p->status,
                r->cleanup == ETUDE_CLEANUP_CONFIRMED ? "confirmed" : "unconfirmed",
                (unsigned long long)r->cpu_us.value) < 0 ||
        fflush(s->record))
        s->error = EIO;
    if (index && routine_direct_phase(p->name, &s->owner, r, &p->started))
        s->error = EIO;
    etude_proc_free(p->process);
    p->process = NULL;
    return s->error ? -1 : 0;
}

static int observer_ended(struct sequence *s, unsigned index)
{
    const struct sequence_phase *p = &s->phases[0];
    if (index == 0 || !p->done)
        return 0;
    const struct etude_exec_result *r = etude_proc_result(p->process);
    return index != 2 || r->end != ETUDE_END_EXITED || r->value || stop_matches(s) != 1;
}

static int wait_phase(struct sequence *s, unsigned index)
{
    while (!s->phases[index].done) {
        if (poll_sequence(s))
            return -1;
        if (observer_ended(s, index)) {
            s->error = EPIPE;
            return -1;
        }
    }
    return end_phase(s, index);
}

static int observer_ready(struct sequence *s)
{
    const char *ready = "etude observe: ready\n";
    for (;;) {
        if (poll_sequence(s))
            return -1;
        if (s->phases[0].done) {
            s->error = EPIPE;
            return -1;
        }
        const struct etude_exec_result *r = etude_proc_result(s->phases[0].process);
        if (r->out.kept >= strlen(ready))
            return memcmp(r->out.data, ready, strlen(ready)) ? -1 : 0;
    }
}

static int run_sequence(struct sequence *s)
{
    if (begin_observer(s) || observer_ready(s) || begin(s, 1, s->prep_argv, 0) || wait_phase(s, 1))
        return 125;
    int status = s->phases[1].status;
    if (!status) {
        int ack;
        while ((ack = routine_direct_ack()) == 0)
            if (poll_sequence(s) || observer_ended(s, 1))
                return 125;
        if (ack < 0)
            return 125;
        if (unsetenv("ETUDE_PREPARATION_GRANT") || unsetenv("ETUDE_PREPARATION_IMPORT") ||
            unsetenv("ETUDE_PREPARATION_CLEAN"))
            return 125;
        if (begin(s, 2, s->run_argv, 0) || wait_phase(s, 2))
            return 125;
        status = s->phases[2].status;
        if (!status && stop_matches(s) != 1)
            return 125;
    }
    if (request_stop(s) || wait_phase(s, 0) || s->phases[0].status)
        return 125;
    return status;
}

static int sequence_open(struct sequence *s, const char *path)
{
    struct stat st;
    const char *input = getenv("ETUDE_SEQUENCE_INPUT");
    if (input && strcmp(input, "pipe"))
        return -1;
    s->watch_input = input != NULL;
    if (!lstat(s->trace, &st) || errno != ENOENT || !lstat(s->stop, &st) || errno != ENOENT)
        return -1;
    if (etude_sequence_parent_identity(getppid(), &s->parent) ||
        etude_process_state(getpid(), &s->owner) != 1 || !s->owner.start_known ||
        s->parent.start > s->owner.start)
        return -1;
    int fd = open(path, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
    if (fd < 0)
        return -1;
    s->record = fdopen(fd, "wb");
    if (!s->record) {
        close(fd);
        return -1;
    }
    if (fprintf(s->record, "owner %ld %llu\nparent %ld %llu\nrun %s\n", s->owner.pid,
                (unsigned long long)s->owner.start, s->parent.pid,
                (unsigned long long)s->parent.start, s->run) < 0 ||
        fflush(s->record))
        return -1;
    const char *log_path = getenv("ETUDE_SEQUENCE_LOG");
    if (log_path && *log_path) {
        fd = open(log_path, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
        if (fd < 0)
            return -1;
        s->log = fdopen(fd, "wb");
        if (!s->log) {
            close(fd);
            return -1;
        }
    }
    s->executor = etude_exec_new();
    return s->executor ? 0 : -1;
}

static int sequence_close(struct sequence *s, int status)
{
    cancel(s);
    for (unsigned i = 0; i < 3; i++) {
        if (!s->phases[i].process)
            continue;
        while (!s->phases[i].done)
            poll_sequence(s);
        end_phase(s, i);
    }
    if (s->executor)
        etude_exec_free(s->executor);
    if (s->observer_input >= 0)
        close(s->observer_input);
    if (s->log && fclose(s->log))
        s->error = EIO;
    int result = !s->cleanup ? 125 : s->cancelled ? 130 : s->error ? 125 : status;
    if (s->record) {
        if (fprintf(s->record, "error %d\ncancelled %d\nstatus %d\nactive %s\ncleanup %s\n",
                    s->error, s->cancelled, result, s->cleanup ? "0" : "unknown",
                    s->cleanup ? "confirmed" : "unconfirmed") < 0)
            result = 125;
        if (fclose(s->record))
            result = 125;
    }
    free(s->prep_argv);
    free(s->run_argv);
    return result;
}

int etude_posix_observe_sequence_main(int argc, char **argv)
{
    struct sequence s = {0};
    s.observer_input = -1;
    s.cleanup = 1;
    s.phases[0].name = "observer";
    s.phases[1].name = "preparation";
    s.phases[2].name = "collection";
    if (arguments(&s, argc, argv)) {
        fputs("etude observe sequence: invalid counted sequence arguments\n", stderr);
        return sequence_close(&s, 2);
    }
    struct sigaction action = {0}, old_int, old_term, old_hup;
    action.sa_handler = stop;
    sigemptyset(&action.sa_mask);
    stopped = 0;
    sigaction(SIGINT, &action, &old_int);
    sigaction(SIGTERM, &action, &old_term);
    sigaction(SIGHUP, &action, &old_hup);
    int status;
    if (sequence_open(&s, argv[2])) {
        perror("etude observe sequence");
        s.error = errno ? errno : EIO;
        status = 125;
    } else
        status = run_sequence(&s);
    int result = sequence_close(&s, status);
    sigaction(SIGINT, &old_int, NULL);
    sigaction(SIGTERM, &old_term, NULL);
    sigaction(SIGHUP, &old_hup, NULL);
    return result;
}
#endif
