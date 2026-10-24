/* Held directory entries for open and executable files. */
#ifndef _VFS_NAME_H_
#define _VFS_NAME_H_

#include "../lib/kint.h"

struct vfs_name;
struct vfs_mount;
struct vfs_ref;

/* Capture the parent and leaf under the namespace lock. The record owns a
 * node hold and a mount reference until its final put. No lookup is made.
 * Types without counted node holds retain their existing per-node paths;
 * those types have no hard links. Return 0, or -ENOMEM. */
int vfs_name_capture(const struct vfs_ref *parent, const char *leaf, struct vfs_name **out);
struct vfs_name *vfs_name_ref(struct vfs_name *name);
void vfs_name_put(struct vfs_name *name);
int vfs_name_deleted(const struct vfs_name *name);

/* Read the selected entry's path under its namespace lock. A deleted entry
 * keeps its own leaf, never another hard link. Return 0 or a negative errno. */
int vfs_name_path(struct vfs_name *name, void *node, char *out, uint32_t cap, int *deleted);

/* Namespace notifications run only after successful mutation, under its
 * lock. A same-inode rename leaves both original entries unchanged. */
void vfs_name_unlink(struct vfs_mount *mnt, void *dir, const char *leaf);
int vfs_name_same(struct vfs_mount *mnt, void *from, const char *old, void *to, const char *leaf);
void vfs_name_rename(struct vfs_mount *mnt, void *from, const char *old, void *to,
                     const char *leaf);
void vfs_name_repoint(struct vfs_mount *mnt, const void *from, void *to);

#endif /* _VFS_NAME_H_ */
