#ifndef _VFS_H_
#define _VFS_H_

#include "../block/block.h"
#include "../proc/wait_queue.h"

/*
 * vfs: the mount table, the resolver, and the operation interface every
 * mounted file system answers.
 *
 * The table has eight entries. Entry zero is the root; entries one and two
 * are the two synthetic namespaces, /dev and /proc. The three are permanent:
 * umount2 on any of them answers EBUSY. The rest are placed by mount(2) and
 * removed by umount2.
 *
 * A mount point is the covered directory's node. It is held through
 * the parent file system's `hold`, so it cannot be reached under another
 * spelling, it is what the resolver climbs back to for `..` at a mount root,
 * and it answers EBUSY to unlink, rmdir and rename while the mount exists.
 *
 * Every path-taking syscall resolves through `vfs_resolve`, which walks one
 * component at a time: a lookup of one name in one directory on one mount,
 * after the caller's search permission on that directory has been checked
 * here, once, for every file system. `..` is physical: at a mount root it
 * continues at the parent's covered node, at the root of the root mount it
 * stays, and anywhere else it is a `lookup` of "..". A path that names a
 * missing or unsearchable directory before its `..` fails.
 *
 * The two synthetic types are the one exception: a type whose ops set
 * `lookup_rest` instead of `lookup` is handed the remainder of the path in
 * one call, because its namespace is a flat table classified by string. A
 * `..` inside that remainder that would climb above the mount root is
 * answered with `VFS_CLIMB`, and the resolver continues at the parent's
 * covered node. A `lookup_rest` type may not host a mount point.
 *
 * An operation a file system does not implement is a NULL pointer, and the
 * common layer answers the standard error for it: EROFS for a write-side
 * operation, EPERM for `link` on a writable mount whose type has no links,
 * EINVAL for `readlink` on something that is not a link.
 *
 * Lock order, fixed and checked: the mount table, then a mount's namespace
 * lock, then a description, then cache buffers in ascending block order.
 * A process holding one may take only what comes after it.
 */

#define VFS_MOUNTS 8
#define VFS_COVERED_PATH_MAX 64 /* a longer target answers ENAMETOOLONG */
#define VFS_NAME_MAX 255        /* one component, NUL not counted */

/* Mount states. A transient operation runs only on an active mount; the
 * mounting and unmounting states refuse new work with EBUSY. */
#define VFS_FREE 0
#define VFS_MOUNTING 1
#define VFS_ACTIVE 2
#define VFS_UNMOUNTING 3

/* The mount(2) flags this kernel honours or records, with Linux's values.
 * MS_RDONLY, MS_NOSUID and MS_NOEXEC are enforced in the common layer;
 * MS_NODEV is recorded and not enforced, since no type here has device
 * nodes; the atime group and MS_SILENT are recorded because nothing here
 * updates access times. Any other flag answers EINVAL. */
#define VFS_MS_RDONLY 0x0001u
#define VFS_MS_NOSUID 0x0002u
#define VFS_MS_NODEV 0x0004u
#define VFS_MS_NOEXEC 0x0008u
#define VFS_MS_NOATIME 0x0400u
#define VFS_MS_NODIRATIME 0x0800u
#define VFS_MS_SILENT 0x8000u
#define VFS_MS_RELATIME 0x200000u

/* What `lookup_rest` returns when `..` would climb above the mount root. */
#define VFS_CLIMB 1

struct vfs_mount;

/* What getattr reports and setattr changes. Times are seconds and
 * nanoseconds; `mode` includes the S_IF type bits. */
struct vfs_stat {
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint32_t nlink;
    uint64_t ino;
    uint64_t size;
    uint32_t rdev;
    int64_t atime_sec;
    int64_t atime_nsec;
    int64_t mtime_sec;
    int64_t mtime_nsec;
    int64_t ctime_sec;
    int64_t ctime_nsec;
};

/* Which fields a setattr call is changing. */
#define VFS_SET_MODE 0x01
#define VFS_SET_UID 0x02
#define VFS_SET_GID 0x04
#define VFS_SET_ATIME 0x08
#define VFS_SET_MTIME 0x10
#define VFS_SET_SIZE 0x20

/* One directory entry, as readdir fills it. */
struct vfs_dirent {
    char name[VFS_NAME_MAX + 1];
    uint64_t ino;
    int is_dir;
};

