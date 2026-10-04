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
 * physical memory. The region is sized at boot from the memory the frame
 * allocator manages (`kheap_size_for`): an eighth of it, 256 MiB at the
 * least, and at most what the architecture's layout gives the region,
 * KVA_HEAP_SIZE. On i686 that is 256 MiB, and the heap always has it; on
 * x86-64 it is the 512 GiB of the region's PML4 entry, and QEMU's machine
 * of 8 GiB has a heap of 1020 MiB. An object of at most 2048 bytes comes
 * from a page carved into blocks of one size class, 16 to 2048 bytes by
 * doubling, with a free list per class; a larger object takes whole pages.
 * A class page and the first page of a large object start with a 16-byte
 * header that says the class or the page count, which is how `kfree` finds
 * what it was given: the page base of the pointer. Pages from
 * `kalloc_pages` have no header. `kalloc_guarded` gives pages with a guard
 * below them, a page of the region that is marked taken and never mapped.
 *
 * The allocator never sleeps: it scans the frame table and the page bitmap
 * with interrupts masked, so it may be called from any context that may
 * allocate at all. A request the heap cannot meet answers NULL with a
 * record, and the caller answers its own caller with ENOMEM or the error
 * its contract names.
 *
 * The top 16 MiB of the region is a reserve of sixteen runs of 256 pages
 * that allocations under 64 pages never enter. A run of 64 pages or more
 * that the rest of the region cannot give, because a cache that takes and
 * returns pages one at a time has scattered its free pages, comes from
 * there, so that a raw batch or a request of the largest size is had
 * beside a full cache. The reserve costs no frame until a run is taken.
 */

/* The least region the heap is given, i686's whole region, and the grain
 * of its size, a whole number of leaf page tables' spans on i686 and on
 * x86-64. */
#define KHEAP_FLOOR 0x10000000u
#define KHEAP_GRAIN 0x400000u

/**
 * The bytes of the heap's region on a machine whose frame allocator
 * manages `managed` bytes: an eighth of them, rounded down to KHEAP_GRAIN,
 * never less than KHEAP_FLOOR and never more than `most`, the region the
 * architecture's layout has (KVA_HEAP_SIZE).
 */
size_t kheap_size_for(uint64_t managed, uint64_t most);

/**
 * Set up the heap in the first `bytes` of its region, a whole number of
 * KHEAP_GRAIN, making the region's page tables (`premap_kernel_window`).
 * The map of the region's pages is in the image for a region of
 * KHEAP_FLOOR, and for a larger one in the region's own first pages, a
 * frame for each 128 MiB of region, taken now. Called once page tables come
 * from the frame allocator (`mmu_heap_ready`).
 */
void kheap_init(size_t bytes);

/** The pages of the region `kheap_init` was given. */
uint32_t kheap_region_pages(void);

/** Give back what `kmalloc` returned; NULL is accepted and ignored. */
void kfree(void *p);

/* The allocator attribute is for the callers. Inside the allocator's own
   file it is left off: the analyser would take kzalloc's call of kmalloc
   for an allocation, follow kmalloc's body, and report the block it hands
   back as lost. */
#ifdef KHEAP_INSIDE
#define KHEAP_MALLOC
#else
#define KHEAP_MALLOC K_MALLOC(kfree)
#endif

/** `size` bytes, or NULL; the memory is not zeroed. */
void *kmalloc(size_t size) KHEAP_MALLOC;

/** `size` bytes zeroed, or NULL. */
void *kzalloc(size_t size) KHEAP_MALLOC;

/**
 * `count` pages, virtually contiguous and page-aligned, or NULL. `phys`,
 * when given, receives the physical address of the first page, for a
 * page table the processor reads by that address. Freed with `kfree_pages`.
 */
void *kalloc_pages(uint32_t count, uintptr_t *phys);

/**
 * As `kalloc_pages`, but a refusal is not logged: for a caller that goes
 * on with a smaller size or a buffer of its own, so that a size the heap
 * cannot give at the moment is not an error in the log.
 */
void *kalloc_pages_try(uint32_t count, uintptr_t *phys);

/**
 * One page for the buffer cache, as `kalloc_pages(1, 0)` gives it, or NULL
 * with a record. The cache's pages are taken from the top of the region
 * down and every other allocation from the bottom up, as Linux keeps
 * reclaimable pages apart from the rest: the cache, which holds up to half
 * the region a page at a time and gives its pages back in any order, then
 * leaves holes among its own pages and not among the runs the rest needs,
 * so a pipe's buffer or a socket's ring of 17 pages is still found after
 * the cache has filled. Freed with `kfree_pages`.
 */
void *kalloc_cache_page(void);

/** Give back what `kalloc_pages` or `kalloc_cache_page` returned. */
void kfree_pages(void *p, uint32_t count);

/**
 * `count` pages as `kalloc_pages` gives them, with the page below them
 * marked taken and never mapped: a guard, which no other allocation is
 * given, so an access that runs off the low end of the pages faults. A
 * kernel stack comes this way. The guard takes no frame. NULL, with a
 * record, when the region has no free run of `count + 1` pages or the
 * frames run out. Freed with `kfree_guarded`.
 */
void *kalloc_guarded(uint32_t count);

/**
 * Give back what `kalloc_guarded` returned, and its guard page with it.
 * Pages with no guard below them are refused with a record and kept, since
 * clearing the page below would free another allocation's page.
 */
void kfree_guarded(void *p, uint32_t count);

/** Pages the heap holds now, and bytes in use inside them. */
void kheap_stats(uint32_t *pages, size_t *bytes);

/** One size class of the heap, as /proc/slabinfo reports it. */
struct kheap_class {
    uint32_t size;     /* the bytes of a block */
    uint32_t per_page; /* the blocks a page holds after its header */
    uint32_t live;     /* the blocks in use */
    uint32_t pages;    /* the pages carved into blocks of the class */
};

/** Fill `out` with the counts of class `cls`, the smallest 0; 0, or -1 when
 * there is no such class. */
int kheap_class(uint32_t cls, struct kheap_class *out);

#endif /* _KHEAP_H_ */
