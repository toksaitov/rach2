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

/** Writes `n` bytes of `c` to `ptr` and returns `ptr`. */
void* k_memset(void *ptr, char c, size_t n);

/**
 * Copies `n` bytes from `src` to `dest` and returns `dest`. The regions
 * must not overlap.
 */
void* k_memcpy(void *dest, const void *src, size_t n);

/**
 * Copies up to `n` characters from `src` to `dest` and returns `dest`.
 * If `src` is shorter than `n`, the remainder of `dest` is null-padded.
 */
char* k_strncpy(char *dest, const char *src, size_t n);

#endif
