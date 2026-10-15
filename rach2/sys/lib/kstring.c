/* The string and memory functions of the freestanding kernel. */
#include "kstring.h"

#include "../arch/arch.h"

size_t k_strlen(const char *str)
{
    const char *end;
    for (end = str; *end; ++end)
        ;
    return end - str;
}

/**
 * An empty string is left untouched; without that guard the length-minus-one
 * upper index would wrap to SIZE_MAX and walk off the buffer.
 */
void k_strreverse(char *str)
{
    size_t i, j, len;
    char c;

    len = k_strlen(str);
    if (len == 0) {
        return;
    }
    j = len - 1;
    for (i = 0; i < j; ++i, --j) {
        c = str[i];
        str[i] = str[j];
        str[j] = c;
    }
}

void k_strtoupper(char *str)
{
    while (*str) {
        *str = k_toupper(*str);
        ++str;
    }
}

/*
 * `k_memcpy` and `k_memset` move a machine word at a time where the
 * architecture has string instructions for it: its cpu.h defines
 * ARCH_STRING_WORDS, and `arch_copy_words` and `arch_fill_words` use them
 * (on an x86, `rep movs` and `rep stos` with the direction flag cleared,
 * since a user program may leave it set at a system call). The kernel is
 * built without optimisation, so a byte loop costs a load, a store and a
 * compare per byte, and every disk and network path passes each byte
 * through several of them; on the Lenovo, byte loops limit the SSD to
 * about 90 MB/s. A byte loop moves the tail. Elsewhere, and in the host
 * tests, the byte loop is the whole function, and the tests of the tail
 * run against it.
 */
#if defined(ARCH_STRING_WORDS) && !defined(RACH2_HOSTTEST)
#define KSTRING_WORDS 1
#endif

void *k_memset(void *ptr, int c, size_t n)
{
    char *str = (char *)ptr;
#ifdef KSTRING_WORDS
    size_t words = n / sizeof(unsigned long);

    if (words) {
        /* The byte in every byte of a word: ~0 / 0xFF is 0x01 in each. */
        unsigned long fill = (unsigned long)(unsigned char)c * (~0UL / 0xFFu);

        arch_fill_words(str, fill, words);
        str += words * sizeof(unsigned long);
        n %= sizeof(unsigned long);
    }
#endif
    while (n--) {
        *str++ = c;
    }
    return ptr;
}

void *k_memcpy(void *dest, const void *src, size_t n)
{
    char *d = (char *)dest;
    const char *s = (const char *)src;
#ifdef KSTRING_WORDS
    size_t words = n / sizeof(unsigned long);

    if (words) {
        arch_copy_words(d, s, words);
        d += words * sizeof(unsigned long);
        s += words * sizeof(unsigned long);
        n %= sizeof(unsigned long);
    }
#endif
    while (n--) {
        *d++ = *s++;
    }
    return dest;
}

/**
 * When the destination starts inside the source, a forward copy would
 * overwrite source bytes before they are read, so the copy runs backward
 * from the last byte. Every other arrangement copies forward through
 * `k_memcpy`, which is safe with the destination below the source or apart
 * from it, in words as in bytes: each word is read before the word at or
 * below it is written.
 *
 * The overlap test compares addresses as integers, not as pointers: a
 * relational compare of pointers into two different objects is undefined in
 * C, and a copy between two separate structs is this function's normal case
 * under tcc.
 */
void *k_memmove(void *dest, const void *src, size_t n)
{
    char *d = (char *)dest;
    const char *s = (const char *)src;
    uintptr_t du = (uintptr_t)dest;
    uintptr_t su = (uintptr_t)src;

    if (du > su && du - su < n) {
        d += n;
        s += n;
        while (n--) {
            *--d = *--s;
        }
        return dest;
    }
    return k_memcpy(dest, src, n);
}

/**
 * The standard memory routines and `strlen`, as wrappers over the `k_`
 * bodies above. A kernel linked without a libc must define what its
 * compiler emits, or those calls fail at link time. The bodies stay in the
 * `k_` forms because the host test runs this file under AddressSanitizer,
 * which rewrites every call to a standard memory name into its own checked
 * copy; a body under the standard name would be unreachable there. The
 * size parameter is `__SIZE_TYPE__`, the type each compiler's builtin
 * prototype expects, which `-Werror` enforces on these definitions.
 */
void *memset(void *ptr, int c, __SIZE_TYPE__ n)
{
    return k_memset(ptr, c, (size_t)n);
}

void *memcpy(void *dest, const void *src, __SIZE_TYPE__ n)
{
    return k_memcpy(dest, src, (size_t)n);
}

__SIZE_TYPE__ strlen(const char *s)
{
    return k_strlen(s);
}

void *memmove(void *dest, const void *src, __SIZE_TYPE__ n)
{
    return k_memmove(dest, src, (size_t)n);
}

char *k_strncpy(char *dest, const char *src, size_t n)
{
    size_t i;
    for (i = 0; i < n && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }
    for (; i < n; i++) {
        dest[i] = '\0';
    }
    return dest;
}
