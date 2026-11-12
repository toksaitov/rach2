/*
 * regions: the frame of a change of the calling thread's regions by a call
 * of the mmap family, under the regions' lock alone, as Linux's mmap_lock.
 *
 * A change of the regions, a new mapping, an unmap, a change of protection,
 * a move, advice that drops pages, a lock and a change of the break, holds
 * the address space's regions' lock to write from its first look at the
 * regions to its last change of them (`regions_begin`, `regions_end`), as
 * Linux's brk, mmap, munmap, mprotect, mremap, madvise and mlock take
 * mmap_lock to write (mm/mmap.c:126, mm/util.c:586, mm/mmap.c:1640,
 * mm/mprotect.c:740, mm/mremap.c:1101, mm/madvise.c:1463,
 * mm/mlock.c:632,688,762,780 in Linux 6.12.115). A fault, a copy from or to
 * user memory, a pin and a walk of the regions from /proc hold it to read
 * (`mmap_read_lock`, `mm_read_begin`), as Linux's fault path takes
 * lock_mm_and_find_vma or lock_vma_under_rcu (mm/memory.c:6197,6259), so
 * none of them sees a change half made.
 *
 * The big lock is not held across the change. It is taken only around the
 * steps that reach what has no lock of its own (Locking in
 * sys/proc/Readme.md): the change of entries that takes a page away or a
 * right from it (`unmap_user_range`, `paging_protect_range`,
 * `paging_move_range`), since code under the big lock that reads user
 * memory after a residency check that took no regions' lock expects a page
 * it found to stay until it sleeps (`vm_resident`); the change notice's
 * list and calls (`mmn_notify`); the release of the files whose last region
 * went (`mm_reap`); and the caches' balance (`regions_balance`).
 */
#ifndef _REGIONS_H_
#define _REGIONS_H_

#include "../lib/kint.h"

struct mm;

/** Begin a change of the calling thread's regions: their lock to write,
 * which may sleep. The address space, for `regions_end`. */
struct mm *regions_begin(void);

/** End what `regions_begin` began, and release the files whose last region
 * the change removed (`mm_reap`). */
void regions_end(struct mm *mm);

/** Have the caches give frames back before a call maps memory, under the big
 * lock, which they need, and before the regions' lock is taken, since a
 * write back may wait for a file, whose lock comes first (`mem_balance`). */
void regions_balance(void);

/**
 * Remove [start, end) from the regions of `mm`, whose lock the caller holds
 * to write, and take its pages away: the regions first, so that a fault
 * after the call finds none there, then the change notice, then the entries
 * and the frames (`unmap_user_range`), and last the removed regions' files,
 * which they hold until no processor can reach their pages (`vm_detach`).
 * Nothing is removed when a split finds no room for its region. 0, or -1
 * for no room.
 */
int regions_unmap(struct mm *mm, uintptr_t start, uintptr_t end);

#endif /* _REGIONS_H_ */
