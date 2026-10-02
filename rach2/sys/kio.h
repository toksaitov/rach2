#ifndef _KIO_H_
#define _KIO_H_

#include "display/vga_text.h"

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

/* Processor I/O */

/** Write byte `val` to I/O port `port`. */
void outb(uint16_t port, uint8_t val);

#ifdef RACH2_HARNESS
/** Report a process exit status to QEMU's isa-debug-exit device (harness
 * only). */
void harness_exit(uint8_t status);
#endif

/** Write word `val` to I/O port `port`. */
void outw(uint16_t port, uint16_t val);

/** Write double-word `val` to I/O port `port`. */
void outd(uint16_t port, uint32_t val);

/** Read a byte from I/O port `port`. */
uint8_t inb(uint16_t port);

/** Read a word from I/O port `port`. */
uint16_t inw(uint16_t port);

/** Read a double-word from I/O port `port`. */
uint32_t ind(uint16_t port);

/** Read `count` 16-bit words from I/O port `port` into `buffer`. */
void insw(uint16_t port, void *buffer, uint32_t count);

/** Write `count` 16-bit words from `buffer` to I/O port `port`. */
void outsw(uint16_t port, const void *buffer, uint32_t count);

/** Aliases under the conventional outl/inl names. */
#define outl outd
#define inl  ind

#endif
