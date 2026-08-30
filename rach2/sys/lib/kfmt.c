#include "kfmt.h"
#include "kstd.h"

/**
 * Format `format` against the varargs in `args`, emitting each byte through
 * `putc_fn`.
 *
 * Supported conversions are `%d` (signed decimal), `%b`, `%o`, `%x`, `%X`
 * (binary, octal, lower- and upper-case hex), `%s` (a string, with a NULL
 * pointer rendered as `(null)`), `%c` (a character), and `%%` (a literal
 * percent). An unknown conversion is emitted verbatim. `args` points at the
 * first argument and is consumed left to right; each consumed slot is one
 * stack word, which is why integers and characters are read as `int`.
 */
void k_vprintf(putc_fn_t putc_fn, const char *format, char **args)
{
    char c, buffer[_K_PRINTF_BUFFER_SIZE_];
    const char *str;

    if (format == NULL)
        return;

    while ((c = *(format++))) {
        if (c == '%') {
            c = *(format++);

            switch (c) {
            case 'd':
                k_itoa(*((unsigned int *)args++), buffer,
                       _K_PRINTF_BUFFER_SIZE_, c, false);
                str = buffer;
                goto string;
            case 'b':
            case 'o':
            case 'x':
            case 'X':
                k_itoa(*((int *)args++), buffer, _K_PRINTF_BUFFER_SIZE_, c,
                       false);
                str = buffer;
                goto string;

            case 's':
                str = *args++;
                if (!str)
                    str = "(null)";

            string:
                while (*str)
                    putc_fn(*str++);
                break;

            case 'c':
                putc_fn((char)*((int *)args++));
                break;

            case 0:
                /* The format ended on its `%`. The sign is written out, as
                   any other conversion this does not know would be, and then
                   this returns: leaving the switch instead would put the loop
                   past the terminator, reading bytes that are not the
                   string's. */
                putc_fn('%');
                return;

            case '%':
            default:
                putc_fn(c);
                break;
            }
        } else
            putc_fn(c);
    }
}
