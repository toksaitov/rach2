/* The kernel's conversion of an integer to text. */
#include "kstd.h"

/**
 * The digits are written least significant first and the string is then
 * reversed. A negative decimal's magnitude is taken by unsigned negation,
 * which is defined even for LONG_MIN.
 */
size_t k_ltoa(long input, char *buffer, size_t buffer_size, int radix, bool force_sign)
{
    bool switch_case = false;
    bool is_signed = false;
    unsigned long uinput;
    long initial_input = input;
    char digit, *tmp_str = buffer;
    size_t num_of_chars = 1;

    switch (radix) {
    case 'b':
        radix = 2;
        break;

    case 'o':
        radix = 8;
        break;

    case 'd':
        radix = 10;
        is_signed = true;
        break;

    case 'X':
        switch_case = true;
        /* fall through */
    case 'x':
        radix = 16;
        break;
    default:
        break;
    }

    if (radix < 2 || radix > 36)
        return 0;

    if (is_signed && input < 0) {
        uinput = -(unsigned long)input;
        ++num_of_chars;
    } else if (is_signed) {
        uinput = (unsigned long)input;
        if (input > 0 && force_sign) {
            ++num_of_chars;
        }
    } else {
        uinput = (unsigned long)input;
    }

    {
        unsigned long tmp = uinput;
        do {
            ++num_of_chars;
        } while (tmp /= (unsigned long)radix);
    }

    if (buffer_size < num_of_chars) {
        num_of_chars = 0;
    } else if (buffer != NULL) {
        do {
            digit = (char)(uinput % (unsigned long)radix);

            if (digit <= 9) {
                *tmp_str++ = digit + '0';
            } else {
                *tmp_str++ = digit - 10 + 'a';
            }
        } while (uinput /= (unsigned long)radix);

        if (is_signed && initial_input < 0) {
            *tmp_str++ = '-';
        } else if (is_signed && initial_input > 0 && force_sign) {
            *tmp_str++ = '+';
        }
        *tmp_str = 0;

        k_strreverse(buffer);

        if (switch_case) {
            k_strtoupper(buffer);
        }
    }

    return num_of_chars;
}

/**
 * An int other than a decimal is widened through unsigned int, so that a
 * negative one is not sign-extended into the high bits of a 64-bit long.
 */
size_t k_itoa(int input, char *buffer, size_t buffer_size, int radix, bool force_sign)
{
    long wide = input;

    if (radix != 'd') {
        wide = (long)(unsigned int)input;
    }
    return k_ltoa(wide, buffer, buffer_size, radix, force_sign);
}
