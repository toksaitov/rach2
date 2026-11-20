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
#include "../panic.h"
#include "../proc/ns.h"
#include "../proc/process.h"
#include "../proc/smp.h"
#include "../proc/tgroup.h"

/* The address spaces that keep a program's file, linked by `exe_next`. */
static struct mm *exe_list;

/* The address spaces by their directory, in chains linked by `dir_next`,
 * for `mm_resident_add`, whose callers have the directory alone. `dir_lock`
 * covers the chains; it comes before a space's `rss_lock`. */
#define DIR_CHAINS 64u

static struct mm *dir_table[DIR_CHAINS];
static struct spinlock dir_lock = SPINLOCK_INIT_NAMED("mm_dir_lock");

/** The chain of `dir_table` for the directory `dir`, by its page's number. */
static struct mm **dir_chain(const struct page_dir *dir)
{
    return &dir_table[((uintptr_t)dir / PAGE_SIZE) % DIR_CHAINS];
}

/** Take `mm` off the chain of its directory, before the directory goes. */
static void dir_forget(struct mm *mm)
{
    uint32_t flags = spin_lock_irqsave(&dir_lock);
    struct mm **link = dir_chain(mm->dir);

    while (*link && *link != mm) {
        link = &(*link)->dir_next;
    }
    if (*link) {
        *link = mm->dir_next;
    }
    spin_unlock_irqrestore(&dir_lock, flags);
}

struct mm *mm_new(void)
{
    struct mm *mm = kzalloc(sizeof(*mm));
    uint32_t flags;

    if (!mm) {
        return NULL;
    }
    mm->dir = clone_page_directory();
    if (!mm->dir) {
        kfree(mm);
        return NULL;
    }
    mm->space_root = get_page_dir_phys(mm->dir);
    spin_lock_init(&mm->rss_lock, "mm_rss_lock");
    spin_lock_init(&mm->ptl, "mm_ptl");
    rwsem_init(&mm->mmap_lock, "mmap_lock");
    mm->users = 1;
    mm->holds = 1;
    mm->dumpable = 1;
    mm->pkeys = 1;
    mm->pkey_exec = -1;
    flags = spin_lock_irqsave(&dir_lock);
    mm->dir_next = *dir_chain(mm->dir);
    *dir_chain(mm->dir) = mm;
    spin_unlock_irqrestore(&dir_lock, flags);
    return mm;
}

/** The calling thread's slot of holds of `mm`'s regions' lock, or NULL
 * when it holds none. */
static struct mmap_hold *hold_of(const struct mm *mm)
{
    uint32_t i;

    for (i = 0; i < sizeof(current_proc->mmap_holds) / sizeof(current_proc->mmap_holds[0]); i++) {
        if (current_proc->mmap_holds[i].mm == mm) {
            return &current_proc->mmap_holds[i];
        }
    }
    return NULL;
}

/** Record the calling thread's first hold of `mm`'s regions' lock, a
 * writer's when `write`. A third address space at once is a panic. */
static void hold_add(struct mm *mm, int write)
{
    struct mmap_hold *h = hold_of(NULL);

    if (!h) {
        k_panic("mmap_lock: a thread holds three address spaces' locks");
    }
    h->mm = mm;
    h->depth = 1;
    h->write = (uint32_t)write;
}

void mmap_read_lock(struct mm *mm)
{
    struct mmap_hold *h;

    if (!current_proc) {
        return;
    }
    h = hold_of(mm);
    if (h) {
        h->depth++;
        return;
    }
    down_read(&mm->mmap_lock);
    hold_add(mm, 0);
}

int mmap_read_trylock(struct mm *mm)
{
    struct mmap_hold *h;

    if (!current_proc) {
        return 1;
    }
    h = hold_of(mm);
    if (h) {
        h->depth++;
        return 1;
    }
    if (!down_read_trylock(&mm->mmap_lock)) {
        return 0;
    }
    hold_add(mm, 0);
    return 1;
}

void mmap_write_lock(struct mm *mm)
{
    struct mmap_hold *h;

    if (!current_proc) {
        return;
    }
    h = hold_of(mm);
    if (h && !h->write) {
        k_panic("mmap_lock: a write hold wanted under a read hold");
    }
    if (h) {
        h->depth++;
        return;
    }
    down_write(&mm->mmap_lock);
    hold_add(mm, 1);
}

int mmap_write_lock_killable(struct mm *mm)
{
    struct mmap_hold *h;

    if (!current_proc) {
        return 0;
    }
    h = hold_of(mm);
    if (h && !h->write) {
        k_panic("mmap_lock: a write hold wanted under a read hold");
    }
    if (h) {
        h->depth++;
        return 0;
    }
    if (down_write_killable(&mm->mmap_lock) != 0) {
        return -EINTR;
    }
    hold_add(mm, 1);
    return 0;
}

int mmap_write_trylock(struct mm *mm)
{
    struct mmap_hold *h;

    if (!current_proc) {
        return 1;
    }
    h = hold_of(mm);
    if (h) {
        if (!h->write) {
            return 0;
        }
        h->depth++;
        return 1;
    }
    if (!down_write_trylock(&mm->mmap_lock)) {
        return 0;
    }
    hold_add(mm, 1);
    return 1;
}

void mmap_unlock(struct mm *mm)
{
    struct mmap_hold *h;
    uint32_t write;

    if (!current_proc) {
        return;
    }
    h = hold_of(mm);
    if (!h) {
        k_panic("mmap_lock: a release by a thread that holds no lock");
    }
    if (--h->depth != 0) {
        return;
    }
    write = h->write;
    h->mm = NULL;
    h->write = 0;
    if (write) {
        up_write(&mm->mmap_lock);
    } else {
        up_read(&mm->mmap_lock);
    }
}

