#ifndef _KRFFSMOUNT_H_
#define _KRFFSMOUNT_H_

/*
 * KRFFS as a type `mount(2)` places, over a block device through the buffer
 * cache. The root file system is a different thing: a mapped image with a
 * pointer interface, in krffs.c.
 */

struct vfs_type;

/* The type `krffs`, whose uses are serialised under the mount's lock. */
extern const struct vfs_type vfs_krffs_volume_type;

#endif /* _KRFFSMOUNT_H_ */
