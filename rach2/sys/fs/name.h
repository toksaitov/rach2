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
 * `vfs_seq_read` (walkseq.h) before it began: -EAGAIN, with nothing kept,
 * when a change has begun since, which the walk may have raced. */
int vfs_name_capture_at(const struct vfs_ref *parent, const char *leaf, uint64_t seq,
                        struct vfs_name **out);
/* Bracket an unlink, rmdir or rename, from the name cache's first forget to
 * the records' update, with the walk sequence (`vfs_seq_begin`), so that a
 * record captured without the big lock across it is refused
 * (`vfs_name_capture_at`). Under the big lock; may nest. */
void vfs_name_change_begin(void);
void vfs_name_change_end(void);
/** Admit an unlink, rmdir or rename before it takes its inode locks. On a
 * file-locked mount, changes may overlap until a path retry waits. */
void vfs_name_lock_changes(struct vfs_mount *mnt);
/** Release the shared hold after the change and its name updates. */
void vfs_name_unlock_changes(struct vfs_mount *mnt);
struct vfs_name *vfs_name_ref(struct vfs_name *name);
void vfs_name_put(struct vfs_name *name);
int vfs_name_deleted(const struct vfs_name *name);

/* Read the selected entry's path under the big lock. On a file-locked
 * mount, enter without inode or namespace locks. A change during a sleeping
 * walk restarts it with changes excluded. Other types need the namespace lock.
 * A deleted entry keeps its own leaf. Return 0 or a negative errno. */
int vfs_name_path(struct vfs_name *name, void *node, char *out, uint32_t cap, int *deleted);

/* Namespace notifications run only after successful mutation, under its
 * lock. A same-inode rename leaves both original entries unchanged. */
void vfs_name_unlink(struct vfs_mount *mnt, void *dir, const char *leaf);
int vfs_name_same(struct vfs_mount *mnt, void *from, const char *old, void *to, const char *leaf);
void vfs_name_rename(struct vfs_mount *mnt, void *from, const char *old, void *to,
                     const char *leaf);
/* After renameat2's RENAME_EXCHANGE: a record of `old` in `from` names
 * `leaf` in `to`, and one of `leaf` in `to` names `old` in `from`. */
void vfs_name_exchange(struct vfs_mount *mnt, void *from, const char *old, void *to,
                       const char *leaf);
/* A record of an O_TMPFILE file, which has no name: `leaf` in `parent`,
 * already deleted, as Linux's d_path shows such a file "/dir/#ino
 * (deleted)". Return 0, or -ENOMEM. */
int vfs_name_capture_deleted(const struct vfs_ref *parent, const char *leaf, struct vfs_name **out);
void vfs_name_repoint(struct vfs_mount *mnt, const void *from, void *to);

#endif /* _VFS_NAME_H_ */
