/*
 * kheap.c: the kernel heap, as kheap.h describes it.
 */

#define KHEAP_INSIDE
#include "../lib/spinlock.h"
#include "../arch/arch.h"
#include "kheap.h"

#include "../klog.h"
#include "../panic.h"
#include "../lib/kstring.h"
#include "paging.h"

#define KHEAP_MAGIC   0x4B484550u /* "KHEP" */
#define KHEAP_CLASSES 8
#define KHEAP_LARGE   0xFFu
#define KHEAP_HDR     16u

/* The header at the base of every page a class's blocks are carved from and
 * of the first page of a large object; pages from `kalloc_pages` have none. */
struct page_hdr {
    uint32_t magic;
    uint32_t cls;   /* the class index, or KHEAP_LARGE */
    uint32_t used;  /* blocks in use for a class page; the bytes asked for a large one */
    uint32_t pages; /* the run's length, for a large object */
};

static const uint32_t class_size[KHEAP_CLASSES] = {16, 32, 64, 128, 256, 512, 1024, 2048};

/* The reserve of runs for large allocations, at the top of the region:
 * KHEAP_RUNS runs of KHEAP_RUN_PAGES pages that the ordinary search never
 * enters, so that a region whose free pages the cache has scattered can
 * still give a run of a megabyte, the size of a raw batch and of a request
 * of the largest size. The reserve costs no frame until a run is taken,
 * since a page is mapped only when it is allocated; a run is taken only for
 * an allocation of KHEAP_RUN_LEAST pages or more that the rest of the
 * region cannot give, so a small allocation never uses a run, and it is
 * given back when its pages are freed, by `kfree_pages` or by `kfree` of a
 * large object, both of which go through `release_run`. */
#define KHEAP_RUN_PAGES 256u
#define KHEAP_RUNS      16u
#define KHEAP_RUN_LEAST 64u
/* The heap's lock: the region's pages, its runs, the free lists and the
 * counts, against an allocation or a free on any processor and in any
 * interrupt. It is taken with interrupts off, after the big lock and
 * before the frame allocator's, which taking a page's frame takes. */
static struct spinlock heap_lock = SPINLOCK_INIT;
static uint32_t heap_pages;    /* the region's pages, from kheap_init */
static uint32_t reserve_first; /* the reserve's first page */
static uint32_t run_bits;      /* set where a reserve run is taken */
static void *free_list[KHEAP_CLASSES];
/* The map of the region's pages, set where a page is mapped: `floor_bits`
 * in the image for a region of KHEAP_FLOOR, the region's own first pages
 * for a larger one. */
static uint32_t floor_bits[KHEAP_FLOOR / PAGE_SIZE / 32];
static uint32_t *page_bits;
/* The two ends of the ordinary region's search: every page below `low_free`
 * is taken, and so is every page above `high_free` up to the reserve. The
 * buffer cache's pages are taken from the top down and every other
 * allocation from the bottom up (`kalloc_cache_page`). */
static uint32_t low_free;
static uint32_t high_free;
static uint32_t pages_held;
static size_t bytes_used;
static uint32_t class_pages[KHEAP_CLASSES]; /* pages carved into each class */
static uint32_t class_live[KHEAP_CLASSES];  /* blocks of each class in use */

/** Whether page `page` of the region is taken in the map. */
static int page_taken(uint32_t page)
{
    return (page_bits[page / 32] >> (page % 32)) & 1u;
}

/** Mark page `page` of the region taken or free in the map. */
static void page_set(uint32_t page, int taken)
{
    if (taken) {
        page_bits[page / 32] |= 1u << (page % 32);
    } else {
        page_bits[page / 32] &= ~(1u << (page % 32));
    }
}

/** The end of the region, one past its last byte. */
static uintptr_t heap_end(void)
{
    return KVA_HEAP + (uintptr_t)heap_pages * PAGE_SIZE;
}

/** `count` free pages in a row, the lowest such run, a word of the map at
 * a time where all of its pages are taken; `heap_pages` when the region has
 * no such run. The first free page the search passes becomes `low_free`.
 * `heap_lock` is held. */
