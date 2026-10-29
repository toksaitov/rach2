/*
 * lines.c: the compatible formatter. It reads a run's history and prints the
 * lines tests/run printed, so that the release tests tell a suite's failure
 * from a failure of the runner itself.
 */
#include "lines.h"
#include "history_json.h"
#include "include/etude/history.h"
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The longest line of a suite map, and of a fault's text. */
#define MAP_LINE   4096
#define FAULT_TEXT 1024

/* The outcomes of an attempt or an item, lightest first. */
enum rank { R_PASS, R_CANCELLED, R_INCOMPLETE, R_FAILURE };

static const char *const rank_names[] = {"pass", "cancelled", "incomplete", "failure"};
static const char *const suite_words[] = {"PASS", "CANCELLED", "INCOMPLETE", "FAIL"};

static const char usage_text[] =
    "usage: etude show lines [-s SCHEMAS] [-a ARCHES] [-f NAME]... MAP STREAM...\n";

/* The most failures outside the suites that a run may name with -f. */
#define EXTRA_MAX 8

/* A suite of the map: its name, and the cases and families it holds. In a
 * run of two architectures, each line of the map but the runner's gives a
 * suite on each, named `ARCH:SUITE`. */
struct suite {
    char *name;
    const char *arch; /* the architecture of its items, or NULL for any */
    size_t line;      /* the line of the map that gave it */
    int dropped;      /* it has no item, and the suite of its line on the
                         other architecture has */
    char **ids;
    size_t nids;
    size_t items;    /* the planned items of it */
    enum rank worst; /* the heaviest outcome of those items */
    int own;         /* an item failed, or did not finish, by its own cases */
    /* The clock of its attempts' times. */
    const struct eh_json *clock;
    uint64_t start_us, end_us;
    int untimed; /* an attempt did not end, or was timed on another clock */
};

/* An item of the plan, and what its attempts and the run record say. */
struct item {
    const char *id, *case_id, *family_id, *arch;
    int64_t repetition;
    const struct eh_json *attempt_ids;
    int suite;           /* into the map, or -1 */
    unsigned listed;     /* the times the run record lists it */
    const char *summary; /* the run record's outcome of it, or NULL */
    size_t attempts;     /* its attempt records in the history */
    enum rank worst;     /* the heaviest outcome of those */
    const char *reason;  /* the reason of the first attempt of that outcome */
    int own;             /* an attempt failed, or did not finish undisrupted */
    enum rank rank;      /* the item's outcome, as judge gives it */
};

/* A plan, attempt or run record of the history. */
struct record {
    struct eh_json *doc;
    const char *id, *kind;
    const struct eh_json *data;
    int listed; /* an attempt record the run record lists */
};

/* One reading of a run: the map, the records, and the runner's faults. */
struct lines {
    const char *schema;
    const char *stream; /* the stream being read */
    char *run_id;
    const char *foreign; /* the last stream with a record of another run */
    struct record *recs;
    size_t nrecs, caprecs;
    char **faults;
    size_t nfaults, capfaults;
    struct suite *suites;
    size_t nsuites;
    const char *arches[2]; /* a run of two architectures, as -a gives them */
    size_t narches;
    struct item *items;
    size_t nitems;
    const struct record *plan, *run;
    const char *extra[EXTRA_MAX]; /* failures outside the suites, as -f names them */
    size_t nextra;
    int nomem;
};

/** Copy a string into memory of its own, or give NULL. */
static char *copy(const char *s)
{
    char *out = malloc(strlen(s) + 1);
    if (out)
        strcpy(out, s);
    return out;
}

