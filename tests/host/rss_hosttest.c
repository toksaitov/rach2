/**
 * Host test of the resident counts by kind of mm/paging.c, which
 * /proc/PID/status reads as RssAnon, RssFile and RssShmem, with i686's
 * `cpu.h`; rss64_hosttest.c runs the same rows with x86-64's, and the rows
 * of huge pages besides.
 *
 * host-deps: rach2/sys/mm/vm.c rach2/sys/mm/frame.c rach2/sys/mm/notify.c
 * host-deps: rach2/sys/mm/pin.c rach2/sys/mm/regions.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/spinlock_host.c
 * host-deps: tests/host/compat/mmap_lock_host.c
 * host-deps: tests/host/compat/atomic_host.c
 * host-cflags: -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast
 * host-cflags: -U__UINTPTR_TYPE__ -D__UINTPTR_TYPE__=unsigned
 * host-cflags: -U__INTPTR_TYPE__ -D__INTPTR_TYPE__=int
 * host-cflags: -U__SIZE_TYPE__ -D__SIZE_TYPE__=unsigned
 * host-cflags: -U__PTRDIFF_TYPE__ -D__PTRDIFF_TYPE__=int
 * host-cflags: -DRACH2_RSS_CHECK
 *
 * mm_model.h includes paging.c whole over a model of the page tables,
 * whose directory keeps the counts `mm_resident_add` is given. Each row
 * changes the entries as the kernel's paths do and checks the three counts
 * against the figures the row expects and against the debug kernel's walk
 * (`paging_rss_check`), which finds no difference. The rows: faults of
 * private anonymous memory, of a private mapping of a file on disk and of
 * a file in memory, read and written, and of a shared mapping of the file
 * in memory; a copy-on-write break made in place, which moves a file's page
 * to anonymous, and one that copies; a fork, whose child counts what its
 * parent does, and a write in the child; munmap through `regions_unmap`,
 * splitting a region, trimming one at its end and taking a whole one with
 * the piece of another; mremap's move, which keeps the kinds of the pages
 * it moves and takes away the tail it drops, and its failure, which takes
 * the moved pages away by the region they had (`unmap_user_range_as`);
 * MADV_DONTNEED; and the end of the space. With x86-64's entries: a huge
 * fault, a fork of the huge page and a write that splits it, a collapse,
 * the shrinker of underused pages and the huge zero page, which counts
 * nothing until a write copies one page of it. No count goes below zero.
 *
 * Returns 0 when every row passes, 1 otherwise.
 */

#ifndef FRAMES
#define FRAMES 2048u
#endif
#include "mm_model.h"

#include "mm/regions.h"

#define NAME "rss-hosttest"
#define P    ((uintptr_t)PAGE_SIZE)
#define RW   (VM_PROT_READ | VM_PROT_WRITE)
#define LIM  (~(uintptr_t)0)
#define AN   (WIN_BASE)            /* private anonymous memory, 8 pages */
#define PF   (WIN_BASE + 16 * P)   /* a private mapping of a file on disk, 8 pages */
#define SH   (WIN_BASE + 32 * P)   /* a shared mapping of a file in memory, 4 pages */
#define PT   (WIN_BASE + 48 * P)   /* a private mapping of the file in memory, 4 pages */
#define MV   (WIN_BASE + 2 * SPAN) /* where mremap moves PT */
#define MV2  (WIN_BASE + 2 * SPAN + 64 * P)
#define END  (WIN_BASE + WIN_TABLES * SPAN)

/* A file on disk, whose pages a fault reads into a frame of its own. */
static struct vm_file disk;

/** Read page `pgoff` of the disk file: a byte of its number. */
static int disk_fill(struct vm_file *f, uint32_t pgoff, uintptr_t phys)
{
    (void)f;
    *(unsigned char *)model_frame(phys) = (unsigned char)(pgoff + 1);
    return 1;
}

/* A file in memory, a tmpfs file or a memfd, whose pages a fault lends. */
static struct {
    struct vm_file vf;
    uintptr_t page[4];
} tmp;

