#include "kstring.h"

size_t k_strlen(const char *str)
{
    const char *end; for (end = str; *end; ++end); return end - str;
}

/**
 * Reverse the NUL-terminated string `str` in place.
 *
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
    for (i = 0, j = len - 1; i < j; ++i, --j) {
        c = str[i]; str[i] = str[j]; str[j] = c;
    }
}

void k_strtoupper(char *str)
{
    while (*str) { *str = k_toupper(*str); ++str; }
}

/*
 * The copies and the fill move a machine word at a time through the string
 * instructions on the kernel's own target. The kernel is built without
 * optimisation, so a byte loop costs a load, a store and a compare per byte,
 * and every disk and network path passes each byte through several of them;
 * on the Lenovo the byte loops bounded the SSD at 90 MB/s. `rep movsl` and
 * `rep stosl` move the words, a byte loop the tail of up to three, and the
 * direction flag is cleared first since a user program may leave it set at a
 * system call. Both compilers that build the kernel take this asm form. On
 * any other target, the host tests' among them, the byte loop is the whole
 * function, and the tests of the tail run against it.
 */
#if defined(__i386__)
#define KSTRING_WORDS 1
#endif

void* k_memset(void *ptr, char c, size_t n)
{
    char *str = (char *) ptr;
#ifdef KSTRING_WORDS
    size_t words = n >> 2;

    if (words) {
        unsigned int fill = (unsigned char) c;

        fill |= fill << 8;
        fill |= fill << 16;
        __asm__ __volatile__("cld; rep stosl"
                             : "+D"(str), "+c"(words)
                             : "a"(fill)
                             : "memory", "cc");
        n &= 3;
    }
#endif
    while (n--) { *str++ = c; }
    return ptr;
}

void* k_memcpy(void *dest, const void *src, size_t n)
{
    char *d = (char *) dest;
    const char *s = (const char *) src;
#ifdef KSTRING_WORDS
    size_t words = n >> 2;

    if (words) {
        __asm__ __volatile__("cld; rep movsl"
                             : "+S"(s), "+D"(d), "+c"(words)
                             :
                             : "memory", "cc");
        n &= 3;
    }
#endif
    while (n--) { *d++ = *s++; }
    return dest;
}

/**
 * Copy `n` bytes between ranges that may overlap.
 *
 * When the destination starts inside the source, a forward copy would
 * overwrite source bytes before they are read, so the copy runs backward
 * from the last byte. Every other arrangement copies forward.
 */
void* k_memmove(void *dest, const void *src, size_t n)
{
    char *d = (char *) dest;
    const char *s = (const char *) src;
    __SIZE_TYPE__ du = (__SIZE_TYPE__) dest;
    __SIZE_TYPE__ su = (__SIZE_TYPE__) src;

    /* The overlap test compares addresses as integers, not as pointers: a
     * relational compare of pointers into two different objects is undefined
     * in C, and a copy between two separate structs is this function's normal
     * case under tcc. The integer type is `__SIZE_TYPE__` because it is
     * pointer-width under every compiler that builds this file, the host's
     * included; the kernel's own `uintptr_t` is 32 bits and would truncate
     * the host test's addresses. */
    if (du > su && du - su < n) {
        d += n;
        s += n;
        while (n--) { *--d = *--s; }
        return dest;
    }
    /* A forward copy is safe with the destination below the source or apart
     * from it, in words as in bytes: each word is read before the word at or
     * below it is written. */
    return k_memcpy(dest, src, n);
}

/**
 * The standard memory routines, as wrappers over the `k_` bodies above.
 *
 * Kernel code calls the `k_` names; these wrappers exist because both
 * compilers that build this kernel emit calls to the standard names on
 * their own: gcc can emit `memcpy` or `memset` for a struct copy or a large
 * initializer, and tcc emits `memmove` for every struct assignment and
 * `memset` for initializer holes. A kernel linked without a libc must
 * define what its compiler emits, or those calls fail at link time. The
 * bodies stay in the `k_` forms because the host test runs this file
 * under AddressSanitizer, which rewrites every call to a standard memory
 * name into its own checked copy; a body under the standard name would be
 * unreachable there. The size parameter is `__SIZE_TYPE__`, the type each
 * compiler's builtin prototype expects, which `-Werror` holds these
 * definitions to.
 */
void* memset(void *ptr, int c, __SIZE_TYPE__ n)
{
    return k_memset(ptr, (char) c, (size_t) n);
}

void* memcpy(void *dest, const void *src, __SIZE_TYPE__ n)
{
    return k_memcpy(dest, src, (size_t) n);
}

void* memmove(void *dest, const void *src, __SIZE_TYPE__ n)
{
    return k_memmove(dest, src, (size_t) n);
}

char* k_strncpy(char *dest, const char *src, size_t n)
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
