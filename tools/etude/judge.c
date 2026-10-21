/*
 * judge.c: casecheck's judgement of a program's report records, its check
 * of an attempt's results document against the case, and the results
 * document it writes for an attempt from the report and the program's end.
 *
 * All follow the Readme's Report records and Results: an assertion that
 * nothing reported is missing, one the program could not evaluate is not
 * run, and neither is ever a pass. A report that a program wrote is judged
 * here as an executor will judge it; a results document is checked for the
 * assertions it must give, once each, and for an outcome that follows from
 * them, so that a document which leaves an assertion out, or a history that
 * calls a skipped assertion passed, is refused. A document written here is
 * given that check before it is printed.
 */
#include "casecheck.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "etude/processor.h"
#include "schema.h"

/* The pattern of an identity of a manifest. */
#define ID_PATTERN "^[a-z0-9][a-z0-9_-]*(\\.[a-z0-9][a-z0-9_-]*)*$"

/* The most records a report may have, and the longest record. */
#define RECORDS_MAX    4096
#define LINE_MAX_BYTES 255

static const char *const OUTCOMES[] = {"pass", "fail", "not_run", "missing"};
static const char *const ATTEMPTS[] = {"pass", "failure", "incomplete"};

enum verb {
    VERB_BEGIN,
    VERB_ASSERT,
    VERB_NOTRUN,
    VERB_BRANCH,
    VERB_PROGRESS,
    VERB_CHILD,
    VERB_END
};

/* One record of a report, as parsed. */
struct rec {
    enum verb verb;
    int line;
    char id[132];     /* the case, assertion, group or witness */
    char alt[132];    /* a branch's alternative */
    char text[256];   /* a failure's detail or a skip's reason */
    long long number; /* progress units, a child's PID or the end's count */
    int failed;       /* an assert record of a failure */
};

/* A report being judged against its case. */
struct judging {
    const struct json *c;
    const char *input;
    struct rec *rec;
    size_t n;
    int errors;
    int has_end;
    int parse_errors;
    int prior;               /* the errors before the missing end's */
    const char *decided[32]; /* each group's alternative, or NULL */
    const char *how[32];     /* "feature" or "report", or NULL for neither */
    int reported[32];        /* the group's branch record was read */
    struct jbuf *texts;      /* an open JSON array of the errors, or NULL */
    const char *console;     /* the machine's console, or NULL for none */
    size_t console_len;
};

static int same(const char *a, const char *b)
{
    return a != NULL && b != NULL && strcmp(a, b) == 0;
}

/**
 * Appends `s` to `b` as a JSON string of printable ASCII, a `text` of the
 * results schema: a byte outside that range becomes `?`.
 */
static void put_text(struct jbuf *b, const char *s)
{
    jbuf_puts(b, "\"");
    for (; *s != '\0'; s++) {
        char c = *s < 0x20 || *s > 0x7e ? '?' : *s;

        if (c == '"' || c == '\\') {
            jbuf_put(b, "\\", 1);
        }
        jbuf_put(b, &c, 1);
    }
    jbuf_puts(b, "\"");
}

/** Appends a comma to the open array `b` unless it has no item yet. */
static void put_comma(struct jbuf *b)
{
    if (b->len > 0 && b->data[b->len - 1] != '[') {
        jbuf_puts(b, ",");
    }
}

/**
 * Reports an error of the report at `line`, which makes it no longer complete.
 */
static void report_error(struct judging *j, int line, const char *fmt, ...)
{
    char what[400];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(what, sizeof what, fmt, ap);
    va_end(ap);
    finding(j->input, line, "%s", what);
    if (j->texts != NULL) {
        char text[440];

        if (line > 0) {
            snprintf(text, sizeof text, "line %d: %s", line, what);
        } else {
            snprintf(text, sizeof text, "%s", what);
        }
        put_comma(j->texts);
        put_text(j->texts, text);
    }
    j->errors++;
}

/* ---- parsing ---- */

/**
 * Takes the next field of `*p`, separated by one space; returns NULL at the end
 * or a fault.
 */
static char *field(char **p)
{
    char *start = *p, *space;

    if (*start == '\0' || *start == ' ') {
        return NULL;
    }
    space = strchr(start, ' ');
    if (space == NULL) {
        *p = start + strlen(start);
    } else {
        *space = '\0';
        *p = space + 1;
    }
    return start;
}

/** Reads a decimal count of at most 18 digits; returns -1 for anything else. */
static long long count_of(const char *s)
{
    long long v = 0;
    int n = 0;

    if (s == NULL || *s == '\0') {
        return -1;
    }
    for (; *s != '\0'; s++, n++) {
        if (*s < '0' || *s > '9' || n >= 18) {
            return -1;
        }
        v = v * 10 + (*s - '0');
    }
    return v;
}

/** Parses an identity field into `out`; returns 0 or -1. */
static int take_id(char **p, char *out, size_t n)
{
    const char *f = field(p);

    if (f == NULL || strlen(f) >= n || !schema_pattern("^[!-~]{1,128}$", f)) {
        return -1;
    }
    snprintf(out, n, "%s", f);
    return 0;
}

/** Parses the fields of an assert record. */
static int parse_assert(char *p, struct rec *r)
{
    const char *verdict;

    if (take_id(&p, r->id, sizeof r->id) != 0 || (verdict = field(&p)) == NULL) {
        return -1;
    }
    if (same(verdict, "pass")) {
        return *p == '\0' ? 0 : -1;
    }
    if (!same(verdict, "fail") || *p == '\0' || p[strlen(p) - 1] == ' ' || *p == ' ') {
        return -1;
    }
    r->failed = 1;
    snprintf(r->text, sizeof r->text, "%s", p);
    return 0;
}

/** Parses the fields after the verb of the record `r`; returns 0 or -1. */
static int parse_fields(char *p, struct rec *r)
{
    switch (r->verb) {
    case VERB_BEGIN:
        return take_id(&p, r->id, sizeof r->id) == 0 && *p == '\0' ? 0 : -1;
    case VERB_ASSERT:
        return parse_assert(p, r);
    case VERB_NOTRUN:
        if (take_id(&p, r->id, sizeof r->id) != 0 || *p == '\0' || *p == ' ' ||
            p[strlen(p) - 1] == ' ') {
            return -1;
        }
        snprintf(r->text, sizeof r->text, "%s", p);
        return 0;
    case VERB_BRANCH:
        return take_id(&p, r->id, sizeof r->id) == 0 && take_id(&p, r->alt, sizeof r->alt) == 0 &&
                       *p == '\0'
                   ? 0
                   : -1;
    case VERB_PROGRESS:
        if (take_id(&p, r->id, sizeof r->id) != 0) {
            return -1;
        }
        /* fall through */
    default:
        r->number = count_of(field(&p));
        return r->number >= 0 && *p == '\0' ? 0 : -1;
    }
}

/** Parses the record of `line`, after its prefix, into `r`; returns 0 or -1. */
static int parse_record(char *line, struct rec *r)
{
    static const char *const verbs[] = {"begin",    "assert", "notrun", "branch",
                                        "progress", "child",  "end"};
    char *p = line;
    const char *verb = field(&p);
    size_t i;

    for (i = 0; verb != NULL && i < sizeof verbs / sizeof verbs[0]; i++) {
        if (strcmp(verb, verbs[i]) == 0) {
            r->verb = (enum verb)i;
            return parse_fields(p, r);
        }
    }
    return -1;
}

