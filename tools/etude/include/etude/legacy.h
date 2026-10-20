/*
 * legacy.h: the preparation adapter, which builds what the suites of
 * tests/registry read before a suite starts, as tests/run built it, and
 * records those artifacts, and the suite adapter, which runs one prepared
 * suite under the old runner's supervision. tools/etude/Readme.md describes
 * them.
 */
#ifndef ETUDE_LEGACY_H
#define ETUDE_LEGACY_H

#include <signal.h>
#include <stddef.h>
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
    FILE *out;                   /* the progress and the result */
    volatile sig_atomic_t *stop; /* the number of a stop signal once one has
                                    come, or NULL */
};

/**
 * Prepare the suites `o` names in the tree `o->tree`, which the caller holds.
 *
 * The suites' names, architectures and tools are checked before anything is
 * built. The steps then run one at a time through the executor, each with
 * its output in a log in the work directory and cancelled at its bound: the
 * recovery key when the tree has a recovery system and no key, the
 * components, the kernel `o->kernel` names, `prepare-all` for more than one
 * suite, and last the artifacts that each suite's target reaches in make's
 * database and that the steps before did not build. Every artifact must
 * then exist; each regular file is hashed into the record. The work
 * directory is removed after a success and after a stop, and kept after a
 * failure.
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

#endif
