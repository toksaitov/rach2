/*
 * budget.h: the budget of routine testing on the selected machine. Its
 * policy gives each class of run its limits on the site's reference
 * capacity; a forecast charges a selection's recorded costs against them, and
 * a judgement charges a run's records. tools/etude/Readme.md describes them.
 */
#ifndef ETUDE_BUDGET_H
#define ETUDE_BUDGET_H

#include <stddef.h>
#include <stdint.h>

#include "compare.h"
#include "routine_charge.h"
#include "etude/sched.h"
#include "json.h"

/* The policy this code implements, which a policy document names. */
#define ETUDE_BUDGET_POLICY "budget-v1"

/* The resources of the reference capacity, as the ledger names them. */
#define ETUDE_BUDGET_RESOURCES 3

/* The classes of run, by the purpose of their plan. */
enum etude_budget_class {
    ETUDE_BUDGET_PUSH,
    ETUDE_BUDGET_CI,
    ETUDE_BUDGET_RELEASE,
    ETUDE_BUDGET_FOCUSED,
    ETUDE_BUDGET_FIRST_RUN,
    ETUDE_BUDGET_CLASSES
};

/* A budget's verdict, as the history's budget records it. */
enum etude_budget_verdict {
    ETUDE_BUDGET_PASS,
    ETUDE_BUDGET_FAILURE,
    ETUDE_BUDGET_EXEMPT,
    ETUDE_BUDGET_UNKNOWN
};

/* The limits of one class, in microseconds. The deadline of a run's work is
 * the ceiling less the cleanup reserve. */
struct etude_budget_limits {
    uint64_t goal_us, growth_us, ceiling_us, reserve_us;
};

/* A policy document, read and checked. */
struct etude_budget_policy {
    struct json *doc;
    char sha256[65]; /* of the document's bytes, as a run's budget names it */
    uint64_t size_bytes;
    const char *machine_id, *os, *accelerator; /* the site */
    struct etude_ledger_res capacity[ETUDE_BUDGET_RESOURCES];
    size_t ncapacity;
    struct etude_budget_limits limits[ETUDE_BUDGET_CLASSES];
    const struct json *one_time; /* the steps whose first run is reported apart */
};

/* What a forecast or a judgement found: the verdict, and a line for each
 * reason, each ending in a newline. */
struct etude_budget_report {
    enum etude_budget_verdict verdict;
    struct cmp_text lines;
    size_t unknown; /* the reasons that leave a cost or the work unknown */
    size_t failed;  /* the reasons that fail the budget */
};

/* A forecast of a selection's charged time on the site. */
struct etude_budget_forecast {
    struct etude_budget_report report;
    size_t items, nodes;        /* the selection's items and preparation nodes */
    uint64_t prep_us, cases_us; /* the charges of the costs recorded */
};

/* A judgement of a run's charged time from its records. */
struct etude_budget_judgement {
    struct etude_budget_report report;
    enum etude_budget_class cls;
    const char *plan_id;
    int charged_known; /* the charged time has a value */
    uint64_t charged_us, one_time_us;
    int agrees;      /* the recorded budget agrees; unknown is unevaluated */
    char error[600]; /* why the records cannot be judged, when that is so */
};

struct etude_preparation_selection {
    const char *const *nodes;
    size_t count;
    unsigned architectures;
    enum etude_budget_class cls;
};
int etude_budget_preparation(const struct cmp_history *h, const struct etude_budget_policy *p,
                             const struct etude_preparation_selection *selection, uint64_t *us,
                             size_t *count);
/* Release costs require actual clean production of the selected goal. */
int etude_budget_preparation_eligible(const struct cmp_record *plan, enum etude_budget_class cls,
                                      const char *node);
int etude_budget_routine(const struct cmp_record *plan, const struct cmp_record *run,
                         struct etude_routine_cost *cost);

int etude_budget_class_of(const char *name);
const char *etude_budget_class_name(enum etude_budget_class cls);
const char *etude_budget_verdict_name(enum etude_budget_verdict verdict);
int etude_budget_policy_read(const char *schemas, const char *path, struct etude_budget_policy *p,
                             char *err, size_t errlen);
void etude_budget_policy_free(struct etude_budget_policy *p);
int etude_budget_manifest_read(const char *schemas, const char *path, struct json **doc, char *err,
                               size_t errlen);
int etude_budget_schedule(struct etude_job *jobs, size_t n, const struct etude_ledger_res *cap,
                          size_t ncap, uint64_t *makespan_us);
int etude_budget_forecast(const struct etude_budget_policy *p, enum etude_budget_class cls,
                          const struct cmp_history *h, struct json *const *manifests, size_t n,
                          struct etude_budget_forecast *f);
int etude_budget_judge(const struct cmp_history *h, const struct etude_budget_policy *p,
                       const char *run_id, struct json *const *manifests, size_t n,
                       struct etude_budget_judgement *j);
void etude_budget_report_free(struct etude_budget_report *r);
int etude_budget_main(int argc, char **argv);

#endif
