/*
 * krffs_fuse_operations.h: the FUSE handlers of the krffs driver, over the
 * file system in `KRFFS_File_System`.
 *
 * The mount is one flat directory. A path is `/` and a name, and the name is
 * found with `krffs_find_reserved_node_by_name`, by its djb2 hash alone, so a
 * node with a mixed id is not found, and `/` with
 * `krffs_find_reserved_or_root_node_by_name` where the root is allowed. Each
 * handler returns 0 or a negative errno: -ENOENT when `KRFFS_File_System` is
 * NULL or the name is not found, with a line on stderr. The handlers change
 * the mapping in place and take no lock, so FUSE must run them one at a time.
 */
#ifndef KRFFS_FUSE_OPERATIONS_H
#define KRFFS_FUSE_OPERATIONS_H

#include "krffs_platform.h"

#include <fuse.h>

/**
 * Fills `stat_buffer` for `path`: the root as a directory of two links, any
 * other node as a regular file of one, whatever the type bits of its mode.
 * The mode has the node's 07777 bits, and at the root the 0777 bits, so the
 * dirty flag is not shown.
 */
int krffs_getattr(const char *path, struct PLATFORM_STAT *stat_buffer);

/**
 * Lists `.`, `..` and every reserved node with a name, from every directory
 * of the image, when `path` is `/`; any other `path` is -ENOENT. An orphan,
 * whose name is empty, is not listed.
 */
int krffs_readdir(const char *path, void *buffer, fuse_fill_dir_t filler, PLATFORM_OFF_T offset,
                  struct fuse_file_info *file_info);

/** Returns 0 when `path` names a reserved node; it stores no handle. */
int krffs_open(const char *path, struct fuse_file_info *file_info);

/**
 * Copies to `buffer` the data of `path` from `offset`, at most `size` bytes,
 * and sets the access time when a byte is read. Returns the bytes copied, 0
 * at or past the end.
 */
int krffs_read(const char *path, char *buffer, size_t size, PLATFORM_OFF_T offset,
               struct fuse_file_info *file_info);

/**
 * Makes an empty node named by `path` with `mode` as it is, type bits
 * included, the owner of the calling process, and the current time as its
 * three times and the root's modification and change times. `device` is not
 * used. Returns -EEXIST when the name is found and -ENOSPC when no free node
 * has room for a header.
 */
int krffs_mknod(const char *path, mode_t mode, dev_t device);

/**
 * Makes the node as `krffs_mknod` does, or returns 0 and leaves the node as
 * it is when the name is found. It stores no handle.
 */
int krffs_create(const char *path, mode_t mode, struct fuse_file_info *file_info);

/**
 * Writes `size` bytes from `buffer` at `offset` of `path`, growing the node
 * with `krffs_resize_reserved_node` when its space is short, which can move
 * it. A gap between the old end and `offset` is cleared to zero bytes, and
 * the modification time is set. Returns `size`, or -ENOSPC when the node
 * cannot grow.
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
 * grow.
 */
int krffs_truncate(const char *path, PLATFORM_OFF_T new_size);

/**
 * Renames `path` to `new_path`, first freeing a node already named
 * `new_path`, and sets the root's modification and change times.
 */
int krffs_rename(const char *path, const char *new_path);

/**
 * Frees the node of `path` and sets the root's modification and change
 * times.
 */
int krffs_unlink(const char *path);

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
static struct fuse_operations krffs_operations = {.getattr = krffs_getattr,
                                                  .readdir = krffs_readdir,
                                                  .open = krffs_open,
                                                  .read = krffs_read,
                                                  .mknod = krffs_mknod,
                                                  .create = krffs_create,
                                                  .write = krffs_write,
                                                  .fsync = krffs_fsync,
                                                  .truncate = krffs_truncate,
                                                  .unlink = krffs_unlink,
                                                  .rename = krffs_rename,
                                                  .utimens = krffs_utimens,
                                                  .chmod = krffs_chmod,
                                                  .chown = krffs_chown,
                                                  .statfs = krffs_statfs};

#endif