static uint32_t find_run(uint32_t count)
{
    uint32_t run = 0, i, first = heap_pages;

    for (i = low_free; i < heap_pages; i++) {
        if (i % 32u == 0 && page_bits[i / 32] == 0xFFFFFFFFu) {
            run = 0;
            i += 31;
        } else if (page_taken(i)) {
            run = 0;
        } else {
            if (first == heap_pages) {
                first = i;
                low_free = i;
            }
            if (++run == count) {
                return i + 1 - count;
            }
        }
    }
    return heap_pages;
}

/** The highest free page below the reserve, which becomes `high_free`, or
 * `heap_pages` when there is none. `heap_lock` is held. */
static uint32_t find_page_high(void)
{
    uint32_t i = high_free + 1u;

    while (i-- > 0) {
        if (i % 32u == 31u && page_bits[i / 32] == 0xFFFFFFFFu) {
            i -= 31;
        } else if (!page_taken(i)) {
            high_free = i;
            return i;
        }
    }
    return heap_pages;
}

/** Map `count` fresh frames at the run starting at `page` and mark them
 * taken: 0, or -1 when the frames ran out, with what was mapped unmapped
 * again. `heap_lock` is held. */
static int map_run(uint32_t page, uint32_t count)
{
    uint32_t i;

    for (i = 0; i < count; i++) {
        uintptr_t phys = alloc_frame();

        if (phys == 0) {
            while (i-- > 0) {
                uintptr_t va = KVA_HEAP + ((uintptr_t)page + i) * PAGE_SIZE;

                free_frame(paging_virt_to_phys(va));
                unmap_kernel_frame(va);
                page_set(page + i, 0);
            }
            return -1;
        }
        map_kernel_frame(KVA_HEAP + ((uintptr_t)page + i) * PAGE_SIZE, phys, 1);
        page_set(page + i, 1);
    }
    pages_held += count;
    return 0;
}

/** Unmap the run of `count` pages at `page`, give back its frames and mark
 * it free. `heap_lock` is held. */
static void unmap_run(uint32_t page, uint32_t count)
{
    uint32_t i;

    for (i = 0; i < count; i++) {
        uintptr_t va = KVA_HEAP + ((uintptr_t)page + i) * PAGE_SIZE;

        free_frame(paging_virt_to_phys(va));
        unmap_kernel_frame(va);
        page_set(page + i, 0);
    }
    pages_held -= count;
}

size_t kheap_size_for(uint64_t managed, uint64_t most)
{
    uint64_t bytes = managed / 8u / KHEAP_GRAIN * KHEAP_GRAIN;

    if (bytes < KHEAP_FLOOR) {
        bytes = KHEAP_FLOOR;
    }
    if (bytes > most) {
        bytes = most / KHEAP_GRAIN * KHEAP_GRAIN;
    }
    return (size_t)bytes;
}

/** The reserve's pages are marked taken from the start, so that the
 * ordinary search passes them; `run_bits` says which runs are out. */
void kheap_init(size_t bytes)
{
    uint32_t i, map_pages = 0;

    heap_pages = (uint32_t)(bytes / PAGE_SIZE);
    reserve_first = heap_pages - KHEAP_RUNS * KHEAP_RUN_PAGES;
    if (premap_kernel_window(KVA_HEAP, bytes) != 0) {
        k_panic("kheap: no memory for the region's page tables");
    }
    page_bits = floor_bits;
    if (heap_pages > KHEAP_FLOOR / PAGE_SIZE) {
        map_pages = (heap_pages / 8u + PAGE_SIZE - 1) / PAGE_SIZE;
        page_bits = (uint32_t *)KVA_HEAP;
        for (i = 0; i < map_pages; i++) {
            uintptr_t phys = alloc_frame();

            if (phys == 0) {
                k_panic("kheap: no frame for the map of the region's pages");
            }
            map_kernel_frame(KVA_HEAP + (uintptr_t)i * PAGE_SIZE, phys, 1);
        }
    }
    k_memset(page_bits, 0, heap_pages / 8u);
    k_memset(free_list, 0, sizeof(free_list));
    for (i = 0; i < map_pages; i++) {
        page_set(i, 1);
    }
    low_free = map_pages;
    high_free = reserve_first - 1u;
    pages_held = map_pages;
    bytes_used = 0;
    for (i = reserve_first; i < heap_pages; i++) {
        page_set(i, 1);
    }
    run_bits = 0;
}

uint32_t kheap_region_pages(void)
{
    return heap_pages;
}