/** Returns 1 when every byte of `s` is printable ASCII. */
static int printable(const char *s)
{
    for (; *s != '\0'; s++) {
        if ((unsigned char)*s < 0x20 || (unsigned char)*s > 0x7e) {
            return 0;
        }
    }
    return 1;
}

/**
 * Reads the records among the lines of `text`, the rest being the program's own
 * output.
 */
static void read_records(struct judging *j, char *text)
{
    static const char prefix[] = "@etude 1 ";
    char *line = text, *next;
    int number = 1;

    for (; line != NULL && *line != '\0'; line = next, number++) {
        size_t len;

        next = strchr(line, '\n');
        len = next != NULL ? (size_t)(next - line) + 1 : strlen(line) + 1;
        if (next != NULL) {
            *next++ = '\0';
        }
        if (len > 1 && line[len - 2] == '\r') {
            line[len - 2] = '\0';
        }
        if (strncmp(line, "@etude", 6) != 0) {
            continue;
        }
        if (len > LINE_MAX_BYTES) {
            report_error(j, number, "a record is longer than 255 bytes");
            j->parse_errors++;
        } else if (strncmp(line, prefix, sizeof prefix - 1) != 0 || !printable(line) ||
                   j->n >= RECORDS_MAX ||
                   parse_record(line + sizeof prefix - 1, &j->rec[j->n]) != 0) {
            report_error(j, number, "the record \"%s\" does not parse", line);
            j->parse_errors++;
        } else {
            j->rec[j->n++].line = number;
        }
    }
}

/* ---- judging ---- */

/** Returns the declared assertion `id` of the case, or NULL. */
static const struct json *declared(const struct json *c, const char *id)
{
    const struct json *list = json_member(c, "assertions");
    size_t i;

    for (i = 0; i < list->count; i++) {
        if (same(json_text(list->item[i], "assertion_id"), id)) {
            return list->item[i];
        }
    }
    return NULL;
}

/** Returns the index of the group `id` of the case, or -1. */
static int group_index(const struct json *c, const char *id)
{
    const struct json *groups = json_member(c, "branches");
    size_t i;

    for (i = 0; i < groups->count && i < 32; i++) {
        if (same(json_text(groups->item[i], "group_id"), id)) {
            return (int)i;
        }
    }
    return -1;
}

/**
 * Decides the groups decided by features from the processor profile `p`, or
 * without one from `features`, a list of `NAME=present` and `NAME=absent`;
 * returns 0, or -1 when a feature a group names is not given.
 */
static int decide_features(struct judging *j, const struct etude_processor *p,
                           char *const *features, int n)
{
    const struct json *groups = json_member(j->c, "branches");
    size_t g, f;
    int i;

    for (g = 0; g < groups->count && g < 32; g++) {
        const struct json *names =
            json_member(json_member(groups->item[g], "decided_by"), "features");
        const char *when = "absent";

        for (f = 0; names != NULL && f < names->count; f++) {
            size_t len = strlen(names->item[f]->string);
            const char *state = NULL;

            if (p != NULL) {
                enum etude_feature_state has = etude_processor_feature(p, names->item[f]->string);

                if (has == ETUDE_FEATURE_UNDECLARED) {
                    finding(NULL, 0, "%s: the processor profile %s does not declare %s",
                            json_text(j->c, "case_id"), p->name, names->item[f]->string);
                    return -1;
                }
                state = has == ETUDE_FEATURE_PRESENT ? "present" : "absent";
            }
            for (i = 0; p == NULL && i < n; i++) {
                if (strncmp(features[i], names->item[f]->string, len) == 0 &&
                    features[i][len] == '=') {
                    state = features[i] + len + 1;
                }
            }
            if (state == NULL || (!same(state, "present") && !same(state, "absent"))) {
                finding(NULL, 0, "%s: give the processor's %s as -f %s=present or =absent",
                        json_text(j->c, "case_id"), names->item[f]->string, names->item[f]->string);
                return -1;
            }
            when = same(state, "present") ? "present" : when;
        }
        if (names != NULL) {
            const struct json *alts = json_member(groups->item[g], "alternatives");
            size_t a;

            for (a = 0; a < alts->count; a++) {
                if (same(json_text(alts->item[a], "when"), when)) {
                    j->decided[g] = json_text(alts->item[a], "alternative_id");
                }
            }
            j->how[g] = "feature";
        }
    }
    return 0;
}

/** Judges a branch record against the group it names. */
static void judge_branch(struct judging *j, const struct rec *r)
{
    int g = group_index(j->c, r->id);
    const struct json *group;

    if (g < 0) {
        report_error(j, r->line, "the branch group %s is not the case's", r->id);
        return;
    }
    group = json_member(j->c, "branches")->item[g];
    if (!has_member(json_member(group, "alternatives"), "alternative_id", r->alt)) {
        report_error(j, r->line, "the group %s has no alternative %s", r->id, r->alt);
    } else if (j->reported[g]) {
        report_error(j, r->line, "the branch of the group %s is reported twice", r->id);
    } else if (same(j->how[g], "feature") && !same(j->decided[g], r->alt)) {
        report_error(j, r->line, "the program took %s where the processor gives %s", r->alt,
                     j->decided[g]);
    } else if (j->how[g] == NULL) {
        const struct json *alts = json_member(group, "alternatives");
        size_t a;

        for (a = 0; a < alts->count; a++) {
            if (same(json_text(alts->item[a], "alternative_id"), r->alt)) {
                j->decided[g] = json_text(alts->item[a], "alternative_id");
            }
        }
        j->how[g] = "report";
    }
    j->reported[g] = 1;
}

/** Judges a progress record against its witness and the units before it. */
static void judge_progress(struct judging *j, const struct rec *r, size_t at)
{
    const struct json *list = json_member(j->c, "progress");
    const struct json *w = NULL;
    const struct json *limit;
    char units[32];
    size_t i;

    for (i = 0; i < list->count; i++) {
        if (same(json_text(list->item[i], "witness_id"), r->id)) {
            w = list->item[i];
        }
    }
    if (w == NULL) {
        report_error(j, r->line, "the witness %s is not the case's", r->id);
        return;
    }
    for (i = at; i-- > 0;) {
        if (j->rec[i].verb == VERB_PROGRESS && same(j->rec[i].id, r->id)) {
            if (r->number < j->rec[i].number) {
                report_error(j, r->line, "the witness %s goes back", r->id);
            }
            break;
        }
    }
    limit = json_member(w, "limit");
    snprintf(units, sizeof units, "%lld", r->number);
    if (limit->type == JSON_INTEGER && r->number > limit->integer) {
        report_error(j, r->line, "the witness %s passes its limit with %s units", r->id, units);
    }
}

/** Judges an assert or notrun record against the declared assertions. */
static void judge_assertion(struct judging *j, const struct rec *r, size_t at)
{
    const struct json *a = declared(j->c, r->id);
    size_t i;

    if (a == NULL) {
        report_error(j, r->line, "%s is not an assertion of the case", r->id);
        return;
    }
    if (!same(json_text(a, "source"), "program")) {
        report_error(j, r->line, "%s is not the program's to report; its source is %s", r->id,
                     json_text(a, "source"));
        return;
    }
    for (i = 0; i < at; i++) {
        if ((j->rec[i].verb == VERB_ASSERT || j->rec[i].verb == VERB_NOTRUN) &&
            same(j->rec[i].id, r->id)) {
            report_error(j, r->line, "%s is reported twice", r->id);
            return;
        }
    }
}

