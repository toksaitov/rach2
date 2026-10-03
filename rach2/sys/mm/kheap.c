/*
 * kheap.c: the kernel heap, as kheap.h describes it.
 */

#define KHEAP_INSIDE
#include "../lib/kirq.h"
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
static uint32_t heap_pages;    /* the region's pages, from kheap_init */
static uint32_t reserve_first; /* the reserve's first page */
static uint32_t run_bits;      /* set where a reserve run is taken */
static void *free_list[KHEAP_CLASSES];
/* The map of the region's pages, set where a page is mapped: `floor_bits`
 * in the image for a region of KHEAP_FLOOR, the region's own first pages
 * for a larger one. */
static uint32_t floor_bits[KHEAP_FLOOR / PAGE_SIZE / 32];
static uint32_t *page_bits;
static uint32_t next_page;
static uint32_t pages_held;
static size_t bytes_used;
static uint32_t class_pages[KHEAP_CLASSES]; /* pages carved into each class */
static uint32_t class_live[KHEAP_CLASSES];  /* blocks of each class in use */

static int page_taken(uint32_t page)
{
    return (page_bits[page / 32] >> (page % 32)) & 1u;
}

static void page_set(uint32_t page, int taken)
{
    if (taken) {
        page_bits[page / 32] |= 1u << (page % 32);
    } else {
        page_bits[page / 32] &= ~(1u << (page % 32));
    }
}

/* The end of the region, one past its last byte. */
static uintptr_t heap_end(void)
{
    return KVA_HEAP + (uintptr_t)heap_pages * PAGE_SIZE;
}

/* Interrupts off. `count` free pages in a row, next fit from where the
 * last run ended and round once through the region, a word of the map at a
 * time where all of its pages are taken; `heap_pages` when the region has
 * no such run. */
static uint32_t find_run(uint32_t count)
{
    uint32_t run = 0, i, n;

    i = next_page;
    for (n = 0; n < 2 * heap_pages; n++, i++) {
        if (i >= heap_pages) {
            i = 0;
            run = 0;
        }
        if (i % 32u == 0 && page_bits[i / 32] == 0xFFFFFFFFu) {
            run = 0;
            n += 31;
            i += 31;
        } else if (page_taken(i)) {
            run = 0;
        } else if (++run == count) {
            return i + 1 - count;
        }
    }
    return heap_pages;
}

/* Interrupts off. Map `count` fresh frames at the run starting at `page`;
 * on a frame shortage what was mapped is unmapped again. */
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
    next_page = page + count;
    return 0;
}

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
    next_page = map_pages;
    pages_held = map_pages;
    bytes_used = 0;
    /* The reserve's pages read as taken to the ordinary search from the
     * start; the run bits say which runs are out. */
    for (i = reserve_first; i < heap_pages; i++) {
        page_set(i, 1);
    }
    run_bits = 0;
}

uint32_t kheap_region_pages(void)
{
    return heap_pages;
}

/* Interrupts off. The first page of a free reserve run, marked taken, or
 * `heap_pages` when every run is out or `count` is more than a run. */
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

/* Interrupts off. A reserve run whose pages `map_run` or `unmap_run` left
 * clear is closed to the ordinary search again and, when `release` says
 * so, given back to the reserve. */
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

/* Interrupts off. Unmap a run and, when it lies in the reserve, give the
 * run back: the one way pages leave the heap, whoever allocated them. */
static void release_run(uint32_t page, uint32_t count)
{
    unmap_run(page, count);
    if (page >= reserve_first) {
        reserve_close(page, count, 1);
    }
}

/* Interrupts off. `count` pages from the ordinary region or, for a run of
 * KHEAP_RUN_LEAST or more, from the reserve; `heap_pages` when neither has
 * them or the frames ran out. */
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

/* `count` pages as kalloc_pages gives them; `quiet` leaves a refusal out of
 * the log, for a caller that has a smaller size or a static buffer to go
 * on with. */
