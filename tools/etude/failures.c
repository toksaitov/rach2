/*
 * failures.c: the attempts of a run that did not pass, read from the run's
 * history, as tools/ci/report.sh gives them in ci's issues.
 */
#include "failures.h"
#include "history_json.h"
#include "include/etude/history.h"
#include "../../utils/sha256.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FL_LINES      80  /* the most lines of a listing */
#define FL_WIDTH      240 /* the most bytes of a line, its indent included */
#define FL_ATTEMPTS   6   /* the most attempts of one item listed */
#define FL_ASSERTIONS 4   /* the most assertions of one attempt listed */
#define FL_PATH       4096

/* One record of the history; `replay` is set on a later copy of a record. */
struct fl_record {
    struct eh_json *json;
    const char *id, *kind;
    const struct eh_json *data;
    unsigned char digest[32];
    size_t length;
    int replay;
};

/* The records of every stream, in the order they were read. */
struct fl_history {
    struct fl_record *records;
    size_t count, capacity;
    int failed; /* memory ran out */
};

/* Lines of a listing, kept terminated by a NUL. */
struct fl_text {
    char *data;
    size_t length, capacity;
    unsigned lines;
    int failed; /* memory ran out */
};

/* What the attempts of one item of the plan came to. */
struct fl_item {
    const char *id;
    const struct eh_json *item, *summary; /* the plan's, and the run record's or NULL */
    size_t attempts;
    int worst;       /* the rank of the worst outcome of its attempts */
    int passed_once; /* an undisrupted attempt passed */
    int failed_once; /* an undisrupted attempt failed or did not finish */
};

static const char *const fl_words[] = {"passed", "was cancelled", "did not finish", "failed"};

/** Return a member's string, or NULL when it is absent or not a string. */
static const char *text_of(const struct eh_json *object, const char *name)
{
    const struct eh_json *member = eh_json_get(object, name);
    return member && member->type == 's' ? member->string : NULL;
}

/** Return the first element of an object's array member, or NULL. */
static const struct eh_json *first_of(const struct eh_json *object, const char *name)
{
    const struct eh_json *member = eh_json_get(object, name);
    return member && member->type == 'a' ? member->child : NULL;
}

/** Rank an outcome: pass 0, cancelled 1, incomplete 2, and failure 3. */
static int rank_of(const char *outcome)
{
    if (outcome && !strcmp(outcome, "pass"))
        return 0;
    if (outcome && !strcmp(outcome, "cancelled"))
        return 1;
    if (outcome && !strcmp(outcome, "incomplete"))
        return 2;
    return 3;
}

/** Append bytes to a listing. */
static void put(struct fl_text *t, const char *bytes, size_t length)
{
    if (t->failed)
        return;
    if (t->length + length + 1 > t->capacity) {
        size_t capacity = t->capacity ? t->capacity : 1024;
        char *data;
        while (capacity < t->length + length + 1)
            capacity *= 2;
        data = realloc(t->data, capacity);
        if (!data) {
            t->failed = 1;
            return;
        }
        t->data = data;
        t->capacity = capacity;
    }
    memcpy(t->data + t->length, bytes, length);
    t->length += length;
    t->data[t->length] = 0;
}

/**
 * Append a line indented by `indent` spaces. A line past FL_WIDTH bytes is
 * cut and ends in three periods; a backquote becomes a quote and any other
 * byte that is not printable ASCII a space, so that the listing can stand in
 * a fenced block of Markdown.
 */
static void add(struct fl_text *t, int indent, const char *format, ...)
{
    char line[FL_WIDTH + 2];
    size_t i, length;
    va_list arguments;
    int n;
    memset(line, ' ', (size_t)indent);
    va_start(arguments, format);
    n = vsnprintf(line + indent, FL_WIDTH + 1 - (size_t)indent, format, arguments);
    va_end(arguments);
    if (n < 0)
        line[indent] = 0;
    else if ((size_t)n + (size_t)indent > FL_WIDTH)
        memcpy(line + FL_WIDTH - 3, "...", 4);
    length = strlen(line);
    for (i = (size_t)indent; i < length; i++) {
        unsigned char c = (unsigned char)line[i];
        if (c == '`')
            line[i] = '\'';
        else if (c < ' ' || c > '~')
            line[i] = ' ';
    }
    line[length++] = '\n';
    put(t, line, length);
    t->lines++;
}

