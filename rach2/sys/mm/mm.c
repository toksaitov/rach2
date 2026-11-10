/*
 * mm.c: address spaces, as mm.h describes them.
 */

#include "mm.h"

#include "kheap.h"
#include "notify.h"
#include "paging.h"
#include "../fs/fd.h"
#include "../fs/vfs.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../proc/process.h"
#include "../proc/smp.h"
#include "../proc/tgroup.h"

/* The address spaces that keep a program's file, linked by `exe_next`. */
static struct mm *exe_list;

/* The address spaces by their directory, in chains linked by `dir_next`,
 * for `mm_resident_add`, whose callers have the directory alone. */
#define DIR_CHAINS 64u

static struct mm *dir_table[DIR_CHAINS];

/** The chain of `dir_table` for the directory `dir`, by its page's number. */
static struct mm **dir_chain(const struct page_dir *dir)
{
    return &dir_table[((uintptr_t)dir / PAGE_SIZE) % DIR_CHAINS];
}

/** Take `mm` off the chain of its directory, before the directory goes. */
static void dir_forget(struct mm *mm)
{
    struct mm **link = dir_chain(mm->dir);

    while (*link && *link != mm) {
        link = &(*link)->dir_next;
    }
    if (*link) {
        *link = mm->dir_next;
    }
}

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
    mm->pkeys = 1;
    mm->pkey_exec = -1;
    mm->dir_next = *dir_chain(mm->dir);
    *dir_chain(mm->dir) = mm;
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
    mmn_release(mm);
    arch_mm_release(mm);
    smp_space_release(mm->dir);
    free_user_pages(mm->dir);
    mm_resident_add(mm->dir, -(long)mm->resident);
    dir_forget(mm);
    free_page_directory(mm->dir);
    mm->dir = NULL;
    mm->space_root = 0;
    vm_free(&mm->vm);
    if (mm->interp_file) {
        struct open_file *ofd = mm->interp_file;
        struct vfs_mount *locked = fs_exe_lock(ofd);

        mm->interp_file = NULL;
        fs_exe_close(ofd, locked);
    }
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
    {
        int rc = fd_ofd_access(ofd, ref, 2);

        if (rc != 0) {
            fs_exe_close(ofd, fs_exe_lock(ofd));
            return rc;
        }
    }
    if (mm->exe_file) {
        mm->interp_file = ofd;
    } else {
        exe_list_add(mm, ofd);
    }
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
        if (mnt && node &&
            ((vfs_mount_base(m->exe_file->vmnt) == vfs_mount_base(mnt) &&
              m->exe_file->vnode == node) ||
             (m->interp_file && vfs_mount_base(m->interp_file->vmnt) == vfs_mount_base(mnt) &&
              m->interp_file->vnode == node))) {
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
    if (from->interp_file) {
        fd_ofd_ref(from->interp_file);
        to->interp_file = from->interp_file;
    }
    to->arg_start = from->arg_start;
    to->arg_end = from->arg_end;
    to->env_start = from->env_start;
    to->env_end = from->env_end;
    k_memcpy(to->auxv, from->auxv, sizeof(to->auxv));
    to->dumpable = from->dumpable;
    to->membarrier = from->membarrier;
    to->compat = from->compat;
    to->vdso = from->vdso;
    to->pkeys = from->pkeys;
    to->pkey_exec = from->pkey_exec;
}

unsigned long mm_resident(const struct mm *mm)
{
    return mm->resident;
}

unsigned long mm_resident_peak(const struct mm *mm)
{
    return mm->resident > mm->resident_peak ? mm->resident : mm->resident_peak;
}

void mm_resident_add(const struct page_dir *dir, long pages)
{
    struct mm *mm = current_proc ? current_proc->mm : NULL;

    if (pages == 0) {
        return;
    }
    if (!mm || mm->dir != dir) {
        mm = *dir_chain(dir);
        while (mm && mm->dir != dir) {
            mm = mm->dir_next;
        }
        if (!mm) {
            return;
        }
    }
    if (pages > 0) {
        mm->resident += (unsigned long)pages;
        return;
    }
    if (mm->resident > mm->resident_peak) {
        mm->resident_peak = mm->resident;
    }
    mm->resident -= (unsigned long)-pages < mm->resident ? (unsigned long)-pages : mm->resident;
}

void mm_drop(struct process *p)
{
    struct mm *mm = p->mm;

    vm_policy_put(p->mempolicy);
    p->mempolicy = NULL;
    if (!mm) {
        return;
    }
    if (mm->users == 1 && p->tg) {
        unsigned long kb = mm_resident_peak(mm) * (PAGE_SIZE / 1024u);

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
