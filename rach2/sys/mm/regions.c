/*
 * regions.c: the frame of a change of the calling thread's regions, as
 * regions.h describes it.
 */

#include "regions.h"

#include "mm.h"
#include "notify.h"
#include "paging.h"
#include "pressure.h"
#include "../proc/process.h"
#include "../proc/smp.h"

struct mm *regions_begin(void)
{
    struct mm *mm = current_proc->mm;

    mmap_write_lock(mm);
    return mm;
}

void regions_end(struct mm *mm)
{
    mmap_unlock(mm);
    mm_reap();
}

void regions_balance(void)
{
    bkl_enter();
    mem_balance();
    bkl_exit();
}

/** The anonymous mark is taken while the pages still show it
 * (`mm_note_anon`). */
int regions_unmap(struct mm *mm, uintptr_t start, uintptr_t end)
{
    mm_note_anon(mm, start, end);
    if (vm_detach(&mm->vm, start, end) != 0) {
        return -1;
    }
    mmn_notify(mm, MMN_UNMAP, start, end);
    unmap_user_range(mm->dir, start, end);
    vm_release_detached(&mm->vm);
    return 0;
}
