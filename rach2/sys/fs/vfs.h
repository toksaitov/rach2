#ifndef _VFS_H_
#define _VFS_H_

/*
 * The VFS: the mount table, the resolver, and the operations every file
 * system type provides, with the wrappers that answer for an operation a
 * type does not have.
 *
 * The table is an array of records from the heap that grows as mounts are
 * made. Its first VFS_PERMANENT entries are made at boot and never
 * unmounted; the rest are placed by mount(2) and removed by umount2(2).
 *
 * A mount over a directory covers the directory's node, held through the
 * parent type's `hold` for the life of the mount: the resolver replaces the
 * node with the mount's root, `..` at the mount's root climbs back to it,
 * and unlink, rmdir and rename answer EBUSY for it while the mount exists.
 * A nameless mount, such as /dev, /proc, /sys, /dev/shm or /dev/mqueue,
 * covers no node and is entered by its name from its parent's root
 * directory.
 *
 * `vfs_resolve` walks a path one component at a time: a lookup of one name
 * in one directory on one mount, after the caller's search permission on
 * that directory is checked here, in the same way for every type. `..` is
 * physical: at the root of a mount over a directory it is taken again at
 * the covered node, at the root of a nameless mount it is the parent's root
 * directory, at the walk's root and at the root of the root mount it stays,
 * and anywhere else it is the type's `lookup` of "..". A path that names a
 * missing or unsearchable directory before its `..` fails.
 *
 * The three synthetic types, of /dev, /proc and /sys, set `lookup_rest` in
 * place of `lookup` and are handed the rest of the path in one call, since
 * each namespace is a table classified by string. A `..` in the rest that
 * would climb above the mount's root is answered with VFS_CLIMB, and the
 * walk takes that `..` at the mount's root as at the root of any other
 * mount; a `..` first at that root is never handed to the type. A rest that
 * comes back to the mount's root directory in front of the name of a
 * nameless mount there is answered with VFS_ROOT, and the walk enters that
 * mount by its name. No mount covers a node of such a type.
 *
 * An operation a type does not have is a NULL pointer, and the wrapper of
 * the operation, below, gives the default answer for it.
 *
 * The lock order is a mount's namespace lock, then an open file
 * description's lock (fd.h), then buffers of the cache by the cache's rules
 * (bcache.h).
 */

#include "../block/block.h"
#include "../proc/wait_queue.h"

#define VFS_COVERED_PATH_MAX 64  /* NUL included; ENAMETOOLONG past it */
#define VFS_NAME_MAX         255 /* one component, NUL not counted */

/* Mount states. An operation enters only an active mount (`vfs_enter`); a
 * mount being made or unmounted answers EBUSY. */
#define VFS_FREE       0
#define VFS_MOUNTING   1
#define VFS_ACTIVE     2
#define VFS_UNMOUNTING 3

/* The mount(2) flags this kernel honours or records, with Linux's values.
 * The wrappers below enforce MS_RDONLY; execve enforces MS_NOSUID and
 * MS_NOEXEC, and open MS_NODEV, with EACCES for a device node.
 * MS_SYNCHRONOUS and MS_DIRSYNC make ext2 put each metadata write on the
 * medium before the next. The access-time flags and MS_SILENT are recorded
 * and change nothing. Any other flag is EINVAL. */
#define VFS_MS_RDONLY      0x0001u
#define VFS_MS_NOSUID      0x0002u
#define VFS_MS_NODEV       0x0004u
#define VFS_MS_NOEXEC      0x0008u
#define VFS_MS_SYNCHRONOUS 0x0010u
#define VFS_MS_DIRSYNC     0x0080u
#define VFS_MS_NOATIME     0x0400u
#define VFS_MS_NODIRATIME  0x0800u
#define VFS_MS_SILENT      0x8000u
#define VFS_MS_RELATIME    0x200000u

/* What `lookup_rest` returns when `..` would climb above the mount root, and
 * when the remainder reaches, at the mount's root directory, the name of a
 * nameless mount there. */
#define VFS_CLIMB 1
#define VFS_ROOT  2

struct vfs_mount;

/* What getattr reports and setattr changes. Times are seconds and
 * nanoseconds; `mode` includes the S_IF type bits. `blocks` is the 512-byte
 * units the node takes on its medium, which stat reports as st_blocks, when
 * `blocks_known` is set; a type that does not count them leaves it clear,
 * and stat rounds the size up instead. */
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
    uint64_t blocks;
    uint32_t blocks_known;
    uint32_t flags;       /* the inode flags of FS_IOC_GETFLAGS */
    uint32_t generation;  /* and the generation of FS_IOC_GETVERSION */
    uint32_t flags_known; /* 0 for a type that keeps neither */
};

/* Which fields a setattr call is changing. */
#define VFS_SET_MODE  0x01
#define VFS_SET_UID   0x02
#define VFS_SET_GID   0x04
#define VFS_SET_ATIME 0x08
#define VFS_SET_MTIME 0x10
#define VFS_SET_SIZE  0x20
#define VFS_SET_FLAGS 0x40 /* the inode flags of FS_IOC_SETFLAGS */

/* The inode flags the VFS enforces, as Linux names them: no change to the
 * file at all, and its data only appended to. */
#define VFS_FL_IMMUTABLE 0x00000010u
#define VFS_FL_APPEND    0x00000020u