/* What statfs reports; the syscall layer shapes it into linux_statfs64. */
struct vfs_statfs {
    uint32_t magic;
    uint32_t bsize;
    uint64_t blocks;
    uint64_t bfree;
    uint32_t namelen;
};

/*
 * The operations, one set per type. A node handle is the file system's own: a
 * place in the image for KRFFS, a pool entry for FAT32, a pooled classified
 * path for the synthetic types. Whoever receives a node from `lookup`,
 * `create` or the mount's root owns one hold on it and must `put` it; open-
 * file descriptions, working directories and covering mounts hold theirs for
 * as long as they refer to it. For a type with empty `hold` and `put` the
 * pointers may be NULL.
 */
struct vfs_ops {
    /* One name in one directory, `..` included; `dir` NULL is the mount
     * root. The node comes back held. */
    int (*lookup)(struct vfs_mount *mnt, void *dir, const char *name,
                  void **node);
    /* The synthetic exception: the whole remaining path in one call.
     * Returns 0 with a held node, a negative errno, or VFS_CLIMB with
     * `*left` at the unconsumed remainder, and the resolver continues at
     * the covered node. */
    int (*lookup_rest)(struct vfs_mount *mnt, const char *rest, void **node,
                       const char **left);
    void (*hold)(struct vfs_mount *mnt, void *node);
    void (*put)(struct vfs_mount *mnt, void *node);
    int (*getattr)(struct vfs_mount *mnt, void *node, struct vfs_stat *st);
    int (*setattr)(struct vfs_mount *mnt, void *node, int what,
                   const struct vfs_stat *st);
    /* Once per successful open(2); `release` once at the description's last
     * close. The handle is the per-open object; a type that needs none
     * returns its node. Every type counts opens and releases, and the
     * record written at unmount says both numbers. */
    int (*open)(struct vfs_mount *mnt, void *node, int flags, void **handle);
    void (*release)(struct vfs_mount *mnt, void *handle);
    int32_t (*read)(struct vfs_mount *mnt, void *handle, void *buf,
                    uint32_t count, uint64_t offset);
    int32_t (*write)(struct vfs_mount *mnt, void *handle, const void *buf,
                     uint32_t count, uint64_t offset);
    int (*truncate)(struct vfs_mount *mnt, void *handle, uint64_t size);
    int (*fsync)(struct vfs_mount *mnt, void *handle);
    /* One directory entry at `*cursor`, a position whose meaning belongs
     * to the file system (an image offset, a directory-entry index); the
     * call advances it past the entry it returns. A caller stores it
     * between calls and never interprets it. 1 with the entry filled, 0 at
     * the end, or a negative errno. */
    int (*readdir)(struct vfs_mount *mnt, void *handle, uint32_t *cursor,
                   struct vfs_dirent *ent);
    /* The namespace. `rename` is within one mount; the common layer answers
     * EXDEV across mounts. */
    int (*create)(struct vfs_mount *mnt, void *dir, const char *name,
                  uint32_t mode, void **node);
    int (*unlink)(struct vfs_mount *mnt, void *dir, const char *name);
    int (*mkdir)(struct vfs_mount *mnt, void *dir, const char *name,
                 uint32_t mode);
    int (*rmdir)(struct vfs_mount *mnt, void *dir, const char *name);
    int (*rename)(struct vfs_mount *mnt, void *olddir, const char *oldname,
                  void *newdir, const char *newname);
    int (*link)(struct vfs_mount *mnt, void *dir, const char *name,
                void *node);
    int (*readlink)(struct vfs_mount *mnt, void *node, char *buf,
                    uint32_t cap);
    int (*statfs)(struct vfs_mount *mnt, struct vfs_statfs *st);
    int (*sync)(struct vfs_mount *mnt);
    /* The two halves of an unmount. The first writes the volume's last
     * metadata into cache buffers and dismantles nothing, so it can be
     * repeated or abandoned; the second frees the private state and is not
     * undone. The buffer cache is written between them. */
    int (*prepare_unmount)(struct vfs_mount *mnt);
    void (*unmount)(struct vfs_mount *mnt);
};

