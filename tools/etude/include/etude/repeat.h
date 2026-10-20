/*
 * repeat.h: shuffled and loaded repetitions. A seed fixes which cases a run
 * selects, the order of their repetitions and the load beside each; the
 * repetition record keeps the seed and what it gave; the driver runs the
 * plan through the ledger under watchdogs, retries an attempt that did not
 * pass without erasing it, and writes the history. tools/etude/Readme.md
 * describes it.
 */
#ifndef ETUDE_REPEAT_H
#define ETUDE_REPEAT_H

#include <stddef.h>
#include <stdint.h>

#include "etude/exec.h"
#include "etude/history.h"
#include "etude/ledger.h"
#include "etude/watchdog.h"

/* The policy by which etude_repeat_plan draws a plan from its seed. */
#define ETUDE_REPEAT_POLICY "repeat-splitmix64-v1"

/* The largest seed, as a JSON integer of the history may hold it. */
#define ETUDE_REPEAT_SEED_MAX UINT64_C(9007199254740991)

/* The load of an item that runs beside none. */
#define ETUDE_REPEAT_NO_LOAD ((size_t)-1)

/* The line a load fixture writes on its standard output once its load is in
 * effect. */
#define ETUDE_LOAD_READY "etude-load: ready\n"

/* The generator of a plan: splitmix64. */
struct etude_rng {
    uint64_t state;
};

/** Start generator `r` at `seed`. */
void etude_rng_seed(struct etude_rng *r, uint64_t seed);

/** Give the next 64 bits of generator `r`. */
uint64_t etude_rng_next(struct etude_rng *r);

/**
 * Give a number below `n`, each value as likely as the others: the next
 * output of `r` modulo `n`, drawn again while it falls in the incomplete
 * last span of 2^64 values. `n` of 0 gives 0 and draws nothing.
 */
uint64_t etude_rng_below(struct etude_rng *r, uint64_t n);

/* What a load fixture applies. */
enum etude_load_kind {
    ETUDE_LOAD_CPU,    /* `amount` processes that spin */
    ETUDE_LOAD_MEMORY, /* `amount` bytes allocated and written */
    ETUDE_LOAD_DISK    /* `amount` bytes written and synchronized, again and again */
};

/* A load fixture that a repetition may run beside its case. Admission
 * reserves `amount` thousand `cpu_millis`, or `amount` `memory_bytes` or
 * `disk_bytes`, for it. */
struct etude_load {
    const char *load_id;
    enum etude_load_kind kind;
    uint64_t amount;
};

/* A case to repeat. The plan reads the first three members; the driver runs
 * the rest. */
struct etude_repeat_case {
    const char *case_id;
    const char *arch;     /* i686, x86_64 or none */
    unsigned repetitions; /* the case's items in the run, at least 1 */
    const char *family_id;
    char *const *argv; /* argv[0] is the program's path */
    char *const *envp;
    const char *cwd; /* NULL keeps the driver's */
    size_t output_max;
    uint64_t grace_us;
    const struct etude_watchdog_spec *wd;
    const struct etude_ledger_res *res; /* what admission reserves */
    size_t nres;
    /* The history's blob references, as JSON, of its workload, assertion
     * inventory and profile, and the inventory's digest. */
    const char *workload;
    const char *assertions;
    const char *profile;
    const char *inventory_sha256;
};

/* What a run of repetitions is asked to do. */
struct etude_repeat_spec {
    uint64_t seed;         /* at most ETUDE_REPEAT_SEED_MAX */
    size_t pick;           /* the cases selected; 0 for all */
    unsigned load_percent; /* the share of items run beside a load, 0 to 100 */
    unsigned width;        /* the cases run at once, at least 1 */
    unsigned retries;      /* the attempts after the first of an item that did not pass */
    const struct etude_repeat_case *cases;
    size_t ncases;
    const struct etude_load *loads;
    size_t nloads;
};

