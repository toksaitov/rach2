/*
 * string.h: the string functions of Doom on Rach2. crt0.c defines the
 * functions declared here, a byte at a time, and the rest are inline.
 */
#ifndef _STRING_H
#define _STRING_H

#include <stddef.h>

void *memcpy(void *dest, const void *src, size_t n);
void *memmove(void *dest, const void *src, size_t n);
void *memset(void *s, int c, size_t n);
int memcmp(const void *s1, const void *s2, size_t n);

size_t strlen(const char *s);
char *strcpy(char *dest, const char *src);
char *strncpy(char *dest, const char *src, size_t n);
int strcmp(const char *s1, const char *s2);
int strncmp(const char *s1, const char *s2, size_t n);
char *strcat(char *dest, const char *src);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *haystack, const char *needle);

static inline char *strncat(char *dest, const char *src, size_t n) {
    char *d = dest;
    while (*d) d++;
    while (n-- && (*d++ = *src++));
    if (n == (size_t)-1) *d = '\0';
    return dest;
}

static inline int strcasecmp(const char *s1, const char *s2) {
    while (*s1 && *s2) {
        int c1 = *s1, c2 = *s2;
        if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
        if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
        if (c1 != c2) return c1 - c2;
        s1++; s2++;
    }
    return *(unsigned char *)s1 - *(unsigned char *)s2;
}

static inline int strncasecmp(const char *s1, const char *s2, size_t n) {
    while (n-- && *s1 && *s2) {
        int c1 = *s1, c2 = *s2;
        if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
        if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
        if (c1 != c2) return c1 - c2;
        s1++; s2++;
    }
    if (n == (size_t)-1) return 0;
    return *(unsigned char *)s1 - *(unsigned char *)s2;
}

static inline char *strdup(const char *s) {
    size_t len = strlen(s) + 1;
    extern void *malloc(size_t);
    char *dup = malloc(len);
    if (dup) memcpy(dup, s, len);
    return dup;
}

/** "Error", whatever `errnum` is. */
static inline char *strerror(int errnum) {
    (void)errnum;
    return "Error";
}

#endif /* _STRING_H */