/*
 * An entry's file type as getdents64 reports it in `d_type`: the type bits
 * of its mode shifted down twelve, which is what Linux's DT_* values are,
 * or VFS_DT_UNKNOWN when the file system cannot tell without more reading
 * than a listing does.
 */
#define VFS_DT_UNKNOWN  0
#define VFS_DT_DIR      4
#define VFS_DT_REG      8
#define VFS_DT_OF(mode) (((mode) >> 12) & 017)

/* One directory entry, as readdir fills it: `type` is a VFS_DT_* value,
 * VFS_DT_UNKNOWN until the type fills it, since `vfs_readdir` zeroes the
 * record first. */
struct vfs_dirent {
    char name[VFS_NAME_MAX + 1];
    uint64_t ino;
    int is_dir;
    int type;
};

/* What statfs reports; the system call layer copies it out as a
 * `struct user_statfs`. */
struct vfs_statfs {
    uint32_t magic;
    uint32_t bsize;
    uint64_t blocks;
    uint64_t bfree;
    uint64_t reserved; /* of the free blocks, those only a privileged writer takes */
    uint64_t files;    /* the inodes, and the free ones, or 0 for a type that */
    uint64_t ffree;    /* has no fixed count of them */
    uint32_t namelen;
};

/*
 * The operations, one set per type. A node handle is the type's own: an
 * address in the mapped image for the KRFFS root, an offset plus one for a
 * KRFFS volume, a heap record on the mount's list for FAT32, exFAT and
 * ext2, a heap record of a classified path for the synthetic types.
 * Whoever receives a node from `lookup`, `create`, `mknod` or a mount's
 * root owns one hold on it and must `put` it; open file descriptions,
 * working directories, process roots and covering mounts keep theirs for as
 * long as they refer to it. For a type with empty `hold` and `put` the
 * pointers may be NULL.
 */
struct vfs_ops {
    /* One name in one directory, `..` included; `dir` NULL is the mount
     * root. The node comes back held. */
    int (*lookup)(struct vfs_mount *mnt, void *dir, const char *name, void **node);
    /* In place of `lookup`, for a synthetic type: the rest of the path in
     * one call. 0 with the node held; VFS_CLIMB with `*left` at a `..`
     * that climbs above the mount root, which the walk takes there;
     * VFS_ROOT with `*left` at the name of a nameless mount at the mount
     * root, which the walk enters; or a negative errno. */
    int (*lookup_rest)(struct vfs_mount *mnt, const char *rest, void **node, const char **left);
    void (*hold)(struct vfs_mount *mnt, void *node);
    void (*put)(struct vfs_mount *mnt, void *node);
    int (*getattr)(struct vfs_mount *mnt, void *node, struct vfs_stat *st);
    int (*setattr)(struct vfs_mount *mnt, void *node, int what, const struct vfs_stat *st);
    /* Once for each open of the node, which `vfs_open_node` makes for a
     * description without O_PATH, a truncate(2), a program being run and a
     * loop device; `release` once at the end of that open, a description's
     * last close. The handle is the per-open object; a type that needs none
     * returns its node. `vfs_open_node` and `vfs_release` count the two, and
     * the record written at unmount gives both numbers. */
    int (*open)(struct vfs_mount *mnt, void *node, int flags, void **handle);
    void (*release)(struct vfs_mount *mnt, void *handle);
    int32_t (*read)(struct vfs_mount *mnt, void *handle, void *buf, uint32_t count,
                    uint64_t offset);
    int32_t (*write)(struct vfs_mount *mnt, void *handle, const void *buf, uint32_t count,
                     uint64_t offset);
    int (*truncate)(struct vfs_mount *mnt, void *handle, uint64_t size);
    int (*fsync)(struct vfs_mount *mnt, void *handle);
    /* One directory entry at `*cursor`, a position whose meaning belongs
     * to the file system (an image offset, a directory-entry index); the
     * call advances it past the entry it returns. A caller stores it
     * between calls and never interprets it. 1 with the entry filled, 0 at
     * the end, or a negative errno. */
    int (*readdir)(struct vfs_mount *mnt, void *handle, uint32_t *cursor, struct vfs_dirent *ent);
    /* The namespace. `rename` is within one mount; the system call layer
     * answers EXDEV across mounts. */
    int (*create)(struct vfs_mount *mnt, void *dir, const char *name, uint32_t mode, void **node);
    int (*unlink)(struct vfs_mount *mnt, void *dir, const char *name);
    int (*mkdir)(struct vfs_mount *mnt, void *dir, const char *name, uint32_t mode);
    int (*rmdir)(struct vfs_mount *mnt, void *dir, const char *name);
    int (*rename)(struct vfs_mount *mnt, void *olddir, const char *oldname, void *newdir,
                  const char *newname);
    int (*link)(struct vfs_mount *mnt, void *dir, const char *name, void *node);
    int (*readlink)(struct vfs_mount *mnt, void *node, char *buf, uint32_t cap);
    int (*statfs)(struct vfs_mount *mnt, struct vfs_statfs *st);
    int (*sync)(struct vfs_mount *mnt);
    /* The two halves of an unmount. The first writes the volume's last
     * metadata into cache buffers and dismantles nothing, so it can be
     * repeated or abandoned; the second frees the private state and is not
     * undone. The buffer cache is written between them.
     *
     * `abort_unmount` runs when an unmount that got past `prepare_unmount`
     * fails at the writeback, the flush or the cache's drop of the device:
     * the mount stays, and the type puts back what it wrote for the
     * unmount. NULL for a type with nothing to put back. */
    int (*prepare_unmount)(struct vfs_mount *mnt);
    void (*abort_unmount)(struct vfs_mount *mnt);
    void (*unmount)(struct vfs_mount *mnt);
    /* The canonical path of a directory node within the mount, for a
     * directory descriptor's `*at` calls after a rename and for
     * `vfs_node_path`; NULL for a type whose directories do not move. 0,
     * -ENAMETOOLONG, or -ENOENT for a directory that has no name. */
    int (*path)(struct vfs_mount *mnt, void *node, char *buf, uint32_t cap);
    /* A symbolic link `name` in `dir` to `target`; NULL on a type without
     * them, which the common layer answers with EPERM. */
    int (*symlink)(struct vfs_mount *mnt, void *dir, const char *name, const char *target);
    /* A FIFO, a socket or a device node `name` in `dir`, by the S_IF type
     * bits of `mode`, with the device number `rdev` for a device. The node
     * comes back held, as `create` returns one. A type answers EPERM for a
     * kind it cannot keep; NULL on a type with none, which the common layer
     * answers with EPERM. */
    int (*mknod)(struct vfs_mount *mnt, void *dir, const char *name, uint32_t mode, uint32_t rdev,
                 void **node);
    /* Keep `node` for a description that opens nothing, an O_PATH one, on a
     * type that keeps a node only while it is open: an unlink or a rename
     * over the node then leaves it without a name, and the last `unpin`
     * frees it, as the last release of an unlinked open file does. `*token`
     * is what `unpin` takes; it stays valid when the node moves. NULL for a
     * type whose `hold` keeps a node, and for the KRFFS boot root, whose
     * descriptors the syscall layer counts. */
    int (*pin)(struct vfs_mount *mnt, void *node, void **token);
    void (*unpin)(struct vfs_mount *mnt, void *token);
    /* Make the mount read-only or writable, by VFS_MS_RDONLY in its new
     * `flags`, for mount(2) with MS_REMOUNT, before the VFS stores them: a
     * mount made read-only is synced and left as an unmount leaves its
     * volume, and one made writable is set up as a writable mount is; and
     * take the options of `options`, mount(2)'s data, NULL for none, that
     * the type lets a remount change. Called for a change of VFS_MS_RDONLY
     * and for options that are not empty. NULL for a type that does
     * neither, whose remounts change only the other flags. */
    int (*remount)(struct vfs_mount *mnt, uint32_t flags, const char *options);
    /* fallocate(2) on an open file, `mode` its FALLOC_FL_ bits: allocate
     * [offset, offset + len) ahead of its writes, the size grown to its end
     * unless FALLOC_FL_KEEP_SIZE, or punch a hole there. A mode the type does
     * not take is -EOPNOTSUPP; NULL for a type that takes none, which the
     * common layer answers with -EOPNOTSUPP. */
    int (*fallocate)(struct vfs_mount *mnt, void *handle, uint32_t mode, uint64_t offset,
                     uint64_t len);
};

