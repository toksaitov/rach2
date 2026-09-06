#ifndef _KHEAP_H_
#define _KHEAP_H_

#include "../lib/kdef.h"
#include "../lib/kint.h"

/*
 * kheap: the kernel heap, which allocates and frees objects of any size.
 *
 * The heap is the region `KVA_HEAP` of the kernel's layout, grown a page
 * at a time by taking a frame from anywhere the map offers and mapping it
 * there, so its pages are contiguous in virtual memory and scattered in
 * physical memory. An object of at most 2048 bytes comes from a page carved
 * into blocks of one size class, 16 to 2048 bytes by doubling, with a free
 * list per class; a larger object takes whole pages. Every page starts with
 * a 16-byte header naming its class or its page count, which is how `kfree`
 * finds what it was given: the page base of the pointer.
 *
 * The allocator never sleeps: it scans the frame table and the page bitmap
 * with interrupts masked, so it may be called from any context that may
 * allocate at all. A request the heap cannot meet answers NULL with a
 * record; the caller refuses by name.
 */

/** Set up the heap; the page tables of the region exist since init_paging. */
void kheap_init(void);

/** `size` bytes, or NULL; the memory is not zeroed. */
void *kmalloc(size_t size);

/** `size` bytes zeroed, or NULL. */
void *kzalloc(size_t size);

/** Give back what `kmalloc` returned; NULL is accepted and ignored. */
void kfree(void *p);

/**
 * `count` pages, virtually contiguous and page-aligned, or NULL. `phys`,
 * when given, receives the physical address of the first page, for a
 * page table the processor reads by that address. Freed with `kfree_pages`.
 */
void *kalloc_pages(uint32_t count, uintptr_t *phys);

/** Give back what `kalloc_pages` returned. */
void kfree_pages(void *p, uint32_t count);

/** Pages the heap holds now, and bytes in use inside them. */
void kheap_stats(uint32_t *pages, uint32_t *bytes);

#endif /* _KHEAP_H_ */
