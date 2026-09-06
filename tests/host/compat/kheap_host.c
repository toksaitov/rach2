/*
 * The kernel heap on the host: the three calls the kernel sources make,
 * over the C library's allocator, for a host test of a source that
 * allocates. Declared here rather than through stdlib.h, since the kernel's
 * headers define the types that header would define again.
 */

#include "mm/kheap.h"

void *malloc(unsigned long n);
void *calloc(unsigned long count, unsigned long size);
void free(void *p);

void *kmalloc(size_t size)
{
    return size ? malloc(size) : NULL;
}

void *kzalloc(size_t size)
{
    return size ? calloc(1, size) : NULL;
}

void kfree(void *p)
{
    free(p);
}

void *kalloc_pages(uint32_t count, uintptr_t *phys)
{
    if (phys) {
        *phys = 0;
    }
    return count ? malloc(4096ul * count) : NULL;
}

void kfree_pages(void *p, uint32_t count)
{
    (void)count;
    free(p);
}
