/* Record measured native steps, terminal results and complete coverage. */
#define _POSIX_C_SOURCE 200809L
#include "history.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static const char *outcome(int status)
{
    return !status ? "pass" : status == 3 ? "cancelled" : status == 4 ? "incomplete" : "failure";
}
static void result(struct cmp_text *t, int status, int cleaned, const char *reason, int proof)
{
    cmp_printf(t, "{\"outcome\":\"%s\",\"reason\":", outcome(status));
    if (!status)
        cmp_printf(t, "null");
    else
        nc_quote(t, reason && *reason ? reason : "native case did not complete successfully");
    cmp_printf(t, ",\"cleanup\":\"%s\",\"evidence\":[],\"capture_errors\":[],\"disruptions\":[",
               cleaned ? "confirmed" : "unconfirmed");
    if (proof)
        nc_quote(t, "intentional protocol proof");
    cmp_printf(t, "]}");
}
static int resources(struct nc_history *h, struct cmp_text *t, const struct nc_measure *m,
                     const struct etude_native_result *r, int measured)
{
    struct cmp_text scope = {0};
    cmp_printf(&scope, "{\"machine_id\":");
    nc_quote(&scope, h->machine);
    cmp_printf(&scope, ",\"boot_id\":");
    nc_quote(&scope, r->attempt.boot);
    cmp_printf(&scope,
               ",\"pid\":%ld,\"creation\":%llu,\"creation_known\":%s,"
               "\"method\":\"etude-guest-owned-tree\"}",
               r->attempt.process.pid, (unsigned long long)r->attempt.process.start,
               r->attempt.process.start_known ? "true" : "false");
    cmp_printf(t, "{\"scope\":");
    int ok = nc_blob(h, t, scope.data, scope.length);
    free(scope.data);
    struct etude_measure absent = {ETUDE_QUALITY_UNAVAILABLE, 0,
                                   "individual stage usage is not available"};
    cmp_printf(t, ",\"tree_cpu_us\":");
    nc_metric(t, measured ? &m->cpu : &absent);
    cmp_printf(t, ",\"combined_peak_bytes\":");
    nc_metric(t, measured ? &m->memory : &absent);
    cmp_printf(t, ",\"samples\":[]}");
    return ok;
}
static int assertions(struct nc_history *h, struct nc_measure *m,
                      const struct etude_native_result *r, int status, const char *reason,
                      struct cmp_text *ref)
{
    struct cmp_text t = {0};
    const char *end = r->attempt.end == ETUDE_END_EXITED     ? "exited"
                      : r->attempt.end == ETUDE_END_SIGNALED ? "signaled"
                      : r->attempt.end == ETUDE_END_STOPPED  ? "stopped"
                                                             : "lost";
    cmp_printf(&t, "{\"schema_version\":1,\"run_id\":");
    nc_quote(&t, h->conf->run);
    cmp_printf(&t, ",\"attempt_id\":");
    nc_quote(&t, m->attempt);
    cmp_printf(&t, ",\"item_id\":");
    nc_quote(&t, m->item);
    cmp_printf(&t, ",\"case_id\":");
    nc_quote(&t, m->case_id);
    cmp_printf(&t,
               ",\"inventory_sha256\":\"%s\",\"placement\":\"native\",\"termination\":{"
               "\"end\":\"%s\",\"value\":",
               m->assertions, end);
    if (r->attempt.value < 0)
        cmp_printf(&t, "null");
    else
        cmp_printf(&t, "%d", r->attempt.value);
    cmp_printf(&t,
               ",\"observed_by\":\"etude_guest\",\"detail\":null},\"report\":{"
               "\"state\":\"not_used\",\"records\":0,\"errors\":[]},\"branches\":[],"
               "\"assertions\":[{\"assertion_id\":\"completed\",\"source\":\"executor\","
               "\"outcome\":\"%s\",\"detail\":",
               !status ? "pass" : "fail");
    if (!status)
        cmp_printf(&t, "null");
    else
        nc_quote(&t, reason && *reason ? reason : "native case did not complete successfully");
    cmp_printf(&t, "}],\"undeclared\":[]}");
    int ok = !t.failed && nc_blob(h, ref, t.data, t.length);
    free(t.data);
    return ok;
}
struct step_context {
    struct nc_history *history;
    size_t index;
    const struct etude_native_result *result;
    const char *reason;
    const struct cmp_text *resources;
    int status;
};

