/*
 * kheap.c: the kernel heap, as kheap.h describes it.
 */

#include "kheap.h"

#include "../klog.h"
#include "../lib/kstring.h"
#include "paging.h"

#define KHEAP_PAGES   (KVA_HEAP_SIZE / PAGE_SIZE)
#define KHEAP_MAGIC   0x4B484550u /* "KHEP" */
#define KHEAP_CLASSES 8
#define KHEAP_LARGE   0xFFu
#define KHEAP_HDR     16u

/* The header at the base of every heap page. */
struct page_hdr {
    uint32_t magic;
    uint32_t cls;   /* the class index, or KHEAP_LARGE */
    uint32_t used;  /* blocks in use for a class page; the bytes asked for a large one */
    uint32_t pages; /* the run's length, for a large object */
};

static const uint32_t class_size[KHEAP_CLASSES] = {16, 32, 64, 128, 256, 512, 1024, 2048};
static void *free_list[KHEAP_CLASSES];
static uint32_t page_bits[KHEAP_PAGES / 32]; /* set where a page is mapped */
static uint32_t next_page;
static uint32_t pages_held;
static uint32_t bytes_used;

static uint32_t irq_save(void)
{
    uint32_t flags;
    __asm__ __volatile__("pushfl; popl %0; cli" : "=r"(flags)::"memory");
    return flags;
}

static void irq_restore(uint32_t flags)
{
    if (flags & 0x200) {
        __asm__ __volatile__("sti");
    }
}

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

/* Interrupts off. `count` free pages in a row, next fit from where the
 * last run ended and round once through the region; KHEAP_PAGES when the
 * region has no such run. */
static uint32_t find_run(uint32_t count)
{
    uint32_t run = 0, i, n;

    for (n = 0, i = next_page; n < 2 * KHEAP_PAGES; n++, i++) {
        if (i >= KHEAP_PAGES) {
            i = 0;
            run = 0;
        }
        if (page_taken(i)) {
            run = 0;
        } else if (++run == count) {
            return i + 1 - count;
        }
    }
    return KHEAP_PAGES;
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
                uintptr_t va = KVA_HEAP + (page + i) * PAGE_SIZE;

                free_frame(paging_virt_to_phys(va));
                unmap_kernel_frame(va);
                page_set(page + i, 0);
            }
            return -1;
        }
        map_kernel_frame(KVA_HEAP + (page + i) * PAGE_SIZE, phys, 1);
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
        uintptr_t va = KVA_HEAP + (page + i) * PAGE_SIZE;

        free_frame(paging_virt_to_phys(va));
        unmap_kernel_frame(va);
        page_set(page + i, 0);
    }
    pages_held -= count;
}

void kheap_init(void)
{
    k_memset(page_bits, 0, sizeof(page_bits));
    k_memset(free_list, 0, sizeof(free_list));
    next_page = 0;
    pages_held = 0;
    bytes_used = 0;
}

void *kalloc_pages(uint32_t count, uintptr_t *phys)
{
    uint32_t flags = irq_save();
    uint32_t page;
    void *va;

    if (count == 0 || count > KHEAP_PAGES) {
        irq_restore(flags);
        return NULL;
    }
    page = find_run(count);
    if (page >= KHEAP_PAGES || map_run(page, count) != 0) {
        irq_restore(flags);
        klog(KLOG_ERR, "kheap", "%d pages: no room or no frame", (int)count);
        return NULL;
    }
    va = (void *)(KVA_HEAP + page * PAGE_SIZE);
    if (phys) {
        *phys = paging_virt_to_phys((uintptr_t)va);
    }
    irq_restore(flags);
    return va;
}

void kfree_pages(void *p, uint32_t count)
{
    uintptr_t va = (uintptr_t)p;
    uint32_t flags;

    if (!p || va < KVA_HEAP || va >= KVA_HEAP + KVA_HEAP_SIZE || (va & (PAGE_SIZE - 1))) {
        klog(KLOG_ERR, "kheap", "free of pages the heap did not give: 0x%x", (uint32_t)va);
        return;
    }
    flags = irq_save();
    unmap_run((va - KVA_HEAP) / PAGE_SIZE, count);
    irq_restore(flags);
}

/* Interrupts off. A page carved into blocks of class `cls`, its blocks on
 * the class's free list; -1 when no page could be had. */
