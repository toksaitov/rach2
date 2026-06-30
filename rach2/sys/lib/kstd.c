#include "kstd.h"

/**
 * Convert `input` to its base-`radix` text in `buffer`.
 *
 * `radix` is either a numeric base in [2, 36] or one of the format letters
 * 'b', 'o', 'd', 'x', 'X' selecting binary, octal, decimal, lowercase hex,
 * or uppercase hex. Only the 'd' form is signed: a negative value gets a
 * leading minus. The other bases format the raw bit pattern as unsigned.
 * `force_sign` reserves room in the returned count for a leading plus on a
 * positive decimal, but the current code does not write that plus.
 *
 * Returns the number of characters the conversion needs, including any sign
 * and the terminating NUL. It returns 0 and writes nothing when `buffer_size`
 * is smaller than that count or the radix is out of range; the two cases are
 * not distinguished.
 */
size_t k_itoa(int input, char *buffer, size_t buffer_size, int radix, bool force_sign)
{
    bool switch_case = false;
    bool is_signed = false;
    unsigned int uinput;
    int initial_input = input;
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
            __attribute__((fallthrough));
        case 'x':
            radix = 16;
            break;
    }

    if (radix < 2 || radix > 36) return 0;

    if (is_signed && input < 0) {
        uinput = -(unsigned int)input; /* unsigned negation: defined even for INT_MIN */
        ++num_of_chars;
    } else if (is_signed) {
        uinput = (unsigned int)input;
        if (input > 0 && force_sign) { ++num_of_chars; }
    } else {
        uinput = (unsigned int)input;
    }

    {
        unsigned int tmp = uinput;
        do { ++num_of_chars; } while (tmp /= radix);
    }

    if (buffer_size < num_of_chars) {
        num_of_chars = 0;
    } else if (buffer != NULL) {
        do {
            digit = uinput % radix;

            if (digit <= 9) { *tmp_str++ = digit + '0'; }
            else { *tmp_str++ = digit - 10 + 'a'; }
        } while (uinput /= radix);

        if (is_signed && initial_input < 0) { *tmp_str++ = '-'; }
        *tmp_str = 0;

        k_strreverse(buffer);

        if (switch_case) { k_strtoupper(buffer); }
    }

    return num_of_chars;
}
