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
#include "etude/exec.h"

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
    ETUDE_LOST,    /* machine, transport or owned cleanup could not be confirmed */
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

/**
 * Give the identity of the attempt's case, as the invocation names it, so
 * that one program may be the scenario of several cases of its family.
 *
 * @return the identity, owned by the library.
 */
const char *etude_case_id(const struct etude_case *c);
/** The selected target architecture, or "none" for a host-only case. */
const char *etude_case_architecture(const struct etude_case *c);
/** The name QEMU gives the site's accelerator, which the case's machines
 * run under: "kvm", "hvf", "whpx" or "tcg". */
const char *etude_case_accel(const struct etude_case *c);

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

/** Give a declared host tool or a checked executable artifact copy. The
 * caller may pass this path to a child program that executes a helper. */
const char *etude_program(struct etude_case *c, const char *name);

/**
 * Give the path of a private writable copy of artifact `name`, made for this
 * attempt and removed in its cleanup unless the attempt failed. For the name
 * of an empty disk that a machine's profile declares with `size_bytes`, give
 * instead a sparse file of that size; and for the name of an overlay that it
 * declares with `base`, an empty qcow2 file whose raw backing file is the
 * base's object, so that a host tool such as `qemu-img` can read its map or
 * convert it between boots. Either is made at the first call, given again at
 * each later one, and removed as a copy is.
 *
 * @return the path, owned by the library, or NULL as for etude_artifact, or
 *         when the overlay cannot be made.
 */
const char *etude_private_copy(struct etude_case *c, const char *name);

/** Say whether the manifest declares machine NAME. This query neither
 * starts it nor changes the attempt's result. */
int etude_has_machine(const struct etude_case *c, const char *name);

/** Bind a private regular file to a declared empty disk SLOT of MACHINE.
 * PATH must be inside this attempt's scratch, with no links to other files.
 * The machine and any other machine using the file must have ended with
 * confirmed process cleanup. The binding lasts until replaced or until the
 * attempt ends; it does not copy the file. Immutable artifacts and host
 * disks cannot be bound. */
enum etude_status etude_bind_disk(struct etude_case *c, const char *machine, const char *slot,
                                  const char *path);

/**
 * Boot machine `machine` of the manifest, with `disks`, a list of names of
 * disks of its profile ended by NULL, or all of them for NULL, and, when the
 * machine runs etude-guest, wait until it has answered hello. A CD's medium
 * is its artifact, read-only; any other disk is the file etude_private_copy
 * gives. The disks of each bus take its places in the order of the list.
 *
 * The accelerator is the plan's; a failure to start it returns ETUDE_FAILED
 * with the end ETUDE_END_ACCEL_FAILED in `end`, and is not retried with
 * another. A boot of a case whose `resources.memory_bytes` does not cover
 * every machine the manifest declares returns ETUDE_REFUSED before QEMU
 * starts, as does a disk or file its profile names that the manifest does
 * not declare. A disk or artifact that is the case's but cannot be checked or
 * made, as when the host's disk is full, returns ETUDE_FAILED, and the case's
 * stderr has the system's error.
 */
enum etude_status etude_boot(struct etude_case *c, const char *machine, const char *const disks[],
                             struct etude_case_machine **m, struct etude_termination *end);

/**
 * Wait for `text` on the machine's console after byte `from`, and give in
 * `at` the offset just past it.
 */
enum etude_status etude_console_wait(struct etude_case_machine *m, const char *text, uint64_t from,
                                     uint64_t *at);

/**
 * Search the console bytes the library has read from the machine, after
 * byte `from`, for `text`, and give in `at` the offset just past its first
 * occurrence. It does not wait and needs no step.
 *
 * The library reads a running machine's console only while it waits, so a
 * search shows that text is absent only on a machine that has ended, whose
 * console it has read to the end.
 *
 * @return ETUDE_OK when the text is there; ETUDE_FAILED when it is not;
 *         ETUDE_LOST when it is not in the bytes kept and the console wrote
 *         more than the case keeps, so that its absence cannot be shown; and
 *         ETUDE_REFUSED for a machine without a console or an empty text.
 */
enum etude_status etude_console_find(struct etude_case_machine *m, const char *text, uint64_t from,
                                     uint64_t *at);

/**
 * Write `text` to the machine's console input. A boundary machine's console
 * takes `exec /memprobe total\r` once and `poweroff\r`, and other text
 * returns ETUDE_REFUSED.
 */
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

/* An installed command's observed end and independent cleanup result. */
struct etude_guest_result {
    struct etude_termination termination;
    struct etude_pid process;
    enum etude_cleanup cleanup;
    int complete; /* all result records and verified output arrived */
};

