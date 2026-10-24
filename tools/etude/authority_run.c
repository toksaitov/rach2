/* Keep the endpoint capability and its child alive under one native owner. */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "authority_run.h"
#include "observe_sequence.h"
#include "json.h"
#include "measurement_context.h"
#include "routine_proof.h"
#ifdef _WIN32
#include "win/files.h"
#else
#include <poll.h>
#include <fcntl.h>
#include "posix/posix.h"
#endif
extern char **environ;

static volatile sig_atomic_t stopped;
static void stop(int value)
{
    stopped = value;
}

static int input_lost(void)
{
#ifdef _WIN32
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD available;
    if (GetFileType(input) != FILE_TYPE_PIPE ||
        !PeekNamedPipe(input, NULL, 0, NULL, &available, NULL))
        return 1;
    return available != 0;
#else
    struct pollfd input = {0, POLLIN, 0};
    int rc = poll(&input, 1, 0);
    return rc < 0 ? errno != EINTR : rc && input.revents;
#endif
}

static int identity(const struct etude_pid *id)
{
    struct etude_pid now;
    return etude_process_state(id->pid, &now) == 1 && now.start_known && now.start == id->start;
}

/* The JSON wrapper is written from the actual executor result only after
 * cleanup. The original admission result stays a separate referenced file. */
static int receipt(const struct authority_run *run, const struct etude_exec_result *r,
                   const struct etude_reading *start)
{
    if (!run->receipt)
        return 0;
    struct etude_context_clock clock;
    struct etude_reading end;
    struct etude_pid owner;
    if (etude_process_state(getpid(), &owner) != 1 || !owner.start_known)
        return -1;
    if (etude_context_clock(&clock) || etude_history_now(&end))
        return -1;
    struct jbuf b = {0};
    jbuf_puts(&b, "{\"format\":\"etude-authority-child\",\"schema_version\":1,\"machine_id\":");
    routine_quote(&b, clock.machine_id);
    jbuf_puts(&b, ",\"boot_id\":");
    routine_quote(&b, clock.boot_id);
    jbuf_puts(&b, ",\"start\":");
    routine_reading(&b, start);
    jbuf_puts(&b, ",\"end\":");
    routine_reading(&b, &end);
    char values[768];
    snprintf(values, sizeof values,
             ",\"owner\":{\"pid\":\"%ld\",\"creation\":\"%llu\"},\"process\":{\"pid\":\"%ld\","
             "\"creation\":\"%llu\"},"
             "\"keeper\":{\"pid\":\"%ld\",\"creation\":\"%llu\"},"
             "\"result\":{\"end\":%d,\"value\":%d,\"exit_end\":%d,\"exit_value\":%d,"
             "\"cleanup\":%d,\"output_error\":%d}}\n",
             owner.pid, (unsigned long long)owner.start, r->process.pid,
             (unsigned long long)r->process.start, r->keeper.pid,
             (unsigned long long)r->keeper.start, r->end, r->value, r->exit_end, r->exit_value,
             r->cleanup, r->output_error);
    jbuf_puts(&b, values);
    int error = routine_create(run->receipt, &b);
    jbuf_free(&b);
    return error;
}

static void output(const struct etude_output *stream, size_t *seen, FILE *file)
{
    if (stream->kept > *seen) {
        if (fwrite(stream->data + *seen, 1, stream->kept - *seen, file) != stream->kept - *seen ||
            fflush(file))
            stopped = SIGTERM;
        *seen = stream->kept;
    }
}

#ifndef _WIN32
static int input_channel(int *writer, int *original)
{
    int channel[2];
    if (pipe(channel))
        return -1;
    *writer = channel[1];
    *original = dup(STDIN_FILENO);
    int error =
        *original < 0 || fcntl(*writer, F_SETFD, FD_CLOEXEC) || dup2(channel[0], STDIN_FILENO) < 0;
    close(channel[0]);
    return error ? -1 : 0;
}
#endif

int authority_run_command(const struct authority_run *run, struct etude_exec_result *result)
{
    struct etude_pid parent;
    if (etude_sequence_caller_identity(&parent))
        return 125;
    struct etude_reading start;
    if (etude_history_now(&start))
        return 125;
    if (run->clear_parent && unsetenv("ETUDE_SEQUENCE_PARENT"))
        return 125;
    stopped = 0;
    void (*oldterm)(int) = signal(SIGTERM, stop), (*oldint)(int) = signal(SIGINT, stop);
    struct etude_exec *executor = etude_exec_new();
    struct etude_proc *child = NULL;
    struct etude_exec_spec spec = {0};
    spec.attempt_id = run->phase ? run->phase : "authority";
    spec.argv = run->argv;
    spec.envp = environ;
    spec.stdin_path = run->forward_input ? "-" : NULL;
    spec.output_max = 64u << 20;
    spec.grace_us = 2000000;
    int status = 125;
    int writer = -1, original = -1;
#ifndef _WIN32
    if (run->dedicated_input) {
        if (input_channel(&writer, &original))
            goto done;
        spec.stdin_path = "-";
    }
#endif
    int launch = !executor || etude_exec_start(executor, &spec, &child);
#ifndef _WIN32
    if (original >= 0) {
        if (dup2(original, STDIN_FILENO) < 0)
            launch = 1;
        close(original);
        original = -1;
    }
#endif
    if (launch)
        goto done;
    size_t out = 0, err = 0;
    for (;;) {
        struct etude_proc *finished = etude_exec_wait(executor, 50);
        const struct etude_exec_result *r = etude_proc_result(child);
        output(&r->out, &out, stdout);
        output(&r->err, &err, stderr);
        if (stopped || !identity(&parent) || (run->watch_input && input_lost()))
            etude_exec_cancel(child);
        if (finished == child)
            break;
    }
    *result = *etude_proc_result(child);
    result->out.data = result->err.data = NULL;
    if (result->out.total != result->out.kept || result->err.total != result->err.kept) {
        result->output_error = EOVERFLOW;
        fputs("etude authority: owned output exceeded its retained bound\n", stderr);
    }
    if (receipt(run, result, &start))
        goto done;
    if (result->cleanup == ETUDE_CLEANUP_CONFIRMED && !result->output_error &&
        result->end == ETUDE_END_EXITED)
        status = result->value;
done:
#ifndef _WIN32
    if (original >= 0) {
        dup2(original, STDIN_FILENO);
        close(original);
    }
#endif
    if (writer >= 0)
        close(writer);
    etude_exec_free(executor);
    signal(SIGTERM, oldterm);
    signal(SIGINT, oldint);
    return status;
}
