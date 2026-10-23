/*
 * drive.h: the console driver of the machine scenarios under tests/cases:
 * one machine's boot, the lines typed at its serial console and the text
 * waited for there, over libetude-case, and the reading of disks and files
 * the scenarios judge on the host.
 */
#ifndef DRIVE_H
#define DRIVE_H

#include <stddef.h>
#include <stdint.h>

#include "etude/scenario.h"

#ifdef _WIN32
#include "../../../tools/etude/win/files.h"
#endif

/*
 * The longest line drive_send types: what the guest terminal's canonical line
 * holds before its newline (N_TTY_LINE_MAX less one, rach2/sys/tty/n_tty.h).
 * A line typed while a command still runs reaches the shell through that
 * line, not its line editor, and loses whatever is past this length.
 */
#define DRIVE_LINE_MAX 255

/*
 * One boot of a machine, driven at its console as a person types at it.
 * Each call after the first one that did not complete does nothing and
 * fails, so that a scenario writes a boot's exchange as one sequence and
 * judges it once; `why` says which call failed and how.
 */
struct drive {
    struct etude_case *c;
    struct etude_case_machine *m;
    char machine[64];
    unsigned boot;                /* the machine's boots in this attempt, counted from 1 */
    uint64_t at;                  /* the console offset just past the last text waited for */
    int failed;                   /* a call did not complete */
    char why[200];                /* what the first call that did not complete was */
    struct etude_termination end; /* how the machine ended, once it has */
};

/**
 * Say whether the executor has stopped the case. A scenario returns 1 from
 * etude_scenario as soon as it sees this, as libetude-case requires.
 */
int drive_halted(void);

/**
 * Boot machine `machine` of the manifest with `disks`, as etude_boot does.
 *
 * @return 0, or -1 with `d->why` saying why.
 */
int drive_boot(struct drive *d, struct etude_case *c, const char *machine,
               const char *const disks[]);

/**
 * Wait for `text` on the console after the last text waited for, within the
 * step's bound.
 *
 * @return 0, or -1 with `d->why` saying why.
 */
int drive_expect(struct drive *d, const char *text);

/**
 * Type the line that `fmt` gives, and a newline, at the console.
 *
 * A line longer than DRIVE_LINE_MAX bytes is refused, typed or not, since
 * whether the shell reads it whole depends on whether its last command has
 * ended.
 *
 * @return 0, or -1 with `d->why` saying why.
 */
int drive_send(struct drive *d, const char *fmt, ...);

/**
 * End the machine through QMP's `quit` and wait for QEMU to end. A machine
 * that has already ended is only waited for.
 *
 * @return 0, or -1 when the end could not be seen.
 */
int drive_stop(struct drive *d);

/**
 * Wait for the machine to end by itself, and keep how it ended in `d->end`.
 *
 * @return 0, or -1 when the end could not be seen.
 */
int drive_wait_end(struct drive *d);

/**
 * Give the console of the boot so far, with every carriage return taken out
 * and a NUL after it.
 *
 * @return the text, which the caller frees, or NULL when the console file
 *         cannot be read.
 */
char *drive_console(const struct drive *d);

/**
 * Copy into `buf` the rest of line `nth`, counted from 1, of `text` that
 * begins with `prefix`.
 *
 * @return 0, or -1 when `text` has no such line.
 */
int text_field(const char *text, const char *prefix, int nth, char *buf, size_t size);

/** Count the lines of `text` that have `needle` in them. */
int text_lines(const char *text, const char *needle);

/**
 * Give in `hex` the SHA-256 of the `len` bytes of file `path` from byte
 * `off`, or of the rest of the file when `len` is UINT64_MAX.
 *
 * @return 0, or -1 when the file cannot be read or is shorter.
 */
int file_digest(const char *path, uint64_t off, uint64_t len, char hex[65]);

/**
 * Write `size` bytes of the pattern the scenarios fill a disk with into file
 * `path`: the numbers 0 to 65535 as sixteen decimal digits each, one mebibyte
 * over and over, so that a stray write of zeros leaves a trace.
 *
 * @return 0, or -1 when the file cannot be written.
 */
int fill_pattern(const char *path, uint64_t size);

/**
 * Run host tool or artifact `argv[0]` of the manifest, as etude_host does,
 * keeping its standard output as evidence `output`.
 *
 * @return its exit status, or -1 when it did not exit or could not run.
 */
int drive_host(struct etude_case *c, char *const argv[], const char *output);

/**
 * Read the evidence `name` whole, with a NUL after it.
 *
 * @return the text, which the caller frees, or NULL.
 */
char *evidence_text(struct etude_case *c, const char *name);

#endif
