/* compare_report.c: the report of a comparison, for a person who looks into a
 * slow case. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "compare.h"
#include "../../utils/sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define popen  _popen
#define pclose _pclose
#define QUIET  "2>NUL"
#else
#define QUIET "2>/dev/null"
#endif

#define CMP_LISTED 200 /* the commits listed before the rest are counted */

/** Return whether a blob file has exactly the bytes its digest names. */
static int matches(const char *bytes, size_t length, const char *hex)
{
    struct sha256 sha;
    unsigned char digest[32];
    char actual[65];
    sha256_init(&sha);
    sha256_update(&sha, (const unsigned char *)bytes, length);
    sha256_final(&sha, digest);
    sha256_hex(digest, actual);
    return !strcmp(actual, hex);
}

/** Write a word for a shell, quoted when it has a character the shell reads. */
static void word(struct cmp_text *t, const char *text)
{
    const char *c;
    if (*text && strspn(text, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
                              "0123456789_./=:,+-@%") == strlen(text)) {
        cmp_printf(t, "%s", text);
        return;
    }
    cmp_printf(t, "'");
    for (c = text; *c; c++)
        cmp_printf(t, *c == '\'' ? "'\\''" : "%c", *c);
    cmp_printf(t, "'");
}

/** Write a command's arguments as a line a shell can run. */
static void command(struct cmp_text *t, const char *label, const struct eh_json *arguments)
{
    const struct eh_json *a;
    cmp_printf(t, "  %s:", label);
    for (a = arguments->child; a; a = a->next) {
        cmp_printf(t, " ");
        word(t, a->string);
    }
    cmp_printf(t, "\n");
}

/** Write what was compared, the verdict and both sides' statistics. */
static void head(struct cmp_text *t, const struct cmp_result *r)
{
    const struct cmp_key *k = &r->candidates[0]->key;
    char median[32], mad[32], band[32];
    cmp_printf(t, "comparison of %s: %s, %s, machine %s, accelerator %s\n", k->value[0],
               k->value[1], k->value[2], k->value[3], k->value[4]);
    cmp_printf(t, "verdict: %s\nreason: %s\n", r->verdict, r->reason);
    cmp_printf(t, "policy: median-mad-v1, with at least %d independent baseline samples\n",
               CMP_MINIMUM);
    if (r->baseline_id[0]) {
        cmp_seconds(r->base.median_us, median);
        cmp_seconds(r->base.mad_us, mad);
        cmp_seconds(r->base.band_us, band);
        cmp_printf(t, "baseline: %s, %s; %lu samples, median %s, deviation %s, band %s\n",
                   r->baseline_id, r->origin, (unsigned long)r->baseline_count, median, mad, band);
    } else if (r->origin[0]) {
        cmp_printf(t, "baseline: none decided; %lu eligible samples, %s\n",
                   (unsigned long)r->baseline_count, r->origin);
    }
    if (r->cand.count) {
        cmp_seconds(r->cand.median_us, median);
        cmp_seconds(r->cand.mad_us, mad);
        cmp_seconds(r->cand.band_us, band);
        cmp_printf(t, "candidate: %lu sample%s, median %s, deviation %s, band %s\n",
                   (unsigned long)r->cand.count, r->cand.count == 1 ? "" : "s", median, mad, band);
    }
}

/** Return the baseline sample that ended last. */
static const struct cmp_sample *newest(const struct cmp_result *r)
{
    const struct cmp_sample *last = r->baseline[0];
    size_t i;
    for (i = 1; i < r->baseline_count; i++)
        if (strcmp(r->baseline[i]->end, last->end) > 0)
            last = r->baseline[i];
    return last;
}

/** Count the distinct commits the baseline tested. */
static size_t baseline_commits(const struct cmp_result *r)
{
    size_t i, j, count = 0;
    for (i = 0; i < r->baseline_count; i++) {
        const char *commit = cmp_text(r->baseline[i]->plan->data, "commit");
        for (j = 0; j < i; j++)
            if (!strcmp(commit, cmp_text(r->baseline[j]->plan->data, "commit")))
                break;
        count += j == i;
    }
    return count;
}

/** Quote a path for the shell popen runs; return -1 if it cannot be. */
static int quote_path(const char *path, char *out, size_t size)
{
#ifdef _WIN32
    if (strchr(path, '"') || strchr(path, '%') || strlen(path) + 3 > size)
        return -1;
    snprintf(out, size, "\"%s\"", path);
    return 0;
#else
    size_t used = 0;
    const char *c;
    out[used++] = '\'';
    for (c = path; *c; c++) {
        if (used + 6 > size)
            return -1;
        if (*c == '\'') {
            memcpy(out + used, "'\\''", 4);
            used += 4;
        } else {
            out[used++] = *c;
        }
    }
    out[used++] = '\'';
    out[used] = 0;
    return 0;
#endif
}

