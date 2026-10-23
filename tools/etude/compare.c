/* compare.c: history samples, statistics, text buffers and stored blobs. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "compare.h"
#include "include/etude/history.h"
#include "../../utils/sha256.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

/** Append formatted text, growing the buffer. */
void cmp_printf(struct cmp_text *t, const char *format, ...)
{
    va_list arguments;
    int length;
    va_start(arguments, format);
    length = vsnprintf(NULL, 0, format, arguments);
    va_end(arguments);
    if (t->failed || length < 0) {
        t->failed = 1;
        return;
    }
    if (t->length + (size_t)length + 1 > t->capacity) {
        size_t capacity = 2 * (t->length + (size_t)length + 1);
        char *data = realloc(t->data, capacity);
        if (!data) {
            t->failed = 1;
            return;
        }
        t->data = data;
        t->capacity = capacity;
    }
    va_start(arguments, format);
    vsnprintf(t->data + t->length, t->capacity - t->length, format, arguments);
    va_end(arguments);
    t->length += (size_t)length;
}

/** Read a whole file of at most limit bytes; return -1 if it cannot be. */
int cmp_read_file(const char *path, size_t limit, char **bytes, size_t *length)
{
    FILE *file = fopen(path, "rb");
    char *data;
    size_t got;
    *bytes = NULL;
    if (!file)
        return -1;
    data = malloc(limit + 1);
    got = data ? fread(data, 1, limit + 1, file) : 0;
    if (!data || ferror(file) || got > limit) {
        fclose(file);
        free(data);
        return -1;
    }
    fclose(file);
    data[got] = 0;
    *bytes = data;
    *length = got;
    return 0;
}

/** Say whether the file `path` has exactly the `length` bytes `bytes`. */
static int holds(const char *path, const char *bytes, size_t length)
{
    char *old;
    size_t old_length;
    int same;
    if (cmp_read_file(path, length, &old, &old_length))
        return 0;
    same = old_length == length && !memcmp(old, bytes, length);
    free(old);
    return same;
}

/**
 * Keep bytes in the store as a file named by their SHA-256, unless the store
 * has them already; give the file's path and the digest. The bytes go to a
 * partial file of this process's, which is synchronized and renamed to the
 * name, so that a write cut short, as on a full disk, leaves nothing under
 * the name, and a file there with other bytes is replaced. Return -1 when the
 * file cannot be written.
 */
int cmp_store(const char *store, const char *bytes, size_t length, char *path, size_t size,
              char hex[65])
{
    static unsigned long made;
    struct sha256 sha;
    unsigned char digest[32];
    char part[4200];
    FILE *file;
    int ok;
    sha256_init(&sha);
    sha256_update(&sha, (const unsigned char *)bytes, length);
    sha256_final(&sha, digest);
    sha256_hex(digest, hex);
    if ((size_t)snprintf(path, size, "%s/%s", store, hex) >= size ||
        (size_t)snprintf(part, sizeof(part), "%s.%ld.%lu.part", path, (long)getpid(), made++) >=
            sizeof(part))
        return -1;
    if (holds(path, bytes, length))
        return 0;
    file = fopen(part, "wb");
    if (!file)
        return -1;
    ok = fwrite(bytes, 1, length, file) == length && fflush(file) == 0;
#ifndef _WIN32
    ok = ok && fsync(fileno(file)) == 0;
#endif
    ok = fclose(file) == 0 && ok;
    /* The C runtime of Windows renames to no name that exists. */
    if (ok && rename(part, path) != 0)
        ok = remove(path) == 0 && rename(part, path) == 0;
    if (!ok)
        remove(part);
    return ok || holds(path, bytes, length) ? 0 : -1;
}

const char *const cmp_field_names[CMP_FIELDS] = {"case",
                                                 "architecture",
                                                 "domain",
                                                 "machine",
                                                 "accelerator",
                                                 "workload",
                                                 "assertion inventory",
                                                 "profile",
                                                 "operating system",
                                                 "operating system version",
                                                 "site profile",
                                                 "tools",
                                                 "cache state",
                                                 "declared load",
                                                 "concurrency policy",
                                                 "timing method"};

