/* The case's claim includes its load, whose processes are registered first. */
#include "collection_load.h"
#include "posix/posix.h"
#include "etude/ledger.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define READY_US UINT64_C(30000000)
#define GRACE_US UINT64_C(5000000)
struct etude_collection_load {
    struct etude_exec *executor;
    struct etude_proc *process;
    const struct etude_load *load;
    uint64_t ready_us;
    int ended, failed, stopping;
    char output[4096], error[4096], record[2048];
};

static const char *quality(enum etude_quality value)
{
    return value == ETUDE_QUALITY_EXACT         ? "exact"
           : value == ETUDE_QUALITY_LOWER_BOUND ? "lower_bound"
                                                : "unavailable";
}

static const char *kind(enum etude_load_kind value)
{
    return value == ETUDE_LOAD_CPU ? "cpu" : value == ETUDE_LOAD_MEMORY ? "memory" : "disk";
}

int etude_collection_load_resources(const struct etude_load *load, uint64_t resources[3])
{
    if (!load || !load->amount || load->amount > ETUDE_REPEAT_SEED_MAX)
        return -1;
    resources[0] = load->kind == ETUDE_LOAD_CPU ? load->amount * 1000 : 1000;
    resources[1] = UINT64_C(16777216);
    resources[2] = 65536;
    if (load->kind == ETUDE_LOAD_MEMORY)
        resources[1] += load->amount;
    else if (load->kind == ETUDE_LOAD_DISK)
        resources[2] += load->amount;
    else if (load->kind == ETUDE_LOAD_CPU && load->amount <= 1024)
        resources[1] += load->amount * UINT64_C(1048576);
    else
        return -1;
    return 0;
}
/* Add the case to the running load claim; neither keeper proves both trees. */
int etude_collection_load_register_case(void *context, const char *claim,
                                        const struct etude_pid *keeper,
                                        const struct etude_pid *process)
{
    int error = 0;
    if (keeper && keeper->pid > 0)
        error = etude_ledger_child(context, claim, keeper);
    return error ? error : etude_ledger_child(context, claim, process);
}

struct etude_collection_load *
etude_collection_load_start(const struct etude_collection_load_spec *s)
{
    struct etude_collection_load *l = calloc(1, sizeof(*l));
    if (!l)
        return NULL;
    l->load = s->load;
    int a = snprintf(l->output, sizeof(l->output), "%s.load.out", s->output);
    int b = snprintf(l->error, sizeof(l->error), "%s.load.err", s->output);
    if (a < 0 || (size_t)a >= sizeof(l->output) || b < 0 || (size_t)b >= sizeof(l->error)) {
        free(l);
        errno = ENAMETOOLONG;
        return NULL;
    }
    char amount[32];
    snprintf(amount, sizeof(amount), "%llu", (unsigned long long)s->load->amount);
    char *argv[] = {(char *)s->program, "stress", (char *)kind(s->load->kind), amount, NULL};
    char *environment[] = {"PATH=", NULL};
    struct etude_exec_spec spec;
    memset(&spec, 0, sizeof(spec));
    spec.attempt_id = s->claim;
    spec.argv = argv;
    spec.envp = environment;
    spec.cwd = s->workspace;
    spec.stdout_path = l->output;
    spec.stderr_path = l->error;
    spec.output_max = 65536;
    spec.grace_us = GRACE_US;
    spec.reg = etude_ledger_register;
    spec.child = etude_ledger_child;
    spec.reg_ctx = s->ledger;
    l->executor = etude_exec_new();
    if (!l->executor || etude_exec_start(l->executor, &spec, &l->process)) {
        etude_collection_load_free(l);
        return NULL;
    }
    return l;
}

