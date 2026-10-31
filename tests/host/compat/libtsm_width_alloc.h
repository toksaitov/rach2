/* libtsm allocation faults, defined after the host's allocator declarations. */
#ifndef LIBTSM_WIDTH_ALLOC_H
#define LIBTSM_WIDTH_ALLOC_H

#include <stdlib.h>

/** Allocate size bytes, or return NULL at the selected fault. */
void *width_malloc(size_t size);
/** Allocate count zeroed elements, or return NULL at the selected fault. */
void *width_calloc(size_t count, size_t size);
/** Resize ptr, leaving it allocated when the selected fault returns NULL. */
void *width_realloc(void *ptr, size_t size);

#define malloc  width_malloc
#define calloc  width_calloc
#define realloc width_realloc

#endif