/** The first page of a free reserve run, marked out, or `heap_pages` when
 * every run is out or `count` is more than a run. `heap_lock` is held. */
static uint32_t reserve_take(uint32_t count)
{
    uint32_t r;

    if (count > KHEAP_RUN_PAGES) {
        return heap_pages;
    }
    for (r = 0; r < KHEAP_RUNS; r++) {
        if (!(run_bits & (1u << r))) {
            run_bits |= 1u << r;
            return reserve_first + r * KHEAP_RUN_PAGES;
        }
    }
    return heap_pages;
}

/** A reserve run whose pages `map_run` or `unmap_run` left clear is closed
 * to the ordinary search again and, when `release` says so, given back to
 * the reserve. `heap_lock` is held. */
static void reserve_close(uint32_t page, uint32_t count, int release)
{
    uint32_t i;

    for (i = 0; i < count; i++) {
        page_set(page + i, 1);
    }
    if (release) {
        run_bits &= ~(1u << ((page - reserve_first) / KHEAP_RUN_PAGES));
    }
}

/** Pages `first` through `last`, below the reserve, are free again: the
 * searches from both ends start at them if they lie beyond where each would
 * start. `heap_lock` is held. */
static void ends_widen(uint32_t first, uint32_t last)
{
    if (first < low_free) {
        low_free = first;
    }
    if (last > high_free) {
        high_free = last;
    }
}

/** Unmap a run and, when it lies in the reserve, give the run back: the
 * one way pages leave the heap, whoever allocated them. `heap_lock` is
 * held. */
static void release_run(uint32_t page, uint32_t count)
{
    unmap_run(page, count);
    if (page < reserve_first) {
        ends_widen(page, page + count - 1u);
    }
    if (page >= reserve_first) {
        reserve_close(page, count, 1);
    }
}

/** `count` pages, mapped, from the ordinary region or, for a run of
 * KHEAP_RUN_LEAST or more that it cannot give, from the reserve; `heap_pages`
 * when neither has them or the frames ran out. `heap_lock` is held. */
static uint32_t take_pages(uint32_t count)
{
    uint32_t page = find_run(count);

    if (page >= heap_pages && count >= KHEAP_RUN_LEAST) {
        page = reserve_take(count);
        if (page < heap_pages && map_run(page, count) != 0) {
            reserve_close(page, count, 1);
            page = heap_pages;
        }
    } else if (page < heap_pages && map_run(page, count) != 0) {
        page = heap_pages;
    }
    return page;
}

/** `count` pages as kalloc_pages gives them; `quiet` leaves a refusal out
 * of the log, for a caller that has a smaller size or a static buffer to go
 * on with. */
static void *pages_or_null(uint32_t count, uintptr_t *phys, int quiet)
{
    uint32_t flags = spin_lock_irqsave(&heap_lock);
    uint32_t page;
    void *va;

    if (count == 0 || count > heap_pages) {
        spin_unlock_irqrestore(&heap_lock, flags);
        return NULL;
    }
    page = take_pages(count);
    if (page >= heap_pages) {
        spin_unlock_irqrestore(&heap_lock, flags);
        if (!quiet) {
            klog(KLOG_ERR, "kheap", "%d pages: no room or no frame", (int)count);
        }
        return NULL;
    }
    va = (void *)(KVA_HEAP + (uintptr_t)page * PAGE_SIZE);
    if (phys) {
        *phys = paging_virt_to_phys((uintptr_t)va);
    }
    spin_unlock_irqrestore(&heap_lock, flags);
    return va;
}

void *kalloc_pages(uint32_t count, uintptr_t *phys)
{
    return pages_or_null(count, phys, 0);
}

void *kalloc_pages_try(uint32_t count, uintptr_t *phys)
{
    return pages_or_null(count, phys, 1);
}

void *kalloc_cache_page(void)
{
    uint32_t flags = spin_lock_irqsave(&heap_lock);
    uint32_t page = find_page_high();

    if (page >= heap_pages || map_run(page, 1) != 0) {
        spin_unlock_irqrestore(&heap_lock, flags);
        klog(KLOG_ERR, "kheap", "a cache page: no room or no frame");
        return NULL;
    }
    spin_unlock_irqrestore(&heap_lock, flags);
    return (void *)(KVA_HEAP + (uintptr_t)page * PAGE_SIZE);
}

