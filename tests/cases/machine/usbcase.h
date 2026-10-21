/*
 * usbcase.h: the operations the USB scenarios of tests/cases/machine share,
 * over libetude-case: the debug shell, the console's text, the keyboard and
 * mouse of QEMU's monitor, devices and media plugged and pulled, and the
 * assertions that follow from them.
 */
#ifndef USBCASE_H
#define USBCASE_H

#include <stddef.h>
#include <stdint.h>

#include "etude/scenario.h"

/* The debug shell's greeting, at the boot and after each program. */
#define USB_SHELL "Type 'help' for available commands."

/*
 * One attempt of a case, and the first operation of it that failed. Once
 * one has failed, every later operation returns its status at once without
 * acting, the next assertion fails with the reason, and those after it are
 * reported not run.
 */
struct usb_case {
    struct etude_case *c;
    enum etude_status st; /* ETUDE_OK until an operation fails */
    char why[200];        /* what failed */
    int told;             /* an assertion has failed with `why` */
};

/* One boot of a machine, and how far the scenario has read its console. */
struct usb_boot {
    struct usb_case *u;
    struct etude_case_machine *m;
    char console[4096]; /* the boot's console evidence, which the library
                           writes as QEMU writes the console */
    uint64_t at;        /* the offset just past the last text waited for */
};

/* A program run at the kernel's debug shell, as `exec` runs it. */
struct usb_run {
    char *text; /* the console from the command to the shell's return, from
                   malloc, or NULL when it did not return */
    int status; /* its exit status, or -1 */
    int signal; /* the signal that killed it, or 0 */
};

/** Begin the attempt `u` of case `c`. */
void usb_case(struct usb_case *u, struct etude_case *c);

/** Say whether every operation of `u` so far succeeded. */
int usb_ok(const struct usb_case *u);

/** Fail the attempt's work with the reason `format` makes, as a failed
 * operation does. */
void usb_fail(struct usb_case *u, const char *format, ...);

/** Begin step `step` of the manifest. */
enum etude_status usb_step(struct usb_case *u, const char *step);

/**
 * Report assertion `id`: held when `held` is nonzero, else failed with the
 * detail `format` makes. After a failed operation it fails, or is not run,
 * as struct usb_case says, whatever `held` is.
 */
void usb_assert(struct usb_case *u, const char *id, int held, const char *format, ...);

/**
 * Report assertion `id` of a probe's run: held when it exited 0, printed
 * `TOKEN OK` and never `TOKEN FAIL`; otherwise failed with the probe's
 * failure line or how the run ended.
 */
void usb_probe(struct usb_case *u, const char *id, const struct usb_run *r, const char *token);

/** Give the value scenario returns: 0 when every operation succeeded. */
int usb_end(const struct usb_case *u);

/**
 * Boot `machine` of the case with `disks`, as etude_boot takes them, as
 * the boot `n` of that machine in the attempt, counted from 1. The console
 * is read from its start.
 */
enum etude_status usb_boot(struct usb_boot *b, struct usb_case *u, const char *machine, unsigned n,
                           const char *const disks[]);

/** Wait for `text` after the offset read so far, and move past it. */
enum etude_status usb_wait(struct usb_boot *b, const char *text);

/** Wait for `text` anywhere on the console since the boot began, and leave
 * the offset read as it was. */
enum etude_status usb_seen(struct usb_boot *b, const char *text);

/** Wait for `text` after byte `from` of the console, give the offset just
 * past it in `at`, and leave the offset read as it was. */
enum etude_status usb_after(struct usb_boot *b, const char *text, uint64_t from, uint64_t *at);

/** Wait until `text` has appeared `n` times on the console since the boot
 * began, give the offset just past the last in `at`, and leave the offset
 * read as it was. */
enum etude_status usb_times(struct usb_boot *b, const char *text, unsigned n, uint64_t *at);

/** Wait for the machine to end, and give how in `end`. */
enum etude_status usb_end_of(struct usb_boot *b, struct etude_termination *end);

