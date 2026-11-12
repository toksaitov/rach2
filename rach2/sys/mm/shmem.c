/*
 * shmem.c: file bytes in memory, as shmem.h describes them.
 */

#include "shmem.h"

#include "filemap.h"
#include "kheap.h"
#include "paging.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../proc/sched.h"

/* The most pages a file in memory has, SHMEM_MAX_SIZE of them. */
#define SHMEM_MAX_PAGES ((uint32_t)(SHMEM_MAX_SIZE / PAGE_SIZE))

/** Make the directory reach leaf `n - 1`; 0, or -ENOSPC. The length doubles
 * from 4; the largest file's directory has 4194304 entries, 16 MiB on i686
 * and 32 MiB on x86-64, which a write near its end needs and the heap may
 * not have. */
static int reach(struct shmem *sh, uint32_t n)
{
    uint32_t cap = sh->nleaves ? sh->nleaves : 4u;
    uintptr_t **grown;

    if (n <= sh->nleaves) {
        return 0;
    }
    while (cap < n) {
        cap *= 2;
    }
    grown = kzalloc(sizeof(uintptr_t *) * cap);
    if (!grown) {
        return -ENOSPC;
    }
    if (sh->nleaves) {
        k_memcpy(grown, sh->leaves, sizeof(uintptr_t *) * sh->nleaves);
    }
    kfree(sh->leaves);
    sh->leaves = grown;
    sh->nleaves = cap;
    sh->npages = cap > SHMEM_MAX_PAGES / SHMEM_LEAF ? SHMEM_MAX_PAGES : cap * SHMEM_LEAF;
    return 0;
}

/** The table's entry for page `i`, or NULL when its leaf is missing; with
 * `make`, the leaf is made, and NULL means no memory or `i` past the pages
 * a file in memory may have. */
static uintptr_t *slot(struct shmem *sh, uint32_t i, int make)
{
    uint32_t leaf = i / SHMEM_LEAF;

    if (i >= SHMEM_MAX_PAGES) {
        return NULL;
    }
    if (leaf >= sh->nleaves || !sh->leaves[leaf]) {
        if (!make || reach(sh, leaf + 1u) != 0) {
            return NULL;
        }
        sh->leaves[leaf] = kzalloc(sizeof(uintptr_t) * SHMEM_LEAF);
        if (!sh->leaves[leaf]) {
            return NULL;
        }
    }
    return &sh->leaves[leaf][i % SHMEM_LEAF];
}

int shmem_quota_full(const struct shmem *sh)
{
    const struct shmem_quota *q = sh->quota;

    if (!q) {
        return 0;
    }
    if (q->cap) {
        return q->used >= q->cap();
    }
    return q->limit && q->used >= q->limit;
}

/** Count a frame the table takes: 1, or 0 when the quota has no room for
 * it. */
static int take_page(struct shmem *sh)
{
    if (sh->quota) {
        if (shmem_quota_full(sh)) {
            return 0;
        }
        sh->quota->used++;
    }
    sh->held++;
    return 1;
}

/** Count a frame the table gives back. */
static void give_page(struct shmem *sh)
{
    if (sh->quota) {
        sh->quota->used--;
    }
    sh->held--;
}

/** The frame of page `i`, made and zeroed when there is none; 0 when none
 * can be had, the quota has no room, or `i` is past the pages a file in
 * memory may have: a mapping asks for its page by an offset the caller
 * chose. */
