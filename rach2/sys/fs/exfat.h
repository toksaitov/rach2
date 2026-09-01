#ifndef _EXFAT_H_
#define _EXFAT_H_

#include "vfs.h"

/*
 * exfat: the `exfat` type, the file system flash media above 32 GB is sold
 * with, read from Microsoft's published specification. The contract is in
 * exfat.Readme.md.
 *
 * The constructor checks the boot sector field by field and both boot
 * regions' checksums before the mount is published, refuses a volume with
 * a record naming the field, and mounts read-only, with the reason named,
 * a volume whose two regions disagree, whose flags say MediaFailure, or
 * which has two FATs (TexFAT). The root's allocation bitmap and up-case
 * table are checked at mount; every name comparison and hash goes through
 * the volume's own up-case table.
 *
 * A node is an entry in a fixed per-mount pool of `EXFAT_NODES`, freed at
 * its last put. A directory's identity is its first cluster; a file's is
 * its directory's cluster and the index of its file entry. Every entry
 * reports the mount's `uid` and `gid` and the mode `0777` less its `umask`,
 * with the read-only attribute as the write bits, as vfat does.
 */

#define EXFAT_NODES  32
#define EXFAT_MOUNTS 4

extern const struct vfs_type vfs_exfat_type;

#endif /* _EXFAT_H_ */
