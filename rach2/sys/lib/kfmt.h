#ifndef _KFMT_H_
#define _KFMT_H_

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
 * Format according to `format` against `args`, emitting each byte through
 * `putc_fn`.
 *
 * Supported conversions are `%d` (signed decimal), `%b`, `%o`, `%x`, `%X`
 * (binary, octal, lower- and upper-case hex), `%s` (a string, with a NULL
 * pointer rendered as `(null)`), `%c` (a character), and `%%` (a literal
 * percent). An unknown conversion is emitted verbatim. `args` points at the
 * first argument and is consumed left to right; each consumed slot is one
 * stack word, which is why integers and characters are read as `int`.
 */
void k_vprintf(putc_fn_t putc_fn, const char *format, char **args);

#endif /* _KFMT_H_ */