/* One item of a plan, in the order of the run. */
struct etude_repeat_item {
    size_t case_index; /* into the spec's cases */
    unsigned repetition;
    size_t load; /* into the spec's loads, or ETUDE_REPEAT_NO_LOAD */
};

/**
 * Draw the plan of `spec` from its seed, by the policy ETUDE_REPEAT_POLICY.
 *
 * One generator, started at the seed, makes three choices in turn. The
 * selection: with `pick` below the count of cases, the first `pick` places
 * of a partial Fisher-Yates shuffle of the cases' indices, kept in the
 * spec's order; otherwise every case. The order: each selected case's
 * repetitions, in the spec's order, then shuffled by Fisher-Yates from the
 * last place down, the place `i` exchanged with the place below `i + 1`.
 * The loads: for each item in that order, a number below 100, and when it is
 * below `load_percent` and there are loads, the load below their count.
 *
 * @return 0 with `*n` items in `*items`, which the caller frees; or -1 with
 *         errno EINVAL for a seed past ETUDE_REPEAT_SEED_MAX, a share past
 *         100, no case selected or a case without repetitions, EOVERFLOW
 *         for too many items, or ENOMEM.
 */
int etude_repeat_plan(const struct etude_repeat_spec *spec, struct etude_repeat_item **items,
                      size_t *n);

/**
 * Write into `buf` the identity of the item of run `run_id` that is
 * repetition `rep` of case `c`: `RUN:CASE:ARCH:rREP`.
 *
 * @return 0, or -1 with errno ERANGE when it does not fit `size` or is past
 *         the 255 characters of a history identity.
 */
int etude_repeat_item_id(const char *run_id, const struct etude_repeat_case *c, unsigned rep,
                         char *buf, size_t size);

/**
 * Make the repetition record of run `run_id`: the policy, the seed, what the
 * plan was drawn from, and the `n` items of `items` in their order, each
 * with its identity and load. The record is a document of
 * schemas/repeat.schema.json, and the plan's `selection` and `concurrency`.
 *
 * @return the record, which the caller frees, or NULL with errno set.
 */
char *etude_repeat_record(const struct etude_repeat_spec *spec, const char *run_id,
                          const struct etude_repeat_item *items, size_t n);

/**
 * Check repetition record `json` against the schema at `schema_path`, draw
 * its plan again from its seed and what it lists, and require the items it
 * gives to be those, in that order.
 *
 * @return 0, or -1 with the first fault in `why`.
 */
int etude_repeat_check(const char *json, const char *schema_path, char *why, size_t size);

/* How an attempt ended, as the history's outcome gives it. */
enum etude_repeat_outcome {
    ETUDE_REPEAT_PASS,
    ETUDE_REPEAT_FAILURE,
    ETUDE_REPEAT_INCOMPLETE,
    ETUDE_REPEAT_CANCELLED
};

/* The attempts of one case and workload on one architecture, as the history
 * gives them. */
struct etude_repeat_tally {
    char case_id[256];
    char arch[8];
    char workload[65]; /* the workload blob's digest */
    unsigned pass, failure, incomplete, cancelled;
    unsigned disrupted; /* attempts with a disruption, counted apart */
    unsigned retries;   /* attempts that retry an earlier one */
    int flaky;          /* undisrupted attempts that passed, and others that
                           failed or did not finish */
};

struct cmp_history;

/**
 * Tally the attempts of the history `h`, those of run `run_id` alone when it
 * is not NULL, by case, architecture and workload. Every attempt counts,
 * retries and the attempts they retry alike; an attempt with a disruption
 * counts as disrupted alone, since its conditions were not those declared.
 *
 * @return 0 with `*n` tallies in `*out`, which the caller frees, sorted by
 *         case, architecture and workload; or -1 when memory ran out.
 */
int etude_repeat_tally(const struct cmp_history *h, const char *run_id,
                       struct etude_repeat_tally **out, size_t *n);

/* ---- The driver, on POSIX systems. ---- */