/** Take a validated line of a stream; return 1 to stop when memory ran out. */
static int take(const char *line, void *context)
{
    struct fl_history *h = context;
    struct fl_record *r;
    struct sha256 sha;
    if (h->count == h->capacity) {
        size_t capacity = h->capacity ? 2 * h->capacity : 256;
        struct fl_record *records = realloc(h->records, capacity * sizeof(*records));
        if (!records)
            return h->failed = 1;
        h->records = records;
        h->capacity = capacity;
    }
    r = &h->records[h->count];
    memset(r, 0, sizeof(*r));
    r->json = eh_json_parse(line);
    if (!r->json)
        return h->failed = 1;
    r->id = text_of(r->json, "record_id");
    r->kind = text_of(r->json, "kind");
    r->data = eh_json_get(r->json, "data");
    r->length = strlen(line);
    sha256_init(&sha);
    sha256_update(&sha, (const unsigned char *)line, r->length);
    sha256_final(&sha, r->digest);
    h->count++;
    return 0;
}

/** Read one stream whole; return -1 after naming why it cannot be used. */
static int load(struct fl_history *h, const char *schema, const char *path)
{
    uint64_t line = 0;
    int status = etude_history_read(path, schema, take, h, &line);
    if (h->failed) {
        fprintf(stderr, "history_failures: %s: out of memory\n", path);
        return -1;
    }
    if (status < 0) {
        fprintf(stderr, "history_failures: %s: unreadable or invalid at line %llu\n", path,
                (unsigned long long)line);
        return -1;
    }
    if (status == 1) {
        fprintf(stderr, "history_failures: %s: the record at line %llu is incomplete\n", path,
                (unsigned long long)line);
        return -1;
    }
    return 0;
}

static int by_identity(const void *a, const void *b)
{
    const struct fl_record *x = *(const struct fl_record *const *)a;
    const struct fl_record *y = *(const struct fl_record *const *)b;
    int order = strcmp(x->id, y->id);
    return order ? order : (x > y) - (x < y);
}

/**
 * Mark each later copy of a record that more than one stream gives; return -1
 * when one identity has two contents.
 */
static int mark_replays(struct fl_history *h)
{
    struct fl_record **index = malloc((h->count + 1) * sizeof(*index));
    size_t i;
    int status = 0;
    if (!index) {
        fprintf(stderr, "history_failures: out of memory\n");
        return -1;
    }
    for (i = 0; i < h->count; i++)
        index[i] = &h->records[i];
    qsort(index, h->count, sizeof(*index), by_identity);
    for (i = 1; i < h->count; i++) {
        struct fl_record *last = index[i - 1], *r = index[i];
        if (strcmp(last->id, r->id))
            continue;
        if (last->length != r->length || memcmp(last->digest, r->digest, 32)) {
            fprintf(stderr, "history_failures: record %s has two different contents\n", r->id);
            status = -1;
        }
        r->replay = 1;
    }
    free(index);
    return status;
}

/** Return the record with this identity, or NULL. */
static const struct fl_record *record_of(const struct fl_history *h, const char *id)
{
    size_t i;
    for (i = 0; id && i < h->count; i++)
        if (!h->records[i].replay && !strcmp(h->records[i].id, id))
            return &h->records[i];
    return NULL;
}

/**
 * Return a record of KIND, of the plan PLAN when it is not NULL, and set
 * `*count` to the number there are.
 */
static const struct fl_record *find(const struct fl_history *h, const char *kind, const char *plan,
                                    size_t *count)
{
    const struct fl_record *found = NULL;
    size_t i;
    *count = 0;
    for (i = 0; i < h->count; i++) {
        const struct fl_record *r = &h->records[i];
        if (r->replay || strcmp(r->kind, kind))
            continue;
        if (plan && strcmp(text_of(r->data, "plan_id"), plan))
            continue;
        found = r;
        ++*count;
    }
    return found;
}

/** Return the element of an array of objects whose member FIELD is TEXT. */
static const struct eh_json *element_of(const struct eh_json *e, const char *field,
                                        const char *text)
{
    for (; e; e = e->next) {
        const char *value = text_of(e, field);
        if (value && !strcmp(value, text))
            return e;
    }
    return NULL;
}

/** Return whether an array of strings has TEXT. */
static int listed(const struct eh_json *e, const char *text)
{
    for (; e; e = e->next)
        if (e->type == 's' && !strcmp(e->string, text))
            return 1;
    return 0;
}