/**
 * Judges the records in order: begin first, end last, and each against the
 * case.
 */
static void judge_records(struct judging *j)
{
    const char *case_id = json_text(j->c, "case_id");
    const char *children = json_text(json_member(j->c, "execution"), "children");
    long long counted = 0;
    size_t i;

    if (j->n > 0 && (j->rec[0].verb != VERB_BEGIN || !same(j->rec[0].id, case_id))) {
        report_error(j, j->rec[0].line, "the first record is not begin %s", case_id);
    }
    for (i = 0; i < j->n; i++) {
        const struct rec *r = &j->rec[i];

        if (j->has_end) {
            report_error(j, r->line, "a record follows end");
            break;
        }
        switch (r->verb) {
        case VERB_BEGIN:
            if (i > 0) {
                report_error(j, r->line, "a second begin");
            }
            break;
        case VERB_ASSERT:
        case VERB_NOTRUN:
            counted++;
            judge_assertion(j, r, i);
            break;
        case VERB_BRANCH:
            counted++;
            judge_branch(j, r);
            break;
        case VERB_PROGRESS:
            judge_progress(j, r, i);
            break;
        case VERB_CHILD:
            if (!same(children, "reported") || r->number == 0) {
                report_error(j, r->line, "a child record from a case whose children are %s",
                             children);
            }
            break;
        case VERB_END:
            j->has_end = 1;
            if (r->number != counted) {
                report_error(j, r->line, "end counts records that the report does not have");
            }
            break;
        }
    }
}

/** Returns the record of the assertion `id`, or NULL. */
static const struct rec *record_of(const struct judging *j, const char *id)
{
    size_t i;

    for (i = 0; i < j->n; i++) {
        if ((j->rec[i].verb == VERB_ASSERT || j->rec[i].verb == VERB_NOTRUN) &&
            same(j->rec[i].id, id)) {
            return &j->rec[i];
        }
    }
    return NULL;
}

/**
 * Returns 1 when the case requires the assertion `a` of the attempt: it is
 * in no group, or in the alternative its group took. An assertion of a
 * group that nothing decided is required, and can only be missing.
 */
static int required(const struct judging *j, const struct json *a)
{
    const struct json *alt = NULL;
    const struct json *group = group_of(j->c, json_text(a, "assertion_id"), &alt);
    int g;

    if (group == NULL) {
        return 1;
    }
    g = group_index(j->c, json_text(group, "group_id"));
    return g < 0 || j->decided[g] == NULL || same(j->decided[g], json_text(alt, "alternative_id"));
}

/**
 * Returns the outcome of the program's assertion `a` by its record, which it
 * gives in `*r`, or -1 when the attempt does not require `a`.
 */
static int program_outcome(const struct judging *j, const struct json *a, const struct rec **r)
{
    *r = record_of(j, json_text(a, "assertion_id"));
    if (!required(j, a)) {
        return -1;
    }
    if (*r == NULL) {
        return OUTCOME_MISSING;
    }
    if ((*r)->verb == VERB_NOTRUN) {
        return OUTCOME_NOT_RUN;
    }
    return (*r)->failed ? OUTCOME_FAIL : OUTCOME_PASS;
}

/**
 * Reports each record of an assertion whose alternative did not run, and,
 * when the report has its end, each assertion the program must give and did
 * not.
 */
static void judge_required(struct judging *j)
{
    const struct json *list = json_member(j->c, "assertions");
    size_t i;

    for (i = 0; i < list->count; i++) {
        const struct json *a = list->item[i];
        const char *id = json_text(a, "assertion_id");
        const struct rec *r;
        int outcome;

        if (!same(json_text(a, "source"), "program")) {
            continue;
        }
        outcome = program_outcome(j, a, &r);
        if (outcome < 0 && r != NULL) {
            report_error(j, r->line, "%s belongs to an alternative that did not run", id);
        } else if (outcome == OUTCOME_MISSING && j->has_end) {
            report_error(j, 0, "%s was never reported", id);
        }
    }
}

/**
 * Prints the outcome of each assertion the program must report, and returns the
 * attempt's.
 */
static enum attempt print_outcomes(const struct judging *j)
{
    const struct json *list = json_member(j->c, "assertions");
    enum attempt result = ATTEMPT_PASS;
    size_t i;

    for (i = 0; i < list->count; i++) {
        const struct json *a = list->item[i];
        const char *id = json_text(a, "assertion_id");
        const struct rec *r;
        int outcome;

        if (!same(json_text(a, "source"), "program") || (outcome = program_outcome(j, a, &r)) < 0) {
            continue;
        }
        if (outcome == OUTCOME_PASS || outcome == OUTCOME_MISSING) {
            say("assert %s %s\n", id, OUTCOMES[outcome]);
        } else {
            say("assert %s %s: %s\n", id, OUTCOMES[outcome], r->text);
        }
        if (outcome == OUTCOME_FAIL) {
            result = ATTEMPT_FAILURE;
        } else if (outcome != OUTCOME_PASS && result == ATTEMPT_PASS) {
            result = ATTEMPT_INCOMPLETE;
        }
    }
    return result;
}

/** Reports each group whose branch the report does not give. */
static void judge_groups(struct judging *j)
{
    const struct json *groups = json_member(j->c, "branches");
    size_t g;

    for (g = 0; g < groups->count && g < 32; g++) {
        if (!j->reported[g] && j->has_end) {
            report_error(j, 0, "no branch record for the group %s",
                         json_text(groups->item[g], "group_id"));
        }
    }
}

/** Prints each group's branch and what decided it. */
static void print_groups(const struct judging *j)
{
    const struct json *groups = json_member(j->c, "branches");
    size_t g;

    for (g = 0; g < groups->count && g < 32; g++) {
        say("branch %s %s decided by %s\n", json_text(groups->item[g], "group_id"),
            j->decided[g] != NULL ? j->decided[g] : "-", j->how[g] != NULL ? j->how[g] : "nothing");
    }
}

/**
 * Returns the number of assert, notrun and branch records, which end counts.
 */
static size_t counted(const struct judging *j)
{
    size_t i, n = 0;

    for (i = 0; i < j->n; i++) {
        n += j->rec[i].verb == VERB_ASSERT || j->rec[i].verb == VERB_NOTRUN ||
             j->rec[i].verb == VERB_BRANCH;
    }
    return n;
}

/**
 * Returns the state of the report: `absent` with no record at all, `complete`
 * with an end and no error, `truncated` with no end and no other error than
 * that, and `invalid` otherwise.
 */
static const char *report_state(const struct judging *j)
{
    if (j->n == 0 && j->parse_errors == 0) {
        return "absent";
    }
    if (!j->has_end) {
        return j->prior == 0 ? "truncated" : "invalid";
    }
    return j->errors == 0 ? "complete" : "invalid";
}