/* One file-system kind: the name mount(2) takes and /proc/filesystems
 * lists, the constructor, and the operations. The constructor validates the
 * volume, parses `options`, sets `mnt->priv` and `mnt->root`, and returns;
 * one that fails leaves nothing behind. */
struct vfs_type {
    const char *name;
    int (*mount)(struct vfs_mount *mnt, block_device_t *dev, uint32_t flags, const char *options);
    const struct vfs_ops *ops;
    /* Nonzero when a description's use of this file system must be taken
     * under the mount's namespace lock: KRFFS moves a file's bytes while it
     * is open, ext2 keeps a batch of dirty blocks between calls, and the
     * operations of tmpfs take no lock of their own. A file system with
     * none of these needs no lock, and taking it there would serialise
     * every reader of one volume. */
    int ns_serialised;
    /* Nonzero for a type that stands on no device, tmpfs, mqueue and the
     * three synthetic types: mount(2) takes any source string for one and
     * claims no sectors, and /proc/filesystems marks it `nodev`. */
    int nodev;
};

/* One table entry. `covered` is the held node of the directory this mount
 * covers, on `parent`; the root and the nameless mounts cover nothing.
 * `refs` counts what keeps the mount from being unmounted: the
 * descriptions on it, the working directories and process roots in it, the
 * child mounts under it and the binds that show it. `inprogress` counts
 * operations in flight, which is what unmount drains. */