/** List the commits from base to candidate with git, or say why not. */
static void list_commits(struct cmp_text *t, const char *repo, const char *base, const char *cand)
{
    char quoted[4096], run[4400], line[1024];
    FILE *pipe;
    unsigned long count = 0;
    int status;
    if (quote_path(repo, quoted, sizeof(quoted))) {
        cmp_printf(t, "commits in between: unknown, since the path %s cannot be quoted\n", repo);
        return;
    }
    snprintf(run, sizeof(run), "git -C %s log --reverse --format=\"%%H %%s\" %s..%s %s", quoted,
             base, cand, QUIET);
    fflush(NULL);
    pipe = popen(run, "r");
    cmp_printf(t, "commits in between, by git log %s..%s in %s:\n", base, cand, repo);
    while (pipe && fgets(line, sizeof(line), pipe)) {
        if (++count <= CMP_LISTED)
            cmp_printf(t, "  %.*s\n", (int)strcspn(line, "\r\n"), line);
    }
    status = pipe ? pclose(pipe) : -1;
    if (count > CMP_LISTED)
        cmp_printf(t, "  and %lu more\n", count - CMP_LISTED);
    if (status)
        cmp_printf(t, "  none listed, since git log failed with status %d\n", status);
    else if (!count)
        cmp_printf(t, "  none\n");
}

/** Write the commits the two sides tested and those in between. */
static void commits(struct cmp_text *t, const struct cmp_options *o, const struct cmp_result *r)
{
    const char *cand = cmp_text(r->eligible[0]->plan->data, "commit");
    const char *base;
    if (!r->baseline_count) {
        cmp_printf(t, "commits: the candidate tested %s\n", cand);
        return;
    }
    base = cmp_text(newest(r)->plan->data, "commit");
    cmp_printf(
        t, "commits: the baseline tested %lu commit%s, the newest %s; the candidate tested %s\n",
        (unsigned long)baseline_commits(r), baseline_commits(r) == 1 ? "" : "s", base, cand);
    if (!strcmp(base, cand))
        cmp_printf(t, "commits in between: none, since both tested %s\n", base);
    else if (o->repo)
        list_commits(t, o->repo, base, cand);
    else
        cmp_printf(t, "commits in between: git log --reverse --format=\"%%H %%s\" %s..%s\n", base,
                   cand);
}

/**
 * Find the one invocation of a step in an attempt and take its time; return
 * 0 and leave the reason in why when it ran more than once or is not eligible.
 */
static int step_time(const struct cmp_history *h, const struct cmp_record *attempt,
                     const char *step_id, uint64_t *elapsed, uint64_t *error, char *why)
{
    const struct eh_json *id, *found = NULL, *value, *bound;
    int count = 0;
    for (id = eh_json_get(attempt->data, "steps")->child; id; id = id->next) {
        const struct cmp_record *s = cmp_find(h, id->string);
        if (s && !strcmp(s->kind, "step") && !strcmp(cmp_text(s->data, "step_id"), step_id)) {
            found = s->data;
            count++;
        }
    }
    if (count != 1) {
        sprintf(why, "it ran %d times in attempt %s", count, attempt->id);
        return 0;
    }
    value = eh_json_get(eh_json_get(eh_json_get(found, "time"), "elapsed_us"), "value");
    bound = eh_json_get(eh_json_get(eh_json_get(found, "time"), "elapsed_us"), "error_bound");
    if (strcmp(cmp_text(eh_json_get(found, "result"), "outcome"), "pass") ||
        eh_json_get(eh_json_get(found, "result"), "disruptions")->child || value->type != 'n' ||
        !bound || bound->type != 'n') {
        sprintf(why, "its record in attempt %s is not a passing, timed one", attempt->id);
        return 0;
    }
    *elapsed = (uint64_t)value->number;
    if ((uint64_t)bound->number > *error)
        *error = (uint64_t)bound->number;
    return 1;
}

/** Take one step's statistics over attempts; return -1 if one lacks it. */
static int step_stats(const struct cmp_history *h, struct cmp_sample *const *samples, size_t count,
                      const char *step_id, struct cmp_stats *stats, char *why)
{
    uint64_t *values = malloc((count + 1) * sizeof(*values));
    size_t i;
    int status = -1;
    memset(stats, 0, sizeof(*stats));
    if (!values) {
        strcpy(why, "out of memory");
        return -1;
    }
    for (i = 0; i < count; i++)
        if (!step_time(h, samples[i]->attempt, step_id, &values[i], &stats->error_us, why))
            break;
    if (i == count)
        status = cmp_statistics(values, count, stats);
    free(values);
    return status;
}

