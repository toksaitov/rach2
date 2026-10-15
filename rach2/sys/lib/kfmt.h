#ifndef _KFMT_H_
#define _KFMT_H_

#include <stdarg.h>

/*
 * The kernel's one text formatter.
 *
 * It writes through a character sink rather than to a destination of its own,
 * which is what lets the screen, the serial port and the log record share it.
 * It lives beside the other leaf library code, and not in `kio.c` with the
 * screen, so that a caller which formats but does not draw (the log) does not
 * pull the VGA console in behind it.
 */

/** The size of the buffer `k_vprintf` converts one number into. */
#define _K_PRINTF_BUFFER_SIZE_ 128

/** A character-output sink. */
typedef void (*putc_fn_t)(char);

/**
 * Format `format` against the arguments in `ap`, emitting each byte through
 * `putc_fn`. A NULL `format` writes nothing.
 *
 * Supported conversions are `%d` (signed decimal), `%u` (unsigned decimal),
 * `%b`, `%o`, `%x`, `%X` (binary, octal, lower- and upper-case hex), `%p`
 * (a pointer, as `0x` and its hex digits at the pointer's full width), `%s`
 * (a string, with a NULL pointer rendered as `(null)`), `%c` (a character),
 * and `%%` (a literal percent). The numeric conversions and `%s` take an
 * optional `0` flag and an optional single-digit width between the percent
 * and the letter, and are padded on the left to that width, so `%02x`
 * writes a byte as two hex digits. A numeric conversion takes an `l` before
 * its letter for a long or an unsigned long, and a `z` for a `size_t` or a
 * `ssize_t`, which are as wide as a long; without one it reads an int. `ap`
 * is consumed left to right; a character is read as `int`, which is what
 * the call passed.
 *
 * An unknown conversion writes its letter alone, without the `%`, flag or
 * width, and consumes nothing, so every argument after it in the same call
 * is read one slot early; `tools/verify/fmtcheck.sh`, in the style check,
 * refuses a format string with a conversion this does not have. A format
 * that ends on its `%`, or on a flag or width after it, writes the `%` and
 * stops.
 */
void k_vprintf(putc_fn_t putc_fn, const char *format, va_list ap);

#endif /* _KFMT_H_ */