static void *pages_or_null(uint32_t count, uintptr_t *phys, int quiet)
{
    uint32_t flags = kirq_save();
    uint32_t page;
    void *va;

    if (count == 0 || count > heap_pages) {
        kirq_restore(flags);
        return NULL;
    }
    page = take_pages(count);
    if (page >= heap_pages) {
        kirq_restore(flags);
        if (!quiet) {
            klog(KLOG_ERR, "kheap", "%d pages: no room or no frame", (int)count);
        }
        return NULL;
    }
    va = (void *)(KVA_HEAP + (uintptr_t)page * PAGE_SIZE);
    if (phys) {
        *phys = paging_virt_to_phys((uintptr_t)va);
    }
    kirq_restore(flags);
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

void kfree_pages(void *p, uint32_t count)
{
    uintptr_t va = (uintptr_t)p;
    uint32_t flags;

    if (!p || va < KVA_HEAP || va >= heap_end() || (va & (PAGE_SIZE - 1))) {
        klog(KLOG_ERR, "kheap", "free of pages the heap did not give: %p", p);
        return;
    }
    flags = kirq_save();
    release_run((uint32_t)((va - KVA_HEAP) / PAGE_SIZE), count);
    kirq_restore(flags);
}

/* The guard is the first page of a run of `count + 1` and is marked taken
 * only once the pages above it are mapped, so a shortage of frames leaves
 * nothing behind. A guarded run is smaller than KHEAP_RUN_LEAST and never
 * comes from the reserve. */
void *kalloc_guarded(uint32_t count)
{
    uint32_t flags = kirq_save();
    uint32_t guard = heap_pages;

    if (count > 0 && count < KHEAP_RUN_LEAST) {
        guard = find_run(count + 1);
    }
    if (guard >= heap_pages || map_run(guard + 1, count) != 0) {
        kirq_restore(flags);
        klog(KLOG_ERR, "kheap", "%d pages and a guard: no room or no frame", (int)count);
        return NULL;
    }
    page_set(guard, 1);
    kirq_restore(flags);
    return (void *)(KVA_HEAP + ((uintptr_t)guard + 1) * PAGE_SIZE);
}

/* A guard is a page below the reserve that is taken and has no mapping.
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
    flags = kirq_save();
    if (count == 0 || count >= reserve_first || page > reserve_first - count ||
        !page_taken(page - 1) || paging_virt_to_phys(va - PAGE_SIZE) != 0) {
        kirq_restore(flags);
        klog(KLOG_ERR, "kheap", "free of pages with no guard below them: %p", p);
        return;
    }
    release_run(page, count);
    page_set(page - 1, 0);
    kirq_restore(flags);
}

/* Interrupts off. A page carved into blocks of class `cls`, its blocks on
 * the class's free list; -1 when no page could be had. */
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

/* Interrupts off. Take every block of the page at `base` off its class's
 * free list, so the page can go back. */
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

void *kmalloc(size_t size)
{
    uint32_t flags, cls;
    void *block;

    /* Nothing, more than the region could hold, or more than a large
     * object's header counts in its 32 bits. */
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
        flags = kirq_save();
        bytes_used += hdr->used;
        kirq_restore(flags);
        return (uint8_t *)base + KHEAP_HDR;
    }
    for (cls = 0; class_size[cls] < size; cls++) {
    }
    flags = kirq_save();
    if (!free_list[cls] && add_class_page(cls) != 0) {
        kirq_restore(flags);
        klog(KLOG_ERR, "kheap", "%d bytes: no room or no frame", (int)size);
        return NULL;
    }
    block = free_list[cls];
    free_list[cls] = *(void **)block;
    ((struct page_hdr *)((uintptr_t)block & ~(PAGE_SIZE - 1)))->used++;
    bytes_used += class_size[cls];
    class_live[cls]++;
    kirq_restore(flags);
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
    flags = kirq_save();
    if (hdr->magic != KHEAP_MAGIC) {
        kirq_restore(flags);
        klog(KLOG_ERR, "kheap", "free of a page without the header: %p", p);
        return;
    }
    if (hdr->cls == KHEAP_LARGE) {
        uint32_t count = hdr->pages;

        bytes_used -= hdr->used;
        hdr->magic = 0;
        release_run((uint32_t)(((uintptr_t)hdr - KVA_HEAP) / PAGE_SIZE), count);
        kirq_restore(flags);
        return;
    }
    if (hdr->cls >= KHEAP_CLASSES || hdr->used == 0) {
        kirq_restore(flags);
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
    kirq_restore(flags);
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
    flags = kirq_save();
    out->live = class_live[cls];
    out->pages = class_pages[cls];
    kirq_restore(flags);
    return 0;
}
