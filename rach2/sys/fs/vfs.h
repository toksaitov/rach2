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
 * directory, or the directory there that holds the mount when it is
 * entered by more than one name (/sys/kernel for /sys/kernel/debug), at
 * the walk's root and at the root of the root mount it stays,
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
 * mount by its name; one that reaches the names of a nameless mount entered
 * by more than one, as debugfs is by kernel/debug in /sys, is answered
 * with VFS_ENTER, and the walk enters that mount there. A rest that goes
 * on past /proc/PID/fd/N, cwd or root, or /dev/fd/N, is answered with
 * VFS_LINK, and the walk goes on from the object the link refers to. No
 * mount covers a node of such a type.
 *
 * An operation a type does not have is a NULL pointer, and the wrapper of
 * the operation, below, gives the default answer for it.
 *
 * The lock order is an open file description's lock (fd.h), then a
 * mount's namespace lock, then buffers of the cache by the cache's rules
 * (bcache.h).
 */

#include "../block/block.h"
#include "../lib/idset.h"
#include "../proc/wait_queue.h"
#include "sleeplock.h"

#define VFS_COVERED_PATH_MAX 256 /* NUL included; ENAMETOOLONG past it */
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

/* What `lookup_rest` returns when `..` would climb above the mount root;
 * when the remainder reaches, at the mount's root directory, the name of a
 * nameless mount there; and when it goes on past a link of /proc that refers
 * to an open file or a directory. */
#define VFS_CLIMB 1
#define VFS_ROOT  2
#define VFS_LINK  3
/* What `lookup_rest` returns when the remainder reaches the names of a
 * nameless mount entered by more than one, with the mount in its node. */
#define VFS_ENTER 4

#define VFS_XATTR_GET    0
#define VFS_XATTR_LIST   1
#define VFS_XATTR_SET    2
#define VFS_XATTR_REMOVE 3

/* One attribute operation in kernel memory; size zero queries GET or LIST. */
struct vfs_xattr {
    int op;           /* VFS_XATTR_* */
    const char *name; /* full name, NULL for LIST */
    void *value;      /* kernel input for SET, output for GET/LIST */
    uint32_t size;    /* buffer capacity, or SET's value length */
    int flags;        /* CREATE=1, REPLACE=2, or 0 */
};

struct vfs_mount;
struct fc_inode;
struct vfs_ino_err;
struct vfs_name;
struct block_frames_done;

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
    uint32_t flags;            /* the inode flags of FS_IOC_GETFLAGS */
    uint32_t generation;       /* and the generation of FS_IOC_GETVERSION */
    uint32_t flags_known;      /* 0 for a type that keeps no flags */
    uint32_t generation_known; /* 0 for a type that keeps no generation */
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

struct vfs_cachestat;

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
    /* In place of `lookup`, for a synthetic type: the rest of the path, its
     * first `len` bytes, in one call. 0 with the node held; VFS_CLIMB with
     * `*left` at a `..` that climbs above the mount root, which the walk
     * takes there; VFS_ROOT with `*left` at the name of a nameless mount at
     * the mount root, which the walk enters; VFS_ENTER with a nameless mount
     * entered by more than one name in `*node`, not held, and `*left` past
     * those names, which the walk enters; VFS_LINK with the node of a
     * /proc/PID/fd/N, cwd or root link held and `*left` at the rest after
     * it, which the walk goes on with from the link's object; or a negative
     * errno. */
    int (*lookup_rest)(struct vfs_mount *mnt, const char *rest, uint32_t len, void **node,
                       const char **left);
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
     * `abort_unmount` runs when the writeback or the flush fails after
     * `prepare_unmount` succeeded: the type puts back what it wrote for the
     * unmount, so the volume stays marked in use, and the unmount then goes
     * on. NULL for a type with nothing to put back. */
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
     * the type lets a remount change. Called for a change of VFS_MS_RDONLY,
     * VFS_MS_SYNCHRONOUS or VFS_MS_DIRSYNC, which a type that orders its
     * writes may need to settle first, and for options that are not empty.
     * NULL for a type that does none of this, whose remounts change only
     * the other flags. */
    int (*remount)(struct vfs_mount *mnt, uint32_t flags, const char *options);
    /* fallocate(2) on an open file, `mode` its FALLOC_FL_ bits: allocate
     * [offset, offset + len) ahead of its writes, the size grown to its end
     * unless FALLOC_FL_KEEP_SIZE, or punch a hole there. A mode the type does
     * not take is -EOPNOTSUPP; NULL for a type that takes none, which the
     * common layer answers with -EOPNOTSUPP. */
    int (*fallocate)(struct vfs_mount *mnt, void *handle, uint32_t mode, uint64_t offset,
                     uint64_t len);
    /* fdatasync(2): as `fsync`, but a change of the file's times alone need
     * not reach the medium. NULL for a type with nothing to leave out, whose
     * `fsync` then answers. */
    int (*fdatasync)(struct vfs_mount *mnt, void *handle);
    /* Take and give back the sleeping lock of the held node `node`
     * (sleeplock.h), for a type that sets `file_locked`; the lock covers
     * the node's attributes, its block map and a directory's entries. NULL
     * for any other type, whose changes the mount's namespace lock covers. */
    void (*lock_node)(struct vfs_mount *mnt, void *node);
    void (*unlock_node)(struct vfs_mount *mnt, void *node);
    /* Leave a mount for a power off, the root last (`kernel_shutdown`), for a
     * type whose namespace lock does not keep its writers out (`file_locked`):
     * close it to new writers for good, wait until the tick `deadline` for
     * those inside, holding no lock, and then do what `prepare_unmount` does. 0
     * when the volume is marked clean; -EBUSY when a writer is still inside at
     * the deadline, or another change is still owed, which leaves the volume
     * marked in use; or the error of the write. NULL for any other type, whose
     * shutdown takes the namespace lock and calls `prepare_unmount`. */
    int (*shutdown)(struct vfs_mount *mnt, uint64_t deadline);
    /* Extended attributes in kernel buffers: GET/LIST return bytes, SET/REMOVE
     * return 0, or -errno. Size zero queries length. The caller checks access
     * and holds the node lock; flags are Linux's CREATE=1 and REPLACE=2. */
    int (*xattr)(struct vfs_mount *mnt, void *node, struct vfs_xattr *arg);
    /* Count the inclusive range of 4096-byte pages [first, last], clipped
     * to the file's size, into the zeroed `cs`. The caller holds the
     * description lock. Walk resident entries or allocated map branches,
     * read no disk block, and offer scheduling during a long walk.
     * NULL for a type that keeps no file page in memory. */
    void (*cachestat)(struct vfs_mount *mnt, void *node, uint64_t first, uint64_t last,
                      struct vfs_cachestat *cs);
    /* Drop a hold on `node` without the big lock when it is not the last
     * and the put has nothing else to do: 1 when it did, 0 with nothing
     * changed when the caller must `put` under the big lock. A type that
     * sets `name_cache` has it, for the cached walk (`resolve_path`); NULL
     * for any other type. */
    int (*put_unless_last)(struct vfs_mount *mnt, void *node);
    /* Copy the target of the symbolic link `node` into `buf` of `cap` bytes,
     * with a NUL, when the node in memory holds it, as Linux's `i_link`
     * does for a fast link: its length, or -1 for a target that must be
     * read with `readlink`. Takes no lock and does not sleep, for the cached
     * walk (`resolve_path`), whose hold keeps the node; NULL for a type that
     * keeps no target so. */
    int (*readlink_cached)(struct vfs_mount *mnt, void *node, char *buf, uint32_t cap);
    /* Read pages `first` to `first + count - 1` of the regular file open as
     * `handle` into the page frames `frames`, a page each, with zeros in a
     * hole and past the end of the file, the device's transfer landing in
     * the frames where it can (`block_read_frames`), as Linux's
     * `readahead` of `address_space_operations` reads into the page
     * cache's folios. The pages filled, from the first, which stop short of
     * `count` only at the end of the file or at a failed read after the
     * first; 0 at or past the end; or a negative errno. A frame left
     * unfilled may still be a device's to fill (`block_read_frames`), and
     * the caller lets go of it and reads that page into another. A file's
     * page in memory is the type's only where the type says so: the caller
     * keeps what it reads in the file page cache. NULL for a type that
     * reads no page so, whose caller reads with `read`.
     *
     * With `done` not NULL the type starts the reads and returns without
     * waiting for the device, as Linux's `readahead` submits its bios
     * (fs/mpage.c), and calls `done->fn` once for each frame, with
     * `done->index` plus the frame's place in `frames`, whether from the
     * device's completion or in the call for a frame it fills itself
     * (`struct block_frames_done`); the answer is then the pages it
     * started, each of which the completion will be or was called for, and
     * the caller lets go of the frames from there on itself. */
    int32_t (*read_frames)(struct vfs_mount *mnt, void *handle, uint32_t first, uint32_t count,
                           const uintptr_t *frames, const struct block_frames_done *done);
    /* The node an open handle holds, for a type whose `open` gives a handle
     * of its own; NULL for a type whose handle is its node. */
    void *(*handle_node)(void *handle);
    /* Move into the buffer cache what the type keeps dirty outside it and
     * has kept since tick `before` or longer, for the flusher (writeback.h),
     * which then writes the device's dirty buffers back, those just moved
     * among them: no flush, and no write of the device but those the cache
     * makes to free its buffers. Called with the big lock and admission, as
     * `sync` is, and never with `before` 0. 1 when something moved, 0 when
     * nothing was old enough, or -errno, which the type has recorded. NULL
     * for a type that keeps nothing dirty outside the cache. */
    int (*writeback)(struct vfs_mount *mnt, uint64_t before);
    /* Take and give back the write lock of the held regular file `node`
     * (sleeplock.h), for a type that sets `file_locked`: the lock that keeps
     * the file's writes, truncations and fallocates in order, as Linux's
     * `i_rwsem` keeps them, taken before the node's lock (`lock_node`) and
     * held across a whole write, a vector's included, while the node's lock
     * is let go around a fault on the writer's buffer. A fault on a mapping
     * of a file takes that file's node lock and never a write lock, so a
     * writer that holds one may fault. The holder takes it again by depth.
     * NULL for any other type, whose description lock and namespace lock
     * order its writes. */
    void (*lock_write)(struct vfs_mount *mnt, void *node);
    void (*unlock_write)(struct vfs_mount *mnt, void *node);
    /* renameat2(2) with VFS_RENAME_EXCHANGE, which swaps the names
     * `oldname` in `olddir` and `newname` in `newdir`, both there, whatever
     * their kinds, or with VFS_RENAME_WHITEOUT, which renames as `rename`
     * does and leaves at the old name a whiteout, a character device of
     * number 0:0 and mode 0, owned as a new node is. The common layer has
     * made Linux's checks of both names, and the two names are of two
     * different nodes. 0, or the type's error. NULL for a type that takes
     * neither, which the common layer answers with -EINVAL, as Linux's ext2
     * does (fs/ext2/namei.c in 6.12). */
    int (*rename_flags)(struct vfs_mount *mnt, void *olddir, const char *oldname, void *newdir,
                        const char *newname, uint32_t flags);
    /* open(2) with O_TMPFILE: a new regular file of `mode` in the
     * directory `dir`, the root for NULL, with no name, which a hard link
     * may give it only when `linkable` is set, as Linux's I_LINKABLE allows;
     * that link, `link`, clears it. Owned as `create` owns a new file. 0
     * with the node held, its link count 0, so that its last put frees it;
     * -ENOENT for a removed directory, or the type's error. NULL for a type
     * without, which open(2) answers with -EOPNOTSUPP. */
    int (*tmpfile)(struct vfs_mount *mnt, void *dir, uint32_t mode, int linkable, void **node);
    /* Let the cache go of the clean blocks of the bytes [offset, offset +
     * len) of the regular file open as `handle`, which an O_DIRECT read or
     * write has just moved, so that no copy of them stays in memory, as
     * Linux's direct I/O leaves no page of the range in the file's cache; a
     * dirty block stays for its write. NULL for a type that keeps no copy
     * of a file's data apart from the file. */
    void (*uncache)(struct vfs_mount *mnt, void *handle, uint64_t offset, uint64_t len);
};