/** Checks that `arch` and `placement` are ones the case can have. */
static int applies(const struct json *c, const char *arch, const char *placement)
{
    const struct json *archs = json_member(c, "architectures");
    const char *domain = json_text(c, "domain");
    const char *plan = json_text(json_member(c, "native"), "plan");
    const char *case_id = json_text(c, "case_id");
    int arch_ok = archs->type == JSON_STRING ? same(arch, "none") : 0;
    size_t i;

    for (i = 0; archs->type == JSON_ARRAY && i < archs->count; i++) {
        arch_ok |= same(archs->item[i]->string, arch);
    }
    if (!arch_ok) {
        finding(NULL, 0,
                "%s does not apply to %s: no plan has an item of it there, and it is "
                "neither run nor passed",
                case_id, arch);
        return 0;
    }
    if (same(domain, "host")
            ? !same(placement, "host")
            : !(same(placement, "qemu") || (same(placement, "native") && same(plan, "native")))) {
        finding(NULL, 0, "%s cannot be placed %s; its domain is %s and its native plan %s", case_id,
                placement, domain, plan);
        return 0;
    }
    return 1;
}

/**
 * Returns the processor profile of a guest case placed `qemu`, whose boot
 * has it, or NULL for another placement, where the processor is the
 * machine's.
 */
static const struct etude_processor *placed_processor(const struct json *c, const char *placement)
{
    const struct json *qemu = json_member(json_member(c, "requirements"), "qemu");

    return same(placement, "qemu") ? etude_processor_find(json_text(qemu, "cpu")) : NULL;
}

/* The arguments of `report` and `attempt`: the processor's features and
 * the console, then the manifest, the case, the architecture, the placement
 * and the rest. */
struct request {
    char *features[64]; /* each `NAME=present` or `NAME=absent` of a -f */
    int nfeatures;
    const char *console; /* the file of a -k, or NULL */
    char **arg;
    int n;
};

/**
 * Takes the -f and -k options of `args` into `q`, and the arguments after
 * them.
 */
static void read_request(char **args, int n, struct request *q)
{
    int i = 0;

    q->nfeatures = 0;
    q->console = NULL;
    for (; i + 1 < n; i += 2) {
        if (strcmp(args[i], "-f") == 0 && q->nfeatures < 64) {
            q->features[q->nfeatures++] = args[i + 1];
        } else if (strcmp(args[i], "-k") == 0 && q->console == NULL) {
            q->console = args[i + 1];
        } else {
            break;
        }
    }
    q->arg = args + i;
    q->n = n - i;
}

/**
 * Starts the judgement `j` of the case the request names: reads its
 * manifest, finds the case, checks that the architecture and placement are
 * its, and decides the groups the processor's features decide. A guest case
 * placed `qemu` has the features its processor profile declares, and takes
 * no `-f`. Returns 0; 1 for a manifest with a finding, a case it lacks or a
 * placement the case cannot have; or 2 for a usage fault or a manifest that
 * cannot be read.
 */
static int open_case(struct cases *set, const struct request *q, struct judging *j)
{
    const struct etude_processor *processor;
    int status;

    memset(j, 0, sizeof *j);
    status = load_manifests(set, q->arg, 1);
    j->c = find_case(set, q->arg[1], NULL);
    if (status != 0 || j->c == NULL) {
        if (status == 0) {
            finding(q->arg[0], 0, "no case %s", q->arg[1]);
        }
        return status == 2 ? 2 : 1;
    }
    if (!applies(j->c, q->arg[2], q->arg[3])) {
        return 1;
    }
    processor = placed_processor(j->c, q->arg[3]);
    if (processor != NULL && q->nfeatures > 0) {
        finding(NULL, 0,
                "%s: placed qemu, its processor is the profile %s's, and -f gives a "
                "native or host machine's",
                q->arg[1], processor->name);
        return 2;
    }
    return decide_features(j, processor, q->features, q->nfeatures) != 0 ? 2 : 0;
}

/**
 * Judges the records of `path`, `-` for standard input, as the report of
 * the case `open_case` opened: each record against the case, each group's
 * branch and each assertion the program must give, and its end. Reports
 * each error as a finding, and in `j->texts` when the caller gave it.
 * Returns 0, or 2 when the file cannot be read; the caller frees `j->rec`.
 */
static int judge_text(struct judging *j, const char *path)
{
    size_t len;
    char *text;

    j->input = path;
    text = read_file(path, &len);
    if (text == NULL) {
        return 2;
    }
    j->rec = json_alloc(RECORDS_MAX * sizeof *j->rec);
    read_records(j, text);
    judge_records(j);
    judge_groups(j);
    judge_required(j);
    j->prior = j->errors;
    if (!j->has_end && (j->n > 0 || j->parse_errors > 0)) {
        report_error(j, 0, "the report has no end record");
    }
    free(text);
    return 0;
}

/**
 * Judges a program's output as the report of a case. `args` are, after any
 * `-f NAME=present|absent` options for the processor's features, the
 * manifest, the case, the architecture, the placement and the output's
 * file, `-` or none for standard input. Prints the report's state, each
 * group's branch, each assertion the program must report and the outcome
 * they give; returns 0 when they give a pass, 1 when not, and 2 for a usage
 * fault or a file that cannot be read.
 */
int judge_report(struct cases *set, char **args, int n)
{
    struct request q;
    struct judging j;
    enum attempt result;
    int status;

    read_request(args, n, &q);
    if (q.n < 4 || q.n > 5 || q.console != NULL) {
        return 2;
    }
    status = open_case(set, &q, &j);
    if (status != 0) {
        return status;
    }
    if (judge_text(&j, q.n == 5 ? q.arg[4] : "-") != 0) {
        return 2;
    }
    print_groups(&j);
    result = print_outcomes(&j);
    say("report %s, %lu records\n", report_state(&j), (unsigned long)counted(&j));
    if (same(report_state(&j), "invalid")) {
        result = ATTEMPT_FAILURE;
    } else if (j.errors > 0 || j.n == 0) {
        result = result == ATTEMPT_FAILURE ? result : ATTEMPT_INCOMPLETE;
    }
    say("outcome %s\n", ATTEMPTS[result]);
    free(j.rec);
    return result == ATTEMPT_PASS ? 0 : 1;
}

/* ---- results ---- */

/* A results document being checked, and the case it gives the results of. */
struct checking {
    const char *file;
    int line;
    const struct json *doc;
    const struct json *c;
    const char *alternative[32]; /* each group's alternative, or NULL */
};

/** Returns the outcome named `name`, or -1. */
static int outcome_of(const char *name)
{
    size_t i;

    for (i = 0; i < sizeof OUTCOMES / sizeof OUTCOMES[0]; i++) {
        if (same(OUTCOMES[i], name)) {
            return (int)i;
        }
    }
    return -1;
}

/**
 * Checks each group of the case appears once, decided as the case decides it.
 */
static void check_branches(struct checking *k)
{
    const struct json *groups = json_member(k->c, "branches");
    const struct json *given = json_member(k->doc, "branches");
    size_t g, i;

    for (i = 0; i < given->count; i++) {
        if (group_index(k->c, json_text(given->item[i], "group_id")) < 0) {
            finding(k->file, k->line, "the group %s is not the case's",
                    json_text(given->item[i], "group_id"));
        }
    }
    for (g = 0; g < groups->count && g < 32; g++) {
        const char *group_id = json_text(groups->item[g], "group_id");
        const char *want =
            json_member(groups->item[g], "decided_by")->type == JSON_OBJECT ? "feature" : "report";
        const struct json *b = NULL;
        const char *how;
        int times = 0;

        for (i = 0; i < given->count; i++) {
            if (same(json_text(given->item[i], "group_id"), group_id)) {
                b = given->item[i];
                times++;
            }
        }
        if (times != 1) {
            finding(k->file, k->line, "the group %s appears %d times, not once", group_id, times);
            continue;
        }
        how = json_text(b, "decided_by");
        k->alternative[g] = json_text(b, "alternative_id");
        if (!same(how, want) && !(same(want, "report") && same(how, "undecided"))) {
            finding(k->file, k->line, "the group %s is decided by %s, not %s", group_id, want, how);
        } else if (same(how, "undecided") != (k->alternative[g] == NULL)) {
            finding(k->file, k->line, "the group %s has an alternative only when it is decided",
                    group_id);
        } else if (k->alternative[g] != NULL &&
                   !has_member(json_member(groups->item[g], "alternatives"), "alternative_id",
                               k->alternative[g])) {
            finding(k->file, k->line, "the group %s has no alternative %s", group_id,
                    k->alternative[g]);
        }
    }
}

