/*
 * lifecycle.h: what the scenarios of tests/cases/machine/ share: each
 * declared assertion reported once, waits and searches of a console that
 * report what they found, a value read off a console, a program run from the
 * kernel's debug shell and judged by the record that hands the shell back,
 * the end of a machine by QMP's quit, a look at its processors, fsck.krffs
 * on a disk copy, the kernel's record of its clock's check, and a clock
 * reference on the host for clockmusl.
 */
#ifndef LIFECYCLE_H
#define LIFECYCLE_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "etude/scenario.h"

/* The most assertions a case of these families declares. */
#define LC_ASSERTS 48

/* The KRFFS partition of the disks mkdisk.sh makes with `64 48`. */
#define LC_PART_OFFSET "1048576"
#define LC_PART_SIZE   "50331648"

/* One attempt of a case, as the scenario runs it. */
struct lc {
    struct etude_case *c;
    const char *ids[LC_ASSERTS + 1]; /* the declared assertions, ended by NULL */
    int told[LC_ASSERTS];            /* ids[i] has been reported */
    int stopped;                     /* the executor stopped the case */
    char why[200];                   /* why the scenario cannot go on; empty
                                        while it can */
};

/**
 * Begin attempt `c`, whose case declares the assertions `ids`, ended by
 * NULL, at most LC_ASSERTS of them.
 */
void lc_begin(struct lc *l, struct etude_case *c, const char *const ids[]);

/**
 * Report assertion `id`: held when `held` is nonzero, else failed with
 * `detail`. A second report of an assertion, or one of an assertion not
 * declared, is a fault of the scenario, said on standard error and dropped.
 */
void lc_assert(struct lc *l, const char *id, int held, const char *detail);

/**
 * Report assertion `id` as not evaluated, with `reason`, under the same rule
 * of one report each as lc_assert.
 */
void lc_notrun(struct lc *l, const char *id, const char *reason);

/** Say whether the scenario can go on: nothing it needed failed. */
int lc_ok(const struct lc *l);

/**
 * Record that the scenario cannot go on, because `what` gave `st`, unless a
 * reason is recorded already. ETUDE_STOPPED also records the stop.
 */
void lc_halt(struct lc *l, enum etude_status st, const char *what);

/**
 * Report each declared assertion not yet reported as not run, with the
 * reason the scenario stopped, and give etude_scenario's status: 1 after a
 * stop, else 0.
 */
int lc_finish(struct lc *l);

/** Begin step `step`. @return 1 when it began. */
int lc_step(struct lc *l, const char *step);

/**
 * Boot machine `machine` of the step on its manifest's disks.
 *
 * @return the machine, or NULL when it did not boot.
 */
struct etude_case_machine *lc_boot(struct lc *l, const char *machine);

/**
 * Wait for `text` on the console after `*at`, and move `*at` past it.
 * Assertion `id`, when not NULL, holds when the text came and fails
 * otherwise; any outcome but the text also halts the scenario.
 *
 * @return 1 when the text came.
 */
int lc_wait(struct lc *l, struct etude_case_machine *m, const char *text, uint64_t *at,
            const char *id);

/** Type `text` at the console. @return 1 when it was written. */
int lc_type(struct lc *l, struct etude_case_machine *m, const char *text);

/**
 * Begin step `step`, boot machine `machine` and wait for the prompt of the
 * kernel's debug shell, putting in `*at` the offset past it.
 *
 * @return the machine, or NULL when the scenario cannot go on.
 */
struct etude_case_machine *lc_shell(struct lc *l, const char *step, const char *machine,
                                    uint64_t *at);

/**
 * Read bytes `from` to `to` of the console of the first boot of machine
 * `machine`, which the library writes to its evidence as it reads them.
 *
 * @return the bytes, NUL-ended, which the caller frees; or NULL, after
 *         which the scenario cannot go on.
 */
char *lc_console(struct lc *l, const char *machine, uint64_t from, uint64_t to);

/**
 * Give the offset past the kernel's record prefix, `<N>[SECONDS] `, at
 * offset `at` of the `len` bytes of `text`, or `at` when there is none.
 */
size_t lc_past_prefix(const char *text, size_t len, size_t at);

/**
 * Run `command`, typed at the debug shell of machine `machine`, `m`, whose
 * console has been read to `*at`, and wait for the kernel's record that
 * hands the shell back. Assertion `id` holds when no line of the run's
 * window, from the typing to that record, begins with the record of the
 * program's death by a signal, and the first that begins with the record of
 * its exit gives status 0; each record is matched whole after its prefix,
 * so that a program that prints a record's text is not credited with it.
 *
 * @return the window, NUL-ended, which the caller frees; or NULL when the
 *         scenario cannot go on, with `id` reported failed unless a stop
 *         came.
 */
