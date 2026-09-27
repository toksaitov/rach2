/*
 * mm.c: address spaces, as mm.h describes them.
 */

#include "mm.h"

#include "kheap.h"
#include "paging.h"
#include "../proc/process.h"

struct mm *mm_new(void)
{
    struct mm *mm = kzalloc(sizeof(*mm));

    if (!mm) {
        return NULL;
    }
    mm->dir = clone_page_directory();
    if (!mm->dir) {
        kfree(mm);
        return NULL;
    }
    mm->cr3 = get_page_dir_phys(mm->dir);
    mm->refs = 1;
    return mm;
}

void mm_get(struct mm *mm)
{
    mm->refs++;
}

void mm_put(struct mm *mm)
{
    if (!mm || --mm->refs != 0) {
        return;
    }
    free_user_pages(mm->dir);
    free_page_directory(mm->dir);
    vm_free(&mm->vm);
    kfree(mm);
}

void mm_drop(struct process *p)
{
    struct mm *mm = p->mm;

    if (!mm) {
        return;
    }
    if (mm->refs == 1 && _curr_dir == mm->dir) {
        switch_page_directory(_kernel_dir);
    }
    p->mm = NULL;
    p->cr3 = 0;
    p->page_dir = NULL;
    mm_put(mm);
}
