/* The directories and symbolic links root makes under /dev (devtree.c). */
#ifndef _DEVTREE_H_
#define _DEVTREE_H_

#include "../lib/kint.h"

/*
 * devtree: the directories and symbolic links that root makes under /dev,
 * kept beside devfs's devices as Linux's devtmpfs keeps what udev makes
 * beside the kernel's nodes: /dev/char/13:64, /dev/input/by-path/... and
 * the like. An entry has an id above 0, never given twice, and a parent:
 * /dev itself (DEVTREE_ROOT), a directory of /dev's table (DEVTREE_ROW,
 * synthfs.h), or a directory made here. The names devfs and synthfs give
 * are not looked up here; the caller refuses a new entry that would take
 * one (`EEXIST`). The functions run under the big kernel lock, and only
 * the allocation of a new entry may sleep, before anything is changed.
 */

#define DEVTREE_ROOT   0
#define DEVTREE_ROW(r) (-1 - (r))
/* Not a directory of /dev: what `synth_dev_parent` gives for any other node. */
#define DEVTREE_NONE   (-0x7fff)

/* The inodes of the entries, above every number devfs and devpts give. */
#define DEVTREE_INO_BASE 0x40000000u

/* The most entries at once; a new one past it is ENOSPC. */
#define DEVTREE_MAX 4096

/* What stat(2) reports of an entry. */
struct devtree_attr {
    uint32_t mode; /* S_IFDIR or S_IFLNK and the permission bits */
    uint32_t uid;
    uint32_t gid;
    uint32_t ino;
    uint32_t nlink; /* 2 and one for each subdirectory for a directory, else 1 */
    uint32_t size;  /* a link's target's length, 0 for a directory */
    int64_t atime, mtime, ctime;
    int64_t atime_ns, mtime_ns, ctime_ns;
};

/** The id of the entry named by the `len` bytes at `name` in `parent`, or
 * 0 for none. */
int devtree_child(int parent, const char *name, uint32_t len);

/**
 * Find the entry named by the path `rel` below /dev, its `len` bytes, such
 * as "char/13:64" or "input/by-path/NAME": the first name may be a row of
 * /dev's table. The id, -ENOENT for a name not there, or -ENOTDIR for a
 * path that goes on past an entry that is no directory.
 */
int devtree_resolve(const char *rel, uint32_t len);

/** Whether the entry `id` is there and is a directory: 1, 0 for a link,
 * -ENOENT for none. */
int devtree_is_dir(int id);

/** Fill `a` with the attributes of the entry `id`: 0, or -ENOENT. */
int devtree_attr(int id, struct devtree_attr *a);

/** Copy the target of the link `id`, without a NUL, into `buf` of `cap`
 * bytes, cut to fit: its whole length, -ENOENT, or -EINVAL for a
 * directory. */
int devtree_readlink(int id, char *buf, uint32_t cap);

/** Write the path of the entry `id` from the root, "/dev/char" for one,
 * into `out` of `cap` bytes, NUL-terminated and cut to fit; "/" for an
 * entry that is gone. */
void devtree_path(int id, char *out, int cap);

/**
 * The `n`th entry of `parent`, in the order they were made: its name, inode
 * and directory type, `VFS_DT_DIR` or `VFS_DT_LNK`. 1, or 0 past the last.
 * The name stays valid until the entry is removed or renamed.
 */
int devtree_dirent(int parent, int n, const char **name, uint32_t *ino, uint8_t *dtype);

/**
 * Make the entry `name` in `parent`: a directory with the permission bits
 * of `mode` when `target` is NULL, else a link to the `target` text, owned
 * by `uid` and `gid`. The new id, -ENOENT for a parent
 * that is gone, -EEXIST for a name taken here, -EINVAL for "." or "..",
 * -ENAMETOOLONG for a name past 255 bytes or a target past 4095, -ENOSPC
 * past DEVTREE_MAX entries, or -ENOMEM.
 */
int devtree_make(int parent, const char *name, uint32_t mode, const char *target, uint32_t uid,
                 uint32_t gid);

/**
 * Remove the entry `name` of `parent`: a directory when `dir`, which must
 * be empty, else a link. 0, -ENOENT, -ENOTDIR for a link asked for as a
 * directory, -EISDIR for a directory asked for as a link, or -ENOTEMPTY.
 */
int devtree_remove(int parent, const char *name, int dir);

/**
 * Move the entry `oname` of `from` to `nname` in `to`, replacing an entry
 * there as rename(2) does: a directory only by an empty directory
 * (-ENOTDIR, -ENOTEMPTY) and a link only by an entry that is no directory
 * (-EISDIR); with `noreplace` an entry there is -EEXIST. A directory moved
 * below itself is -EINVAL. 0, -ENOENT, -ENAMETOOLONG or -ENOMEM.
 */
int devtree_rename(int from, const char *oname, int to, const char *nname, int noreplace);

/** Set the permission bits of `id` to `mode` unless it is (uint32_t)-1,
 * and its owner and group unless each is (uint32_t)-1, and stamp its
 * change time. 0, or -ENOENT. */
int devtree_setattr(int id, uint32_t mode, uint32_t uid, uint32_t gid);

#endif