/** Note a fault of the runner, which the output names as `runner`. */
static void fault(struct lines *l, const char *format, ...)
{
    char text[FAULT_TEXT];
    va_list ap;
    if (l->nfaults == l->capfaults) {
        size_t cap = l->capfaults ? 2 * l->capfaults : 16;
        char **more = realloc(l->faults, cap * sizeof(*more));
        if (!more) {
            l->nomem = 1;
            return;
        }
        l->faults = more;
        l->capfaults = cap;
    }
    va_start(ap, format);
    vsnprintf(text, sizeof(text), format, ap);
    va_end(ap);
    if (!(l->faults[l->nfaults] = copy(text)))
        l->nomem = 1;
    else
        l->nfaults++;
}

/** Print one line and flush it, so that lines of both streams stay in order. */
static void say(FILE *f, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    vfprintf(f, format, ap);
    va_end(ap);
    fputc('\n', f);
    fflush(f);
}

/** Give a string member of an object, or "" when it is absent or not one. */
static const char *text_of(const struct eh_json *object, const char *key)
{
    const struct eh_json *v = eh_json_get(object, key);
    return v && v->type == 's' ? v->string : "";
}

/** Say whether a suite's name has the form of the registry's names. */
static int suite_name(const char *s)
{
    size_t i;
    if (!*s || *s == '-' || strlen(s) > 64)
        return 0;
    for (i = 0; s[i]; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '-'))
            return 0;
    return 1;
}

/** Say whether a suite of the map holds `id` already. */
static int mapped(const struct lines *l, const char *id)
{
    size_t i, j;
    for (i = 0; i < l->nsuites; i++)
        for (j = 0; j < l->suites[i].nids; j++)
            if (!strcmp(l->suites[i].ids[j], id))
                return 1;
    return 0;
}

/**
 * Read one line of the map, a suite's name and the cases and families it
 * holds, separated by spaces.
 *
 * @return 0, or -1 for a line that is not one, or that names a suite or an
 *         identity again.
 */
static int map_line(struct lines *l, char *line)
{
    struct suite *s, *more;
    char *word = strtok(line, " \t");
    size_t i;
    if (!word || !suite_name(word))
        return -1;
    for (i = 0; i < l->nsuites; i++)
        if (!strcmp(l->suites[i].name, word))
            return -1;
    more = realloc(l->suites, (l->nsuites + 1) * sizeof(*more));
    if (!more)
        return -1;
    l->suites = more;
    s = &l->suites[l->nsuites];
    memset(s, 0, sizeof(*s));
    s->line = l->nsuites++;
    if (!(s->name = copy(word)))
        return -1;
    while ((word = strtok(NULL, " \t")) != NULL) {
        char **ids = realloc(s->ids, (s->nids + 1) * sizeof(*ids));
        if (!ids)
            return -1;
        s->ids = ids;
        if (!eh_identity(word) || mapped(l, word) || !(s->ids[s->nids] = copy(word)))
            return -1;
        s->nids++;
    }
    return s->nids ? 0 : -1;
}

/** Read the suite map at `path`; 0, or -1 after a message. */
static int read_map(struct lines *l, const char *path)
{
    char line[MAP_LINE];
    unsigned n = 0;
    int bad = 0;
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "etude show: %s: cannot be read\n", path);
        return -1;
    }
    while (!bad && fgets(line, sizeof(line), f)) {
        n++;
        if (!strchr(line, '\n') && !feof(f))
            bad = 1;
        line[strcspn(line, "\n")] = 0;
        if (!bad && line[0] && line[0] != '#')
            bad = map_line(l, line) != 0;
    }
    if (ferror(f) && !bad) {
        bad = 1;
        n = 0;
    }
    fclose(f);
    if (bad)
        fprintf(stderr, "etude show: %s:%u: not a suite's name and its cases\n", path, n);
    else if (!l->nsuites)
        fprintf(stderr, "etude show: %s: names no suite\n", path);
    return bad || !l->nsuites ? -1 : 0;
}

/** Say whether suite `si` is the runner's own, `runner`, which a fault of
 * the runner fails as well. */
static int runner_suite(const struct lines *l, size_t si)
{
    return !strcmp(l->suites[si].name, "runner");
}

