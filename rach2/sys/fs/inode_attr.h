#ifndef _INODE_ATTR_H_
#define _INODE_ATTR_H_

/*
 * The attributes of a file no file system holds, an anonymous pipe or a
 * socket, which Linux keeps on the inode pipefs or sockfs gives it: fstat
 * reads them, and fchmod, fchown and futimens change them.
 */

#include "../lib/kint.h"

struct vfs_stat;

/* One pipe's or socket's attributes. `mode` has the S_IF type bits; `uid`
 * and `gid` are kernel ids; the times are seconds and nanoseconds since
 * 1970. All of them are read and written under one lock, `inode_attr_get`
 * and `inode_attr_set`'s. */
struct inode_attr {
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    int64_t atime_sec;
    int64_t mtime_sec;
    int64_t ctime_sec;
    int32_t atime_nsec;
    int32_t mtime_nsec;
    int32_t ctime_nsec;
};

/* What `inode_attr_stamp` sets to the time now: the access time, which a
 * read sets, and the modification and change times, which a write sets. */
#define INODE_ATTR_READ  1
#define INODE_ATTR_WRITE 2

/**
 * Give `a` the mode `mode`, type bits and all, the calling process's
 * file-system ids, or root's with no process, and, with `stamped`, the time
 * now for all three times, as Linux's pipefs gives a pipe; without it, 0,
 * the epoch, as sockfs leaves a socket's times.
 */
void inode_attr_init(struct inode_attr *a, uint32_t mode, int stamped);

/** Copy `a` into the mode, owner, group and times of `st`, leaving its
 * other fields as they are. */
void inode_attr_get(const struct inode_attr *a, struct vfs_stat *st);

/**
 * Store in `a` the fields of `st` that `what`, an OR of the VFS_SET_* bits
 * of vfs.h, names: the permission bits of the mode, keeping the type, the
 * owner, the group and the access and modification times; the change time
 * becomes the time now, as a setattr of any inode sets it.
 */
void inode_attr_set(struct inode_attr *a, int what, const struct vfs_stat *st);

/** Set the times `what` names, INODE_ATTR_READ or INODE_ATTR_WRITE, to the
 * coarse time now, as Linux's file_accessed and file_update_time do. */
void inode_attr_stamp(struct inode_attr *a, int what);

#endif /* _INODE_ATTR_H_ */
