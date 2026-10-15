#ifndef _KRFFS_H_
#define _KRFFS_H_

/*
 * KRFFS, the K&R first-fit file system: the node format, the offset
 * interface that works on any mount through a backend, and the boot root's
 * pointer interface over its mapped image.
 */

#include "../lib/kint.h"
#include "../lib/kdef.h"
#include "../lib/kstring.h"

/* File system magic number: "KR" in little-endian. */
#define KRFFS_FILE_SYSTEM_MAGIC 0x4B52

/* Maximum file name length, including the NUL terminator. */
#define KRFFS_FILE_NAME_BUFFER_SIZE 256

/* The root node's name is the empty string (it represents the "/"
 * directory). */
#define KRFFS_FILE_SYSTEM_ROOT_NODE_NAME ""

/* Default root node permissions (rwxr-xr-x). */
#define KRFFS_FILE_SYSTEM_ROOT_NODE_MODE 0755

/*
 * The type bits of a node's mode, with the POSIX values. A node's directory
 * is the one whose id is in its `parent_id`, KRFFS_ROOT_ID for the root
 * directory, and a directory is a reserved node whose mode has S_IFDIR. A
 * directory's id is its parent's id mixed with the hash of its name, so
 * that its children can name it; krffs.Readme.md gives the rules.
 */
#define KRFFS_S_IFMT   0170000u /* file-type mask within mode */
#define KRFFS_S_IFDIR  0040000u /* directory */
#define KRFFS_S_IFREG  0100000u /* regular file */
#define KRFFS_S_IFIFO  0010000u /* FIFO: a node with no data */
#define KRFFS_S_IFSOCK 0140000u /* socket file: a node with no data */

/* The dirty flag, a bit of the root node's mode: set while a writable mount
 * may have changes that are not yet on the medium, so that an image found
 * with it set is owed a check. A writable volume sets it as the last step of
 * its mount and clears it at its unmount, after a flush (krffsmount.c). A
 * disk root sets it at boot, written through, and `kernel_shutdown` clears
 * it when the debug shell's `halt`, `poweroff` or `reboot`, or `reboot(2)`,
 * stops the machine, and only if the write-back of the whole page cache
 * before it succeeded: a write-back walks the cache by slot and fails whole,
 * so a clear written first could reach the disk while another frame's write
 * failed, and leave an image that reads clean with data missing. A machine
 * that was killed, or whose flush failed, leaves it set. `sync` writes the
 * cache and leaves the flag. The bit lies above the nine permission bits
 * and below the 0170000 type field. No masked mode comparison sees it on
 * the root: stat reports the root with a fixed mode, chmod does not change
 * the root's mode, and the access check uses a fixed 0755 for the root. */
#define KRFFS_MODE_DIRTY 0004000u

/* The set-id and sticky bits of a node other than the root, stored and
 * reported in the 07777 width of the mode. `execve` applies the set-id bits
 * and the system call layer clears them after a modification; KRFFS only
 * stores them. KRFFS_MODE_SUID is the dirty flag's position, which is safe
 * because the dirty flag is only on the root node, and every permission
 * answer and every set-id clear tells the root by its identity rather than
 * by its mode. On a directory with the sticky bit, only an entry's owner,
 * the directory's owner or a privileged caller may remove or rename that
 * entry away. */
#define KRFFS_MODE_SUID      0004000u
#define KRFFS_MODE_SGID      0002000u
#define KRFFS_MODE_STICKY    0001000u
#define KRFFS_ROOT_ID        0u /* the parent_id of a node of the root directory */
#define KRFFS_PATH_MAX_DEPTH 64 /* max directory nesting krffs_node_path reconstructs */

/* A directory's mode is S_IFDIR with its permission bits; a regular file's
 * has no type bits, which `krffs_mode_type` reports as S_IFREG. A FIFO and
 * a socket have their own type bits and no data. */
#define KRFFS_IS_DIR(mode) (((mode) & KRFFS_S_IFMT) == KRFFS_S_IFDIR)

/**
 * The S_IF type bits a node's `mode` stands for: a directory's, a FIFO's or
 * a socket's own, and S_IFREG for a mode with none. The root's stored mode
 * has no type bits, so a caller answers for the root by identity.
 */
uint32_t krffs_mode_type(uint16_t mode);

/* Node types in the file system. */
typedef enum {
    KRFFS_ROOT_NODE,    /* The first node, which is the root directory */
    KRFFS_FREE_NODE,    /* Free space (available for allocation) */
    KRFFS_RESERVED_NODE /* A file, a directory, a FIFO or a socket file */
} krffs_node_type_t;

/* Node timestamp, packed for on-disk compatibility. */
#pragma pack(push, 1)
typedef struct {
    int64_t seconds;
    int64_t nanoseconds;
} krffs_node_time_t;
#pragma pack(pop)

