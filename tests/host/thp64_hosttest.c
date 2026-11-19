/**
 * Host test of the transparent huge pages of mm/paging.c and mm/thp.c with
 * x86-64's `cpu.h`, whose directory entries map 2 MiB pages.
 *
 * host-deps: rach2/sys/mm/vm.c rach2/sys/mm/frame.c rach2/sys/mm/notify.c
 * host-deps: rach2/sys/mm/pin.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/spinlock_host.c
 * host-deps: tests/host/compat/mmap_lock_host.c tests/host/compat/atomic_host.c
 * host-cflags: -DRACH2_HOST_X86_64
 *
 * mm_model.h includes paging.c whole over a model of the page tables whose
 * directory entries keep the table a huge page replaced. The rows: a fault
 * maps a huge page only in an aligned 2 MiB of a private anonymous region
 * that `enabled`, MADV_HUGEPAGE, MADV_NOHUGEPAGE and PR_SET_THP_DISABLE
 * allow, and only over an empty table; a partial unmap or mprotect splits
 * the page and frees only what it unmapped, and a whole one keeps it; a
 * fork keeps it in parent and child, and a write splits and copies the one
 * page written; a read maps the huge zero page, which a write splits and
 * the shrinker frees once nothing maps it; khugepaged and MADV_COLLAPSE
 * collapse small pages into one, keeping their bytes, as Linux 6.12's
 * checks allow; and the shrinker splits an underused page and frees its
 * pages of zeros.
 *
 * Returns 0 when every row passes, 1 otherwise.
 */

#define FRAMES 4096u
#include "mm_model.h"

#define NAME "thp64-hosttest"
#define P    ((uintptr_t)PAGE_SIZE)
#define H    ((uintptr_t)HUGE_SPAN)
#define RW   (VM_PROT_READ | VM_PROT_WRITE)
#define LIM  (~(uintptr_t)0)
#define A    WIN_BASE

/** The host address of the page `dir` maps at `va`, or NULL. */
static unsigned char *page_at(page_dir_t *dir, uintptr_t va)
{
    pte_t e = pte_of(dir, va);

    return e ? model_frame((uintptr_t)PTE_ADDR(e)) : NULL;
}

/** Whether the 2 MiB of `dir` from `start` is one huge page over frames in
 * order from a 2 MiB boundary, with each entry present beneath it. */
static int is_huge(page_dir_t *dir, uintptr_t start)
{
    pte_t leaf = mmu_user_huge(dir, start);
    uintptr_t base = (uintptr_t)PTE_ADDR(leaf), i;

    if (!leaf || (base & (H - 1)) != 0) {
        return 0;
    }
    for (i = 0; i < HUGE_PAGES; i++) {
        pte_t e = pte_of(dir, start + i * P);

        if (!(e & PTE_P) || PTE_ADDR(e) != base + i * P) {
            return 0;
        }
    }
    return 1;
}

/** A private anonymous region of `mm` over [start, end). */
static void region(struct mm *mm, uintptr_t start, uintptr_t end, uint32_t prot)
{
    check(vm_insert(&mm->vm, start, end, prot, VM_ANON) == 0, "a region");
}

/** Take [start, end) out of `mm`, its region and its pages. */
static void drop(struct mm *mm, uintptr_t start, uintptr_t end)
{
    vm_remove(&mm->vm, start, end);
    unmap_user_range(mm->dir, start, end);
}

/** Whether a write fault at `va` of a fresh region over the 2 MiB from A
 * maps a huge page; the region goes after. */
static int fault_maps_huge(uintptr_t va, uint32_t set)
{
    int huge;

    region(&mm_a, A, A + H, RW);
    if (set) {
        vm_advise(&mm_a.vm, A, A + H, set, VM_PROT_HUGEPAGE | VM_PROT_NOHUGEPAGE);
    }
    check(resolve_held(&mm_a, va, 1, LIM) == 1, "a write faults in");
    huge = mmu_user_huge(&dir_a, A) != 0;
    drop(&mm_a, A, A + H);
    return huge;
}

/** A fault maps a huge page where the policy and the region allow it, and
 * a page of zeros; elsewhere the 4 KiB page alone. */
