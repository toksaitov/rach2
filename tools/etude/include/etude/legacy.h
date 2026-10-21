/*
 * legacy.h: the preparation adapter, which builds what the suites of
 * tests/registry read before a suite starts, as tests/run built it, and
 * records those artifacts; the suite adapter, which runs one prepared
 * suite under the old runner's supervision; and the set driver, which runs
 * the suites of one run of tests/run through the machine's ledger and writes
 * the run's history. tools/etude/Readme.md describes them.
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

/* The seconds a suite may do nothing before it is stopped as hung when the
 * caller gives no limit, as tests/run's RACH2_IDLE_TIMEOUT. */
#define ETUDE_LEGACY_IDLE 300u

/* One preparation, as the caller asks for it. */
struct etude_legacy_opts {
    const char *tree;    /* the root of the tree whose suites are prepared */
    const char *arch;    /* i686 or x86_64 */
    const char *kernel;  /* NAME of the kernel rach2-NAME the suites boot, as
                            RACH2_KERNEL names it, or NULL for rach2 */
    const char *work;    /* the work directory, which must not exist, or
                            NULL for a new one under TMPDIR */
    const char *record;  /* the file the record is written to, or NULL */
    unsigned seconds;    /* the bound of each step */
    char *const *suites; /* the suites' names, as the registry gives them */
    size_t nsuites;
    char *const *goals; /* further make goals of the tree, such as
                           build/tools/verify/prosecheck, built with the
                           suites' artifacts and recorded as artifacts */
    size_t ngoals;
    FILE *out;                   /* the progress and the result */
    volatile sig_atomic_t *stop; /* the number of a stop signal once one has
                                    come, or NULL */
};

/**
 * Prepare the suites `o` names in the tree `o->tree`, which the caller holds.
 *
 * The suites' names, architectures and tools, and the goals' names, are
 * checked before anything is built; with a goal the suites may be none. The
 * steps then run one at a time through the executor, each with its output
 * in a log in the work directory and cancelled at its bound: the recovery
 * key when the tree has a recovery system and no key, the components, the
 * kernel `o->kernel` names, `prepare-all` for more than one suite, and last
 * the goals and the artifacts that each suite's target reaches in make's
 * database and that the steps before did not build. Every artifact, each
 * goal among them, must then exist; each regular file is hashed into the
 * record. The work directory is removed after a success and after a stop,
 * and kept after a failure.
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

/* One run of a prepared suite, as the caller asks for it. */
struct etude_legacy_run_opts {
    const char *record;          /* the record of the suite's preparation */
    const char *suite;           /* the suite's name, which the record lists */
    const char *work;            /* the work directory, which must not
                                    exist, or NULL for a new one under
                                    TMPDIR */
    unsigned idle;               /* the seconds without output, console or
                                    processor time after which the suite
                                    is stopped as hung */
    unsigned seconds;            /* the bound of the whole run */
    int posix_only;              /* nonzero: the executor uses POSIX
                                    interfaces alone, as the tests ask on
                                    Linux */
    FILE *out;                   /* the suite's output, the progress and
                                    the result */
    volatile sig_atomic_t *stop; /* as for a preparation */
};

/**
 * Run suite `o->suite` of the preparation `o->record` records, in the tree,
 * on the architecture and with the kernel that the record names. The caller
 * holds the tree, and the port block that RACH2_PORT_OFFSET names.
 *
 * The record must be unchanged and the suite's commands on the path. The
 * suite's target then runs through the executor under `runutil record`, as
 * tests/run runs it under `runutil suite`, with a shim before each QEMU
 * that records the machine and relays its console. The suite passes only
 * when its target exits 0 by itself, every machine and relay has ended
 * within runutil's settling time, each console was copied whole, nothing
 * outlived the suite, and the cleanup is confirmed. A suite stopped as hung,
 * past its bound, with a console cut, unconfirmed cleanup or no record of
 * its end did not finish. The work directory, with the suite's output, the
 * machines' ledger and consoles and the logs it wrote under the images, is
 * kept when the suite failed or did not finish, and removed otherwise.
 *
 * @return 0 when the suite passed; 1 when it failed or did not finish; 2
 *         for a request refused before the suite started or an error of the
 *         system; 128 plus the signal's number after a stop.
 */
int etude_legacy_run(const struct etude_legacy_run_opts *o);

/* One suite of a set, as the caller selects it from the registry. */
struct etude_legacy_member {
    const char *suite;
    uint64_t memory_mib; /* the most guest memory its machines ask for at once */
    int serial;          /* it runs before the others, with nothing beside it */
    int stops;           /* its failure cancels the suites not yet started */
    const char *cases;   /* the manifest whose host and guest cases run the
                            suite, from the tree's root unless absolute, or
                            NULL for a suite of the registry */
};

/* The resources of a machine's ledger that a set's claims take. */
enum etude_legacy_resource { ETUDE_LEGACY_CPU, ETUDE_LEGACY_MEMORY, ETUDE_LEGACY_DISK };