/**
 * Returns 1 when an attempt of the case `c` placed `placement` has
 * `etude.kernel`: under QEMU, and natively with the `log` fixture.
 */
static int kernel_read(const struct json *c, const char *placement)
{
    const struct json *native = json_member(json_member(c, "native"), "fixtures");

    return same(placement, "qemu") ||
           (same(placement, "native") && has_member(native, "fixture", "log"));
}

/**
 * Returns the source the results must give for `id`, or NULL when the
 * attempt does not require it.
 */
static const char *required_source(const struct checking *k, const char *id)
{
    const struct json *a = declared(k->c, id);
    const struct json *alt = NULL;
    const struct json *group;
    const char *placement = json_text(k->doc, "placement");
    int g;

    if (same(id, "etude.termination")) {
        return "executor";
    }
    if (same(id, "etude.report")) {
        return program_reports(k->c) ? "executor" : NULL;
    }
    if (same(id, "etude.kernel")) {
        return kernel_read(k->c, placement) ? "executor" : NULL;
    }
    if (a == NULL) {
        return NULL;
    }
    group = group_of(k->c, id, &alt);
    g = group != NULL ? group_index(k->c, json_text(group, "group_id")) : -1;
    if (g >= 0 && k->alternative[g] != NULL &&
        !same(k->alternative[g], json_text(alt, "alternative_id"))) {
        return NULL;
    }
    return json_text(a, "source");
}

/** Returns 1 when `id` is an assertion of a group that nothing decided. */
static int undecided(const struct checking *k, const char *id)
{
    const struct json *group = group_of(k->c, id, NULL);
    int g = group != NULL ? group_index(k->c, json_text(group, "group_id")) : -1;

    return g >= 0 && k->alternative[g] == NULL;
}

/**
 * Checks the results give each assertion the attempt requires, once, and no
 * other.
 */
static void check_assertions(const struct checking *k)
{
    static const char *const executor[] = {"etude.termination", "etude.report", "etude.kernel"};
    const struct json *given = json_member(k->doc, "assertions");
    const struct json *list = json_member(k->c, "assertions");
    size_t i, e;

    for (i = 0; i < given->count; i++) {
        const char *id = json_text(given->item[i], "assertion_id");
        const char *source = required_source(k, id);

        if (source == NULL) {
            finding(k->file, k->line, "%s is not an assertion this attempt requires", id);
        } else if (!same(source, json_text(given->item[i], "source"))) {
            finding(k->file, k->line, "%s has the source %s, not %s", id,
                    json_text(given->item[i], "source"), source);
        } else if (undecided(k, id) && !same(json_text(given->item[i], "outcome"), "missing")) {
            finding(k->file, k->line, "%s is in a group nothing decided, so it is missing", id);
        }
    }
    for (i = 0; i < list->count + 3; i++) {
        const char *id =
            i < list->count ? json_text(list->item[i], "assertion_id") : executor[i - list->count];
        size_t times = 0;

        if (required_source(k, id) == NULL) {
            continue;
        }
        for (e = 0; e < given->count; e++) {
            times += same(json_text(given->item[e], "assertion_id"), id);
        }
        if (times != 1) {
            finding(k->file, k->line,
                    "%s appears %lu times; each assertion the attempt requires "
                    "appears once",
                    id, (unsigned long)times);
        }
    }
}

/** Returns the outcome of the assertion `id` in the results, or -1. */
static int given_outcome(const struct checking *k, const char *id)
{
    const struct json *given = json_member(k->doc, "assertions");
    size_t i;

    for (i = 0; i < given->count; i++) {
        if (same(json_text(given->item[i], "assertion_id"), id)) {
            return outcome_of(json_text(given->item[i], "outcome"));
        }
    }
    return -1;
}

/**
 * Returns the outcome of etude.report for a report in `state`: a pass when
 * it is complete, a failure when it is invalid, and missing when it stopped
 * before its end or never began, since the program then did not finish.
 */
static int report_verdict(const char *state)
{
    if (same(state, "complete")) {
        return OUTCOME_PASS;
    }
    return same(state, "invalid") ? OUTCOME_FAIL : OUTCOME_MISSING;
}

/** Checks the report's state against the assertions it decides. */
static void check_report(const struct checking *k)
{
    const struct json *report = json_member(k->doc, "report");
    const char *state = json_text(report, "state");
    int etude_report = given_outcome(k, "etude.report");
    int uses = program_reports(k->c);

    if (json_member(k->doc, "undeclared")->count > 0 && !same(state, "invalid")) {
        finding(k->file, k->line,
                "an undeclared assertion was reported, so the report is "
                "invalid, not %s",
                state);
    }
    if (same(state, "complete") && json_member(report, "errors")->count > 0) {
        finding(k->file, k->line, "a complete report has no errors");
    }
    if (uses == same(state, "not_used")) {
        finding(k->file, k->line, "the report is %s, but the case %s assertions of its program",
                state, uses ? "has" : "has no");
    }
    if (etude_report >= 0 && etude_report != report_verdict(state)) {
        finding(k->file, k->line, "etude.report is %s for a report that is %s, where it is %s",
                OUTCOMES[etude_report], state, OUTCOMES[report_verdict(state)]);
    }
}

/**
 * Writes the end of the termination `term` and its value, as `exited 1`, into
 * `out`.
 */
static void end_text(const struct json *term, char *out, size_t n)
{
    const struct json *value = json_member(term, "value");

    if (value != NULL && value->type == JSON_INTEGER) {
        snprintf(out, n, "%s %lld", json_text(term, "end"), value->integer);
    } else {
        snprintf(out, n, "%s", json_text(term, "end"));
    }
}

/**
 * Checks etude.termination against the end observed and the end the case
 * expects.
 */
static void check_termination(const struct checking *k)
{
    const struct json *term = json_member(k->doc, "termination");
    const struct json *want = json_member(k->c, "termination");
    const char *end = json_text(term, "end");
    int outcome = given_outcome(k, "etude.termination");
    int unobserved = same(end, "stopped") || same(end, "lost");
    int expected = same(end, json_text(want, "end")) &&
                   json_equal(json_member(term, "value"), json_member(want, "value"));

    if (outcome < 0) {
        return;
    }
    if (unobserved ? outcome != OUTCOME_MISSING
                   : outcome != (expected ? OUTCOME_PASS : OUTCOME_FAIL)) {
        char seen[80], wanted[80];

        end_text(term, seen, sizeof seen);
        end_text(want, wanted, sizeof wanted);
        finding(k->file, k->line,
                "etude.termination is %s for the end %s, where the case expects %s",
                OUTCOMES[outcome], seen, wanted);
    }
}

