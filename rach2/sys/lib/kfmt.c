#include "kfmt.h"
#include "kstd.h"

/**
 * Format `format` against the arguments in `ap`, emitting each byte through
 * `putc_fn`.
 *
 * Supported conversions are `%d` (signed decimal), `%b`, `%o`, `%x`, `%X`
 * (binary, octal, lower- and upper-case hex), `%s` (a string, with a NULL
 * pointer rendered as `(null)`), `%c` (a character), and `%%` (a literal
 * percent). The numeric conversions and `%s` take an optional `0` flag and
 * an optional single-digit width between the percent and the letter, and are
 * padded on the left to that width, so `%02x` writes a byte as two hex
 * digits. The padding is done here and not in `k_itoa`, so numbers and
 * strings share one rule. `ap` is consumed left to right; a character is
 * read as `int`, which is what the call passed.
 *
 * An unknown conversion writes its letter alone, without the `%`, flag or
 * width, and consumes nothing, so every argument after it in the same call
 * is read one slot early. That is why `tools/verify/fmtcheck.sh`, in the
 * style check, refuses a format string with a conversion this does not
 * have. A format that ends on its `%`, or on a flag or width after it,
 * writes the `%` and returns, since reading on would pass the terminator.
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
                /* The format ended on its `%`. */
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
