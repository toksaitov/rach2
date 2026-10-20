/*
 * verdict.c: casecheck's verdict on one run from its history, for ci: the
 * suites whose items did not pass, and `runner` when the records cannot show
 * that every planned item ran and passed.
 */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "casecheck.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "etude/history.h"
#include "schema.h"
#include "sha256.h"

/* The outcome of an attempt, an item or a run, in the history's words. */
enum result { RESULT_PASS, RESULT_FAILURE, RESULT_INCOMPLETE, RESULT_CANCELLED };

static const char *const RESULTS[] = {"pass", "failure", "incomplete", "cancelled"};

/* A record of the history, and the stream it was read from. */
struct record {
    struct json *doc;
    const struct json *data;
    const char *id;
    const char *kind;
    const char *stream;
};

/* An item of the plan, and what its attempt records say of it. */
struct item {
    const struct json *j;
    const char *id;
    const char *case_id;
    int suite;                     /* the index of its suite, or -1 */
    const struct record **attempt; /* its attempt records, in history order */
    size_t attempts, cap;
    enum result claimed; /* what the attempts' results give */
    enum result proved;  /* the same, once each pass is checked */
};

/* The verdict being reached, and what it was asked. */
struct verdict {
    const struct cases *set;
    const char *store;  /* the directory of blobs, or NULL */
    const char *commit; /* the commit the plan must name, or NULL */
    const char *arch;
    const char *name; /* the set's, for a failure outside its suites */
    char **suite;
    size_t suites;
    int *failed;    /* by suite: an item of it did not pass */
    int set_failed; /* the shared preparation or an item of no suite */
    int runner;     /* the runner left work unconfirmed */
    struct record *rec;
    size_t records, cap;
    char **owned; /* the paths of the streams read */
    size_t owners, owned_cap;
    const char *reading;
    const struct record *plan;
    const struct record *run;
    struct item *item;
    size_t items;
    struct json *schema; /* the results schema */
    char history_schema[1024];
};

static int same(const char *a, const char *b)
{
    return a != NULL && b != NULL && strcmp(a, b) == 0;
}

/** Returns the outcome `name` names; anything else is incomplete. */
static enum result result_of(const char *name)
{
    int i;

    for (i = 0; i < 4; i++) {
        if (same(RESULTS[i], name)) {
            return (enum result)i;
        }
    }
    return RESULT_INCOMPLETE;
}

/**
 * Returns the array `p` of `used` elements of `size` bytes, made larger when
 * its capacity `*cap` has no room for one more.
 */
static void *grow(void *p, size_t used, size_t *cap, size_t size)
{
    void *bigger;

    if (used < *cap) {
        return p;
    }
    *cap = *cap ? *cap * 2 : 16;
    bigger = realloc(p, *cap * size);
    if (bigger == NULL) {
        fprintf(stderr, "casecheck: out of memory\n");
        exit(2);
    }
    return bigger;
}

/** Orders two paths of a list for qsort. */
static int by_name(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/**
 * Lists the files of the directory `dir` whose names end in `suffix`, as
 * `DIR/NAME` in the order of their names; sets `*n` to their number. Returns
 * NULL when `dir` is no directory that can be read. The caller frees each
 * path and the list.
 */
static char **list_dir(const char *dir, const char *suffix, size_t *n)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    char **list = NULL;
    size_t cap = 0, k = strlen(suffix);

    *n = 0;
    if (d == NULL) {
        return NULL;
    }
    while ((e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);

        if (e->d_name[0] == '.' || len <= k || strcmp(e->d_name + len - k, suffix) != 0) {
            continue;
        }
        list = grow(list, *n, &cap, sizeof *list);
        list[*n] = json_alloc(strlen(dir) + len + 2);
        sprintf(list[*n], "%s/%s", dir, e->d_name);
        (*n)++;
    }
    closedir(d);
    if (list == NULL) {
        list = json_alloc(sizeof *list);
    }
    qsort(list, *n, sizeof *list, by_name);
    return list;
}

/** Frees a list of `n` paths that list_dir made. */
static void free_list(char **list, size_t n)
{
    while (n > 0) {
        free(list[--n]);
    }
    free(list);
}

/* ---- reading the history ---- */