/** Lend page `pgoff` of the file in memory. */
static int tmp_share(struct vm_file *f, uint32_t pgoff, uintptr_t *phys)
{
    (void)f;
    if (pgoff >= 4) {
        return -1;
    }
    frame_ref(tmp.page[pgoff]);
    *phys = tmp.page[pgoff];
    return 0;
}

/** Whether the counts of `mm` are `anon`, `file` and `shmem` pages, and the
 * debug kernel's walk agrees with them. */
static int counts(struct mm *mm, long anon, long file, long shmem)
{
    page_dir_t *d = mm->dir;

    return d->rss[MM_RSS_ANON] == anon && d->rss[MM_RSS_FILE] == file &&
           d->rss[MM_RSS_SHMEM] == shmem && d->resident == anon + file + shmem &&
           paging_rss_check(mm) == 0;
}

/** Fault `va` of `mm` in, for a write when `write`. */
static void touch(struct mm *mm, uintptr_t va, int write)
{
    check(resolve_held(mm, va, write, LIM) == 1, "a fault resolves");
}

/** The regions of the rows, and the files they map. */
static void setup(void)
{
    uint32_t i;

    disk.refs = 1;
    disk.fill = disk_fill;
    tmp.vf.refs = 1;
    tmp.vf.share = tmp_share;
    for (i = 0; i < 4; i++) {
        tmp.page[i] = alloc_user_frame();
    }
    model_shmem = &tmp.vf;
    check(vm_insert(&mm_a.vm, AN, AN + 8 * P, RW, VM_ANON) == 0 &&
              vm_insert_file(&mm_a.vm, PF, PF + 8 * P, RW, VM_FILE, &disk, 0) == 0 &&
              vm_insert_file(&mm_a.vm, SH, SH + 4 * P, RW, VM_SHARED, &tmp.vf, 0) == 0 &&
              vm_insert_file(&mm_a.vm, PT, PT + 4 * P, RW, VM_FILE, &tmp.vf, 0) == 0,
          "the rows' regions are made");
}

/** A fault counts the page by the kind its region and frame give it. */
static void fault_rows(void)
{
    touch(&mm_a, AN, 1);
    touch(&mm_a, AN + P, 1);
    touch(&mm_a, AN + 2 * P, 0);
    check(counts(&mm_a, 3, 0, 0), "anonymous memory written or read counts as anonymous");
    touch(&mm_a, PF, 0);
    touch(&mm_a, PF + P, 0);
    touch(&mm_a, PF + 2 * P, 1);
    check(counts(&mm_a, 4, 2, 0),
          "a private file page read counts as the file's, and one written as anonymous");
    touch(&mm_a, SH, 0);
    touch(&mm_a, SH + P, 1);
    touch(&mm_a, PT, 0);
    check(counts(&mm_a, 4, 2, 3),
          "a page of a file in memory counts as shared memory, shared or private and read");
}

/** A copy-on-write break moves a file's page to anonymous, in place or by
 * a copy. */
static void cow_rows(void)
{
    uintptr_t was = PTE_ADDR(pte_of(&dir_a, PF));

    touch(&mm_a, PF, 1);
    check(PTE_ADDR(pte_of(&dir_a, PF)) == was && counts(&mm_a, 5, 1, 3),
          "a private file page made the process's own in place moves to anonymous");
    touch(&mm_a, PT, 1);
    check(PTE_ADDR(pte_of(&dir_a, PT)) != tmp.page[0] && counts(&mm_a, 6, 1, 2),
          "a copy of a page of a file in memory moves it to anonymous");
}

/** A fork's child counts its pages as its parent does, from none. */
static void fork_rows(void)
{
    check(paging_fork_copy(&mm_a, &dir_b) == 0 && vm_copy(&mm_b.vm, &mm_a.vm) == 0, "fork copies");
    check(counts(&mm_b, 6, 1, 2) && counts(&mm_a, 6, 1, 2),
          "the child counts the parent's pages by the same kinds");
    touch(&mm_b, PF + P, 1);
    check(counts(&mm_b, 7, 0, 2) && counts(&mm_a, 6, 1, 2),
          "a write in the child copies a file page and moves the child's alone");
    unmap_user_range(&mm_b, WIN_BASE, END);
    check(counts(&mm_b, 0, 0, 0), "the child's pages all go");
    vm_free(&mm_b.vm);
}