const char *cmp_program = "etude compare";

struct load_state {
    struct cmp_history *history;
    size_t order;
    int failed;
};

/** Return a member's string, or NULL when it is absent or not a string. */
const char *cmp_text(const struct eh_json *object, const char *name)
{
    const struct eh_json *member = eh_json_get(object, name);
    return member && member->type == 's' ? member->string : NULL;
}

/** Return the SHA-256 of a member that is a blob reference. */
const char *cmp_digest(const struct eh_json *object, const char *name)
{
    return cmp_text(eh_json_get(object, name), "sha256");
}

/** Write a history UTC time with six digits of fraction, so that it sorts. */
void cmp_utc(const char *text, char out[CMP_UTC])
{
    size_t i, length = text ? strlen(text) : 0;
    if (length < 20) {
        out[0] = 0;
        return;
    }
    memcpy(out, text, 19);
    out[19] = '.';
    for (i = 0; i < 6; i++)
        out[20 + i] = text[19] == '.' && 20 + i < length - 1 ? text[20 + i] : '0';
    out[26] = 0;
}

/** Write microseconds as seconds with six decimals. */
void cmp_seconds(uint64_t us, char out[32])
{
    snprintf(out, 32, "%llu.%06llu s", (unsigned long long)(us / 1000000),
             (unsigned long long)(us % 1000000));
}

/** Keep a note on what the history could not give. */
static int note(struct cmp_history *h, const char *format, ...)
{
    char text[1200], **notes;
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);
    notes = realloc(h->notes, (h->note_count + 1) * sizeof(*notes));
    if (!notes)
        return -1;
    h->notes = notes;
    notes[h->note_count] = malloc(strlen(text) + 1);
    if (!notes[h->note_count])
        return -1;
    strcpy(notes[h->note_count++], text);
    return 0;
}

/** Take one validated line of a stream; return nonzero to stop reading. */
static int add_record(const char *line, void *context)
{
    struct load_state *state = context;
    struct cmp_history *h = state->history;
    struct cmp_record *r;
    struct sha256 sha;
    if (h->count == h->capacity) {
        size_t capacity = h->capacity ? 2 * h->capacity : 256;
        struct cmp_record *records = realloc(h->records, capacity * sizeof(*records));
        if (!records)
            return state->failed = 1;
        h->records = records;
        h->capacity = capacity;
    }
    r = &h->records[h->count];
    memset(r, 0, sizeof(*r));
    r->json = eh_json_parse(line);
    if (!r->json)
        return state->failed = 1;
    r->id = cmp_text(r->json, "record_id");
    r->run = cmp_text(r->json, "run_id");
    r->kind = cmp_text(r->json, "kind");
    r->data = eh_json_get(r->json, "data");
    cmp_utc(cmp_text(r->json, "recorded_utc"), r->utc);
    r->length = strlen(line);
    sha256_init(&sha);
    sha256_update(&sha, (const unsigned char *)line, r->length);
    sha256_final(&sha, r->digest);
    r->order = state->order++;
    h->count++;
    return 0;
}

/** Read one stream; an invalid one is noted and none of its records kept. */
static int load_stream(struct cmp_history *h, const char *schema, const char *path,
                       struct load_state *state)
{
    size_t before = h->count;
    uint64_t line = 0;
    int status;
    FILE *file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "%s: %s: cannot read\n", cmp_program, path);
        return -1;
    }
    fclose(file);
    status = etude_history_read(path, schema, add_record, state, &line);
    if (state->failed) {
        fprintf(stderr, "%s: %s: out of memory\n", cmp_program, path);
        return -1;
    }
    if (status < 0) {
        while (h->count > before)
            eh_json_free(h->records[--h->count].json);
        return note(h, "%s is invalid at line %llu, and none of its records is used", path,
                    (unsigned long long)line);
    }
    if (status == 1)
        return note(h, "%s ends in an incomplete line %llu, which is not used", path,
                    (unsigned long long)line);
    return 0;
}

