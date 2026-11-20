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

/** Keep a proc task directory and its name as the start of a copied path.
 * For other proc directories keep the mount root. Return 1 for a task. */
int path_buf_set_dir(char *buf, struct vfs_mount *mnt, void *node, const char *base);

/** Return the held mount for a spelling in a path page and skip its prefix.
 * A path outside such a page or prefix gives NULL and leaves *path alone. */
struct vfs_mount *path_buf_mount(const char **path, void **node);

/** Give back a page `path_buf_get` returned; NULL is ignored. */
void path_buf_put(char *buf);

/** The working directory's absolute, canonical path, for a caller that
 * holds the big lock, under which it changes. */
const char *proc_cwd(const struct process *p);

/** Copy the working directory's path into `out`, a path page, under the
 * lock of `p`'s `fs_struct`. Return nonzero for a directory on procfs. */
int proc_cwd_read(const struct process *p, char *out);

/**
 * Make `path` the working directory's path: 0, `-ENAMETOOLONG` past
 * `VFS_PATH_MAX`, or `-ENOMEM`, when the old path is kept. `path` may be the
 * current one.
 */
int proc_cwd_set(struct process *p, const char *path);

/** Give `to` the working directory path of `from`, for fork: 0 or -ENOMEM. */
int proc_cwd_copy(struct process *to, const struct process *from);

/** Free a record's path pages. The working directory's path, a long one in
 * the heap included, belongs to the `fs_struct` and is freed with it when
 * its last reference goes (`proc_fs_put`). */
void path_bufs_free(struct process *p);

#endif /* _PATHBUF_H_ */