static void fault_rows(void)
{
    uint32_t allocs = thp_count(THP_FAULT_ALLOC);
    uintptr_t base, i;
    int zeros = 1;

    thp_policy.enabled = THP_ALWAYS;
    region(&mm_a, A, A + 2 * H, RW);
    check(resolve_held(&mm_a, A + 5 * P, 1, LIM) == 1, "a write faults in");
    check(is_huge(&dir_a, A), "a write in an aligned 2 MiB of a region maps a huge page");
    check((mmu_user_huge(&dir_a, A) & (PTE_W | PTE_DIRTY)) == (PTE_W | PTE_DIRTY),
          "writable and dirty, as Linux marks it");
    check(thp_count(THP_FAULT_ALLOC) == allocs + 1 && thp_mapped() == 1 &&
              paging_user_huge(&dir_a, A, A + 2 * H) == HUGE_PAGES,
          "counted as thp_fault_alloc and AnonHugePages");
    for (i = 0; i < HUGE_PAGES; i++) {
        zeros &= page_at(&dir_a, A + i * P)[i % P] == 0;
    }
    check(zeros, "every page of it reads zero");
    base = (uintptr_t)PTE_ADDR(mmu_user_huge(&dir_a, A));
    drop(&mm_a, A, A + 2 * H);
    check(mmu_user_huge(&dir_a, A) == 0 && thp_mapped() == 0 && frame_maps(base / P) == 0,
          "an unmap of the whole page frees it");

    region(&mm_a, A + P, A + H + P, RW);
    check(resolve_held(&mm_a, A + 2 * P, 1, LIM) == 1 && mmu_user_huge(&dir_a, A) == 0,
          "a region that holds no aligned 2 MiB has small pages");
    drop(&mm_a, A + P, A + H + P);

    thp_policy.enabled = THP_MADVISE;
    check(!fault_maps_huge(A, 0), "with enabled madvise a region not advised has small pages");
    check(fault_maps_huge(A, VM_PROT_HUGEPAGE), "and one marked MADV_HUGEPAGE a huge page");
    thp_policy.enabled = THP_ALWAYS;
    check(!fault_maps_huge(A, VM_PROT_NOHUGEPAGE), "MADV_NOHUGEPAGE keeps small pages");
    mm_a.thp_disable = 1;
    check(!fault_maps_huge(A, 0), "and so does PR_SET_THP_DISABLE");
    mm_a.thp_disable = 0;
    thp_policy.enabled = THP_NEVER;
    check(!fault_maps_huge(A, VM_PROT_HUGEPAGE), "and enabled never");

    region(&mm_a, A, A + H, RW);
    check(resolve_held(&mm_a, A, 1, LIM) == 1, "a small page");
    thp_policy.enabled = THP_ALWAYS;
    check(resolve_held(&mm_a, A + 9 * P, 1, LIM) == 1 && mmu_user_huge(&dir_a, A) == 0,
          "a fault over a table with an entry maps a small page");
    drop(&mm_a, A, A + H);
}

/** A partial unmap or mprotect splits the page; a whole mprotect keeps it. */
static void split_rows(void)
{
    uint32_t splits = thp_count(THP_SPLIT_PMD);
    uintptr_t base;

    region(&mm_a, A, A + H, RW);
    check(resolve_held(&mm_a, A, 1, LIM) == 1 && is_huge(&dir_a, A), "a huge page");
    base = (uintptr_t)PTE_ADDR(mmu_user_huge(&dir_a, A));
    unmap_user_range(&dir_a, A + 3 * P, A + 4 * P);
    check(mmu_user_huge(&dir_a, A) == 0 && thp_mapped() == 0 &&
              thp_count(THP_SPLIT_PMD) == splits + 1,
          "an unmap of one page splits the huge page");
    check(frame_maps((base + 3 * P) / P) == 0 && pte_of(&dir_a, A + 3 * P) == 0,
          "and frees that page at once");
    check(frame_maps((base + 4 * P) / P) == 1 &&
              PTE_ADDR(pte_of(&dir_a, A + 4 * P)) == base + 4 * P,
          "and keeps the others where they were");
    drop(&mm_a, A, A + H);

    region(&mm_a, A, A + H, RW);
    check(resolve_held(&mm_a, A, 1, LIM) == 1 && is_huge(&dir_a, A), "a huge page");
    paging_protect_range(&dir_a, A + P, A + 2 * P, VM_PROT_READ);
    check(mmu_user_huge(&dir_a, A) == 0 && !(pte_of(&dir_a, A + P) & PTE_W) &&
              (pte_of(&dir_a, A + 2 * P) & PTE_W),
          "an mprotect of one page splits it and changes that page alone");
    paging_protect_range(&dir_a, A, A + H, RW);
    drop(&mm_a, A, A + H);

    region(&mm_a, A, A + H, RW);
    check(resolve_held(&mm_a, A, 1, LIM) == 1 && is_huge(&dir_a, A), "a huge page");
    paging_protect_range(&dir_a, A, A + H, VM_PROT_READ);
    check(is_huge(&dir_a, A) && !(mmu_user_huge(&dir_a, A) & PTE_W) &&
              !(pte_of(&dir_a, A + 7 * P) & PTE_W),
          "an mprotect of the whole 2 MiB keeps it huge, read-only");
    drop(&mm_a, A, A + H);
}

