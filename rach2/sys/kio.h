/*
 * kio.h: the console's screen as a terminal, which interprets what is
 * written to it, with the console's lock, its font and the screen's print
 * calls.
 */
#ifndef _KIO_H_
#define _KIO_H_

#include "display/textcon.h"

#include "lib/kdef.h"
#include "lib/kfmt.h"
#include "lib/kstd.h"
#include "lib/kint.h"

/* Printing */

/** Null character. */
#define CHAR_NULL '\0'

/** Bell. */
#define CHAR_BEL 0x07

/** Backspace. */
#define CHAR_BS '\b'

/** Horizontal tab. */
#define CHAR_HT '\t'

/** Tab width in spaces, a terminal's. */
#define TAB_SIZE 8

/** Line feed. */
#define CHAR_LF 0x0A

/** Carriage return. */
#define CHAR_CR 0x0D

/** Stand-in glyph for any non-printable character. */
#define CHAR_NON_PRINTABLE '?'

/**
 * Write the byte `c` to the console's screen, as a terminal takes it.
 *
 * A plain byte is drawn at the cursor; BS, HT, LF (which also returns the
 * carriage) and CR move the cursor; NUL and BEL draw nothing; any other
 * control byte is drawn as `CHAR_NON_PRINTABLE`. A byte from 0x80 up is part
 * of a UTF-8 character, drawn from the console's font or as a stand-in.
 * `ESC` begins an escape sequence: a CSI (`ESC [`) moves the cursor, erases,
 * sets the colors, saves and restores the cursor, or answers a query
 * (`k_take_answer`); a private CSI, one with an intermediate byte, a charset
 * selector and an OSC, DCS, APC, PM or SOS string are read and not drawn.
 * display/Readme.md lists the sequences.
 *
 * The byte's work runs under the console's lock (`console_lock`), so an
 * interrupt handler and another processor may write too, and the hardware
 * cursor is placed after it unless a writer holds it (`textcon_hold`).
 */
void k_putc(char c);

/** The console's columns and rows, which its terminal reports as its window
 * size: 80 by 25 on the VGA's text mode, and on a framebuffer what the
 * screen holds. */
void k_console_size(unsigned int *cols, unsigned int *rows);

/**
 * The console's lock, over the screen's writer and the serial port's
 * receive ring: interrupts off, and the lock taken unless this processor
 * holds it already, so that a panic or a message raised inside the console
 * still reaches it. A processor that has begun a panic takes no lock.
 * Returns the flags `console_unlock` puts back.
 */
uint32_t console_lock(void);

/** Give back what `console_lock` took. */
void console_unlock(uint32_t flags);

/**
 * Load the console's font with the Cyrillic letters into the adapter, and
 * draw from it when the adapter reads it back; otherwise keep the adapter's
 * own font, code page 437. On a framebuffer the console draws the whole font
 * itself. The log records which font the console has.
 */
void k_font_init(void);

/** The longest answer the screen gives to a query, a cursor position. */
#define K_ANSWER_MAX 16

/**
 * Take the screen's answer to the queries written to it since the last call
 * into `buf`, of `size` bytes, and return its length, 0 when there is none.
 * An answer longer than `size` is dropped, and 0 returned.
 */
unsigned int k_take_answer(char *buf, unsigned int size);

/**
 * Write a null-terminated string to the screen, then a newline, with the
 * cursor placed once at the end.
 *
 * A NULL pointer is ignored.
 */
void k_puts(const char *str);

/**
 * Format according to `format` and write the result to the screen, with the
 * cursor placed once at the end.
 *
 * The variadic arguments supply the conversions; see `k_vprintf` for the
 * supported set.
 */
void k_printf(const char *format, ...);

#ifdef RACH2_HARNESS
/** Report a process exit status to the emulator the test harness runs the
 * kernel under, which exits with it (harness only): on a PC, a write to
 * QEMU's isa-debug-exit port, 0xF4, after which QEMU exits with
 * `(status << 1) | 1`. */
void harness_exit(uint8_t status);
#endif

#endif