/**
 * In a run of two architectures, give each suite of the map but the
 * runner's a suite on each architecture, `ARCH:SUITE`, in the order of
 * `l->arches`, with the cases and families of its line.
 *
 * @return 0, or -1 when memory ran out.
 */
static int split(struct lines *l)
{
    struct suite *more, *s;
    size_t i, a, j, n = 0, k = 0, size;
    int bad = 0;
    if (l->narches < 2)
        return 0;
    for (i = 0; i < l->nsuites; i++)
        n += runner_suite(l, i) ? 1 : l->narches;
    if (!(more = calloc(n, sizeof(*more))))
        return -1;
    for (i = 0; i < l->nsuites; i++)
        for (a = 0; a < (runner_suite(l, i) ? 1 : l->narches); a++) {
            s = &more[k++];
            s->line = l->suites[i].line;
            s->arch = runner_suite(l, i) ? NULL : l->arches[a];
            size = strlen(l->suites[i].name) + 8;
            s->name = malloc(size);
            s->ids = calloc(l->suites[i].nids, sizeof(*s->ids));
            if (!s->name || !s->ids) {
                bad = 1;
                continue;
            }
            snprintf(s->name, size, "%s%s%s", s->arch ? s->arch : "", s->arch ? ":" : "",
                     l->suites[i].name);
            for (j = 0; j < l->suites[i].nids; j++)
                if ((s->ids[s->nids] = copy(l->suites[i].ids[j])))
                    s->nids++;
            bad |= s->nids < l->suites[i].nids;
        }
    for (i = 0; i < l->nsuites; i++) {
        for (j = 0; j < l->suites[i].nids; j++)
            free(l->suites[i].ids[j]);
        free(l->suites[i].ids);
        free(l->suites[i].name);
    }
    free(l->suites);
    l->suites = more;
    l->nsuites = n;
    return bad ? -1 : 0;
}

/** Keep a plan, attempt or run record of the run that the streams hold. */
static int visit(const char *line, void *context)
{
    struct lines *l = context;
    struct eh_json *doc = eh_json_parse(line);
    const char *run, *kind;
    if (!doc) {
        l->nomem = 1;
        return 1;
    }
    run = text_of(doc, "run_id");
    kind = text_of(doc, "kind");
    if (!l->run_id && !(l->run_id = copy(run))) {
        eh_json_free(doc);
        l->nomem = 1;
        return 1;
    }
    if (strcmp(run, l->run_id)) {
        if (l->foreign != l->stream)
            fault(l, "%s: the record %s is of the run %s, not %s", l->stream,
                  text_of(doc, "record_id"), run, l->run_id);
        l->foreign = l->stream;
    } else if (!strcmp(kind, "plan") || !strcmp(kind, "attempt") || !strcmp(kind, "run")) {
        if (l->nrecs == l->caprecs) {
            size_t cap = l->caprecs ? 2 * l->caprecs : 64;
            struct record *more = realloc(l->recs, cap * sizeof(*more));
            if (!more) {
                eh_json_free(doc);
                l->nomem = 1;
                return 1;
            }
            l->recs = more;
            l->caprecs = cap;
        }
        l->recs[l->nrecs].doc = doc;
        l->recs[l->nrecs].id = text_of(doc, "record_id");
        l->recs[l->nrecs].kind = kind;
        l->recs[l->nrecs].data = eh_json_get(doc, "data");
        l->recs[l->nrecs++].listed = 0;
        return 0;
    }
    eh_json_free(doc);
    return 0;
}

/** Read every stream; a stream that is not whole is a fault of the runner. */
static void load(struct lines *l, char **streams, int count)
{
    int i, rc;
    uint64_t bad;
    for (i = 0; i < count && !l->nomem; i++) {
        l->stream = streams[i];
        rc = etude_history_read(streams[i], l->schema, visit, l, &bad);
        if (rc == 1)
            fault(l, "%s: its last record was cut short", streams[i]);
        else if (rc < 0 && !l->nomem && bad)
            fault(l, "%s: line %llu is not a record of the history", streams[i],
                  (unsigned long long)bad);
        else if (rc < 0 && !l->nomem)
            fault(l, "%s: cannot be read as a history stream", streams[i]);
    }
}