/** Require an attempt record to be of the plan and one its item may use. */
static int check_attempt(const struct fl_record *plan, const struct fl_record *r)
{
    const char *item_id = text_of(r->data, "item_id");
    const char *attempt = text_of(r->data, "attempt_id");
    const struct eh_json *item = element_of(first_of(plan->data, "items"), "item_id", item_id);
    if (strcmp(text_of(r->data, "plan_id"), plan->id)) {
        fprintf(stderr, "history_failures: attempt record %s names plan %s, not %s\n", r->id,
                text_of(r->data, "plan_id"), plan->id);
        return -1;
    }
    if (!item) {
        fprintf(stderr, "history_failures: attempt record %s names item %s, which the plan lacks\n",
                r->id, item_id);
        return -1;
    }
    if (!listed(first_of(item, "attempt_ids"), attempt)) {
        fprintf(stderr, "history_failures: attempt %s is not one the plan gives item %s\n", attempt,
                item_id);
        return -1;
    }
    return 0;
}

/** Require each attempt record a run record lists to be one of its item's. */
static int check_run(const struct fl_history *h, const struct fl_record *run)
{
    const struct eh_json *e, *a;
    for (e = first_of(run->data, "items"); e; e = e->next) {
        const char *item_id = text_of(e, "item_id");
        for (a = first_of(e, "attempt_records"); a; a = a->next) {
            const struct fl_record *r = record_of(h, a->string);
            if (!r || strcmp(r->kind, "attempt") || strcmp(text_of(r->data, "item_id"), item_id)) {
                fprintf(stderr,
                        "history_failures: run record %s gives item %s the record %s, which is "
                        "not an attempt of it\n",
                        run->id, item_id, a->string);
                return -1;
            }
        }
    }
    return 0;
}

/** Require the attempt and run records to name what the history has. */
static int check_references(const struct fl_history *h, const struct fl_record *plan,
                            const struct fl_record *run)
{
    size_t i;
    for (i = 0; i < h->count; i++) {
        const struct fl_record *r = &h->records[i];
        if (!r->replay && !strcmp(r->kind, "attempt") && check_attempt(plan, r))
            return -1;
    }
    return run ? check_run(h, run) : 0;
}

/** Return whether R is an attempt record of the item ITEM_ID. */
static int attempt_of(const struct fl_record *r, const char *item_id)
{
    return !r->replay && !strcmp(r->kind, "attempt") &&
           !strcmp(text_of(r->data, "item_id"), item_id);
}

/** Judge the attempts of one item of the plan. */
static void judge(const struct fl_history *h, const struct fl_record *run,
                  const struct eh_json *item, struct fl_item *j)
{
    size_t i;
    memset(j, 0, sizeof(*j));
    j->id = text_of(item, "item_id");
    j->item = item;
    j->summary = run ? element_of(first_of(run->data, "items"), "item_id", j->id) : NULL;
    for (i = 0; i < h->count; i++) {
        const struct eh_json *result;
        int rank, disrupted;
        if (!attempt_of(&h->records[i], j->id))
            continue;
        result = eh_json_get(h->records[i].data, "result");
        rank = rank_of(text_of(result, "outcome"));
        disrupted = first_of(result, "disruptions") != NULL;
        j->attempts++;
        if (rank > j->worst)
            j->worst = rank;
        j->passed_once |= !disrupted && rank == 0;
        j->failed_once |= !disrupted && rank >= 2;
    }
}

/** Return whether every attempt of an item passed and its run record agrees. */
static int passed(const struct fl_item *j, const struct fl_record *run)
{
    if (!j->attempts || j->worst)
        return 0;
    return !run || (j->summary && !rank_of(text_of(j->summary, "outcome")));
}

/** Write what an item's attempts came to, after its case and identity. */
static void item_line(struct fl_text *t, const struct fl_item *j)
{
    const char *arch = text_of(j->item, "architecture");
    char head[FL_WIDTH];
    snprintf(head, sizeof(head), "%s%s%s, item %s", text_of(j->item, "case_id"),
             strcmp(arch, "none") ? " on " : "", strcmp(arch, "none") ? arch : "", j->id);
    if (!j->attempts)
        add(t, 0, "%s: no attempt was recorded", head);
    else if (j->worst && j->passed_once && j->failed_once)
        add(t, 0, "%s: %s, and passed on another attempt, so it may be flaky", head,
            fl_words[j->worst]);
    else if (j->worst)
        add(t, 0, "%s: %s", head, fl_words[j->worst]);
    else if (!j->summary)
        add(t, 0, "%s: passed, but the run record does not list it", head);
    else
        add(t, 0, "%s: passed on every attempt, but the run record gives it %s", head,
            text_of(j->summary, "outcome"));
}

