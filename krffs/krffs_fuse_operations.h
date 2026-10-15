/*
 * krffs_fuse_operations.h: the FUSE handlers of the krffs driver, over the
 * file system in `KRFFS_File_System`.
 *
 * A path is found as the kernel finds one, a component at a time by its
 * directory's id and its name (`krffs_find_node_by_path`), so an image the
 * stager wrote and one made through the mount are read alike. A handler that
 * makes, renames or removes a name finds the directory of the last component
 * first, and fails with -ENOENT when it is missing, -ENOTDIR when it is not a
 * directory, and -ENAMETOOLONG for a name of more than 255 bytes. Each
 * handler returns 0 or a negative errno: -ENOENT when `KRFFS_File_System` is
 * NULL or the node is not found, with a line on stderr. The handlers change
 * the mapping in place and take no lock, so FUSE must run them one at a time.
 */
#ifndef KRFFS_FUSE_OPERATIONS_H
#define KRFFS_FUSE_OPERATIONS_H

#include "krffs_platform.h"

#include <fuse.h>

/**
 * Fills `stat_buffer` for `path`: the root and a node with a directory's
 * mode as a directory of two links and one for each directory in it, and any
 * other node as a regular file of one link, whatever the type bits of its
 * mode. The mode has the node's 07777 bits, and at the root the 0777 bits,
 * so the dirty flag is not shown.
 */
int krffs_getattr(const char *path, struct PLATFORM_STAT *stat_buffer);

/**
 * Lists `.`, `..` and every node with a name in the directory `path`, or
 * returns -ENOTDIR when `path` is not a directory. An orphan, whose name is
 * empty, is not listed.
 */
int krffs_readdir(const char *path, void *buffer, fuse_fill_dir_t filler, PLATFORM_OFF_T offset,
                  struct fuse_file_info *file_info);

/** Returns 0 when `path` names a node; it stores no handle. */
int krffs_open(const char *path, struct fuse_file_info *file_info);

/**
 * Copies to `buffer` the data of `path` from `offset`, at most `size` bytes,
 * and sets the access time when a byte is read. Returns the bytes copied, 0
 * at or past the end, or -EISDIR for a directory.
 */
int krffs_read(const char *path, char *buffer, size_t size, PLATFORM_OFF_T offset,
               struct fuse_file_info *file_info);

/**
 * Makes an empty node named by `path` with `mode` as it is, type bits
 * included, the owner of the calling process, and the current time as its
 * three times and its directory's modification and change times. Its id is
 * the djb2 hash of its name, as the kernel gives a file. `device` is not
 * used. Returns -EEXIST when the name is found and -ENOSPC when no free node
 * has room for a header.
 */
int krffs_mknod(const char *path, mode_t mode, dev_t device);

/**
 * Makes the node as `krffs_mknod` does, or returns 0 and leaves the node as
 * it is when the name is found, or -EISDIR when it is a directory. It stores
 * no handle.
 */
int krffs_create(const char *path, mode_t mode, struct fuse_file_info *file_info);

/**
 * Makes the directory `path` with the 07777 bits of `mode`, as `krffs_mknod`
 * makes a node, but with the id the kernel's `mkdir` gives: the djb2 hash of
 * the name mixed with the id of the directory that holds it
 * (`krffs_mix_id`). Two names can mix to one id, and two directories with
 * one id would share their children, so a name whose id a directory already
 * has is refused with -EINVAL, as the kernel refuses it.
 */
int krffs_mkdir(const char *path, mode_t mode);

/**
 * Writes `size` bytes from `buffer` at `offset` of `path`, growing the node
 * with `krffs_resize_reserved_node` when its space is short, which can move
 * it. A gap between the old end and `offset` is cleared to zero bytes, and
 * the modification time is set. Returns `size`, -ENOSPC when the node
 * cannot grow, or -EISDIR for a directory.
 */
int krffs_write(const char *path, const char *buffer, size_t size, PLATFORM_OFF_T offset,
                struct fuse_file_info *file_info);

/**
 * Returns 0 when `path` names a node, the root included; a write is a store
 * into the mapping, so there is nothing to flush.
 */
int krffs_fsync(const char *path, int is_data_sync, struct fuse_file_info *file_info);

/**
 * Sets the data size of `path` to `new_size`, clearing the bytes a growth
 * adds, and sets the modification time. Returns -ENOSPC when the node cannot
 * grow, or -EISDIR for a directory.
 */
int krffs_truncate(const char *path, PLATFORM_OFF_T new_size);

/**
 * Renames `path` to `new_path`, which may be in another directory, first
 * freeing a node already named `new_path`, and sets the change time of the
 * node and the modification and change times of both directories. The node
 * gets the djb2 hash of its new name as its id, as the kernel's rename gives
 * it. As on the kernel's boot root, a directory is not renamed (-EPERM) and
 * a directory is not replaced (-EISDIR). Renaming a node to its own name
 * changes nothing.
 */
int krffs_rename(const char *path, const char *new_path);

/**
 * Frees the node of `path` and sets its directory's modification and change
 * times. Returns -EISDIR for a directory.
 */
int krffs_unlink(const char *path);

/**
 * Frees the directory `path` and sets the modification and change times of
 * the directory that held it. Returns -ENOTDIR when `path` is not a
 * directory and -ENOTEMPTY when a node with a name is in it; an orphan in it
 * does not count, as the kernel's `rmdir` does not count one.
 */
int krffs_rmdir(const char *path);

/**
 * Sets the access time from `time[0]` and the modification time from
 * `time[1]`, each a time, `UTIME_NOW` or `UTIME_OMIT`; a NULL `time` means
 * now for both. The change time becomes now unless both are `UTIME_OMIT`.
 * The root is allowed.
 */
int krffs_utimens(const char *path, const struct timespec time[2]);

/**
 * Sets the 07777 bits of the mode from `mode`, as the kernel's `chmod` does,
 * and the change time. At the root only the 0777 bits change, so the dirty
 * flag is neither set nor cleared.
 */
int krffs_chmod(const char *path, mode_t mode);

/**
 * Sets the owner to `uid` and the group to `gid`, keeping one given as -1,
 * and the change time. The root is allowed.
 */
int krffs_chown(const char *path, uid_t uid, gid_t gid);

/**
 * Fills `statvfs_buffer` in 1024-byte blocks: the region's size, rounded up,
 * and the free space less a header for each free node, rounded up; the
 * reserved nodes as files, with none free; and names of 255 bytes. A NULL
 * `path` is -EFAULT.
 */
int krffs_statfs(const char *path, struct statvfs *statvfs_buffer);

/** The handlers above, as `fuse_main` takes them. */
extern const struct fuse_operations krffs_operations;

#endif
