/*
 * inode_attr.c: the attributes of an anonymous pipe or a socket, kept as
 * Linux keeps them on the pipefs or sockfs inode (inode_attr.h).
 */

#include "inode_attr.h"
#include "vfs.h"
#include "../rtc.h"
#include "../proc/process.h"
#include "../lib/spinlock.h"

/* The lock of every pipe's and socket's attributes, which a read, a write,
 * fstat and the calls that change them hold for a few stores. */
static struct spinlock inode_attr_lock = SPINLOCK_INIT_NAMED("inode_attr");

void inode_attr_init(struct inode_attr *a, uint32_t mode, int stamped)
{
    int64_t sec = 0, nsec = 0;

    if (stamped) {
        rtc_coarse_realtime(&sec, &nsec);
    }
    a->mode = mode;
    a->uid = current_proc ? proc_fsuid(current_proc) : 0;
    a->gid = current_proc ? proc_fsgid(current_proc) : 0;
    a->atime_sec = a->mtime_sec = a->ctime_sec = sec;
    a->atime_nsec = a->mtime_nsec = a->ctime_nsec = (int32_t)nsec;
}

void inode_attr_get(const struct inode_attr *a, struct vfs_stat *st)
{
    uint32_t irq = spin_lock_irqsave(&inode_attr_lock);

    st->mode = a->mode;
    st->uid = a->uid;
    st->gid = a->gid;
    st->atime_sec = a->atime_sec;
    st->atime_nsec = a->atime_nsec;
    st->mtime_sec = a->mtime_sec;
    st->mtime_nsec = a->mtime_nsec;
    st->ctime_sec = a->ctime_sec;
    st->ctime_nsec = a->ctime_nsec;
    spin_unlock_irqrestore(&inode_attr_lock, irq);
}

void inode_attr_set(struct inode_attr *a, int what, const struct vfs_stat *st)
{
    int64_t sec, nsec;
    uint32_t irq;

    rtc_coarse_realtime(&sec, &nsec);
    irq = spin_lock_irqsave(&inode_attr_lock);
    if (what & VFS_SET_MODE) {
        a->mode = (a->mode & 0170000u) | (st->mode & 07777u);
    }
    if (what & VFS_SET_UID) {
        a->uid = st->uid;
    }
    if (what & VFS_SET_GID) {
        a->gid = st->gid;
    }
    if (what & VFS_SET_ATIME) {
        a->atime_sec = st->atime_sec;
        a->atime_nsec = (int32_t)st->atime_nsec;
    }
    if (what & VFS_SET_MTIME) {
        a->mtime_sec = st->mtime_sec;
        a->mtime_nsec = (int32_t)st->mtime_nsec;
    }
    if (what) {
        a->ctime_sec = sec;
        a->ctime_nsec = (int32_t)nsec;
    }
    spin_unlock_irqrestore(&inode_attr_lock, irq);
}

void inode_attr_stamp(struct inode_attr *a, int what)
{
    int64_t sec, nsec;
    uint32_t irq;

    rtc_coarse_realtime(&sec, &nsec);
    irq = spin_lock_irqsave(&inode_attr_lock);
    if (what & INODE_ATTR_READ) {
        a->atime_sec = sec;
        a->atime_nsec = (int32_t)nsec;
    }
    if (what & INODE_ATTR_WRITE) {
        a->mtime_sec = a->ctime_sec = sec;
        a->mtime_nsec = a->ctime_nsec = (int32_t)nsec;
    }
    spin_unlock_irqrestore(&inode_attr_lock, irq);
}
