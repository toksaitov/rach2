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
 * A node is taken from the heap when a lookup first reaches its entry, kept
 * on the mount's list and freed at its last put; a lookup that cannot take
 * one answers ENOMEM. A directory's identity is its first cluster; a file's
 * is its directory's cluster and its entry index, and the inode number
 * reported is built from the same fields so that no two entries share one.
 * Every entry reports the mount's `uid` and `gid` and the mode `0777` less
 * its `umask` option (0755 under the default 022), the options Linux's vfat
 * takes, without the write bits when the entry has the read-only
 * attribute; FAT has no execute bit, so a program on a volume runs unless
 * the mount is `noexec`.
 *
 * Names: a long name is kept when its entries check and its checksum
 * matches the short entry; otherwise the short name is shown, in lower
 * case where the reserved byte says so, with a byte above 0x7F shown as
 * `?`. Lookup folds ASCII case; a non-ASCII name matches exactly.
 * Directories are read at 32-byte entry offsets, which is what
 * `getdents64` reports.
 */

/** The `vfat` type, in the VFS's table of mount types. */
extern const struct vfs_type vfs_vfat_type;

/**
 * The attribute byte of `node` on the vfat mount `mnt` as Linux's
 * FAT_IOCTL_GET_ATTRIBUTES gives it (`fat_make_attrs`): the entry's
 * attributes, with the directory bit of a directory, the root's included,
 * and the read-only bit for a node whose mode has no write bit.
 */
uint32_t fat_attr_get(struct vfs_mount *mnt, void *node);

/* What the caller of `fat_attr_set` may do: change a node's attributes, as
 * its owner or with CAP_FOWNER may chmod it, and change a system file's or
 * make one under sys_immutable, with CAP_LINUX_IMMUTABLE. */
#define FAT_ATTR_MAY_CHANGE 0x1
#define FAT_ATTR_MAY_SYS    0x2

/**
 * Store `attr` as the attributes of `node` on the vfat mount `mnt`, as
 * Linux's FAT_IOCTL_SET_ATTRIBUTES does (`fat_ioctl_set_attributes`): the
 * volume and directory bits stay as they are, and the rest of the low byte
 * is written to the entry, the read-only bit with it, which the mode then
 * shows. `rights` is an OR of FAT_ATTR_MAY_*. Returns 0, -EROFS for a
 * mount that does not write, -EINVAL for the root given any attribute but
 * the directory bit, -EPERM under sys_immutable for a change that has the
 * system bit before or after without FAT_ATTR_MAY_SYS, or for a caller
 * without FAT_ATTR_MAY_CHANGE, or the entry's write error. Under
 * sys_immutable the system bit makes the node immutable (`fat_getattr`).
 * Takes the mount's namespace lock.
 */
int fat_attr_set(struct vfs_mount *mnt, void *node, uint32_t attr, int rights);

#endif /* _FAT32_H_ */