/* renameat2(2)'s flags, Linux's values. */
#define VFS_RENAME_NOREPLACE 1u
#define VFS_RENAME_EXCHANGE  2u
#define VFS_RENAME_WHITEOUT  4u

/* What a type's regular files take of O_DIRECT (`direct_io`): nothing,
 * which open(2) and fcntl(2) refuse with -EINVAL, as Linux refuses a file
 * without FMODE_CAN_ODIRECT; any offset, length and buffer, as Linux's
 * tmpfs takes them; or each a multiple of the device's sector, as Linux's
 * ext2 asks (fs/iomap/direct-io.c: `iomap_dio_bio_iter`). */
#define VFS_DIRECT_NONE   0
#define VFS_DIRECT_ANY    1
#define VFS_DIRECT_SECTOR 2

/* What cachestat(2) counts over a file's range of pages, in the order and
 * widths of Linux's struct cachestat. A page that was in memory and went is
 * counted by Linux as evicted, and as recently evicted when it went lately;
 * no type here keeps a trace of the pages it lets go, so both stay 0, and
 * so does `writeback`, a write-back never being under way between calls. */
struct vfs_cachestat {
    uint64_t cache;
    uint64_t dirty;
    uint64_t writeback;
    uint64_t evicted;
    uint64_t recently_evicted;
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
    /* Nonzero for a type whose names the name cache keeps (dcache.h): one
     * whose `hold` keeps a node at one address, takes no lock and may run
     * without the big lock on a node already held, whose names compare
     * byte for byte, and whose namespace changes only through the wrappers
     * of vfs.c. Its `getattr` of a held node reads only the node, so the
     * cached walk may judge a search permission without the big lock, and
     * it has `put_unless_last`. */
    int name_cache;
    /* Nonzero for a type that locks each file on its own (`lock_node`) in
     * place of the mount's namespace lock: a change locks the nodes it
     * changes, so that a slow request for one file stops only that file's
     * users. */
    int file_locked;
    /* Nonzero for a type whose `read`, `readdir`, `readlink`, `getattr`,
     * `open`, `release`, `hold` and `put` run without the big lock on a node
     * the caller holds, into kernel buffers, each taking the lock itself
     * for a step that needs it, as Linux's ext2 reads under the inode's own
     * lock alone; the file calls then run without it on the type's regular
     * files, directories and links. A type that sets it sets `file_locked`
     * and `name_cache`. */
    int reads_unlocked;
    /* Nonzero for a type whose `write`, `truncate`, `fallocate`, `fsync`
     * and `fdatasync` of a regular file run without the big lock under the
     * file's own lock (`lock_node`), as Linux's ext2 writes under the
     * inode's `i_rwsem`, each taking the big lock itself for a step that
     * needs it; write, pwrite64, writev, ftruncate, fallocate, fsync and
     * fdatasync then run without it on the type's regular files. A type
     * that sets it sets `reads_unlocked`. */
    int writes_unlocked;
    /* Nonzero for a type whose directory cursor is a byte of the directory,
     * which its `readdir` moves to the next record's start when a seek or a
     * change of the directory leaves it elsewhere: lseek(2) takes any place
     * in such a directory through the mount's `max_bytes`, and SEEK_END,
     * SEEK_DATA and SEEK_HOLE from its size, as Linux's
     * `generic_file_llseek` does on an ext2 directory. Any other type's
     * `readdir` is given only a place it gave. */
    int dir_seeks_bytes;
    /* What the type's regular files take of O_DIRECT: a VFS_DIRECT_ value. */
    int direct_io;
};