static int by_id(const void *a, const void *b)
{
    const struct cmp_record *x = *(struct cmp_record *const *)a;
    const struct cmp_record *y = *(struct cmp_record *const *)b;
    int order = strcmp(x->id, y->id);
    return order ? order : (x->order > y->order) - (x->order < y->order);
}

static int by_run(const void *a, const void *b)
{
    const struct cmp_record *x = *(struct cmp_record *const *)a;
    const struct cmp_record *y = *(struct cmp_record *const *)b;
    int order = strcmp(x->run, y->run);
    return order ? order : (x->order > y->order) - (x->order < y->order);
}

static int by_time(const void *a, const void *b)
{
    const struct cmp_record *x = *(struct cmp_record *const *)a;
    const struct cmp_record *y = *(struct cmp_record *const *)b;
    int order = strcmp(x->utc, y->utc);
    return order ? order : (x->order > y->order) - (x->order < y->order);
}

static int by_case(const void *a, const void *b)
{
    const struct cmp_sample *x = *(struct cmp_sample *const *)a;
    const struct cmp_sample *y = *(struct cmp_sample *const *)b;
    int order = strcmp(x->key.value[0], y->key.value[0]);
    if (order)
        return order;
    order = strcmp(x->end, y->end);
    return order
               ? order
               : (x->attempt->order > y->attempt->order) - (x->attempt->order < y->attempt->order);
}

/**
 * Sort the records by identity, keeping one of each exact replay that more
 * than one stream gives; return -1 when one identity has two contents.
 */
static int index_records(struct cmp_history *h)
{
    size_t i, kept = 0;
    h->by_id = malloc((h->count + 1) * sizeof(*h->by_id));
    if (!h->by_id)
        return -1;
    for (i = 0; i < h->count; i++)
        h->by_id[i] = &h->records[i];
    qsort(h->by_id, h->count, sizeof(*h->by_id), by_id);
    for (i = 0; i < h->count; i++) {
        struct cmp_record *r = h->by_id[i], *last = kept ? h->by_id[kept - 1] : NULL;
        if (last && !strcmp(last->id, r->id)) {
            if (last->length != r->length || memcmp(last->digest, r->digest, 32)) {
                fprintf(stderr, "%s: record %s has two different contents\n", cmp_program, r->id);
                return -1;
            }
            continue;
        }
        h->by_id[kept++] = r;
    }
    h->unique = kept;
    return 0;
}

/** List the run and comparison records of the history. */
static int index_kinds(struct cmp_history *h)
{
    size_t i;
    h->by_run = malloc((h->unique + 1) * sizeof(*h->by_run));
    h->comparisons = malloc((h->unique + 1) * sizeof(*h->comparisons));
    if (!h->by_run || !h->comparisons)
        return -1;
    for (i = 0; i < h->unique; i++) {
        struct cmp_record *r = h->by_id[i];
        if (!strcmp(r->kind, "run"))
            h->by_run[h->runs++] = r;
        else if (!strcmp(r->kind, "comparison"))
            h->comparisons[h->comparison_count++] = r;
    }
    qsort(h->by_run, h->runs, sizeof(*h->by_run), by_run);
    qsort(h->comparisons, h->comparison_count, sizeof(*h->comparisons), by_time);
    return 0;
}

static void sample_of(const struct cmp_history *h, const struct cmp_record *attempt,
                      struct cmp_sample *s);

/** Make a sample of each attempt, and list those with a case by case. */
static int index_samples(struct cmp_history *h)
{
    size_t i, attempts = 0;
    for (i = 0; i < h->unique; i++)
        attempts += !strcmp(h->by_id[i]->kind, "attempt");
    h->samples = calloc(attempts + 1, sizeof(*h->samples));
    h->by_case = malloc((attempts + 1) * sizeof(*h->by_case));
    if (!h->samples || !h->by_case)
        return -1;
    for (i = 0; i < h->unique; i++) {
        struct cmp_record *r = h->by_id[i];
        if (strcmp(r->kind, "attempt"))
            continue;
        r->sample = &h->samples[h->sample_count++];
        sample_of(h, r, r->sample);
        if (r->sample->key.value[0])
            h->by_case[h->cased++] = r->sample;
    }
    qsort(h->by_case, h->cased, sizeof(*h->by_case), by_case);
    return 0;
}

