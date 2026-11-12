/*
 * notify.c: the ranges an address space tells of changes to its pages, as
 * notify.h says.
 */

#include "notify.h"

#include "mm.h"
#include "../arch/arch.h"
#include "../proc/smp.h"

/** Add `delta` to `*count` in one atomic step, a full barrier on x86. */
static void count_add(volatile uint32_t *count, uint32_t delta)
{
    uint32_t old;

    do {
        old = *count;
    } while (arch_atomic_cmpxchg((volatile unsigned int *)count, old, old + delta) != old);
}

void mmn_change_begin(struct mm *mm)
{
    count_add(&mm->changing, 1u);
}

void mmn_change_end(struct mm *mm)
{
    count_add(&mm->changes, 1u);
    count_add(&mm->changing, (uint32_t)-1);
}

/** The list is changed under the page-table lock too, so that a fault that
 * changes a present page reads it in the same hold as it changes the entry
 * (`page_change_begin` in paging.c). */
void mmn_register(struct mm *mm, struct mmn_range *r, uintptr_t start, uintptr_t end,
                  const struct mmn_ops *ops, void *owner)
{
    uint32_t flags;

    r->mm = mm;
    r->start = start;
    r->end = end;
    r->ops = ops;
    r->owner = owner;
    r->seq = 0;
    flags = mm_ptl_lock(mm);
    r->next = mm->notify;
    mm->notify = r;
    mm_ptl_unlock(mm, flags);
}

void mmn_unregister(struct mmn_range *r)
{
    struct mmn_range **link;
    uint32_t flags;

    if (!r->mm) {
        return;
    }
    flags = mm_ptl_lock(r->mm);
    for (link = &r->mm->notify; *link; link = &(*link)->next) {
        if (*link == r) {
            *link = r->next;
            break;
        }
    }
    mm_ptl_unlock(r->mm, flags);
    r->mm = NULL;
    r->next = NULL;
}

/** The next range is read before the call, since a callback may unregister
 * its own range, which clears that range's link. The walk and the calls
 * are made under the big lock, taken when the caller runs without it. */
void mmn_notify(struct mm *mm, int event, uintptr_t start, uintptr_t end)
{
    struct mmn_range *r, *next;
    int taken;

    if (!mm || start >= end || !mm->notify) {
        return;
    }
    taken = !bkl_held();
    if (taken) {
        bkl_enter();
    }
    for (r = mm->notify; r; r = next) {
        next = r->next;
        if (r->start < end && start < r->end) {
            r->seq++;
            r->ops->invalidate(r, event, r->start > start ? r->start : start,
                               r->end < end ? r->end : end);
        }
    }
    if (taken) {
        bkl_exit();
    }
}

void mmn_release(struct mm *mm)
{
    struct mmn_range *r;

    while ((r = mm->notify) != NULL) {
        mm->notify = r->next;
        r->mm = NULL;
        r->next = NULL;
        r->seq++;
        r->ops->invalidate(r, MMN_RELEASE, r->start, r->end);
    }
}