/* Where and how a run of repetitions runs. */
struct etude_repeat_site {
    const char *run_id;
    struct etude_ledger *ledger;
    const char *account;
    const struct etude_ledger_res *cap; /* the machine's limits */
    size_t ncap;
    char *const *load_argv; /* the load program and the arguments before its
                               kind and amount */
    char *const *load_envp;
    const char *scratch;    /* the loads' working directory */
    uint64_t load_ready_us; /* the bound on a load's start */
    uint64_t load_grace_us;
    struct etude_history *history; /* the run's stream, which the caller opened */
    const char *store;             /* the directory of blobs, each named by its SHA-256 */
    /* The members of the plan record the caller fixes, as JSON members
     * without braces: purpose, commit, source, runner, artifacts,
     * environment, policies, exemption and reproduce. */
    const char *plan_fixed;
    const char *machine_id; /* the domain of the monotonic clock */
    const char *boot_id;
    const char *budget_policy; /* the budget policy's blob reference, as JSON */
};

/* One attempt of a run. */
struct etude_repeat_attempt {
    size_t item;  /* into the plan's items */
    unsigned try; /* 0 for the planned attempt, K for its Kth retry */
    char attempt_id[272];
    char record_id[256];
    enum etude_repeat_outcome outcome;
    char reason[200];
    char disruption[200]; /* "" when none */
    enum etude_end end;
    int value;
    enum etude_cleanup cleanup;
    struct etude_measure cpu_us;
    /* Monotonic times: the case's start and its end, and its load's
     * readiness and end, 0 when not seen. */
    uint64_t started_us, ended_us;
    uint64_t load_ready_us, load_ended_us;
    enum etude_cleanup load_cleanup;
    struct etude_measure load_cpu_us;
};

/* What a run of repetitions did. */
struct etude_repeat_result {
    struct etude_repeat_item *items;
    size_t nitems;
    struct etude_repeat_attempt *attempts; /* in the order they were planned */
    size_t nattempts;
    enum etude_repeat_outcome outcome;
    enum etude_cleanup cleanup;
    char plan_record[256];
    char run_record[256];
};

/**
 * Run the plan of `spec` on `site`: write the repetition record to the store
 * and the plan record to the history, then run the items in the plan's
 * order, `width` cases at once, and each attempt's retries after the items.
 *
 * Each attempt is admitted through the ledger, its load first and then its
 * case, in the plan's order, and waits for room. A load starts before its
 * case, which starts only once the load has written ETUDE_LOAD_READY, and
 * is cancelled after its case has ended. The case runs under its watchdog
 * and the load under the load's bounds. An attempt passes when its case
 * exits 0 within its bounds and its cleanup is confirmed; a load that ends
 * before its case is a disruption of the attempt. An attempt that does not
 * pass is retried, under the same load, until `retries` more attempts have
 * run; each retry names the attempt it repeats, and none replaces it.
 *
 * Each attempt's results document and the run's totals go to the store, and
 * an attempt record for each attempt and the run record to the history. An
 * item's outcome is the worst of its attempts', and the run's the worst of
 * its items'. A claim whose cleanup is unconfirmed stays in the ledger.
 *
 * @return 0 with what ran in `*r`, which etude_repeat_result_free frees; or
 *         -1 with errno set when the plan, the ledger, the store or the
 *         history failed, after which the attempts that ran have been ended.
 */
int etude_repeat_run(const struct etude_repeat_spec *spec, const struct etude_repeat_site *site,
                     struct etude_repeat_result *r);

/** Free what etude_repeat_run gave. */
void etude_repeat_result_free(struct etude_repeat_result *r);

/**
 * Be the load fixture: `argv` is `etude-load KIND AMOUNT`, KIND `cpu`,
 * `memory` or `disk`. It applies the load, writes ETUDE_LOAD_READY on its
 * standard output with one write(2), and keeps the load until it is killed.
 * A disk load writes a file it has unlinked in its working directory.
 *
 * @return 2 for a wrong usage, or 1 when the load cannot be applied.
 */
int etude_load_main(int argc, char **argv);

#endif