struct vfs_mount {
    const struct vfs_type *type;
    block_device_t *dev; /* NULL for the KRFFS root and a type with no device */
    uint32_t flags;
    int state;
    struct vfs_mount *parent;
    void *covered;
    char covered_path[VFS_COVERED_PATH_MAX];
    uint32_t refs;
    uint32_t inprogress;
    int ns_holder;       /* pid holding the namespace lock, or -1 */
    int ns_depth;        /* how many times the holder has taken it */
    uint32_t ns_next;    /* the next place in the line for the lock */
    uint32_t ns_serving; /* the place whose turn it is */
    wait_queue_t ns_wq;
    void *root; /* the mount's root node, held for the mount's life */
    void *priv;
    /* What the unmount record reports: every open matched by a release. */
    uint32_t opens;
    uint32_t releases;
    int write_error; /* the error of the first failed write, 0 for none */
    int claim;       /* the devfs claim on the source's sectors, or -1 */
    char source[32]; /* the source string mount(2) was given, for /proc/mounts */
    /* The mount namespaces that do not see it, a bit each (proc/ns.h): a
     * mount made in one is seen there and in the namespaces made from it
     * afterwards, and a mount of the boot by all. */
    uint32_t ns_hidden;
    /* For a bind mount, the mount whose file system it shows, never itself
     * a bind, and the path of its root within that file system; NULL and
     * empty for every other mount. A bind has that mount's type, device,
     * private state and lock, and its own root, flags and place. */
    struct vfs_mount *bind_of;
    char bind_root[VFS_COVERED_PATH_MAX];
};

/* A held place: one node on one mount. Whoever receives one from the
 * resolver ends it with `vfs_ref_release`. */
struct vfs_ref {
    struct vfs_mount *mnt;
    void *node;
};

/* Resolver flags. WALK_PARENT stops before the last component and returns
 * the held parent directory with the leaf name copied out, which is what
 * create, unlink, mkdir, rmdir and rename start from. WALK_REAL_IDS judges
 * search permission by the real ids, for access(2). WALK_NOFOLLOW leaves a
 * symbolic link at the last component as the link, for lstat, readlink and
 * the calls told not to follow one, by AT_SYMLINK_NOFOLLOW, O_NOFOLLOW,
 * O_CREAT|O_EXCL and the like; a link earlier in the path is always
 * followed. WALK_LOCK takes the namespace locks, as `vfs_resolve` says. */
#define VFS_WALK_PARENT   0x1
#define VFS_WALK_REAL_IDS 0x2
#define VFS_WALK_NOFOLLOW 0x4
#define VFS_WALK_LOCK     0x8
/* Symbolic links followed along one path before ELOOP. */
#define VFS_LINK_MAX      40
/* A path, terminator included: Linux's PATH_MAX, one page. A path a caller
 * names may be this long, and so may the absolute path a relative one makes
 * with the directory it is resolved from, and a followed link's target with
 * the rest of the path after the link; ENAMETOOLONG past it. The kernel
 * copies paths into pages from fs/pathbuf.c, not onto its stack. */
#define VFS_PATH_MAX      4096

/**
 * Install the KRFFS root at entry zero and the other permanent mounts over
 * it, before any path is resolved. A later call installs nothing, and on a
 * KRFFS root points the entry at the image's current root directory.
 */
void vfs_init(void);

/**
 * Resolve `path` to a held node, in `out`.
 *
 * The walk starts at the calling process's root, the root of the mount
 * table unless chroot gave it another, when `path` begins with '/' or
 * `start` is NULL, and at `start`, the directory of an `*at` call,
 * otherwise. A path that begins with two slashes starts at the root of the
 * mount table and keeps the caller's root for `..` and absolute links. The
 * system call layer joins a relative path to the working directory's path
 * first and passes the result without its leading slash.
 *
 * On success `out` has the node, held, and the mount it is on, with that
 * mount's count of operations in progress taken, so the mount cannot be
 * unmounted while the caller uses the node; the caller ends both with
 * `vfs_ref_release`. With VFS_WALK_PARENT, `out` is the parent directory,
 * its search permission checked, and the last component, which need not
 * exist, is copied into `leaf` of `leafcap` bytes.
 *
 * VFS_WALK_LOCK takes each mount's namespace lock before its lookups,
 * releases it when the walk crosses to another mount, and returns with the
 * last mount's lock held, all only on a type that sets `ns_serialised`,
 * since that is what a description of a moving file needs and a type
 * without it loses the concurrency its readers are meant to have. The
 * caller releases the lock with `vfs_walk_unlock` once its description is
 * registered or its change made; `vfs_ref_release` does not. An error
 * returns with no lock held.
 *
 * Returns 0 or a negative errno: the type's, from `lookup`, `getattr` or
 * `readlink`; EACCES or ENOTDIR from the search check; ENAMETOOLONG for a
 * component over VFS_NAME_MAX, a leaf over `leafcap` or a link's expansion
 * over VFS_PATH_MAX; ELOOP past VFS_LINK_MAX links; ENOENT for an empty
 * link target or before a root is installed; EINVAL for a parent walk of a
 * path with no component; EIO on a mount whose device has gone; EBUSY on a
 * mount that is not active; and ENOMEM.
 */
int vfs_resolve(const struct vfs_ref *start, const char *path, int flags, struct vfs_ref *out,
                char *leaf, uint32_t leafcap);

/** Take another hold on `r`'s node, on a type that counts holds. */
void vfs_ref_hold(const struct vfs_ref *r);

/** Put back the hold on `r`'s node and clear `r`; a cleared `r` puts
 * nothing. The put may sleep. */
void vfs_ref_put(struct vfs_ref *r);

/** Put the node back and leave the mount: the pair every resolution ends
 * with. `vfs_resolve` returns with the mount's in-progress count taken, so
 * the mount cannot unmount while the caller still uses the node. */
void vfs_ref_release(struct vfs_ref *r);

