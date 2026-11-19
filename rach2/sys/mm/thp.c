/*
 * thp.c: the policy, the counters and khugepaged of transparent huge pages,
 * as thp.h describes. The pages themselves are paging.c's.
 */

#include "thp.h"
#include "mm.h"
#include "paging.h"
#include "vm.h"

#include "../lib/spinlock.h"

struct thp_policy thp_policy = {
    THP_MADVISE, THP_DEFRAG_MADVISE, 1, 1, 1, 1, 4096u, 10000u, 60000u, 511u, 64u, 256u};

/* The counts of thp_note's events and of the huge pages mapped, under
 * `thp_count_lock`, which nothing is taken under. */
static uint32_t _events[THP_EVENTS];
static uint32_t _mapped;
static struct spinlock thp_count_lock = SPINLOCK_INIT_NAMED("thp_count_lock");

void thp_note(int event)
{
    uint32_t flags;

    if (event < 0 || event >= THP_EVENTS) {
        return;
    }
    flags = spin_lock_irqsave(&thp_count_lock);
    _events[event]++;
    spin_unlock_irqrestore(&thp_count_lock, flags);
}

uint32_t thp_count(int event)
{
    return event >= 0 && event < THP_EVENTS ? _events[event] : 0;
}

void thp_mapped_add(int n)
{
    uint32_t flags = spin_lock_irqsave(&thp_count_lock);

    _mapped += (uint32_t)n;
    spin_unlock_irqrestore(&thp_count_lock, flags);
}

uint32_t thp_mapped(void)
{
    return _mapped;
}

/**
 * Linux's hugepage_init turns every flag off on a machine of less than
 * 512 MiB, where a huge page is too large a share of memory: `enabled` and
 * `defrag` read never and the huge zero page is off.
 */
void thp_boot(void)
{
    if (frame_total_count() < (512u << 20) / PAGE_SIZE) {
        thp_policy.enabled = THP_NEVER;
        thp_policy.defrag = THP_DEFRAG_NEVER;
        thp_policy.zero_page = 0;
    }
}

int thp_region_allows(const struct mm *mm, const struct vma *vma, uintptr_t start, int how)
{
#if MMU_HUGE
    if (!mm || !vma || vma->file || vma->type == VM_SHARED || mm->thp_disable ||
        !(vma->prot & (VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXEC)) ||
        (vma->prot & (VM_PROT_IO | VM_PROT_VVAR | VM_PROT_SECRET | VM_PROT_NOHUGEPAGE)) ||
        (start & (HUGE_SPAN - 1)) != 0 || start < vma->start || vma->end - start < HUGE_SPAN) {
        return 0;
    }
    if (how == THP_COLLAPSE) {
        return 1;
    }
    return thp_policy.enabled == THP_ALWAYS ||
           (thp_policy.enabled == THP_MADVISE && (vma->prot & VM_PROT_HUGEPAGE));
#else
    (void)mm;
    (void)vma;
    (void)start;
    (void)how;
    return 0;
#endif
}

void thp_init(void)
{
    paging_thp_init();
}

void thp_policy_changed(void)
{}
