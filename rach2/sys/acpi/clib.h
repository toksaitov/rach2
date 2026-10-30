/*
 * clib.h: the C library ACPICA calls, under the standard names, for
 * acrach2.h: four from the kernel's sys/lib, eleven from clib.c, and three that
 * ACPICA's own utprint.c defines.
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

/* utprint.c's, with ACPI_SIZE, which is `__SIZE_TYPE__` (acrach2.h). */
int vsnprintf(char *s, __SIZE_TYPE__ n, const char *format, va_list args);
int snprintf(char *s, __SIZE_TYPE__ n, const char *format, ...);
int sprintf(char *s, const char *format, ...);

#endif /* _ACPI_CLIB_H_ */