/** How many mount numbers there are now; the array grows as mounts are made. */
int vfs_mount_slots(void);

/** The table entry at `idx` (0..vfs_mount_slots()-1), or NULL when free; for
 * the readers of the whole table, /proc/mounts among them. */
struct vfs_mount *vfs_mount_at(int idx);

/** Whether the caller's mount namespace sees the mount `m`. */
int vfs_mount_seen(const struct vfs_mount *m);

/** Entry zero, the root of the mount table. */
struct vfs_mount *vfs_root_mount(void);

/** Whether the device under `mnt` has gone, by its own flags or, for a
 * partition window, by those of the disk under it (`block_flags`); 0 for a
 * mount with no device. */
int vfs_mount_gone(const struct vfs_mount *mnt);

/** The mount whose file system `m` shows: the one a bind mount was made
 * from, or `m` itself. A file is the same file by its inode number on the
 * base mount through every mount that shows it. */
const struct vfs_mount *vfs_mount_base(const struct vfs_mount *m);

/** Whether a root is installed at entry zero, of any type. */
int vfs_root_mounted(void);

/**
 * Install a root of another type at entry zero before any process runs,
 * with the other permanent mounts over it as `vfs_init` places them: the
 * ext2 root, from a partition window the kernel found itself. `claim` is
 * the window's claim on its disk, kept with the entry so that a mount(2) of
 * the same partition is EBUSY. Returns 0, the constructor's error, after
 * which nothing is installed and the cache has dropped what it read of
 * `dev`, -ENOMEM, or -EBUSY once a root is installed.
 */
int vfs_init_root(const struct vfs_type *type, struct block_device *dev, const char *source,
                  int claim);

/** The `idx`-th type mount(2) finds by name, or NULL past the last; the
 * order /proc/filesystems lists them in. */
const struct vfs_type *vfs_type_at(int idx);

/**
 * The `n`-th nameless mount entered by name from the root directory of
 * `parent`, as /dev, /proc and /sys are from the root's and /dev/shm and
 * /dev/mqueue from /dev's: its name in `*name`, and the inode number of its
 * root, or 0 when the type reports none, in `*ino`. 1, or 0 past the last.
 * The synthetic directories list them with their own entries.
 */
int vfs_named_mount(const struct vfs_mount *parent, int n, const char **name, uint64_t *ino);

struct process;
/**
 * Make `node` of `mnt` the working directory of `p`. A working directory
 * inside a mount is a reference on it, so the mount cannot be unmounted
 * from under it, and holds its node on a type that counts holds; one in
 * /dev or /proc takes neither. The new directory is stored before the old
 * one is let go of, a put that may sleep.
 */
void vfs_cwd_set(struct process *p, struct vfs_mount *mnt, void *node);
/** Let go of the working directory of `p`, at its exit. */
void vfs_cwd_clear(struct process *p);

/**
 * Make `node` of `mnt` the root directory of `p`, as chroot does: where the
 * resolver starts an absolute path, stops `..` and restarts an absolute
 * symbolic link's target. The root holds its node and is a reference on
 * its mount, which unmount then answers EBUSY for. It is stored before the
 * root it replaces is let go of, a put that may sleep. NULL `mnt`, or the
 * root of the mount table itself, leaves the process with that root.
 */
void vfs_root_set(struct process *p, struct vfs_mount *mnt, void *node);
/** Give `child`, of fork, the root of `parent`. */
void vfs_root_copy(struct process *child, const struct process *parent);
/** Let go of the root of `p`, at its exit. */
void vfs_root_clear(struct process *p);

/**
 * Count one operation in progress on `mnt`; refused with -EBUSY unless the
 * mount is active. The unmount path is the one path that does not take it,
 * because it is the path that waits for the count to reach zero.
 */
int vfs_enter(struct vfs_mount *mnt);
/** End an operation `vfs_enter` counted, and wake an unmount waiting for
 * the count to reach zero. */
void vfs_leave(struct vfs_mount *mnt);

/**
 * Whether the calling process holds the namespace lock of any mount.
 *
 * The buffer cache checks it before it writes back a buffer of a device
 * that is a file, a loop device: that write takes the lock of the mount
 * that has the file, and a caller that already holds another mount's lock
 * could wait for that lock while its holder waits for the caller's.
 */
int vfs_ns_held_by_caller(void);

/** Take the namespace lock of `mnt`, which a bind shares with its base,
 * sleeping until it is free; a signal does not end the wait. It is taken by
 * every namespace change and by the constructor and the unmount. The holder
 * may take it again, since a type's own operations lock for the callers
 * that arrive unlocked (`write`, `sync`, a last `put`) and are also called
 * under it. */
void vfs_ns_lock(struct vfs_mount *mnt);
/** Release one taking of the lock; the last one lets the next waiter in. */
void vfs_ns_unlock(struct vfs_mount *mnt);

/**
 * Release what a `VFS_WALK_LOCK` walk took on `mnt`, if anything.
 *
 * The walk locks only a type that sets `ns_serialised`, so a caller that
 * unlocked unconditionally would release a lock it never held. Every exit
 * from a walk made with that flag goes through here.
 */
void vfs_walk_unlock(struct vfs_mount *mnt);