/* One table entry. `covered` is the held node of the directory this mount
 * covers, on `parent`; the root and the nameless mounts cover nothing.
 * `refs` counts what keeps the mount from being unmounted: the
 * descriptions on it, the working directories and process roots in it, the
 * child mounts under it and the binds that show it. `inprogress` counts
 * operations in flight, which is what unmount drains. Both change in one
 * atomic step (`vfs_mount_get`, `vfs_mount_put`, `vfs_enter`, `vfs_leave`),
 * and the unmount judges them under the big lock. */
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
    /* The namespace lock (`vfs_ns_lock`), and the queue on which an
     * unmount waits for `inprogress` to drain. */
    struct sleep_lock ns;
    /* On a type that sets `file_locked`, the lock a rename between two
     * directories takes before either directory's (`vfs_rename_lock`), so
     * that no other such rename changes which is above the other while it
     * asks. A bind's is its base's. */
    struct sleep_lock rename;
    wait_queue_t drain_wq;
    void *root; /* the mount's root node, held for the mount's life */
    void *priv;
    struct fc_inode *filecache_files; /* cached inodes on the base, under fc_lock */
    /* The largest offset a file of the volume takes, Linux's `s_maxbytes`,
     * which the constructor of a type that sets `dir_seeks_bytes` sets and
     * lseek(2) of its directories is bounded by; a bind's is its base's. */
    uint64_t max_bytes;
    /* What the unmount record reports: every open matched by a release. */
    uint32_t opens;
    uint32_t releases;
    /* The error of the first failed write, 0 for none: the latch that
     * keeps the mount read-only for the rest of its life, which a remount
     * read-write refuses. A sync reports errors through `wb_err`. */
    int write_error;
    /* Set while a remount makes this mount read-only, from the check for
     * writers on (`fd_mount_readonly_begin`) to the flag's change: an open
     * for writing is refused with EROFS meanwhile (`fd_ofd_access`), as
     * Linux's s_readonly_remount makes mnt_is_readonly true. Under fd.c's
     * lock of the descriptions. */
    int readonly_pending;
    /* The write-back errors of the file system, on the base mount, as
     * Linux's errseq_t (lib/errseq.c): the last error's errno in the low
     * twelve bits, a bit set once a sync has reported it, and above them a
     * count that moves when a new error follows a reported one. fsync and
     * fdatasync report `wb_err`, Linux's mapping's `wb_err`, and syncfs
     * `sb_wb_err`, the superblock's `s_wb_err`, each once to each open
     * description, which keeps a cursor into each (`vfs_wb_err_sample`).
     * Under `wb_err_lock` (vfs.c). */
    uint32_t wb_err;
    uint32_t sb_wb_err;
    /* On the base mount, the error sequences of the files whose mapped
     * pages failed a write-back, each kept as `wb_err` is, which fsync,
     * fdatasync and MS_SYNC report to the file's descriptions alone
     * (`vfs_ino_err_set`). Under `wb_err_lock` (vfs.c). */
    struct vfs_ino_err *ino_errs;
    /* The tick since which what a write-back of shared mappings' pages put
     * in the file system may have been dirty, the oldest kept, on the base
     * mount; 0 for none (`vfs_writeback_aged`). Under the big lock. */
    uint64_t aged;
    int claim;       /* the devfs claim on the source's sectors, or -1 */
    char source[32]; /* the source string mount(2) was given, for /proc/mounts */
    /* The ids of the live mount namespaces that see it (proc/ns.h): a
     * mount made in one is seen there and in the namespaces made from it
     * afterwards, and a mount of the boot by the first and those made from
     * it. Its words are freed with the record (vfs_ns.c). */
    struct idset ns_seen;
    /* The inotify watches on the file system, counted on its base mount
     * (`inotify_mount_watched`); changes under the big lock and is read
     * without it as one word. */
    int inotify_watched;
    /* For a bind mount, the mount whose file system it shows, never itself
     * a bind, and the path of its root within that file system; NULL and
     * empty for every other mount. A bind has that mount's type, device,
     * private state and lock, and its own root, flags and place. */
    struct vfs_mount *bind_of;
    char bind_root[VFS_COVERED_PATH_MAX];
    /* The order the mounts were made in, which /proc/PID/mountinfo lists
     * them by, so that a mount over another's root comes after it. */
    uint32_t seq;
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
/* A walk of `vfs_resolve_scoped` that may end at a last name that does not
 * exist, as an open with O_CREAT may: it then returns VFS_MISSING with the
 * parent directory held and the name in `leaf`, after following a link at
 * the end to the name its target ends with, unless VFS_WALK_NOFOLLOW. */
#define VFS_WALK_CREATE   0x10
#define VFS_MISSING       1

/* openat2(2)'s RESOLVE_ restrictions, with Linux's values, which
 * `vfs_resolve_scoped` applies to its walk. BENEATH and IN_ROOT scope the
 * walk to its anchor. */
#define VFS_RESOLVE_NO_XDEV       0x01
#define VFS_RESOLVE_NO_MAGICLINKS 0x02
#define VFS_RESOLVE_NO_SYMLINKS   0x04
#define VFS_RESOLVE_BENEATH       0x08
#define VFS_RESOLVE_IN_ROOT       0x10
#define VFS_RESOLVE_CACHED        0x20

/* The bits of a process's `walk_replay` (`vfs_replay`). */
#define VFS_REPLAY_ON      0x1
#define VFS_REPLAY_MAGIC   0x2
#define VFS_REPLAY_TRIPPED 0x4 /* a walk of the replay answered -EAGAIN */
/* Symbolic links followed along one path before ELOOP. */
#define VFS_LINK_MAX       40
/* A path, terminator included: Linux's PATH_MAX, one page. A path a caller
 * names may be this long, and so may the absolute path a relative one makes
 * with the directory it is resolved from, and a followed link's target with
 * the rest of the path after the link; ENAMETOOLONG past it. The kernel
 * copies paths into pages from fs/pathbuf.c, not onto its stack. */
#define VFS_PATH_MAX       4096

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
 * `readlink`; EACCES or ENOTDIR from the search check; EACCES for a link of
 * /proc to another process's file or directory that the caller may not
 * read, and ENOTDIR for a path past such a link to a pipe, a socket or a
 * device; ENAMETOOLONG for a
 * component over VFS_NAME_MAX, a leaf over `leafcap` or a link's expansion
 * over VFS_PATH_MAX; ELOOP past VFS_LINK_MAX links; ENOENT for an empty
 * link target or before a root is installed; EINVAL for a parent walk of a
 * path with no component; EIO on a mount whose device has gone; EBUSY on a
 * mount that is not active; and ENOMEM.
 */
int vfs_resolve(const struct vfs_ref *start, const char *path, int flags, struct vfs_ref *out,
                char *leaf, uint32_t leafcap);

/* `vfs_resolve_cached`'s answer for a path the name cache cannot finish. */
#define VFS_CACHED_MISS 1

