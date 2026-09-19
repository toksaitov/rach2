#ifndef KRFFS_FUSE_OPERATIONS_H
#define KRFFS_FUSE_OPERATIONS_H

#include "krffs_platform.h"

#include <fuse.h>

/** Returns the attributes of the file at `path` in `stat_buffer`. */
int krffs_getattr(const char *path, struct PLATFORM_STAT *stat_buffer);

/** Lists the files and directories at `path`. */
int krffs_readdir(const char *path, void *buffer, fuse_fill_dir_t filler, PLATFORM_OFF_T offset,
                  struct fuse_file_info *file_info);

/**
 * Verifies the file at `path` exists in the mounted file system.
 *
 * The FUSE `open` handler. Returns `-ENOENT` if there is no file system or
 * `path` names no file; it stores no handle in `file_info`.
 */
int krffs_open(const char *path, struct fuse_file_info *file_info);

/** Reads `size` bytes at `offset` from the file at `path` into `buffer`. */
int krffs_read(const char *path, char *buffer, size_t size, PLATFORM_OFF_T offset,
               struct fuse_file_info *file_info);

/** Creates a file node (anything but a directory or a link) at `path`. */
int krffs_mknod(const char *path, mode_t mode, dev_t device);

/** Creates a regular file at `path` and opens it. */
int krffs_create(const char *path, mode_t mode, struct fuse_file_info *file_info);

/** Writes `size` bytes from `buffer` at `offset` to the file at `path`. */
int krffs_write(const char *path, const char *buffer, size_t size, PLATFORM_OFF_T offset,
                struct fuse_file_info *file_info);

/**
 * Succeeds for an existing file at `path`; writes go straight to the mapping,
 * so there is no buffer to flush.
 */
int krffs_fsync(const char *path, int is_data_sync, struct fuse_file_info *file_info);

/** Changes the size of the file at `path` to `new_size`. */
int krffs_truncate(const char *path, PLATFORM_OFF_T new_size);

/** Renames or moves the file at `path` to `new_path`. */
int krffs_rename(const char *path, const char *new_path);

/** Removes the file at `path`. */
int krffs_unlink(const char *path);

/** Updates the access and modification times for `path`. */
int krffs_utimens(const char *path, const struct timespec time[2]);

/** Sets the file permissions for `path`. */
int krffs_chmod(const char *path, mode_t mode);

/** Changes the owner or group of the file at `path`. */
int krffs_chown(const char *path, uid_t uid, gid_t gid);

/** Returns file system information for the store that has `path`. */
int krffs_statfs(const char *path, struct statvfs *statvfs_buffer);

/* The file system handlers FUSE dispatches to, by operation. */
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