/**
 * mount(2), after the system call layer has checked the caller's privilege
 * and copied the strings: a mount of the type named `type` from `source`
 * over the directory `target` names, with `flags`, and `options` for the
 * constructor.
 *
 * The target is resolved and held, and its path and what it is are
 * checked. Then, under the parent mount's namespace lock, the target is
 * checked for a mount already over it, the source is resolved to a device
 * and its sectors claimed, and a slot is taken as VFS_MOUNTING. The
 * constructor runs under the new mount's own lock, and the mount becomes
 * active when it succeeds. A failure releases what the steps before it
 * took. The target is recorded by its path from the root of the mount table
 * (`vfs_node_path`). A read-only device (BLOCK_READ_ONLY) makes the mount
 * read-only whatever `flags` asks. A mount of type proc over a directory of
 * /proc succeeds and changes nothing.
 *
 * Returns 0 or a negative errno: ENODEV for an unknown type, or proc
 * anywhere but /proc; EINVAL for a flag not listed above; the
 * resolver's errors for the target; ENAMETOOLONG for a target path of
 * VFS_COVERED_PATH_MAX bytes or more; EINVAL for a target in a synthetic
 * type or at the root of the mount table; EBUSY for a target that is a
 * mount's root, is covered or is reserved by a mount being made; ENOTDIR
 * for a target that is no directory; the source's errors (`devfs_window_of`)
 * and EBUSY for a claimed range or a device with a mount already; ENOMEM;
 * and the constructor's error.
 */
int vfs_do_mount(const char *source, const char *target, const char *type, uint32_t flags,
                 const char *options);
/**
 * umount2(2), after the system call layer has checked the caller's
 * privilege and copied the target: unmount the mount whose root `target`
 * names. `dir` says that the target named a directory by its end, which
 * must then reach one.
 *
 * The mount goes to VFS_UNMOUNTING, which no new operation enters; the
 * operations in progress drain; the reference count is checked; the type's
 * `sync` and `prepare_unmount` run, the cache writes the device's dirty
 * buffers, the device flushes its own cache, and the cache drops the
 * device's buffers, a failure in any of these returning the mount to
 * VFS_ACTIVE; then the type's `unmount` runs, and the covered node, the
 * parent's reference, the claim and the slot are released. A bind is put
 * back with nothing written. On a device that has gone the sync, the
 * preparation, the writeback and the flush are skipped. A mount another
 * live mount namespace sees is only hidden from the caller's, and stays
 * mounted for the others.
 *
 * The walk may end on a mount whose device has gone. A walk that meets
 * such a device on the way answers EIO, and the mount is found instead by
 * its recorded path, when exactly one active mount below a removed device
 * has the target's path.
 *
 * Returns 0 or a negative errno: the resolver's errors; ENOTDIR for a
 * target named as a directory that reached something else; EINVAL for a
 * node that is no mount's root; EBUSY for a permanent mount, a mount that
 * is not active or one something still refers to, or when a buffer of the
 * device is still held or dirty; EIO when the writeback or the flush fails;
 * and the error of the type's `sync` or `prepare_unmount`.
 */
int vfs_do_umount(const char *target, int dir);

/**
 * mount(2) with MS_BIND: the directory `source` names is shown at the
 * directory `target` names, with the flags of the mount it is on, as
 * `vfs_do_mount` places a mount; with `rec`, the mounts below it are shown
 * below the new one too, as MS_REC asks. The mount whose file system the
 * bind shows cannot be unmounted while the bind exists. Returns 0 or a
 * negative errno: the resolver's errors; ENOTDIR for a source or target
 * that is not a directory; EINVAL for a source inside a synthetic type but
 * not at its root; ENAMETOOLONG for a source whose path within its file
 * system would not fit; the errors `vfs_do_mount` gives for its target;
 * EBUSY for a target covered already; and ENOMEM.
 */
int vfs_do_bind(const char *source, const char *target, int rec);

/**
 * mount(2) with MS_REMOUNT and MS_BIND: the flags of the mount `m` alone
 * become `flags`, as Linux changes a bind mount's; its file system is not
 * remounted. 0, -EINVAL for a flag not listed above, -EBUSY for a mount
 * that is not active, or -EROFS for a writable flag set over a read-only
 * base.
 */
int vfs_remount_bind(struct vfs_mount *m, uint32_t flags);

/**
 * mount(2) with MS_REMOUNT: the flags of the active mount `m` become
 * `flags`. A change between read-only and writable goes through the type's
 * `remount`, and is EINVAL on a type without one; a read-only device
 * (BLOCK_READ_ONLY), or a mount that has failed a write, stays read-only,
 * EROFS. The options of `options`, mount(2)'s data or NULL, go to the
 * type's `remount` too, and a type without one ignores them. EINVAL for a
 * flag not listed above, EBUSY for a mount that is not active, EIO for one
 * whose device has gone, or the type's error. The caller has checked that
 * no descriptor writes to a mount it makes read-only.
 */
int vfs_remount(struct vfs_mount *m, uint32_t flags, const char *options);

/**
 * A write to `dev` failed at `lba` with `rc`: every mount on the device goes
 * read-only for the rest of its life, with a record in the kernel log, and
 * keeps the first such error in `write_error`. The buffer cache calls it for
 * a failed write or flush and for a device that has gone, and `vfs_fsync`,
 * `vfs_sync_all` and the unmount for a failed flush of the device.
 */