char *lc_run(struct lc *l, struct etude_case_machine *m, const char *machine, const char *command,
             uint64_t *at, const char *id);

/**
 * Wait after `*at` for the kernel's record that hands the debug shell of
 * machine `machine`, `m`, back, and judge the window from `from` to it as
 * lc_run does, for a program that the scenario started at `from` and typed
 * at since.
 *
 * @return the window, NUL-ended, which the caller frees; or NULL as lc_run
 *         gives it.
 */
char *lc_handback(struct lc *l, struct etude_case_machine *m, const char *machine, uint64_t from,
                  uint64_t *at, const char *id);

/**
 * Judge the `len` bytes of `w`, the window of a program run at the debug
 * shell, as lc_run judges it, putting what decided it in `why`.
 *
 * @return 1 when the program exited with status 0.
 */
int lc_exited0(const char *w, size_t len, char *why, size_t size);

/**
 * Find the kernel's record `timing: tick N Hz, ...` at the start of a line of
 * the `len` bytes of `w`, and give its level: 6 when its clock's period
 * accounting passed its check on fixed readings, 3 when it did not; another
 * level's record does not count.
 *
 * @return the level of the first record of level 6, else of level 3, or 0
 *         when there is none.
 */
int lc_timing_level(const char *w, size_t len);

/**
 * Start the clock reference for clockmusl: a child that answers on a UDP
 * port of 127.0.0.1, which the guest reaches at QEMU's host, 10.0.2.2. Each
 * request `CLOCKREF N` gets back `CLOCKREF N MONO REAL`, the host's monotonic
 * and calendar clocks in nanoseconds, read as the request is answered.
 *
 * @return the child's pid, with the port in `*port`, or -1.
 */
pid_t lc_clockref_start(unsigned *port);

/** End the clock reference `pid`, when it is not -1, and reap it. */
void lc_clockref_stop(pid_t pid);

/**
 * Say whether a line of the `len` bytes of `w` is clockmusl's result of its
 * comparison with the reference, `CLOCKMUSL REFERENCE OK`, whole.
 */
int lc_clockref_said(const char *w, size_t len);

/**
 * Report assertion `id` from a search of the console after `from`: it holds
 * when `text` is there and `present` is set, or is not there and `present`
 * is clear. An absence is shown only on a machine that has ended.
 */
void lc_find(struct lc *l, struct etude_case_machine *m, const char *id, const char *text,
             uint64_t from, int present);

/**
 * Copy into `buf` the rest of the console's line after the first `key`
 * after byte `from`, without its line ending; `console` is the evidence that
 * keeps the machine's console, `MACHINE.N.console` for its Nth boot. Only
 * the bytes a wait or the machine's end has read are there.
 *
 * @return 1 when the key was found, 0 when it was not, with `buf` empty.
 */
int lc_text(struct lc *l, const char *console, const char *key, uint64_t from, char *buf,
            size_t size);

/**
 * Wait for the machine to end by itself, and give its end in `end`.
 *
 * @return 1 when its end was seen.
 */
int lc_end(struct lc *l, struct etude_case_machine *m, struct etude_termination *end);

/**
 * End the machine with QMP's `quit` and wait for its end. Assertion `id`,
 * when not NULL, holds when the machine was still running and the quit
 * ended it, as `qemu_exited`; a machine that had ended by itself fails it.
 *
 * @return 1 when the machine has ended, by the quit or before it.
 */
int lc_quit(struct lc *l, struct etude_case_machine *m, const char *id);

/**
 * Report assertion `id` from a look at the machine through QMP: QEMU says
 * that it runs, and every processor has halted with its interrupts off;
 * `cpus`, when not 0, is how many processors there must be.
 */
void lc_halted(struct lc *l, struct etude_case_machine *m, int cpus, const char *id);

/**
 * Report assertion `id` from the serial port's line status register, read
 * through the human monitor: it holds when a byte typed at the console is
 * waiting in the port's receiver, where a machine that ran would read it.
 */
void lc_received(struct lc *l, struct etude_case_machine *m, const char *id);

/**
 * Run fsck.krffs on the private copy of disk artifact `disk`, between the
 * announcement and the end of the quiet witness `fsck`. Assertion `id` holds
 * when fsck.krffs exits 0 and says that the file system was not cleanly
 * unmounted exactly when `dirty` is set.
 */
void lc_fsck(struct lc *l, const char *disk, int dirty, const char *id);

#endif
