/*
 * judge.c: casecheck's judgement of a program's report records, and its
 * check of an attempt's results document against the case.
 *
 * Both follow the Readme's Report records and Results: an assertion that
 * nothing reported is missing, one the program could not evaluate is not
 * run, and neither is ever a pass. A report that a program wrote is judged
 * here as an executor will judge it; a results document is checked for the
 * assertions it must give, once each, and for an outcome that follows from
 * them, so that a document which leaves an assertion out, or a history that
 * calls a skipped assertion passed, is refused.
 */
#include "casecheck.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    const char *decided[32]; /* each group's alternative, or NULL */
    const char *how[32];     /* "feature" or "report", or NULL for neither */
    int reported[32];        /* the group's branch record was read */
};

static int same(const char *a, const char *b)
{
    return a != NULL && b != NULL && strcmp(a, b) == 0;
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
 * Decides the groups decided by features from `features`, a list of
 * `NAME=present` and `NAME=absent`; returns 0, or -1 when a feature a group
 * names is not given.
 */
static int decide_features(struct judging *j, char **features, int n)
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

            for (i = 0; i < n; i++) {
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
 * Prints the outcome of each assertion the program must report, and returns the
 * attempt's.
 */
static enum attempt judge_outcomes(struct judging *j)
{
    const struct json *list = json_member(j->c, "assertions");
    enum attempt result = ATTEMPT_PASS;
    size_t i;

    for (i = 0; i < list->count; i++) {
        const struct json *a = list->item[i];
        const char *id = json_text(a, "assertion_id");
        const struct rec *r = record_of(j, id);

        if (!same(json_text(a, "source"), "program")) {
            continue;
        }
        if (!required(j, a)) {
            if (r != NULL) {
                report_error(j, r->line, "%s belongs to an alternative that did not run", id);
            }
            continue;
        }
        if (r == NULL) {
            say("assert %s missing\n", id);
            if (j->has_end) {
                report_error(j, 0, "%s was never reported", id);
            }
            result = result == ATTEMPT_FAILURE ? result : ATTEMPT_INCOMPLETE;
        } else if (r->verb == VERB_NOTRUN) {
            say("assert %s not_run: %s\n", id, r->text);
            result = result == ATTEMPT_FAILURE ? result : ATTEMPT_INCOMPLETE;
        } else if (r->failed) {
            say("assert %s fail: %s\n", id, r->text);
            result = ATTEMPT_FAILURE;
        } else {
            say("assert %s pass\n", id);
        }
    }
    return result;
}

/**
 * Reports each group whose branch the report does not give, and prints each
 * group's.
 */
static void judge_groups(struct judging *j)
{
    const struct json *groups = json_member(j->c, "branches");
    size_t g;

    for (g = 0; g < groups->count && g < 32; g++) {
        const char *group_id = json_text(groups->item[g], "group_id");
        if (!j->reported[g] && j->has_end) {
            report_error(j, 0, "no branch record for the group %s", group_id);
        }
        say("branch %s %s decided by %s\n", group_id, j->decided[g] != NULL ? j->decided[g] : "-",
            j->how[g] != NULL ? j->how[g] : "nothing");
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
 * that, and `invalid` otherwise. `prior` is the count of errors before the
 * missing end's.
 */
static const char *report_state(const struct judging *j, int prior)
{
    if (j->n == 0 && j->parse_errors == 0) {
        return "absent";
    }
    if (!j->has_end) {
        return prior == 0 ? "truncated" : "invalid";
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
    char *features[64];
    int nfeatures = 0, i = 0, status, prior;
    struct judging j;
    size_t len;
    char *text;
    enum attempt result;

    for (; i + 1 < n && strcmp(args[i], "-f") == 0 && nfeatures < 64; i += 2) {
        features[nfeatures++] = args[i + 1];
    }
    if (n - i < 4 || n - i > 5) {
        return 2;
    }
    memset(&j, 0, sizeof j);
    status = load_manifests(set, &args[i], 1);
    j.c = find_case(set, args[i + 1], NULL);
    if (status != 0 || j.c == NULL) {
        if (status == 0) {
            finding(args[i], 0, "no case %s", args[i + 1]);
        }
        return status == 2 ? 2 : 1;
    }
    if (!applies(j.c, args[i + 2], args[i + 3])) {
        return 1;
    }
    if (decide_features(&j, features, nfeatures) != 0) {
        return 2;
    }
    j.input = n - i == 5 && strcmp(args[i + 4], "-") != 0 ? args[i + 4] : "-";
    text = read_file(j.input, &len);
    if (text == NULL) {
        return 2;
    }
    j.rec = json_alloc(RECORDS_MAX * sizeof *j.rec);
    read_records(&j, text);
    judge_records(&j);
    judge_groups(&j);
    result = judge_outcomes(&j);
    prior = j.errors;
    if (!j.has_end && (j.n > 0 || j.parse_errors > 0)) {
        report_error(&j, 0, "the report has no end record");
    }
    say("report %s, %lu records\n", report_state(&j, prior), (unsigned long)counted(&j));
    if (same(report_state(&j, prior), "invalid")) {
        result = ATTEMPT_FAILURE;
    } else if (j.errors > 0 || j.n == 0) {
        result = result == ATTEMPT_FAILURE ? result : ATTEMPT_INCOMPLETE;
    }
    say("outcome %s\n", ATTEMPTS[result]);
    free(j.rec);
    free(text);
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
        const struct json *native = json_member(json_member(k->c, "native"), "fixtures");

        return same(placement, "qemu") ||
                       (same(placement, "native") && has_member(native, "fixture", "log"))
                   ? "executor"
                   : NULL;
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
 * Checks one results document; prints its outcome and compares it with `claim`.
 */
static void check_document(const struct cases *set, struct checking *k, const char *claim)
{
    char hex[65], why[300];
    const char *case_id = json_text(k->doc, "case_id");
    enum attempt result;

    k->c = find_case(set, case_id, NULL);
    if (k->c == NULL) {
        finding(k->file, k->line, "%s is no case of the manifests given", case_id);
        return;
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
    result = derive(k, why, sizeof why);
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