void vfs_device_failed(block_device_t *dev, uint64_t lba, int rc);

/**
 * How many holds the mounts on `dev` account for: one for each mount, and
 * one for each of its references (`refs`) and operations in flight. An
 * unmount waits out the operations and answers EBUSY while anything else
 * refers to the mount. Zero when nothing is mounted from the device, which
 * the stick and loop drivers wait for before they free one.
 */
int vfs_device_holds(block_device_t *dev);

/** sync(2)'s half for the mounts: for every active mount, not a bind, with a
 * device that is still there, the type's `sync`, then the cache's dirty
 * buffers of the device and the device's own flush. Always 0, as sync(2)
 * is. */
int vfs_sync_all(void);

/**
 * A description opened on `mnt`: the mount's reference count goes up; the
 * node's hold is the one the resolution already took, moved rather than
 * retaken.
 */
void vfs_desc_open(struct vfs_mount *mnt);
/**
 * The last close of a description on `mnt`: release the handle, put the
 * node, drop the reference. A description opened with O_PATH has a NULL
 * handle, since the type's `open` never ran, and its `release` does not
 * run either. The put and the release may sleep.
 */
void vfs_desc_close(struct vfs_mount *mnt, void *node, void *handle);

/**
 * Repoint every reference to `from` on `mnt` to `to`, on `mnt` and on each
 * bind that shows its file system: working directories, process roots,
 * covering mounts, the mount's root, descriptions, loop devices, and the
 * advisory locks on the file and the waits for them, since a KRFFS file's
 * locks are keyed by its node (lock.h); on the boot root also the node of
 * every descriptor, which its descriptors keep themselves. The KRFFS root
 * and a KRFFS volume call it when a relocation moves a node.
 */
void vfs_repoint(struct vfs_mount *mnt, const void *from, void *to);

/*
 * The wrappers: each answers for an operation the type does not have, so
 * that each default is written once, and passes the base of a bind to the
 * type. Every wrapper but `vfs_release` answers -EIO once the mount's device
 * has gone, and each that writes answers -EROFS on a read-only mount, or a
 * bind whose base is read-only, before it calls the type. `vfs_setattr`,
 * `vfs_fallocate` and the wrappers that remove, rename or link a name
 * enforce the immutable and append-only flags a type reports with
 * `flags_known`, by Linux's rules, with -EPERM. After a namespace change or
 * a setattr succeeds, the wrapper tells inotify while any watch exists, and
 * a write, a truncation, an fallocate or a size change drops the file's
 * pages from the file cache.
 */
/** The type's read: the bytes read, 0 at the end, -EINVAL for a type without
 * `read`, or the type's error. */
int32_t vfs_read(struct vfs_mount *mnt, void *handle, void *buf, uint32_t count, uint64_t offset);
/** The type's write: the bytes written, -EROFS for a type without `write`,
 * or the type's error. */
int32_t vfs_write(struct vfs_mount *mnt, void *handle, const void *buf, uint32_t count,
                  uint64_t offset);
/** Truncate through `handle`, the handle of an open of the file, which on
 * the KRFFS root is the node: 0, -EBUSY for a file that backs a loop
 * device, -EROFS for a type without `truncate`, or the type's error. */
int vfs_truncate(struct vfs_mount *mnt, void *handle, uint64_t size);
/** fallocate(2) through an open handle: the type's operation, -EOPNOTSUPP
 * for a type without one, -EROFS on a read-only mount, -EBUSY for a file
 * that backs a loop device, and -EPERM for an immutable file or a mode
 * other than FALLOC_FL_KEEP_SIZE on an append-only one. */
int vfs_fallocate(struct vfs_mount *mnt, void *handle, uint32_t mode, uint64_t offset,
                  uint64_t len);
/** The type's fsync, or for a type without one a write of the device's
 * dirty buffers, then the device's flush, a failure of which makes the
 * device's mounts read-only (`vfs_device_failed`). 0 or a negative errno. */
int vfs_fsync(struct vfs_mount *mnt, void *handle);
/** The type's getattr, or -EINVAL for a type without one. */
int vfs_getattr(struct vfs_mount *mnt, void *node, struct vfs_stat *st);
/** The type's setattr of the fields `what` gives: 0, -EBUSY for a size
 * change of a file that backs a loop device, whose length is the device's,
 * -EROFS for a type without `setattr`, -EPERM for a change other than of the
 * flags to an immutable or append-only file, or the type's error. */
int vfs_setattr(struct vfs_mount *mnt, void *node, int what, const struct vfs_stat *st);
/** Open `node` for the handle in `*handle`, the node itself on a type
 * without `open`, and count the open: 0 or the type's error. */
int vfs_open_node(struct vfs_mount *mnt, void *node, int flags, void **handle);
/** End an open `vfs_open_node` made, and count the release; it runs on a
 * mount whose device has gone too. */
void vfs_release(struct vfs_mount *mnt, void *handle);
/** The type's readdir into `ent`, zeroed first: 1, 0 at the end, -EINVAL
 * for a type without `readdir`, or the type's error. */
int vfs_readdir(struct vfs_mount *mnt, void *handle, uint32_t *cursor, struct vfs_dirent *ent);
/** The type's create of the regular file `name` in `dir`, held in `*node`:
 * 0, -EROFS for a type without `create`, or the type's error. */
