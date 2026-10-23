/*
 * legacy.h: the preparation adapter, which builds the goals that the suites
 * of tests/run read before one starts and records those artifacts, and the
 * set driver, which runs the suites of one run of tests/run by their cases
 * through the machine's ledger and writes the run's history.
 * tools/etude/Readme.md describes them.
 */
#ifndef ETUDE_LEGACY_H
#define ETUDE_LEGACY_H

#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* The bound of one step of a preparation when the caller gives none, in
 * seconds: the GNU system's root takes over an hour to build from nothing. */
#define ETUDE_LEGACY_SECONDS 14400u

/* The digests file of a tree, from its root, which the preparation, its
 * check and the set's store share across runs. */
#define ETUDE_LEGACY_DIGESTS "build/tests/digests"

/* One preparation, as the caller asks for it. */
struct etude_legacy_opts {
    const char *tree;   /* the root of the tree whose goals are built */
    const char *arch;   /* i686, x86_64, or both as i686,x86_64 */
    const char *kernel; /* NAME of the kernel rach2-NAME the suites boot, as
                           RACH2_KERNEL names it, or NULL for rach2 */
    const char *work;   /* the work directory, which must not exist, or
                           NULL for a new one under TMPDIR */
    const char *record; /* the file the record is written to, or NULL */
    unsigned seconds;   /* the bound of each step */
    char *const *goals; /* the make goals of the tree, such as
                           build/tools/verify/prosecheck, built and recorded
                           as artifacts; one at least */
    size_t ngoals;
    FILE *out;                   /* the progress and the result */
    volatile sig_atomic_t *stop; /* the number of a stop signal once one has
                                    come, or NULL */
};

/**
 * Build the goals `o->goals` of the tree `o->tree`, which the caller holds,
 * and record them.
 *
 * The goals' names are checked before anything is built, and a request of
 * no goal is refused. The steps then run one at a time through the
 * executor, each with its output in a log in the work directory and
 * cancelled at its bound: the path, the recovery key when the tree has a
 * recovery system and no key, the components, the kernel `o->kernel` names,
 * and last the goals. Every goal, and the kernel the suites boot, must then
 * exist; each regular file is hashed into the record. A preparation of both
 * architectures makes the components, the kernel and the goals of i686 and
 * then those of x86_64, each with its own ARCH, the goals under
 * `build/x86_64/` being x86_64's and the others i686's, and writes one
 * record of both. An i686 request that includes a fixed `build/x86_64/`
 * artifact also prepares that architecture and records both. Shared goals
 * a request for x86_64 alone still use ARCH=x86_64. The work directory is
 * removed after a success and after a stop, and kept after a failure.
 *
 * @return 0 when every artifact exists; 1 when a step failed or an artifact
 *         is missing; 2 for a request refused before anything was built or
 *         an error of the system; 128 plus the signal's number after a stop.
 */
int etude_legacy_prepare(const struct etude_legacy_opts *o);

/**
 * Check that the artifacts the record at `record` lists are as it recorded
 * them: by their size, inode and modification time, or with `digests`
 * nonzero by their size and SHA-256.
 *
 * @return 0 when every artifact is unchanged; 1 when one changed or is
 *         missing; 2 for a record that cannot be read or is malformed.
 */
int etude_legacy_check(const char *record, int digests, FILE *out);
/* Check a deployed record against TREE instead of its production path. */
int etude_legacy_check_tree(const char *record, const char *tree, int digests, FILE *out);

/* One suite of a set, as the caller selects it. */
struct etude_legacy_member {
    const char *suite;
    int serial;        /* it runs before the others, with nothing beside it */
    int stops;         /* its failure cancels the suites not yet started */
    const char *cases; /* the manifest whose host, guest or machine cases run
                          the suite, from the tree's root unless absolute */
    const char *arch;  /* the one architecture of the set's that it runs on,
                          or NULL for each of them */
};

/* The resources of a machine's ledger that a set's claims take. */
enum etude_legacy_resource { ETUDE_LEGACY_CPU, ETUDE_LEGACY_MEMORY, ETUDE_LEGACY_DISK };