void kfree_pages(void *p, uint32_t count)
{
    uintptr_t va = (uintptr_t)p;
    uint32_t flags;

    if (!p || va < KVA_HEAP || va >= heap_end() || (va & (PAGE_SIZE - 1))) {
        klog(KLOG_ERR, "kheap", "free of pages the heap did not give: %p", p);
        return;
    }
    flags = spin_lock_irqsave(&heap_lock);
    release_run((uint32_t)((va - KVA_HEAP) / PAGE_SIZE), count);
    spin_unlock_irqrestore(&heap_lock, flags);
}

/** The guard is the first page of a run of `count + 1` and is marked taken
 * only once the pages above it are mapped, so a shortage of frames leaves
 * nothing behind. */
void *kalloc_guarded(uint32_t count)
{
    uint32_t flags = spin_lock_irqsave(&heap_lock);
    uint32_t guard = heap_pages;

    if (count > 0 && count < KHEAP_RUN_LEAST) {
        guard = find_run(count + 1);
    }
    if (guard >= heap_pages || map_run(guard + 1, count) != 0) {
        spin_unlock_irqrestore(&heap_lock, flags);
        klog(KLOG_ERR, "kheap", "%d pages and a guard: no room or no frame", (int)count);
        return NULL;
    }
    page_set(guard, 1);
    spin_unlock_irqrestore(&heap_lock, flags);
    return (void *)(KVA_HEAP + ((uintptr_t)guard + 1) * PAGE_SIZE);
}

/** A guard is a page below the reserve that is taken and has no mapping.
 * Below the first page of any other run is a mapped page, a free page or
 * the region's start: every taken page outside the reserve is mapped
 * except a guard, and the page above a guard begins the guard's own run. */
void kfree_guarded(void *p, uint32_t count)
{
    uintptr_t va = (uintptr_t)p;
    uint32_t flags, page;

    if (!p || va <= KVA_HEAP || va >= heap_end() || (va & (PAGE_SIZE - 1))) {
        klog(KLOG_ERR, "kheap", "free of pages the heap did not give: %p", p);
        return;
    }
    page = (uint32_t)((va - KVA_HEAP) / PAGE_SIZE);
    flags = spin_lock_irqsave(&heap_lock);
    if (count == 0 || count >= reserve_first || page > reserve_first - count ||
        !page_taken(page - 1) || paging_virt_to_phys(va - PAGE_SIZE) != 0) {
        spin_unlock_irqrestore(&heap_lock, flags);
        klog(KLOG_ERR, "kheap", "free of pages with no guard below them: %p", p);
        return;
    }
    release_run(page, count);
    page_set(page - 1, 0);
    ends_widen(page - 1, page - 1);
    spin_unlock_irqrestore(&heap_lock, flags);
}

/** A page carved into blocks of class `cls`, its blocks on the class's
 * free list: 0, or -1 when no page could be had. `heap_lock` is held. */
static int add_class_page(uint32_t cls)
{
    uint32_t page = find_run(1);
    struct page_hdr *hdr;
    uintptr_t base;
    uint32_t size = class_size[cls];
    uint32_t off;

    if (page >= heap_pages || map_run(page, 1) != 0) {
        return -1;
    }
    base = KVA_HEAP + (uintptr_t)page * PAGE_SIZE;
    hdr = (struct page_hdr *)base;
    hdr->magic = KHEAP_MAGIC;
    hdr->cls = cls;
    hdr->used = 0;
    hdr->pages = 1;
    for (off = KHEAP_HDR; off + size <= PAGE_SIZE; off += size) {
        *(void **)(base + off) = free_list[cls];
        free_list[cls] = (void *)(base + off);
    }
    class_pages[cls]++;
    return 0;
}

/** Take every block of the page at `base` off its class's free list, so
 * the page can go back. `heap_lock` is held. */
static void drop_class_page(void *base, uint32_t cls)
{
    void **link = &free_list[cls];
    char *lo = base;

    while (*link) {
        void *block = *link;
        char *at = block;

        if (at >= lo && at < lo + PAGE_SIZE) {
            *link = *(void **)block;
        } else {
            link = block;
        }
    }
}

/** A size above the largest class takes whole pages with the header in
 * the first, and the header's `used` has 32 bits for the size. */
