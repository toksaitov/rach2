/* The kernel's text formatter, k_vprintf. */
#include "kfmt.h"
#include "kstd.h"

/**
 * Each number is converted into `buffer` and then written by the same code
 * as a string, so the padding is done here and not in `k_ltoa`, and numbers
 * and strings share one rule. `%u` converts its digits here, since `k_ltoa`
 * treats a decimal as signed. A format that ends after its `%` stops at the
 * terminator, since reading on would pass it.
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
            int wide = 0;

            c = *(format++);
            if (c == '0') {
                fill = '0';
                c = *(format++);
            }
            while (c >= '1' && c <= '9' && width == 0) {
                width = c - '0';
                c = *(format++);
            }
            if (c == 'l' || c == 'z') {
                wide = 1;
                c = *(format++);
            }

            switch (c) {
            case 'd':
                if (wide) {
                    k_ltoa(va_arg(ap, long), buffer, _K_PRINTF_BUFFER_SIZE_, c, false);
                } else {
                    k_itoa(va_arg(ap, int), buffer, _K_PRINTF_BUFFER_SIZE_, c, false);
                }
                str = buffer;
                goto string;
            case 'u': {
                unsigned long v = wide ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
                char digits[24];
                int n = 0, k = 0;

                do {
                    digits[n++] = (char)('0' + v % 10u);
                    v /= 10u;
                } while (v != 0);
                while (n > 0) {
                    buffer[k++] = digits[--n];
                }
                buffer[k] = '\0';
                str = buffer;
                goto string;
            }
            case 'b':
            case 'o':
            case 'x':
            case 'X':
                if (wide) {
                    k_ltoa((long)va_arg(ap, unsigned long), buffer, _K_PRINTF_BUFFER_SIZE_, c,
                           false);
                } else {
                    k_itoa((int)va_arg(ap, unsigned int), buffer, _K_PRINTF_BUFFER_SIZE_, c, false);
                }
                str = buffer;
                goto string;
            case 'p': {
                unsigned long v = (unsigned long)va_arg(ap, void *);
                int digits = (int)sizeof(void *) * 2;

                putc_fn('0');
                putc_fn('x');
                while (digits-- > 0) {
                    putc_fn("0123456789abcdef"[(v >> (4 * digits)) & 0xFu]);
                }
                break;
            }

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