/* One file-system kind: the name mount(2) and /proc/filesystems spell, the
 * constructor, and the operations. The constructor validates the volume,
 * parses `options`, sets `mnt->priv` and `mnt->root`, and returns; one that
 * fails leaves nothing behind. */
struct vfs_type {
    const char *name;
    int (*mount)(struct vfs_mount *mnt, block_device_t *dev, uint32_t flags,
                 const char *options);
    const struct vfs_ops *ops;
};

/* One table entry. `covered` is the held node of the directory this mount
 * covers, on `parent`; the root and the two synthetic mounts cover nothing.
 * `refs` counts descriptions on the mount, working directories in it, and
 * child mounts covering a node on it. `inprogress` counts operations in
 * flight, which is what unmount drains. */
struct vfs_mount {
    const struct vfs_type *type;
    block_device_t *dev; /* NULL for the root window and the synthetics */
    uint32_t flags;
    int state;
    struct vfs_mount *parent;
    void *covered;
    char covered_path[VFS_COVERED_PATH_MAX];
    uint32_t refs;
    uint32_t inprogress;
    int ns_holder; /* pid holding the namespace lock, or -1 */
    wait_queue_t ns_wq;
    void *root; /* the mount's root node, held for the mount's life */
    void *priv;
    /* What the unmount record reports: every open matched by a release. */
    uint32_t opens;
    uint32_t releases;
    int claim;        /* the devfs claim on the source's sectors, or -1 */
    char source[32];  /* the source as mount(2) spelled it, for /proc/mounts */
};

/* A held place: one node on one mount. The resolver returns these; whoever
 * receives one puts it back through `vfs_ref_put`. */
struct vfs_ref {
    struct vfs_mount *mnt;
    void *node;
};

/* Resolver flags. WALK_PARENT stops before the last component and returns
 * the held parent directory with the leaf name copied out, which is what
 * create, unlink, mkdir, rmdir and rename start from. WALK_REAL_IDS charges
 * search permission by the real ids, for access(2). */
#define VFS_WALK_PARENT 0x1
#define VFS_WALK_REAL_IDS 0x2

/**
 * Install the three permanent mounts. Called once the root file system is
 * up; until then no resolver runs.
 */
void vfs_init(void);

/**
 * Resolve `path` to a held node.
 *
 * `start` is where the walk begins: with NULL it is the root of the root
 * mount whether or not `path` begins with '/', because the syscall layer
 * joins a relative path to the working directory's canonical spelling
 * before resolving and strips the leading slash; an `*at` call may pass
 * its directory descriptor's place instead. On success `out` receives the
 * node and the mount it lives on, and the caller owns the hold. With
 * `VFS_WALK_PARENT`, `out` is the held parent directory, and the final
 * component, which may not exist yet, is copied into `leaf` (of `leafcap`
 * bytes; ENAMETOOLONG if it does not fit).
 */
int vfs_resolve(const struct vfs_ref *start, const char *path, int flags,
                struct vfs_ref *out, char *leaf, uint32_t leafcap);

/** Take another hold on `r`'s node. */
void vfs_ref_hold(const struct vfs_ref *r);

/** Put a held node back; the ref is dead after. */
void vfs_ref_put(struct vfs_ref *r);

/** Put the node back and leave the mount: the pair every resolution ends
 * with. `vfs_resolve` returns with the mount's in-progress count taken, so
 * the mount cannot unmount while the caller still uses the node. */
void vfs_ref_release(struct vfs_ref *r);

/** The table entry at `idx` (0..VFS_MOUNTS-1), or NULL when free; for
 * /proc/mounts and /proc/filesystems. */
struct vfs_mount *vfs_mount_at(int idx);

/** The three permanent entries, for the syscall layer's direct needs. */
struct vfs_mount *vfs_root_mount(void);

/** The type mount(2) accepts at index `idx` of four, or NULL; the order
 * /proc/filesystems lists them in. */
const struct vfs_type *vfs_type_at(int idx);

/**
 * The working directory's hold on its mount: a process whose directory is
 * inside a mount counts as a reference on it, so the mount cannot be
 * unmounted from under it. The permanent mounts need no pin and take none.
 * `vfs_cwd_set` replaces the previous hold; `vfs_cwd_clear` drops it.
 */
struct process;
void vfs_cwd_set(struct process *p, struct vfs_mount *mnt, void *node);
void vfs_cwd_clear(struct process *p);

