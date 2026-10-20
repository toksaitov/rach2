/*
 * legacy.h: the preparation adapter, which builds what the suites of
 * tests/registry read before a suite starts, as tests/run built it, and
 * records those artifacts. tools/etude/Readme.md describes it.
 */
#ifndef ETUDE_LEGACY_H
#define ETUDE_LEGACY_H

#include <signal.h>
#include <stddef.h>
#include <stdio.h>

/* The bound of one step of a preparation when the caller gives none, in
 * seconds: the GNU system's root takes over an hour to build from nothing. */
#define ETUDE_LEGACY_SECONDS 14400u

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

#endif
