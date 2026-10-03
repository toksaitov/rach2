/*
 * mm.c: address spaces, as mm.h describes them.
 */

#include "mm.h"

#include "kheap.h"
#include "paging.h"
#include "../fs/fd.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../proc/process.h"
#include "../proc/tgroup.h"

/* The address spaces that keep a program's file, linked by `exe_next`. */
static struct mm *exe_list;

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
    mm->space_root = get_page_dir_phys(mm->dir);
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
    mm->space_root = 0;
    vm_free(&mm->vm);
    mm_exe_close(mm);
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
    kfree(mm->interp);
    kfree(mm);
}

/** A copy of `path` from the heap, given a leading slash where it has none,
 * since /proc gives absolute paths; NULL when the heap has no room. */
static char *path_copy(const char *path)
{
    uint32_t len = (uint32_t)k_strlen(path);
    uint32_t slash = path[0] != '/' ? 1u : 0u;
    char *copy = kmalloc(len + slash + 1);

    if (copy) {
        copy[0] = '/';
        k_memcpy(copy + slash, path, len + 1);
    }
    return copy;
}

void mm_set_exe(struct mm *mm, const char *path)
{
    char *copy = path_copy(path);

    kfree(mm->exe);
    mm->exe = copy;
}

void mm_set_interp(struct mm *mm, const char *path)
{
    char *copy = path_copy(path);

    kfree(mm->interp);
    mm->interp = copy;
}

/** Put `mm`, which keeps the description `ofd` of its program's file from
 * now on, on the list `mm_exe_holds` reads. */
static void exe_list_add(struct mm *mm, struct open_file *ofd)
{
    mm->exe_file = ofd;
    mm->exe_next = exe_list;
    exe_list = mm;
}

int mm_exe_open(struct mm *mm, const struct vfs_ref *ref)
{
    struct open_file *ofd = fs_exe_open(ref);

    if (!ofd) {
        return -ENOMEM;
    }
    exe_list_add(mm, ofd);
    return 0;
}

void mm_exe_close(struct mm *mm)
{
    struct open_file *ofd = mm->exe_file;
    struct mm **link = &exe_list;
    struct vfs_mount *locked;

    if (!ofd) {
        return;
    }
    locked = fs_exe_lock(ofd);
    while (*link && *link != mm) {
        link = &(*link)->exe_next;
    }
    if (*link) {
        *link = mm->exe_next;
    }
    mm->exe_file = NULL;
    mm->exe_next = NULL;
    fs_exe_close(ofd, locked);
}

int mm_exe_holds(const struct vfs_mount *mnt, const void *node)
{
    const struct mm *m;

    for (m = exe_list; m; m = m->exe_next) {
        if (node && m->exe_file->vmnt == mnt && m->exe_file->vnode == node) {
            return 1;
        }
    }
    return 0;
}

void mm_copy_info(struct mm *to, const struct mm *from)
{
    if (from->exe) {
        mm_set_exe(to, from->exe);
    }
    if (from->interp) {
        mm_set_interp(to, from->interp);
    }
    if (from->exe_file) {
        fd_ofd_ref(from->exe_file);
        exe_list_add(to, from->exe_file);
    }
    to->arg_start = from->arg_start;
    to->arg_end = from->arg_end;
    to->env_start = from->env_start;
    to->env_end = from->env_end;
    k_memcpy(to->auxv, from->auxv, sizeof(to->auxv));
    to->dumpable = from->dumpable;
}

uint32_t mm_resident(const struct mm *mm)
{
    uint32_t n = 0;
    uint32_t i;

    for (i = 0; i < mm->vm.count; i++) {
        uintptr_t a;

        for (a = mm->vm.vmas[i].start; a < mm->vm.vmas[i].end; a += PAGE_SIZE) {
            if (paging_user_phys(mm->dir, a) != 0) {
                n++;
            }
        }
    }
    return n;
}

void mm_drop(struct process *p)
{
    struct mm *mm = p->mm;

    if (!mm) {
        return;
    }
    if (mm->users == 1 && p->tg) {
        uint32_t kb = mm_resident(mm) * (PAGE_SIZE / 1024u);

        if (kb > p->tg->maxrss_kb) {
            p->tg->maxrss_kb = kb;
        }
    }
    if (mm->users == 1 && _curr_dir == mm->dir) {
        switch_page_directory(_kernel_dir);
    }
    p->mm = NULL;
    p->space_root = 0;
    p->page_dir = NULL;
    mm_put(mm);
}
