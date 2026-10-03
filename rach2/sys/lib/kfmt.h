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

/** Buffer size for argument interpolation in `k_vprintf`. */
#define _K_PRINTF_BUFFER_SIZE_ 128

/** A character-output sink. */
typedef void (*putc_fn_t)(char);

/**
 * Format according to `format` against `ap`, emitting each byte through
 * `putc_fn`.
 *
 * Supported conversions are `%d` (signed decimal), `%u` (unsigned decimal),
 * `%b`, `%o`, `%x`, `%X` (binary, octal, lower- and upper-case hex), `%p` (a
 * pointer), `%s` (a
 * string, with a NULL pointer rendered as `(null)`), `%c` (a character), and
 * `%%` (a literal percent). The numeric conversions and `%s` take an optional
 * `0` flag and a single-digit width, and a numeric conversion an `l` or `z`
 * for a long or a `size_t`. An unknown conversion writes its letter alone,
 * without the `%`, flag or width, and consumes no argument. `ap` is consumed
 * left to right, an int, a long or a pointer for each conversion as its
 * letter and length say.
 */
void k_vprintf(putc_fn_t putc_fn, const char *format, va_list ap);

#endif /* _KFMT_H_ */
