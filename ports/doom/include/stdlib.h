/*
 * stdlib.h - Minimal standard library for Doom on Rach2
 * Functions implemented in crt0.c
 */
#ifndef _STDLIB_H
#define _STDLIB_H

#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

void *malloc(size_t size);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void free(void *ptr);

void _exit(int status);
void exit(int status);  /* Implemented in crt0.c with debug output */

int atoi(const char *s);
int abs(int n);

/* Not implemented but needed by Doom */
static inline long strtol(const char *s, char **endptr, int base) {
    (void)endptr; (void)base;
    return atoi(s);
}

static inline char *getenv(const char *name) {
    (void)name;
    return NULL;
}

static inline void qsort(void *base, size_t nmemb, size_t size,
                         int (*compar)(const void *, const void *)) {
    /* Simple bubble sort for Doom's needs */
    char *arr = base;
    size_t i, j;
    char *temp = malloc(size);
    if (!temp) return;
    for (i = 0; i < nmemb - 1; i++) {
        for (j = 0; j < nmemb - i - 1; j++) {
            if (compar(arr + j * size, arr + (j + 1) * size) > 0) {
                /* Swap */
                for (size_t k = 0; k < size; k++) {
                    temp[k] = arr[j * size + k];
                    arr[j * size + k] = arr[(j + 1) * size + k];
                    arr[(j + 1) * size + k] = temp[k];
                }
            }
        }
    }
}

#endif /* _STDLIB_H */