/*
 * On-disk KRFFS node, little-endian. Every node shares this header; its data
 * follows it immediately. Layout: [node header (357 bytes)][data (variable)].
 * A directory's `id` is its parent's id mixed with the DJB hash of its name
 * (`krffs_mix_hash`); a regular file's is a hash of its name that no node
 * refers to.
 */
#pragma pack(push, 1)
typedef struct krffs_node {
    uint16_t magic;                            /* Magic number 0x4B52 "KR" */
    uint8_t type;                              /* Node type (see krffs_node_type_t) */
    uint64_t id;                               /* Name hash, or a directory's mixed id */
    uint8_t name[KRFFS_FILE_NAME_BUFFER_SIZE]; /* Null-terminated name; empty for an orphan */
    uint16_t mode;                             /* Type, set-id, sticky and permission bits */
    uint32_t uid;                              /* Owner user ID */
    uint32_t gid;                              /* Owner group ID */
    krffs_node_time_t access_time;             /* Last access time */
    krffs_node_time_t modification_time;       /* Last modification time */
    krffs_node_time_t attribute_change_time;   /* Last attribute change time */
    uint64_t size;                             /* Total node size (header + data space) */
    uint64_t previous_node_size;               /* Size of previous node (for backward traversal) */
    uint64_t data_size;                        /* Actual data bytes used */
    uint64_t parent_id; /* id of the containing directory (KRFFS_ROOT_ID at the root) */
} krffs_node_t;
#pragma pack(pop)

/* The boot root: its image in memory and its extent. */
typedef struct krffs_file_system {
    krffs_node_t *node; /* root node, the start of the file system */
    uint32_t size;      /* total size of the file system in bytes */
} krffs_file_system_t;

/*
 * How a mount reaches its image.
 *
 * The format is walked in offsets from the first byte of the region, and a
 * backend moves bytes between those offsets and wherever the image is.
 * `read` and `write` answer 0 on success and -1 on a failure. Writes may
 * reach the medium in any order until the next `sync`, so the order that
 * survives a stop is the caller's, and it is written down in
 * krffs.Readme.md. `sync` makes everything written so far durable, and
 * answers 0 or -1.
 *
 * The memory backend is one memcpy either way and never fails. It is the
 * only backend for the boot root, which is a mapped image.
 */
typedef struct krffs_backend {
    int (*read)(void *ctx, uint64_t offset, void *buffer, uint32_t size);
    int (*write)(void *ctx, uint64_t offset, const void *buffer, uint32_t size);
    int (*sync)(void *ctx);
} krffs_backend_t;

/* A mounted region: what reaches it, and how many bytes it has. A region is
 * addressed in 32 bits, which is the largest image this kernel can map or
 * cache and the largest file offset it has. */
typedef struct krffs_mount {
    const krffs_backend_t *backend;
    void *ctx; /* the backend's: the image's address, or the volume */
    uint32_t size;
    /* How many bytes the backend puts on the medium at a time, or 0 for one
     * that has no such unit. A field whose changed bytes fall in two of these
     * reaches the medium in two pieces, and a stop between them leaves a
     * value that was never written; the core fails such a write rather
     * than publish something no reader could make sense of. */
    uint32_t grain;
} krffs_mount_t;

/* The offset that is no node, since every real one is a byte of the region. */
#define KRFFS_NO_NODE ((uint64_t)-1)

/* Byte offsets of the fields a write publishes, for the backend's writes. */
#define KRFFS_FIELD_TYPE      2u
#define KRFFS_FIELD_ID        3u
#define KRFFS_FIELD_NAME      11u
#define KRFFS_FIELD_MODE      267u
#define KRFFS_FIELD_UID       269u
#define KRFFS_FIELD_GID       273u
#define KRFFS_FIELD_ATIME     277u
#define KRFFS_FIELD_MTIME     293u
#define KRFFS_FIELD_CTIME     309u
#define KRFFS_FIELD_SIZE      325u
#define KRFFS_FIELD_PREVIOUS  333u
#define KRFFS_FIELD_DATA_SIZE 341u
#define KRFFS_FIELD_PARENT    349u

/*
 * The fields every walk reads, which is the header without its name.
 *
 * The two runs of bytes are contiguous on the medium, so this is two reads:
 * the type and the id, then everything from the mode to the end of the
 * header. The layout follows the node's, so a read fills it directly.
 */
#pragma pack(push, 1)
typedef struct krffs_brief {
    uint8_t type;
    uint64_t id;
    uint16_t mode;
    uint32_t uid;
    uint32_t gid;
    krffs_node_time_t access_time;
    krffs_node_time_t modification_time;
    krffs_node_time_t attribute_change_time;
    uint64_t size;
    uint64_t previous_node_size;
    uint64_t data_size;
    uint64_t parent_id;
} krffs_brief_t;
#pragma pack(pop)

