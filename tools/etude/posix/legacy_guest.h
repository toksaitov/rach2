/*
 * legacy_guest.h: one attempt of a guest case as the set driver runs it: a
 * QEMU machine of the case's profile under the supervisor, booted from the
 * run's artifact store, with etude-guest started at its debug shell and the
 * case's invocation run over the serial console.
 */
#ifndef ETUDE_LEGACY_GUEST_H
#define ETUDE_LEGACY_GUEST_H

#include <stddef.h>
#include <stdint.h>

#include "etude/artifact.h"
#include "etude/exec.h"
#include "etude/watchdog.h"
#include "legacy_cases.h"

/* From a frame's sending to its sending again, in microseconds. */
#define LEGACY_GUEST_RESEND_US UINT64_C(1000000)

/* The bytes kept of each stream of the case's program, which etude-guest
 * sends, and of the machine's console. */
#define LEGACY_GUEST_OUTPUT_MAX  ((size_t)4 << 20)
#define LEGACY_GUEST_CONSOLE_MAX ((size_t)32 << 20)

/* One attempt to start. Its strings must outlive the attempt. */
struct legacy_guest_spec {
    const struct legacy_case *k;
    const char *run_id, *attempt_id;
    const char *arch;   /* the run's, the machine's architecture */
    const char *accel;  /* kvm, hvf or tcg */
    const char *tree;   /* the tree's absolute path */
    const char *record; /* the preparation's record, whose digests the
                           artifacts must still have */
    struct etude_store *store;
    const char *work;      /* the workspace, which the caller made: the
                              console's file and the disks' copies go in it */
    const char *out;       /* the file of the program's standard output */
    const char *err;       /* and of its standard error */
    char *const *envp;     /* QEMU's whole environment */
    uint64_t memory_bytes; /* the memory admitted for the machine */
    uint64_t grace_us;     /* from a request to stop to a kill */
    etude_register_fn reg; /* registers QEMU's processes with the claim */
    void *reg_ctx;
    const struct etude_watchdog_spec *wd; /* the attempt's bounds, which must
                                             outlive it */
};

/* A guest attempt that runs. */
struct legacy_guest;

/**
 * Start the attempt `spec` under its watchdog `spec->wd`: put
 * the case's artifacts in the store, each with the digest the record gives
 * it, make the machine's disks, and start the machine under the supervisor
 * `s` in its boot stage, with its console written to `work/console`.
 *
 * @return 0 with the attempt in `*g` and the supervisor's in `*a`, or -1
 *         with why in `why`.
 */
int legacy_guest_start(struct etude_supervisor *s, const struct legacy_guest_spec *spec,
                       struct legacy_guest **g, struct etude_supervised **a, char *why,
                       size_t size);

/**
 * Move the attempt on by what its console has added: start etude-guest
 * once the debug shell has greeted, open its session, store the program
 * and the invocation, and run it, moving the watchdog from the boot stage
 * to the transfer and then to execution. The program's output goes to its
 * files as it comes. Once etude-guest reports the attempt cleaned, or the
 * session fails, the machine is stopped, and the supervisor finishes the
 * attempt when QEMU has ended. The caller calls it whenever the supervisor
 * returns, and before it reads the output's file.
 */
void legacy_guest_serve(struct legacy_guest *g);

/**
 * Stop the attempt for a stop of the run: etude-guest is asked to cancel a
 * run that goes on, and the machine is stopped once the run is cleaned or
 * the grace has passed, or at once when no run goes on.
 */
void legacy_guest_cancel(struct legacy_guest *g);

/**
 * Give the end of the case as the judge takes it, from what the supervisor
 * gave in `r` and etude-guest reported: `stopped` after an expiry or a
 * cancellation, `exited` or `signaled` with its value in `*value`, or `lost`
 * when etude-guest did not report it.
 */
const char *legacy_guest_end(const struct legacy_guest *g, const struct etude_supervised_result *r,
                             int *value);

/** Give why the attempt did not run to etude-guest's `cleaned`, or "". */
const char *legacy_guest_why(const struct legacy_guest *g);

/** Give the path of the machine's console. */
const char *legacy_guest_console(const struct legacy_guest *g);

/** Give the error that ended the writes to the machine's console, or 0. */
int legacy_guest_console_error(const struct legacy_guest *g);

/** Free the attempt, whose supervised attempt the caller frees. */
void legacy_guest_free(struct legacy_guest *g);

#endif