/**
 * Returns the attempt's outcome, and in `why` the first assertion that decides
 * it.
 */
static enum attempt derive(const struct checking *k, char *why, size_t n)
{
    const struct json *lists[2];
    const char *state = json_text(json_member(k->doc, "report"), "state");
    const char *end = json_text(json_member(k->doc, "termination"), "end");
    enum attempt result = ATTEMPT_PASS;
    size_t l, i;

    lists[0] = json_member(k->doc, "assertions");
    lists[1] = json_member(k->doc, "undeclared");
    why[0] = '\0';
    for (l = 0; l < 2; l++) {
        for (i = 0; i < lists[l]->count; i++) {
            const struct json *a = lists[l]->item[i];
            int o = outcome_of(json_text(a, "outcome"));

            if (o == OUTCOME_FAIL || (o != OUTCOME_PASS && result == ATTEMPT_PASS)) {
                snprintf(why, n, "%s is %s", json_text(a, "assertion_id"), OUTCOMES[o]);
                result = o == OUTCOME_FAIL ? ATTEMPT_FAILURE : ATTEMPT_INCOMPLETE;
            }
            if (result == ATTEMPT_FAILURE) {
                return result;
            }
        }
    }
    if (result == ATTEMPT_PASS && !same(state, "complete") && !same(state, "not_used")) {
        snprintf(why, n, "the report is %s", state);
        result = ATTEMPT_INCOMPLETE;
    }
    if (result == ATTEMPT_PASS && (same(end, "stopped") || same(end, "lost"))) {
        snprintf(why, n, "the end was %s", end);
        result = ATTEMPT_INCOMPLETE;
    }
    return result;
}

/**
 * Checks one results document against its case in `set`; returns the outcome
 * its assertions give, with the first assertion that decides it in `why`, or
 * -1 when `set` has no case of it.
 */
static int examine(const struct cases *set, struct checking *k, char *why, size_t n)
{
    char hex[65];
    const char *case_id = json_text(k->doc, "case_id");

    k->c = find_case(set, case_id, NULL);
    if (k->c == NULL) {
        finding(k->file, k->line, "%s is no case of the manifests given", case_id);
        return -1;
    }
    inventory_digest(k->c, hex);
    if (!same(hex, json_text(k->doc, "inventory_sha256"))) {
        finding(k->file, k->line, "%s: the inventory digest is not the case's, %s", case_id, hex);
    }
    if (same(json_text(k->c, "domain"), "host") != same(json_text(k->doc, "placement"), "host") ||
        (same(json_text(k->doc, "placement"), "native") &&
         !same(json_text(json_member(k->c, "native"), "plan"), "native"))) {
        finding(k->file, k->line, "%s cannot be placed %s", case_id,
                json_text(k->doc, "placement"));
    }
    check_branches(k);
    check_assertions(k);
    check_report(k);
    check_termination(k);
    return (int)derive(k, why, n);
}

/**
 * Checks the results document `doc`, read from `file`, against its case in
 * `set`, as `results` checks each document of its file, and prints nothing
 * but its findings. Returns the attempt outcome its assertions
 * give, with the first assertion that decides it in `why`, or -1 when `set`
 * has no case of it. The caller has checked `doc` against the results schema.
 */
int judge_document(const struct cases *set, const struct json *doc, const char *file, char *why,
                   size_t n)
{
    struct checking k;

    memset(&k, 0, sizeof k);
    k.file = file;
    k.doc = doc;
    return examine(set, &k, why, n);
}

/**
 * Checks one results document; prints its outcome and compares it with `claim`.
 */
static void check_document(const struct cases *set, struct checking *k, const char *claim)
{
    char why[300];
    const char *case_id = json_text(k->doc, "case_id");
    int result = examine(set, k, why, sizeof why);

    if (result < 0) {
        return;
    }
    say("%s:%d: %s %s: %s%s%s\n", k->file, k->line, case_id, json_text(k->doc, "attempt_id"),
        ATTEMPTS[result], why[0] ? ", as " : "", why);
    if (claim != NULL && !same(claim, ATTEMPTS[result])) {
        finding(k->file, k->line, "%s: the attempt's outcome is %s, not %s: %s", case_id,
                ATTEMPTS[result], claim, why[0] ? why : "every assertion passed");
    }
}

/**
 * Checks each results document of the JSON Lines file `path` against the
 * results schema and its case in `set`. With `claim`, the outcome a history
 * records for every attempt of the file, a document whose assertions give
 * another outcome is refused. Returns 0, 1 after a finding, or 2.
 */
int judge_results(const struct cases *set, const char *path, const char *claim)
{
    struct json *schema = read_schema(set->schemas, "results");
    size_t len;
    char *text = read_file(path, &len);
    char *line, *next;
    int number = 1, before = findings();

    if (schema == NULL || text == NULL) {
        json_free(schema);
        free(text);
        return 2;
    }
    for (line = text; *line != '\0'; line = next, number++) {
        struct json *doc;
        char err[200];

        next = strchr(line, '\n');
        next = next != NULL ? next + 1 : line + strlen(line);
        if (strspn(line, " \t\r\n") >= (size_t)(next - line)) {
            continue;
        }
        if (json_parse(line, (size_t)(next - line), number, &doc, err, sizeof err) != 0) {
            finding(path, 0, "%s", err);
            continue;
        }
        if (schema_findings(schema, doc, path) == 0) {
            struct checking k;

            memset(&k, 0, sizeof k);
            k.file = path;
            k.line = number;
            k.doc = doc;
            check_document(set, &k, claim);
        }
        json_free(doc);
    }
    json_free(schema);
    free(text);
    return findings() > before ? 1 : 0;
}

/* ---- the results of an attempt ---- */

/* The pattern of a history identity, results.schema.json's history_id. */
#define HISTORY_ID_PATTERN "^[A-Za-z0-9][A-Za-z0-9._:/-]{0,254}$"

/* How an attempt's program ended, as the executor saw it. */
struct ending {
    const char *end; /* exited, signaled, stopped or lost */
    int value;       /* the status or the signal, or -1 for stopped and lost */
};

/**
 * Reads the end `args[0]` and its value `args[1]`, which exited and
 * signaled take and stopped and lost do not; `n` is 1 or 2. Returns 0, or
 * -1 after a finding.
 */
static int read_ending(char **args, int n, struct ending *e)
{
    int valued = same(args[0], "exited") || same(args[0], "signaled");
    long long value = n == 2 ? count_of(args[1]) : -1;

    e->end = args[0];
    e->value = -1;
    if (!valued && !same(args[0], "stopped") && !same(args[0], "lost")) {
        finding(NULL, 0, "attempt: %s is no end; give exited, signaled, stopped or lost", args[0]);
        return -1;
    }
    if (!valued && n == 2) {
        finding(NULL, 0, "attempt: the end %s takes no value", args[0]);
        return -1;
    }
    if (valued && (value < 0 || value > 255)) {
        finding(NULL, 0, "attempt: the end %s takes a value of 0 to 255", args[0]);
        return -1;
    }
    e->value = (int)value;
    return 0;
}

/**
 * Writes into `out` how the end `e` differs from the one the case expects,
 * and returns `out`; returns NULL when it is the one expected.
 */
