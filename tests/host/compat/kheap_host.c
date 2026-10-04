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

/* The largest allocation granted, in bytes, or 0 for no limit. A test that
 * sets it stands in for a kernel heap whose free pages lie in runs no longer
 * than that, which is how a heap looks after a boot has moved a gigabyte
 * through the cache: memory is free, but not in one long run. */
unsigned long kheap_host_largest;

static int too_large(unsigned long n)
{
    return kheap_host_largest != 0 && n > kheap_host_largest;
}

void *kmalloc(size_t size)
{
    return size && !too_large(size) ? malloc(size) : NULL;
}

void *kzalloc(size_t size)
{
    return size && !too_large(size) ? calloc(1, size) : NULL;
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
    return count && !too_large(4096ul * count) ? malloc(4096ul * count) : NULL;
}

void *kalloc_pages_try(uint32_t count, uintptr_t *phys)
{
    return kalloc_pages(count, phys);
}

void *kalloc_cache_page(void)
{
    return kalloc_pages(1, 0);
}

void kfree_pages(void *p, uint32_t count)
{
    (void)count;
    free(p);
}

/* The pages of the heap's region: i686's, the floor's, unless a test sets
 * another to stand for a heap the boot sized from the memory. */
uint32_t kheap_host_region_pages = KHEAP_FLOOR / 4096u;

uint32_t kheap_region_pages(void)
{
    return kheap_host_region_pages;
}
