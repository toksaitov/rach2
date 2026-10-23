/* compare_run.c: `etude compare`, which compares a run with its history. */
#include "compare.h"
#include "include/etude/history.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The bytes of a stream of comparison records. */
#define CMP_STREAM_LIMIT (UINT64_C(64) << 20)

/* The policy blob each comparison record names. */
const char cmp_policy[] =
    "median-mad-v1, from dev-log/62-etude/measurements.md, \"Comparisons and noise\".\n"
    "A baseline has at least five independent eligible samples of the candidate's key.\n"
    "Each side has its median m, its median absolute deviation d, and the noise band\n"
    "ceil(max(3 * 1.4826 * d, 0.01 * m, e)), where e is the largest clock resolution or\n"
    "error bound of its samples. The verdict is within_noise when the medians differ\n"
    "by no more than the sum of the bands, and otherwise regression or improved.\n";

static const char usage_text[] =
    "usage: etude compare [-n] [-s SCHEMAS] [-b STORE] [-g REPO] [-o STREAM -i ID] RUN "
    "HISTORY...\n";

/* A group of the run's items that share a key, and their first attempts. */
struct group {
    struct cmp_key key;
    struct cmp_sample **samples;
    size_t count;
    const char *item_id; /* the first item, named when it has no attempt */
};

/* What one invocation works with. */
struct session {
    struct cmp_options options;
    struct cmp_history history;
    const struct cmp_record *plan;
    struct group *groups;
    size_t group_count;
    struct etude_history *stream;
    char policy_path[4200], policy_hex[65];
    unsigned long verdicts[5];
};

static const char *const verdict_names[5] = {"regression", "improved", "within_noise",
                                             "insufficient_history", "not_comparable"};

/** Read the options and the operands; return -1 for a wrong usage. */
static int parse(int argc, char **argv, struct cmp_options *o)
{
    int i;
    memset(o, 0, sizeof(*o));
    o->schemas = "tools/etude/schemas";
    for (i = 1; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char **value = NULL;
        switch (argv[i][1]) {
        case 'n':
            o->new_baseline = 1;
            break;
        case 's':
            value = &o->schemas;
            break;
        case 'b':
            value = &o->store;
            break;
        case 'g':
            value = &o->repo;
            break;
        case 'o':
            value = &o->output;
            break;
        case 'i':
            value = &o->stream_id;
            break;
        default:
            return -1;
        }
        if (argv[i][2] || (value && ++i == argc))
            return -1;
        if (value)
            *value = argv[i];
    }
    if (argc - i < 2 || !eh_identity(argv[i]) || !o->output != !o->stream_id ||
        (o->output && !o->store))
        return -1;
    o->run_id = argv[i];
    o->streams = argv + i + 1;
    o->stream_count = argc - i - 1;
    return 0;
}

/** Find the one plan record of the run compared. */
static int find_plan(struct session *s)
{
    size_t i, plans = 0;
    for (i = 0; i < s->history.unique; i++) {
        const struct cmp_record *r = s->history.by_id[i];
        if (!strcmp(r->kind, "plan") && !strcmp(r->run, s->options.run_id)) {
            s->plan = r;
            plans++;
        }
    }
    if (plans != 1)
        fprintf(stderr, "etude compare: run %s has %lu plan records, not one\n", s->options.run_id,
                (unsigned long)plans);
    return plans == 1 ? 0 : -1;
}

/** Return the first attempt of an item of the run, which retries none. */
static struct cmp_sample *first_attempt(const struct session *s, const char *item_id)
{
    size_t i;
    for (i = 0; i < s->history.sample_count; i++) {
        struct cmp_sample *sample = &s->history.samples[i];
        const struct eh_json *d = sample->attempt->data;
        if (!strcmp(sample->attempt->run, s->options.run_id) &&
            !strcmp(cmp_text(d, "item_id"), item_id) && !cmp_text(d, "previous_attempt_id") &&
            !strcmp(cmp_text(d, "plan_id"), s->plan->id))
            return sample;
    }
    return NULL;
}

/** Put an item in the group of its key, which the first item makes. */
static int place(struct session *s, const struct eh_json *item)
{
    struct cmp_key key;
    struct cmp_sample *sample = first_attempt(s, cmp_text(item, "item_id"));
    struct group *g = NULL;
    size_t i;
    cmp_item_key(s->plan->data, item, &key);
    for (i = 0; i < s->group_count && !g; i++)
        if (!cmp_key_differs(&key, &s->groups[i].key, 0, CMP_PLAN, NULL, 0))
            g = &s->groups[i];
    if (!g) {
        g = &s->groups[s->group_count++];
        memset(g, 0, sizeof(*g));
        g->key = key;
        g->item_id = cmp_text(item, "item_id");
        g->samples = malloc(sizeof(*g->samples));
    } else if (sample) {
        struct cmp_sample **more = realloc(g->samples, (g->count + 1) * sizeof(*more));
        if (!more)
            return -1;
        g->samples = more;
    }
    if (!g->samples)
        return -1;
    if (sample)
        g->samples[g->count++] = sample;
    return 0;
}