/** Start /etude-guest at the current guest shell prompt and open its framed
 * session. The caller first logs in and waits for that prompt. A helper
 * machine's etude_boot already opens its session. The console then belongs
 * to the channel until the machine ends. */
enum etude_status etude_guest_open(struct etude_case_machine *m);

/** Run an installed absolute guest program, argv[0], with argv[1...] as
 * its arguments. ENV is a NULL-terminated list of NAME=VALUE strings, or
 * NULL for PATH=/bin:/sbin:/usr/bin:/usr/sbin:/ and HOME=/root. INPUT is a
 * host file used as standard input, or NULL for no input. Standard output
 * is evidence OUTPUT, and standard error OUTPUT.err. A nonzero guest exit
 * is still ETUDE_OK; RESULT distinguishes exit, signal, lost result and
 * unconfirmed cleanup. Each command has a fresh attempt identity and ends
 * its process group before returning. No guest report is interpreted. */
enum etude_status etude_guest_command(struct etude_case_machine *m, const char *const argv[],
                                      const char *const env[], const char *input,
                                      const char *output, struct etude_guest_result *result);

/** As etude_guest_command, taking standard input from the absolute guest
 * path INPUT, which may be a declared attached raw disk. No input bytes
 * cross the console channel. The machine and its disk remain owned by the
 * scenario's declared resources and cleanup. */
enum etude_status etude_guest_command_file(struct etude_case_machine *m, const char *const argv[],
                                           const char *const env[], const char *input,
                                           const char *output, struct etude_guest_result *result);

/** Start the same command without waiting, for a workload that must stay
 * running while other machines or host tools are observed. One command may
 * run in the foreground per channel; an explicitly started guest service
 * may remain beside it. The machine owns both until finish or cleanup. */
enum etude_status etude_guest_start(struct etude_case_machine *m, const char *const argv[],
                                    const char *const env[], const char *input);

/** Wait for TEXT on the command's standard output, within the step bound. */
enum etude_status etude_guest_output_wait(struct etude_case_machine *m, const char *text);

/** Read currently available channel events and say whether the installed
 * command has started without an observed exit. This does not wait. */
int etude_guest_running(struct etude_case_machine *m);

/** Collect the started command, cancelling it first when CANCEL is nonzero.
 * A cancellation uses the existing cancel and cleaned records. */
enum etude_status etude_guest_finish(struct etude_case_machine *m, int cancel, const char *output,
                                     struct etude_guest_result *result);

/** Start one foreground guest service whose owned command remains alive
 * beside later guest commands and transfers. OUTPUT names its retained
 * streams. The machine cancels it on every case cleanup path. */
enum etude_status etude_guest_service_start(struct etude_case_machine *m, const char *const argv[],
                                            const char *const env[], const char *input,
                                            const char *output);
int etude_guest_service_running(struct etude_case_machine *m);
enum etude_status etude_guest_service_finish(struct etude_case_machine *m, int cancel,
                                             struct etude_guest_result *result);

/** Copy a host file to an absolute guest path with octal MODE (such as
 * "0755"). The guest replaces it only after its size and digest match.
 * The destination's parent directory must exist. */
enum etude_status etude_guest_put(struct etude_case_machine *m, const char *host, const char *guest,
                                  const char *mode);

/** Fetch an absolute guest file as evidence NAME. The evidence is published
 * only after the channel verifies its complete size and SHA-256. */
enum etude_status etude_guest_get(struct etude_case_machine *m, const char *guest,
                                  const char *name);

/**
 * Wait for the machine to end and give how it ended. A shutdown, a reset and
 * a panic are reported only from QEMU's own event, and QEMU's exit without
 * one is ETUDE_END_QEMU_EXITED, with its exit status or signal; a machine
 * whose QEMU or QMP the launcher lost ends ETUDE_END_LOST, and the wait
 * returns ETUDE_LOST. Unconfirmed process cleanup also returns ETUDE_LOST,
 * preserving the observed end in `end`; it cannot authorize disk reuse.
 *
 * The end it observes, as the one etude_boot observes of a machine that ends
 * while it starts, is written in the report as the record
 * `machine MACHINE.N END`, unless etude_fault wrote that boot's end before.
 */
enum etude_status etude_wait_end(struct etude_case_machine *m, struct etude_termination *end);

/* One stop of a suspension: two times of the host's CLOCK_MONOTONIC, in
 * microseconds, between which the machine's QEMU process was stopped. */