/** Find the record of `kind` with identity `id`, or NULL. */
static struct record *find_record(struct lines *l, const char *kind, const char *id)
{
    size_t i;
    for (i = 0; i < l->nrecs; i++)
        if (!strcmp(l->recs[i].kind, kind) && !strcmp(l->recs[i].id, id))
            return &l->recs[i];
    return NULL;
}

/** Find the planned item `id`, or NULL. */
static struct item *find_item(struct lines *l, const char *id)
{
    size_t i;
    for (i = 0; i < l->nitems; i++)
        if (!strcmp(l->items[i].id, id))
            return &l->items[i];
    return NULL;
}

/** Write an item's name for a line: its case, architecture and repetition. */
static const char *label(const struct item *it)
{
    static char out[600];
    int n = snprintf(out, sizeof(out), "%s", it->case_id);
    if (strcmp(it->arch, "none") && n > 0 && (size_t)n < sizeof(out))
        n += snprintf(out + n, sizeof(out) - (size_t)n, " on %s", it->arch);
    if (it->repetition && n > 0 && (size_t)n < sizeof(out))
        snprintf(out + n, sizeof(out) - (size_t)n, " repetition %lld", (long long)it->repetition);
    return out;
}

/** Say whether suite `s` may hold item `it` by their architectures: an item
 * of no architecture goes to the suite on the run's first. */
static int same_arch(const struct lines *l, const struct suite *s, const struct item *it)
{
    if (!s->arch)
        return 1;
    if (!strcmp(it->arch, "none"))
        return s->arch == l->arches[0];
    return !strcmp(it->arch, s->arch);
}

/** Give an item to the one suite of the map that holds its case or family. */
static void assign(struct lines *l, struct item *it)
{
    size_t i, j;
    it->suite = -1;
    for (i = 0; i < l->nsuites; i++) {
        if (!same_arch(l, &l->suites[i], it))
            continue;
        for (j = 0; j < l->suites[i].nids; j++)
            if (!strcmp(l->suites[i].ids[j], it->case_id) ||
                !strcmp(l->suites[i].ids[j], it->family_id))
                break;
        if (j == l->suites[i].nids)
            continue;
        if (it->suite < 0)
            it->suite = (int)i;
        else
            fault(l, "%s is in both suites %s and %s", label(it), l->suites[it->suite].name,
                  l->suites[i].name);
    }
    if (it->suite < 0)
        fault(l, "%s is in no suite of the map", label(it));
    else
        l->suites[it->suite].items++;
}

/** Say whether the line of the map that gave suite `si` gave a suite with an
 * item on another architecture. */
static int other_arch(const struct lines *l, size_t si)
{
    size_t i;
    for (i = 0; l->suites[si].arch && i < l->nsuites; i++)
        if (i != si && l->suites[i].line == l->suites[si].line && l->suites[i].items)
            return 1;
    return 0;
}

/** Take the run's one plan and its items; 0, or -1 when there is none. */
static int take_plan(struct lines *l)
{
    const struct eh_json *items, *v;
    size_t i, plans = 0;
    for (i = 0; i < l->nrecs; i++)
        if (!strcmp(l->recs[i].kind, "plan") && plans++ == 0)
            l->plan = &l->recs[i];
    if (plans != 1) {
        fault(l, plans ? "the history has %lu plan records" : "the history has no plan record",
              (unsigned long)plans);
        l->plan = NULL;
        return -1;
    }
    items = eh_json_get(l->plan->data, "items");
    for (v = items ? items->child : NULL; v; v = v->next)
        l->nitems++;
    if (!(l->items = calloc(l->nitems ? l->nitems : 1, sizeof(*l->items)))) {
        l->nomem = 1;
        return -1;
    }
    for (i = 0, v = items ? items->child : NULL; v; v = v->next, i++) {
        struct item *it = &l->items[i];
        const struct eh_json *repetition = eh_json_get(v, "repetition");
        it->id = text_of(v, "item_id");
        it->case_id = text_of(v, "case_id");
        it->family_id = text_of(v, "family_id");
        it->arch = text_of(v, "architecture");
        it->repetition = repetition ? repetition->number : 0;
        it->attempt_ids = eh_json_get(v, "attempt_ids");
        assign(l, it);
    }
    for (i = 0; i < l->nsuites; i++)
        if (!l->suites[i].items && !(l->suites[i].dropped = other_arch(l, i)))
            fault(l, "the plan has no case of the suite %s", l->suites[i].name);
    return 0;
}

