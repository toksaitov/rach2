/*
 * synth_attr.c: the attributes chmod, chown and utimensat give the nodes of
 * /sys, /proc and /dev whose Linux inodes keep them (synth_attr.h).
 */

#include "synth_attr.h"
#include "vfs.h"
#include "../rtc.h"
#include "../mm/kheap.h"
#include "../lib/kerrno.h"
#include "../lib/spinlock.h"

/* One node's kept attributes: `what` has a VFS_SET_* bit for each field of
 * `st` a change set, and VFS_SET_CTIME once any has. */
struct synth_attr {
    struct synth_attr *next;
    uint64_t dev;
    uint64_t ino;
    int what;
    struct vfs_stat st;
};

/* The kept records, newest first, and the lock over the list and every
 * record's fields. */
static struct synth_attr *synth_attrs;
static struct spinlock synth_attrs_lock = SPINLOCK_INIT_NAMED("synth_attrs");

/** The record of `dev` and `ino`, or NULL. Called under the lock. */
static struct synth_attr *synth_attr_find(uint64_t dev, uint64_t ino)
{
    struct synth_attr *a;

    for (a = synth_attrs; a; a = a->next) {
        if (a->dev == dev && a->ino == ino) {
            return a;
        }
    }
    return NULL;
}

/** Copy the fields `what` names from `from` into `to`, the mode's
 * permission bits alone. */
static void synth_attr_copy(struct vfs_stat *to, const struct vfs_stat *from, int what)
{
    if (what & VFS_SET_MODE) {
        to->mode = (to->mode & 0170000u) | (from->mode & 07777u);
    }
    if (what & VFS_SET_UID) {
        to->uid = from->uid;
    }
    if (what & VFS_SET_GID) {
        to->gid = from->gid;
    }
    if (what & VFS_SET_ATIME) {
        to->atime_sec = from->atime_sec;
        to->atime_nsec = from->atime_nsec;
    }
    if (what & VFS_SET_MTIME) {
        to->mtime_sec = from->mtime_sec;
        to->mtime_nsec = from->mtime_nsec;
    }
    if (what & VFS_SET_CTIME) {
        to->ctime_sec = from->ctime_sec;
        to->ctime_nsec = from->ctime_nsec;
    }
}

int synth_attr_apply(uint64_t dev, uint64_t ino, struct vfs_stat *st)
{
    uint32_t irq = spin_lock_irqsave(&synth_attrs_lock);
    const struct synth_attr *a = synth_attr_find(dev, ino);
    int what = a ? a->what : 0;

    if (a) {
        synth_attr_copy(st, &a->st, a->what);
    }
    spin_unlock_irqrestore(&synth_attrs_lock, irq);
    return what;
}

int32_t synth_attr_store(uint64_t dev, uint64_t ino, int what, const struct vfs_stat *st)
{
    struct synth_attr *made = kzalloc(sizeof(*made));
    struct synth_attr *a;
    int64_t sec, nsec;
    uint32_t irq;

    rtc_coarse_realtime(&sec, &nsec);
    irq = spin_lock_irqsave(&synth_attrs_lock);
    a = synth_attr_find(dev, ino);
    if (!a && made) {
        a = made;
        made = NULL;
        a->dev = dev;
        a->ino = ino;
        a->next = synth_attrs;
        synth_attrs = a;
    }
    if (a) {
        synth_attr_copy(&a->st, st, what);
        a->st.ctime_sec = sec;
        a->st.ctime_nsec = nsec;
        a->what |= what | VFS_SET_CTIME;
    }
    spin_unlock_irqrestore(&synth_attrs_lock, irq);
    kfree(made);
    return a ? 0 : -ENOMEM;
}