/**
 * Resolve the absolute path `path` as `vfs_resolve` does, from the name
 * cache alone and without the big lock: 0 with `out` held and its mount
 * entered, on a type that sets `name_cache`; -ENOENT for a name the cache
 * holds as absent; or VFS_CACHED_MISS with nothing taken, for a path the
 * caller resolves again under the big lock. `flags` takes VFS_WALK_NOFOLLOW,
 * VFS_WALK_REAL_IDS and VFS_WALK_LOCK; any other is a miss. With `entry` not
 * NULL a regular file at the last name has its directory entry recorded
 * there, as `vfs_resolve_entry` records it, and a change of names that the
 * walk may have raced makes it a miss (`vfs_name_capture_at`); `*entry` is
 * NULL on every other answer.
 */
int vfs_resolve_cached(const char *path, int flags, struct vfs_ref *out, struct vfs_name **entry);

/**
 * Resolve `path` from the caller's root, as `vfs_resolve` does with no
 * `start`, and put in `real`, of VFS_PATH_MAX bytes, the path the walk took
 * to the node it holds: from the caller's root, with each symbolic link the
 * walk followed replaced by its target and each `..` it took applied, so that
 * it names that node by the names the walk looked up. It is realpath(3)'s
 * answer, taken from the walk that found the node, so a link replaced
 * afterwards does not change it. A link of /proc or /dev that refers to an
 * object stays a name. `real` is an empty string when the path does not fit,
 * and undefined on an error. VFS_WALK_PARENT is ignored. Returns as
 * `vfs_resolve` does.
 */
int vfs_resolve_real(const char *path, int flags, struct vfs_ref *out, char *real);

/*
 * The walk of an openat2(2) path. `anchor` is the directory the call names,
 * held by the caller, where a relative path starts and, for BENEATH and
 * IN_ROOT, the root of the walk; `anchor_real` is its path from the
 * caller's root, `anchor_len` bytes, as `vfs_resolve_real` gives it, and
 * `seq` the tree's sequence (`vfs_tree_seq`) when the call began.
 */
struct vfs_scope {
    uint32_t resolve; /* VFS_RESOLVE_* */
    struct vfs_ref anchor;
    const char *anchor_real;
    uint32_t anchor_len;
    uint32_t seq;
};

/**
 * Resolve `path` as openat2(2) does, with `scope`'s restrictions, and put in
 * `real`, of VFS_PATH_MAX bytes, the path the walk took, as
 * `vfs_resolve_real` does. A relative path starts at the anchor; an
 * absolute one at the caller's root, or at the anchor under IN_ROOT, and is
 * -EXDEV under BENEATH.
 *
 * Under BENEATH, `..` at the anchor and a link whose target is absolute are
 * -EXDEV; under IN_ROOT, `..` at the anchor stays there and an absolute
 * target starts at the anchor. A scoped walk's `..` is -EAGAIN once a
 * rename or a mount has changed the tree since `seq`, since it may then
 * have climbed past the anchor by a path that no longer exists, as Linux
 * answers. NO_SYMLINKS makes every link the walk would follow -ELOOP,
 * NO_MAGICLINKS every link of /proc to an object (a descriptor, a working
 * directory, a root, a program or a namespace), and a scoped walk answers
 * such a link -EXDEV. NO_XDEV is -EXDEV at any step onto another mount than
 * the one the walk started on: a mount crossed downward or by `..`, a
 * nameless mount entered, an absolute link to a root on another mount.
 * CACHED is -EAGAIN for a name of a type that keeps its names in the name
 * cache when the cache does not have it, or of a type on a device that does
 * not. The checks run in Linux's order: ELOOP before EXDEV.
 *
 * `flags` takes VFS_WALK_NOFOLLOW, VFS_WALK_LOCK and VFS_WALK_CREATE, whose
 * missing name goes in `leaf`, of VFS_NAME_MAX + 1 bytes.
 * Returns 0 or VFS_MISSING with `out` held, or `vfs_resolve`'s errors and
 * those above.
 */
int vfs_resolve_scoped(const struct vfs_scope *scope, const char *path, int flags,
                       struct vfs_ref *out, char *leaf, char *real);

/**
 * A count that moves at every rename and every change of what is mounted
 * where: Linux's rename and mount sequences in one, which a scoped walk's
 * `..` and an openat2's replay compare.
 */
uint32_t vfs_tree_seq(void);

/** Move the tree's sequence (`vfs_tree_seq`), before and after a rename. */
void vfs_tree_changed(void);

/**
 * Make every walk of the calling thread a replay of the walk an openat2
 * checked, until `vfs_replay_end`: the path it is given is the real path
 * that walk took, with no `..` and no link but those of /proc to an object,
 * which `magic` allows, so a replay that meets another link, or finds the
 * tree's sequence moved from `seq`, answers -EAGAIN instead of going where
 * the checked walk did not. A non-NULL `xdev`, which the caller keeps held,
 * is the mount NO_XDEV preserves, including when a descriptor link changes
 * its target before the replay follows it.
 */
void vfs_replay(int magic, uint32_t seq, struct vfs_mount *xdev);

/**
 * End the calling thread's replay (`vfs_replay`). Returns 1 when a walk of
 * the replay answered -EAGAIN, so the caller may walk again, and 0 if not.
 */
int vfs_replay_end(void);

/* Resolve and capture the final regular-file entry in the same walk.
 * The returned name owns its parent and mount; the caller puts or transfers
 * it even if a later open fails. No entry is returned for a mount root.
 * With `dir` not NULL, the attributes of the directory the path's last name
 * was looked up in go there, after any link at the end is followed, as
 * Linux's nd->dir_mode and dir_vfsuid; its mode is 0 when the walk ended
 * on no name of a directory: the root, a mount entered by name, or a node
 * of /dev, /proc or /sys. */
int vfs_resolve_entry(const char *path, int flags, struct vfs_ref *out, char *real,
                      struct vfs_name **entry, struct vfs_stat *dir);

/* The protections of Linux's /proc/sys/fs that Rach2 keeps, each its
 * sysctl's value: protected_hardlinks and protected_symlinks 0 or 1,
 * protected_fifos and protected_regular 0, 1 or 2 (fs/namei.c:1048-1085). */
extern int vfs_protected_hardlinks;
extern int vfs_protected_symlinks;
extern int vfs_protected_fifos;
extern int vfs_protected_regular;

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

/** The slot of mount `m` in the table, read under the table's lock, so a
 * caller without the big lock may ask; -1 when `m` has none. */
int vfs_mount_index(const struct vfs_mount *m);

/* The mount namespaces' views of the table (vfs_ns.c). */

/** Whether the caller's mount namespace sees the mount `m`. */
int vfs_mount_seen(const struct vfs_mount *m);

/** `vfs_mount_seen` for a caller without the big lock, which may read only
 * the set's first word, since its other words grow under the lock: 1 or 0,
 * or -1 when the caller's namespace id lies past that word. */
int vfs_mount_seen_word(const struct vfs_mount *m);

/** Whether a live mount namespace other than the caller's sees the mount
 * `m`, which a change made only for the caller's must then leave alone. */
int vfs_mount_shared(const struct vfs_mount *m);

/** Whether no live mount namespace sees the mount `m`. */
int vfs_mount_unseen(const struct vfs_mount *m);

/** Take the mount `m` from the caller's mount namespace's view. */
void vfs_mount_hide(struct vfs_mount *m);

/** Give the new record `m`, whose view is empty, the view of a mount made
 * by the caller: its own mount namespace alone, or the first when no
 * process runs, as at boot. 0, or -ENOMEM. */
