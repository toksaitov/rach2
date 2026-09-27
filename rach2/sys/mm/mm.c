/*
 * mm.c: address spaces, as mm.h describes them.
 */

#include "mm.h"

#include "kheap.h"
#include "paging.h"
#include "../lib/kstring.h"
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
    kfree(mm->exe);
    kfree(mm);
}

void mm_set_exe(struct mm *mm, const char *path)
{
    uint32_t len = (uint32_t)k_strlen(path);
    char *copy = kmalloc(len + 1);

    if (copy) {
        k_memcpy(copy, path, len + 1);
    }
    kfree(mm->exe);
    mm->exe = copy;
}

void mm_copy_info(struct mm *to, const struct mm *from)
{
    if (from->exe) {
        mm_set_exe(to, from->exe);
    }
    to->arg_start = from->arg_start;
    to->arg_end = from->arg_end;
    to->env_start = from->env_start;
    to->env_end = from->env_end;
    k_memcpy(to->auxv, from->auxv, sizeof(to->auxv));
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
