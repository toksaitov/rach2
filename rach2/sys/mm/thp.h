/*
 * thp: the policy, the counters and the scanner of transparent huge pages,
 * the 2 MiB pages of anonymous memory that x86-64's page directory maps
 * (paging.c, MMU_HUGE), as Linux 6.12's mm/huge_memory.c and
 * mm/khugepaged.c give them.
 */
#ifndef _THP_H_
#define _THP_H_

#include "../lib/kint.h"

struct mm;
struct vma;

/* The words of /sys/kernel/mm/transparent_hugepage's `enabled`, as indexes. */
#define THP_ALWAYS  0
#define THP_MADVISE 1
#define THP_NEVER   2

/* The words of its `defrag`, as indexes. */
#define THP_DEFRAG_ALWAYS        0
#define THP_DEFRAG_DEFER         1
#define THP_DEFRAG_DEFER_MADVISE 2
#define THP_DEFRAG_MADVISE       3
#define THP_DEFRAG_NEVER         4

/*
 * The policy, which root's writes to /sys/kernel/mm/transparent_hugepage
 * and its khugepaged directory change (fs/procfs.c), with Linux 6.12's
 * defaults: `enabled` madvise, as Debian 13's kernel is built, so that a
 * region has huge pages when its program asks with MADV_HUGEPAGE, as QEMU
 * asks for a guest's memory; `defrag` madvise; the huge zero page on; and
 * khugepaged scanning 4096 pages every 10 seconds, waiting a minute after
 * a huge page could not be had, and collapsing a range that has any page
 * present (max_ptes_none 511), at most 256 of them shared and 64 swapped.
 * The big lock covers a change; a reader takes each word as it stands.
 */
struct thp_policy {
    int enabled;              /* THP_ALWAYS, THP_MADVISE or THP_NEVER */
    int defrag;               /* THP_DEFRAG_ */
    int shmem;                /* shmem_enabled's index; tmpfs has no huge pages */
    int zero_page;            /* use_zero_page */
    int shrink_underused;     /* shrink_underused */
    int scan_defrag;          /* khugepaged/defrag */
    uint32_t pages_to_scan;   /* khugepaged/pages_to_scan, never 0 */
    uint32_t scan_sleep_ms;   /* khugepaged/scan_sleep_millisecs */
    uint32_t alloc_sleep_ms;  /* khugepaged/alloc_sleep_millisecs */
    uint32_t max_ptes_none;   /* khugepaged/max_ptes_none, at most 511 */
    uint32_t max_ptes_swap;   /* khugepaged/max_ptes_swap, at most 511 */
    uint32_t max_ptes_shared; /* khugepaged/max_ptes_shared, at most 511 */
};

extern struct thp_policy thp_policy;

/* The events /proc/vmstat counts under Linux's names (`thp_note`). */
#define THP_FAULT_ALLOC            0  /* thp_fault_alloc */
#define THP_FAULT_FALLBACK         1  /* thp_fault_fallback */
#define THP_COLLAPSE_ALLOC         2  /* thp_collapse_alloc */
#define THP_COLLAPSE_ALLOC_FAILED  3  /* thp_collapse_alloc_failed */
#define THP_SPLIT_PAGE             4  /* thp_split_page */
#define THP_SPLIT_PMD              5  /* thp_split_pmd */
#define THP_UNDERUSED_SPLIT_PAGE   6  /* thp_underused_split_page */
#define THP_ZERO_PAGE_ALLOC        7  /* thp_zero_page_alloc */
#define THP_ZERO_PAGE_ALLOC_FAILED 8  /* thp_zero_page_alloc_failed */
#define THP_PAGES_COLLAPSED        9  /* khugepaged/pages_collapsed */
#define THP_FULL_SCANS             10 /* khugepaged/full_scans */
#define THP_EVENTS                 11

/** Count one `event`, a THP_ number. */
void thp_note(int event);

/** The count of `event` since the boot. */
uint32_t thp_count(int event);

/** Add `n`, which may be negative, to the huge pages mapped
 * (`paging_huge_mapped`). */
void thp_mapped_add(int n);

/** The huge pages that some directory entry maps whole, each counted once
 * however many address spaces map it, the huge zero page aside:
 * AnonHugePages of /proc/meminfo, nr_anon_transparent_hugepages of
 * /proc/vmstat. */
uint32_t thp_mapped(void);

/* Who asks `thp_region_allows`. */
#define THP_FAULT    0 /* a page fault, which the policy binds */
#define THP_SCAN     1 /* khugepaged, which the policy binds */
#define THP_COLLAPSE 2 /* MADV_COLLAPSE, which `enabled` and `defrag` do not bind */

/**
 * Whether the 2 MiB of the region `vma` of `mm` from the 2 MiB boundary
 * `start` may be one huge page for `how`, as Linux 6.12's
 * thp_vma_allowable_order asks: the 2 MiB lie inside the region, which is
 * private anonymous memory with some access, not device memory, the vDSO's
 * or memfd_secret's, and not marked MADV_NOHUGEPAGE; the address space has
 * no PR_SET_THP_DISABLE; and, but for MADV_COLLAPSE, `enabled` is always,
 * or madvise and the region is marked MADV_HUGEPAGE. Always 0 where the
 * page directory maps no huge page (MMU_HUGE 0).
 */
int thp_region_allows(const struct mm *mm, const struct vma *vma, uintptr_t start, int how);

/** Whether some 2 MiB of the region `vma` of `mm` may be a huge page at a
 * fault (`thp_region_allows`, THP_FAULT): THPeligible of its smaps. */
int thp_region_eligible(const struct mm *mm, const struct vma *vma);

/**
 * Scan `mm` from where khugepaged last stopped in it (`thp_next`) for at most
 * `budget` pages, as Linux's hpage_collapse_scan_mm_slot: each 2 MiB of a
 * region that allows huge pages to khugepaged counts 512 and is collapsed where
 * it can be (`paging_collapse`), one at least however small the budget, and
 * each other region counts one; the budget is judged after each 2 MiB alone,
 * so regions that allow none are passed over however many there are. Once
 * its last region is scanned the space has
 * finished the pass numbered `pass`, which its `thp_pass` records. The caller
 * holds the regions' lock to write. The pages counted; `*nomem` is set when no
 * huge page could be had, after which khugepaged sleeps for
 * `alloc_sleep_millisecs`. Nothing where the page directory maps no huge page
 * (MMU_HUGE 0).
 */
uint32_t thp_scan_mm(struct mm *mm, uint32_t budget, uint32_t pass, int *nomem);

/** Set the policy for the machine's memory and processor at boot, once the
 * frame allocator has counted it. */
void thp_boot(void);

/**
 * Refuse every huge page from now on, for a processor that fails with them
 * or a kernel with no memory to count them: `thp_region_allows` answers 0
 * whoever asks.
 */
void thp_refuse(void);

/**
 * Register the shrinker of huge pages and start khugepaged, the kernel
 * thread that collapses the small pages of eligible ranges into huge pages
 * (khugepaged.c), once, at boot. No thread where the page directory maps
 * no huge page.
 */
void thp_init(void);

/** Wake khugepaged for a change of its policy, as Linux's sysfs stores
 * wake it. */
void thp_policy_changed(void);

#endif /* _THP_H_ */
