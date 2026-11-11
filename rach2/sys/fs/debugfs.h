/* debugfs.h: the debug file system at /sys/kernel/debug, whose files the
 * kernel's subsystems register, root's alone, as Linux mounts it. */
#ifndef _DEBUGFS_H_
#define _DEBUGFS_H_

#include "../lib/kint.h"
#include "file.h"

struct vfs_mount;
struct vfs_ops;
struct block_device;

/* The magic number statfs reports, Linux's DEBUGFS_MAGIC. */
#define DEBUGFS_MAGIC 0x64626720u

/* The longest name of one file or directory, NUL not counted. */
#define DEBUGFS_NAME_MAX 63

/* What an open of a registered file calls once its descriptor has `ops`
 * and `priv` = the registrant's `data`: it may replace both, and refuses
 * the open with -errno. `flags` are open(2)'s. It may sleep, and after a
 * sleep it leaves `e` alone, since the descriptor table may have moved. */
typedef int32_t (*debugfs_open_fn)(fd_entry_t *e, int flags);

/*
 * A text file's hooks, Linux's seq_file with a write. `show` writes the
 * text of the registrant's `data` into the `cap` bytes at `buf`, as far as
 * they go, and returns the whole text's length, or -errno, which the read
 * returns; it is called with NULL and 0 to size the text, then again, and
 * does not sleep. A read from offset 0 makes the text anew, as seq_read
 * does. `store` takes a write of `len` bytes from the user's `buf` and
 * returns `len` or -errno; it may sleep. A file without `store` answers a
 * write with EINVAL.
 */
struct debugfs_text_ops {
    int32_t (*show)(void *data, char *buf, uint32_t cap);
    int32_t (*store)(void *data, const char __user *buf, uint32_t len);
};

/* A registrant's hold on what its files' data belong to: `get` runs as
 * each node below its directory is made and `put` as each is freed, once
 * nothing holds the node, so the data outlive every walk and every open
 * description that reaches them. */
struct debugfs_owner {
    void (*get)(void *owner);
    void (*put)(void *owner);
};

/**
 * Register the file `path`, relative to the mount's root ("sync/sw_sync",
 * "dri/0/name"), owned by root with the permission bits `mode`, and the
 * directories on the path that are not there yet, mode 0755. An open of the
 * file binds `ops` to the descriptor with `priv` set to `data`, then calls
 * `open` unless it is NULL. 0; -EINVAL for an empty path, an empty, `.` or
 * `..` component or one past DEBUGFS_NAME_MAX, or NULL `ops`; -EEXIST for a
 * path that is there; -ENOTDIR for a file on the way; or -ENOMEM. Under the
 * big kernel lock; does not sleep.
 */
int32_t debugfs_create_file(const char *path, uint32_t mode, const struct file_ops *ops,
                            debugfs_open_fn open, void *data);

/**
 * Register the text file `path` as `debugfs_create_file` does, with the
 * hooks `ops` on `data`. 0 or its errors; -EINVAL for NULL `ops` or `show`.
 */
int32_t debugfs_create_text(const char *path, uint32_t mode, const struct debugfs_text_ops *ops,
                            void *data);

/**
 * Register the directory `path`, mode 0755, as `debugfs_create_file` makes
 * the directories on its way, held by `owner` through `ops` (NULL: by the
 * owner of the directory it is made in, if any), and every node made below
 * it with it. 0 or `debugfs_create_file`'s errors.
 */
int32_t debugfs_create_dir(const char *path, const struct debugfs_owner *ops, void *owner);

/**
 * Register the symbolic link `path` to `target`, mode 0777, as Linux's
 * debugfs_create_symlink, with a copy of `target`. 0 or
 * `debugfs_create_file`'s errors; -EINVAL for NULL `target`.
 */
int32_t debugfs_create_symlink(const char *path, const char *target);

/**
 * Take the file or the directory `path`, and everything below it, out of
 * debugfs at once. A descriptor open on a file taken out keeps its
 * operations and `priv`, as on Linux, so the registrant keeps what they use
 * until the last close. Nothing for a path that is not there. Under the big
 * kernel lock.
 */
void debugfs_remove(const char *path);

/**
 * Bind the registered file `node` of a debugfs mount to the descriptor `e`
 * just opened on it with open(2)'s `flags`, for the open of a regular file
 * of the system call layer: `ops`, `priv`, then the file's `open`, or a
 * text file's operations. 0, or the `open`'s -errno or -ENOMEM, after which
 * the caller frees the descriptor's slot, which clears `ops` and `priv`. It
 * may sleep; the slot, a reservation until the caller publishes it, stays
 * where it is.
 */
int32_t debugfs_bind(void *node, fd_entry_t *e, int flags);

/* The operations of the debugfs type, which vfs.c's vfs_debugfs_type
 * gives. */
extern const struct vfs_ops debugfs_vops;

/**
 * The constructor of a debugfs mount: every mount shows the one tree, as
 * Linux's single superblock does, and the first registers the files of the
 * kernel's own, sync/sw_sync and sync/info. `options` are Linux's: `mode=`
 * in octal, `uid=` and `gid=` in decimal, for the root directory, and
 * unknown ones are passed over. 0, or -ENOMEM.
 */
int debugfs_mount(struct vfs_mount *mnt, struct block_device *dev, uint32_t flags,
                  const char *options);

#endif /* _DEBUGFS_H_ */