struct etude_stop {
    uint64_t stopped_us; /* the executor saw every thread of QEMU stopped */
    uint64_t resumed_us; /* just before it sent SIGCONT */
};

/**
 * Run suspension `suspension` of the manifest on its machine `m`, as the
 * host descheduling a machine under load: `cycles` times, let the machine
 * run for `run_us`, stop its QEMU process with SIGSTOP, hold it stopped for
 * `stop_us` from when the executor saw it stop, and resume it with SIGCONT,
 * going on once the executor has seen it continue. The workload declares the
 * suspension, in the case's `workload.suspensions`, so its intervals are the
 * stimulus of the case and not a wait for an event; each stop seen advances
 * the suspension's counter witness. While QEMU is stopped no processor of
 * the guest runs and no timer of QEMU's fires, and neither the console nor
 * QMP answers; a QMP `stop` differs, since QEMU's timers go on under it.
 *
 * The cycles run within the step's bound. A machine that ends, a bound that
 * expires and a stop of the case end them, and the library resumes QEMU
 * before it returns, whatever the status. When the scenario's own process
 * ends instead, its keeper of QEMU sends SIGCONT after SIGTERM, so that no
 * machine is left stopped and none is waited for while it is.
 *
 * `*done` counts the cycles that ran whole, on any return, and `stops`, which
 * has room for `max`, gets the first `max` of their stops.
 *
 * @return ETUDE_OK when every cycle ran; ETUDE_LOST when the machine
 *         ended; ETUDE_BOUND or ETUDE_STOPPED; ETUDE_FAILED when the
 *         executor could not signal QEMU; and ETUDE_REFUSED outside a step,
 *         for a suspension the manifest does not declare, or one declared
 *         for another machine.
 */
enum etude_status etude_suspend(struct etude_case_machine *m, const char *suspension,
                                struct etude_stop *stops, size_t max, unsigned *done);

/**
 * Record that boot `m` ended in the fault `end` while its QEMU may still run,
 * as the kernel's panic stops every processor without an event of QEMU's.
 * ETUDE_END_PANIC needs a record of the kernel's panic, a line that begins
 * `<2>[` and whose subsystem is `panic`, in the console bytes the library
 * has read, so a scenario waits for the panic's text first.
 * ETUDE_END_HALTED needs QMP's `info registers -a` to show each of the
 * profile's processors halted with interrupts off. A fault shown is written
 * in the report as `machine MACHINE.N END`, which `etude.fault` judges
 * against the console; the boot is still ended by the step.
 *
 * @return ETUDE_OK when the fault is shown and recorded; ETUDE_FAILED, with
 *         nothing recorded, when it is not; ETUDE_REFUSED for another end, a
 *         boot whose end is recorded already, a panic of a machine without a
 *         console, or a halt of one without QMP; and as etude_qmp otherwise.
 */
enum etude_status etude_fault(struct etude_case_machine *m, enum etude_end end);

/**
 * Give the WAV path of this boot, or NULL when its profile selects no audio
 * card on the `wav` backend. The library owns the path and keeps the file as
 * attempt evidence. Read it after etude_wait_end, when QEMU has finished its
 * WAV header and samples.
 */
const char *etude_audio(const struct etude_case_machine *m);

/**
 * Give the file in which boot `m` records card network `card`, such as
 * `net0`, as its profile's `captures` asks: the evidence
 * `MACHINE.N.netK.pcap`, in pcap format, to which QEMU writes each frame the
 * card sends or receives as it passes, and which is whole once the machine
 * has ended. A scenario gives the path to a host tool or artifact to read.
 *
 * @return the path, owned by the library, or NULL, a fault of the attempt,
 *         for a card the profile does not record.
 */
const char *etude_capture(struct etude_case_machine *m, const char *card);

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

/** As etude_service_wait, but return ETUDE_LOST if the same case's guest
 * command ends before the text arrives. Already received text wins. The
 * caller must still collect the guest command's result and cleanup. */
enum etude_status etude_service_wait_guest(struct etude_case_service *s,
                                           struct etude_case_machine *guest, const char *text,
                                           uint64_t from, uint64_t *at);

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
 * @return the path, in a buffer of the library's that its next call may
 *         overwrite, so a caller that keeps the path copies it; or NULL for
 *         a name that is not a file name.
 */
const char *etude_evidence(struct etude_case *c, const char *name);

/**
 * Run the attempt that the scenario invocation `argv[1]` gives: the
 * library's `main`, which a scenario does not call. tools/etude/Readme.md
 * gives the invocation and the exit status.
 */
int etude_case_main(int argc, char **argv);

#endif
