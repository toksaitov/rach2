/*
 * The kernel heap on the host, over the C library's allocator, for a host
 * test of a source that allocates. The allocator is declared here rather than
 * through stdlib.h, since the kernel's headers define the types that header
 * would define again.
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

/** Whether `n` bytes exceed `kheap_host_largest`, when it is set. */
static int too_large(unsigned long n)
{
    return kheap_host_largest != 0 && n > kheap_host_largest;
}

/** `malloc`, or NULL for 0 bytes or a size over the limit. */
void *kmalloc(size_t size)
{
    return size && !too_large(size) ? malloc(size) : NULL;
}

/** `calloc`, or NULL for 0 bytes or a size over the limit. */
void *kzalloc(size_t size)
{
    return size && !too_large(size) ? calloc(1, size) : NULL;
}

/** `free`. */
void kfree(void *p)
{
    free(p);
}

/** `count` pages from `malloc`, aligned only as `malloc` aligns, with a
 * physical address of 0 stored through `phys` when it is not NULL; NULL for
 * 0 pages or a size over the limit. */
void *kalloc_pages(uint32_t count, uintptr_t *phys)
{
    if (phys) {
        *phys = 0;
    }
    return count && !too_large(4096ul * count) ? malloc(4096ul * count) : NULL;
}

/** `kalloc_pages`, which logs nothing on the host either. */
void *kalloc_pages_try(uint32_t count, uintptr_t *phys)
{
    return kalloc_pages(count, phys);
}

/** One page from `kalloc_pages`: the host keeps no separate cache region. */
void *kalloc_cache_page(void)
{
    return kalloc_pages(1, 0);
}

/** `free`; the count is not needed. */
void kfree_pages(void *p, uint32_t count)
{
    (void)count;
    free(p);
}

/* The pages of the heap's region: KHEAP_FLOOR's, unless a test sets another
 * to stand for a heap the boot sized from the memory. */
uint32_t kheap_host_region_pages = KHEAP_FLOOR / 4096u;

/** The pages of the heap's region: `kheap_host_region_pages`. */
uint32_t kheap_region_pages(void)
{
    return kheap_host_region_pages;
}