/** A fork keeps the huge page in both; a write splits and copies one page. */
static void cow_rows(void)
{
    uintptr_t base;

    region(&mm_a, A, A + H, RW);
    check(resolve_held(&mm_a, A, 1, LIM) == 1 && is_huge(&dir_a, A), "a huge page");
    base = (uintptr_t)PTE_ADDR(mmu_user_huge(&dir_a, A));
    page_at(&dir_a, A + 7 * P)[0] = 0x5a;
    check(paging_fork_copy(&mm_a, &dir_b) == 0 && vm_copy(&mm_b.vm, &mm_a.vm) == 0, "fork copies");
    check(is_huge(&dir_a, A) && is_huge(&dir_b, A) && PTE_ADDR(mmu_user_huge(&dir_b, A)) == base &&
              thp_mapped() == 2,
          "a fork keeps the huge page in the parent and the child");
    check(!(mmu_user_huge(&dir_a, A) & PTE_W) && (pte_of(&dir_b, A + 7 * P) & PTE_COW) &&
              frame_maps(base / P) == 2,
          "copy-on-write in both");
    check(resolve_held(&mm_b, A + 7 * P, 1, LIM) == 1, "the child writes");
    check(mmu_user_huge(&dir_b, A) == 0 && is_huge(&dir_a, A),
          "a write splits the child's page and leaves the parent's");
    check(PTE_ADDR(pte_of(&dir_b, A + 7 * P)) != base + 7 * P &&
              (pte_of(&dir_b, A + 7 * P) & PTE_W) && page_at(&dir_b, A + 7 * P)[0] == 0x5a,
          "and copies the one page written");
    check(frame_maps((base + 8 * P) / P) == 2 && frame_maps((base + 7 * P) / P) == 1,
          "the other pages stay shared");
    drop(&mm_b, A, A + H);
    check(resolve_held(&mm_a, A + 8 * P, 1, LIM) == 1 && mmu_user_huge(&dir_a, A) == 0 &&
              PTE_ADDR(pte_of(&dir_a, A + 8 * P)) == base + 8 * P,
          "the parent's write splits its page and takes the frame it alone holds");
    drop(&mm_a, A, A + H);
}

/** A read maps the huge zero page, which a write splits and the shrinker
 * frees once nothing maps it. */