/** Type `line` and a carriage return at the serial console. */
enum etude_status usb_line(struct usb_boot *b, const char *line);

/**
 * Press each key of `keys`, names separated by spaces as the monitor's
 * `sendkey` takes them, on the machine's keyboard, one after the other.
 */
enum etude_status usb_keys(struct usb_boot *b, const char *keys);

/** Move the mouse by `dx` and `dy` and, when `buttons` is not 0, set its
 * buttons to that mask: 1 the left, 2 the middle and 4 the right. */
enum etude_status usb_mouse(struct usb_boot *b, int dx, int dy, int buttons);

/**
 * Send QMP command `execute` with the arguments that `format` and the
 * values after it make, a JSON object, and require a reply of `return`.
 */
enum etude_status usb_qmp(struct usb_boot *b, const char *execute, const char *format, ...);

/** Plug in device `id` of `model` on `bus`, with `more`, further members of
 * the JSON object or NULL, as QMP's `device_add`. */
enum etude_status usb_plug(struct usb_boot *b, const char *model, const char *bus, const char *id,
                           const char *more);

/** Pull device `id` out, as QMP's `device_del`. */
enum etude_status usb_pull(struct usb_boot *b, const char *id);

/**
 * Make the medium `node` of the raw image at `path`, read-only when
 * `readonly` is nonzero, as QMP's `blockdev-add`, for a stick plugged in
 * later to name as its drive.
 *
 * The file is opened without QEMU's image locks. The drive of a stick
 * pulled from the same file can still hold its write lock when the next
 * stick is plugged in, and `device_add` then fails with `Failed to get
 * "write" lock`; the pulled device does no more I/O.
 */
enum etude_status usb_medium(struct usb_boot *b, const char *node, const char *path, int readonly);

/** Type `exec COMMAND` at the debug shell, and give the offset the
 * program's run begins at, for usb_finish. */
uint64_t usb_start(struct usb_boot *b, const char *command);

/**
 * Wait for the shell's return after a program started at `from`, and then
 * for its greeting, and give the run in `r`: its text, and its exit status
 * or signal. Free `r->text` with free.
 */
enum etude_status usb_finish(struct usb_boot *b, uint64_t from, struct usb_run *r);

/** usb_start and usb_finish together, for a program that needs nothing
 * from the host while it runs. */
enum etude_status usb_exec(struct usb_boot *b, const char *command, struct usb_run *r);

/**
 * Run host tool or artifact `argv[0]` with its arguments, keeping its
 * standard output as evidence `output`, and require it to exit 0.
 */
enum etude_status usb_host(struct usb_case *u, char *const argv[], const char *output);

/** Give the console's bytes from `from` to `to`, from malloc, with a
 * terminating NUL; an empty text when they cannot be read. */
char *usb_text(const struct usb_boot *b, uint64_t from, uint64_t to);

/** Give the bytes of the file at `path`, from malloc, with a terminating
 * NUL; an empty text when it cannot be read. */
char *usb_file(const char *path);

/**
 * Say whether line `line` of `len` bytes has `pattern` in it: each `*` of
 * the pattern matches any run of bytes, each `#` a run of one or more
 * digits, and every other byte itself.
 */
int usb_match(const char *line, size_t len, const char *pattern);

/** Count the lines of `text` that have `pattern` in them. */
unsigned usb_count(const char *text, const char *pattern);

/** Give the first line of `text` that has `pattern` in it, or NULL; a line
 * ends at a carriage return or a newline. */
const char *usb_find(const char *text, const char *pattern);

/** Give the last line of `text` that has `pattern` in it, or NULL. */
const char *usb_last(const char *text, const char *pattern);

/** Copy the line that begins at `line` into `buf` of `size` bytes, without
 * its end, and give `buf`; an empty text for NULL. */
const char *usb_copy_line(const char *line, char *buf, size_t size);

#endif