static uintptr_t page_at(struct shmem *sh, uint32_t i)
{
    uintptr_t *e = slot(sh, i, 1);
    uintptr_t phys;

    if (!e || *e != 0 || !take_page(sh)) {
        return e ? *e : 0;
    }
    phys = alloc_user_frame();
    if (phys != 0 && paging_zero_frame(phys) != 0) {
        free_frame(phys);
        phys = 0;
    }
    if (phys == 0) {
        give_page(sh);
        return 0;
    }
    *e = phys;
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
        if (shmem_resident(sh, i)) {
            uint8_t *page = kmap_temp(shmem_resident(sh, i));

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

/**
 * Free the pages of `sh` from page `keep` on, and the leaves that hold none
 * below `keep`. With `give_way` set, the walk gives way after each leaf, or
 * each SHMEM_LEAF slots of the table with no leaf (`sched_give_way`), as
 * Linux's shmem_undo_range calls cond_resched after each batch of folios
 * (mm/shmem.c:1060 and 1101); each leaf is whole then, its freed entries
 * cleared and a freed leaf off the table.
 */
static void cut_pages(struct shmem *sh, uint32_t keep, int give_way)
{
    uint32_t i, leaf, work = 0;

    for (leaf = keep / SHMEM_LEAF; leaf < sh->nleaves; leaf++) {
        uintptr_t *frames;

        if (give_way && work >= SHMEM_LEAF) {
            sched_give_way();
            work = 0;
        }
        frames = sh->leaves[leaf];
        if (!frames) {
            work++;
            continue;
        }
        work += SHMEM_LEAF;
        for (i = leaf == keep / SHMEM_LEAF ? keep % SHMEM_LEAF : 0; i < SHMEM_LEAF; i++) {
            if (frames[i]) {
                free_frame(frames[i]);
                frames[i] = 0;
                give_page(sh);
            }
        }
        if (leaf * SHMEM_LEAF >= keep) {
            kfree(frames);
            sh->leaves[leaf] = NULL;
        }
    }
}

int shmem_truncate(struct shmem *sh, uint64_t size)
{
    uint32_t keep;
    uintptr_t last;

    if (size > SHMEM_MAX_SIZE) {
        return -EFBIG;
    }
    keep = (uint32_t)((size + PAGE_SIZE - 1) / PAGE_SIZE);
    if (sh->maps && size < sh->size) {
        filemap_shmem_cut(sh, keep);
    }
    cut_pages(sh, keep, 0);
    last = keep ? shmem_resident(sh, keep - 1u) : 0;
    if (size % PAGE_SIZE && last) {
        uint8_t *page = kmap_temp(last);

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
    uint32_t leaf = pgoff / SHMEM_LEAF;

    if (leaf >= sh->nleaves || !sh->leaves[leaf]) {
        return 0;
    }
    return sh->leaves[leaf][pgoff % SHMEM_LEAF];
}

int shmem_has_leaf(const struct shmem *sh, uint32_t pgoff)
{
    uint32_t leaf = pgoff / SHMEM_LEAF;

    return pgoff < SHMEM_MAX_PAGES && leaf < sh->nleaves && sh->leaves[leaf] != NULL;
}

uint64_t shmem_count(const struct shmem *sh, uint32_t first, uint32_t last)
{
    uint32_t leaf = first / SHMEM_LEAF;
    uint32_t end = last / SHMEM_LEAF;
    uint64_t count = 0;
    uint32_t work = 0;

    for (; leaf <= end && leaf < sh->nleaves; leaf++) {
        const uintptr_t *frames = sh->leaves[leaf];
        uint32_t from = leaf == first / SHMEM_LEAF ? first % SHMEM_LEAF : 0;
        uint32_t to = leaf == end ? last % SHMEM_LEAF : SHMEM_LEAF - 1;
        uint32_t i;

        if (frames) {
            for (i = from; i <= to; i++) {
                count += frames[i] != 0;
            }
            work += SHMEM_LEAF;
        } else {
            work++;
        }
        if (work >= SHMEM_LEAF) {
            sched_service_point();
            work = 0;
        }
    }
    return count;
}

uintptr_t shmem_adopt(struct shmem *sh, uint32_t pgoff, uintptr_t phys)
{
    uintptr_t *e = slot(sh, pgoff, 1);

    if (!e) {
        free_frame(phys);
        return 0;
    }
    if (*e) {
        free_frame(phys);
    } else {
        sh->held++;
        if (sh->quota) {
            sh->quota->used++;
        }
        *e = phys;
    }
    frame_ref(*e);
    return *e;
}

int shmem_allocate(struct shmem *sh, uint64_t off, uint64_t len)
{
    uint32_t i, last;

    if (off >= SHMEM_MAX_SIZE || len > SHMEM_MAX_SIZE - off) {
        return -EFBIG;
    }
    last = (uint32_t)((off + len - 1u) >> 12);
    for (i = (uint32_t)(off >> 12); i <= last; i++) {
        if (page_at(sh, i) == 0) {
            return -ENOSPC;
        }
    }
    return 0;
}

void shmem_punch(struct shmem *sh, uint64_t off, uint64_t len)
{
    uint64_t end = off + len > (uint64_t)sh->npages << 12 ? (uint64_t)sh->npages << 12 : off + len;
    uint64_t i;

    for (i = off >> 12; off < end && i << 12 < end; i++) {
        uint64_t start = i << 12;
        uint32_t from = start < off ? (uint32_t)(off - start) : 0u;
        uint32_t to = end - start < PAGE_SIZE ? (uint32_t)(end - start) : PAGE_SIZE;
        uintptr_t *e = slot(sh, (uint32_t)i, 0);
        uint8_t *page;

        if (!e) {
            i |= SHMEM_LEAF - 1u;
            continue;
        }
        if (!*e) {
            continue;
        }
        if (from == 0 && to == PAGE_SIZE && sh->maps == 0) {
            free_frame(*e);
            *e = 0;
            give_page(sh);
            continue;
        }
        page = kmap_temp(*e);
        if (page) {
            k_memset(page + from, 0, to - from);
            kunmap_temp(page);
        }
    }
}

/** Free what `shmem_free` frees, the pages with or without giving way, as
 * `shmem_truncate` to 0 frees them. */
static void free_all(struct shmem *sh, int give_way)
{
    struct shmem_policy *n;

    if (sh->maps && sh->size > 0) {
        filemap_shmem_cut(sh, 0);
    }
    cut_pages(sh, 0, give_way);
    sh->size = 0;
    kfree(sh->leaves);
    sh->leaves = NULL;
    sh->nleaves = 0;
    sh->npages = 0;
    while ((n = sh->policies) != NULL) {
        sh->policies = n->next;
        kfree(n);
    }
}

void shmem_free(struct shmem *sh)
{
    free_all(sh, 0);
}

void shmem_free_yielding(struct shmem *sh)
{
    free_all(sh, 1);
}
