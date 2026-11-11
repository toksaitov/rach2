/*
 * klog.h: the kernel log, one call for a diagnostic, and the debug shell's
 * console calls beside it.
 *
 * A diagnostic is a record. `klog` formats one whole record, appends it to the
 * ring, and then offers it to the screen, the serial port and netconsole in
 * that order, each of which may decline it by level or be absent. A record
 * looks like this on the serial port:
 *
 *     <3>[12.340000] ahci: command: timed out on port 0
 *
 * The angle brackets hold the level with the kernel facility implied, which is
 * the form BusyBox's `klogd` parses. The brackets hold uptime in seconds and
 * microseconds, as Linux's do, counted in whole ticks. The subsystem and the
 * `event: detail` text after it are the shape `AGENTS.md` requires of every
 * message; the call writes the subsystem and its colon itself.
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

/**
 * The machine's own copy of its records: 128 KiB, Linux's default, which is
 * `__LOG_BUF_LEN` (kernel/printk/printk.c:498), 1 << CONFIG_LOG_BUF_SHIFT,
 * whose default is 17 (init/Kconfig:768). The ring is in .bss.
 */
#define KLOG_RING_SIZE 131072

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
 * Any context may call it, an interrupt handler and an NMI included: the
 * record is built and put in the ring with interrupts off, and neither step
 * sleeps. A record raised while another is being written out, or inside a
 * console write (`console_write_begin`), waits in a queue of sixty-four for
 * the rest of its destinations and goes out when that write ends; one
 * raised when the queue is full is dropped, and a later record says how
 * many were. A record made in an NMI, or by a processor without the big
 * lock (`smp_serialized`), is kept whole, in a queue of 128 that any
 * processor may add to, until a processor that holds the big lock makes a
 * record, writes the console or takes the boot processor's tick, or the
 * panic writes its own (`klog_flush`); it then reaches the ring and the
 * destinations, in the order the records were made. A record made inside
 * a delivery or a console write while kept records wait for room in the
 * full queue is kept behind them, so the ring has it after them too.
 */
void klog(int level, const char *subsystem, const char *format, ...);

/** As `klog`, with the arguments in `ap`, which the call consumes. */
void klog_v(int level, const char *subsystem, const char *format, va_list ap);

/* Where `klog_report_begin` sends a report: the ring and the serial port,
 * under the report lock; the ring alone, when this processor holds that
 * lock already, as an interrupt of its holder does, or another held it
 * longer than KLOG_REPORT_WAIT_MS; or both without the lock, in a panic
 * that found it held by a processor it stopped. */
#define KLOG_REPORT_LOCKED 1
#define KLOG_REPORT_RING   0
#define KLOG_REPORT_PANIC  2

/* The longest a report waits for the report lock. The holder may be an
 * ordinary record's write to a port that has stopped draining, or may be
 * interrupted by a handler that spins on a spinlock the reporter holds. */
#define KLOG_REPORT_WAIT_MS 100UL

/**
 * Begin a report that cannot wait for the big lock, the lock-order
 * checker's: its records go to the ring at once, from any processor, and
 * to the serial port under klog's report lock, which a record written to
 * the port by the usual path also holds, so that two never mix. The screen
 * and netconsole, which the big lock guards, do not have them, nor
 * ramoops's console. Interrupts are off from the begin to
 * `klog_report_end`, and the caller takes no lock in between. Returns a
 * KLOG_REPORT_ number, for `klog_report` and `klog_report_end`.
 */
int klog_report_begin(void);

/** Make one record of the report `how` began, as `klog` makes one. */
void klog_report(int how, int level, const char *subsystem, const char *format, ...);

/** End the report `how` began. */
void klog_report_end(int how);

/** Write out the records kept for later, oldest first, when this processor
 * may (`smp_serialized`) and no other is writing them out for it
 * (`klog_flush_stuck`), after the records a delivery the panic stopped left
 * in the queue; inside another record's delivery or a console write, put
 * them in the ring and the queue, ahead of what comes next, for that
 * delivery or write to write out when it ends. Nothing otherwise, and
 * nothing, with no interrupt bracket, while none is kept. */
void klog_flush(void);

/** Whether records are kept for later. */
int klog_kept(void);

/**
 * Write out the records kept for later from a processor that may not write
 * klog's shared state, outside an NMI, once the hard-lockup detector has
 * reported the holder of the big lock locked up: that one would never
 * write them, and Linux writes such a report from the processor that found
 * it. One processor at a time does, and only the records kept when it
 * began. It does nothing while the holder is writing klog's state itself,
 * a record, a console write or a console byte, and the holder, should it
 * come back meanwhile, keeps its records for later until this is done and
 * waits for it before a console byte goes out.
 */
void klog_flush_stuck(void);

/** Whether this processor, which holds the big lock, is writing klog's
 * state: a record, a console write or a console byte. A switch to another
 * thread waits until it is not (`sched_preempt`), since the count would
 * stay up while the big lock passes on and `klog_flush_stuck` would stand
 * back for good. */
int klog_inside(void);

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
 * because nothing after it is expected to run. The records it leaves in the
 * queue go out ahead of the kept records and the next record, which a full
 * queue cannot drop; one a delivery was part way through goes out again
 * whole.
 */
void klog_deliver_directly(void);

/**
 * Show no more records on the screen, and nowhere else changed: for a
 * panic, which draws its own report there (panic.c) and sends its records
 * to the other destinations. It takes no lock, and nothing undoes it.
 */
void klog_screen_stop(void);

/**
 * Set the lowest-priority record the screen shows.
 *
 * The ring, the serial port and netconsole are not affected: every record is
 * kept and sent whatever this is, so turning the screen down loses nothing
 * that a later reader of `/dev/bootlog` or a capture on the port would want.
 * What it decides is how much of the machine's own record a person standing
 * at it has to read. The default keeps the boot log, `KLOG_INFO`, and drops
 * `KLOG_DEBUG`; a program talking to a person turns it down for its own run
 * and puts it back. `KLOG_CRIT` is the floor, so a critical record always
 * shows, until a panic stops the screen's records (`klog_screen_stop`). A
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
 * nest, and no writer sleeps inside one, nor is switched out
 * (`klog_inside`). `console_write_end` writes out the records that waited,
 * unless a record's delivery is in progress, which writes them itself.
 */
void console_write_begin(void);
void console_write_end(void);

/** As `k_puts`: the string and then a newline, as one console write. A NULL
 * pointer is ignored. */
void console_puts(const char *str);

#endif /* _KLOG_H_ */