/**
 * Read every stream and index its records.
 *
 * An invalid stream, or the incomplete last line of one, becomes a note,
 * and the comparison goes on with the records that the other streams give.
 * Return -1 for a schema or a stream that cannot be read, a record whose
 * identity has two contents, or a lack of memory.
 */
int cmp_load(struct cmp_history *h, const char *schema, char **paths, int count)
{
    struct load_state state;
    FILE *file = fopen(schema, "rb");
    int i;
    memset(h, 0, sizeof(*h));
    if (!file) {
        fprintf(stderr, "%s: %s: cannot read the schema\n", cmp_program, schema);
        return -1;
    }
    fclose(file);
    state.history = h;
    state.order = 0;
    state.failed = 0;
    for (i = 0; i < count; i++)
        if (load_stream(h, schema, paths[i], &state))
            return -1;
    if (index_records(h) || index_kinds(h) || index_samples(h))
        return -1;
    return 0;
}

/** Free the records, their indexes and the notes. */
void cmp_free(struct cmp_history *h)
{
    size_t i;
    for (i = 0; i < h->count; i++)
        eh_json_free(h->records[i].json);
    for (i = 0; i < h->note_count; i++)
        free(h->notes[i]);
    free(h->records);
    free(h->by_id);
    free(h->by_run);
    free(h->comparisons);
    free(h->samples);
    free(h->by_case);
    free(h->notes);
    memset(h, 0, sizeof(*h));
}

/** Return the record with this identity, or NULL. */
const struct cmp_record *cmp_find(const struct cmp_history *h, const char *id)
{
    size_t low = 0, high = h->unique;
    while (id && low < high) {
        size_t middle = low + (high - low) / 2;
        int order = strcmp(id, h->by_id[middle]->id);
        if (!order)
            return h->by_id[middle];
        if (order < 0)
            high = middle;
        else
            low = middle + 1;
    }
    return NULL;
}

/** Return the first run record of a run, or NULL. */
static const struct cmp_record *run_record(const struct cmp_history *h, const char *run)
{
    size_t low = 0, high = h->runs;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (strcmp(run, h->by_run[middle]->run) <= 0)
            high = middle;
        else
            low = middle + 1;
    }
    return low < h->runs && !strcmp(h->by_run[low]->run, run) ? h->by_run[low] : NULL;
}

/** Return whether an array of strings has this one. */
static int listed(const struct eh_json *array, const char *text)
{
    const struct eh_json *member;
    for (member = array ? array->child : NULL; text && member; member = member->next)
        if (member->type == 's' && !strcmp(member->string, text))
            return 1;
    return 0;
}

/** Return the member of an array of objects whose field has this value. */
static const struct eh_json *member_with(const struct eh_json *array, const char *field,
                                         const char *value)
{
    const struct eh_json *member;
    for (member = array ? array->child : NULL; value && member; member = member->next) {
        const char *text = cmp_text(member, field);
        if (text && !strcmp(text, value))
            return member;
    }
    return NULL;
}

/** Give the first reason a sample is not eligible; later ones are not kept. */
static void exclude(struct cmp_sample *s, const char *format, ...)
{
    va_list arguments;
    if (s->reason[0])
        return;
    va_start(arguments, format);
    vsnprintf(s->reason, sizeof(s->reason), format, arguments);
    va_end(arguments);
}

/** Fill the values a plan and its item give to the key. */
void cmp_item_key(const struct eh_json *plan, const struct eh_json *item, struct cmp_key *k)
{
    const struct eh_json *environment = eh_json_get(plan, "environment");
    k->value[0] = cmp_text(item, "case_id");
    k->value[1] = cmp_text(item, "architecture");
    k->value[2] = cmp_text(item, "domain");
    k->value[3] = cmp_text(environment, "machine_id");
    k->value[4] = cmp_text(environment, "accelerator");
    k->value[5] = cmp_digest(item, "workload");
    k->value[6] = cmp_digest(item, "assertions");
    k->value[7] = cmp_digest(item, "profile");
    k->value[8] = cmp_text(environment, "os");
    k->value[9] = cmp_text(environment, "os_version");
    k->value[10] = cmp_digest(environment, "site_profile");
    k->value[11] = cmp_digest(environment, "tools");
    k->value[12] = cmp_digest(environment, "cache_state");
    k->value[13] = cmp_digest(environment, "load");
    k->value[14] = cmp_digest(plan, "concurrency");
}

