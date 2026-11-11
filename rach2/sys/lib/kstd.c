/* The kernel's conversions between integers and text. */
#include "kstd.h"
#include "kerrno.h"
#include "kint.h"

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

/** The value of the digit `c` in any base up to 16, or 16 for none. */
static unsigned int digit_value(char c)
{
    if (c >= '0' && c <= '9') {
        return (unsigned int)(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return (unsigned int)(c - 'a' + 10);
    }
    if (c >= 'A' && c <= 'F') {
        return (unsigned int)(c - 'A' + 10);
    }
    return 16;
}

/* The largest value of 64 bits, the width Linux's kstrtoul parses at. */
#define KSTD_U64_MAX (~(uint64_t)0)

/**
 * Read the unsigned 64-bit number at the `len` bytes of `s`, which end
 * sooner at a NUL, as Linux's _kstrtoull with base 0 (lib/kstrtox.c:
 * 93-110): hexadecimal after a 0x whose next byte is a hex digit, octal
 * after another leading 0, else decimal, then at most one newline. An
 * overflow is -ERANGE whatever follows it; junk, or no digit, -EINVAL.
 * The limits are constants, so that i686 needs no 64-bit division.
 */
static int parse_u64(const char *s, size_t len, uint64_t *out)
{
    uint64_t v = 0, lim;
    unsigned int base = 10, d, last;
    size_t i = 0, start;

    for (start = 0; start < len && s[start] != '\0'; start++) {
    }
    len = start;
    if (i < len && s[i] == '0') {
        base = 8;
        if (i + 2 < len && (s[i + 1] == 'x' || s[i + 1] == 'X') && digit_value(s[i + 2]) < 16) {
            base = 16;
            i += 2;
        }
    }
    lim = base == 16 ? KSTD_U64_MAX / 16u : base == 8 ? KSTD_U64_MAX / 8u : KSTD_U64_MAX / 10u;
    last = base == 10 ? (unsigned int)(KSTD_U64_MAX % 10u) : base - 1u;
    for (start = i; i < len && (d = digit_value(s[i])) < base; i++) {
        if (v > lim || (v == lim && d > last)) {
            return -ERANGE;
        }
        v = v * base + d;
    }
    if (i == start) {
        return -EINVAL;
    }
    if (i < len && s[i] == '\n') {
        i++;
    }
    if (i < len) {
        return -EINVAL;
    }
    *out = v;
    return 0;
}

/** parse_u64 after an optional `+`, as Linux's kstrtoull. */
static int parse_plus_u64(const char *s, size_t len, uint64_t *out)
{
    if (len > 0 && s[0] == '+') {
        s++;
        len--;
    }
    return parse_u64(s, len, out);
}

/**
 * The number is read in 64 bits and its end checked before it is held to
 * an unsigned long, as Linux's kstrtoul calls kstrtoull first
 * (lib/kstrtox.c:181-190): on i686 "4294967296x" is then -EINVAL for its
 * junk, and a 64-bit overflow is -ERANGE whatever follows it.
 */
int k_strtoul(const char *s, size_t len, unsigned long *out)
{
    uint64_t v;
    int rc = parse_plus_u64(s, len, &v);

    if (rc != 0) {
        return rc;
    }
    if (v > (uint64_t)~0ul) {
        return -ERANGE;
    }
    *out = (unsigned long)v;
    return 0;
}

/**
 * As Linux's kstrtoll (lib/kstrtox.c:156-177): a `-` takes the magnitude
 * with no `+` after it, up to 2^63; otherwise the value up to 2^63 - 1.
 */
int k_strtoll(const char *s, size_t len, long long *out)
{
    uint64_t v, half = (uint64_t)1 << 63;
    int rc;

    if (len > 0 && s[0] == '-') {
        rc = parse_u64(s + 1, len - 1, &v);
        if (rc != 0) {
            return rc;
        }
        if (v > half) {
            return -ERANGE;
        }
        /* -2^63 too: the conversion of 2^63 wraps, as the compilers do. */
        *out = (long long)(0 - v);
        return 0;
    }
    rc = parse_plus_u64(s, len, &v);
    if (rc != 0) {
        return rc;
    }
    if (v >= half) {
        return -ERANGE;
    }
    *out = (long long)v;
    return 0;
}