/** munmap takes each page from the count of the kind its region gave it,
 * the pieces a split or a trim cuts from a region that stays among them. */
static void munmap_rows(void)
{
    uint32_t refs = disk.refs;

    check(regions_unmap(&mm_a, PF + P, PF + 3 * P) == 0 && counts(&mm_a, 5, 0, 2),
          "a split takes a file page and an anonymous page from the middle of a region");
    check(mm_a.vm.detached == NULL && mm_a.vm.held == 0 && disk.refs == refs + 1,
          "the piece cut out lets its file go, and the split's upper part holds it");
    check(regions_unmap(&mm_a, SH + P, SH + 8 * P) == 0 && counts(&mm_a, 5, 0, 1),
          "a trim at a region's end takes its page of shared memory");
    check(regions_unmap(&mm_a, AN + P, PF + P) == 0 && counts(&mm_a, 2, 0, 1),
          "a cut through a trimmed region and a whole one takes their anonymous pages");
    check(regions_unmap(&mm_a, PF + 3 * P, PF + 8 * P) == 0 && counts(&mm_a, 2, 0, 1),
          "a cut of pages never backed counts nothing");
}

/** mremap's move keeps each moved page's kind, takes the tail it drops away
 * by its region, and a failed move takes its pages away by the region they
 * had. */
static void mremap_rows(void)
{
    vma_t like;
    uint32_t pgoff;

    touch(&mm_a, PT + P, 0);
    touch(&mm_a, PT + 3 * P, 0);
    check(counts(&mm_a, 2, 0, 3), "two more pages of the file in memory are read");
    like = *vm_find(&mm_a.vm, PT);
    pgoff = like.pgoff;
    check(vm_reserve(&mm_a.vm, 3) == 0 && paging_move_range(&mm_a, PT, MV, 2 * P) == 0 &&
              vm_detach(&mm_a.vm, PT, PT + 4 * P) == 0,
          "the move's entries and the old region go");
    unmap_user_range(&mm_a, PT, PT + 4 * P);
    vm_release_detached(&mm_a.vm);
    check(vm_insert_like(&mm_a.vm, MV, MV + 2 * P, &like, pgoff) == 0 && counts(&mm_a, 2, 0, 2),
          "the moved pages keep their kinds, and the tail dropped takes its page away");
    like = *vm_find(&mm_a.vm, MV);
    vm_file_get(like.file);
    check(paging_move_range(&mm_a, MV, MV2, 2 * P) == 0 && vm_detach(&mm_a.vm, MV, MV + 2 * P) == 0,
          "a second move's entries and region go");
    vm_release_detached(&mm_a.vm);
    unmap_user_range_as(&mm_a, MV2, MV2 + 2 * P, &like);
    vm_file_put(like.file);
    check(counts(&mm_a, 1, 0, 1),
          "a move whose region could not be made takes its pages away by the old region");
}

/** MADV_DONTNEED and the end of the space take every page away. */
static void end_rows(void)
{
    touch(&mm_a, SH + 0, 1);
    unmap_user_range(&mm_a, SH, SH + P);
    check(counts(&mm_a, 1, 0, 0), "MADV_DONTNEED takes a shared page from shared memory");
    unmap_user_range(&mm_a, WIN_BASE, END);
    check(counts(&mm_a, 0, 0, 0) && !model_rss_below,
          "the end of the space takes every page, and no count went below zero");
    vm_remove(&mm_a.vm, WIN_BASE, END);
}

/** The protection keys are off. */
int fpu_pkeys(void)
{
    return 0;
}

/** PKRU with the keys off. */
uint32_t fpu_pkru(void)
{
    return 0;
}

/** No region maps a disk file whose mapping a pin would hold. */
int filemap_disk_pages(const struct vm_file *f)
{
    (void)f;
    return 0;
}