/*
 * The offset interface: the format itself, on a mount.
 *
 * These address nodes by their offset from the first byte of the region and
 * reach the medium through the mount's backend, so one implementation
 * serves the boot root in memory and a volume on a device. `KRFFS_NO_NODE`
 * is the answer that is no node, and offset 0 is always the root. None of
 * them takes a lock. Two calls that change one mount's image must not run
 * at once, and the adapters in vfs.c and krffsmount.c make such calls under
 * the mount's namespace lock. On a volume a call may sleep in the buffer
 * cache. The pointer interface below is the boot root's, and it is these
 * with the conversion done for it.
 */

/**
 * Read the header of the node at `offset` into `out`.
 *
 * Returns 0, or -1 for a null argument, for a header that would not fit
 * inside the region, which is what a walk that has left the ring reaches,
 * or for a read of the backend that failed.
 */
int krffs_head(krffs_mount_t *mount, uint64_t offset, krffs_node_t *out);

/**
 * The inode number `stat` and `getdents64` report for a node of the boot
 * root: 1 for the root node, a directory's id, and for any other node its
 * `parent_id` mixed with its `id` (`krffs_mix_hash`).
 *
 * A directory's id is unique among directories, so it serves. A regular
 * file's id is a hash of its name, which files of one name in different
 * directories share, so the parent goes into the number; the number does
 * not change when KRFFS moves the file's bytes. Two files of one directory
 * whose names have one hash share a number. A volume reports offsets
 * instead (krffsmount.c).
 */
uint64_t krffs_node_ino(const krffs_node_t *node);

/**
 * The whole check of the ring: every node inside the region with the magic
 * number, a known type, a size of at least one header, a `data_size` that
 * fits (a free node's excepted) and, if reserved, a name that ends within
 * its field; the sizes tiling the region exactly; and each backward link
 * the size of the node before it. Every step is bounded by
 * `krffs_max_steps`. 1 when the ring passes, 0 when it does not or a read
 * fails.
 */
int krffs_chain_is_valid(krffs_mount_t *mount);

/**
 * `krffs_chain_is_valid` without the backward links.
 *
 * What a mount requires of a volume it does not repair: the links are
 * derived from the forward chain, nothing that only reads follows them, and
 * a machine that stopped inside a change can have left them behind. A
 * mount that writes repairs them and then requires the whole check.
 */
int krffs_chain_is_walkable(krffs_mount_t *mount);

/* Which of the attributes a caller is setting. */
#define KRFFS_SET_UID   1
#define KRFFS_SET_GID   2
#define KRFFS_SET_ATIME 1
#define KRFFS_SET_MTIME 2

/**
 * Store the permission, set-id and sticky bits of `mode` in the node at
 * `offset`, keeping its type bits, so a directory stays a directory. 0, or
 * -1 when the header cannot be read or the store fails. The caller
 * stamps the change time, and does not pass the root, whose mode has the
 * dirty flag.
 */
int krffs_set_mode_at(krffs_mount_t *mount, uint64_t offset, uint16_t mode);

/**
 * Store `uid`, `gid` or both in the node at `offset`, as the KRFFS_SET_UID
 * and KRFFS_SET_GID bits of `which` select; 0, or -1 when a store fails.
 */
int krffs_set_owner_at(krffs_mount_t *mount, uint64_t offset, uint32_t uid, uint32_t gid,
                       int which);

/**
 * Store the access time of the node at `offset` for a `which` of
 * KRFFS_SET_ATIME, and its modification time otherwise; 0, or -1 when the
 * store fails.
 */
int krffs_set_time_at(krffs_mount_t *mount, uint64_t offset, int which, int64_t seconds,
                      int64_t nanoseconds);

/**
 * The most nodes the region can hold, which bounds every walk: its size in
 * headers, and one more. It comes from the region, since a fixed bound
 * would stop a walk short of a node on a large volume.
 */
uint64_t krffs_max_steps(krffs_mount_t *mount);

/**
 * The offset of the node after the one at `offset` whose header is `head`,
 * 0 after the last. A size smaller than a header, which fails the mount's
 * check, answers 0 as well, so a walk that stops at the root ends there
 * instead of looping.
 */
uint64_t krffs_next_offset(krffs_mount_t *mount, uint64_t offset, const krffs_node_t *head);

/** `krffs_walk_err` with no error code. */
uint64_t krffs_walk(krffs_mount_t *mount, const char *path);

/**
 * The offset of the child named `name` of the directory at `dir`, one step
 * of a walk, or KRFFS_NO_NODE when there is none or a read fails.
 *
 * `dir` 0, or KRFFS_NO_NODE, is the root directory. `name` is one component
 * of at least one byte, since an empty name matches an orphan; `.` and `..`
 * are the caller's.
 */
uint64_t krffs_child_at(krffs_mount_t *mount, uint64_t dir, const char *name);

/**
 * Resolve `path` to a node offset, honoring `.` and `..`.
 *
 * Slashes before and between components are skipped, and "/", "" or a NULL
 * `path` is the root, offset 0. A `..` whose directory cannot be found goes
 * to the root. A missing component answers KRFFS_NO_NODE with `*err`
 * -ENOENT, and a component before the last that is not a directory
 * KRFFS_NO_NODE with -ENOTDIR; `err` may be NULL, and is 0 on success. The
 * walk checks no search permission; the mount table does (vfs.c).
 */
