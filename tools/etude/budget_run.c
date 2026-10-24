/*
 * budget_run.c: `etude estimate`, which forecasts a selection's charge from
 * recorded costs, and checks the budget of a run from its records.
 */
#include "budget.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char usage_text[] = "usage: etude estimate [-s SCHEMAS] [--routine-proofs DIR] "
                                 "forecast POLICY CLASS MANIFEST... [-- HISTORY...]\n"
                                 "       etude estimate [-s SCHEMAS] [--routine-proofs DIR] check "
                                 "POLICY RUN [MANIFEST...] -- HISTORY...\n";

/* What one invocation works with. */
struct command {
    const char *schemas, *verb, *policy_path, *subject;
    char **manifest_paths, **history_paths;
    int manifest_count, history_count;
    struct etude_budget_policy policy;
    struct json **manifests;
    struct cmp_history history;
};

/**
 * Read the options and operands into `c`: the manifests run from the fourth
 * operand to `--`, and the histories follow it. A check needs `--`.
 *
 * @return 0, or -1 for a wrong usage.
 */
static int parse(int argc, char **argv, struct command *c)
{
    int i = 1, end;

    c->schemas = "tools/etude/schemas";
    for (;;) {
        if (argc - i < 3)
            return -1;
        if (argv[i][0] != '-')
            break;
        if (!strcmp(argv[i], "-s"))
            c->schemas = argv[i + 1];
        else if (strcmp(argv[i], "--routine-proofs") || etude_routine_proof_directory(argv[i + 1]))
            return -1;
        i += 2;
    }
    c->verb = argv[i];
    c->policy_path = argv[i + 1];
    c->subject = argv[i + 2];
    for (end = i + 3; end < argc && strcmp(argv[end], "--") != 0; end++) {
    }
    c->manifest_paths = argv + i + 3;
    c->manifest_count = end - (i + 3);
    c->history_paths = end < argc ? argv + end + 1 : NULL;
    c->history_count = end < argc ? argc - end - 1 : 0;
    if (strcmp(c->verb, "forecast") == 0) {
        return c->manifest_count > 0 ? 0 : -1;
    }
    return strcmp(c->verb, "check") == 0 && c->history_count > 0 ? 0 : -1;
}

/** Read the policy, the manifests and the histories of `c`; 0 or -1. */
static int load(struct command *c)
{
    char err[1200], schema[4200];
    int i;

    if (etude_budget_policy_read(c->schemas, c->policy_path, &c->policy, err, sizeof(err)) != 0) {
        fprintf(stderr, "etude estimate: %s\n", err);
        return -1;
    }
    c->manifests = calloc((size_t)c->manifest_count + 1, sizeof(*c->manifests));
    if (c->manifests == NULL) {
        fprintf(stderr, "etude estimate: out of memory\n");
        return -1;
    }
    for (i = 0; i < c->manifest_count; i++) {
        if (etude_budget_manifest_read(c->schemas, c->manifest_paths[i], &c->manifests[i], err,
                                       sizeof(err)) != 0) {
            fprintf(stderr, "etude estimate: %s\n", err);
            return -1;
        }
    }
    snprintf(schema, sizeof(schema), "%s/history.schema.json", c->schemas);
    return cmp_load(&c->history, schema, c->history_paths, c->history_count);
}

/**
 * Print the lines of report `r` and its verdict, and say when the run's
 * record is not `agreed`; give the exit status they earn.
 */
static int print_report(const struct etude_budget_report *r, int agreed)
{
    if (r->lines.data != NULL) {
        fputs(r->lines.data, stdout);
    }
    printf("etude estimate: result: %s%s\n", etude_budget_verdict_name(r->verdict),
           agreed ? "" : "; the run records another budget");
    return agreed && (r->verdict == ETUDE_BUDGET_PASS || r->verdict == ETUDE_BUDGET_EXEMPT) ? 0 : 1;
}

