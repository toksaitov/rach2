/*
 * notify: callbacks on a range of a process's memory, called when what the
 * range maps changes under a driver that keeps its own view of the pages,
 * as Linux's MMU notifiers are. A driver that pins a process's pages for a
 * device, maps them into a GPU's or a guest's page tables, or caches their
 * physical addresses registers the range, and the memory manager calls it
 * before the pages there go or change: an unmap, a change of protection, a
 * copy-on-write break, a page `madvise` drops, a truncation or revocation
 * of the object behind the pages, and the end of the address space.
 *
 * The record is the caller's, embedded in its own, so a registration cannot
 * fail. The list is the big kernel lock's: a registration and its end are
 * made under it, and so is every call, which `mmn_notify` takes the lock
 * for when its caller runs without it; a list change is made under the
 * address space's page-table lock too. A callback must not sleep or give
 * way (`sched_give_way`): the memory manager calls it between a change to
 * the regions and the change to the page tables, and either would let the
 * big lock go and another thread fault in a page of a region already
 * gone. A fault that changes a page already present, a copy-on-write
 * break, holds the big lock from the call to the change of the entry
 * whenever the address space has a range, so that a driver that reads an
 * entry under the big lock and maps what it found sees the page before the
 * change or hears of it after.
 */
#ifndef _NOTIFY_H_
#define _NOTIFY_H_

#include "../lib/kint.h"

struct mm;
struct mmn_range;

/* What happened to [start, end) of a registered range, the event given to
 * `invalidate`. */
#define MMN_UNMAP    1 /* munmap, mremap, brk, a MAP_FIXED mapping over it, shmdt */
#define MMN_PROTECT  2 /* mprotect changed the access the pages allow */
#define MMN_COW      3 /* a private page was given a new frame, a copy of the old */
#define MMN_DROP     4 /* madvise let the pages go; the next touch backs them anew */
#define MMN_TRUNCATE 5 /* the file or device object behind the pages took them back */
#define MMN_RELEASE  6 /* the address space ends; the range is unregistered first */

/*
 * What a registered range does. `invalidate` is called before the pages of
 * [start, end), the part of the range the event touches, change: the driver
 * stops the device's use of them and forgets their frames. It must not
 * sleep, and may unregister its own range, and no other.
 */
struct mmn_ops {
    void (*invalidate)(struct mmn_range *r, int event, uintptr_t start, uintptr_t end);
};

/*
 * A registered range, [start, end) of the address space `mm`, page-aligned.
 * An empty range, start equal to end, is called at MMN_RELEASE alone, for a
 * driver that wants to know only when the address space ends. `seq` counts
 * the invalidations the range has had, as the sequence of Linux's interval
 * notifier: a driver reads it before it looks the pages up and again before
 * it uses what it found, and looks again when the two differ.
 */
struct mmn_range {
    struct mm *mm; /* NULL when the range is not registered */
    uintptr_t start;
    uintptr_t end;
    const struct mmn_ops *ops;
    void *owner;            /* the driver's, for `invalidate` */
    struct mmn_range *next; /* the next range of `mm` */
    uint32_t seq;
};

/** Register `r` on [start, end) of `mm`, with `ops` and `owner`; `r` must
 * not be registered already. */
void mmn_register(struct mm *mm, struct mmn_range *r, uintptr_t start, uintptr_t end,
                  const struct mmn_ops *ops, void *owner);

/** Take `r` off its address space's list; nothing for a range that is not
 * registered, which includes one MMN_RELEASE took off. */
void mmn_unregister(struct mmn_range *r);

/**
 * Tell each range of `mm` that meets [start, end) of the event `event`, one
 * of MMN_UNMAP to MMN_TRUNCATE, with the part they share. Nothing for a
 * NULL `mm` or an empty range. The memory manager calls it before the
 * change, with the regions or the page tables still as they were.
 */
void mmn_notify(struct mm *mm, int event, uintptr_t start, uintptr_t end);

/**
 * Unregister every range of `mm` and call each with MMN_RELEASE and its
 * whole range, for the last put of the address space, before its pages go.
 * A callback may unregister its range again, which does nothing.
 */
void mmn_release(struct mm *mm);

#endif /* _NOTIFY_H_ */
