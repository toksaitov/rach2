/* Every SSH client and peer is registered before it runs and reaped. */
#define _POSIX_C_SOURCE 200809L
#include "deploy.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern char **environ;
volatile sig_atomic_t nd_interrupted;
int nd_format(char *out, size_t size, const char *format, ...)
{
    va_list arguments;
    int n;
    va_start(arguments, format);
    n = vsnprintf(out, size, format, arguments);
    va_end(arguments);
    return n >= 0 && (size_t)n < size;
}
static int registered(void *ctx, const char *id, const struct etude_pid *keeper,
                      const struct etude_pid *process)
{
    struct nd_deploy *d = ctx;
    char path[4096];
    FILE *file;
    if (!nd_format(path, sizeof(path), "%s/processes", d->c.logs))
        return ENAMETOOLONG;
    file = fopen(path, "a");
    if (file == NULL)
        return errno;
    fprintf(file, "%s keeper %ld:%llu process %ld:%llu\n", id, keeper->pid,
            (unsigned long long)keeper->start, process->pid, (unsigned long long)process->start);
    return fclose(file) == 0 ? 0 : EIO;
}
struct nd_process *nd_start(struct nd_deploy *d, const char *name, char **argv, char **env,
                            const char *input)
{
    struct etude_exec_spec spec;
    struct nd_process *p;
    if ((!d->cleaning && nd_interrupted) || d->count == ND_PROCESSES)
        return NULL;
    p = &d->processes[d->count];
    if (!nd_format(p->id, sizeof(p->id), "%s-%lu", name, (unsigned long)d->count) ||
        !nd_format(p->out, sizeof(p->out), "%s/%s.out", d->c.logs, p->id) ||
        !nd_format(p->err, sizeof(p->err), "%s/%s.err", d->c.logs, p->id))
        return NULL;
    memset(&spec, 0, sizeof(spec));
    spec.attempt_id = p->id;
    spec.argv = argv;
    spec.envp = env != NULL ? env : environ;
    spec.stdin_path = input;
    spec.stdout_path = p->out;
    spec.stderr_path = p->err;
    spec.output_max = 65536;
    spec.grace_us = 5000000;
    spec.reg = registered;
    spec.reg_ctx = d;
    if (etude_exec_start(d->executor, &spec, &p->process) != 0)
        return NULL;
    d->count++;
    return p;
}
static int complete(const struct etude_exec_result *r)
{
    return r->finished_us != 0 && r->cleanup == ETUDE_CLEANUP_CONFIRMED && r->out.closed &&
           r->err.closed && r->output_error == 0;
}
static void observed(struct nd_deploy *d, struct etude_proc *process)
{
    unsigned i;
    if (d->cleaning)
        return;
    for (i = 0; i < 2; i++)
        if (d->peers[i] != NULL && d->peers[i]->process == process) {
            d->peer_failed = 1;
            fprintf(stderr, "native-deploy: external peer ended; evidence %s\n", d->peers[i]->err);
        }
}
int nd_wait(struct nd_deploy *d, struct nd_process *p)
{
    const struct etude_exec_result *r;
    struct etude_proc *ended;
    while ((r = etude_proc_result(p->process))->finished_us == 0) {
        if (!d->cleaning && (nd_interrupted || d->peer_failed))
            etude_exec_cancel(p->process);
        ended = etude_exec_wait(d->executor, 250);
        if (ended != NULL)
            observed(d, ended);
    }
    if (!complete(r) || r->end != ETUDE_END_EXITED)
        return -1;
    return r->value;
}
int nd_cleanup(struct nd_deploy *d)
{
    char path[4096];
    FILE *file;
    size_t n;
    int ok = 1;
    d->cleaning = 1;
    for (n = 0; n < d->count; n++)
        if (etude_proc_result(d->processes[n].process)->finished_us == 0)
            etude_exec_cancel(d->processes[n].process);
    for (n = 0; n < d->count; n++) {
        const struct etude_exec_result *r;
        struct nd_process *p = &d->processes[n];
        nd_wait(d, p);
        r = etude_proc_result(p->process);
        ok &= complete(r);
        if (nd_format(path, sizeof(path), "%s/cleanup", d->c.logs) &&
            (file = fopen(path, "a")) != NULL) {
            fprintf(file, "%s cleanup %s end %d value %d\n", p->id,
                    complete(r) ? "confirmed" : "unconfirmed", (int)r->end, r->value);
            ok &= fclose(file) == 0;
        } else
            ok = 0;
    }
    etude_exec_free(d->executor);
    d->executor = NULL;
    return ok;
}
int nd_ssh(struct nd_deploy *d, const char *name, const char *command, const char *input,
           struct nd_process **process)
{
    char target[512], known[4096], known_option[4200];
    char *argv[] = {(char *)d->c.ssh,
                    "-T",
                    "-i",
                    (char *)d->c.key,
                    "-o",
                    "BatchMode=yes",
                    "-o",
                    "ConnectTimeout=10",
                    "-o",
                    "ServerAliveInterval=10",
                    "-o",
                    "ServerAliveCountMax=2",
                    "-o",
                    "StrictHostKeyChecking=accept-new",
                    "-o",
                    known_option,
                    target,
                    (char *)command,
                    NULL};
    if (!nd_format(target, sizeof(target), "root@%s", d->c.host) ||
        !nd_format(known, sizeof(known), "%s/known-hosts", d->c.logs) ||
        !nd_format(known_option, sizeof(known_option), "UserKnownHostsFile=%s", known))
        return -1;
    *process = nd_start(d, name, argv, NULL, input);
    return *process != NULL ? nd_wait(d, *process) : -1;
}
