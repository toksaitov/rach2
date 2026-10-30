/* clib.c: the C library functions ACPICA calls that the kernel's sys/lib
 * does not have, as clib.h declares them, and i686's 64-bit division, as
 * acrach2.h declares it. */

#include "clib.h"

/** Compare `n` bytes as unsigned chars: below 0, 0 or above 0. */
int memcmp(const void *a, const void *b, __SIZE_TYPE__ n)
{
    const unsigned char *p = (const unsigned char *)a;
    const unsigned char *q = (const unsigned char *)b;

    for (; n > 0; n--, p++, q++) {
        if (*p != *q) {
            return *p < *q ? -1 : 1;
        }
    }
    return 0;
}

/** Append `src` to the string at `dest`; returns `dest`. */
char *strcat(char *dest, const char *src)
{
    strcpy(dest + strlen(dest), src);
    return dest;
}

/** Compare two strings as unsigned chars. */
int strcmp(const char *a, const char *b)
{
    for (; *a == *b; a++, b++) {
        if (*a == '\0') {
            return 0;
        }
    }
    return (unsigned char)*a < (unsigned char)*b ? -1 : 1;
}

/** Copy `src` with its terminator to `dest`; returns `dest`. */
char *strcpy(char *dest, const char *src)
{
    char *d = dest;

    while ((*d++ = *src++) != '\0') {
    }
    return dest;
}

/** Compare at most `n` characters of two strings as unsigned chars. */
int strncmp(const char *a, const char *b, __SIZE_TYPE__ n)
{
    for (; n > 0; n--, a++, b++) {
        if (*a != *b) {
            return (unsigned char)*a < (unsigned char)*b ? -1 : 1;
        }
        if (*a == '\0') {
            break;
        }
    }
    return 0;
}

/** The lowercase of an ASCII capital, any other `c` unchanged. */
int tolower(int c)
{
    return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c;
}

/** The capital of an ASCII lowercase letter, any other `c` unchanged. */
int toupper(int c)
{
    return c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c;
}

/** Whether `c` is a decimal digit. */
int isdigit(int c)
{
    return c >= '0' && c <= '9';
}

/** Whether `c` is printable ASCII, the space included. */
int isprint(int c)
{
    return c >= 0x20 && c < 0x7F;
}

/** Whether `c` is a space, tab, newline, vertical tab, form feed or return. */
int isspace(int c)
{
    return c == ' ' || (c >= '\t' && c <= '\r');
}

/** Whether `c` is a hexadecimal digit. */
int isxdigit(int c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

#if !defined(__x86_64__)
/**
 * Divide the 64-bit n_hi:n_lo by `d`, for ACPICA's ACPI_DIV_64_BY_32 on
 * i686, where the kernel links no libgcc: long division, one bit of the
 * quotient a step, in 32-bit arithmetic alone. `n_hi` must be below `d`, so
 * that the quotient, stored at `q`, fits; the remainder is stored at `r`. A
 * bit carried out of the partial remainder makes it 2^32 more than `rem`,
 * above `d`, and the difference, below `d`, is `rem - d` modulo 2^32.
 */
void acpi_div_64_by_32(unsigned int n_hi, unsigned int n_lo, unsigned int d, unsigned int *q,
                       unsigned int *r)
{
    unsigned int rem = n_hi;
    unsigned int quo = 0;
    unsigned int out;
    int i;

    for (i = 31; i >= 0; i--) {
        out = rem >> 31;
        rem = (rem << 1) | ((n_lo >> i) & 1u);
        quo <<= 1;
        if (out != 0 || rem >= d) {
            rem -= d;
            quo |= 1u;
        }
    }
    *q = quo;
    *r = rem;
}
#endif
