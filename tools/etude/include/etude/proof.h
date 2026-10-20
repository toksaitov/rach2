/*
 * proof.h: etude's driver of mutation proofs. A proof puts each fault of its
 * catalogue into the sources, builds the artifacts and requires the test to
 * fail as the fault declares, then restores the sources and requires the
 * artifacts the unmodified sources made and the test's pass again.
 * tools/etude/Readme.md describes the catalogue and the driver.
 */
#ifndef ETUDE_PROOF_H
#define ETUDE_PROOF_H

#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* What etude_proof_run returns when a signal stopped it. */
#define ETUDE_PROOF_STOPPED 3

/* One change to a source: `find`, which must occur once, becomes `replace`. */
struct etude_edit {
    char *file; /* one of the proof's sources */
    char *find;
    char *replace;
};

/* One fault and the failure the test must give with it. */
struct etude_fault {
    char *name;
    char *summary;
    struct etude_edit *edit; /* applied in order, each to the text the
                                previous ones left */
    size_t nedits;
    int status;   /* the test's exit status, or 0 for any but 0 */
    char **lines; /* texts each of which some line of the output contains */
    size_t nlines;
    int only; /* nonzero: every line that begins with the test's
                 `fail_prefix` contains one of `lines` */
};

/* A proof, as its catalogue gives it. */
struct etude_proof {
    char *name;
    char *summary;
    int shared;     /* nonzero: the faults go into the tree's own sources;
                       zero: into private copies under ${SRC} */
    char **sources; /* paths relative to the tree */
    size_t nsources;
    char **artifacts; /* paths, with the placeholders of `build` */
    size_t nartifacts;
    char **build; /* the build's arguments, ended by NULL */
    uint64_t build_bound_us;
    char **test; /* the test's arguments, ended by NULL */
    uint64_t test_bound_us;
    char *fail_prefix; /* how a line of the test's failure begins, or NULL */
    struct etude_fault *faults;
    size_t nfaults;
};

/**
 * Read the catalogue at `path`, check it against `proof.schema.json` of
 * directory `schemas` and by the rules a schema cannot state, and give the
 * proof in `*out`.
 *
 * Each finding goes to `log` as `etude-proof: PATH:LINE: WHAT`.
 *
 * @return 0; 1 for a catalogue with findings; 2 for a file that cannot be
 *         read or a schema that cannot be used.
 */
int etude_proof_read(const char *path, const char *schemas, FILE *log, struct etude_proof **out);

/** Free a proof that etude_proof_read gave. */
void etude_proof_free(struct etude_proof *p);

/**
 * Apply edit `e` to `text` of `len` bytes, giving the result from malloc in
 * `*out` and `*outlen`.
 *
 * @return the number of times `e->find` occurs in `text`; the result is
 *         made only when it is 1, and `*out` is otherwise NULL.
 */
size_t etude_edit_apply(const char *text, size_t len, const struct etude_edit *e, char **out,
                        size_t *outlen);

/**
 * Check every fault of `p` against the sources of the tree at `tree`: each
 * edit's `find` occurs once in the text the fault's earlier edits leave.
 *
 * Nothing is written. Each finding goes to `log`.
 *
 * @return 0 when every edit applies; 1 when one does not; 2 for a source
 *         that cannot be read.
 */
int etude_proof_texts(const struct etude_proof *p, const char *tree, FILE *log);

/* How a proof is run. */
struct etude_proof_opts {
    const char *tree;            /* the tree's root, an absolute path */
    const char *work;            /* the work directory, which must not exist, or NULL
                                    for one that mkdtemp(3) makes under $TMPDIR */
    const char *store;           /* the artifact store, or NULL for one in the work
                                    directory */
    char *const *only;           /* the faults to run, ended by NULL, or NULL for all */
    int base;                    /* nonzero: run the base phase alone, no fault */
    int keep;                    /* nonzero: keep the work directory after a pass */
    FILE *log;                   /* where the progress and the outcome go */
    volatile sig_atomic_t *stop; /* set by the caller's signal handler; the
                                    proof then ends what runs, restores the
                                    sources and returns ETUDE_PROOF_STOPPED */
};

/**
 * Run proof `p`: the unmodified sources must build and pass; each fault must
 * change an artifact and make the test fail as it declares; the restored
 * sources must equal the saved ones, build the unmodified artifacts again and
 * pass. With `o->base` set, the unmodified sources are built and tested alone,
 * as a suite runs a test whose build only its proof describes.
 *
 * A build or a test runs through the executor in the tree's root, under its
 * bound, and must end with every process it started. In its arguments and its
 * artifacts' paths, `${TREE}` is the tree's root, `${SRC}` the root the
 * sources are read from (the tree's for a shared proof, a private copy's
 * otherwise), `${WORK}` the work directory and `${PHASE}` `base`,
 * `fault-NAME` or `restored-NAME`. A shared proof requires the tree's run
 * lock to be held by the process RACH2_TREE_HELD names. Each phase's
 * artifacts are put in the store. The sources are restored on every path out,
 * a signal's included; the work directory is removed after a pass or a
 * signal and kept, with each phase's output, after a failure.
 *
 * @return 0 for a proof that passed, 1 for one that failed, 2 for an error
 *         of the system or a tree that is not held, and ETUDE_PROOF_STOPPED.
 */
int etude_proof_run(const struct etude_proof *p, const struct etude_proof_opts *o);

#endif
