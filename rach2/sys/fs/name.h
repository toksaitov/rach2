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
/* `vfs_name_capture` for a walk without the big lock that read `seq` with
 * `vfs_name_seq` before it began: -EAGAIN, with nothing kept, when a change
 * of names has begun since, which the walk may have raced. */
int vfs_name_capture_at(const struct vfs_ref *parent, const char *leaf, uint32_t seq,
                        struct vfs_name **out);
/* The count of changes of names into `*seq`; 1 when none is under way, 0
 * when one is, which a walk without the big lock leaves to the full walk. */
int vfs_name_seq(uint32_t *seq);
/* Bracket an unlink or a rename, from the name cache's first forget to the
 * records' update, so that a record captured without the big lock across
 * it is refused (`vfs_name_capture_at`). Under the big lock; may nest. */
void vfs_name_change_begin(void);
void vfs_name_change_end(void);
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