/** Group the plan's items by key, in the order the plan lists them. */
static int group_items(struct session *s)
{
    const struct eh_json *item, *items = eh_json_get(s->plan->data, "items");
    size_t count = 0;
    for (item = items->child; item; item = item->next)
        count++;
    s->groups = calloc(count + 1, sizeof(*s->groups));
    if (!s->groups)
        return -1;
    for (item = items->child; item; item = item->next)
        if (place(s, item))
            return -1;
    return 0;
}

/** Append a JSON string, escaped. */
static void json_string(struct cmp_text *t, const char *text)
{
    const unsigned char *c;
    cmp_printf(t, "\"");
    for (c = (const unsigned char *)text; *c; c++) {
        if (*c == '"' || *c == '\\')
            cmp_printf(t, "\\%c", *c);
        else if (*c < 0x20 || *c > 0x7e)
            cmp_printf(t, "\\u%04x", *c);
        else
            cmp_printf(t, "%c", *c);
    }
    cmp_printf(t, "\"");
}

/** Append a blob reference to a file of the store. */
static void blob(struct cmp_text *t, const char *hex, size_t size, const char *path)
{
    cmp_printf(t, "{\"sha256\":\"%s\",\"size_bytes\":%lu,\"locations\":[", hex,
               (unsigned long)size);
    json_string(t, path);
    cmp_printf(t, "]}");
}

/** Append one measurement of a side, or its absence with a reason. */
static void measurement(struct cmp_text *t, const struct cmp_stats *s, uint64_t value,
                        const char *reason)
{
    struct etude_measurement m;
    char json[2600];
    memset(&m, 0, sizeof(m));
    m.value = value;
    m.method = s->method;
    m.quality = s->quality;
    m.interval_us = s->interval_us;
    m.error_bound = s->error_us;
    m.error_known = 1;
    m.reason = reason;
    if (etude_measurement_json(&m, json, sizeof(json)))
        t->failed = 1;
    else
        cmp_printf(t, "%s", json);
}

/** Append one side's samples and statistics, or the reason it has none. */
static void side(struct cmp_text *t, struct cmp_sample *const *samples, size_t count,
                 const struct cmp_stats *s, const char *reason)
{
    size_t i;
    cmp_printf(t, "{\"samples\":[");
    for (i = 0; i < count; i++) {
        cmp_printf(t, i ? "," : "");
        json_string(t, samples[i]->attempt->id);
    }
    cmp_printf(t, "],\"median_us\":");
    measurement(t, s, s->median_us, reason);
    cmp_printf(t, ",\"mad_us\":");
    measurement(t, s, s->mad_us, reason);
    cmp_printf(t, ",\"band_us\":");
    measurement(t, s, s->band_us, reason);
    cmp_printf(t, "}");
}

/** Build the data of a comparison record whose report the store has. */
static void record_data(struct cmp_text *t, const struct session *s, const struct cmp_result *r,
                        const char *report_hex, size_t report_size, const char *report_path)
{
    int decided = r->baseline_id[0] != 0;
    cmp_printf(t, "{\"purpose\":\"same_site_regression\",\"scope\":\"case\",\"subject_id\":");
    json_string(t, r->candidates[0]->key.value[0]);
    cmp_printf(t, ",\"policy\":");
    blob(t, s->policy_hex, sizeof(cmp_policy) - 1, s->policy_path);
    cmp_printf(t, ",\"baseline_id\":");
    if (decided)
        json_string(t, r->baseline_id);
    else
        cmp_printf(t, "null");
    cmp_printf(t, ",\"baseline\":");
    side(t, r->baseline, r->baseline_count, &r->base,
         decided             ? NULL
         : r->baseline_count ? "too few eligible samples"
                             : "no eligible samples");
    cmp_printf(t, ",\"candidate\":");
    side(t, r->eligible, r->eligible_count, &r->cand,
         r->eligible_count ? NULL : "no eligible candidate");
    cmp_printf(t, ",\"equivalence_review\":null,\"verdict\":\"%s\",\"reasons\":[", r->verdict);
    json_string(t, r->reason);
    cmp_printf(t, "],\"report\":");
    blob(t, report_hex, report_size, report_path);
    cmp_printf(t, "}");
}

