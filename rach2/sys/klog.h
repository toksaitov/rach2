#ifndef _KLOG_H_
#define _KLOG_H_

/**
 * The kernel log: one call for a diagnostic, one call for the console.
 *
 * A diagnostic is a record. `klog` formats one whole record, appends it to the
 * ring, and then offers it to the screen, the serial port and netconsole in
 * that order, each of which may decline it by level or be absent. A record
 * looks like this on the serial port:
 *
 *     <3>[12.34] ahci: command: timed out on port 0
 *
 * The angle brackets hold the level with the kernel facility implied, which is
 * the form BusyBox's `klogd` parses, so a userland log reader can be added
 * later without changing the kernel. The brackets hold uptime in seconds and
 * hundredths. The subsystem and the `event: detail` text after it are the shape
 * `CLAUDE.md` requires of every message; here the call enforces it.
 *
 * The debug shell's menus, prompts and command output are not diagnostics.
 * They go through `console_putc`, `console_puts` and `console_printf`, which
 * reach the screen and the serial port and nothing else: they never enter the
 * ring, and they never leave the machine over the network.
 */

#include "lib/kint.h"

/** Syslog's numbers, so a record needs no translation later. */
#define KLOG_CRIT  2
#define KLOG_ERR   3
#define KLOG_WARN  4
#define KLOG_INFO  6
#define KLOG_DEBUG 7

/** The longest record the logger will build, including its newline. */
#define KLOG_RECORD_MAX 256

/** The machine's own copy of its records. */
#define KLOG_RING_SIZE 16384

/**
 * Format and deliver one record.
 *
 * `level` is one of the `KLOG_` numbers, `subsystem` a lowercase literal such
 * as `"ahci"`, and `format` the rest of the line, which by convention begins
 * with `event: ` and carries the detail after it. The conversions are the
 * kernel's own: `%d`, `%b`, `%o`, `%x`, `%X`, `%s`, `%c` and `%%`.
 *
 * A record longer than `KLOG_RECORD_MAX` is cut and marked with `...`. Exactly
 * one newline ends every record, however the format was written.
 */
void klog(int level, const char *subsystem, const char *format, ...);

/**
 * The same, for a caller that already holds its arguments.
 *
 * `args` points at the first argument and is consumed left to right, one
 * stack word each, which is what `k_vprintf` expects. `klog` is this with the
 * words taken off its own stack. It is separate because reaching a variadic
 * function's arguments that way is an i386 fact, and a test that drives the
 * record builder should not have to be running on i386 to do it.
 */
void klog_v(int level, const char *subsystem, const char *format, char **args);

/**
 * Read out of the ring, oldest byte first.
 *
 * `offset` counts from the oldest byte still held, so a reader that walks
 * forward sees each record once. Returns the number of bytes copied, which is
 * zero at the end. The ring holds whole records only: a record that does not
 * fit evicts oldest records entire, so what comes back never begins mid-line.
 */
uint32_t klog_read(char *buf, uint32_t cap, uint32_t offset);

/** How many bytes the ring holds now. */
uint32_t klog_len(void);

/**
 * The debug shell's own output: the screen and the serial port, nothing else.
 *
 * A prompt is not a diagnostic. It does not belong in the machine's log, and
 * it must not be broadcast: netconsole carries the log to whoever is on the
 * segment, and a person typing at the console did not agree to that.
 */
void console_putc(char c);
void console_printf(const char *format, ...);

/** As `k_puts`: the string and then a newline. */
void console_puts(const char *str);

#endif /* _KLOG_H_ */