int vfs_mount_view_new(struct vfs_mount *m);

/** Free the words of the view of `m`, whose record is let go of. */
void vfs_mount_view_free(struct vfs_mount *m);

/** Unmount the active mount `m` as umount2(2) without flags does, with
 * nothing left to check, for the end of a mount namespace: 0, or -EBUSY
 * for a mount something still holds, which stays (vfs.c). */
int vfs_unmount_record(struct vfs_mount *m);

/** Entry zero, the root of the mount table, read under the table's lock,
 * so a caller need not hold the big lock. */
struct vfs_mount *vfs_root_mount(void);

/** Add `delta` to the count of inotify watches on the base mount `m`
 * (`inotify_watched`), whose record the table holds; under the big lock. */
void vfs_mount_watches(const struct vfs_mount *m, int delta);

/** Whether the device under `mnt` has gone, by its own flags or, for a
 * partition window, by those of the disk under it (`block_flags`); 0 for a
 * mount with no device. */
int vfs_mount_gone(const struct vfs_mount *mnt);

/** The mount whose file system `m` shows: the one a bind mount was made
 * from, or `m` itself. A file is the same file by its inode number on the
 * base mount through every mount that shows it. */
const struct vfs_mount *vfs_mount_base(const struct vfs_mount *m);

/** Whether a root is installed at entry zero, of any type, read under the
 * mount table's lock, so a caller needs no lock of its own. */
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

/**
 * Put a root of `type` on `dev`, a loop device over a file of the root
 * installed now, at entry zero before any process runs, and leave the old
 * root mounted as a nameless mount entered by `place`, one name below the
 * new root, as /dev is: the root from an image file, `rach2.rootimage=`.
 * The permanent mounts move to the new root. `*host` is the old root's
 * mount, which a shutdown writes out after the new root, since the new
 * root's blocks go to a file on it. Returns 0, -ENOENT with no root
 * installed, -EINVAL for a `place` of more than one name, -ENOMEM, or the
 * constructor's error, after which the old root stays at entry zero.
 */
int vfs_root_over_image(const struct vfs_type *type, struct block_device *dev, const char *source,
                        const char *place, struct vfs_mount **host);

/** The `idx`-th type mount(2) finds by name, or NULL past the last; the
 * order /proc/filesystems lists them in. */
const struct vfs_type *vfs_type_at(int idx);

/**
 * The `n`-th nameless mount entered by name from the root directory of
 * `parent`, as /dev, /proc and /sys are from the root's and /dev/shm and
 * /dev/mqueue from /dev's: its name in `*name`, and the inode number of its
 * root, or 0 when the type reports none, in `*ino`. 1, or 0 past the last.
 * The synthetic directories list them with their own entries. A mount
 * entered by more than one name, which is no entry of that directory, is
 * left out.
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

/** Take a reference to `mnt`, which keeps it from being unmounted, in one
 * atomic step. */
void vfs_mount_get(struct vfs_mount *mnt);

/** Drop a reference `vfs_mount_get` took, in one atomic step, and return
 * how many are left; the caller reaps a detached mount at 0. */
uint32_t vfs_mount_put(struct vfs_mount *mnt);

/**
 * Whether the calling process holds the namespace lock of any mount, or a
 * file lock (`file_locks` in process.h).
 *
 * The buffer cache checks it before it writes back a buffer of a device
 * that is a file, a loop device: that write takes the lock of the mount
 * that has the file, and a caller that already holds another mount's lock
 * could wait for that lock while its holder waits for the caller's.
 */
int vfs_ns_held_by_caller(void);
/** Whether the calling process holds a file lock, or the namespace lock of
 * a mount whose base is not that of `mnt`; the name cache puts a node of
 * `mnt` only when it holds neither (dcache.h). */
int vfs_ns_held_other(const struct vfs_mount *mnt);

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
 * Whether `mnt`, or the file system a bind shows, locks each file on its own
 * (`vfs_type.file_locked`): 1 or 0.
 */
int vfs_file_locked(const struct vfs_mount *mnt);

/**
 * Take the lock that covers a change to the node `ref`: the node's own on a
 * type that sets `file_locked`, through the type's `lock_node`, and the
 * mount's namespace lock on any other. A NULL node is the mount's root.
 * The holder may take it again. Sleeps until the lock is free; a signal
 * does not end the wait.
 *
 * Node locks go in this order: a mount's rename lock, then directories,
 * an ancestor before what is below it and unrelated ones by inode number,
 * then the other nodes by inode number. A walk holds none while it looks
 * names up, so a caller that checked something before it took the lock of
 * a directory checks it again under the lock.
 */
void vfs_node_lock(const struct vfs_ref *ref);
/** Give back one take of the lock `vfs_node_lock` took for `ref`. */
void vfs_node_unlock(const struct vfs_ref *ref);

/**
 * Take the write lock of the regular file `ref` (`vfs_ops.lock_write`) on a
 * type that sets `file_locked`, and nothing on any other. A write, a
 * truncation and a fallocate take it before the file's node lock
 * (`vfs_node_lock`) and hold it to their end, so that one never sees
 * another half done, even while the writer lets the node's lock go to
 * bring its buffer in. It is taken with no node lock of a regular file
 * held, and no fault takes it. Sleeps until it is free; a signal does not
 * end the wait.
 */
void vfs_write_lock(const struct vfs_ref *ref);
/** Give back one take of the lock `vfs_write_lock` took for `ref`. */
void vfs_write_unlock(const struct vfs_ref *ref);

/**
 * Hold and lock the node named `name` in the directory `dir`, whose lock
 * (`vfs_node_lock`) the caller holds, into `child`, on a type that sets
 * `file_locked`, so that a check of the node's owner or flags and the change
 * it allows see one node. 0 with `child->node` NULL on any other type and
 * for a name the directory does not have, 0 with the node held and locked,
 * or the lookup's error. The caller gives both back with
 * `vfs_child_unlock`.
 */
int vfs_child_lock(const struct vfs_ref *dir, const char *name, struct vfs_ref *child);
/** Unlock and put what `vfs_child_lock` gave `child`, if anything, and
 * leave `child->node` NULL. */
void vfs_child_unlock(struct vfs_ref *child);

/*
 * The locks a rename holds (`vfs_rename_lock`): its directories' and the
 * nodes its names have. A node is NULL for none.
 */
struct vfs_rename_hold {
    struct vfs_mount *renaming; /* the mount whose rename lock is held, or NULL */
    struct vfs_ref dir[2];      /* the directories, in the order locked */
    struct vfs_ref child[2];    /* the nodes of the names, in the order locked */
};

/**
 * Lock for a rename of `oleaf` in the directory `opar` to `nleaf` in `npar`,
 * both on one mount, which the caller walked to with nothing held, into
 * `h`. On a type without `file_locked` this is the mount's namespace lock.
 *
 * On one with it, two directories take the mount's rename lock first and
 * find which is above the other by walking up from each through `..`, then
 * lock the one above first, or the lower inode number first when neither
 * is; then the nodes of both names, by inode number, each once. A node of a
 * name that is a directory above the other directory is left unlocked,
 * since the rename is refused (EINVAL for the source, ENOTEMPTY for the
 * target) and its lock would come after a directory below it. A name that
 * changed while the locks were taken starts the order again. Returns 0 with
 * the locks held, or the walk's error with none. The caller looks both
 * names up again under the locks and gives them back with
 * `vfs_rename_unlock`.
 */
int vfs_rename_lock(const struct vfs_ref *opar, const char *oleaf, const struct vfs_ref *npar,
                    const char *nleaf, struct vfs_rename_hold *h);
