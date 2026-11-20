#ifndef _SYNTH_ATTR_H_
#define _SYNTH_ATTR_H_

/*
 * The attributes chmod, chown and utimensat give a node of /sys, of /proc
 * outside a process's directory and /proc/sys, or a directory or device of
 * /dev, which Linux keeps on the node's kernfs, proc_dir_entry or devtmpfs
 * inode: a stat of the node gives them in place of its fixed ones.
 */

#include "../lib/kint.h"

struct vfs_stat;

/**
 * Give `st` the fields kept for the node of device `dev` and inode `ino`:
 * the permission bits, keeping the type, the owner, the group and the three
 * times, each only once a change has set it. Returns the VFS_SET_* bits of
 * the fields kept, 0 for a node no change has reached, which `st` keeps as
 * it was.
 */
int synth_attr_apply(uint64_t dev, uint64_t ino, struct vfs_stat *st);

/**
 * Keep for the node of `dev` and `ino` the fields of `st` that `what`, an
 * OR of vfs.h's VFS_SET_* bits, names, and the time now as its change time.
 * Returns 0, or -ENOMEM when the heap has no room for a node's first
 * record. Records last as long as the kernel, as the nodes they describe
 * do.
 */
int32_t synth_attr_store(uint64_t dev, uint64_t ino, int what, const struct vfs_stat *st);

#endif /* _SYNTH_ATTR_H_ */