/** Read an attempt's results document from the store; NULL when not kept. */
static struct eh_json *results_of(struct fl_text *t, const char *store, const struct eh_json *blob)
{
    const char *hex = text_of(blob, "sha256");
    const struct eh_json *size = eh_json_get(blob, "size_bytes");
    char path[FL_PATH], *bytes, digest[65];
    unsigned char raw[32];
    struct sha256 sha;
    struct eh_json *doc = NULL;
    size_t got;
    FILE *file;
    snprintf(path, sizeof(path), "%s/%s", store, hex);
    file = fopen(path, "rb");
    if (!file) {
        add(t, 4, "results: not in the store");
        return NULL;
    }
    if (size->number > (int64_t)EH_MAX_LINE) {
        fclose(file);
        add(t, 4, "results: %lld bytes, more than this reader takes", (long long)size->number);
        return NULL;
    }
    bytes = malloc((size_t)size->number + 2);
    if (!bytes) {
        fclose(file);
        t->failed = 1;
        return NULL;
    }
    got = fread(bytes, 1, (size_t)size->number + 1, file);
    fclose(file);
    sha256_init(&sha);
    sha256_update(&sha, (const unsigned char *)bytes, got);
    sha256_final(&sha, raw);
    sha256_hex(raw, digest);
    bytes[got] = 0;
    if (got != (size_t)size->number || strcmp(digest, hex) || strlen(bytes) != got)
        add(t, 4, "results: the store's copy does not match the history's digest");
    else if (!(doc = eh_json_parse(bytes)))
        add(t, 4, "results: the store's copy is not a JSON document");
    free(bytes);
    return doc;
}

/** List the report's state and the assertions that did not pass. */
static void list_results(struct fl_text *t, const char *store, const struct eh_json *attempt)
{
    struct eh_json *doc = results_of(t, store, eh_json_get(attempt, "assertion_results"));
    const struct eh_json *report, *e;
    const char *state, *outcome;
    unsigned shown = 0, more = 0;
    int undeclared;
    if (!doc)
        return;
    report = eh_json_get(doc, "report");
    state = text_of(report, "state");
    if (state && strcmp(state, "complete") && strcmp(state, "not_used")) {
        e = first_of(report, "errors");
        add(t, 4, "report: %s%s%s", state, e && e->type == 's' ? ": " : "",
            e && e->type == 's' ? e->string : "");
    }
    for (undeclared = 0; undeclared < 2; undeclared++) {
        for (e = first_of(doc, undeclared ? "undeclared" : "assertions"); e; e = e->next) {
            outcome = text_of(e, "outcome");
            if (!outcome || !strcmp(outcome, "pass"))
                continue;
            if (shown++ < FL_ASSERTIONS)
                add(t, 4, "%s%s: %s%s%s", undeclared ? "undeclared " : "",
                    text_of(e, "assertion_id"), outcome, text_of(e, "detail") ? ": " : "",
                    text_of(e, "detail") ? text_of(e, "detail") : "");
            else
                more++;
        }
    }
    if (more)
        add(t, 4, "and %u more assertions that did not pass", more);
    eh_json_free(doc);
}

/** Write the disruptions of a result, joined, into `out`. */
static void disruptions_of(const struct eh_json *result, char *out, size_t size)
{
    const struct eh_json *e;
    size_t used = 0;
    out[0] = 0;
    for (e = first_of(result, "disruptions"); e && used < size; e = e->next)
        used += (size_t)snprintf(out + used, size - used, "%s%s",
                                 used ? "; " : "; disrupted: ", e->string);
}

/** List one attempt, and when it did not pass, its results. */
static void list_attempt(struct fl_text *t, const char *store, const struct eh_json *attempt)
{
    const struct eh_json *result = eh_json_get(attempt, "result");
    const char *previous = text_of(attempt, "previous_attempt_id");
    const char *outcome = text_of(result, "outcome"), *reason = text_of(result, "reason");
    char disrupted[FL_WIDTH];
    disruptions_of(result, disrupted, sizeof(disrupted));
    add(t, 2, "attempt %s%s%s: %s%s%s%s", text_of(attempt, "attempt_id"),
        previous ? ", a retry of " : "", previous ? previous : "", outcome, reason ? ": " : "",
        reason ? reason : "", disrupted);
    if (store && rank_of(outcome))
        list_results(t, store, attempt);
}

/** List an item that did not pass and each of its attempts. */
static void list_item(struct fl_text *t, const struct fl_history *h, const char *store,
                      const struct fl_item *j)
{
    size_t i, shown = 0;
    item_line(t, j);
    for (i = 0; i < h->count; i++) {
        if (!attempt_of(&h->records[i], j->id))
            continue;
        if (shown++ < FL_ATTEMPTS)
            list_attempt(t, store, h->records[i].data);
    }
    if (shown > FL_ATTEMPTS)
        add(t, 2, "and %lu more attempts", (unsigned long)(shown - FL_ATTEMPTS));
}