/** Give back what `vfs_rename_lock` took into `h`. */
void vfs_rename_unlock(struct vfs_rename_hold *h);

/** Make the rename lock of `m` a free one, of the kind `rename`. */
void vfs_rename_lock_init(struct vfs_mount *m);

/**
 * Take the lock of the directory `tref` that a mount or a bind is about to
 * cover (`vfs_node_lock`), and answer -ENOENT when, on a type that sets
 * `file_locked`, the directory lost its last name before the lock was had:
 * its `rmdir` holds the same lock while it looks for a mount on it, so the
 * two cannot pass each other. 0 otherwise, and on any other type, where the
 * parent's namespace lock orders the two. The lock is held either way; the
 * caller gives it back with `vfs_node_unlock` once the mount is placed.
 */
int vfs_cover_lock(const struct vfs_ref *tref);

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
 * /proc succeeds and changes nothing; over any other directory it shows
 * /proc's files there.
 *
 * Returns 0 or a negative errno: ENODEV for an unknown type; EINVAL for a
 * flag not listed above; the
 * resolver's errors for the target; ENAMETOOLONG for a target path of
 * VFS_COVERED_PATH_MAX bytes or more; EINVAL for a target in a synthetic
 * type, but at the root of a bind of one, or at the root of the mount
 * table; EBUSY for a target that is covered or is reserved by a mount
 * being made; ENOTDIR
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
 * operations in progress drain; the reference count is checked, and a
 * mount something holds returns to VFS_ACTIVE; the type's `sync` and
 * `prepare_unmount` run, the cache writes the device's dirty buffers, the
 * device flushes its own cache, and the cache drops the device's buffers;
 * then the type's `unmount` runs, and the covered node, the parent's
 * reference, the claim and the slot are released. A failed write does not
 * keep the mount, as Linux's `generic_shutdown_super` ignores the result of
 * its sync (fs/super.c:618): after a failed `sync`, or on a device whose
 * earlier write failed, the preparation and the writeback are skipped; a
 * writeback or a flush that fails after the preparation has the type's
 * `abort_unmount` put back the volume's in-use mark; buffers still dirty or
 * held are dropped (`bcache_drop`); and the failure is logged as `vfs:
 * unmount: PATH: STEP failed (ERR): N dirty buffers dropped`. A bind is put
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
 * node that is no mount's root; and EBUSY for a permanent mount, a mount
 * that is not active or one something still refers to. A failed write is
 * not returned, as Linux's umount returns none: it is logged, and the
 * volume, left marked in use, has the next mount check it.
 */
int vfs_do_umount(const char *target, int dir);

/**
 * umount2(2) with MNT_DETACH: take the mount whose root `target` names, and
 * every mount below it, from the caller's mount namespace at once, however
 * busy they are; `dir` is as `vfs_do_umount` takes it. The caller's root and
 * the mounts below it stay, so that after pivot_root the old root can be
 * let go of while the new one, which was below it, is in use. A mount no
 * live namespace sees any more is unmounted once nothing holds it
 * (`vfs_reap_detached`). A permanent mount may be taken from the view, and
 * stays mounted. Returns 0 or a negative errno: the resolver's errors;
 * ENOTDIR and EINVAL as `vfs_do_umount` gives them; EINVAL for the root of
 * the mount table; and EBUSY for a mount that is not active.
 */
int vfs_do_detach(const char *target, int dir);

/**
 * Unmount every mount that no live mount namespace sees and that nothing
 * holds any more: the mounts a lazy unmount took from the last view of
 * them, and those a namespace's end left busy. A hold's or a description's
 * end on such a mount calls it, and so does the end of a namespace.
 */
void vfs_reap_detached(void);

/**
 * The mounts' part of pivot_root(2): `new_root` names the root of a mount,
 * which becomes the caller's root, and `put_old` a directory at or below it
 * where the old root is shown, both as paths from the caller's root. The
 * old root, which must be the root of a mount, is shown at `put_old` by a
 * bind, with the mounts below it but the new root and those below that, and
 * the originals are taken from the caller's namespace, as Linux moves them,
 * but for those the new root is below, through which a descriptor opened
 * before still reaches it.
 * When `put_old` names the new root itself nothing is shown or taken: Linux
 * stacks the old root over the new, where it stays until a lazy unmount
 * through a descriptor or a working directory on it. On success `*out` has
 * the new root, held, for the system call layer to move the processes'
 * roots to and release. 0, or the resolver's errors; ENOTDIR for a
 * `put_old` that is no directory; EINVAL for a `new_root` that is no
 * mount's root, is in a synthetic type or is a permanent mount, for a
 * caller whose root is no mount's root, or for a `put_old` not at or below
 * `new_root`; EBUSY for a `new_root` or a `put_old` on the caller's root
 * mount; or the errors of a bind (`vfs_do_bind`).
 */
int vfs_pivot_mounts(const char *new_root, const char *put_old, struct vfs_ref *out);

/**
 * mount(2) with MS_BIND: the directory `source` names is shown at the
 * directory `target` names, with the flags of the mount it is on, as
 * `vfs_do_mount` places a mount; with `rec`, the mounts below it are shown
 * below the new one too, as MS_REC asks. The mount whose file system the
 * bind shows cannot be unmounted while the bind exists. Returns 0 or a
 * negative errno: the resolver's errors; ENOTDIR for a source or target
 * that is not a directory; EINVAL for a source that is /proc/PID/fd/N, or
 * whose path names some other place of its synthetic type, and for a target
 * in a synthetic type deeper than a name in a mount's root directory;
 * ENAMETOOLONG for a source whose path within its file system would not
 * fit; the errors `vfs_do_mount` gives for its target; EBUSY for a target
 * covered already; and ENOMEM.
 */
int vfs_do_bind(const char *source, const char *target, int rec);

/**
 * mount(2) with MS_REMOUNT and MS_BIND: the flags of the mount `m` alone
 * become `flags`, as Linux changes a bind mount's; its file system is not
 * remounted. 0, -EINVAL for a flag not listed above, -EBUSY for a mount
 * that is not active or, made read-only, through which a description is
 * open for writing (`fd_mount_readonly_begin`), as Linux's
 * mnt_make_readonly answers (fs/namespace.c), or -EROFS for a writable flag
 * set over a read-only base.
 */
int vfs_remount_bind(struct vfs_mount *m, uint32_t flags);

/**
 * mount(2) with MS_REMOUNT: the flags of the active mount `m` become
 * `flags`. A change between read-only and writable goes through the type's
 * `remount`, and is EINVAL on a type without one; a change of
 * VFS_MS_SYNCHRONOUS or VFS_MS_DIRSYNC goes through it when the type has
 * one, which may settle its writes first; a read-only device
 * (BLOCK_READ_ONLY), or a mount that has failed a write, stays read-only,
 * EROFS. The options of `options`, mount(2)'s data or NULL, go to the
 * type's `remount` too, and a type without one ignores them. EINVAL for a
 * flag not listed above, EBUSY for a mount that is not active, EIO for one
 * whose device has gone, or the type's error. A change to read-only first
 * refuses new writers and is EBUSY while a description open for writing on
 * the file system lives, a writable shared mapping's among them
 * (`fd_mount_readonly_begin`), as Linux's sb_prepare_remount_readonly
 * (fs/namespace.c:665); then the dirty pages of its files' shared mappings
 * are written (`filemap_sync_mount`), whose failure is the answer, with the
 * mount still writable, as Linux syncs the file system before the change
 * (fs/ext2/super.c:1334).
 */
int vfs_remount(struct vfs_mount *m, uint32_t flags, const char *options);