void mm_read_begin(struct mm *mm)
{
    mm_get(mm);
    mmap_read_lock(mm);
}

void mm_read_end(struct mm *mm)
{
    mmap_unlock(mm);
    mm_put(mm);
}

int mmap_held(const struct mm *mm, int write)
{
    const struct mmap_hold *h;

    if (!current_proc) {
        return 1;
    }
    h = hold_of(mm);
    return h != NULL && (!write || h->write);
}

int mmap_hold_free(void)
{
    return !current_proc || hold_of(NULL) != NULL;
}

int mmap_may_drop(const struct mm *mm)
{
    const struct mmap_hold *h = current_proc ? hold_of(mm) : NULL;

    return h != NULL && h->depth == 1 && !h->write;
}

uint32_t mm_ptl_lock(struct mm *mm)
{
    return spin_lock_irqsave(&mm->ptl);
}

void mm_ptl_unlock(struct mm *mm, uint32_t flags)
{
    spin_unlock_irqrestore(&mm->ptl, flags);
}

/** A thread holds no regions' lock while it reaps, since a release may
 * write a file back under the file's lock, which comes before them. */
void mm_reap(void)
{
    int taken;

    if (!vm_file_dead()) {
        return;
    }
    if (current_proc && (current_proc->mmap_holds[0].mm || current_proc->mmap_holds[1].mm)) {
        return;
    }
    taken = !bkl_held();
    if (taken) {
        bkl_enter();
    }
    vm_file_reap();
    if (taken) {
        bkl_exit();
    }
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
    struct page_dir *dir;

    if (!mm || --mm->users != 0) {
        return;
    }
    mmn_release(mm);
    arch_mm_release(mm);
    dir = mm->dir;
    smp_space_release(dir);
    mm_resident_add(dir, -(long)mm->resident);
    dir_forget(mm);
    mm->dir = NULL;
    mm->space_root = 0;
    free_user_pages(dir);
    free_page_directory(dir);
    vm_free(&mm->vm);
    if (mm->interp_file) {
        struct open_file *ofd = mm->interp_file;
        struct vfs_mount *locked = fs_exe_lock(ofd);

        mm->interp_file = NULL;
        fs_exe_close(ofd, locked);
    }
    mm_exe_close(mm);
    mm_reap();
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
    ns_userns_put(mm->userns);
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
    to->userns = ns_userns_get(from->userns);
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
    to->start_code = from->start_code;
    to->end_code = from->end_code;
    to->start_data = from->start_data;
    to->end_data = from->end_data;
    to->start_brk = from->start_brk;
    to->start_stack = from->start_stack;
    k_memcpy(to->auxv, from->auxv, sizeof(to->auxv));
    to->dumpable = from->dumpable;
    to->membarrier = from->membarrier;
    to->thp_disable = from->thp_disable;
    to->compat = from->compat;
    to->vdso = from->vdso;
    to->pkeys = from->pkeys;
    to->pkey_exec = from->pkey_exec;
}

unsigned long mm_resident(const struct mm *mm)
{
    return mm->resident;
}

/** Each word is read once, without the lock, as Linux's
 * get_mm_hiwater_rss reads its counts: the peak never falls, so the larger
 * of the two is one the space has had. */
unsigned long mm_resident_peak(const struct mm *mm)
{
    unsigned long now = mm->resident;
    unsigned long peak = mm->resident_peak;

    return now > peak ? now : peak;
}

/** Count `pages` more, or fewer, present pages in `mm`; its `rss_lock` is
 * held. The peak is kept when pages go, as Linux's update_hiwater_rss. */
static void rss_add(struct mm *mm, long pages)
{
    if (pages > 0) {
        mm->resident += (unsigned long)pages;
        return;
    }
    if (mm->resident > mm->resident_peak) {
        mm->resident_peak = mm->resident;
    }
    mm->resident -= (unsigned long)-pages < mm->resident ? (unsigned long)-pages : mm->resident;
}

/**
 * The running thread's space is found without the table, since it cannot go
 * while the thread runs; another is found under `dir_lock`, which is held
 * across the count so that the space found is not freed meanwhile.
 */
void mm_resident_add(const struct page_dir *dir, long pages)
{
    struct mm *mm = current_proc ? current_proc->mm : NULL;
    uint32_t flags;

    if (pages == 0) {
        return;
    }
    if (mm && mm->dir == dir) {
        flags = spin_lock_irqsave(&mm->rss_lock);
        rss_add(mm, pages);
        spin_unlock_irqrestore(&mm->rss_lock, flags);
        return;
    }
    flags = spin_lock_irqsave(&dir_lock);
    mm = *dir_chain(dir);
    while (mm && mm->dir != dir) {
        mm = mm->dir_next;
    }
    if (mm) {
        spin_lock(&mm->rss_lock);
        rss_add(mm, pages);
        spin_unlock(&mm->rss_lock);
    }
    spin_unlock_irqrestore(&dir_lock, flags);
}

void mm_note_anon(struct mm *mm, uintptr_t start, uintptr_t end)
{
    struct paging_frame_counts counts;
    uintptr_t lo = start >= PAGE_SIZE ? start - PAGE_SIZE : 0;
    uintptr_t hi = end <= ~(uintptr_t)0 - PAGE_SIZE ? end + PAGE_SIZE : end;
    vma_t *r;

    for (r = vm_lower(&mm->vm, lo); r && r->start < hi; r = vm_next(r)) {
        if (r->anon || r->type == VM_SHARED || (r->prot & VM_PROT_IO)) {
            continue;
        }
        paging_user_frames(mm->dir, r->start, r->end, &counts);
        if ((r->file ? counts.anon : counts.frames) > 0) {
            vm_anon_prepare(&mm->vm, r);
        }
    }
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
