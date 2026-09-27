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
    mm->users = 1;
    mm->holds = 1;
    mm->dumpable = 1;
    return mm;
}

void mm_get(struct mm *mm)
{
    mm->users++;
}

int mm_get_live(struct mm *mm)
{
    if (mm->users == 0) {
        return 0;
    }
    mm->users++;
    return 1;
}

void mm_put(struct mm *mm)
{
    if (!mm || --mm->users != 0) {
        return;
    }
    free_user_pages(mm->dir);
    free_page_directory(mm->dir);
    mm->dir = NULL;
    mm->cr3 = 0;
    vm_free(&mm->vm);
    vm_file_reap();
    mm_unhold(mm);
}

void mm_hold(struct mm *mm)
{
    mm->holds++;
}

void mm_unhold(struct mm *mm)
{
    if (!mm || --mm->holds != 0) {
        return;
    }
    kfree(mm->exe);
    kfree(mm);
}

void mm_set_exe(struct mm *mm, const char *path)
{
    uint32_t len = (uint32_t)k_strlen(path);
    uint32_t slash = path[0] != '/' ? 1u : 0u;
    char *copy = kmalloc(len + slash + 1);

    if (copy) {
        copy[0] = '/';
        k_memcpy(copy + slash, path, len + 1);
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
    to->dumpable = from->dumpable;
}

void mm_drop(struct process *p)
{
    struct mm *mm = p->mm;

    if (!mm) {
        return;
    }
    if (mm->users == 1 && _curr_dir == mm->dir) {
        switch_page_directory(_kernel_dir);
    }
    p->mm = NULL;
    p->cr3 = 0;
    p->page_dir = NULL;
    mm_put(mm);
}