uint64_t krffs_walk_err(krffs_mount_t *mount, const char *path, int *err);

/**
 * The offset of the directory the node at `offset` is in, 0 for the root
 * directory. KRFFS_NO_NODE for KRFFS_NO_NODE, for a header that cannot be
 * read, and for a `parent_id` no reserved node has, which an orphan whose
 * directory was removed, or a damaged image, gives.
 */
uint64_t krffs_parent_offset(krffs_mount_t *mount, uint64_t offset);

/**
 * Write the absolute, canonical path of the node at `offset` into `buf` of
 * `size` bytes, "/" for the root. 0, or -1 for a null `buf`, a `size` of 0
 * or KRFFS_NO_NODE, a path that does not fit, a node deeper than
 * KRFFS_PATH_MAX_DEPTH, a parent link no node answers, or a read that
 * fails.
 */
int krffs_node_path_at(krffs_mount_t *mount, uint64_t offset, char *buf, uint32_t size);

/**
 * Read up to `size` bytes of the data of the node at `node`, starting `at`
 * bytes in, into `buffer`.
 *
 * Returns the number of bytes read, 0 at or past the end of the data, or -1
 * for KRFFS_NO_NODE, a null `buffer`, a read that fails, or a node that is
 * not reserved, so that a handle to a node since freed reads nothing rather
 * than what the space now holds.
 */
int krffs_read_at(krffs_mount_t *mount, uint64_t node, char *buffer, uint32_t size, uint32_t at);

/**
 * Write `size` bytes from `buffer` into the data of the reserved node at
 * `*node`, `at` bytes in, growing the node when the data would pass its
 * room.
 *
 * A grow that moves the node stores the new offset in `*node` before the
 * bytes are written, so the caller repoints the node's holders even when the
 * write then fails. The bytes between the old end of the data and `at` are
 * zeroed, since they hold whatever the free space held. The data is synced
 * before `data_size` is raised to count it, and the modification and change
 * times are stamped. A write of 0 bytes changes nothing and answers 0, as
 * POSIX has it for a regular file.
 *
 * Returns `size`; KRFFS_EBAD for a null argument or KRFFS_NO_NODE, a header
 * that cannot be read, a node that is not reserved, such as one a failed
 * move freed while its holders kept the offset, or a write or sync that
 * failed, which can leave the bytes written and `data_size` not raised; or
 * KRFFS_ENOSPACE when the node had to grow and `krffs_resize_at` failed,
 * which a write that fails during the resize also gives.
 */
int krffs_write_at(krffs_mount_t *mount, uint64_t *node, const char *buffer, uint32_t size,
                   uint32_t at);

/**
 * `krffs_create_node_at` of a regular file, mode 0644, owned by root. An
 * existing name, of a file or a directory, answers 0 and is left as it is.
 */
int krffs_create_at(krffs_mount_t *mount, const char *path);

/**
 * Create an empty node at `path` with `mode`, which has the type bits of a
 * FIFO or a socket, or none for a regular file, owned by `uid` and `gid`.
 *
 * The node is allocated with an empty name. Its id, the DJB hash of its
 * name, its parent, the mode, the owner and the three times reach the
 * medium before the name does, so the node is never found with fields it
 * was not made with. The parent's modification and change times are then
 * stamped. An existing name answers 0 and is left as it is.
 *
 * Returns 0; KRFFS_EBAD for a path with no last name or whose directory is
 * not found; KRFFS_ENOSPACE when no free node can hold the node; or
 * KRFFS_EIO when a read, a write or a sync the creation needed failed,
 * which a device removed on the way causes, so a caller can tell a
 * full volume from a failing one. A node published before such a failure
 * has no name, and the repair or `fsck.krffs` reclaims it.
 */
int krffs_create_node_at(krffs_mount_t *mount, const char *path, uint16_t mode, uint32_t uid,
                         uint32_t gid);

/**
 * Set the data of the node at `node` to `new_size` bytes, zeroing any
 * growth, and stamp its modification and change times.
 *
 * It takes the node and not a path, because a file whose name has been
 * taken away is still a file its holder may truncate, and there is no path
 * that names it. `*node_out`, when `node_out` is not NULL, is where the
 * node is afterwards, which a grow with no room after it changes: the new
 * offset; the old one when a resize that failed left the node there; or
 * KRFFS_NO_NODE when a failed move had already freed the old node, so the
 * caller's holders keep what they have.
 *
 * Returns 0; KRFFS_EBAD for KRFFS_NO_NODE, the root, a directory, a node
 * that is not reserved, or a write that failed; or KRFFS_ENOSPACE when the
 * resize failed.
 */
int krffs_truncate_node_at(krffs_mount_t *mount, uint64_t node, uint32_t new_size,
                           uint64_t *node_out);