/* One run of a set of suites, as the caller asks for it. */
struct etude_legacy_set_opts {
    const char *tree;    /* the root of the tree, which the caller holds */
    const char *arch;    /* i686 or x86_64 */
    const char *kernel;  /* as for a preparation, or NULL */
    const char *program; /* the path of etude-legacy, which runs the steps */
    const struct etude_legacy_member *members;
    size_t nmembers;
    const char *ledger;          /* the absolute directory of the machine's ledger */
    const char *account;         /* the account the claims are made for */
    uint64_t cap[3];             /* the ledger's limits, by enum etude_legacy_resource */
    const char *locks;           /* the absolute directory of the old runner's host
                                    and port locks, which each claim takes; NULL
                                    when the suites share the port block of the
                                    caller's RACH2_PORT_OFFSET */
    unsigned wait;               /* the seconds a request may wait for admission */
    unsigned seconds;            /* the bound of the preparation and of each suite */
    unsigned idle;               /* each suite's idle limit, as for a run */
    unsigned width;              /* the most suites at once; 0 for the ledger's limits alone */
    unsigned cpus;               /* each machine's processors, as RACH2_CPUS gives them */
    const char *accel;           /* QEMU's accelerator, as RACH2_QEMU_ACCEL gives it */
    int stop_on_fail;            /* the first suite that does not pass cancels the rest */
    const char *results;         /* the directory of the history's `history/` and
                                    `store/`, and of each command's output in
                                    `output/` */
    const char *schemas;         /* the directory of etude's schemas */
    const char *casecheck;       /* casecheck's path, which judges a case's attempt;
                                    needed when a member has `cases` */
    const char *record;          /* the file of the preparation's record, or NULL for
                                    `prepare.record` under `results` */
    const char *evidence;        /* the directory under which a suite that does not pass
                                    keeps its work, and each case its workspace;
                                    the guest cases' store of artifacts is made in
                                    it for the run */
    const char *status;          /* the status record, rewritten as the run goes, or NULL */
    const char *stamp;           /* a file touched once the preparation has ended, or NULL */
    const char *commit;          /* the tree's commit, or NULL when it has none */
    const char *args;            /* what the run was asked, for the status record */
    FILE *out;                   /* the progress, and each suite's output */
    volatile sig_atomic_t *stop; /* as for a preparation */
};

/**
 * Run the suites `o->members` of the tree as one run of etude: prepare them,
 * admit each through the machine's ledger, run each prepared suite as
 * etude_legacy_run runs one, under a watchdog of the whole suite, and write
 * the run's history.
 *
 * The preparation is a claim of every limited resource of the ledger, with
 * the old runner's host lock alone. Each suite is then a claim of its own:
 * half a processor for each of `o->cpus`, its memory and 128 MiB for QEMU,
 * and a GiB of disk, each held within the limit. With `o->locks`, its claim
 * also takes the old host lock, alone for a suite whose memory is more than
 * half the limit, and a port block, whose offset the suite is given as
 * RACH2_PORT_OFFSET. The serial members run first, one at a time; the
 * others run in the order given, as many at once as the ledger and
 * `o->width` admit. A suite that `stops` and did not pass cancels what
 * runs and what has not started, once every member of the suite has ended;
 * with `o->stop_on_fail`, the first member that does not pass does so at
 * once.
 *
 * A member with `cases` is run by the host and guest cases of its manifest
 * that run on `o->arch` or on none, each repetition of each a member of its
 * own that keeps the suite's `serial`. Their artifacts are goals of the
 * preparation; with `o->kernel`, the kernel that a guest case boots from the
 * node `rach2/kernel` is `rach2-KERNEL/kernel` in its place. Each runs in a
 * workspace of its own under `o->evidence`, under the case's initial
 * bounds, with a claim of the case's resources: a
 * host case's program with ETUDE_TREE naming the tree, and a guest case's
 * program on a machine of its QEMU profile, through etude-guest, with the
 * memory the launcher reserves for the machine as its claim. `o->casecheck`
 * judges each attempt, a guest's placed `qemu` with the machine's console. A
 * manifest that cannot be read, of another suite's family, with no case on
 * the architecture, or with a case that the set driver cannot run
 * (legacy_manifest_read) is refused.
 *
 * The history is one stream under `o->results`, with the plan of one item
 * for each suite of the registry, the case `legacy.SUITE` of the family
 * `legacy.SUITE`, and one for each repetition of each case, a step for the
 * preparation, an attempt record for each member started, and the run
 * record; its blobs go to the store. A suite's output is printed
 * whole when it ends, or as it comes when the set has one member, and the
 * output of the suites a stop interrupted is printed with what they wrote.
 * The status record, when asked for, gives the claims that wait and the
 * attempts that run.
 *
 * @return 0 when every suite passed; 1 when one did not, or when the run
 *         could not go on after its plan was written; 2 for a request
 *         refused before anything ran; 3 when the preparation failed; 128
 *         plus the signal's number after a stop.
 */
int etude_legacy_set(const struct etude_legacy_set_opts *o);

#endif
