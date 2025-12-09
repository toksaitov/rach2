#include "kstd.h"

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

    /* For signed decimal, handle negative; for others, treat as unsigned */
    if (is_signed && input < 0) {
        uinput = (unsigned int)(-input);
        ++num_of_chars;
    } else if (is_signed) {
        uinput = (unsigned int)input;
        if (input > 0 && force_sign) { ++num_of_chars; }
    } else {
        uinput = (unsigned int)input;
    }

    /* Count digits */
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