void *kmalloc(size_t size)
{
    uint32_t flags, cls;
    void *block;

    if (size == 0 || size > (size_t)heap_pages * PAGE_SIZE - PAGE_SIZE || size > 0xFFFFFFF0u) {
        return NULL;
    }
    if (size > class_size[KHEAP_CLASSES - 1]) {
        uint32_t count = (uint32_t)((size + KHEAP_HDR + PAGE_SIZE - 1) / PAGE_SIZE);
        void *base = kalloc_pages(count, NULL);
        struct page_hdr *hdr;

        if (!base) {
            return NULL;
        }
        hdr = base;
        hdr->magic = KHEAP_MAGIC;
        hdr->cls = KHEAP_LARGE;
        hdr->used = (uint32_t)size;
        hdr->pages = count;
        flags = spin_lock_irqsave(&heap_lock);
        bytes_used += hdr->used;
        spin_unlock_irqrestore(&heap_lock, flags);
        return (uint8_t *)base + KHEAP_HDR;
    }
    for (cls = 0; class_size[cls] < size; cls++) {
    }
    flags = spin_lock_irqsave(&heap_lock);
    if (!free_list[cls] && add_class_page(cls) != 0) {
        spin_unlock_irqrestore(&heap_lock, flags);
        klog(KLOG_ERR, "kheap", "%d bytes: no room or no frame", (int)size);
        return NULL;
    }
    block = free_list[cls];
    free_list[cls] = *(void **)block;
    ((struct page_hdr *)((uintptr_t)block & ~(PAGE_SIZE - 1)))->used++;
    bytes_used += class_size[cls];
    class_live[cls]++;
    spin_unlock_irqrestore(&heap_lock, flags);
    return block;
}

void *kzalloc(size_t size)
{
    void *p = kmalloc(size);

    if (p) {
        k_memset(p, 0, size);
    }
    return p;
}

void kfree(void *p)
{
    uintptr_t va = (uintptr_t)p;
    struct page_hdr *hdr;
    uint32_t flags;

    if (!p) {
        return;
    }
    if (va < KVA_HEAP || va >= heap_end()) {
        klog(KLOG_ERR, "kheap", "free of memory the heap did not give: %p", p);
        return;
    }
    hdr = (struct page_hdr *)(va & ~(PAGE_SIZE - 1));
    flags = spin_lock_irqsave(&heap_lock);
    if (hdr->magic != KHEAP_MAGIC) {
        spin_unlock_irqrestore(&heap_lock, flags);
        klog(KLOG_ERR, "kheap", "free of a page without the header: %p", p);
        return;
    }
    if (hdr->cls == KHEAP_LARGE) {
        uint32_t count = hdr->pages;

        bytes_used -= hdr->used;
        hdr->magic = 0;
        release_run((uint32_t)(((uintptr_t)hdr - KVA_HEAP) / PAGE_SIZE), count);
        spin_unlock_irqrestore(&heap_lock, flags);
        return;
    }
    if (hdr->cls >= KHEAP_CLASSES || hdr->used == 0) {
        spin_unlock_irqrestore(&heap_lock, flags);
        klog(KLOG_ERR, "kheap", "free of a block not in use: %p", p);
        return;
    }
    *(void **)p = free_list[hdr->cls];
    free_list[hdr->cls] = p;
    hdr->used--;
    bytes_used -= class_size[hdr->cls];
    class_live[hdr->cls]--;
    if (hdr->used == 0) {
        drop_class_page(hdr, hdr->cls);
        class_pages[hdr->cls]--;
        hdr->magic = 0;
        release_run((uint32_t)(((uintptr_t)hdr - KVA_HEAP) / PAGE_SIZE), 1);
    }
    spin_unlock_irqrestore(&heap_lock, flags);
}

void kheap_stats(uint32_t *pages, size_t *bytes)
{
    if (pages) {
        *pages = pages_held;
    }
    if (bytes) {
        *bytes = bytes_used;
    }
}

int kheap_class(uint32_t cls, struct kheap_class *out)
{
    uint32_t flags;

    if (cls >= KHEAP_CLASSES) {
        return -1;
    }
    out->size = class_size[cls];
    out->per_page = (PAGE_SIZE - KHEAP_HDR) / class_size[cls];
    flags = spin_lock_irqsave(&heap_lock);
    out->live = class_live[cls];
    out->pages = class_pages[cls];
    spin_unlock_irqrestore(&heap_lock, flags);
    return 0;
}