/**
 * `krffs_truncate_node_at` of the node `path` names, with a log record for
 * KRFFS_EBAD and KRFFS_ENOSPACE.
 */
int krffs_truncate_at(krffs_mount_t *mount, const char *path, uint32_t new_size,
                      uint64_t *node_out);

/**
 * Remove the node `path` names, which is not a directory, and stamp its
 * directory's modification and change times. 0, or -1 for a path that
 * names nothing, the root or a directory, or a removal that fails
 * (`krffs_remove_at`). The node is freed at once, so a caller whose node has
 * holders orphans it instead (`krffs_orphan_at`).
 */
int krffs_unlink_at(krffs_mount_t *mount, const char *path);

/**
 * Give the file `old_path` names the directory and the name of `new_path`.
 *
 * Neither the source nor an existing destination may be a directory, since
 * a directory's id is its parent's mixed with its name and its children
 * refer to it by that id; an existing destination file is removed first,
 * and freed whoever holds it. A rename to the name the file already has
 * does nothing at all, as POSIX requires. The node takes the DJB hash of
 * its new name as its id and the new parent, then a sync, then the name and
 * a sync. The directories left and entered get new modification and change
 * times, and the node a new change time.
 *
 * Returns 0, or -1 for a missing source, the root, a directory, a
 * destination path with no last name or whose directory is not found, an id
 * or a parent that would reach the medium in two pieces, which fails the
 * rename before anything is removed, a destination that is a directory or
 * cannot be removed, or a write that failed.
 */
int krffs_rename_at(krffs_mount_t *mount, const char *old_path, const char *new_path);

/**
 * Make an empty directory at `path` with S_IFDIR and the permission, set-id
 * and sticky bits of `mode`, owned by `uid` and `gid`, written as
 * `krffs_create_node_at` writes a node. Its id is its parent's mixed with
 * the hash of its name, which its children will have as their `parent_id`.
 *
 * Returns 0; KRFFS_EBAD for a name that is taken, or a path with no last
 * name or whose directory is not found; KRFFS_ETAKEN for a directory whose
 * id another directory has; or KRFFS_ENOSPACE or KRFFS_EIO as
 * `krffs_create_node_at` does.
 */
int krffs_mkdir_at(krffs_mount_t *mount, const char *path, uint16_t mode, uint32_t uid,
                   uint32_t gid);

/**
 * Remove the directory `path` names when no named node is in it, and stamp
 * its parent's modification and change times. An orphan does not keep the
 * directory, since it keeps its parent id but no path reaches it, so a
 * directory that holds only orphans is empty as POSIX means it. 0, or -1
 * for a path that is the root or not a directory, a named child, a read
 * that fails, or a removal that fails (`krffs_remove_at`). Whether the
 * directory is in use is the caller's question.
 */
int krffs_rmdir_at(krffs_mount_t *mount, const char *path);

/**
 * The `index`-th named child, in ring order, of the directory `path` names,
 * for a listing.
 *
 * Copies the child's name into `name_out`, cut to `buf_size` - 1 bytes and
 * ended with a NUL, and sets `*is_dir_out`, when `is_dir_out` is not NULL,
 * to 1 for a directory and 0 otherwise. Returns 1 for a child, 0 past the
 * last, and -1 for a null `name_out`, a `buf_size` of 0, a path that names
 * no directory, or a read that fails. Each call walks the ring from the
 * start, a teaching simplification.
 */
int krffs_readdir_at(krffs_mount_t *mount, const char *path, uint32_t index, char *name_out,
                     uint32_t buf_size, int *is_dir_out);

/**
 * Take the node at `offset` out of the namespace without freeing it, by
 * storing an empty name.
 *
 * No walk reaches a node with an empty name, and the scans that list or
 * count the children of a directory, or look for any, skip one, so it is in
 * no path, listing or count. It keeps its type, its data and its
 * parent id, so it is still a live file for whoever has it open and an
 * ordinary reserved node to the allocator. The caller frees it with
 * `krffs_remove_at` when its last holder lets it go. The root and
 * KRFFS_NO_NODE are left alone, and a write that fails is not reported.
 */
void krffs_orphan_at(krffs_mount_t *mount, uint64_t offset);

/**
 * Free every orphan on the medium. Only a mount calls it, since no node has
 * a holder then.
 *
 * An orphan is kept by its holders and freed by the last of them, so a stop
 * between the two leaves one on the medium with nobody left to free it, and
 * since nothing else looks for a node with no name, its space would be lost
 * for good. At a mount there are no descriptors, so every reserved node
 * without a name is garbage. Returns how many it freed; one whose removal
 * fails is left where it is and not counted.
 */
uint32_t krffs_reap_orphans_at(krffs_mount_t *mount);

