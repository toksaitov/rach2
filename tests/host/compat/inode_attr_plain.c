/* The attributes of pipes and sockets (fs/inode_attr.h) for a host test
 * that compiles their callers: kept as given, with no lock and no clock. */
#include "../../../rach2/sys/fs/inode_attr.h"
#include "../../../rach2/sys/fs/vfs.h"

/** Give `a` the mode `mode` and root's ids, and times of 0 whatever
 * `stamped` says. */
void inode_attr_init(struct inode_attr *a, uint32_t mode, int stamped)
{
    (void)stamped;
    a->mode = mode;
    a->uid = a->gid = 0;
    a->atime_sec = a->mtime_sec = a->ctime_sec = 0;
    a->atime_nsec = a->mtime_nsec = a->ctime_nsec = 0;
}

/** Copy the mode, owner, group and times of `a` into `st`. */
void inode_attr_get(const struct inode_attr *a, struct vfs_stat *st)
{
    st->mode = a->mode;
    st->uid = a->uid;
    st->gid = a->gid;
    st->atime_sec = a->atime_sec;
    st->atime_nsec = a->atime_nsec;
    st->mtime_sec = a->mtime_sec;
    st->mtime_nsec = a->mtime_nsec;
    st->ctime_sec = a->ctime_sec;
    st->ctime_nsec = a->ctime_nsec;
}

/** Store the fields of `st` that `what` names in `a`. */
void inode_attr_set(struct inode_attr *a, int what, const struct vfs_stat *st)
{
    if (what & VFS_SET_MODE) {
        a->mode = (a->mode & 0170000u) | (st->mode & 07777u);
    }
    if (what & VFS_SET_UID) {
        a->uid = st->uid;
    }
    if (what & VFS_SET_GID) {
        a->gid = st->gid;
    }
}

/** Nothing: the fixture has no clock. */
void inode_attr_stamp(struct inode_attr *a, int what)
{
    (void)a;
    (void)what;
}
