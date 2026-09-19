#include "kfmt.h"
#include "kstd.h"

/**
 * Format `format` against the arguments in `ap`, emitting each byte through
 * `putc_fn`.
 *
 * Supported conversions are `%d` (signed decimal), `%b`, `%o`, `%x`, `%X`
 * (binary, octal, lower- and upper-case hex), `%s` (a string, with a NULL
 * pointer rendered as `(null)`), `%c` (a character), and `%%` (a literal
 * percent). A numeric conversion takes an optional `0` flag
 * and an optional single-digit width between the percent and the letter, so
 * `%02x` writes a byte as two hex digits. `ap` is consumed left to right; a
 * character is read as `int`, which is what the call passed.
 *
 * An unknown conversion is emitted verbatim AND CONSUMES NOTHING, so every
 * argument after it in the same call is read one slot early. That is why the
 * style check refuses a format string this cannot do: a message that asked
 * for `%02x` before the flag and width were understood printed the literal
 * text `02x` and then read the wrong value for every field after it.
 */
void k_vprintf(putc_fn_t putc_fn, const char *format, va_list ap)
{
    char c, buffer[_K_PRINTF_BUFFER_SIZE_];
    const char *str;

    if (format == NULL)
        return;

    while ((c = *(format++))) {
        if (c == '%') {
            char fill = ' ';
            int width = 0;

            c = *(format++);
            if (c == '0') {
                fill = '0';
                c = *(format++);
            }
            while (c >= '1' && c <= '9' && width == 0) {
                width = c - '0';
                c = *(format++);
            }

            switch (c) {
            case 'd':
                k_itoa(va_arg(ap, unsigned int), buffer, _K_PRINTF_BUFFER_SIZE_, c, false);
                str = buffer;
                goto string;
            case 'b':
            case 'o':
            case 'x':
            case 'X':
                k_itoa(va_arg(ap, int), buffer, _K_PRINTF_BUFFER_SIZE_, c, false);
                str = buffer;
                goto string;

            case 's':
                str = va_arg(ap, const char *);
                if (!str)
                    str = "(null)";

            string: {
                /* The pad is counted here rather than in `k_itoa`, so one
                   rule covers every conversion and the converter keeps
                   its own contract. */
                size_t len = 0;

                while (str[len])
                    len++;
                while ((int)len < width) {
                    putc_fn(fill);
                    width--;
                }
            }
                while (*str)
                    putc_fn(*str++);
                break;

            case 'c':
                putc_fn((char)va_arg(ap, int));
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
