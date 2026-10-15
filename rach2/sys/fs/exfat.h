#ifndef _EXFAT_H_
#define _EXFAT_H_

/* The exFAT file system, the type `exfat` of mount(2). */

#include "vfs.h"

/**
 * The type `exfat`, the file system flash media above 32 GB is sold with,
 * written from Microsoft's published specification; exfat.Readme.md has the
 * contract in full.
 *
 * The constructor takes the options of vfat (`fat_parse_options`), and
 * checks both boot regions' checksums and the boot sector field by field
 * before the mount is published. It returns -EINVAL with the record
 * `exfat: refused: BootChecksum` when both boot regions fail their
 * checksums, with no record when the name, jump or boot signature is not
 * exFAT's or the options are not vfat's, and with `exfat: refused: WHAT`
 * for any other field out of range or root object that breaks the format;
 * -EIO when the device fails a read, and -ENOMEM. It mounts read-only, with
 * the reason recorded, a volume one of whose boot regions fails its
 * checksum or whose two regions disagree, whose flags say MediaFailure, or
 * which has two FATs (TexFAT) or a second allocation bitmap. The root's
 * allocation bitmap and up-case table are checked at mount; every name
 * comparison and hash goes through the volume's own up-case table. A
 * writable mount of a volume whose VolumeDirty bit is set checks the volume
 * first, and is read-only when the check finds a fault. The type has no
 * `remount`, so a change between read-only and writable is -EINVAL.
 *
 * A node is a record from the heap on the mount's list, freed at its last
 * put. A directory's identity is its first cluster; a file's is its
 * directory's cluster and the index of its file entry. Every entry reports
 * the mount's `uid` and `gid` and the mode `0777` less its `umask`, with the
 * read-only attribute as the write bits, as vfat does. Every change runs
 * under the mount's namespace lock; reads run without it.
 */
extern const struct vfs_type vfs_exfat_type;

#endif /* _EXFAT_H_ */
