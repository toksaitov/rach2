/*
 * stdlib.h: the allocator, the conversions and the exits of Doom on Rach2.
 * crt0.c defines the functions declared here but exit, which is the
 * platform layer's, in doomgeneric_rach2.c; the rest are inline.
 */
#ifndef _STDLIB_H
#define _STDLIB_H

#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

/**
 * Take `size` bytes, rounded up to a multiple of 8, from the top of the
 * heap, which is the memory between the process's first break and its
 * current one. When the heap is short, the break moves up by the shortfall
 * rounded up to 64 KiB. Returns NULL when brk refuses. Memory is never
 * reused, since free does nothing.
 */
void *malloc(size_t size);
void *calloc(size_t nmemb, size_t size);

/**
 * Take a new block of `size` bytes and copy `size` bytes from `ptr` into
 * it, reading past the end of the old block when it was smaller; the old
 * block is not freed. A `size` of 0 returns NULL.
 */
void *realloc(void *ptr, size_t size);

/** Does nothing: memory is never returned to the heap. */
void free(void *ptr);

void _exit(int status);

/**
 * Shut the platform down with `DG_Shutdown`, which closes the mouse and the
 * framebuffer and prints the report, and end the process with `status`.
 * There are no atexit handlers.
 */
void exit(int status);

/** The decimal number after any spaces and tabs, with an optional sign; no
 * overflow is detected. */
int atoi(const char *s);
int abs(int n);

/** atoi of `s`: `base` is ignored and `endptr` is not set. */
static inline long strtol(const char *s, char **endptr, int base) {
    (void)endptr; (void)base;
    return atoi(s);
}

/** A stub: there is no environment, and every name answers NULL. */
static inline char *getenv(const char *name) {
    (void)name;
    return NULL;
}

/** A bubble sort, which takes time of the square of `nmemb`. Its swap space
 * comes from malloc on each call; when malloc fails nothing is sorted. */
static inline void qsort(void *base, size_t nmemb, size_t size,
                         int (*compar)(const void *, const void *)) {
    char *arr = base;
    size_t i, j;
    char *temp = malloc(size);
    if (!temp) return;
    for (i = 0; i < nmemb - 1; i++) {
        for (j = 0; j < nmemb - i - 1; j++) {
            if (compar(arr + j * size, arr + (j + 1) * size) > 0) {
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
