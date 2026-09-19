#ifndef _FAT32_H_
#define _FAT32_H_

#include "vfs.h"

/*
 * fat32: the `vfat` type.
 *
 * The constructor checks the BPB field by field before the mount is
 * published and refuses a volume with a record naming the field; what it
 * accepts is FAT32 as Linux recognises it (a zero 16-bit FAT size, a
 * nonzero 32-bit one, no root entries, FAT version zero), whatever the
 * cluster count.
 *
 * Writing: every metadata write goes through the cache synchronously, in
 * the order fat32.Readme.md gives for each operation, so a crash between
 * two writes may leak clusters and never cross-links them, and an entry
 * never names a free cluster. Data is written back. The dirty flag is set
 * in both places before the first metadata write of a mount and cleared by
 * a clean unmount; FSInfo is kept at sync and unmount. Every mutation runs
 * under the mount's namespace lock. A name is checked and stored by the
 * rules in fat32name.h: short alone when it fits, long entries in front of
 * an alias otherwise. A dirty volume is checked at mount and the states
 * this driver's own order leaves are repaired; any other mounts read-only.
 *
 * A node is an entry in a fixed per-mount pool of `FAT32_NODES`, freed at its
 * last put; a lookup that finds the pool full answers ENOMEM. A directory's
 * identity is its first cluster; a file's is its directory's cluster and its
 * entry index, and the inode number reported is built from the same fields so
 * that no two entries share one. Every entry reports the mount's `uid` and
 * `gid` and the mode `0777` less its `umask` option (0755 under the default
 * 022), the options Linux's vfat takes; FAT has no execute bit, so a program
 * on a volume runs unless the mount is `noexec`.
 *
 * Names: a long name is kept when its entries check and its checksum
 * matches the short entry; otherwise the short name is shown, in lower
 * case where the reserved byte says so, with a byte above 0x7F shown as
 * `?`. Lookup folds ASCII case; a non-ASCII name matches exactly.
 * Directories are read at 32-byte entry offsets, which is what
 * `getdents64` reports.
 */

extern const struct vfs_type vfs_vfat_type;

#endif /* _FAT32_H_ */