/**
 * Stamp the node at `offset` with the current real time: the modification
 * and change times for `what` KRFFS_STAMP_DATA, what a write does, and the
 * change time alone for any other `what`, what a chmod does. KRFFS_NO_NODE
 * is left alone, and a write that fails is not reported.
 *
 * The access time is never stamped. KRFFS is reached through a write-back
 * cache, the page cache under a root in the window and the buffer cache
 * under a volume, so a stamp on every read would dirty a page on every read
 * and turn a reading workload into a writing one. An access time set
 * through `utimensat` is kept and reported, and nothing advances it, as on
 * a `noatime` mount; krffs.Readme.md states the rule.
 */
void krffs_stamp_at(krffs_mount_t *mount, uint64_t offset, int what);

/**
 * Allocate a reserved node with room for `size` bytes of data, first fit,
 * splitting the free node it takes when the rest can hold a header.
 *
 * With `original` a node, the new node is a copy of it, the move half of a
 * grow: its name, ids, mode, owner, times, data and `data_size`. With
 * KRFFS_NO_NODE it has an empty name and a `data_size` of `size`. The node
 * is published by its type, written last and synced. Returns its offset,
 * or KRFFS_NO_NODE when no free node fits or a read, a write or a sync
 * failed.
 */
uint64_t krffs_allocate_at(krffs_mount_t *mount, uint64_t size, uint64_t original);

/**
 * Give the reserved node at `offset` room for `size` bytes of data.
 *
 * Growing takes the free node after it when that is large enough, and
 * otherwise moves the node to a new allocation and frees the old one, so
 * the answer may be another offset. Shrinking returns the surplus to the
 * free list, merged with a free node after it. In either direction a
 * surplus too small for a header stays inside the node, where a later
 * shrink or a removal reclaims it. A grow changes the node's room and not
 * its length: `data_size` stays, and the caller stores the new length once
 * the bytes are written. A shrink below `data_size` stores the shorter
 * length first, since the surplus's header is written inside bytes the old
 * length counts.
 *
 * Returns the node's offset, or KRFFS_NO_NODE for a `size` larger than the
 * region, an offset that is no reserved node, a move that found no room, a
 * write that failed, or a move whose old node could not be freed. The last
 * leaves one name on two nodes, or the old offset inside a free node when
 * the removal failed after the type was stored.
 */
uint64_t krffs_resize_at(krffs_mount_t *mount, uint64_t offset, uint64_t size);

/**
 * Free the reserved node at `offset` and merge it with a free node after it
 * and a free node before it, so that no two free nodes are adjacent.
 *
 * The type is stored as free first, and each merge is published by one
 * size, so a machine that stops between them leaves a ring that can still
 * be walked; the backward link of the node after the merged one is repaired
 * last, and a sync ends the call. Returns the sync's answer, or -1 for an
 * offset that is no reserved node or a store of the type that failed, which
 * leaves the node reserved. A merge whose store failed leaves two free nodes
 * side by side.
 */
int krffs_remove_at(krffs_mount_t *mount, uint64_t offset);

/**
 * The named reserved nodes of the region, files and directories. An orphan
 * is not counted, since no path names it. A read that fails ends the count.
 */
uint32_t krffs_count_at(krffs_mount_t *mount);

/**
 * Set the root's dirty flag, or clear it for a `dirty` of 0, and sync. 0
 * when the flag already had that value or the store and the sync succeed,
 * -1 otherwise.
 */
int krffs_set_dirty(krffs_mount_t *mount, int dirty);

/**
 * 1 when the root's dirty flag is set, which says that a check is owed and
 * not what is wrong; 0 when it is clear or the root cannot be read.
 */
int krffs_is_dirty(krffs_mount_t *mount);

/**
 * Repair what a machine that stopped inside a change can leave.
 *
 * There are three faults, the ones the format's order of writes can
 * produce: a backward link the forward chain contradicts, rebuilt from the
 * walk; one name on two nodes of a directory, an interrupted move or
 * rename, where the loser's name is cleared; and reserved nodes with no
 * name, freed by `krffs_reap_orphans_at`. Of two nodes with one name, a
 * directory keeps it over a file, since its children name it by its id,
 * and otherwise the larger keeps it, since a move only makes a node larger.
 * The counts go to `links`, `orphans` and `doubles`, any of which may be
 * NULL.
 *
 * The forward chain must already pass `krffs_chain_is_walkable`. Returns
 * the last sync's answer, or -1 for a read or a write that failed or for a
 * volume of more than 4096 named nodes (KRFFS_REPAIR_NAMES in krffs.c),
 * which is left unwritten for `fsck.krffs`.
 */
int krffs_repair_at(krffs_mount_t *mount, uint32_t *links, uint32_t *orphans, uint32_t *doubles);

/**
 * The bytes still free in the region: the size of each free node less its
 * header, which is what a write can use. A region with no free node has 0.
 * A read that fails ends the sum.
 */
uint32_t krffs_free_at(krffs_mount_t *mount);