/** Forecast the charge of `c`'s manifests in its class; give the status. */
static int forecast(struct command *c)
{
    const struct etude_budget_limits *l;
    struct etude_budget_forecast f;
    char a[32], b[32], d[32];
    int cls = etude_budget_class_of(c->subject), status;

    if (cls < 0) {
        fprintf(stderr, "etude estimate: %s: not a class of run\n", c->subject);
        return 2;
    }
    if (etude_budget_forecast(&c->policy, (enum etude_budget_class)cls, &c->history, c->manifests,
                              (size_t)c->manifest_count, &f) != 0) {
        fprintf(stderr, "etude estimate: the forecast failed: out of memory\n");
        etude_budget_report_free(&f.report);
        return 2;
    }
    l = &c->policy.limits[cls];
    cmp_seconds(f.prep_us, a);
    cmp_seconds(f.cases_us, b);
    cmp_seconds(l->ceiling_us - l->reserve_us, d);
    printf("forecast of %s on %s, %s, %s: %lu items, %lu nodes\n", c->subject, c->policy.machine_id,
           c->policy.os, c->policy.accelerator, (unsigned long)f.items, (unsigned long)f.nodes);
    printf("preparation recorded: %s\ncases recorded: %s\ndeadline: %s\n", a, b, d);
    cmp_seconds(l->goal_us, a);
    cmp_seconds(l->growth_us, b);
    cmp_seconds(l->ceiling_us, d);
    printf("goal: %s\ngrowth: %s\nceiling: %s\n", a, b, d);
    status = print_report(&f.report, 1);
    etude_budget_report_free(&f.report);
    return status;
}

/** Check the budget of run `c->subject`; give the exit status. */
static int check(struct command *c)
{
    const struct etude_budget_limits *l;
    struct etude_budget_judgement j;
    char a[32], b[32], d[32];
    int status;

    if (etude_budget_judge(&c->history, &c->policy, c->subject, c->manifests,
                           (size_t)c->manifest_count, &j) != 0) {
        fprintf(stderr, "etude estimate: %s\n", j.error);
        etude_budget_report_free(&j.report);
        return 2;
    }
    l = &c->policy.limits[j.cls];
    cmp_seconds(j.charged_us, a);
    cmp_seconds(j.one_time_us, b);
    cmp_seconds(l->ceiling_us, d);
    printf("budget of run %s, plan %s, %s\n", c->subject, j.plan_id,
           etude_budget_class_name(j.cls));
    printf("charge: %s\none-time setup: %s\nceiling: %s\n", j.charged_known ? a : "unknown",
           j.charged_known ? b : "unknown", d);
    status = print_report(&j.report, j.agrees);
    etude_budget_report_free(&j.report);
    return status;
}

/**
 * Run `etude estimate`, as usage_text gives it. `forecast` forecasts the
 * charge of a run of CLASS that selects every case of the manifests, from
 * the costs the histories record on the policy's site. `check` judges the
 * budget of run RUN from its records, and, with the manifests of its cases,
 * its preparation of their nodes. Each prints a line for each reason and its
 * verdict.
 *
 * The exit status is 0 for a pass or an exemption, 1 for a failure, an
 * unknown verdict, or a run whose recorded budget is not the judgement's,
 * and 2 for a wrong usage, a policy, manifest or history that cannot be
 * read, or a run that cannot be judged.
 */
int etude_budget_main(int argc, char **argv)
{
    struct command c;
    int status = 2, i;

    memset(&c, 0, sizeof(c));
    etude_routine_proof_directory(NULL);
    if (parse(argc, argv, &c) != 0) {
        fputs(usage_text, stderr);
        return 2;
    }
    if (load(&c) == 0) {
        status = strcmp(c.verb, "forecast") == 0 ? forecast(&c) : check(&c);
    }
    for (i = 0; c.manifests != NULL && i < c.manifest_count; i++) {
        json_free(c.manifests[i]);
    }
    free(c.manifests);
    cmp_free(&c.history);
    etude_budget_policy_free(&c.policy);
    fflush(stdout);
    return status;
}
