/*
 * clib.h: the C library ACPICA calls, under the standard names, for
 * acrach2.h: four from the kernel's sys/lib, eleven from clib.c, and three that
 * ACPICA's own utprint.c defines; and clib.c's 64-bit division for i686.
 */
#ifndef _ACPI_CLIB_H_
#define _ACPI_CLIB_H_

#include <stdarg.h>

/* The kernel's kstring.c, as kstring.h declares them. */
__SIZE_TYPE__ strlen(const char *s);
void *memset(void *ptr, int c, __SIZE_TYPE__ n);
void *memcpy(void *dest, const void *src, __SIZE_TYPE__ n);
void *memmove(void *dest, const void *src, __SIZE_TYPE__ n);

/* clib.c's, each as the C standard has it, in the C locale. */
int memcmp(const void *a, const void *b, __SIZE_TYPE__ n);
char *strcat(char *dest, const char *src);
int strcmp(const char *a, const char *b);
char *strcpy(char *dest, const char *src);
int strncmp(const char *a, const char *b, __SIZE_TYPE__ n);
int tolower(int c);
int toupper(int c);
int isdigit(int c);
int isprint(int c);
int isspace(int c);
int isxdigit(int c);

/* utprint.c's, with ACPI_SIZE: UINT64 on x86-64 and UINT32 on i686, which
 * is not `__SIZE_TYPE__` under a compiler whose i686 size_t is a long. */
#if defined(__x86_64__)
#define CLIB_ACPI_SIZE unsigned long
#else
#define CLIB_ACPI_SIZE unsigned int
#endif
int vsnprintf(char *s, CLIB_ACPI_SIZE n, const char *format, va_list args);
int snprintf(char *s, CLIB_ACPI_SIZE n, const char *format, ...);
int sprintf(char *s, const char *format, ...);

#if !defined(__x86_64__)
/* n_hi:n_lo divided by d, n_hi below d, for acrach2.h's ACPI_DIV_64_BY_32. */
void acpi_div_64_by_32(unsigned int n_hi, unsigned int n_lo, unsigned int d, unsigned int *q,
                       unsigned int *r);
#endif

#endif /* _ACPI_CLIB_H_ */
