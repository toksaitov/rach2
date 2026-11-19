/*
 * thp.c: the policy, the counters and khugepaged's scan of transparent huge
 * pages, as thp.h describes. The pages are paging.c's and khugepaged's
 * thread khugepaged.c's.
 */

#include "thp.h"
#include "mm.h"
#include "paging.h"
#include "vm.h"

#include "../lib/kerrno.h"
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

int thp_region_eligible(const struct mm *mm, const struct vma *vma)
{
#if MMU_HUGE
    uintptr_t addr = (vma->start + HUGE_SPAN - 1) & ~(uintptr_t)(HUGE_SPAN - 1);

    return addr >= vma->start && addr < vma->end && thp_region_allows(mm, vma, addr, THP_FAULT);
#else
    (void)mm;
    (void)vma;
    return 0;
#endif
}

uint32_t thp_scan_mm(struct mm *mm, uint32_t budget, uint32_t pass, int *nomem)
{
#if MMU_HUGE
    const vma_t *r;
    uintptr_t addr;
    uint32_t done = 0;

    for (r = vm_lower(&mm->vm, mm->thp_next); r && done < budget; r = vm_next(r)) {
        addr = r->start > mm->thp_next ? r->start : mm->thp_next;
        addr = (addr + HUGE_SPAN - 1) & ~(uintptr_t)(HUGE_SPAN - 1);
        done++;
        if (addr < r->start || addr >= r->end || r->end - addr < HUGE_SPAN ||
            !thp_region_allows(mm, r, addr, THP_SCAN)) {
            mm->thp_next = r->end;
            continue;
        }
        for (; r->end - addr >= HUGE_SPAN && done < budget; addr += HUGE_SPAN) {
            mm->thp_next = addr + HUGE_SPAN;
            done += HUGE_SPAN / PAGE_SIZE;
            if (paging_collapse(mm, addr, THP_SCAN) == -ENOMEM) {
                *nomem = 1;
                return done;
            }
            if (addr + HUGE_SPAN < addr) {
                break;
            }
        }
        if (r->end - addr < HUGE_SPAN) {
            mm->thp_next = r->end;
        }
    }
    if (!r && done < budget) {
        mm->thp_pass = pass;
        mm->thp_next = 0;
    }
    return done;
#else
    (void)mm;
    (void)budget;
    (void)pass;
    (void)nomem;
    return 0;
#endif
}