/** Write why the run did not pass, if it did not; return 1 when it did not. */
static int run_line(struct fl_text *t, const struct fl_record *plan, const struct fl_record *run)
{
    const char *id = text_of(plan->json, "run_id");
    const struct eh_json *result = run ? eh_json_get(run->data, "result") : NULL;
    if (!run) {
        add(t, 0, "run %s: no run record, so the run did not finish", id);
        return 1;
    }
    if (!rank_of(text_of(result, "outcome")))
        return 0;
    add(t, 0, "run %s: %s: %s", id, text_of(result, "outcome"),
        text_of(result, "reason") ? text_of(result, "reason") : "no reason given");
    return 1;
}

/**
 * List the run and each item that did not pass, as many as FL_LINES lines
 * hold whole, and then the count of those left out; return 0 when the run
 * and every item passed, 1 otherwise, or -1 when memory ran out.
 */
static int listing(struct fl_text *out, const struct fl_history *h, const char *store,
                   const struct fl_record *plan, const struct fl_record *run)
{
    struct fl_text item = {NULL, 0, 0, 0, 0};
    const struct eh_json *e;
    unsigned long left = 0;
    int status = run_line(out, plan, run);
    for (e = first_of(plan->data, "items"); e; e = e->next) {
        struct fl_item j;
        judge(h, run, e, &j);
        if (passed(&j, run))
            continue;
        status = 1;
        item.length = item.lines = 0;
        list_item(&item, h, store, &j);
        if (out->lines + item.lines < FL_LINES) {
            put(out, item.data, item.length);
            out->lines += item.lines;
        } else {
            left++;
        }
    }
    if (left)
        add(out, 0, "and %lu more items that did not pass, which the history lists", left);
    free(item.data);
    return out->failed || item.failed ? -1 : status;
}

/** Judge and print the run of the history's one plan; return the status. */
static int report(const struct fl_history *h, const char *store)
{
    struct fl_text out = {NULL, 0, 0, 0, 0};
    const struct fl_record *plan, *run;
    size_t plans, runs, all;
    int status;
    plan = find(h, "plan", NULL, &plans);
    if (plans != 1) {
        fprintf(stderr, "history_failures: the history has %lu plan records, not one\n",
                (unsigned long)plans);
        return 2;
    }
    run = find(h, "run", plan->id, &runs);
    find(h, "run", NULL, &all);
    if (runs > 1) {
        fprintf(stderr, "history_failures: plan %s has %lu run records\n", plan->id,
                (unsigned long)runs);
        return 2;
    }
    if (all != runs) {
        fprintf(stderr, "history_failures: a run record names a plan other than %s\n", plan->id);
        return 2;
    }
    if (check_references(h, plan, run))
        return 2;
    status = listing(&out, h, store, plan, run);
    if (status < 0) {
        fprintf(stderr, "history_failures: out of memory\n");
        status = 2;
    } else if (out.data && (fputs(out.data, stdout) == EOF || fflush(stdout))) {
        fprintf(stderr, "history_failures: cannot write the listing\n");
        status = 2;
    }
    free(out.data);
    return status;
}

/** Free the records of a history. */
static void release(struct fl_history *h)
{
    size_t i;
    for (i = 0; i < h->count; i++)
        eh_json_free(h->records[i].json);
    free(h->records);
}

int etude_failures_main(int argc, char **argv)
{
    const char *schemas = "tools/etude/schemas", *store = NULL;
    char schema[FL_PATH];
    struct fl_history h;
    FILE *file;
    int i, status = 0;
    for (i = 1; i + 1 < argc && argv[i][0] == '-' && argv[i][1] && !argv[i][2]; i += 2) {
        if (argv[i][1] == 's')
            schemas = argv[i + 1];
        else if (argv[i][1] == 'b')
            store = argv[i + 1];
        else
            break;
    }
    if (i >= argc || argv[i][0] == '-') {
        fprintf(stderr, "usage: history_failures [-s SCHEMAS] [-b STORE] HISTORY...\n");
        return 2;
    }
    snprintf(schema, sizeof(schema), "%s/history.schema.json", schemas);
    file = fopen(schema, "rb");
    if (!file) {
        fprintf(stderr, "history_failures: %s: cannot read the schema\n", schema);
        return 2;
    }
    fclose(file);
    memset(&h, 0, sizeof(h));
    for (; i < argc && !status; i++)
        status = load(&h, schema, argv[i]) ? 2 : 0;
    if (!status)
        status = mark_replays(&h) ? 2 : report(&h, store);
    release(&h);
    return status;
}