#if MMU_HUGE
#define HB (WIN_BASE + 3 * SPAN) /* a 2 MiB of private anonymous memory */

/** A huge page counts its 512 pages as anonymous, the huge zero page none
 * of its own, and a fork, a split, a collapse and the shrinker keep the
 * counts. */
static void huge_rows(void)
{
    uintptr_t i;

    thp_policy.enabled = THP_ALWAYS;
    check(vm_insert(&mm_a.vm, HB, HB + HUGE_SPAN, RW, VM_ANON) == 0, "a 2 MiB region");
    touch(&mm_a, HB, 1);
    check(mmu_user_huge(&dir_a, HB) != 0 && counts(&mm_a, HUGE_PAGES, 0, 0),
          "a huge fault counts 512 anonymous pages");
    check(paging_fork_copy(&mm_a, &dir_b) == 0 && vm_copy(&mm_b.vm, &mm_a.vm) == 0 &&
              counts(&mm_b, HUGE_PAGES, 0, 0),
          "a fork's child counts the huge page's pages");
    touch(&mm_a, HB + 3 * P, 1);
    check(counts(&mm_a, HUGE_PAGES, 0, 0) && counts(&mm_b, HUGE_PAGES, 0, 0),
          "a write that splits the shared page and copies one counts no new page");
    unmap_user_range(&mm_b, HB, HB + HUGE_SPAN);
    vm_free(&mm_b.vm);
    unmap_user_range(&mm_a, HB, HB + HUGE_SPAN);
    check(counts(&mm_a, 0, 0, 0) && counts(&mm_b, 0, 0, 0), "both unmaps take every page");

    thp_policy.enabled = THP_NEVER;
    for (i = 0; i < 3; i++) {
        touch(&mm_a, HB + i * P, 1);
        *(unsigned char *)model_frame(PTE_ADDR(pte_of(&dir_a, HB + i * P))) = 1;
    }
    touch(&mm_a, HB + 400 * P, 0);
    thp_policy.enabled = THP_ALWAYS;
    check(counts(&mm_a, 4, 0, 0) && paging_collapse(&mm_a, HB, THP_COLLAPSE) == 0 &&
              mmu_user_huge(&dir_a, HB) != 0 && counts(&mm_a, HUGE_PAGES, 0, 0),
          "a collapse of four small pages counts 512");
    thp_policy.shrink_underused = 1;
    thp_policy.max_ptes_none = 100;
    check(huge_shrink_underused(4096) == HUGE_PAGES - 3 && counts(&mm_a, 3, 0, 0),
          "the shrinker of underused pages takes the 509 pages of zeros away");
    thp_policy.shrink_underused = 0;
    thp_policy.max_ptes_none = 511;
    unmap_user_range(&mm_a, HB, HB + HUGE_SPAN);

    thp_policy.zero_page = 1;
    touch(&mm_a, HB, 0);
    check((mmu_user_huge(&dir_a, HB) & PTE_ZEROFILL) && counts(&mm_a, 0, 0, 0),
          "the huge zero page counts nothing");
    touch(&mm_a, HB + 5 * P, 1);
    check(counts(&mm_a, 1, 0, 0), "a write to it copies and counts one page");
    unmap_user_range(&mm_a, HB, HB + HUGE_SPAN);
    vm_remove(&mm_a.vm, HB, HB + HUGE_SPAN);
    check(counts(&mm_a, 0, 0, 0) && !model_rss_below, "the unmap leaves none");
}
#endif

int main(void)
{
    model_start(NAME);
#if MMU_HUGE
    /* The device window is the first 2 MiB, which huge pages never take. */
    _window_frames = HUGE_PAGES;
    _free_window_frames = HUGE_PAGES - USER_FRAME0;
    paging_thp_init();
#endif
    setup();
    fault_rows();
    cow_rows();
    fork_rows();
    munmap_rows();
    mremap_rows();
    end_rows();
#if MMU_HUGE
    huge_rows();
#endif
    return model_end();
}
