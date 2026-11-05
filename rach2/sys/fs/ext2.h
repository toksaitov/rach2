#ifndef _EXT2_H_
#define _EXT2_H_

/* The ext2 file system, the type `ext2` of mount(2) and of the disk root. */

#include "vfs.h"

/**
 * The type `ext2`, read and written through the buffer cache;
 * ext2.Readme.md has the contract in full.
 *
 * The constructor ignores its options and reads three mount flags:
 * VFS_MS_RDONLY, and VFS_MS_SYNCHRONOUS and VFS_MS_DIRSYNC, either of which
 * puts each metadata write on the medium before the next. It returns
 * -EINVAL, with the record `ext2: refused: WHAT`, for a volume whose
 * superblock, features or group descriptors it does not take or whose root
 * inode is not a directory; -EIO when the device fails a read, and -ENOMEM.
 * It mounts read-only, with the reason recorded, a volume with an unknown
 * read-only-compatible feature, with `dir_index`, or with blocks below 4096
 * bytes. A writable mount of a volume whose valid bit is clear or whose
 * error bit is set checks the volume first, and is read-only when the check
 * finds a fault; a remount to writable checks it the same way, and is
 * -EROFS then.
 *
 * `file_locked` is set and `ns_serialised` clear: the operations lock each
 * inode on its own (`lock_node`), the mount's allocator apart, and the VFS
 * takes a file's or a directory's lock, not the mount's, around a change,
 * so that a slow request on one file holds up only that file's callers. Of
 * the operations it leaves out `lookup_rest`, `pin` and `unpin`.
 */
extern const struct vfs_type vfs_ext2_type;

/** The nodes in memory that every ext2 mount's search for an inode by its
 * number has read since boot, for /proc/stat: one or two for each search
 * while the mounts' hashes keep their chains short. */
uint32_t ext2_node_visits(void);

#endif /* _EXT2_H_ */
