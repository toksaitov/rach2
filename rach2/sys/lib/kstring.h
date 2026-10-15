#ifndef _KSTRING_H_
#define _KSTRING_H_

/* The string and memory functions of the freestanding kernel. */

#include "kdef.h"
#include "kctypes.h"

/** Returns the length of the null-terminated string `str`. */
size_t k_strlen(const char *str);

/** Reverses the characters of the null-terminated string `str` in place. */
void k_strreverse(char *str);

/** Upper-cases the letters of the null-terminated string `str` in place. */
void k_strtoupper(char *str);

/** Writes `n` bytes of `c`, taken as an unsigned char like memset's, to
 * `ptr` and returns `ptr`. */
void *k_memset(void *ptr, int c, size_t n);

/**
 * Copies `n` bytes from `src` to `dest` and returns `dest`. The regions
 * must not overlap.
 */
void *k_memcpy(void *dest, const void *src, size_t n);

/** Copies `n` bytes from `src` to `dest`; the regions may overlap. */
void *k_memmove(void *dest, const void *src, size_t n);

/*
 * The standard names, which the compilers emit calls to on their own: gcc
 * for struct copies, large initializers and, at -O2, `strlen`, and tcc for
 * struct assignment and initializer holes. Each is the `k_` form under the
 * standard name, with the compiler's own size type, which its builtin
 * prototype check requires. Kernel code calls the `k_` forms.
 */
__SIZE_TYPE__ strlen(const char *s);
void *memset(void *ptr, int c, __SIZE_TYPE__ n);
void *memcpy(void *dest, const void *src, __SIZE_TYPE__ n);
void *memmove(void *dest, const void *src, __SIZE_TYPE__ n);

/**
 * Copies up to `n` characters from `src` to `dest` and returns `dest`.
 * If `src` is shorter than `n`, the remainder of `dest` is null-padded;
 * if it is not, `dest` is not terminated.
 */
char *k_strncpy(char *dest, const char *src, size_t n);

#endif
