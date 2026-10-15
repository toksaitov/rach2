/*
 * klog.h: the kernel log, one call for a diagnostic, and the debug shell's
 * console calls beside it.
 *
 * A diagnostic is a record. `klog` formats one whole record, appends it to the
 * ring, and then offers it to the screen, the serial port and netconsole in
 * that order, each of which may decline it by level or be absent. A record
 * looks like this on the serial port:
 *
 *     <3>[12.34] ahci: command: timed out on port 0
 *
 * The angle brackets hold the level with the kernel facility implied, which is
 * the form BusyBox's `klogd` parses. The brackets hold uptime in seconds and
 * hundredths. The subsystem and the `event: detail` text after it are the
 * shape `AGENTS.md` requires of every message; the call writes the subsystem
 * and its colon itself.
 *
 * The debug shell's menus, prompts and command output are not diagnostics.
 * They go through `console_putc`, `console_puts` and `console_printf`, which
 * reach the screen and the serial port and nothing else: they never enter the
 * ring, and they never leave the machine over the network.
 */
#ifndef _KLOG_H_
#define _KLOG_H_

#include "lib/kint.h"
#include <stdarg.h>

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
 * with `event: ` and gives the detail after it. The conversions are the
 * kernel's own: `%d`, `%b`, `%o`, `%x`, `%X`, `%s`, `%c` and `%%`. A level
 * outside `KLOG_CRIT` to `KLOG_DEBUG` is taken as the nearer of the two.
 *
 * A record longer than `KLOG_RECORD_MAX` is cut and marked with `...`. Exactly
 * one newline ends every record, however the format was written.
 *
 * Any context may call it, an interrupt handler included: the record is built
 * and put in the ring with interrupts off, and neither step sleeps. A record
 * raised while another is being written out, or inside a console write
 * (`console_write_begin`), waits in a queue of eight for the rest of its
 * destinations and goes out when that write ends; one raised when the queue
 * is full is dropped, and a later record says how many were.
 */
void klog(int level, const char *subsystem, const char *format, ...);

/** As `klog`, with the arguments in `ap`, which the call consumes. */
void klog_v(int level, const char *subsystem, const char *format, va_list ap);

/**
 * Read out of the ring, oldest byte first.
 *
 * `offset` counts from the oldest byte still held. One call copies with the
 * ring held still, so what comes back is one moment of it. Between two calls
 * the ring can evict, and then the same offset reaches a later byte: a reader
 * that walks it in pieces can miss a record or see one twice while the
 * machine is busy printing. A reader that cannot accept that reads the whole
 * ring in one call, or keeps its place with `klog_read_from`.
 *
 * Returns the number of bytes copied, which is zero at the end. The ring
 * holds whole records only: a record that does not fit evicts oldest records
 * entire, so what comes back never begins mid-line.
 */
uint32_t klog_read(char *buf, uint32_t cap, uint32_t offset);

/** How many bytes the ring holds now. */
uint32_t klog_len(void);

/** How many bytes the ring has taken since boot, which a reader that keeps
 * its place counts from. */
uint32_t klog_written(void);

/** The byte `at`, counted as `klog_written` counts, or the oldest the ring
 * holds when it has dropped `at`, which begins a record. */
uint32_t klog_clamp(uint32_t at);

/**
 * Read out of the ring from the byte `*at`, counted as `klog_written`
 * counts, and move `*at` past what was read: at most `cap` bytes, with the
 * ring held still. A place the ring has dropped begins at the oldest byte
 * it holds, which begins a record. Returns the number of bytes copied, zero
 * when `*at` is the end.
 */
uint32_t klog_read_from(char *buf, uint32_t cap, uint32_t *at);

/**
 * Let the next record go to every destination at once, even inside another
 * record's delivery or a console write, where it would otherwise wait in the
 * queue: for a machine about to stop, which never comes back to write the
 * queue out. It clears what would have queued the record, which is safe only
 * because nothing after it is expected to run.
 */
void klog_deliver_directly(void);

/**
 * Set the lowest-priority record the screen shows.
 *
 * The ring, the serial port and netconsole are not affected: every record is
 * kept and sent whatever this is, so turning the screen down loses nothing
 * that a later reader of `/dev/bootlog` or a capture on the port would want.
 * What it decides is how much of the machine's own record a person standing
 * at it has to read. The default keeps the boot log, `KLOG_INFO`, and drops
 * `KLOG_DEBUG`; a program talking to a person turns it down for its own run
 * and puts it back. `KLOG_CRIT` is the floor, so a panic always shows. A
 * level past `KLOG_DEBUG` is taken as `KLOG_DEBUG`, which shows what
 * `KLOG_INFO` shows, since a `KLOG_DEBUG` record is never offered to the
 * screen.
 */
void klog_set_screen_level(int level);

/** The level the screen is showing down to. */
int klog_screen_level(void);

/** As `klog_set_screen_level`, and record that a boot line asked for it. */
void klog_set_screen_level_from_cmdline(int level);

/** Whether the boot line named the level. */
int klog_screen_level_was_asked_for(void);

/**
 * The debug shell's own output: the screen and the serial port, nothing else.
 * `console_putc` writes one byte, and a newline goes to the port as a
 * carriage return and a newline; `console_printf` formats as `k_printf` does,
 * as one console write.
 *
 * A prompt is not a diagnostic. It does not belong in the machine's log, and
 * it must not be broadcast: netconsole sends the log to whoever is on the
 * segment, and a person typing at the console did not agree to that.
 */
void console_putc(char c);
void console_printf(const char *format, ...);

/** As `console_putc`, for a writer that has expanded the newline itself. */
void console_putc_raw(char c);

/**
 * Bracket a console write, a program's chunk or a call of the debug shell's
 * `console_puts` or `console_printf`: a record raised inside it waits for its
 * end, and the screen's cursor is held and placed once at the end. Brackets
 * nest, and no writer sleeps inside one. `console_write_end` writes out the
 * records that waited, unless a record's delivery is in progress, which
 * writes them itself.
 */
void console_write_begin(void);
void console_write_end(void);

/** As `k_puts`: the string and then a newline, as one console write. A NULL
 * pointer is ignored. */
void console_puts(const char *str);

#endif /* _KLOG_H_ */
