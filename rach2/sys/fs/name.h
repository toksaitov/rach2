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
/* `vfs_name_seq` without the lock, for a walk that compares the count
 * again only if it climbs a `..` (`vfs_name_seq_still`): a change begun
 * after the read moves the count, and one under way before it is the 0
 * returned. */
int vfs_name_seq_peek(uint32_t *seq);
/* Whether no change of names is under way and none has begun since `seq`
 * was read (`vfs_name_seq`, `vfs_name_seq_peek`), as Linux's walk under RCU
 * checks the sequence of the directory it climbs from before it trusts the
 * parent (fs/namei.c:1990-1995). */
int vfs_name_seq_still(uint32_t seq);
/* Bracket an unlink, an rmdir or a rename, from the name cache's first
 * forget to the records' update, so that a record captured without the big
 * lock across it is refused (`vfs_name_capture_at`). Under the big lock;
 * may nest. */
void vfs_name_change_begin(void);
void vfs_name_change_end(void);
struct vfs_name *vfs_name_ref(struct vfs_name *name);
void vfs_name_put(struct vfs_name *name);
int vfs_name_deleted(const struct vfs_name *name);
/* The inode number of the directory the record names its file in, into
 * `*dir_ino`, and the file's name there, into `*leaf`, which stays the
 * record's: 0, or -1 when that directory is not on the file system of the
 * base mount `base` or its attributes cannot be read. Under the big lock,
 * which keeps both from a rename, as inotify reports an event of an open
 * file to its directory under its name there now, as Linux's
 * fsnotify_parent takes the dentry's parent at the event. */
int vfs_name_dir(struct vfs_name *name, const struct vfs_mount *base, uint64_t *dir_ino,
                 const char **leaf);

/* Read the selected entry's path under its namespace lock. A deleted entry
 * keeps its own leaf, never another hard link. Return 0 or a negative errno. */
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