/** Keep the report in the store, and append its comparison record. */
static int record(struct session *s, const struct cmp_result *r, const struct cmp_text *report)
{
    struct cmp_text data = {NULL, 0, 0, 0};
    struct etude_reading now;
    char path[4200], hex[65], id[300];
    int status = -1;
    if (cmp_store(s->options.store, report->data, report->length, path, sizeof(path), hex)) {
        fprintf(stderr, "etude compare: %s: cannot keep the report\n", s->options.store);
        return -1;
    }
    record_data(&data, s, r, hex, report->length, path);
    if (!data.failed && !etude_history_now(&now) &&
        !etude_history_record_id(s->stream, id, sizeof(id)))
        status = etude_history_append(s->stream, id, now.utc, "comparison", data.data);
    if (status)
        fprintf(stderr, "etude compare: %s: cannot append the comparison of %s\n",
                s->options.output, r->candidates[0]->key.value[0]);
    free(data.data);
    return status;
}

/** Compare one group, print its report, and record it when asked. */
static int compare_group(struct session *s, const struct group *g)
{
    struct cmp_result r;
    struct cmp_text report = {NULL, 0, 0, 0};
    size_t i;
    int status;
    if (!g->count) {
        printf("comparison of %s: item %s has no first attempt in the history\n\n", g->key.value[0],
               g->item_id);
        return 0;
    }
    status = cmp_compare(&s->history, &s->options, g->samples, g->count, &r);
    if (!status)
        status = cmp_report(&s->history, &s->options, &r, &report);
    if (!status) {
        fwrite(report.data, 1, report.length, stdout);
        printf("\n");
        for (i = 0; i < 5; i++)
            s->verdicts[i] += !strcmp(r.verdict, verdict_names[i]);
        if (s->stream)
            status = record(s, &r, &report);
    }
    free(report.data);
    cmp_result_free(&r);
    return status;
}

/** Open the stream of comparisons and keep the policy in the store. */
static int open_output(struct session *s)
{
    char schema[4200];
    snprintf(schema, sizeof(schema), "%s/history.schema.json", s->options.schemas);
    if (cmp_store(s->options.store, cmp_policy, sizeof(cmp_policy) - 1, s->policy_path,
                  sizeof(s->policy_path), s->policy_hex)) {
        fprintf(stderr, "etude compare: %s: cannot keep the policy\n", s->options.store);
        return -1;
    }
    s->stream = etude_history_open(s->options.output, schema, s->options.stream_id,
                                   s->options.run_id, CMP_STREAM_LIMIT);
    if (!s->stream)
        fprintf(stderr, "etude compare: %s: cannot create the stream\n", s->options.output);
    return s->stream ? 0 : -1;
}

/** Compare every group of the run, and print the count of each verdict. */
static int compare_all(struct session *s)
{
    size_t i;
    for (i = 0; i < s->group_count; i++)
        if (compare_group(s, &s->groups[i]))
            return -1;
    printf("etude compare: result: %lu regression, %lu improved, %lu within noise, "
           "%lu insufficient history, %lu not comparable\n",
           s->verdicts[0], s->verdicts[1], s->verdicts[2], s->verdicts[3], s->verdicts[4]);
    return 0;
}

/**
 * Compare each case of a run with the history of its series, print a
 * report of each, and with -o record each comparison in a new stream whose
 * reports the store keeps. A verdict, a regression included, is not a
 * failure: the status is 0 when every comparison was made, and 2 for a wrong
 * usage, a history that cannot be read, a run without one plan record, or a
 * record that cannot be kept.
 */
int cmp_main(int argc, char **argv)
{
    struct session s;
    char schema[4200];
    int status = 2;
    size_t i;
    memset(&s, 0, sizeof(s));
    if (parse(argc, argv, &s.options)) {
        fputs(usage_text, stderr);
        return 2;
    }
    snprintf(schema, sizeof(schema), "%s/history.schema.json", s.options.schemas);
    if (!cmp_load(&s.history, schema, s.options.streams, s.options.stream_count) &&
        !find_plan(&s) && !group_items(&s) && (!s.options.output || !open_output(&s)) &&
        !compare_all(&s))
        status = 0;
    if (s.stream && etude_history_close(s.stream)) {
        fprintf(stderr, "etude compare: %s: cannot close the stream\n", s.options.output);
        status = 2;
    }
    for (i = 0; i < s.group_count; i++)
        free(s.groups[i].samples);
    free(s.groups);
    cmp_free(&s.history);
    fflush(stdout);
    return status;
}