static void zero_rows(void)
{
    uint32_t made = thp_count(THP_ZERO_PAGE_ALLOC);
    uintptr_t zero;
    pte_t leaf;

    thp_policy.zero_page = 1;
    region(&mm_a, A, A + H, RW);
    check(resolve_held(&mm_a, A + 9 * P, 0, LIM) == 1, "a read faults in");
    leaf = mmu_user_huge(&dir_a, A);
    zero = (uintptr_t)PTE_ADDR(leaf);
    check(leaf && (leaf & PTE_ZEROFILL) && (leaf & PTE_COW) && !(leaf & PTE_W) &&
              zero == _huge_zero,
          "a read maps the huge zero page, copy-on-write");
    check(thp_mapped() == 0 && paging_user_huge(&dir_a, A, A + H) == 0 &&
              thp_count(THP_ZERO_PAGE_ALLOC) == made + 1,
          "which AnonHugePages leaves out, counted as thp_zero_page_alloc");
    region(&mm_b, A, A + H, VM_PROT_READ);
    check(resolve_held(&mm_b, A, 0, LIM) == 1 && PTE_ADDR(mmu_user_huge(&dir_b, A)) == zero &&
              !(mmu_user_huge(&dir_b, A) & (PTE_W | PTE_COW)),
          "a read of a read-only region maps the same page, read-only");
    check(resolve_held(&mm_a, A + 9 * P, 1, LIM) == 1, "a write faults in");
    check(mmu_user_huge(&dir_a, A) == 0 && PTE_ADDR(pte_of(&dir_a, A + 9 * P)) != zero + 9 * P &&
              (pte_of(&dir_a, A + 9 * P) & PTE_W) && page_at(&dir_a, A + 9 * P)[5] == 0,
          "a write splits it and gives the page written a frame of its own");
    check(PTE_ADDR(pte_of(&dir_a, A + 10 * P)) == zero + 10 * P,
          "the other pages map the zero page still");
    check(huge_shrink(&_huge_shrinker, 1, 0) == 0 && _huge_zero == zero,
          "the shrinker keeps it while an entry maps it");
    drop(&mm_a, A, A + H);
    drop(&mm_b, A, A + H);
    check(frame_maps(zero / P) == 1, "its own reference is left");
    check(huge_shrink(&_huge_shrinker, 1, 0) == HUGE_PAGES && _huge_zero == 0 &&
              frame_maps(zero / P) == 0,
          "the shrinker frees it once nothing maps it");
}

/** Fault in pages 0 to `n` - 1 of the 2 MiB from A with small pages, each
 * written with its number, and page 400 read. */
static void small_pages(uintptr_t n)
{
    uintptr_t i;

    thp_policy.enabled = THP_NEVER;
    region(&mm_a, A, A + H, RW);
    for (i = 0; i < n; i++) {
        check(resolve_held(&mm_a, A + i * P, 1, LIM) == 1, "a small page");
        page_at(&dir_a, A + i * P)[1] = (unsigned char)(i + 1);
    }
    check(resolve_held(&mm_a, A + 400 * P, 0, LIM) == 1, "a page read");
    thp_policy.enabled = THP_ALWAYS;
}

/** khugepaged and MADV_COLLAPSE collapse small pages as Linux's checks
 * allow, keeping their bytes and freeing their frames. */
static void collapse_rows(void)
{
    uint32_t collapsed = thp_count(THP_PAGES_COLLAPSED);
    uintptr_t first, i;
    int kept = 1, nomem = 0;

    small_pages(300);
    check(resolve_held(&mm_a, A + 401 * P, 0, LIM) == 1, "a page read, then stored into");
    page_at(&dir_a, A + 401 * P)[1] = 0x5a;
    mmu_user_table(&dir_a, A, 0)[401] |= PTE_DIRTY;
    first = (uintptr_t)PTE_ADDR(pte_of(&dir_a, A));
    check(paging_collapse(&mm_a, A, THP_SCAN) == -EINVAL,
          "khugepaged leaves a range none of whose pages is referenced");
    mmu_user_table(&dir_a, A, 0)[5] |= PTE_ACCESSED;
    thp_policy.max_ptes_none = 100;
    check(paging_collapse(&mm_a, A, THP_SCAN) == -EBUSY,
          "and one with more empty entries than max_ptes_none");
    thp_policy.max_ptes_none = 511;
    check(frame_pin((uintptr_t)PTE_ADDR(pte_of(&dir_a, A + 2 * P))) == 0, "a pin");
    check(paging_collapse(&mm_a, A, THP_SCAN) == -EAGAIN, "and one with a pinned page");
    frame_unpin((uintptr_t)PTE_ADDR(pte_of(&dir_a, A + 2 * P)));
    check(paging_collapse(&mm_a, A, THP_SCAN) == 0 && is_huge(&dir_a, A),
          "khugepaged collapses the range into a huge page");
    for (i = 0; i < HUGE_PAGES; i++) {
        kept &= page_at(&dir_a, A + i * P)[1] == (i < 300    ? (unsigned char)(i + 1)
                                                  : i == 401 ? 0x5a
                                                             : 0);
    }
    check(kept, "each page keeps its bytes, a page read and then stored into among them, and "
                "an empty or unwritten one reads zero");
    check(frame_maps(first / P) == 0 && thp_mapped() == 1 &&
              thp_count(THP_PAGES_COLLAPSED) == collapsed + 1,
          "the small pages' frames are freed, counted as pages_collapsed");
    check(paging_collapse(&mm_a, A, THP_COLLAPSE) == 0, "a huge page is collapsed already");
    drop(&mm_a, A, A + H);

    small_pages(1);
    thp_policy.enabled = THP_NEVER;
    check(paging_collapse(&mm_a, A, THP_COLLAPSE) == 0 && is_huge(&dir_a, A),
          "MADV_COLLAPSE collapses a range whatever enabled says");
    thp_policy.enabled = THP_ALWAYS;
    drop(&mm_a, A, A + H);

    region(&mm_a, A, A + H, RW);
    check(paging_collapse(&mm_a, A, THP_COLLAPSE) == -EINVAL, "an empty range is not collapsed");
    drop(&mm_a, A, A + H);

    small_pages(3);
    mmu_user_table(&dir_a, A, 0)[1] |= PTE_ACCESSED;
    mm_a.thp_next = 0;
    mm_a.thp_pass = 0;
    check(thp_scan_mm(&mm_a, 4096, 7, &nomem) == 1 + HUGE_PAGES && !nomem && is_huge(&dir_a, A),
          "a scan of the space collapses its range");
    check(mm_a.thp_pass == 7 && mm_a.thp_next == 0, "and finishes the space for the pass");
    drop(&mm_a, A, A + H);
}

