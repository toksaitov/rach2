#ifndef _EXT2_H_
#define _EXT2_H_

/* The ext2 file system, the type `ext2` of mount(2) and of the disk root. */

#include "vfs.h"

/**
 * The type `ext2`, read and written through the buffer cache;
 * ext2.Readme.md has the contract in full.
 *
 * The constructor takes the option `errors=continue`, `errors=remount-ro`
 * or `errors=panic`, which overrides the superblock's s_errors, ignores
 * the others, and reads three mount flags: VFS_MS_RDONLY, and
 * VFS_MS_SYNCHRONOUS and VFS_MS_DIRSYNC, either of which puts each
 * metadata write on the medium before the next. It returns -EINVAL, with
 * the record `ext2: refused: WHAT`, for a volume whose superblock, features
 * or group descriptors it does not take, whose root inode is not a
 * directory, or for another value of `errors=`; -EIO when the device fails
 * a read, and -ENOMEM. It mounts read-only, with the reason recorded, a
 * volume with an unknown read-only-compatible feature or with blocks below
 * 4096 bytes; a remount to writable of such a volume is -EROFS. A writable
 * mount or remount of a volume whose valid bit is clear or whose error bit
 * is set checks the volume first and is writable whatever the check finds,
 * as Linux's: a fault sets the error bit, and a check stopped at a limit of
 * its own leaves the volume unchecked, its valid bit clear at unmount. A
 * fault met at run time sets the error bit and then continues, makes the
 * mount read-only or stops the kernel, as `errors=` says.
 *
 * `file_locked` is set and `ns_serialised` clear: the operations lock each
 * inode on its own (`node_lock`), the mount's allocator apart, and the VFS
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
