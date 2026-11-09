/*
 * notify.c: the ranges an address space tells of changes to its pages, as
 * notify.h says.
 */

#include "notify.h"

#include "mm.h"

void mmn_register(struct mm *mm, struct mmn_range *r, uintptr_t start, uintptr_t end,
                  const struct mmn_ops *ops, void *owner)
{
    r->mm = mm;
    r->start = start;
    r->end = end;
    r->ops = ops;
    r->owner = owner;
    r->seq = 0;
    r->next = mm->notify;
    mm->notify = r;
}

void mmn_unregister(struct mmn_range *r)
{
    struct mmn_range **link;

    if (!r->mm) {
        return;
    }
    for (link = &r->mm->notify; *link; link = &(*link)->next) {
        if (*link == r) {
            *link = r->next;
            break;
        }
    }
    r->mm = NULL;
    r->next = NULL;
}

/** The next range is read before the call, since a callback may unregister
 * its own range, which clears that range's link. */
void mmn_notify(struct mm *mm, int event, uintptr_t start, uintptr_t end)
{
    struct mmn_range *r, *next;

    if (!mm || start >= end) {
        return;
    }
    for (r = mm->notify; r; r = next) {
        next = r->next;
        if (r->start < end && start < r->end) {
            r->seq++;
            r->ops->invalidate(r, event, r->start > start ? r->start : start,
                               r->end < end ? r->end : end);
        }
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