/*
 * The pointer interface of the boot root.
 *
 * The descriptor table, the system call layer and the boot code keep
 * `krffs_node_t` pointers into the root's mapped image, so these take and
 * answer with pointers and call the offset interface above on the root's
 * mount. A pointer outside the image is no node. None of them takes a lock
 * of its own.
 */

/* The boot root, or NULL before `init_krffs` succeeds. */
extern krffs_file_system_t *krffs_fs;

/**
 * The node after `node` in the ring of `fs`, the root after the last.
 *
 * A node smaller than its own header, which fails the mount's check,
 * answers the root, so a walk that stops at `fs->node` ends instead of
 * looping or reading out of bounds. NULL for a null argument.
 */
krffs_node_t *krffs_get_next_node(krffs_file_system_t *fs, krffs_node_t *node);

/**
 * `node` folded back into the region of `fs` by one region size when it
 * lies past either end of it; NULL for a null argument.
 */
krffs_node_t *krffs_wrap_node_position(krffs_file_system_t *fs, krffs_node_t *node);

/**
 * The child of `dir` named `name`, one step of a walk, for the VFS lookup.
 * `dir` NULL or the root node is the root directory; `name` is one
 * component of at least one byte. NULL when absent or before the mount.
 */
krffs_node_t *krffs_child(krffs_node_t *dir, const char *name);

/**
 * Resolve `path` to its node, the root node for "/", as `krffs_walk` does.
 * NULL before the mount, or for a missing component or one before the last
 * that is not a directory. Callers read the node's mode for its type.
 */
krffs_node_t *krffs_resolve(const char *path);

/**
 * `krffs_reap_orphans_at` on the boot root, which the disk root runs at
 * every mount, before any descriptor exists. Returns the count, 0 before
 * the mount.
 */
uint32_t krffs_reap_orphans(void);

/**
 * `krffs_orphan_at` on the boot root: take `node` out of the namespace
 * without freeing it, for a file somebody still holds. A null `node`, the
 * root and a pointer outside the image are left alone. The release of the
 * last holder frees it (syscall_fs.c).
 */
void krffs_orphan_node(krffs_node_t *node);

/**
 * Take the name off `node`, which somebody still holds, and stamp the
 * modification and change times of the directory it left.
 *
 * The stamp is here rather than at the two callers, the `unlink` path of
 * the system call layer and the debug shell's `rm`, so that neither can
 * leave it out: removing a name changes the directory whether or not the
 * file it named goes with it.
 */
void krffs_unlink_held(krffs_node_t *node);

/** Build the absolute, canonical path of `node` into buf; 0 ok, -1 before
 * the mount, for a null or foreign `node`, or if it does not fit. */
int krffs_node_path(krffs_node_t *node, char *buf, uint32_t size);

/* The errors of the offset interface, negative so that a byte count is
 * never one. A write returns the bytes written, `KRFFS_EBAD` for an
 * argument it cannot use or a write of the medium that failed, or
 * `KRFFS_ENOSPACE` when the file had to grow and the file system had no
 * room. The two are
 * separate because the second is an ordinary full disk, which a program is
 * expected to handle. */
#define KRFFS_EBAD     (-1)
#define KRFFS_ENOSPACE (-2)
/* What a creation answers, beside those two, when a read, a write or a sync
 * it needed failed: a failing or removed device, not a full one. */
#define KRFFS_EIO      (-3)
/* What mkdir answers for a directory whose identifier, its parent's mixed
 * with the hash of its name, another directory already has: two names can
 * have one hash, and a name the format cannot hold beside the other is
 * neither a full volume nor a failing one. */
#define KRFFS_ETAKEN   (-4)

/**
 * `krffs_read_at` on a node of the boot root, for a caller that has the
 * node and so need not resolve a path again. -1 for a null argument.
 */
int krffs_read_node(krffs_node_t *node, char *buffer, uint32_t size, uint32_t offset);

/**
 * `krffs_write_at` on a node of the boot root. `*nodep` is updated when a
 * grow moves the node. KRFFS_EBAD before the mount or for a null argument.
 */
int krffs_write_node(krffs_node_t **nodep, const char *buffer, uint32_t size, uint32_t offset);

/**
 * `krffs_truncate_node_at` on a node of the boot root. `*nodep` is where
 * the node is afterwards, NULL when a failed move freed it. KRFFS_EBAD
 * before the mount or for a null argument.
 */
int krffs_truncate_node(krffs_node_t **nodep, uint32_t new_size);

/* Read operations by path. */

/**
 * 0 when `path` names a node of the boot root other than the root and not
 * a directory; -1 otherwise, or before the mount. It keeps no state: a
 * descriptor resolves and keeps the node itself.
 */
int krffs_open(const char *path);

/**
 * `krffs_read_node` on the node `path` names; -1 for the root, a
 * directory, a missing path, a null `buffer`, or before the mount.
 */
int krffs_read(const char *path, char *buffer, uint32_t size, uint32_t offset);

/** The `data_size` of the node `path` names, 0 for the root, a missing
 * path, or before the mount. */