/** Exclude a sample whose result is not a pass, or was disrupted. */
static void check_result(struct cmp_sample *s, const struct eh_json *result, const char *whose)
{
    const char *outcome = cmp_text(result, "outcome");
    const struct eh_json *disruptions = eh_json_get(result, "disruptions");
    if (outcome && strcmp(outcome, "pass"))
        exclude(s, "%s ended in %s: %s", whose, outcome, cmp_text(result, "reason"));
    if (disruptions && disruptions->child)
        exclude(s, "%s was disrupted: %s", whose, disruptions->child->string);
}

/** Take the elapsed time, its error and its method, or exclude the sample. */
static void check_time(struct cmp_sample *s, const struct eh_json *time)
{
    const struct eh_json *elapsed = eh_json_get(time, "elapsed_us");
    const struct eh_json *value = eh_json_get(elapsed, "value");
    const struct eh_json *error = eh_json_get(elapsed, "error_bound");
    const struct eh_json *interval = eh_json_get(elapsed, "interval_us");
    const struct eh_json *resolution = eh_json_get(eh_json_get(time, "clock"), "resolution_us");
    cmp_utc(cmp_text(time, "start_utc"), s->start);
    cmp_utc(cmp_text(time, "end_utc"), s->end);
    if (!value || value->type != 'n') {
        exclude(s, "its elapsed time is unavailable: %s", cmp_text(elapsed, "reason"));
        return;
    }
    s->key.value[CMP_PLAN] = cmp_text(elapsed, "method");
    s->quality = cmp_text(elapsed, "quality");
    s->elapsed_us = (uint64_t)value->number;
    s->interval_us = interval && interval->type == 'n' ? (uint64_t)interval->number : 0;
    s->error_us = resolution ? (uint64_t)resolution->number : 1;
    if (!strcmp(s->quality, "lower_bound"))
        exclude(s, "its elapsed time is only a lower bound");
    if (!error || error->type != 'n')
        exclude(s, "its elapsed time has no error bound");
    else if ((uint64_t)error->number > s->error_us)
        s->error_us = (uint64_t)error->number;
}

/** Exclude a sample whose run has no record, was disrupted or omits it. */
static void check_run(const struct cmp_history *h, struct cmp_sample *s)
{
    const struct eh_json *item;
    s->run = run_record(h, s->attempt->run);
    if (!s->run) {
        exclude(s, "its run has no run record, so the load over the run is unknown");
        return;
    }
    if (strcmp(cmp_text(s->run->data, "plan_id"), s->plan->id))
        exclude(s, "its run record is of another plan, %s", cmp_text(s->run->data, "plan_id"));
    check_result(s, eh_json_get(s->run->data, "result"), "its run");
    item = member_with(eh_json_get(s->run->data, "items"), "item_id",
                       cmp_text(s->attempt->data, "item_id"));
    if (!item || !listed(eh_json_get(item, "attempt_records"), s->attempt->id))
        exclude(s, "its run's summary does not list it");
}

/**
 * Make a sample of an attempt record, and give the first reason it cannot
 * be one: a plan that does not list it, a result other than a pass, a
 * disruption, a retry, an elapsed time that is unavailable, a lower bound or
 * has no error bound, or a run without a record, disrupted, or whose summary
 * leaves the attempt out. The key is complete when the plan lists the attempt.
 */