/** Say whether two clocks of the history are one: machine, boot and clock. */
static int same_clock(const struct eh_json *a, const struct eh_json *b)
{
    return !strcmp(text_of(a, "machine_id"), text_of(b, "machine_id")) &&
           !strcmp(text_of(a, "boot_id"), text_of(b, "boot_id")) &&
           !strcmp(text_of(a, "clock_id"), text_of(b, "clock_id"));
}

/** Add an attempt's interval to its suite's span. */
static void timed(struct suite *s, const struct eh_json *time)
{
    const struct eh_json *clock = eh_json_get(time, "clock");
    const struct eh_json *start = eh_json_get(time, "start_us"), *end = eh_json_get(time, "end_us");
    if (!clock || !start || start->type != 'n' || !end || end->type != 'n') {
        s->untimed = 1;
    } else if (!s->clock) {
        s->clock = clock;
        s->start_us = (uint64_t)start->number;
        s->end_us = (uint64_t)end->number;
    } else if (!same_clock(s->clock, clock)) {
        s->untimed = 1;
    } else {
        if ((uint64_t)start->number < s->start_us)
            s->start_us = (uint64_t)start->number;
        if ((uint64_t)end->number > s->end_us)
            s->end_us = (uint64_t)end->number;
    }
}

/** Give the rank of an outcome's name. */
static enum rank rank_of(const char *name)
{
    int i;
    for (i = R_FAILURE; i > R_PASS; i--)
        if (!strcmp(name, rank_names[i]))
            break;
    return (enum rank)i;
}

/**
 * Count attempt record `r` for item `it`. A failure is the item's own; so is
 * an attempt that did not finish under the conditions its case declared. One
 * that did not finish because something outside the case disrupted it is a
 * fault of the runner.
 */
static void note(struct lines *l, struct item *it, const struct record *r)
{
    const struct eh_json *result = eh_json_get(r->data, "result");
    const struct eh_json *reason = eh_json_get(result, "reason");
    const struct eh_json *disruptions = eh_json_get(result, "disruptions");
    enum rank outcome = rank_of(text_of(result, "outcome"));
    int disrupted = disruptions && disruptions->child && disruptions->child->type == 's';
    if (!it->attempts++ || outcome > it->worst) {
        it->worst = outcome;
        it->reason = reason && reason->type == 's' ? reason->string : NULL;
    }
    if (outcome == R_FAILURE || (outcome == R_INCOMPLETE && !disrupted))
        it->own = 1;
    else if (outcome == R_INCOMPLETE)
        fault(l, "%s: the attempt %s was disrupted: %s", label(it), text_of(r->data, "attempt_id"),
              disruptions->child->string);
    if (it->suite >= 0)
        timed(&l->suites[it->suite], eh_json_get(r->data, "time"));
}

/** Say whether the plan gave item `it` the attempt identity `id`. */
static int planned_attempt(const struct item *it, const char *id)
{
    const struct eh_json *v;
    for (v = it->attempt_ids ? it->attempt_ids->child : NULL; v; v = v->next)
        if (v->type == 's' && !strcmp(v->string, id))
            return 1;
    return 0;
}

