#ifndef _PATHBUF_H_
#define _PATHBUF_H_

/*
 * pathbuf: where the kernel keeps the paths it is given. A path may be
 * VFS_PATH_MAX bytes, 4096 with its terminator, which is a quarter of a
 * process's kernel stack, so a system call copies its paths into pages
 * instead. Each process has a small set of them, taken one at a time the
 * first time a call needs that many at once and kept for the record's life:
 * after the first, a path costs no frame, and a count of frame allocations,
 * which `rach2.framefail` keeps, does not move with the paths a program
 * names. The working directory's path is here too: a short one is kept in
 * the `fs_struct` the record points to, and a longer one comes from the
 * heap.
 */

#include "../lib/kint.h"

struct process;
struct vfs_mount;
struct vfs_ref;

/**
 * A page of `VFS_PATH_MAX` bytes for one path, from the calling process's
 * set; NULL, which the caller answers with ENOMEM, when the heap has no page
 * or every page of the set is in use. Outside a process the page is the
 * heap's and goes back to it at the put.
 */
char *path_buf_get(void);

/** Keep a relative proc pathname on its directory's mount until buf is put. */
void path_buf_set_mount(char *buf, struct vfs_mount *mnt);

/**
 * Keep the directory (`mnt`, `node`), whose path is `base`, as the start of
 * the walk of the path in `buf`, which is `base` joined to a relative name,
 * until the page is put, as Linux walks a relative name from the
 * descriptor's or the working directory's own pair (fs/namei.c `path_init`
 * in 6.12): the walk starts at the node, of any type, /dev, /proc and /sys
 * among them, with `base` the prefix it skips. A mount of /proc passed with
 * no node is kept as its root, and the walk skips only the mount's place.
 * Return 1 when the walk starts at the node kept, and 0 otherwise, for a
 * directory with no mount, node or path, whose name is walked from the root
 * by the joined path.
 */
int path_buf_set_dir(char *buf, struct vfs_mount *mnt, void *node, const char *base);

/** `path_buf_set_dir` for the caller's working directory, its pair and its
 * path. Under the big lock, under which they change. */
int path_buf_set_cwd(char *buf);

/**
 * Whether a path page of any process keeps the directory `node`, on a mount
 * of the file system of `mnt`, as the start of a walk (`path_buf_set_dir`):
 * one the walk of a name relative to a descriptor or a working directory
 * on another namespace's mount stands on, which no description or working
 * directory may hold by then. A type that counts no holds on its nodes, the
 * KRFFS root and a KRFFS volume, refuses to remove a directory so kept, as
 * it refuses one a descriptor holds. The caller holds the big lock.
 */
int path_bufs_hold(const struct vfs_mount *mnt, const void *node);

/** Return the held mount for a spelling in a path page and skip its prefix.
 * A path outside such a page or prefix gives NULL and leaves *path alone. */
struct vfs_mount *path_buf_mount(const char **path, void **node);

/**
 * The start the page `path` keeps as a node and a prefix (`path_buf_set_dir`),
 * for a walk without the big lock: 1 with the pair, which the page holds, in
 * `*start` and the path after the prefix in `*rest`, "." for none; -1 for a
 * page that keeps another start, whose walk needs the big lock; or 0 for a
 * path that is no kept page. Nothing here sleeps or takes a lock.
 */
int path_buf_start(const char *path, struct vfs_ref *start, const char **rest);

/** Give back a page `path_buf_get` returned; NULL is ignored. */
void path_buf_put(char *buf);

/** The working directory's absolute, canonical path, for a caller that
 * holds the big lock, under which it changes. */
const char *proc_cwd(const struct process *p);

/** Copy the working directory's path into `out`, a path page, under the
 * lock of `p`'s `fs_struct`, for a caller without the big lock. */
void proc_cwd_read(const struct process *p, char *out);

/**
 * Make `path` the working directory's path: 0, `-ENAMETOOLONG` past
 * `VFS_PATH_MAX`, or `-ENOMEM`, when the old path is kept. `path` may be the
 * current one. A caller that changes the directory too brackets both with
 * the walk sequence (`vfs_seq_begin`), so that a walk without the big lock
 * never takes the new path with the old directory.
 */
int proc_cwd_set(struct process *p, const char *path);

/** Give `to` the working directory path of `from`, for fork: 0 or
 * -ENOMEM. */
int proc_cwd_copy(struct process *to, const struct process *from);

/** Free a record's path pages. The working directory's path, a long one in
 * the heap included, belongs to the `fs_struct` and is freed with it when
 * its last reference goes (`proc_fs_put`). */
void path_bufs_free(struct process *p);

#endif /* _PATHBUF_H_ */