uint32_t krffs_get_file_size(const char *path);

/* Write operations by path. */
/* What krffs_stamp writes: data changed, or only the node's attributes. */
#define KRFFS_STAMP_DATA 1
#define KRFFS_STAMP_ATTR 2

/** `krffs_stamp_at` on a node of the boot root; a null `node` is left
 * alone. */
void krffs_stamp(krffs_node_t *node, int what);

/**
 * The directory `node` of the boot root is in, the root node for the root
 * directory. NULL before the mount, for a null `node`, and where
 * `krffs_parent_offset` finds no directory.
 */
krffs_node_t *krffs_parent_node(krffs_node_t *node);

/** `krffs_create_at` on the boot root; -1 before the mount. */
int krffs_create(const char *path);

/** `krffs_create_node_at` on the boot root, owned by root; -1 before the
 * mount. */
int krffs_create_node(const char *path, uint16_t mode);

/**
 * `krffs_write_node` on the node `path` names; -1 for the root, a
 * directory, a missing path, a null `buffer`, or before the mount. A move
 * of the node is not reported.
 */
int krffs_write(const char *path, const char *buffer, uint32_t size, uint32_t offset);

/**
 * `krffs_truncate_at` on the boot root, with only 0 or -1 for an answer.
 * A descriptor's truncation uses `krffs_truncate_node`, which also reports
 * a move.
 */
int krffs_truncate(const char *path, uint32_t new_size);

/** `krffs_unlink_at` on the boot root; -1 before the mount. */
int krffs_unlink(const char *path);

/** `krffs_rename_at` on the boot root; -1 before the mount. */
int krffs_rename(const char *old_path, const char *new_path);

/* Directory operations: create, remove (must be empty), and enumerate
 * children. */

/** `krffs_mkdir_at` on the boot root, mode 0755 owned by root; -1 before the
 * mount. */
int krffs_mkdir(const char *path);

/** `krffs_rmdir_at` on the boot root; -1 before the mount. */
int krffs_rmdir(const char *path);

/** `krffs_readdir_at` on the boot root; -1 before the mount. */
int krffs_readdir(const char *path, uint32_t index, char *name_out, uint32_t buf_size,
                  int *is_dir_out);

/**
 * `krffs_resize_at` on the boot root; NULL before the mount, when `fs` is
 * not `krffs_fs`, for a null `node`, or when the resize fails.
 */
krffs_node_t *krffs_resize_reserved_node(krffs_file_system_t *fs, krffs_node_t *node,
                                         uint64_t size);

/**
 * `krffs_remove_at` on the boot root, whose answer is not reported;
 * nothing before the mount, when `fs` is not `krffs_fs` or when `node` is
 * NULL.
 */
void krffs_remove_reserved_node(krffs_file_system_t *fs, krffs_node_t *node);

/**
 * Bernstein's DJB hash of the NUL-terminated `str`, `h = h * 33 ^ c` from
 * 5381 in 64 bits; NULL hashes to 5381. It is the id the kernel gives a
 * regular file, and the name's part of a directory's id.
 */
uint64_t krffs_calculate_djb_hash(const uint8_t *str);

/**
 * Mount the boot root: the KRFFS image of `ramdisk_size` bytes whose first
 * byte is at `ramdisk_base`, reached through the memory backend.
 *
 * The base is wherever the image can be addressed: the Multiboot module in
 * place, or `PCWIN_BASE`, where the page-cache window maps the module
 * (`rach2.fswindow`) or a disk partition (kernel.c). The first node must
 * have KRFFS_FILE_SYSTEM_MAGIC and the type KRFFS_ROOT_NODE, and the ring
 * must pass `krffs_chain_is_valid`. On success `krffs_fs` is the mounted
 * file system. Returns 0, or -1 with a log record for an image that is
 * missing, smaller than a header or malformed.
 */
int init_krffs(void *ramdisk_base, uint32_t ramdisk_size);

/** `krffs_count_at` on the boot root: its named reserved nodes, 0 before the
 * mount. */
uint32_t krffs_count_files(void);

/**
 * Record what the boot root was mounted from, and with what options, for
 * `/proc/mounts`.
 *
 * The boot code calls it at the moment a mount succeeds rather than derive
 * it afterwards, since the image does not say whether it came from a
 * module, the window over one or a disk. `source` is kept to 23 bytes and
 * `options` to 31, and NULL is the empty string.
 */
void krffs_set_mount(const char *source, const char *options);

/** The source `krffs_set_mount` recorded, "none" before it. */
const char *krffs_mount_source(void);

/** The options `krffs_set_mount` recorded, "ro" before it. */
const char *krffs_mount_options(void);

/** Bytes the boot root's region holds, for `statfs`; zero before a mount. */
uint32_t krffs_total_bytes(void);

/** Bytes still free in the boot root's region (`krffs_free_at`), for
 * `statfs`; zero before a mount. */
uint32_t krffs_free_bytes(void);

#endif /* _KRFFS_H_ */