/** Count each attempt record of the plan for its item. */
static void take_attempts(struct lines *l)
{
    size_t i;
    for (i = 0; i < l->nrecs; i++) {
        const struct record *r = &l->recs[i];
        struct item *it;
        if (strcmp(r->kind, "attempt"))
            continue;
        if (strcmp(text_of(r->data, "plan_id"), l->plan->id)) {
            fault(l, "the attempt record %s is of the plan %s, not of this run's", r->id,
                  text_of(r->data, "plan_id"));
        } else if (!(it = find_item(l, text_of(r->data, "item_id")))) {
            fault(l, "the attempt record %s is of the item %s, which the plan lacks", r->id,
                  text_of(r->data, "item_id"));
        } else if (!planned_attempt(it, text_of(r->data, "attempt_id"))) {
            fault(l, "%s: the attempt %s is not one the plan gave it", label(it),
                  text_of(r->data, "attempt_id"));
        } else {
            note(l, it, r);
        }
    }
}

/** Check one item of the run record against the attempt records it lists. */
static void summary_item(struct lines *l, const struct eh_json *s)
{
    const struct eh_json *records = eh_json_get(s, "attempt_records"), *v;
    struct item *it = find_item(l, text_of(s, "item_id"));
    if (!it) {
        fault(l, "the run record lists the item %s, which the plan lacks", text_of(s, "item_id"));
        return;
    }
    it->listed++;
    it->summary = text_of(s, "outcome");
    for (v = records ? records->child : NULL; v; v = v->next) {
        struct record *r = find_record(l, "attempt", v->type == 's' ? v->string : "");
        if (!r || strcmp(text_of(r->data, "item_id"), it->id))
            fault(l, "%s: the run record lists the attempt record %s, which the history %s",
                  label(it), v->type == 's' ? v->string : "?",
                  r ? "has for another item" : "lacks");
        else
            r->listed = 1;
    }
}

/** Take the run's one run record, which ends a run that finished. */
static void take_run(struct lines *l)
{
    const struct eh_json *items, *v;
    size_t i, runs = 0;
    for (i = 0; i < l->nrecs; i++) {
        const struct record *r = &l->recs[i];
        if (strcmp(r->kind, "run"))
            continue;
        if (strcmp(text_of(r->data, "plan_id"), l->plan->id))
            fault(l, "the run record %s is of the plan %s, not of this run's", r->id,
                  text_of(r->data, "plan_id"));
        else if (runs++ == 0)
            l->run = r;
    }
    if (runs != 1) {
        fault(l,
              runs ? "the history has %lu run records"
                   : "the history has no run record: the run did not finish",
              (unsigned long)runs);
        l->run = NULL;
        return;
    }
    items = eh_json_get(l->run->data, "items");
    for (v = items ? items->child : NULL; v; v = v->next)
        summary_item(l, v);
    for (i = 0; i < l->nrecs; i++)
        if (!strcmp(l->recs[i].kind, "attempt") && !l->recs[i].listed &&
            !strcmp(text_of(l->recs[i].data, "plan_id"), l->plan->id) &&
            find_item(l, text_of(l->recs[i].data, "item_id")))
            fault(l, "the run record lists no item with the attempt record %s", l->recs[i].id);
}

/** Give an item's outcome, and check it against the run record's. */
static enum rank judge(struct lines *l, struct item *it)
{
    enum rank r = it->worst;
    if (!it->attempts)
        r = it->summary && !strcmp(it->summary, "cancelled") ? R_CANCELLED : R_INCOMPLETE;
    if (!it->attempts && r == R_INCOMPLETE)
        fault(l, "%s: no attempt of it ran", label(it));
    if (l->run && !it->listed)
        fault(l, "the run record lacks %s", label(it));
    else if (l->run && it->listed > 1)
        fault(l, "the run record lists %s %u times", label(it), it->listed);
    else if (l->run && it->attempts && strcmp(it->summary, rank_names[r]))
        fault(l, "the run record gives %s the outcome %s; its attempts give %s", label(it),
              it->summary, rank_names[r]);
    else if (l->run && !it->attempts && strcmp(it->summary, rank_names[r]))
        fault(l, "the run record gives %s, which has no attempt, the outcome %s", label(it),
              it->summary);
    it->rank = r;
    return r;
}