/** Keeps one record of the stream being read; returns 0, or 1 to stop. */
static int keep_record(const char *line, void *ctx)
{
    struct verdict *v = ctx;
    struct record *r;
    struct json *doc;
    char err[200];

    if (json_parse(line, strlen(line), 1, &doc, err, sizeof err) != 0) {
        finding(v->reading, 0, "%s", err);
        return 1;
    }
    v->rec = grow(v->rec, v->records, &v->cap, sizeof *v->rec);
    r = &v->rec[v->records++];
    r->doc = doc;
    r->data = json_member(doc, "data");
    r->id = json_text(doc, "record_id");
    r->kind = json_text(doc, "kind");
    r->stream = v->reading;
    return 0;
}

/**
 * Reads the stream `path`. A stream that is not whole and valid is a finding:
 * the records after its fault are lost, and what they said is not known.
 */
static void read_stream(struct verdict *v, const char *path)
{
    uint64_t bad = 0;
    int status;

    v->owned = grow(v->owned, v->owners, &v->owned_cap, sizeof *v->owned);
    v->owned[v->owners] = json_alloc(strlen(path) + 1);
    strcpy(v->owned[v->owners], path);
    v->reading = v->owned[v->owners++];
    status = etude_history_read(path, v->history_schema, keep_record, v, &bad);
    if (status < 0) {
        finding(path, (int)bad, "the stream cannot be read, or is no valid history from here");
    } else if (status > 0) {
        finding(path, (int)bad,
                "the last record is cut off, so its writer did not close the stream");
    }
}

/** Reads each history: a stream, or a directory whose `.jsonl` files are. */
static void read_history(struct verdict *v, char **paths, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        size_t streams, s;
        char **list = list_dir(paths[i], ".jsonl", &streams);

        if (list == NULL) {
            read_stream(v, paths[i]);
            continue;
        }
        if (streams == 0) {
            finding(paths[i], 0, "the directory has no history stream");
        }
        for (s = 0; s < streams; s++) {
            read_stream(v, list[s]);
        }
        free_list(list, streams);
    }
}

/** Orders two records by their identities for qsort. */
static int by_record(const void *a, const void *b)
{
    return strcmp((*(const struct record *const *)a)->id, (*(const struct record *const *)b)->id);
}

/** Checks that every record is of one run, and has an identity of its own. */
static void check_identities(const struct verdict *v)
{
    const struct record **sorted = json_alloc((v->records + 1) * sizeof *sorted);
    const char *run_id = v->records > 0 ? json_text(v->rec[0].doc, "run_id") : NULL;
    size_t i;

    for (i = 0; i < v->records; i++) {
        sorted[i] = &v->rec[i];
        if (!same(json_text(v->rec[i].doc, "run_id"), run_id)) {
            finding(v->rec[i].stream, 0, "the record %s is of the run %s, not %s", v->rec[i].id,
                    json_text(v->rec[i].doc, "run_id"), run_id);
        }
    }
    qsort(sorted, v->records, sizeof *sorted, by_record);
    for (i = 1; i < v->records; i++) {
        if (same(sorted[i - 1]->id, sorted[i]->id)) {
            finding(sorted[i]->stream, 0, "the record %s is also in %s", sorted[i]->id,
                    sorted[i - 1]->stream);
        }
    }
    free(sorted);
}

/** Returns the one record of `kind`, or NULL after a finding. */
static const struct record *only(const struct verdict *v, const char *kind, const char *missing)
{
    const struct record *found = NULL;
    size_t i, count = 0;

    for (i = 0; i < v->records; i++) {
        if (same(v->rec[i].kind, kind)) {
            found = &v->rec[i];
            count++;
        }
    }
    if (count == 1) {
        return found;
    }
    if (count == 0) {
        finding(NULL, 0, "the history has no %s record, %s", kind, missing);
    } else {
        finding(found->stream, 0, "the history has %lu %s records, not one", (unsigned long)count,
                kind);
    }
    return NULL;
}

/** Finds the plan and the run, and checks the commit the plan tested. */
static void find_plan_and_run(struct verdict *v)
{
    v->plan = only(v, "plan", "so nothing says what was to run");
    v->run = only(v, "run", "so the run did not finish");
    if (v->plan == NULL) {
        return;
    }
    if (v->run != NULL && !same(json_text(v->run->data, "plan_id"), v->plan->id)) {
        finding(v->run->stream, 0, "the run record %s is of the plan %s, not %s", v->run->id,
                json_text(v->run->data, "plan_id"), v->plan->id);
        v->run = NULL;
    }
    if (v->commit != NULL && !same(json_text(v->plan->data, "commit"), v->commit)) {
        finding(v->plan->stream, 0, "the plan tested the commit %s, not %s",
                json_text(v->plan->data, "commit"), v->commit);
    }
}

/* ---- the plan's items and their records ---- */

