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

/** Write one character to the screen. */
void k_putc(char c);

/**
 * The console's lock, over the screen's writer and the serial port's
 * receive ring: interrupts off, and the lock taken unless this processor
 * holds it already, so that a panic or a message raised inside the console
 * still reaches it. Returns the flags `console_unlock` puts back.
 */
uint32_t console_lock(void);

/** Give back what `console_lock` took. */
void console_unlock(uint32_t flags);

/**
 * Load the console's font with the Cyrillic letters into the adapter, and
 * draw from it when the adapter reads it back; otherwise keep the adapter's
 * own font, code page 437.
 */
void k_font_init(void);

/** The longest answer the screen gives to a query, a cursor position. */
#define K_ANSWER_MAX 16

/**
 * Take the screen's answer to the queries written to it since the last call
 * into `buf`, of `size` bytes, and return its length, 0 when there is none.
 */
unsigned int k_take_answer(char *buf, unsigned int size);

/**
 * Write a null-terminated string to the screen, then a newline.
 *
 * A NULL pointer is ignored.
 */
void k_puts(const char *str);

/**
 * Format according to `format` and write the result to the screen.
 *
 * The variadic arguments supply the conversions; see `k_vprintf` for the
 * supported set.
 */
void k_printf(const char *format, ...);

#ifdef RACH2_HARNESS
/** Report a process exit status to the emulator the test harness runs the
 * kernel under, which exits with it (harness only). */
void harness_exit(uint8_t status);
#endif

#endif
