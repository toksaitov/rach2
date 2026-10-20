/*
 * legacy_cases.h: the host cases of a manifest as the set driver runs them,
 * and the judge of one attempt of such a case, `casecheck attempt`.
 */
#ifndef ETUDE_LEGACY_CASES_H
#define ETUDE_LEGACY_CASES_H

#include <stddef.h>
#include <stdint.h>

#include "etude/watchdog.h"
#include "json.h"

/* The seconds casecheck may take to judge an attempt before it is killed. */
#define LEGACY_JUDGE_S 60u

/* One case of a manifest that runs on the run's architecture. Its texts
 * point into the manifest's document. */
struct legacy_case {
    const struct json *json;
    const char *case_id;
    const char *arch; /* the item's architecture: the run's, or "none" */
    long long reps;   /* the workload's repetitions */
    int seeded;       /* the workload's seed is not null */
    long long seed;
    int scratch;  /* its isolation has the `scratch` fixture */
    char **goals; /* its prepare artifacts as make goals of the tree,
                     `build/NODE` or `build/x86_64/NODE`, from malloc */
    size_t ngoals;
    size_t program;    /* the index of the goal its program names */
    const char **args; /* the workload's arguments */
    size_t nargs;
    uint64_t case_us, gap_us; /* the initial bounds of the case and the gap */
    struct etude_witness_bound *witnesses;
    size_t nwitnesses;
    uint64_t cpu_millis, memory_bytes, disk_bytes;
    struct jbuf workload, inventory, profile; /* the canonical documents */
};

/* A manifest of host cases, as one suite of a set names it. */
struct legacy_manifest {
    char *path; /* as the driver reads it, from malloc */
    struct json *doc;
    const char *family; /* its family's identity */
    struct legacy_case *cases;
    size_t ncases;
    char why[400]; /* why the manifest was refused */
};

/**
 * Read the manifest at `path`, relative to the tree `tree` unless it is
 * absolute, for suite `suite` on `arch` (i686 or x86_64).
 *
 * The last part of its family's identity must be the suite's name, at least
 * one case must run on `arch`, and each that does must be a host case whose
 * program is an artifact of its `prepare`, with no standard input, ports,
 * loop devices or host locks, and no fixture but `scratch`. The schema's
 * other rules are the caller's to check. The caller frees `m` with
 * legacy_manifest_free whether or not the manifest was taken.
 *
 * @return 0 with `m` filled; -1 with the reason in `m->why`, which begins
 *         with the manifest's path.
 */
int legacy_manifest_read(struct legacy_manifest *m, const char *tree, const char *path,
                         const char *suite, const char *arch);

/** Free what `m` holds. */
void legacy_manifest_free(struct legacy_manifest *m);

/* The outcomes of a judgement, as casecheck's exit status gives them. */
enum legacy_judged {
    LEGACY_UNJUDGED = -1, /* casecheck could not judge the attempt */
    LEGACY_PASS = 0,
    LEGACY_FAILURE = 1,
    LEGACY_INCOMPLETE = 3
};

/* One attempt of a case to judge, as casecheck's arguments give it. */
struct legacy_judge {
    const char *casecheck; /* casecheck's path */
    const char *schemas;
    const char *manifest;
    const char *case_id;
    const char *arch;   /* the item's architecture */
    const char *output; /* the program's output */
    const char *run_id, *attempt_id, *item_id;
    const char *end;     /* exited, signaled, stopped or lost */
    int value;           /* the status or the signal, or -1 for none */
    const char *results; /* the file of casecheck's standard output */
    const char *errors;  /* the file of its standard error */
};

/* What casecheck gave. */
struct legacy_judgement {
    enum legacy_judged outcome;
    char *doc; /* the results document, from malloc, or NULL */
    size_t len;
    char reason[400]; /* the assertion that decides an attempt that did not
                         pass, or why casecheck could not judge it */
};

/**
 * Judge the attempt `j` names by `casecheck attempt`, run with no input and
 * killed after LEGACY_JUDGE_S seconds. Its document is kept only when it
 * exited 0, 1 or 3 and the document parses.
 */
void legacy_judge_run(const struct legacy_judge *j, struct legacy_judgement *out);

/** Remove the tree at `path`, not following links; 0 once it is gone. */
int legacy_remove_tree(const char *path);

#endif