/** Judge every item into its suite; give the heaviest outcome of them all. */
static enum rank judge_all(struct lines *l)
{
    enum rank worst = R_PASS;
    size_t i;
    for (i = 0; i < l->nitems; i++) {
        struct item *it = &l->items[i];
        enum rank r = judge(l, it);
        if (r > worst)
            worst = r;
        if (it->suite < 0)
            continue;
        if (r > l->suites[it->suite].worst)
            l->suites[it->suite].worst = r;
        if (r != R_PASS && it->own)
            l->suites[it->suite].own = 1;
    }
    return worst;
}

/** Check the run's own result against its items'. */
static void check_run(struct lines *l, enum rank worst)
{
    const struct eh_json *result;
    const char *outcome;
    if (!l->run)
        return;
    result = eh_json_get(l->run->data, "result");
    outcome = text_of(result, "outcome");
    if (worst == R_PASS && strcmp(outcome, "pass"))
        fault(l, "the run did not pass, though each item passed: %s", text_of(result, "reason"));
    else if (worst != R_PASS && !strcmp(outcome, "pass"))
        fault(l, "the run record gives the run a pass, though an item did not pass");
}

/** Write a duration rounded up to whole seconds, or `?s` when unknown. */
static const char *seconds(int known, uint64_t us)
{
    static char out[32];
    if (known)
        snprintf(out, sizeof(out), "%llus", (unsigned long long)((us + 999999) / 1000000));
    else
        snprintf(out, sizeof(out), "?s");
    return out;
}

/** Say whether the map has the runner's own suite. */
static int has_runner_suite(const struct lines *l)
{
    size_t i;
    for (i = 0; i < l->nsuites; i++)
        if (runner_suite(l, i))
            return 1;
    return 0;
}

/** Print a suite's items that did not pass, and its result line. */
static void print_suite(const struct lines *l, size_t si)
{
    const struct suite *s = &l->suites[si];
    enum rank word = s->items ? s->worst : R_INCOMPLETE;
    size_t i;
    if (l->nfaults && runner_suite(l, si))
        word = R_FAILURE;
    for (i = 0; i < l->nitems; i++) {
        const struct item *it = &l->items[i];
        if (it->suite == (int)si && it->rank != R_PASS)
            say(stderr, "run: %s: %s: %s: %s", s->name, label(it), rank_names[it->rank],
                it->attempts && it->reason ? it->reason : "no attempt of it ran");
    }
    say(word == R_PASS ? stdout : stderr, "run: %s: %s in %s", s->name, suite_words[word],
        seconds(s->clock && !s->untimed, s->end_us - s->start_us));
}

/** Print the runner's faults and its result line. */
static void print_runner(const struct lines *l)
{
    const struct eh_json *elapsed = NULL, *value = NULL;
    size_t i;
    for (i = 0; i < l->nfaults; i++)
        say(stderr, "run: runner: %s", l->faults[i]);
    if (has_runner_suite(l))
        return;
    if (l->run)
        elapsed = eh_json_get(eh_json_get(l->run->data, "time"), "elapsed_us");
    if (elapsed)
        value = eh_json_get(elapsed, "value");
    say(stderr, "run: runner: FAIL in %s",
        seconds(value && value->type == 'n', value ? (uint64_t)value->number : 0));
}

