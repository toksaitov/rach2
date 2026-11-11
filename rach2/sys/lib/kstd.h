#ifndef _KSTD_H_
#define _KSTD_H_

/* The kernel's conversions between integers and text. */

#include "kdef.h"
#include "kstring.h"

/**
 * Convert the int `input` as `k_ltoa` converts a long: signed for 'd', and
 * for the other bases the 32 bits of the int as an unsigned number.
 */
size_t k_itoa(int input, char *buffer, size_t buffer_size, int radix, bool force_sign);

/**
 * Convert the long `input` to its base-`radix` text in `buffer`.
 *
 * `radix` is either a numeric base in [2, 36] or one of the format letters
 * 'b', 'o', 'd', 'x', 'X' selecting binary, octal, decimal, lowercase hex,
 * or uppercase hex. Only the 'd' form is signed: a negative value gets a
 * leading minus. The other bases format the raw bit pattern as unsigned.
 * `force_sign` writes a leading plus on a positive decimal, and the
 * returned count includes it; zero and the other bases get none.
 *
 * Returns the number of characters the conversion needs, including any sign
 * and the terminating NUL. It returns 0 and writes nothing when `buffer_size`
 * is smaller than that count or the radix is out of range; the two cases are
 * not distinguished. With a NULL `buffer` and a large enough `buffer_size` it
 * returns the count and writes nothing.
 */
size_t k_ltoa(long input, char *buffer, size_t buffer_size, int radix, bool force_sign);

/**
 * Read the unsigned number that is the text at `s`, as Linux's kstrtoul
 * with base 0 reads a sysfs write: an optional `+`, then hexadecimal after
 * `0x` or `0X`, octal after another leading 0, or decimal, and at most one
 * newline after it. The text ends at `len` bytes or at a NUL. 0 with the
 * value in `*out`, -EINVAL for text that is not such a number, or -ERANGE
 * for one an unsigned long cannot hold; `*out` is left alone on an error.
 */
int k_strtoul(const char *s, size_t len, unsigned long *out);

/**
 * Read the signed 64-bit number that is the text at `s`, as Linux's
 * kstrtos64 with base 0: a `-`, or an optional `+`, then the digits as
 * `k_strtoul` reads them, and at most one newline. 0 with the value in
 * `*out`, -EINVAL for text that is not such a number, or -ERANGE for one
 * past 64 signed bits; `*out` is left alone on an error.
 */
int k_strtoll(const char *s, size_t len, long long *out);

#endif