/** Write each step of the candidate, compared as its case is. */
static void steps(struct cmp_text *t, const struct cmp_history *h, const struct cmp_result *r)
{
    const struct eh_json *id;
    cmp_printf(t, "steps:\n");
    for (id = eh_json_get(r->eligible[0]->attempt->data, "steps")->child; id; id = id->next) {
        const struct cmp_record *s = cmp_find(h, id->string);
        const char *step_id = s ? cmp_text(s->data, "step_id") : id->string;
        struct cmp_stats base, cand;
        char why[400], b[32], c[32], band[32];
        cmp_printf(t, "  %s (%s): ", step_id, s ? cmp_text(s->data, "stage") : "no record");
        if (!s || step_stats(h, r->eligible, r->eligible_count, step_id, &cand, why)) {
            cmp_printf(t, "not compared, since in the candidate %s\n",
                       s ? why : "the step has no record");
            continue;
        }
        cmp_seconds(cand.median_us, c);
        if (r->baseline_count < CMP_MINIMUM ||
            step_stats(h, r->baseline, r->baseline_count, step_id, &base, why)) {
            cmp_printf(t, "candidate %s; not compared, since %s\n", c,
                       r->baseline_count < CMP_MINIMUM ? "the baseline is too small" : why);
            continue;
        }
        cmp_seconds(base.median_us, b);
        cmp_seconds(base.band_us + cand.band_us, band);
        cmp_printf(t, "baseline %s, candidate %s, combined band %s: %s\n", b, c, band,
                   cmp_verdict(&base, &cand));
    }
}

/** Write the median of one measurement of attempts, or why it is missing. */
static void usage(struct cmp_text *t, struct cmp_sample *const *samples, size_t count,
                  const char *object, const char *member)
{
    uint64_t *values = malloc((count + 1) * sizeof(*values));
    size_t i, known = 0;
    const char *missing = NULL, *bound = "";
    struct cmp_stats stats;
    char median[32];
    for (i = 0; values && i < count; i++) {
        const struct eh_json *data = samples[i]->attempt->data;
        const struct eh_json *m = eh_json_get(object ? eh_json_get(data, object) : data, member);
        const struct eh_json *value = eh_json_get(m, "value");
        if (value->type == 'n') {
            values[known++] = (uint64_t)value->number;
            if (!strcmp(cmp_text(m, "quality"), "lower_bound"))
                bound = ", a lower bound";
        } else if (!missing) {
            missing = cmp_text(m, "reason");
        }
    }
    memset(&stats, 0, sizeof(stats));
    if (known && !cmp_statistics(values, known, &stats)) {
        cmp_seconds(stats.median_us, median);
        cmp_printf(t, "median %s of %lu%s", median, (unsigned long)known, bound);
    }
    if (missing)
        cmp_printf(t, "%sunavailable in %lu: %s", known ? "; " : "", (unsigned long)(count - known),
                   missing);
    free(values);
}

/** Write both sides' processor time and queueing, apart from elapsed time. */
static void costs(struct cmp_text *t, const struct cmp_result *r)
{
    static const char *const objects[] = {"resources", NULL};
    static const char *const members[] = {"tree_cpu_us", "queue_us"};
    static const char *const labels[] = {"processor time of the tree", "time queued"};
    size_t i;
    for (i = 0; i < 2; i++) {
        cmp_printf(t, "%s: baseline ", labels[i]);
        if (r->baseline_count)
            usage(t, r->baseline, r->baseline_count, objects[i], members[i]);
        else
            cmp_printf(t, "none");
        cmp_printf(t, "; candidate ");
        usage(t, r->eligible, r->eligible_count, objects[i], members[i]);
        cmp_printf(t, "\n");
    }
}

/** Write one sample's line: its record, commit, end and time. */
static void sample_line(struct cmp_text *t, const char *role, const struct cmp_sample *s)
{
    char elapsed[32];
    cmp_seconds(s->elapsed_us, elapsed);
    cmp_printf(t, "  %s %s: commit %s, ended %sZ, %s\n", role, s->attempt->id,
               cmp_text(s->plan->data, "commit"), s->end, elapsed);
}

/** Write every sample compared, and every one left out with its reason. */
static void samples(struct cmp_text *t, const struct cmp_result *r)
{
    size_t i;
    cmp_printf(t, "samples:\n");
    for (i = 0; i < r->baseline_count; i++)
        sample_line(t, "baseline", r->baseline[i]);
    for (i = 0; i < r->eligible_count; i++)
        sample_line(t, "candidate", r->eligible[i]);
    for (i = 0; i < r->excluded_count; i++)
        cmp_printf(t, "  left out%s %s: %s\n", r->excluded[i].candidate ? ", a candidate," : "",
                   r->excluded[i].sample->attempt->id, r->excluded[i].reason);
    if (r->earlier)
        cmp_printf(t, "  %lu earlier samples of the series precede this baseline\n",
                   (unsigned long)r->earlier);
}