/**
 * Count one operation in progress on `mnt`; refused with -EBUSY unless the
 * mount is active. The unmount path is the one path that does not take it,
 * because it is the path that waits for the count to reach zero.
 */
int vfs_enter(struct vfs_mount *mnt);
void vfs_leave(struct vfs_mount *mnt);

/** The per-mount namespace lock, taken by every namespace change and by the
 * constructor and the unmount; sleeps until free. */
void vfs_ns_lock(struct vfs_mount *mnt);
void vfs_ns_unlock(struct vfs_mount *mnt);

/**
 * mount(2) and umount2(2), after the syscall layer has copied the strings.
 * The order of work and every error they answer are in the syscall's
 * contract; both require effective uid 0, checked by the caller.
 */
int vfs_do_mount(const char *source, const char *target, const char *type,
                 uint32_t flags, const char *options);
int vfs_do_umount(const char *target);

/**
 * A description opened on `mnt`: the mount's reference count goes up; the
 * node's hold is the one the resolution already took, moved rather than
 * retaken. `vfs_desc_close` is the last close: release the handle, put the
 * node, drop the reference.
 */
void vfs_desc_open(struct vfs_mount *mnt);
void vfs_desc_close(struct vfs_mount *mnt, void *node, void *handle);

/**
 * Repoint every reference to `from` on `mnt` to `to`: descriptions, working
 * directories, directory descriptors and covering mounts. KRFFS calls it
 * when a relocation moves a node.
 */
void vfs_repoint(struct vfs_mount *mnt, void *from, void *to);

/* The default-answer wrappers: each checks the mount flags and the NULL
 * operation and answers the contract's errno, so the rule exists once. The
 * write-side wrappers answer -EROFS on a read-only mount. */
int32_t vfs_read(struct vfs_mount *mnt, void *handle, void *buf,
                 uint32_t count, uint64_t offset);
int32_t vfs_write(struct vfs_mount *mnt, void *handle, const void *buf,
                  uint32_t count, uint64_t offset);
int vfs_truncate(struct vfs_mount *mnt, void *handle, uint64_t size);
int vfs_fsync(struct vfs_mount *mnt, void *handle);
int vfs_getattr(struct vfs_mount *mnt, void *node, struct vfs_stat *st);
int vfs_setattr(struct vfs_mount *mnt, void *node, int what,
                const struct vfs_stat *st);
int vfs_open_node(struct vfs_mount *mnt, void *node, int flags,
                  void **handle);
void vfs_release(struct vfs_mount *mnt, void *handle);
int vfs_readdir(struct vfs_mount *mnt, void *handle, uint32_t *cursor,
                struct vfs_dirent *ent);
int vfs_create(struct vfs_mount *mnt, void *dir, const char *name,
               uint32_t mode, void **node);
int vfs_unlink_name(struct vfs_mount *mnt, void *dir, const char *name);
int vfs_mkdir(struct vfs_mount *mnt, void *dir, const char *name,
              uint32_t mode);
int vfs_rmdir(struct vfs_mount *mnt, void *dir, const char *name);
int vfs_rename(struct vfs_mount *mnt, void *olddir, const char *oldname,
               void *newdir, const char *newname);
int vfs_link(struct vfs_mount *mnt, void *dir, const char *name, void *node);
int vfs_statfs(struct vfs_mount *mnt, struct vfs_statfs *st);

/**
 * The mount whose root stands at `name` inside directory `node` of `mnt`,
 * or NULL. This is how a namespace change sees a mount point the type's
 * own lookup cannot know: mkdir answers EEXIST for it, unlink and rmdir
 * and rename answer EBUSY.
 */
struct vfs_mount *vfs_leaf_mount(struct vfs_mount *mnt, void *node,
                                 const char *name);

/** The classification stored in a synthetic node, or NULL when `mnt` is not
 * one of the two synthetic mounts. */
struct vpath;
const struct vpath *vfs_synth_vpath(struct vfs_mount *mnt, void *node);

/* The three permanent types: the root's adapter and the two synthetic
 * namespaces. */
extern const struct vfs_type vfs_krffs_type;
extern const struct vfs_type vfs_devfs_type;
extern const struct vfs_type vfs_procfs_type;

#endif /* _VFS_H_ */
