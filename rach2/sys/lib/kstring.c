#include "kstring.h"

size_t k_strlen(const char *str)
{
    const char *end; for (end = str; *end; ++end); return end - str;
}

void k_strreverse(char *str)
{
    size_t i, j;
    char c;

    for (i = 0, j = k_strlen(str) - 1; i < j; ++i, --j) {
        c = str[i]; str[i] = str[j]; str[j] = c;
    }
}

void k_strtoupper(char *str)
{
    while (*str) { *str = k_toupper(*str); ++str; }
}

void* k_memset(void *ptr, char c, size_t n)
{
    char *str = (char *) ptr; while (n--) { *str++ = c; }; return ptr;
}

void* k_memcpy(void *dest, const void *src, size_t n)
{
    char *d = (char *) dest;
    const char *s = (const char *) src;
    while (n--) { *d++ = *s++; }
    return dest;
}