/* One run of a set of suites, as the caller asks for it. */
struct etude_measurement_context;
struct etude_legacy_set_opts {
    const struct etude_measurement_context *context; /* verified measurement inputs */
    const char *run_id; /* preallocated identity, or NULL for a correctness run */
    const char *tree;    /* the root of the tree, which the caller holds */
    const char *arch;    /* i686, x86_64, or both as i686,x86_64 */
    const char *kernel;  /* as for a preparation, or NULL */
    const char *program; /* the path of etude, which runs the steps and judges */
    const struct etude_legacy_member *members;
    size_t nmembers;
    const char *ledger;         /* the absolute directory of the machine's ledger */
    const char *account;        /* the account the claims are made for */
    uint64_t cap[3];            /* the ledger's limits, by enum etude_legacy_resource */
    const char *locks;          /* the absolute directory of the old runner's host
                                   and port locks, which each claim takes; NULL
                                   for none */
    const char *native_locks;   /* collection-wide durable host and port locks */
    int native_ports;           /* the collection needs one compatibility block */
    int host_alone;             /* hold those host locks exclusively */
    unsigned wait;              /* the seconds a request may wait for admission */
    unsigned seconds;           /* the bound of each step of the preparation */
    uint64_t pool_memory_bytes; /* this run's pool budget; 0 uses the ledger alone */
    unsigned width;             /* the most cases at once; 0 for the ledger's limits alone */
    unsigned cpus;              /* each machine's processors, as RACH2_CPUS gives them */
    const char *accel;          /* QEMU's accelerator, as RACH2_QEMU_ACCEL gives it */
    int prepared;               /* artifacts already exist; validate the record, never run make */
    char *history_path;         /* optional caller buffer receiving the new stream path */
    size_t history_path_size;
    const char *history;         /* matching history directory, or results/history */
    int stop_on_fail;            /* the first case that does not pass cancels the rest */
    const char *results;         /* the directory of the history's `history/` and
                                    `store/`, and of each command's output in
                                    `output/` */
    const char *schemas;         /* the directory of etude's schemas */
    const char *record;          /* the file of the preparation's record, or NULL for
                                    `prepare.record` under `results` */
    const char *evidence;        /* the directory under which each case has its
                                    workspace, kept when it does not pass; the
                                    store of the guest and machine cases'
                                    artifacts is made in it for the run */
    const char *status;          /* the status record, rewritten as the run goes, or NULL */
    const char *stamp;           /* a file touched once the preparation has ended, or NULL */
    const char *commit;          /* the tree's commit, or NULL when it has none */
    const char *args;            /* what the run was asked, for the status record */
    const char *site;            /* the installed site policy that admitted the run,
                                    or NULL for none */
    FILE *out;                   /* the progress, and each case's output */
    volatile sig_atomic_t *stop; /* as for a preparation */
};

/**
 * Run the suites `o->members` of the tree as one run of etude, each by the
 * host, guest and machine cases of its manifest: prepare their artifacts,
 * admit each repetition of each case through the machine's ledger, run it
 * under the case's own watchdog, and write the run's history.
 *
 * The cases of a member are those of its manifest that run on `o->arch` or
 * on none, each repetition of each a member of the run that keeps the
 * suite's `serial`. A set of both architectures runs each member on each, or
 * on the one its `arch` names, as one run with one preparation of both: a
 * case runs on each of its architectures, and a case of none runs once,
 * with the suite's first architecture; each name of a case's files and its
 * label give its architecture. Their artifacts are preparation goals; with
 * `o->kernel`, the kernel that a guest case boots from the node
 * `rach2/kernel` is `rach2-KERNEL/kernel` in its place. The preparation is a
 * claim of every limited resource of the ledger, with the old runner's host
 * lock alone. Each case is then a claim of its own, of the case's
 * resources, each held within the limit. With `o->locks`, its claim also
 * takes the old host lock, shared, or alone for a guest case whose memory is
 * more than half the limit, and a guest case's claim takes a port block as
 * well. The serial members run first, one at a time; the others run in the
 * order given, or for both architectures in etude_sched_order's, the
 * costlier case first, so that the long cases of the two run side by side,
 * as many at once as the ledger and `o->width` admit. A suite
 * that `stops` and did not pass cancels what runs and what has not started,
 * once every member of the suite has ended; with `o->stop_on_fail`, the
 * first member that does not pass does so at once.
 *
 * Each case runs in a workspace of its own under `o->evidence`, under the
 * case's initial bounds, as etude_bound_fix fixes them without history:
 * a host case's program with ETUDE_TREE naming the tree, a guest case's program
 * on a machine of its QEMU profile, through etude-guest, with the memory the
 * launcher reserves for the machine as its claim, and a machine case's scenario
 * with its invocation, after its artifacts are published to the run's store.
 * The invocation permits the lazy boundary memory profile only when the policy
 * `o->site` has the line `boundary_lazy bigmem-boundary-v1`; a machine case
 * that declares such a machine ends before its scenario starts when the policy
 * lacks it, or when the ledger's memory limit is less than the case's
 * `memory_bytes`. `o->program check attempt` judges each attempt, a guest's
 * placed `qemu` with the machine's console and a machine case's with the
 * consoles its scenario kept. A member with no manifest is refused, and so is a
 * manifest that cannot be read, of another suite's family, with no case on the
 * architecture, or with a case that the set driver cannot run
 * (legacy_manifest_read), and a request whose `o->site` cannot be read.
 *
 * With `prepared`, validate the existing record and artifact digests without
 * running make, and execute compiled host programs directly with complete
 * output files. `native_locks` keeps the collection's locks until all claims
 * of the run have been released. `history` supplies earlier matching runs.
 *
 * The history is one stream under `o->results`, with the plan of one item
 * for each repetition of each case, a step for the preparation, an attempt
 * record for each member started, and the run record; its blobs go to the
 * store. A case's output is printed whole when it ends, or as it comes when
 * the set has one member, and the output of the cases a stop interrupted is
 * printed with what they wrote. The status record, when asked for, gives the
 * claims that wait and the attempts that run.
 *
 * @return 0 when every case passed; 1 when one did not, or when the run
 *         could not go on after its plan was written; 2 for a request
 *         refused before anything ran; 3 when the preparation failed; 128
 *         plus the signal's number after a stop.
 */
int etude_legacy_selection_export(const struct etude_legacy_set_opts *o, const char *path);

int etude_legacy_set(const struct etude_legacy_set_opts *o);

#endif