static int add_class_page(uint32_t cls)
{
    uint32_t page = find_run(1);
    struct page_hdr *hdr;
    uint8_t *base;
    uint32_t size = class_size[cls];
    uint32_t off;

    if (page >= KHEAP_PAGES || map_run(page, 1) != 0) {
        return -1;
    }
    base = (uint8_t *)(KVA_HEAP + page * PAGE_SIZE);
    hdr = (struct page_hdr *)base;
    hdr->magic = KHEAP_MAGIC;
    hdr->cls = cls;
    hdr->used = 0;
    hdr->pages = 1;
    for (off = KHEAP_HDR; off + size <= PAGE_SIZE; off += size) {
        *(void **)(base + off) = free_list[cls];
        free_list[cls] = base + off;
    }
    return 0;
}

/* Interrupts off. Take every block of the page at `base` off its class's
 * free list, so the page can go back. */
static void drop_class_page(uint8_t *base, uint32_t cls)
{
    void **link = &free_list[cls];

    while (*link) {
        uint8_t *block = *link;

        if (block >= base && block < base + PAGE_SIZE) {
            *link = *(void **)block;
        } else {
            link = (void **)block;
        }
    }
}

void *kmalloc(size_t size)
{
    uint32_t flags, cls;
    void *block;

    if (size == 0 || size > KVA_HEAP_SIZE - PAGE_SIZE) {
        return NULL; /* nothing, or more than the region could hold */
    }
    if (size > class_size[KHEAP_CLASSES - 1]) {
        uint32_t count = (uint32_t)((size + KHEAP_HDR + PAGE_SIZE - 1) / PAGE_SIZE);
        uint8_t *base = kalloc_pages(count, NULL);
        struct page_hdr *hdr;

        if (!base) {
            return NULL;
        }
        hdr = (struct page_hdr *)base;
        hdr->magic = KHEAP_MAGIC;
        hdr->cls = KHEAP_LARGE;
        hdr->used = (uint32_t)size;
        hdr->pages = count;
        flags = irq_save();
        bytes_used += (uint32_t)size;
        irq_restore(flags);
        return base + KHEAP_HDR;
    }
    for (cls = 0; class_size[cls] < size; cls++) {
    }
    flags = irq_save();
    if (!free_list[cls] && add_class_page(cls) != 0) {
        irq_restore(flags);
        klog(KLOG_ERR, "kheap", "%d bytes: no room or no frame", (int)size);
        return NULL;
    }
    block = free_list[cls];
    free_list[cls] = *(void **)block;
    ((struct page_hdr *)((uintptr_t)block & ~(PAGE_SIZE - 1)))->used++;
    bytes_used += class_size[cls];
    irq_restore(flags);
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
    if (va < KVA_HEAP || va >= KVA_HEAP + KVA_HEAP_SIZE) {
        klog(KLOG_ERR, "kheap", "free of memory the heap did not give: 0x%x", (uint32_t)va);
        return;
    }
    hdr = (struct page_hdr *)(va & ~(PAGE_SIZE - 1));
    flags = irq_save();
    if (hdr->magic != KHEAP_MAGIC) {
        irq_restore(flags);
        klog(KLOG_ERR, "kheap", "free of a page without the header: 0x%x", (uint32_t)va);
        return;
    }
    if (hdr->cls == KHEAP_LARGE) {
        uint32_t count = hdr->pages;

        bytes_used -= hdr->used;
        hdr->magic = 0;
        unmap_run(((uintptr_t)hdr - KVA_HEAP) / PAGE_SIZE, count);
        irq_restore(flags);
        return;
    }
    if (hdr->cls >= KHEAP_CLASSES || hdr->used == 0) {
        irq_restore(flags);
        klog(KLOG_ERR, "kheap", "free of a block not in use: 0x%x", (uint32_t)va);
        return;
    }
    *(void **)p = free_list[hdr->cls];
    free_list[hdr->cls] = p;
    hdr->used--;
    bytes_used -= class_size[hdr->cls];
    if (hdr->used == 0) {
        drop_class_page((uint8_t *)hdr, hdr->cls);
        hdr->magic = 0;
        unmap_run(((uintptr_t)hdr - KVA_HEAP) / PAGE_SIZE, 1);
    }
    irq_restore(flags);
}

void kheap_stats(uint32_t *pages, uint32_t *bytes)
{
    if (pages) {
        *pages = pages_held;
    }
    if (bytes) {
        *bytes = bytes_used;
    }
}