/** Print the suites that failed or did not finish by their own cases. */
static void print_failed(const struct lines *l)
{
    size_t i, used = 0, size = (l->nsuites + l->nextra) * 74 + 8;
    char *names = malloc(size);
    if (!names) {
        say(stderr, "run: result: failed: runner");
        return;
    }
    names[0] = 0;
    for (i = 0; i < l->nsuites; i++)
        if (l->suites[i].own || (l->nfaults && runner_suite(l, i)))
            used += (size_t)snprintf(names + used, size - used, "%s%s", used ? " " : "",
                                     l->suites[i].name);
    for (i = 0; i < l->nextra; i++)
        used += (size_t)snprintf(names + used, size - used, "%s%s", used ? " " : "", l->extra[i]);
    if (l->nfaults && !has_runner_suite(l))
        snprintf(names + used, size - used, "%srunner", used ? " " : "");
    if (names[0])
        say(stderr, "run: result: failed: %s", names);
    free(names);
}

/** Print every line; give the exit status. */
static int print(struct lines *l, enum rank worst)
{
    size_t i, n = 0;
    for (i = 0; i < l->nsuites; i++)
        if (!l->suites[i].dropped) {
            print_suite(l, i);
            n++;
        }
    if (l->nfaults)
        print_runner(l);
    print_failed(l);
    if (worst == R_FAILURE || l->nextra)
        say(stdout, "run: result: failed");
    else if (worst == R_INCOMPLETE || l->nfaults)
        say(stdout, "run: result: incomplete");
    else if (worst == R_CANCELLED)
        say(stdout, "run: result: interrupted");
    else
        say(stdout, "run: result: all %lu suites passed", (unsigned long)n);
    return worst == R_PASS && !l->nfaults && !l->nextra ? 0 : 1;
}

/** Free what a reading holds. */
static void release(struct lines *l)
{
    size_t i, j;
    for (i = 0; i < l->nrecs; i++)
        eh_json_free(l->recs[i].doc);
    for (i = 0; i < l->nfaults; i++)
        free(l->faults[i]);
    for (i = 0; i < l->nsuites; i++) {
        for (j = 0; j < l->suites[i].nids; j++)
            free(l->suites[i].ids[j]);
        free(l->suites[i].ids);
        free(l->suites[i].name);
    }
    free(l->recs);
    free(l->faults);
    free(l->suites);
    free(l->items);
    free(l->run_id);
}

int etude_lines_main(int argc, char **argv)
{
    struct lines l;
    char schema[4200];
    const char *schemas = "tools/etude/schemas";
    enum rank worst = R_INCOMPLETE;
    int i = 1, status = 2;
    memset(&l, 0, sizeof(l));
    while (i + 1 < argc &&
           (!strcmp(argv[i], "-s") || !strcmp(argv[i], "-f") || !strcmp(argv[i], "-a"))) {
        if (argv[i][1] == 's')
            schemas = argv[i + 1];
        else if (argv[i][1] == 'a' && !strcmp(argv[i + 1], "i686,x86_64")) {
            l.arches[0] = "i686";
            l.arches[1] = "x86_64";
            l.narches = 2;
        } else if (argv[i][1] == 'a') {
            if (strcmp(argv[i + 1], "i686") && strcmp(argv[i + 1], "x86_64"))
                break;
        } else if (l.nextra < EXTRA_MAX && suite_name(argv[i + 1]))
            l.extra[l.nextra++] = argv[i + 1];
        else
            break;
        i += 2;
    }
    if (argc - i < 2 || argv[i][0] == '-') {
        fputs(usage_text, stderr);
        return 2;
    }
    snprintf(schema, sizeof(schema), "%s/history.schema.json", schemas);
    l.schema = schema;
    if (read_map(&l, argv[i]) == 0) {
        if (split(&l) != 0)
            l.nomem = 1;
        else
            load(&l, argv + i + 1, argc - i - 1);
        if (!l.nomem && take_plan(&l) == 0) {
            take_attempts(&l);
            take_run(&l);
            worst = judge_all(&l);
            check_run(&l, worst);
        }
        if (l.nomem)
            fprintf(stderr, "etude show: memory ran out\n");
        else
            status = print(&l, worst);
    }
    release(&l);
    return status;
}