/** Write the candidate's source, runner and artifacts by digest. */
static void artifacts(struct cmp_text *t, const struct cmp_sample *s)
{
    const struct eh_json *plan = s->plan->data, *a;
    cmp_printf(t, "candidate inputs: source %s, runner %s\n", cmp_digest(plan, "source"),
               cmp_digest(plan, "runner"));
    for (a = eh_json_get(plan, "artifacts")->child; a; a = a->next)
        cmp_printf(t, "  artifact %s: %s\n", cmp_text(a, "sha256"),
                   eh_json_get(a, "locations")->child->string);
}

/** Read and check the reproduce blob of the store; return NULL and why. */
static struct eh_json *reproduce_blob(const struct cmp_options *o, const struct eh_json *blob,
                                      const char **why)
{
    char path[4200], *bytes = NULL, *schema_text = NULL;
    size_t length, schema_length;
    struct eh_json *document = NULL, *schema = NULL;
    const char *hex = cmp_text(blob, "sha256");
    snprintf(path, sizeof(path), "%s/%s", o->store, hex);
    *why = "the store does not have it";
    if (cmp_read_file(path, 4u << 20, &bytes, &length))
        return NULL;
    *why = "the store's copy has other bytes";
    if ((int64_t)length == eh_json_get(blob, "size_bytes")->number && matches(bytes, length, hex)) {
        snprintf(path, sizeof(path), "%s/reproduce.schema.json", o->schemas);
        *why = "it is not a version-one reproduce document";
        document = eh_json_parse(bytes);
        if (!cmp_read_file(path, 1u << 20, &schema_text, &schema_length))
            schema = eh_json_parse(schema_text);
        if (document && (!schema || !eh_json_validate(document, schema))) {
            eh_json_free(document);
            document = NULL;
        }
    }
    eh_json_free(schema);
    free(schema_text);
    free(bytes);
    return document;
}

/** Write the commands that run the candidate again, and under a profiler. */
static void reproduce(struct cmp_text *t, const struct cmp_options *o, const struct cmp_sample *s)
{
    const struct eh_json *blob = eh_json_get(s->plan->data, "reproduce"), *item, *member;
    const char *why = "no store was given";
    struct eh_json *document = o->store ? reproduce_blob(o, blob, &why) : NULL;
    const char *item_id = cmp_text(s->item, "item_id");
    cmp_printf(t, "reproduce: blob %s, at %s\n", cmp_text(blob, "sha256"),
               eh_json_get(blob, "locations")->child->string);
    item = NULL;
    for (member = document ? eh_json_get(document, "items")->child : NULL; member;
         member = member->next)
        if (!strcmp(cmp_text(member, "item_id"), item_id))
            item = member;
    if (!item) {
        cmp_printf(t, "  no commands, since %s\n", document ? "the blob omits the item" : why);
        eh_json_free(document);
        return;
    }
    cmp_printf(t, "  directory: %s\n", cmp_text(item, "directory"));
    for (member = eh_json_get(item, "environment")->child; member; member = member->next)
        cmp_printf(t, "  environment: %s=%s\n", cmp_text(member, "name"),
                   cmp_text(member, "value"));
    for (member = eh_json_get(item, "preparation")->child; member; member = member->next)
        command(t, "prepare", member);
    command(t, "case", eh_json_get(item, "case"));
    member = eh_json_get(item, "profile");
    if (member->type == 'a')
        command(t, "profile", member);
    else
        cmp_printf(t, "  profile: none, since %s\n", cmp_text(member, "reason"));
    eh_json_free(document);
}

/**
 * Write the report of one comparison: the verdict and its reason, both
 * sides' statistics, the commits in between, each step, the processor time
 * and queueing apart from the elapsed time, every sample with what left it
 * out, the candidate's inputs, the commands that run it again and under a
 * profiler, and what the history could not give. Return -1 when memory ran out.
 */
int cmp_report(const struct cmp_history *h, const struct cmp_options *o, const struct cmp_result *r,
               struct cmp_text *t)
{
    size_t i;
    head(t, r);
    if (r->eligible_count) {
        commits(t, o, r);
        steps(t, h, r);
        costs(t, r);
    }
    samples(t, r);
    if (r->eligible_count) {
        artifacts(t, r->eligible[0]);
        reproduce(t, o, r->eligible[0]);
    }
    for (i = 0; i < h->note_count; i++)
        cmp_printf(t, "history: %s\n", h->notes[i]);
    return t->failed ? -1 : 0;
}
