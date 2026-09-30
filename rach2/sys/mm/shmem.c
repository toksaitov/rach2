/*
 * shmem.c: file bytes in memory, as shmem.h describes them.
 */

#include "shmem.h"

#include "filemap.h"
#include "kheap.h"
#include "paging.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"

/* The most pages a file in memory has, SHMEM_MAX_SIZE of them. */
#define SHMEM_MAX_PAGES ((uint32_t)(SHMEM_MAX_SIZE / PAGE_SIZE))

/* Make the table reach page `n - 1`, where `n` is at most SHMEM_MAX_PAGES;
 * 0, or -ENOSPC. The length doubles from 16 and stops at that bound, so
 * neither it nor the table's size in bytes can wrap. */
static int reach(struct shmem *sh, uint32_t n)
{
    uint32_t cap = sh->npages ? sh->npages : 16u;
    uintptr_t *grown;

    if (n <= sh->npages) {
        return 0;
    }
    while (cap < n) {
        cap *= 2;
    }
    if (cap > SHMEM_MAX_PAGES) {
        cap = SHMEM_MAX_PAGES;
    }
    grown = kzalloc(sizeof(uintptr_t) * cap);
    if (!grown) {
        return -ENOSPC;
    }
    if (sh->npages) {
        k_memcpy(grown, sh->pages, sizeof(uintptr_t) * sh->npages);
    }
    kfree(sh->pages);
    sh->pages = grown;
    sh->npages = cap;
    return 0;
}

/* The frame of page `i`, made and zeroed when there is none; 0 when none
 * can be had, or when `i` is past the pages a file in memory may have: a
 * mapping asks for its page by an offset the caller chose. */
static uintptr_t page_at(struct shmem *sh, uint32_t i)
{
    uintptr_t phys;

    if (i >= SHMEM_MAX_PAGES || reach(sh, i + 1u) != 0) {
        return 0;
    }
    if (sh->pages[i]) {
        return sh->pages[i];
    }
    phys = alloc_user_frame();
    if (phys == 0) {
        return 0;
    }
    if (paging_zero_frame(phys) != 0) {
        free_frame(phys);
        return 0;
    }
    sh->pages[i] = phys;
    return phys;
}

int32_t shmem_read(struct shmem *sh, void *buf, uint32_t count, uint64_t off)
{
    uint8_t *out = (uint8_t *)buf;
    uint32_t done = 0;

    if (off >= sh->size) {
        return 0;
    }
    if (count > sh->size - off) {
        count = (uint32_t)(sh->size - off);
    }
    while (done < count) {
        uint32_t i = (uint32_t)((off + done) / PAGE_SIZE);
        uint32_t in = (uint32_t)((off + done) % PAGE_SIZE);
        uint32_t take = PAGE_SIZE - in;

        if (take > count - done) {
            take = count - done;
        }
        if (i < sh->npages && sh->pages[i]) {
            uint8_t *page = kmap_temp(sh->pages[i]);

            if (!page) {
                return done ? (int32_t)done : -EIO;
            }
            k_memcpy(out + done, page + in, take);
            kunmap_temp(page);
        } else {
            k_memset(out + done, 0, take);
        }
        done += take;
    }
    return (int32_t)done;
}

int32_t shmem_write(struct shmem *sh, const void *buf, uint32_t count, uint64_t off)
{
    const uint8_t *in = (const uint8_t *)buf;
    uint32_t done = 0;

    if (off >= SHMEM_MAX_SIZE || count > SHMEM_MAX_SIZE - off) {
        return -EFBIG;
    }
    while (done < count) {
        uint32_t i = (uint32_t)((off + done) / PAGE_SIZE);
        uint32_t at = (uint32_t)((off + done) % PAGE_SIZE);
        uint32_t take = PAGE_SIZE - at;
        uintptr_t phys = page_at(sh, i);
        uint8_t *page;

        if (take > count - done) {
            take = count - done;
        }
        page = phys ? kmap_temp(phys) : NULL;
        if (!page) {
            break;
        }
        k_memcpy(page + at, in + done, take);
        kunmap_temp(page);
        done += take;
        if (off + done > sh->size) {
            sh->size = off + done;
        }
    }
    return done || count == 0 ? (int32_t)done : -ENOSPC;
}

int shmem_truncate(struct shmem *sh, uint64_t size)
{
    uint32_t keep, i;

    if (size > SHMEM_MAX_SIZE) {
        return -EFBIG;
    }
    keep = (uint32_t)((size + PAGE_SIZE - 1) / PAGE_SIZE);
    if (sh->maps && keep < sh->npages) {
        filemap_shmem_cut(sh, keep);
    }
    for (i = keep; i < sh->npages; i++) {
        if (sh->pages[i]) {
            free_frame(sh->pages[i]);
            sh->pages[i] = 0;
        }
    }
    /* The part of the last page past the new end reads as zeros if the
     * file grows again, as in Linux. */
    if (size % PAGE_SIZE && keep - 1u < sh->npages && sh->pages[keep - 1u]) {
        uint8_t *page = kmap_temp(sh->pages[keep - 1u]);

        if (page) {
            k_memset(page + size % PAGE_SIZE, 0, PAGE_SIZE - (uint32_t)(size % PAGE_SIZE));
            kunmap_temp(page);
        }
    }
    sh->size = size;
    return 0;
}

uintptr_t shmem_map_page(struct shmem *sh, uint32_t pgoff)
{
    uintptr_t phys = page_at(sh, pgoff);

    if (phys) {
        frame_ref(phys);
    }
    return phys;
}

uintptr_t shmem_resident(const struct shmem *sh, uint32_t pgoff)
{
    return pgoff < sh->npages ? sh->pages[pgoff] : 0;
}

uintptr_t shmem_adopt(struct shmem *sh, uint32_t pgoff, uintptr_t phys)
{
    if (pgoff >= SHMEM_MAX_PAGES || reach(sh, pgoff + 1u) != 0) {
        free_frame(phys);
        return 0;
    }
    if (sh->pages[pgoff]) {
        free_frame(phys);
    } else {
        sh->pages[pgoff] = phys;
    }
    frame_ref(sh->pages[pgoff]);
    return sh->pages[pgoff];
}

void shmem_free(struct shmem *sh)
{
    shmem_truncate(sh, 0);
    kfree(sh->pages);
    sh->pages = NULL;
    sh->npages = 0;
}