static const char *ending_detail(const struct judging *j, const struct ending *e, char *out,
                                 size_t n)
{
    const struct json *want = json_member(j->c, "termination");
    const struct json *value = json_member(want, "value");

    if (same(e->end, "stopped")) {
        snprintf(out, n, "the executor stopped it");
    } else if (same(e->end, "lost")) {
        snprintf(out, n, "the executor did not observe its end");
    } else if (same(e->end, json_text(want, "end")) && value->type == JSON_INTEGER &&
               value->integer == e->value) {
        return NULL;
    } else {
        char wanted[80];

        end_text(want, wanted, sizeof wanted);
        snprintf(out, n, "%s %d, where the case expects %s", e->end, e->value, wanted);
    }
    return out;
}

/**
 * Appends to the array `b` the assertion `id`, with its `source` unless that
 * is NULL, as `undeclared` gives it, its outcome and its detail, NULL for
 * none.
 */
static void put_assertion(struct jbuf *b, const char *id, const char *source, int outcome,
                          const char *detail)
{
    put_comma(b);
    jbuf_puts(b, "{\"assertion_id\":");
    put_text(b, id);
    if (source != NULL) {
        jbuf_puts(b, ",\"source\":");
        put_text(b, source);
    }
    jbuf_puts(b, ",\"outcome\":");
    put_text(b, OUTCOMES[outcome]);
    jbuf_puts(b, ",\"detail\":");
    if (detail != NULL) {
        put_text(b, detail);
    } else {
        jbuf_puts(b, "null");
    }
    jbuf_puts(b, "}");
}

/**
 * Returns 1 when the console of `j`, NUL bytes and all, has the text `s`,
 * and 0 when not or when `s` is NULL or empty.
 */
static int console_has(const struct judging *j, const char *s)
{
    size_t n = s != NULL ? strlen(s) : 0, i;

    for (i = 0; n > 0 && i + n <= j->console_len; i++) {
        if (j->console[i] == s[0] && memcmp(j->console + i, s, n) == 0) {
            return 1;
        }
    }
    return 0;
}

/**
 * Copies into `out` the first record of the console of `j` at the kernel's
 * level 2, KLOG_CRIT, which only a panic and a fault of the kernel use: a
 * line that begins `<2>[`. With `panics` set, a record of the subsystem
 * `panic` is passed by. Returns 1 when there is one, and 0 when not.
 */
static int console_fault(const struct judging *j, int panics, char *out, size_t size)
{
    const char *c = j->console, *close;
    size_t i, end;

    for (i = 0; i + 4 <= j->console_len; i++) {
        if ((i > 0 && c[i - 1] != '\n') || memcmp(c + i, "<2>[", 4) != 0) {
            continue;
        }
        for (end = i; end < j->console_len && c[end] != '\r' && c[end] != '\n'; end++) {
        }
        close = memchr(c + i, ']', end - i);
        if (panics && close != NULL && (size_t)(c + end - close) >= 9 &&
            memcmp(close, "] panic: ", 9) == 0) {
            continue;
        }
        snprintf(out, size, "the console has %.*s", (int)(end - i), c + i);
        return 1;
    }
    return 0;
}

/**
 * Returns the outcome of the declared assertion `a`, with its detail in
 * `*detail`, or -1 when the attempt does not require it. An assertion of
 * the executor passes when the console has its record and fails when it has
 * not; without the console it is missing, as one of a scenario is, since
 * only an executor's console and a scenario's calls give them.
 */
static int declared_outcome(const struct judging *j, const struct json *a, const char **detail)
{
    const struct json *group = group_of(j->c, json_text(a, "assertion_id"), NULL);
    int g = group != NULL ? group_index(j->c, json_text(group, "group_id")) : -1;
    const struct rec *r;
    int outcome;

    *detail = NULL;
    if (!required(j, a)) {
        return -1;
    }
    if (same(json_text(a, "source"), "executor") && j->console != NULL) {
        if (console_has(j, json_text(a, "record"))) {
            return OUTCOME_PASS;
        }
        *detail = "the console has no such record";
        return OUTCOME_FAIL;
    }
    if (same(json_text(a, "source"), "executor")) {
        *detail = "casecheck attempt was given no console";
        return OUTCOME_MISSING;
    }
    if (same(json_text(a, "source"), "scenario")) {
        *detail = "casecheck attempt does not read a scenario's calls";
        return OUTCOME_MISSING;
    }
    if (g >= 0 && j->decided[g] == NULL) {
        *detail = "the program reported no branch of its group";
        return OUTCOME_MISSING;
    }
    outcome = program_outcome(j, a, &r);
    if (outcome == OUTCOME_MISSING) {
        *detail = "nothing reported it";
    } else if (outcome != OUTCOME_PASS) {
        *detail = r->text;
    }
    return outcome;
}

/**
 * Appends the assertions an attempt of the case requires, which ended as
 * `e`, placed `placement` with its report in `state`: the declared ones in
 * the manifest's order, then the executor's. With the console, `etude.kernel`
 * fails for its first record of a panic or a fault of the kernel, passing by
 * a panic when the termination expects one, and passes when it has none.
 */
static void put_assertions(struct jbuf *b, const struct judging *j, const struct ending *e,
                           const char *placement, const char *state)
{
    const struct json *list = json_member(j->c, "assertions");
    char why[200], seen[64], fault[256];
    const char *detail;
    size_t i;
    int outcome;

    jbuf_puts(b, "[");
    for (i = 0; i < list->count; i++) {
        outcome = declared_outcome(j, list->item[i], &detail);
        if (outcome >= 0) {
            put_assertion(b, json_text(list->item[i], "assertion_id"),
                          json_text(list->item[i], "source"), outcome, detail);
        }
    }
    detail = ending_detail(j, e, why, sizeof why);
    outcome = detail == NULL ? OUTCOME_PASS : OUTCOME_FAIL;
    put_assertion(b, "etude.termination", "executor", e->value < 0 ? OUTCOME_MISSING : outcome,
                  detail);
    if (kernel_read(j->c, placement) && j->console != NULL) {
        outcome =
            console_fault(j, same(json_text(json_member(j->c, "termination"), "end"), "panic"),
                          fault, sizeof fault);
        put_assertion(b, "etude.kernel", "executor", outcome ? OUTCOME_FAIL : OUTCOME_PASS,
                      outcome ? fault : NULL);
    } else if (kernel_read(j->c, placement)) {
        put_assertion(b, "etude.kernel", "executor", OUTCOME_MISSING,
                      "casecheck attempt was given no console and does not read the kernel's log");
    }
    if (program_reports(j->c)) {
        outcome = report_verdict(state);
        snprintf(seen, sizeof seen, "the report is %s", state);
        put_assertion(b, "etude.report", "executor", outcome,
                      outcome == OUTCOME_PASS ? NULL : seen);
    }
    jbuf_puts(b, "]");
}

/**
 * Appends each assert and notrun record of an assertion the case does not
 * declare, with its outcome and its detail.
 */
static void put_undeclared(struct jbuf *b, const struct judging *j)
{
    size_t i;

    jbuf_puts(b, "[");
    for (i = 0; i < j->n; i++) {
        const struct rec *r = &j->rec[i];
        int outcome = r->verb == VERB_NOTRUN ? OUTCOME_NOT_RUN
                      : r->failed            ? OUTCOME_FAIL
                                             : OUTCOME_PASS;

        if ((r->verb == VERB_ASSERT || r->verb == VERB_NOTRUN) && declared(j->c, r->id) == NULL) {
            put_assertion(b, r->id, NULL, outcome, outcome == OUTCOME_PASS ? NULL : r->text);
        }
    }
    jbuf_puts(b, "]");
}