/** The shrinker splits an underused huge page and frees its pages of
 * zeros, only while max_ptes_none is below 511. */
static void underused_rows(void)
{
    uint32_t underused = thp_count(THP_UNDERUSED_SPLIT_PAGE);
    uintptr_t base;

    region(&mm_a, A, A + H, RW);
    check(resolve_held(&mm_a, A, 1, LIM) == 1 && is_huge(&dir_a, A), "a huge page");
    base = (uintptr_t)PTE_ADDR(mmu_user_huge(&dir_a, A));
    page_at(&dir_a, A + 3 * P)[17] = 1;
    thp_policy.shrink_underused = 1;
    check(huge_shrink_underused(4096) == 0 && is_huge(&dir_a, A),
          "nothing is split while max_ptes_none is 511");
    thp_policy.max_ptes_none = 100;
    check(huge_shrink_underused(4096) == HUGE_PAGES - 1 && mmu_user_huge(&dir_a, A) == 0 &&
              thp_count(THP_UNDERUSED_SPLIT_PAGE) == underused + 1,
          "an underused page is split and its 511 pages of zeros freed");
    check(page_at(&dir_a, A + 3 * P)[17] == 1 && (pte_of(&dir_a, A + 3 * P) & PTE_P) &&
              pte_of(&dir_a, A + 4 * P) == 0 && frame_maps((base + 4 * P) / P) == 0,
          "the page written stays mapped, and the others are gone");
    thp_policy.max_ptes_none = 511;
    check(resolve_held(&mm_a, A + 4 * P, 0, LIM) == 1 && page_at(&dir_a, A + 4 * P)[0] == 0,
          "a page freed reads zero again");
    drop(&mm_a, A, A + H);
}

/** Store `value` at `*p` if `*p` is `old`, and return what was there, as
 * the kernel's exchange of a word as wide as a pointer does. */
uintptr_t arch_atomic_cmpxchg_ptr(volatile uintptr_t *p, uintptr_t old, uintptr_t value)
{
    __atomic_compare_exchange_n(p, &old, value, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return old;
}

/** The protection keys are off in the host test, as on a processor without
 * them. */
int fpu_pkeys(void)
{
    return 0;
}

/** PKRU with the keys off. */
uint32_t fpu_pkru(void)
{
    return 0;
}

/** No region here maps a disk file, whose mapping a pin would hold. */
int filemap_disk_pages(const struct vm_file *f)
{
    (void)f;
    return 0;
}

/** Run every row; 0 when each passes, 1 otherwise. */
int main(void)
{
    model_start(NAME);
    /* The device window is the first 2 MiB, which huge pages never take. */
    _window_frames = HUGE_PAGES;
    _free_window_frames = HUGE_PAGES - USER_FRAME0;
    fault_rows();
    split_rows();
    cow_rows();
    zero_rows();
    collapse_rows();
    underused_rows();
    return model_end();
}