int vfs_create(struct vfs_mount *mnt, void *dir, const char *name, uint32_t mode, void **node);
/** The type's unlink of `name` in `dir`: 0, -EROFS for a type without
 * `unlink`, -EPERM when the directory or the file is immutable or
 * append-only, or the type's error. */
int vfs_unlink_name(struct vfs_mount *mnt, void *dir, const char *name);
/** The type's mkdir of `name` in `dir`: 0, -EROFS for a type without
 * `mkdir`, or the type's error. */
int vfs_mkdir(struct vfs_mount *mnt, void *dir, const char *name, uint32_t mode);
/** The type's rmdir of `name` in `dir`: 0, -EROFS for a type without
 * `rmdir`, -EPERM when the directory or the one removed is immutable or
 * append-only, or the type's error. */
int vfs_rmdir(struct vfs_mount *mnt, void *dir, const char *name);
/**
 * The type's rename of `oldname` in `olddir` to `newname` in `newdir`: 0,
 * -EROFS for a type without `rename`, -EPERM by Linux's rules for the
 * immutable and append-only flags (`rename_guarded`), or the type's error.
 * inotify is told IN_MOVED_FROM and IN_MOVED_TO with one cookie to the two
 * directories, IN_MOVE_SELF to the file moved, and to a file the new name
 * replaced what its removal would have told.
 */
int vfs_rename(struct vfs_mount *mnt, void *olddir, const char *oldname, void *newdir,
               const char *newname);
/** The type's link of `node` as `name` in `dir`: 0, -EPERM for a type
 * without `link`, for an immutable or append-only file or for an immutable
 * directory, or the type's error. */
int vfs_link(struct vfs_mount *mnt, void *dir, const char *name, void *node);
/** A symbolic link `name` in `dir` to `target`: EROFS on a read-only mount,
 * EPERM on a type without the operation. */
int vfs_symlink(struct vfs_mount *mnt, void *dir, const char *name, const char *target);
/** A FIFO, a socket or a device node, as the `mknod` operation makes one:
 * EROFS on a read-only mount, EPERM on a type without the operation. */
int vfs_mknod(struct vfs_mount *mnt, void *dir, const char *name, uint32_t mode, uint32_t rdev,
              void **node);
/** The type's statfs into `st`, zeroed first: 0, -EINVAL for a type without
 * `statfs`, or the type's error. */
int vfs_statfs(struct vfs_mount *mnt, struct vfs_statfs *st);

/**
 * The mount at `name` inside directory `node` of `mnt`, or NULL: a nameless
 * mount entered by that name, or a mount over the node the name reaches,
 * a mount still being made included. This is how a namespace change sees a
 * mount point the type's own lookup cannot know: mkdir answers EEXIST for
 * it, unlink and rmdir and rename answer EBUSY, and a directory cannot be
 * removed or renamed while a constructor sleeps over it.
 */
struct vfs_mount *vfs_leaf_mount(struct vfs_mount *mnt, void *node, const char *name);

/**
 * Whether the node at `name` inside directory `node` of `mnt` is a
 * process's root or the root of a bind of `mnt`'s file system, which rmdir
 * answers EBUSY for as it does for a mount point: the resolver starts at
 * the root's node, which the KRFFS root and a KRFFS volume do not count, so
 * it may not be freed from under the process.
 */
int vfs_leaf_root(struct vfs_mount *mnt, void *node, const char *name);

/**
 * The path of directory `node` of `mnt` from the root of the mount table,
 * into `out` of `cap` bytes: on the KRFFS root, the path of its node; on
 * another type, the path of the directory the mount covers followed by the
 * directory's path inside the mount, which the type rebuilds from the
 * volume and so gives after a rename too. On a bind, the path below the
 * bind's place. Returns 0, -ENAMETOOLONG, the type's error, or -ENOENT on a
 * type without a `path` operation, the synthetic types and mqueue, on the
 * tmpfs of memfd_create, which no path reaches, and through a bind for a
 * node outside the tree the bind shows.
 */
int vfs_node_path(struct vfs_mount *mnt, void *node, char *out, uint32_t cap);

struct vpath;
/** The classification stored in a synthetic node, or NULL when `mnt` is not
 * a mount of one of the three synthetic types. */
const struct vpath *vfs_synth_vpath(struct vfs_mount *mnt, void *node);

/* The types defined in vfs.c: the KRFFS root's adapter and the three
 * synthetic types. */
extern const struct vfs_type vfs_krffs_type;
extern const struct vfs_type vfs_devfs_type;
extern const struct vfs_type vfs_procfs_type;
extern const struct vfs_type vfs_sysfs_type;

/* The permanent mounts, entries 0 to VFS_PERMANENT - 1: the root, /dev,
 * /proc, the tmpfs /dev/shm, a nameless mount in /dev's root directory, the
 * tmpfs of memfd_create's files, which no path reaches, /sys, and the mqueue
 * file system /dev/mqueue, nameless in /dev as /dev/shm is. umount2 answers
 * EBUSY for each it can name. */
#define VFS_PERMANENT 7

/** The tmpfs memfd_create makes its files on: no path reaches it, no
 * listing shows it, and it lasts as long as the kernel. */
struct vfs_mount *vfs_memfd_mount(void);

#endif /* _VFS_H_ */