/**
 * A write to `dev` failed at `lba` with `rc`: every mount on the device goes
 * read-only for the rest of its life, with a record in the kernel log, and
 * keeps the first such error in `write_error`, and each file system on the
 * device records this one once for the syncs to report (`vfs_wb_err_set`),
 * however many binds show it, so that a sync between two records does not
 * report one failure twice. The buffer cache calls it for a failed write or
 * flush (`bcache_flush`) and for a device that has gone. Under the big lock.
 */
void vfs_device_failed(block_device_t *dev, uint64_t lba, int rc);

/**
 * Record the write-back error `rc`, a negative errno, in the file system
 * `mnt` shows, for each description's next fsync, fdatasync and syncfs to
 * report, as Linux's mapping_set_error records it in the mapping and the
 * superblock (include/linux/pagemap.h:43). Any context; takes a spinlock
 * with interrupts off.
 */
void vfs_wb_err_set(struct vfs_mount *mnt, int rc);

/**
 * Record the failure `rc` of a write-back of the shared mappings' pages of
 * the file `ino` of the file system `mnt` shows in the file's own error
 * sequence, which each description of the file reports once through its
 * fsync, fdatasync or MS_SYNC (`vfs_ino_err_check`), and for syncfs in
 * `sb_wb_err`, as Linux's mapping_set_error records it in the inode's
 * mapping and the superblock (include/linux/pagemap.h:43); the fsync of
 * another file of the file system hears nothing of it. When the heap has
 * no room for the file's sequence, the file system's `wb_err` takes the
 * error instead. A positive `rc` or one past the errno bits records EIO.
 * Not for interrupt context, since it may allocate the file's sequence.
 */
void vfs_ino_err_set(struct vfs_mount *mnt, uint64_t ino, int rc);

/**
 * The cursor a new description of the file `ino` of `mnt` starts with into
 * the file's error sequence: at the error last reported, or before an
 * error no sync has reported yet, or before any error at all, so that the
 * first sync through the description after a failure nobody has reported
 * sees it, as Linux's open samples the mapping's `wb_err` with
 * errseq_sample (fs/open.c:911, lib/errseq.c:122).
 */
uint32_t vfs_ino_err_sample(struct vfs_mount *mnt, uint64_t ino);

/**
 * The report of the file `ino` of `mnt`'s own error sequence to the
 * description whose cursor is `*seen` (`vfs_ino_err_sample`): the errno of
 * a failure recorded since the cursor last looked, with the sequence marked
 * reported and the cursor moved past it, else 0, as Linux's
 * `file_check_and_advance_wb_err` (mm/filemap.c:726). Serialised with
 * every other report, so that two syncs of one description report an error
 * once.
 */
int vfs_ino_err_check(struct vfs_mount *mnt, uint64_t ino, uint32_t *seen);

/**
 * Free the error sequence of the file `ino` of `mnt`, when it has one, as
 * its inode number is freed and may name another file: at the free of an
 * unlinked inode, once no description of it is left.
 */
void vfs_ino_err_forget(struct vfs_mount *mnt, uint64_t ino);

/**
 * The cursors a new description of a file of `mnt` starts with: `*file`
 * into the errors fsync reports and `*fs` into those syncfs reports, each
 * at the error last reported, or before an error no sync has reported yet,
 * so that the first description to sync after a failure sees it, as Linux's
 * errseq_sample answers 0 for an unseen error (lib/errseq.c:122).
 */
void vfs_wb_err_sample(struct vfs_mount *mnt, uint32_t *file, uint32_t *fs);

/**
 * syncfs(2)'s report for a description of a file of `mnt` whose cursor is
 * `*seen`: the errno of a write-back error recorded since the cursor last
 * looked, with the cursor moved past it, else 0, as Linux's syncfs checks
 * `s_wb_err` (fs/sync.c:166). Serialised with every other report.
 */
int vfs_syncfs_error(struct vfs_mount *mnt, uint32_t *seen);

/**
 * The fsync report alone for a description of a file of `mnt` whose cursor
 * is `*seen`: the errno of a write-back error recorded in the file system's
 * `wb_err` since the cursor last looked, with the cursor moved past it, else
 * 0, as Linux's `file_check_and_advance_wb_err` (mm/filemap.c:726). For a
 * sync that failed before the file system's fsync could run, whose failure
 * it has recorded and answered. Serialised with every other report.
 */
int vfs_fsync_error(struct vfs_mount *mnt, uint32_t *seen);

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

/** The flusher's half for the mounts (writeback.h): for every active mount
 * that `vfs_sync_all` syncs, the type's `writeback` of what has waited
 * since tick `before`, then the cache's write-back of the device's buffers
 * dirty since mark `since` (`bcache_writeback`), or of all of them when the
 * type moved something into the cache, as Linux writes all of an expired
 * inode's pages; a mount whose `aged` is at or before a nonzero `before`
 * is written whole, its `aged` cleared first; no flush. A failure is
 * recorded as the type and the cache record it. */
void vfs_writeback_all(uint64_t before, uint64_t since);

/** Keep on the base of `mnt` that what was just written to one of its files
 * may have been dirty since tick `born`, as shared mappings' pages are
 * written into the file system with no age of their own: once `born` is as
 * old as the flusher's expiry, `vfs_writeback_all` writes the mount whole,
 * the type's pending writes and every dirty buffer of its device, as Linux
 * writes an expired inode's pages whatever their own age. The oldest of
 * the ticks given is kept. Under the big lock. */
void vfs_writeback_aged(const struct vfs_mount *mnt, uint64_t born);

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
 * a setattr succeeds, the wrapper tells inotify while any watch exists. A
 * write drops from the file cache the pages of its bytes, and those from the
 * file's old end on when it starts past it, a truncation those from the
 * shorter of the old and the new end on, and an fallocate or a size change
 * every page of the file.
 */
/** The type's read: the bytes read, 0 at the end, -EINVAL for a type without
 * `read`, or the type's error. */
int32_t vfs_read(struct vfs_mount *mnt, void *handle, void *buf, uint32_t count, uint64_t offset);
/** The type's `read_frames`, with `done` as it takes it: the
 * pages filled, or started for `done`, 0 at the end, -ENOSYS for a type
 * without it, or the type's error. */
int32_t vfs_read_frames(struct vfs_mount *mnt, void *handle, uint32_t first, uint32_t count,
                        const uintptr_t *frames, const struct block_frames_done *done);
/** The type's write: the bytes written, -EROFS for a type without `write`,
 * or the type's error. */
int32_t vfs_write(struct vfs_mount *mnt, void *handle, const void *buf, uint32_t count,
                  uint64_t offset);
/** Truncate through `handle`, the handle of an open of the file, which on
 * the KRFFS root is the node: 0, -EBUSY for a file that backs a loop
 * device, -EROFS for a type without `truncate`, or the type's error. */
int vfs_truncate(struct vfs_mount *mnt, void *handle, uint64_t size);
/** Count into `*cs`, zeroed first, the pages from `first` to `last` of the
 * regular file `node` of `mnt` that are in memory, by the type's
 * `cachestat`, up to the file's size; the caller holds the description's
 * lock. Nothing for a mount that is gone or a type that keeps no page. */
void vfs_cachestat(struct vfs_mount *mnt, void *node, uint64_t first, uint64_t last,
                   struct vfs_cachestat *cs);
/** fallocate(2) through an open handle: the type's operation, -EOPNOTSUPP
 * for a type without one, -EROFS on a read-only mount, -EBUSY for a file
 * that backs a loop device, and -EPERM for an immutable file or a mode
 * other than FALLOC_FL_KEEP_SIZE on an append-only one. */
