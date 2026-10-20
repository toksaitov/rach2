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
struct etude_case_machine;

/* One run of a service of the case's, a host program that runs beside the
 * machines across steps until the case stops it or it ends. */
struct etude_case_service;

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
    uint64_t console_offset; /* bytes of the console read when the end was seen;
                                of a service, of its standard output */
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
                             struct etude_case_machine **m, struct etude_termination *end);

/**
 * Wait for `text` on the machine's console after byte `from`, and give in
 * `at` the offset just past it.
 */
enum etude_status etude_console_wait(struct etude_case_machine *m, const char *text, uint64_t from,
                                     uint64_t *at);

/** Write `text` to the machine's console input. */
enum etude_status etude_type(struct etude_case_machine *m, const char *text);

/**
 * Send QMP command `execute` with `arguments`, the text of a JSON object on
 * one line or NULL, and give QMP's whole reply line in `reply`, which the
 * caller frees. A reply with `return` is ETUDE_OK and one with `error`
 * ETUDE_FAILED.
 */
enum etude_status etude_qmp(struct etude_case_machine *m, const char *execute,
                            const char *arguments, char **reply);

/**
 * Run invocation `name`, an artifact, under the machine's etude-guest, and
 * give its end. Its report records become the case's own. Its processes end
 * with the machine, whose cleanup confirms them gone.
 */
enum etude_status etude_guest(struct etude_case_machine *m, const char *name,
                              struct etude_termination *end);

/**
 * Wait for the machine to end and give how it ended. A shutdown, a reset and
 * a panic are reported only from QEMU's own event, and QEMU's exit without
 * one is ETUDE_END_QEMU_EXITED, with its exit status or signal; a machine
 * whose QEMU or QMP the launcher lost ends ETUDE_END_LOST, and the wait
 * returns ETUDE_LOST.
 */
enum etude_status etude_wait_end(struct etude_case_machine *m, struct etude_termination *end);

/**
 * Run a host tool or an artifact of the manifest, `argv[0]` its name, in the
 * foreground, keeping its standard output as evidence `output` and its
 * standard error as `output`.err, and give its end.
 */
enum etude_status etude_host(struct etude_case *c, char *const argv[], const char *output,
                             struct etude_termination *end);

/**
 * Give local port `i` of the case's, counted from 0, which the plan reserved
 * for this attempt alone, as the case's `resources.ports` declares.
 *
 * @return the port, or 0 when the case has no port `i`.
 */
unsigned etude_port(struct etude_case *c, unsigned i);

/**
 * Start service `service` of the manifest, with `args`, the arguments after
 * its program, ended by NULL, and wait until its standard output has the
 * manifest's `ready` text.
 *
 * The service runs beside the machines until etude_service_stop, its own
 * end, or the attempt's cleanup, which collects the machines' diagnostics
 * before it stops the services. Its standard output and error are kept as
 * evidence `SERVICE.N.out` and `SERVICE.N.err`, N counting its starts from
 * 1, once it has ended. A service that ends before the scenario has seen its
 * end, by etude_service_end, etude_service_stop or a wait that it made
 * ETUDE_LOST, is a fault of the attempt.
 *
 * @return ETUDE_OK with the service in `*s`; ETUDE_FAILED when it ended
 *         before it was ready, with its end in `end`, or could not start,
 *         with the end ETUDE_END_LOST; and
 *         ETUDE_BOUND or ETUDE_STOPPED, after which the library has stopped
 *         it. A service not of the manifest, one that runs already, and a
 *         start outside a step are ETUDE_REFUSED.
 */
enum etude_status etude_service(struct etude_case *c, const char *service, char *const args[],
                                struct etude_case_service **s, struct etude_termination *end);

/**
 * Wait for `text` on the service's standard output after byte `from`, and
 * give in `at` the offset just past it; ETUDE_LOST once the service has
 * ended without it.
 */
enum etude_status etude_service_wait(struct etude_case_service *s, const char *text, uint64_t from,
                                     uint64_t *at);

/** Wait for the service to end by itself, within the step's bound, and give
 * how it ended. */
enum etude_status etude_service_end(struct etude_case_service *s, struct etude_termination *end);

/**
 * Stop the service, its process group getting SIGTERM and SIGKILL after the
 * grace period, and give how it ended: ETUDE_END_STOPPED, or its own end
 * when it had ended before. It needs no step, and a stop signal does not
 * interrupt it.
 */
enum etude_status etude_service_stop(struct etude_case_service *s, struct etude_termination *end);

/**
 * Give the path of evidence `name` in the attempt's scratch directory, as
 * etude_host writes it, whether or not it exists yet.
 *
 * @return the path, owned by the library, or NULL for a name that is not a
 *         file name.
 */
const char *etude_evidence(struct etude_case *c, const char *name);

/**
 * Run the attempt that the scenario invocation `argv[1]` gives: the
 * library's `main`, which a scenario does not call. tools/etude/Readme.md
 * gives the invocation and the exit status.
 */
int etude_case_main(int argc, char **argv);

#endif
