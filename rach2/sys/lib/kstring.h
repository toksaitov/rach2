#ifndef _KSTRING_H_
#define _KSTRING_H_

#include "kdef.h"
#include "kctypes.h"

/** Returns the length of the null-terminated string `str`. */
size_t k_strlen(const char *str);

/** Reverses the characters of the null-terminated string `str` in place. */
void k_strreverse(char *str);

/** Upper-cases the letters of the null-terminated string `str` in place. */
void k_strtoupper(char *str);

/** Writes `n` bytes of `c`, taken as an unsigned char like memset's, to
    `ptr` and returns `ptr`. */
void *k_memset(void *ptr, int c, size_t n);

/**
 * Copies `n` bytes from `src` to `dest` and returns `dest`. The regions
 * must not overlap.
 */
void *k_memcpy(void *dest, const void *src, size_t n);

/** Copies `n` bytes from `src` to `dest`; the regions may overlap. */
void *k_memmove(void *dest, const void *src, size_t n);

/* The compilers emit a call to `strlen` on their own at -O2, as they emit
   the memory calls declared with the others; kstring.c defines it over
   `k_strlen`, with the size type the builtin prototype expects. */
__SIZE_TYPE__ strlen(const char *s);

/*
 * The standard names, defined in kstring.c because the compilers emit calls
 * to them (gcc for struct copies and large initializers, tcc for struct
 * assignment and initializer holes). Kernel code calls the k_ forms. The
 * size type is the compiler's own, which its builtin prototype check
 * requires.
 */
void *memset(void *ptr, int c, __SIZE_TYPE__ n);
void *memcpy(void *dest, const void *src, __SIZE_TYPE__ n);
void *memmove(void *dest, const void *src, __SIZE_TYPE__ n);

/**
 * Copies up to `n` characters from `src` to `dest` and returns `dest`.
 * If `src` is shorter than `n`, the remainder of `dest` is null-padded.
 */
char *k_strncpy(char *dest, const char *src, size_t n);

#endif