static int step(const struct step_context *s, enum etude_stage stage, const char *name,
                char id[256])
{
    struct nc_history *h = s->history;
    size_t n = s->index;
    const struct etude_native_result *r = s->result;
    int status = s->status;
    const char *reason = s->reason;
    const struct cmp_text *res = s->resources;
    struct nc_measure *m = &h->items[n];
    struct cmp_text t = {0};
    cmp_printf(&t, "{\"plan_id\":");
    nc_quote(&t, h->plan);
    cmp_printf(&t, ",\"item_id\":");
    nc_quote(&t, m->item);
    cmp_printf(&t, ",\"attempt_id\":");
    nc_quote(&t, m->attempt);
    cmp_printf(&t, ",\"step_id\":\"%s\",\"stage\":\"%s\",\"time\":", name, name);
    nc_time(h, &t, &m->stage_begin[stage], &m->stage_end[stage]);
    cmp_printf(&t, ",\"resources\":%s,\"witnesses\":[],\"max_progress_gap_us\":", res->data);
    if (stage == ETUDE_STAGE_EXECUTE) {
        uint64_t gap = etude_watchdog_max_gap(m->watchdog, m->end.monotonic_us);
        cmp_printf(&t,
                   "{\"value\":%llu,\"method\":\"native-watchdog-v1\",\"quality\":\"exact\","
                   "\"interval_us\":null,\"error_bound\":%llu}",
                   (unsigned long long)gap, (unsigned long long)m->start.resolution_us);
    } else
        cmp_printf(&t, "{\"value\":null,\"reason\":\"only execution has a progress gap\"}");
    cmp_printf(&t, ",\"result\":");
    result(&t, status, r->attempt.cleanup == ETUDE_CLEANUP_CONFIRMED && (status || r->removed),
           reason, h->cases[n]->proof);
    cmp_printf(&t, "}");
    return nc_record(h, "step", &t, id);
}
int nc_history_attempt(struct nc_history *h, size_t n, const struct etude_native_result *r,
                       int status, const char *reason)
{
    static const char *names[] = {"transfer", "execute", "capture", "cleanup"};
    static enum etude_stage stages[] = {ETUDE_STAGE_TRANSFER, ETUDE_STAGE_EXECUTE,
                                        ETUDE_STAGE_CAPTURE, ETUDE_STAGE_CLEANUP};
    struct nc_measure *m = &h->items[n];
    struct cmp_text res = {0}, stage_res = {0}, refs = {0}, assertion = {0}, t = {0};
    int cleaned = r->attempt.cleanup == ETUDE_CLEANUP_CONFIRMED && (status || r->removed);
    m->status = status;
    m->cpu = r->attempt.cpu_us;
    m->memory = r->attempt.peak_bytes;
    snprintf(m->cpu_method, sizeof(m->cpu_method), "%s",
             m->cpu.method ? m->cpu.method : "CPU unavailable");
    snprintf(m->memory_method, sizeof(m->memory_method), "%s",
             m->memory.method ? m->memory.method : "peak unavailable");
    m->cpu.method = m->cpu_method;
    m->memory.method = m->memory_method;
    int ok = resources(h, &res, m, r, 1) && resources(h, &stage_res, m, r, 0) &&
             assertions(h, m, r, status, reason, &assertion);
    struct step_context context = {h, n, r, reason, &stage_res, status};
    size_t count = 0;
    cmp_printf(&refs, "[");
    for (size_t i = 0; ok && i < 4; i++)
        if (m->stage_end[stages[i]].utc[0]) {
            char id[256];
            ok = step(&context, stages[i], names[i], id);
            cmp_printf(&refs, "%s", count++ ? "," : "");
            nc_quote(&refs, id);
        }
    cmp_printf(&refs, "]");
    cmp_printf(&t, "{\"plan_id\":");
    nc_quote(&t, h->plan);
    cmp_printf(&t, ",\"item_id\":");
    nc_quote(&t, m->item);
    cmp_printf(&t, ",\"attempt_id\":");
    nc_quote(&t, m->attempt);
    cmp_printf(&t, ",\"previous_attempt_id\":null,\"steps\":%s,\"time\":", refs.data);
    nc_time(h, &t, &m->start, &m->end);
    struct etude_measurement queue = etude_history_elapsed(&h->start, &m->start);
    char queue_json[1024];
    if (etude_measurement_json(&queue, queue_json, sizeof(queue_json)))
        ok = 0;
    cmp_printf(&t, ",\"queue_us\":%s,\"resources\":%s,\"assertion_results\":%s,\"result\":",
               ok ? queue_json : "{\"value\":null,\"reason\":\"native queue clock unavailable\"}",
               res.data, assertion.data);
    result(&t, status, cleaned, reason, h->cases[n]->proof);
    cmp_printf(&t, "}");
    ok = ok && nc_record(h, "attempt", &t, m->record);
    free(t.data);
    free(res.data);
    free(stage_res.data);
    free(refs.data);
    free(assertion.data);
    h->failed |= !ok || status != 0;
    h->incomplete |= !ok || status == 3 || status == 4;
    h->unconfirmed |= !cleaned;
    return ok;
}
static void summaries(struct nc_history *h, struct cmp_text *out, struct etude_measure *processor,
                      struct etude_measure *peak)
{
    struct cmp_text t = *out;
    struct etude_measure cpu = *processor, memory = *peak;
    for (size_t n = 0; n < h->count; n++) {
        struct nc_measure *m = &h->items[n];
        if (!*m->record)
            h->failed = h->incomplete = 1;
        cmp_printf(&t, "%s{\"item_id\":", n ? "," : "");
        nc_quote(&t, m->item);
        cmp_printf(&t, ",\"attempt_records\":[");
        if (*m->record)
            nc_quote(&t, m->record);
        cmp_printf(&t, "],\"outcome\":\"%s\"}", *m->record ? outcome(m->status) : "incomplete");
        if (!*m->record || m->cpu.quality == ETUDE_QUALITY_UNAVAILABLE ||
            m->cpu.value > ETUDE_HISTORY_MAX - cpu.value)
            cpu.quality = ETUDE_QUALITY_UNAVAILABLE;
        else {
            cpu.value += m->cpu.value;
            if (cpu.quality != ETUDE_QUALITY_UNAVAILABLE && m->cpu.quality != ETUDE_QUALITY_EXACT)
                cpu.quality = m->cpu.quality;
        }
        if (!*m->record || m->memory.quality == ETUDE_QUALITY_UNAVAILABLE || h->unconfirmed)
            memory.quality = ETUDE_QUALITY_UNAVAILABLE;
        else {
            if (m->memory.value > memory.value)
                memory.value = m->memory.value;
            if (memory.quality != ETUDE_QUALITY_UNAVAILABLE &&
                m->memory.quality != ETUDE_QUALITY_EXACT)
                memory.quality = m->memory.quality;
        }
    }
    *out = t;
    *processor = cpu;
    *peak = memory;
}