/**
 * Returns the index of the suite of the family `family`: the last part of its
 * identity, between dots, that is the name of a suite asked for; -1 when no
 * part is.
 */
static int suite_of(const struct verdict *v, const char *family)
{
    const char *end = family != NULL ? family + strlen(family) : NULL;
    size_t i;

    while (end != NULL && end > family) {
        const char *start = end;

        while (start > family && start[-1] != '.') {
            start--;
        }
        for (i = 0; i < v->suites; i++) {
            if (strlen(v->suite[i]) == (size_t)(end - start) &&
                strncmp(start, v->suite[i], (size_t)(end - start)) == 0) {
                return (int)i;
            }
        }
        end = start > family ? start - 1 : NULL;
    }
    return -1;
}

/** Returns the item `id` of the plan, or NULL. */
static struct item *find_item(const struct verdict *v, const char *id)
{
    size_t i;

    for (i = 0; i < v->items; i++) {
        if (same(v->item[i].id, id)) {
            return &v->item[i];
        }
    }
    return NULL;
}

/** Orders two strings for qsort. */
static int by_text(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/** Checks that no two items of the plan have an attempt identity in common. */
static void check_attempt_ids(const struct verdict *v)
{
    const char **ids = NULL;
    size_t n = 0, cap = 0, i, k;

    for (i = 0; i < v->items; i++) {
        const struct json *list = json_member(v->item[i].j, "attempt_ids");

        for (k = 0; k < list->count; k++) {
            ids = grow(ids, n, &cap, sizeof *ids);
            ids[n++] = list->item[k]->string;
        }
    }
    if (ids == NULL) {
        return;
    }
    qsort(ids, n, sizeof *ids, by_text);
    for (i = 1; i < n; i++) {
        if (same(ids[i - 1], ids[i])) {
            finding(v->plan->stream, 0, "the plan gives the attempt %s to two items", ids[i]);
        }
    }
    free(ids);
}

/** Takes the items of the plan, each with the suite it belongs to. */
static void load_items(struct verdict *v)
{
    const struct json *list = json_member(v->plan->data, "items");
    size_t i;

    v->items = list->count;
    v->item = json_alloc((v->items + 1) * sizeof *v->item);
    for (i = 0; i < v->items; i++) {
        struct item *it = &v->item[i];

        it->j = list->item[i];
        it->id = json_text(it->j, "item_id");
        it->case_id = json_text(it->j, "case_id");
        it->suite = suite_of(v, json_text(it->j, "family_id"));
        if (find_item(v, it->id) != it) {
            finding(v->plan->stream, 0, "the plan has the item %s twice", it->id);
        }
    }
    check_attempt_ids(v);
}

/** Returns 1 when the array `list` has the string `text`. */
static int listed(const struct json *list, const char *text)
{
    size_t i;

    for (i = 0; list != NULL && i < list->count; i++) {
        if (same(list->item[i]->string, text)) {
            return 1;
        }
    }
    return 0;
}

/** Returns the attempt record of the attempt `id` of `it`, or NULL. */
static const struct record *attempt_of(const struct item *it, const char *id)
{
    size_t i;

    for (i = 0; it != NULL && i < it->attempts; i++) {
        if (same(json_text(it->attempt[i]->data, "attempt_id"), id)) {
            return it->attempt[i];
        }
    }
    return NULL;
}

/** Gives each attempt record to its item, once it is one the plan gave. */
static void attach_attempts(struct verdict *v)
{
    size_t i;

    for (i = 0; i < v->records; i++) {
        const struct record *r = &v->rec[i];
        const char *id = json_text(r->data, "attempt_id");
        struct item *it;

        if (!same(r->kind, "attempt")) {
            continue;
        }
        it = find_item(v, json_text(r->data, "item_id"));
        if (!same(json_text(r->data, "plan_id"), v->plan->id) || it == NULL) {
            finding(r->stream, 0, "the attempt record %s is of no item of the plan %s", r->id,
                    v->plan->id);
        } else if (!listed(json_member(it->j, "attempt_ids"), id)) {
            finding(r->stream, 0, "the attempt %s is not one the plan gave the item %s", id,
                    it->id);
        } else if (attempt_of(it, id) != NULL) {
            finding(r->stream, 0, "the attempt %s has two records", id);
        } else {
            it->attempt = grow(it->attempt, it->attempts, &it->cap, sizeof *it->attempt);
            it->attempt[it->attempts++] = r;
        }
    }
}

/* ---- steps and the shared preparation ---- */

/** Returns the step record whose identity is `id`, or NULL. */
static const struct record *step_record(const struct verdict *v, const char *id)
{
    size_t i;

    for (i = 0; i < v->records; i++) {
        if (same(v->rec[i].kind, "step") && same(v->rec[i].id, id)) {
            return &v->rec[i];
        }
    }
    return NULL;
}

/** Returns 1 when the step record `s` passed, its cleanup confirmed. */
static int step_done(const struct record *s)
{
    const struct json *result = json_member(s->data, "result");

    return same(json_text(result, "outcome"), "pass") &&
           same(json_text(result, "cleanup"), "confirmed");
}

/** Returns the steps that the record of the step `r` must be among, or NULL. */
static const struct json *owner_steps(const struct verdict *v, const struct record *r)
{
    const char *attempt = json_text(r->data, "attempt_id");
    const struct record *a;

    if (attempt == NULL) {
        return json_member(v->run->data, "preparation_steps");
    }
    a = attempt_of(find_item(v, json_text(r->data, "item_id")), attempt);
    return a != NULL ? json_member(a->data, "steps") : NULL;
}

/**
 * Checks that each step record is listed by its attempt, or by the run as
 * shared preparation, and that each step an attempt lists is a record of
 * that attempt.
 */
static void check_steps(const struct verdict *v)
{
    size_t i, k, s;

    for (i = 0; i < v->records; i++) {
        const struct record *r = &v->rec[i];
        const char *attempt = json_text(r->data, "attempt_id");

        if (!same(r->kind, "step") || (attempt == NULL && v->run == NULL)) {
            continue;
        }
        if (!same(json_text(r->data, "plan_id"), v->plan->id)) {
            finding(r->stream, 0, "the step record %s is of the plan %s, not %s", r->id,
                    json_text(r->data, "plan_id"), v->plan->id);
        } else if (!listed(owner_steps(v, r), r->id)) {
            finding(r->stream, 0, "the step record %s is not among the steps of %s", r->id,
                    attempt != NULL ? attempt : "the run's preparation");
        }
    }
    for (i = 0; i < v->items; i++) {
        for (k = 0; k < v->item[i].attempts; k++) {
            const struct record *a = v->item[i].attempt[k];
            const struct json *steps = json_member(a->data, "steps");

            for (s = 0; s < steps->count; s++) {
                const struct record *r = step_record(v, steps->item[s]->string);

                if (r == NULL ||
                    !same(json_text(r->data, "attempt_id"), json_text(a->data, "attempt_id"))) {
                    finding(a->stream, 0, "the attempt %s lists the step record %s, not its own",
                            json_text(a->data, "attempt_id"), steps->item[s]->string);
                }
            }
        }
    }
}

/**
 * Checks the run's shared preparation. Returns 1 when every step of it
 * passed; a step that did not is printed and fails the set.
 */
static int check_preparation(struct verdict *v)
{
    const struct json *list = json_member(v->run->data, "preparation_steps");
    size_t i;
    int done = 1;

    for (i = 0; i < list->count; i++) {
        const struct record *s = step_record(v, list->item[i]->string);

        if (s == NULL || json_member(s->data, "item_id")->type != JSON_NULL) {
            finding(v->run->stream, 0, "the run's preparation step %s has no record of one",
                    list->item[i]->string);
            done = 0;
        } else if (!step_done(s)) {
            say("preparation %s: %s, cleanup %s\n", json_text(s->data, "step_id"),
                json_text(json_member(s->data, "result"), "outcome"),
                json_text(json_member(s->data, "result"), "cleanup"));
            v->set_failed = 1;
            done = 0;
        }
    }
    return done;
}

/* ---- the evidence of a pass ---- */

/**
 * Reads the results document of the attempt `a` from the store, by the
 * digest its record gives, into a document; its path goes in `path`. Returns
 * NULL after a finding when the store does not have those bytes or they are
 * no document.
 */
static struct json *read_results(const struct verdict *v, const struct record *a, char *path,
                                 size_t n)
{
    const struct json *blob = json_member(a->data, "assertion_results");
    const char *attempt = json_text(a->data, "attempt_id");
    unsigned char digest[32];
    char hex[65], err[200];
    struct sha256 sha;
    struct json *doc = NULL;
    size_t len;
    char *text;
    FILE *f;

    snprintf(path, n, "%s/%s", v->store != NULL ? v->store : ".", json_text(blob, "sha256"));
    f = v->store != NULL ? fopen(path, "rb") : NULL;
    if (f == NULL) {
        finding(a->stream, 0, "the results of the attempt %s, a pass, are not in the store",
                attempt);
        return NULL;
    }
    fclose(f);
    text = read_file(path, &len);
    if (text == NULL) {
        return NULL;
    }
    sha256_init(&sha);
    sha256_update(&sha, (const unsigned char *)text, len);
    sha256_final(&sha, digest);
    sha256_hex(digest, hex);
    if ((long long)len != json_member(blob, "size_bytes")->integer ||
        !same(hex, json_text(blob, "sha256"))) {
        finding(path, 0, "the bytes are not those the attempt %s gives as its results", attempt);
    } else if (json_parse(text, len, 1, &doc, err, sizeof err) != 0) {
        finding(path, 0, "%s", err);
    }
    free(text);
    return doc;
}

/** Makes a finding when the member `field` of the results is not `want`. */
static void expect(const struct json *doc, const char *field, const char *want, const char *path)
{
    if (!same(json_text(doc, field), want)) {
        finding(path, 0, "the results give the %s %s, where the history has %s", field,
                json_text(doc, field), want);
    }
}

/** Returns 1 when the case `c` runs on `arch`, none for a host case. */
static int runs_on(const struct json *c, const char *arch)
{
    const struct json *archs = json_member(c, "architectures");

    return archs->type == JSON_STRING ? same(arch, "none") : listed(archs, arch);
}

/**
 * Checks the results document `doc`, at `path`, of the attempt `a` of `it`,
 * which its record gives as a pass; returns the outcome it supports.
 */
static enum result judge_pass(const struct verdict *v, const struct item *it,
                              const struct record *a, const struct json *doc, const char *path)
{
    const struct json *c = find_case(v->set, it->case_id, NULL);
    int before = findings(), judged;
    char why[300];

    if (schema_findings(v->schema, doc, path) != 0) {
        return RESULT_INCOMPLETE;
    }
    expect(doc, "run_id", json_text(a->doc, "run_id"), path);
    expect(doc, "attempt_id", json_text(a->data, "attempt_id"), path);
    expect(doc, "item_id", it->id, path);
    expect(doc, "case_id", it->case_id, path);
    expect(doc, "placement", json_text(it->j, "domain"), path);
    expect(doc, "inventory_sha256", json_text(json_member(it->j, "assertions"), "sha256"), path);
    if (c != NULL && !runs_on(c, json_text(it->j, "architecture"))) {
        finding(path, 0, "%s does not run on %s, the architecture of the item %s", it->case_id,
                json_text(it->j, "architecture"), it->id);
    }
    judged = judge_document(v->set, doc, path, why, sizeof why);
    if (judged > 0) {
        finding(path, 0, "the attempt %s is recorded as a pass, and its results give %s, as %s",
                json_text(a->data, "attempt_id"), RESULTS[judged], why);
        return judged == ATTEMPT_FAILURE ? RESULT_FAILURE : RESULT_INCOMPLETE;
    }
    return judged < 0 || findings() > before ? RESULT_INCOMPLETE : RESULT_PASS;
}

/**
 * Returns the outcome that the evidence of the attempt `a` of `it` supports.
 * Only a pass is checked: it needs each step the attempt lists passed, and
 * its results document in the store, of this run, attempt, item and case,
 * with assertions that give a pass.
 */
static enum result prove(const struct verdict *v, const struct item *it, const struct record *a)
{
    enum result claim = result_of(json_text(json_member(a->data, "result"), "outcome"));
    const struct json *steps = json_member(a->data, "steps");
    enum result proved;
    struct json *doc;
    char path[1200];
    size_t s;

    if (claim != RESULT_PASS) {
        return claim;
    }
    for (s = 0; s < steps->count; s++) {
        const struct record *r = step_record(v, steps->item[s]->string);

        if (r != NULL && !step_done(r)) {
            finding(a->stream, 0, "the attempt %s is recorded as a pass, and its step %s is %s",
                    json_text(a->data, "attempt_id"), json_text(r->data, "step_id"),
                    json_text(json_member(r->data, "result"), "outcome"));
            return RESULT_INCOMPLETE;
        }
    }
    doc = read_results(v, a, path, sizeof path);
    if (doc == NULL) {
        return RESULT_INCOMPLETE;
    }
    proved = judge_pass(v, it, a, doc, path);
    json_free(doc);
    return proved;
}

/* ---- outcomes ---- */

/**
 * Returns the outcome of an item from those of its `n` attempts: the worst of
 * them, as the run record gives it (Readme.md, Repetitions), a failure before
 * work or cleanup that is not confirmed, that before a cancellation, and that
 * before a pass. A retry that passed leaves the item what the attempt it
 * repeats came to.
 */
static enum result item_result(const enum result *outcome, size_t n, int unconfirmed)
{
    int seen[4] = {0, 0, 0, 0};
    size_t i;

    for (i = 0; i < n; i++) {
        seen[outcome[i]] = 1;
    }
    if (seen[RESULT_FAILURE]) {
        return RESULT_FAILURE;
    }
    if (n == 0 || unconfirmed || seen[RESULT_INCOMPLETE]) {
        return RESULT_INCOMPLETE;
    }
    return seen[RESULT_CANCELLED] ? RESULT_CANCELLED : RESULT_PASS;
}

/**
 * Gives each item the outcome its attempts claim, and the one their evidence
 * supports.
 */
static void judge_items(struct verdict *v)
{
    size_t i, k;

    for (i = 0; i < v->items; i++) {
        struct item *it = &v->item[i];
        enum result *claimed = json_alloc((it->attempts + 1) * sizeof *claimed);
        enum result *proved = json_alloc((it->attempts + 1) * sizeof *proved);
        int unconfirmed = 0;

        for (k = 0; k < it->attempts; k++) {
            const struct json *result = json_member(it->attempt[k]->data, "result");

            claimed[k] = result_of(json_text(result, "outcome"));
            proved[k] = prove(v, it, it->attempt[k]);
            unconfirmed |= !same(json_text(result, "cleanup"), "confirmed");
        }
        it->claimed = item_result(claimed, it->attempts, unconfirmed);
        it->proved = item_result(proved, it->attempts, unconfirmed);
        free(claimed);
        free(proved);
    }
}

/**
 * Returns the outcome the items' claims, the preparation and the run's own
 * cleanup give the run, in the order of item_result.
 */
static enum result run_result(const struct verdict *v, int prepared)
{
    int seen[4] = {0, 0, 0, 0};
    size_t i;

    for (i = 0; i < v->items; i++) {
        seen[v->item[i].claimed] = 1;
    }
    if (seen[RESULT_FAILURE]) {
        return RESULT_FAILURE;
    }
    if (seen[RESULT_INCOMPLETE] || !prepared ||
        !same(json_text(json_member(v->run->data, "result"), "cleanup"), "confirmed")) {
        return RESULT_INCOMPLETE;
    }
    return seen[RESULT_CANCELLED] ? RESULT_CANCELLED : RESULT_PASS;
}

/** Returns 1 when `ids` are the identities of the attempt records of `it`. */
static int same_records(const struct json *ids, const struct item *it)
{
    size_t k;

    if (ids->count != it->attempts) {
        return 0;
    }
    for (k = 0; k < it->attempts; k++) {
        if (!listed(ids, it->attempt[k]->id)) {
            return 0;
        }
    }
    return 1;
}

/**
 * Checks the run's summary against the records: each item of the plan once,
 * with the records of its attempts and the outcome they give, and the run's
 * result.
 */
static void check_summary(const struct verdict *v, int prepared)
{
    const struct json *list = json_member(v->run->data, "items");
    const char *at = v->run->stream;
    const char *outcome = json_text(json_member(v->run->data, "result"), "outcome");
    int *seen = json_alloc((v->items + 1) * sizeof *seen);
    enum result claimed = run_result(v, prepared);
    size_t i;

    for (i = 0; i < list->count; i++) {
        const struct json *e = list->item[i];
        const struct item *it = find_item(v, json_text(e, "item_id"));

        if (it == NULL || seen[it - v->item]++) {
            finding(at, 0, "the run's summary names %s, which is no item of the plan, or twice",
                    json_text(e, "item_id"));
        } else if (!same_records(json_member(e, "attempt_records"), it)) {
            finding(at, 0, "the run's summary of the item %s does not list its attempt records",
                    it->id);
        } else if (!same(json_text(e, "outcome"), RESULTS[it->claimed])) {
            finding(at, 0, "the run's summary gives the item %s %s, and its attempts give %s",
                    it->id, json_text(e, "outcome"), RESULTS[it->claimed]);
        }
    }
    for (i = 0; i < v->items; i++) {
        if (!seen[i]) {
            finding(at, 0, "the run's summary leaves out the item %s", v->item[i].id);
        }
    }
    if (!same(outcome, RESULTS[claimed])) {
        finding(at, 0, "the run's result is %s, and its items and preparation give %s", outcome,
                RESULTS[claimed]);
    }
    free(seen);
}

/* ---- what was asked for ---- */

/**
 * Returns the number of the plan's items of `case_id` on `arch`, each
 * repetition counted once.
 */
static size_t repetitions(const struct verdict *v, const char *case_id, const char *arch)
{
    size_t i, k, count = 0;

    for (i = 0; i < v->items; i++) {
        const struct item *it = &v->item[i];
        long long rep = json_member(it->j, "repetition")->integer;

        if (!same(it->case_id, case_id) || !same(json_text(it->j, "architecture"), arch)) {
            continue;
        }
        for (k = 0; k < i; k++) {
            if (same(v->item[k].case_id, case_id) &&
                same(json_text(v->item[k].j, "architecture"), arch) &&
                json_member(v->item[k].j, "repetition")->integer == rep) {
                break;
            }
        }
        count += k == i;
    }
    return count;
}

/** Returns 1 when the plan has an item of the suite `s`. */
static int has_suite(const struct verdict *v, size_t s)
{
    size_t i;

    for (i = 0; i < v->items; i++) {
        if (v->item[i].suite == (int)s) {
            return 1;
        }
    }
    return 0;
}

/**
 * Checks that the plan has what was asked for: an item of each suite, and of
 * each case of a suite's manifests that runs on the architecture, as many
 * items as its workload's repetitions. Work left out fails its suite.
 */
static void check_coverage(struct verdict *v)
{
    size_t s, m, c;

    for (s = 0; s < v->suites; s++) {
        if (!has_suite(v, s)) {
            finding(NULL, 0, "the plan has no item of the suite %s", v->suite[s]);
            v->failed[s] = 1;
        }
    }
    for (m = 0; m < v->set->n; m++) {
        const struct json *doc = v->set->m[m].doc;
        const struct json *list = json_member(doc, "cases");
        int suite = suite_of(v, json_text(doc, "family_id"));

        for (c = 0; suite >= 0 && c < list->count; c++) {
            const struct json *k = list->item[c];
            const char *arch =
                json_member(k, "architectures")->type == JSON_STRING ? "none" : v->arch;
            long long want = json_member(json_member(k, "workload"), "repetitions")->integer;
            size_t have = repetitions(v, json_text(k, "case_id"), arch);

            if (runs_on(k, arch) && (long long)have < want) {
                finding(v->set->m[m].path, k->line,
                        "the plan has %lu of the %lld items of %s on %s", (unsigned long)have, want,
                        json_text(k, "case_id"), arch);
                v->failed[suite] = 1;
            }
        }
    }
}

/** Prints each item that did not pass, and fails its suite, or the set. */
static void report_items(struct verdict *v)
{
    size_t i;

    for (i = 0; i < v->items; i++) {
        const struct item *it = &v->item[i];

        if (it->proved == RESULT_PASS) {
            continue;
        }
        say("item %s: %s on %s: %s, in %s\n", it->id, it->case_id, json_text(it->j, "architecture"),
            RESULTS[it->proved], it->suite >= 0 ? v->suite[it->suite] : v->name);
        if (it->suite >= 0) {
            v->failed[it->suite] = 1;
        } else {
            v->set_failed = 1;
        }
    }
}

/** Reaches the verdict on what the history says of the run. */
static void reach(struct verdict *v)
{
    int prepared;

    check_identities(v);
    find_plan_and_run(v);
    if (v->plan == NULL) {
        return;
    }
    load_items(v);
    attach_attempts(v);
    check_steps(v);
    judge_items(v);
    if (v->run != NULL) {
        prepared = check_preparation(v);
        check_summary(v, prepared);
        if (!same(json_text(json_member(v->run->data, "result"), "cleanup"), "confirmed")) {
            say("the run's own cleanup is unconfirmed\n");
            v->runner = 1;
        }
    }
    check_coverage(v);
    report_items(v);
}

/* ---- the command ---- */

/** Adds `name` to the names of `b`, once. */
static void add_name(struct jbuf *b, const char *name)
{
    size_t len = strlen(name);
    const char *p;

    for (p = strstr(b->data, name); p != NULL; p = strstr(p + 1, name)) {
        if (p[-1] == ' ' && (p[len] == ' ' || p[len] == '\0')) {
            return;
        }
    }
    jbuf_puts(b, " ");
    jbuf_puts(b, name);
}

/**
 * Prints the result line, `casecheck: result: passed`, or `casecheck:
 * result: failed:` with the names of what failed: `runner` first when
 * `runner` is set, then the suites in the order asked, then the set. Returns
 * 0 for a pass and 1 for anything else.
 */
static int conclude(const struct verdict *v, int runner)
{
    struct jbuf names = {0};
    const char *p, *end;
    size_t i;
    int failed;

    jbuf_put(&names, "", 0);
    if (runner) {
        add_name(&names, "runner");
    }
    for (i = 0; i < v->suites; i++) {
        if (v->failed[i]) {
            add_name(&names, v->suite[i]);
        }
    }
    if (v->set_failed) {
        add_name(&names, v->name);
    }
    failed = names.len > 0;
    say(failed ? "casecheck: result: failed:" : "casecheck: result: passed\n");
    for (p = names.data; *p != '\0'; p = end) {
        end = strchr(p + 1, ' ');
        end = end != NULL ? end : p + strlen(p);
        say("%.*s", (int)(end - p), p);
    }
    if (failed) {
        say("\n");
    }
    jbuf_free(&names);
    return failed;
}

/**
 * Splits the suites, names separated by commas, into `v`; returns -1 for an
 * empty name.
 */
static int split_suites(struct verdict *v, char *text)
{
    char *p = text, *comma;

    v->suite = json_alloc((strlen(text) / 2 + 2) * sizeof *v->suite);
    for (;;) {
        comma = strchr(p, ',');
        if (comma != NULL) {
            *comma = '\0';
        }
        if (*p == '\0') {
            return -1;
        }
        v->suite[v->suites++] = p;
        if (comma == NULL) {
            break;
        }
        p = comma + 1;
    }
    v->failed = json_alloc(v->suites * sizeof *v->failed);
    return 0;
}

/** Reads the options of `verdict`; returns the words they take, or -1. */
static int verdict_options(struct verdict *v, char **args, int n, const char **cases)
{
    int i;

    for (i = 0; i + 1 < n && args[i][0] == '-'; i += 2) {
        if (strcmp(args[i], "-b") == 0) {
            v->store = args[i + 1];
        } else if (strcmp(args[i], "-c") == 0) {
            v->commit = args[i + 1];
        } else if (strcmp(args[i], "-m") == 0) {
            *cases = args[i + 1];
        } else {
            return -1;
        }
    }
    return i;
}

/** Frees what the verdict holds. */
static void free_verdict(struct verdict *v)
{
    size_t i;

    for (i = 0; i < v->items; i++) {
        free(v->item[i].attempt);
    }
    for (i = 0; i < v->records; i++) {
        json_free(v->rec[i].doc);
    }
    free_list(v->owned, v->owners);
    free(v->item);
    free(v->rec);
    free(v->suite);
    free(v->failed);
    json_free(v->schema);
}

/**
 * Runs the `verdict` command: `[-b STORE] [-c COMMIT] [-m CASES] ARCH SET
 * SUITES HISTORY...`. Each HISTORY is a stream of the run, or a directory
 * whose `.jsonl` files are its streams; STORE is the directory of the blobs
 * they reference, each named by its SHA-256; CASES the directory of the case
 * manifests, `tests/cases` by default; and COMMIT the commit the plan must
 * have tested. SUITES are the suites asked for, separated by commas, and SET
 * the name of the set that ran them, which a failure outside them takes.
 * Prints each item that did not pass and a result line. Returns 0 for a
 * pass, 1 for anything else, and 2 for a wrong usage or schemas that cannot
 * be read.
 */
int judge_verdict(struct cases *set, char **args, int n)
{
    struct verdict v;
    const char *cases = "tests/cases";
    char **manifests;
    char *suites = NULL;
    size_t count = 0;
    int skip, status = 2, before = findings();

    memset(&v, 0, sizeof v);
    skip = verdict_options(&v, args, n, &cases);
    if (skip < 0 || n - skip < 4 || !(same(args[skip], "i686") || same(args[skip], "x86_64"))) {
        return 2;
    }
    args += skip;
    n -= skip;
    v.arch = args[0];
    v.name = args[1];
    suites = json_alloc(strlen(args[2]) + 1);
    strcpy(suites, args[2]);
    v.schema = read_schema(set->schemas, "results");
    snprintf(v.history_schema, sizeof v.history_schema, "%s/history.schema.json", set->schemas);
    if (split_suites(&v, suites) == 0 && v.schema != NULL) {
        manifests = list_dir(cases, ".json", &count);
        if (manifests == NULL) {
            finding(cases, 0, "no directory of case manifests");
        } else {
            load_manifests(set, manifests, (int)count);
        }
        v.set = set;
        read_history(&v, args + 3, n - 3);
        reach(&v);
        status = conclude(&v, v.runner || findings() > before);
        free_manifests(set);
        if (manifests != NULL) {
            free_list(manifests, count);
        }
    }
    free_verdict(&v);
    free(suites);
    return status;
}
