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