int nc_history_finish(struct nc_history *h)
{
    struct cmp_text t = {0}, stages = {0}, family = {0}, concurrency = {0};
    char id[256];
    if (etude_history_now(&h->end))
        return 0;
    struct etude_measure cpu = {ETUDE_QUALITY_EXACT, 0, "native-serial-tree-sum-v1"};
    struct etude_measure memory = {ETUDE_QUALITY_EXACT, 0, "native-serial-tree-peak-v1"};
    cmp_printf(&t, "{\"plan_id\":");
    nc_quote(&t, h->plan);
    cmp_printf(&t, ",\"items\":[");
    summaries(h, &t, &cpu, &memory);
    if (cpu.quality == ETUDE_QUALITY_UNAVAILABLE)
        cpu.method = "one or more native tree CPU measurements are unavailable";
    if (memory.quality == ETUDE_QUALITY_UNAVAILABLE)
        memory.method = "one or more native peaks or cleanup scopes are unavailable";
    if (cpu.quality != ETUDE_QUALITY_UNAVAILABLE)
        cpu.quality = ETUDE_QUALITY_LOWER_BOUND;
    if (memory.quality != ETUDE_QUALITY_UNAVAILABLE)
        memory.quality = ETUDE_QUALITY_LOWER_BOUND;
    cmp_printf(&t, "],\"preparation_steps\":[],\"time\":");
    nc_time(h, &t, &h->start, h->unconfirmed ? NULL : &h->end);
    cmp_printf(&t, ",\"resources\":{\"scope\":%s,\"tree_cpu_us\":", h->selection);
    nc_metric(&t, &cpu);
    cmp_printf(&t, ",\"combined_peak_bytes\":");
    nc_metric(&t, &memory);
    int totals_ok = nc_totals(h, &stages, &family, &concurrency);
    cmp_printf(
        &t,
        ",\"samples\":[]},\"stage_totals\":%s,\"family_totals\":%s,\"useful_concurrency\":%s,"
        "\"load\":%s,\"budget\":{\"policy\":%s,\"charged_us\":",
        stages.data, family.data, concurrency.data, h->environment, h->policy);
    struct etude_measurement elapsed = etude_history_elapsed(&h->start, &h->end);
    char metric[1024];
    int ok = totals_ok && etude_measurement_json(&elapsed, metric, sizeof(metric)) == 0;
    if (h->unconfirmed)
        strcpy(metric, "{\"value\":null,\"reason\":\"native cleanup is not confirmed\"}");
    cmp_printf(
        &t,
        "%s,\"outside_queue_us\":{\"value\":null,\"reason\":\"outside queue was not measured\"},"
        "\"one_time_setup_us\":{\"value\":null,\"reason\":\"external native payload preparation "
        "was not measured\"},"
        "\"goal_us\":3600000000,\"growth_us\":3600000000,"
        "\"ceiling_us\":14400000000,\"cleanup_reserve_us\":600000000,\"verdict\":\"%s\"},"
        "\"result\":",
        ok ? metric : "{\"value\":null,\"reason\":\"native clock unavailable\"}",
        !strcmp(h->conf->purpose, "phase_first_run") ? "exempt" : "unknown");
    result(&t,
           h->unconfirmed || h->incomplete ? 4
           : h->failed                     ? 1
                                           : 0,
           !h->unconfirmed, "a native case failed or was incomplete", 0);
    cmp_printf(&t, "}");
    ok = ok && nc_record(h, "run", &t, id);
    free(t.data);
    free(stages.data);
    free(family.data);
    free(concurrency.data);
    h->terminal = ok;
    return ok;
}