int vfs_fallocate(struct vfs_mount *mnt, void *handle, uint32_t mode, uint64_t offset,
                  uint64_t len);
/**
 * Whether the caller's RLIMIT_FSIZE lets a file reach `end` bytes: 0, or
 * -EFBIG with SIGXFSZ sent to the calling thread, as Linux answers a write
 * that starts at the limit and a truncation past it. The signal is sent
 * under the big lock, which is taken when the caller lacks it. 0 outside a
 * process.
 */
int32_t vfs_fsize_refused(uint64_t end);
/**
 * Whether the caller's RLIMIT_FSIZE lets a file of `size` bytes be set to
 * or allocated up to `end`, the growth half of Linux's inode_newsize_ok
 * (fs/attr.c:258 in 6.12): 0, or `vfs_fsize_refused`'s -EFBIG with SIGXFSZ
 * when `end` is past both the size and the limit, since only growth is
 * checked. A type's `fallocate` calls it once it has taken the mode and
 * holds the lock that keeps `size` current, so that a mode it refuses is
 * -EOPNOTSUPP with no signal, as on Linux.
 */
int32_t vfs_newsize_ok(uint64_t size, uint64_t end);
/** The type's fsync, or for a type without one a write of the device's
 * dirty buffers, then the device's flush, a failure of which makes the
 * device's mounts read-only (`vfs_device_failed`). `seen` is the cursor of
 * the description that syncs (`vfs_wb_err_sample`): a write-back error
 * recorded since it last looked is the answer when the sync itself
 * succeeds, and the cursor moves past it whatever the sync's answer, as
 * Linux's `file_check_and_advance_wb_err` (mm/filemap.c:726). 0 or a
 * negative errno. */
int vfs_fsync(struct vfs_mount *mnt, void *handle, uint32_t *seen);
/** `vfs_fsync` by the type's fdatasync, or by its fsync for a type without
 * one. 0 or a negative errno. */
int vfs_fdatasync(struct vfs_mount *mnt, void *handle, uint32_t *seen);
/** The type's getattr, or -EINVAL for a type without one. */
int vfs_getattr(struct vfs_mount *mnt, void *node, struct vfs_stat *st);
/** The type's setattr of the fields `what` gives: 0, -EBUSY for a size
 * change of a file that backs a loop device, whose length is the device's,
 * -EROFS for a type without `setattr`, -EPERM for a change other than of the
 * flags to an immutable or append-only file, or the type's error. A change
 * made is told to the file's own inotify watches; the caller, which knows
 * the name, tells its directory's. */
int vfs_setattr(struct vfs_mount *mnt, void *node, int what, const struct vfs_stat *st);
/** The inotify events of a setattr of `what`, by Linux's `fsnotify_change`:
 * the size and the modification time alone are a change of the data, and
 * both times together are utime's change of the attributes; 0 for none. */
uint32_t vfs_setattr_events(int what);
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
/** The type's `tmpfile`: a regular file of `mode` with no name, on the
 * file system of the directory `dir`, held in `*node`, which a link may name
 * only when `linkable`, as O_TMPFILE makes. 0, -EROFS on a read-only mount,
 * -EPERM when `dir` is immutable, -EOPNOTSUPP for a type without `tmpfile`,
 * or the type's error. */
int vfs_tmpfile(struct vfs_mount *mnt, void *dir, uint32_t mode, int linkable, void **node);
/** The type's `uncache` of bytes [`offset`, `offset` + `len`) of the file
 * the open handle `handle` holds, after an O_DIRECT read or write; nothing
 * on a type without it or a mount whose device has gone. */
void vfs_uncache(struct vfs_mount *mnt, void *handle, uint64_t offset, uint64_t len);
/** The type's unlink of `name` in `dir`: 0, -EROFS for a type without
 * `unlink`, -EPERM when the directory or the file is immutable or
 * append-only, or the type's error. A file that backs a loop device loses
 * its name and the attachment's open keeps the node, as Linux's loop device
 * keeps the inode; the KRFFS root, whose attachment holds no count, is
 * refused by the caller (`unlink_root_name`). */
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
 * A replaced file that backs a loop device is kept by the attachment's
 * open, as an unlinked one is (`vfs_unlink_name`). The caller holds the
 * rename's locks (`vfs_rename_lock`).
 * inotify is told IN_MOVED_FROM and IN_MOVED_TO with one cookie to the two
 * directories, IN_MOVE_SELF to the file moved, and to a file the new name
 * replaced what its removal would have told.
 */
int vfs_rename(struct vfs_mount *mnt, void *olddir, const char *oldname, void *newdir,
               const char *newname);
/**
 * renameat2(2)'s VFS_RENAME_EXCHANGE or VFS_RENAME_WHITEOUT in `flags`, with
 * VFS_RENAME_NOREPLACE beside the second, through the type's
 * `rename_flags`: `vfs_rename`'s answers, and -EINVAL for a type without
 * `rename_flags`. An exchange swaps the names in the records of open files
 * too, and tells inotify of two moves. The caller holds the rename's locks
 * and has found the two names of two nodes for an exchange.
 */
int vfs_rename_flags(struct vfs_mount *mnt, void *olddir, const char *oldname, void *newdir,
                     const char *newname, uint32_t flags);
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
 * The path of `node` of `mnt` from the root of the mount table,
 * into `out` of `cap` bytes: on the KRFFS root, the path of its node; on
 * another type, the path of the directory the mount covers followed by the
 * node's path inside the mount, which the type rebuilds from the
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

/* The types defined in vfs.c: the KRFFS root's adapter, the three
 * synthetic types and debugfs, whose operations are debugfs.c's. */
extern const struct vfs_type vfs_krffs_type;
extern const struct vfs_type vfs_devfs_type;
extern const struct vfs_type vfs_procfs_type;
extern const struct vfs_type vfs_sysfs_type;
extern const struct vfs_type vfs_debugfs_type;

/* The permanent mounts, entries 0 to VFS_PERMANENT - 1: the root, /dev,
 * /proc, the tmpfs /dev/shm, a nameless mount in /dev's root directory, the
 * tmpfs of memfd_create's files, which no path reaches, /sys, the mqueue
 * file system /dev/mqueue, nameless in /dev as /dev/shm is, and debugfs at
 * /sys/kernel/debug, nameless in /sys and entered by the names
 * kernel/debug. umount2 answers EBUSY for each it can name. */
#define VFS_PERMANENT 8

/**
 * Mount a new file system of `t` at `path` inside /sys, a nameless mount
 * of /sys entered by the names of `path` below it, as /sys/kernel/debug is,
 * with `source` and the VFS_MS_ `flags`, for a mount the kernel makes at
 * boot that umount2 may take away, as /sys/fs/pstore. 0, -EINVAL before
 * /sys is mounted or for a path outside it, -ENOMEM, or the error of the
 * type's constructor, which is called with no device and no options.
 */
int vfs_mount_in_sys(const struct vfs_type *t, const char *path, const char *source,
                     uint32_t flags);

/** The tmpfs memfd_create makes its files on: no path reaches it, no
 * listing shows it, and it lasts as long as the kernel. */
struct vfs_mount *vfs_memfd_mount(void);

/** Extended attributes on a held node, in kernel buffers. The caller holds
 * the node lock and checks permission; unsupported types list empty and
 * return -EOPNOTSUPP otherwise. Enforces read-only and inode flags. */
int vfs_xattr(struct vfs_mount *mnt, void *node, struct vfs_xattr *arg);

#endif /* _VFS_H_ */
