/*
 * scenario.h: the interface of libetude-case, the library a machine scenario
 * links with. A scenario is a host program that boots Rach2 in QEMU, drives
 * it and reports its assertions; the executor supervises it as one case.
 */
#ifndef ETUDE_SCENARIO_H
#define ETUDE_SCENARIO_H

#include <stddef.h>
#include <stdint.h>

#include "etude/contract.h"

/* One attempt of the case, as the executor started it. */
struct etude_case;

/* One QEMU machine that the case booted and owns. */
struct etude_machine;

/* What the library returns. A scenario that receives ETUDE_STOPPED returns
 * from etude_scenario at once; the executor ends what is left. */
enum etude_status {
    ETUDE_OK,      /* the operation completed as asked */
    ETUDE_FAILED,  /* it completed with another result, which is reported */
    ETUDE_LOST,    /* the machine or its transport was lost; nothing is known */
    ETUDE_BOUND,   /* the step's completion bound expired */
    ETUDE_STOPPED, /* the executor stopped the case */
    ETUDE_REFUSED  /* the manifest does not declare what was asked */
};

/* The end of a machine or a process, as it was observed. */
struct etude_termination {
    enum etude_end end;
    int value;               /* the exit status or the signal; -1 when the end has none */
    uint64_t console_offset; /* bytes of the console read when the end was seen */
};

/**
 * The scenario's own code, which each scenario defines and the library's
 * `main` calls once.
 *
 * It reports each assertion of its manifest exactly once, and returns 0
 * when it ran to its end, whatever the assertions said, and 1 when it could
 * not; an assertion it did not report is missing in the results.
 */
int etude_scenario(struct etude_case *c);

/** Report assertion `id`: held when `held` is nonzero, else failed with
 * `detail`. */
void etude_assert(struct etude_case *c, const char *id, int held, const char *detail);

/** Report assertion `id` as not evaluated, with the reason. */
void etude_notrun(struct etude_case *c, const char *id, const char *reason);

/** Report that `alternative` of branch group `group` ran. */
void etude_branch(struct etude_case *c, const char *group, const char *alternative);

/** Report that witness `witness` has completed `units` in all. */
void etude_progress(struct etude_case *c, const char *witness, uint64_t units);

/**
 * Begin step `step` of the manifest, ending the step before it.
 *
 * Each step has the completion bound the plan fixed for it, and every wait
 * below returns ETUDE_BOUND once it expires; a scenario gives no time limit
 * of its own.
 */
enum etude_status etude_step(struct etude_case *c, const char *step);

/**
 * Give the path of artifact `name`, read-only and checked by its digest.
 *
 * @return the path, owned by the library, or NULL when the manifest does not
 *         declare the artifact.
 */
const char *etude_artifact(struct etude_case *c, const char *name);

/**
 * Give the path of a private writable copy of artifact `name`, made for this
 * attempt and removed in its cleanup unless the attempt failed.
 *
 * @return the path, owned by the library, or NULL as for etude_artifact.
 */
const char *etude_private_copy(struct etude_case *c, const char *name);

/**
 * Boot machine `machine` of the manifest, with `disks`, a list of names of
 * private copies ended by NULL, and, when the machine runs etude-guest, wait
 * until it has answered hello.
 *
 * The accelerator is the plan's; a failure to start it returns ETUDE_FAILED
 * with the end ETUDE_END_ACCEL_FAILED in `end`, and is not retried with
 * another.
 */
enum etude_status etude_boot(struct etude_case *c, const char *machine, const char *const disks[],
                             struct etude_machine **m, struct etude_termination *end);

/**
 * Wait for `text` on the machine's console after byte `from`, and give in
 * `at` the offset just past it.
 */
enum etude_status etude_console_wait(struct etude_machine *m, const char *text, uint64_t from,
                                     uint64_t *at);

/** Write `text` to the machine's console input. */
enum etude_status etude_type(struct etude_machine *m, const char *text);

/**
 * Send QMP command `command`, a JSON object, and give its reply in `reply`,
 * which the caller frees.
 */
enum etude_status etude_qmp(struct etude_machine *m, const char *command, char **reply);

/**
 * Run invocation `name`, an artifact, under the machine's etude-guest, and
 * give its end. Its report records become the case's own.
 */
enum etude_status etude_guest(struct etude_machine *m, const char *name,
                              struct etude_termination *end);

/**
 * Wait for the machine to end and give how it ended: a shutdown or a reset
 * is reported only from QEMU's own event, and a closed transport without one
 * is ETUDE_END_LOST.
 */
enum etude_status etude_wait_end(struct etude_machine *m, struct etude_termination *end);

/**
 * Run a host tool or an artifact of the manifest, `argv[0]` its name, in the
 * foreground, keeping its output as evidence `output`, and give its end.
 */
enum etude_status etude_host(struct etude_case *c, char *const argv[], const char *output,
                             struct etude_termination *end);

#endif
