#ifndef _KSTRING_H_
#define _KSTRING_H_

#include "kdef.h"
#include "kctypes.h"

/*
 * Computes the length of the null-terminated string.
 */
size_t k_strlen(const char *str);

/*
 * Reverses characters in the null-terminated string.
 */
void k_strreverse(char *str);

/*
 * Converts lower case letters to upper case.
 */
void k_strtoupper(char *str);

/*
 * Writes 'n' bytes of 'c' to the byte string specified by 'ptr'.
 */
void* k_memset(void *ptr, char c, size_t n);

/*
 * Copies 'n' bytes from memory area 'src' to memory area 'dest'.
 * The memory areas must not overlap.
 */
void* k_memcpy(void *dest, const void *src, size_t n);

/*
 * Copies up to 'n' characters from 'src' to 'dest'.
 * If 'src' is shorter than 'n', 'dest' is padded with null bytes.
 */
char* k_strncpy(char *dest, const char *src, size_t n);

#endif