static void sample_of(const struct cmp_history *h, const struct cmp_record *attempt,
                      struct cmp_sample *s)
{
    const struct eh_json *d = attempt->data;
    memset(s, 0, sizeof(*s));
    s->attempt = attempt;
    s->plan = cmp_find(h, cmp_text(d, "plan_id"));
    if (!s->plan || strcmp(s->plan->kind, "plan") || strcmp(s->plan->run, attempt->run)) {
        s->plan = NULL;
        exclude(s, "its plan record %s is not in the history", cmp_text(d, "plan_id"));
        cmp_utc(cmp_text(eh_json_get(d, "time"), "end_utc"), s->end);
        return;
    }
    s->item = member_with(eh_json_get(s->plan->data, "items"), "item_id", cmp_text(d, "item_id"));
    if (!s->item || !listed(eh_json_get(s->item, "attempt_ids"), cmp_text(d, "attempt_id"))) {
        s->item = NULL;
        exclude(s, "its plan does not list attempt %s", cmp_text(d, "attempt_id"));
        cmp_utc(cmp_text(eh_json_get(d, "time"), "end_utc"), s->end);
        return;
    }
    cmp_item_key(s->plan->data, s->item, &s->key);
    check_result(s, eh_json_get(d, "result"), "it");
    if (cmp_text(d, "previous_attempt_id"))
        exclude(s, "it retries attempt %s, so it is not an independent sample",
                cmp_text(d, "previous_attempt_id"));
    check_time(s, eh_json_get(d, "time"));
    check_run(h, s);
}

/**
 * Count the fields in [from, to) where two keys differ, and list their names
 * in names, separated by commas.
 */
int cmp_key_differs(const struct cmp_key *a, const struct cmp_key *b, size_t from, size_t to,
                    char *names, size_t size)
{
    size_t i, used = 0;
    int count = 0;
    if (size)
        names[0] = 0;
    for (i = from; i < to; i++) {
        const char *x = a->value[i], *y = b->value[i];
        if (x == y || (x && y && !strcmp(x, y)))
            continue;
        if (used < size)
            used += (size_t)snprintf(names + used, size - used, "%s%s", count ? ", " : "",
                                     cmp_field_names[i]);
        count++;
    }
    return count;
}

static int ascending(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/** Return the median of sorted values, which are all even, exactly. */
static uint64_t middle(const uint64_t *sorted, size_t count)
{
    return count % 2 ? sorted[count / 2] : (sorted[count / 2 - 1] + sorted[count / 2]) / 2;
}

/**
 * Take the median m, the median absolute deviation d and the noise band
 * ceil(max(3 * 1.4826 * d, 0.01 * m, e)) of durations, where stats->error_us
 * is e on entry. The arithmetic is in quarters of a microsecond, where both
 * medians of an even count are exact; the stored median and deviation are
 * rounded upward from them. Return -1 for no values or no memory.
 */
int cmp_statistics(const uint64_t *values, size_t count, struct cmp_stats *s)
{
    uint64_t *q, mad4, spread, percent;
    size_t i;
    s->count = count;
    if (!count || !(q = malloc(count * sizeof(*q))))
        return -1;
    for (i = 0; i < count; i++)
        q[i] = 4 * values[i];
    qsort(q, count, sizeof(*q), ascending);
    s->median4 = middle(q, count);
    for (i = 0; i < count; i++)
        q[i] = q[i] > s->median4 ? q[i] - s->median4 : s->median4 - q[i];
    qsort(q, count, sizeof(*q), ascending);
    mad4 = middle(q, count);
    free(q);
    s->median_us = (s->median4 + 3) / 4;
    s->mad_us = (mad4 + 3) / 4;
    spread = mad4 / 40000 * 44478 + (mad4 % 40000 * 44478 + 39999) / 40000;
    percent = (s->median4 + 399) / 400;
    s->band_us = spread > percent ? spread : percent;
    if (s->error_us > s->band_us)
        s->band_us = s->error_us;
    return 0;
}

/**
 * Judge a candidate against its baseline: within the noise when the medians
 * differ by no more than the sum of the two bands, and otherwise a
 * regression when the candidate is slower or an improvement when faster.
 */
const char *cmp_verdict(const struct cmp_stats *base, const struct cmp_stats *cand)
{
    uint64_t difference = base->median4 > cand->median4 ? base->median4 - cand->median4
                                                        : cand->median4 - base->median4;
    if (difference <= 4 * (base->band_us + cand->band_us))
        return "within_noise";
    return cand->median4 > base->median4 ? "regression" : "improved";
}