int etude_collection_load_wait(struct etude_collection_load *l, int milliseconds)
{
    if (!l)
        return -1;
    if (!l->ended && etude_exec_wait(l->executor, milliseconds))
        l->ended = 1;
    const struct etude_exec_result *r = etude_proc_result(l->process);
    if (l->ended && !l->stopping)
        l->failed = 1;
    if (r->output_error)
        l->failed = 1;
    if (!l->ready_us && r->out.kept >= sizeof(ETUDE_LOAD_READY) - 1) {
        if (!memcmp(r->out.data, ETUDE_LOAD_READY, sizeof(ETUDE_LOAD_READY) - 1))
            l->ready_us = posix_now_us();
        else
            l->failed = 1;
    }
    if (!l->ready_us && posix_now_us() - r->started_us > READY_US)
        l->failed = 1;
    return l->failed ? -1 : l->ready_us ? 1 : 0;
}

int etude_collection_load_poll(struct etude_collection_load *l)
{
    return etude_collection_load_wait(l, 0);
}

enum etude_cleanup etude_collection_load_stop(struct etude_collection_load *l)
{
    if (!l)
        return ETUDE_CLEANUP_CONFIRMED;
    etude_collection_load_poll(l);
    l->stopping = 1;
    if (!l->ended)
        etude_exec_cancel(l->process);
    while (!l->ended)
        if (etude_exec_wait(l->executor, 1000))
            l->ended = 1;
    return etude_proc_result(l->process)->cleanup;
}

int etude_collection_load_failed(const struct etude_collection_load *l)
{
    return l && l->failed;
}

struct etude_measure etude_collection_load_cpu(const struct etude_collection_load *l)
{
    struct etude_measure missing = {ETUDE_QUALITY_UNAVAILABLE, 0, "the load did not start"};
    return l && l->process ? etude_proc_result(l->process)->cpu_us : missing;
}

const char *etude_collection_load_output(const struct etude_collection_load *l, int error)
{
    return l ? (error ? l->error : l->output) : NULL;
}

const char *etude_collection_load_record(struct etude_collection_load *l)
{
    if (!l || !l->process)
        return "null";
    const struct etude_exec_result *r = etude_proc_result(l->process);
    char cpu[32] = "null", peak[32] = "null";
    if (r->cpu_us.quality)
        snprintf(cpu, sizeof(cpu), "%llu", (unsigned long long)r->cpu_us.value);
    if (r->peak_bytes.quality)
        snprintf(peak, sizeof(peak), "%llu", (unsigned long long)r->peak_bytes.value);
    snprintf(l->record, sizeof(l->record),
             "{\"load_id\":\"%s\",\"kind\":\"%s\",\"amount\":%llu,"
             "\"ready_us\":%llu,\"ended_us\":%llu,\"cleanup\":\"%s\",\"failed\":%s,"
             "\"keeper\":{\"pid\":%ld,\"creation\":%llu},"
             "\"process\":{\"pid\":%ld,\"creation\":%llu},\"end\":%d,\"value\":%d,"
             "\"cpu_us\":%s,\"cpu_quality\":\"%s\",\"cpu_method\":\"%s\","
             "\"peak_bytes\":%s,\"peak_quality\":\"%s\",\"peak_method\":\"%s\"}",
             l->load->load_id, kind(l->load->kind), (unsigned long long)l->load->amount,
             (unsigned long long)l->ready_us, (unsigned long long)r->finished_us,
             r->cleanup == ETUDE_CLEANUP_CONFIRMED ? "confirmed" : "unconfirmed",
             l->failed ? "true" : "false", r->keeper.pid, (unsigned long long)r->keeper.start,
             r->process.pid, (unsigned long long)r->process.start, (int)r->end, r->value, cpu,
             quality(r->cpu_us.quality), r->cpu_us.method, peak, quality(r->peak_bytes.quality),
             r->peak_bytes.method);
    return l->record;
}

void etude_collection_load_free(struct etude_collection_load *l)
{
    if (!l)
        return;
    if (l->process)
        etude_collection_load_stop(l);
    if (l->executor)
        etude_exec_free(l->executor);
    free(l);
}