/** Appends each group of the case with its alternative and what decided it. */
static void put_branches(struct jbuf *b, const struct judging *j)
{
    const struct json *groups = json_member(j->c, "branches");
    size_t g;

    jbuf_puts(b, "[");
    for (g = 0; g < groups->count && g < 32; g++) {
        put_comma(b);
        jbuf_puts(b, "{\"group_id\":");
        put_text(b, json_text(groups->item[g], "group_id"));
        jbuf_puts(b, ",\"alternative_id\":");
        if (j->decided[g] != NULL) {
            put_text(b, j->decided[g]);
        } else {
            jbuf_puts(b, "null");
        }
        jbuf_puts(b, ",\"decided_by\":");
        put_text(b, j->how[g] != NULL ? j->how[g] : "undecided");
        jbuf_puts(b, "}");
    }
    jbuf_puts(b, "]");
}

/**
 * Appends the termination of the end `e` of an attempt placed `placement`,
 * and what observed it.
 */
static void put_termination(struct jbuf *b, const struct judging *j, const struct ending *e,
                            const char *placement)
{
    char why[200], value[16];
    const char *detail = ending_detail(j, e, why, sizeof why);
    const char *by = same(placement, "host") ? "host_process" : "etude_guest";

    snprintf(value, sizeof value, "%d", e->value);
    jbuf_puts(b, "{\"end\":");
    put_text(b, e->end);
    jbuf_puts(b, ",\"value\":");
    jbuf_puts(b, e->value < 0 ? "null" : value);
    jbuf_puts(b, ",\"observed_by\":");
    put_text(b, e->value < 0 ? "executor" : by);
    jbuf_puts(b, ",\"detail\":");
    if (detail != NULL) {
        put_text(b, detail);
    } else {
        jbuf_puts(b, "null");
    }
    jbuf_puts(b, "}");
}

/**
 * Writes into `b` the results document of the attempt the request `q`
 * names, which ended as `e`, from the judgement `j` of its report: one line
 * of compact JSON.
 */
static void write_document(struct jbuf *b, const struct judging *j, const struct request *q,
                           const struct ending *e)
{
    static const char *const ids[] = {"run_id", "attempt_id", "item_id"};
    const char *state = program_reports(j->c) ? report_state(j) : "not_used";
    char hex[65], records[32];
    size_t i;

    jbuf_puts(b, "{\"schema_version\":1");
    for (i = 0; i < 3; i++) {
        jbuf_puts(b, ",");
        put_text(b, ids[i]);
        jbuf_puts(b, ":");
        put_text(b, q->arg[5 + i]);
    }
    inventory_digest(j->c, hex);
    jbuf_puts(b, ",\"case_id\":");
    put_text(b, json_text(j->c, "case_id"));
    jbuf_puts(b, ",\"inventory_sha256\":");
    put_text(b, hex);
    jbuf_puts(b, ",\"placement\":");
    put_text(b, q->arg[3]);
    jbuf_puts(b, ",\"termination\":");
    put_termination(b, j, e, q->arg[3]);
    snprintf(records, sizeof records, "%lu", (unsigned long)counted(j));
    jbuf_puts(b, ",\"report\":{\"state\":");
    put_text(b, state);
    jbuf_puts(b, ",\"records\":");
    jbuf_puts(b, records);
    jbuf_puts(b, ",\"errors\":");
    jbuf_puts(b, j->texts->data);
    jbuf_puts(b, "},\"branches\":");
    put_branches(b, j);
    jbuf_puts(b, ",\"assertions\":");
    put_assertions(b, j, e, q->arg[3], state);
    jbuf_puts(b, ",\"undeclared\":");
    put_undeclared(b, j);
    jbuf_puts(b, "}\n");
}

/**
 * Checks the results document `text` as `results` checks one: by the
 * results schema, and against its case in `set`. Returns the outcome its
 * assertions give, or -1 after a finding, which is a fault of casecheck.
 */
static int recheck(const struct cases *set, const char *text)
{
    struct json *schema = read_schema(set->schemas, "results");
    struct json *doc = NULL;
    char err[200], why[300];
    int before = findings(), result = -1;

    if (schema == NULL) {
        return -1;
    }
    if (json_parse(text, strlen(text), 1, &doc, err, sizeof err) != 0) {
        finding(NULL, 0, "attempt: %s", err);
    } else if (schema_findings(schema, doc, "attempt") == 0) {
        result = judge_document(set, doc, "attempt", why, sizeof why);
    }
    if (findings() > before) {
        finding(NULL, 0, "attempt: casecheck wrote a results document that fails its own check");
        result = -1;
    }
    json_free(doc);
    json_free(schema);
    return result;
}

/**
 * Gives the results document of one attempt. `args` are those of `report`,
 * with the output's file required, then the run's, the attempt's and the
 * item's identities, the end and, for exited and signaled, its value; a
 * `-k CONSOLE` among the options gives the console of the machine of an
 * attempt placed `qemu`, from which the executor's assertions are decided.
 * Judges the output's records as `report` does, without printing the
 * judgement, and prints the document as one line. Returns 0 when the
 * attempt passed, 1 when it failed and 3 when it is incomplete; 2, with no
 * document, for a usage fault, a file that cannot be read, a case not in the
 * manifest, a placement it cannot have or a console of another placement.
 */
int judge_attempt(struct cases *set, char **args, int n)
{
    struct jbuf texts = {0}, doc = {0};
    struct request q;
    struct judging j;
    struct ending e;
    char *console = NULL;
    size_t console_len = 0;
    int i, result = -1;

    read_request(args, n, &q);
    if (q.n < 9 || q.n > 10 || read_ending(q.arg + 8, q.n - 8, &e) != 0) {
        return 2;
    }
    for (i = 5; i < 8; i++) {
        if (!schema_pattern(HISTORY_ID_PATTERN, q.arg[i])) {
            finding(NULL, 0, "attempt: %s is no identity of a history", q.arg[i]);
            return 2;
        }
    }
    if (open_case(set, &q, &j) != 0) {
        return 2;
    }
    if (q.console != NULL && !same(q.arg[3], "qemu")) {
        finding(NULL, 0, "attempt: -k gives a QEMU machine's console, and %s is placed %s",
                q.arg[1], q.arg[3]);
        return 2;
    }
    if (q.console != NULL && (console = read_file(q.console, &console_len)) == NULL) {
        return 2;
    }
    j.console = console;
    j.console_len = console_len;
    jbuf_puts(&texts, "[");
    j.texts = &texts;
    if (judge_text(&j, q.arg[4]) != 0) {
        jbuf_free(&texts);
        free(console);
        return 2;
    }
    jbuf_puts(&texts, "]");
    if (!program_reports(j.c) && (j.n > 0 || j.parse_errors > 0)) {
        finding(NULL, 0,
                "%s declares no assertion of its program, and no results can give the "
                "report records of its output",
                q.arg[1]);
    } else {
        write_document(&doc, &j, &q, &e);
        result = recheck(set, doc.data);
    }
    if (result >= 0) {
        say_text(doc.data);
    }
    free(j.rec);
    free(console);
    jbuf_free(&texts);
    jbuf_free(&doc);
    return result < 0 ? 2 : result == ATTEMPT_PASS ? 0 : result == ATTEMPT_FAILURE ? 1 : 3;
}
