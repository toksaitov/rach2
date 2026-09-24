#ifndef _KRFFS_H_
#define _KRFFS_H_

/*
 * KRFFS: K&R first-fit file system.
 *
 * On-disk node layout, the file system handle, and the operation interface.
 * The implementation and the full per-function contracts live in krffs.c.
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
 * Hierarchical KRFFS (v2): a node's directory membership is its parent
 * directory's unique id, and a directory is a reserved node whose mode has
 * the S_IFDIR type bit. The root directory has a fixed id KRFFS_ROOT_ID. A
 * node's own id is mix(parent_id, name), unique because sibling names are
 * unique, so parent_id references are unambiguous and survive node moves.
 */
#define KRFFS_S_IFMT  0170000u /* file-type mask within mode */
#define KRFFS_S_IFDIR 0040000u /* directory */
#define KRFFS_S_IFREG 0100000u /* regular file */

/* Reserved bit in the root node's mode flagging a possibly-unclean mount. Set
 * write-through when a disk file system is mounted for writing, and cleared by
 * every route that stops the machine on purpose: the debug shell's `halt`,
 * `poweroff` and `reboot`, and `reboot(2)`, which the userland `halt`,
 * `poweroff` and `reboot` reach, bare through init or forced with `-f`. What
 * leaves it set is a boot that was killed, not stopped, or one whose
 * flush failed, which fsck reads as a mount that may not have flushed. `sync`
 * flushes the cache but does not clear it. The clear is conditional on the
 * data: the shutdown path flushes the whole cache first and touches this bit
 * only if that flush returned 0, because a write-back walks the cache by slot
 * and fails whole, so clearing first can publish the cleared root page while
 * another frame's write fails and leave an image that reads clean with data
 * missing. It sits above the nine permission bits and below the 0170000 type
 * field KRFFS_IS_DIR tests. On the root node it is safe from every masked mode
 * comparison: stat reports the root with a fixed mode, chmod never touches it,
 * and the access check uses a fixed 0755 there. */
#define KRFFS_MODE_DIRTY 0004000u

/* The set-id and sticky bits on ordinary nodes, stored and reported through
 * the 07777 mode width. Exec applies the set-id bits, and a modification takes
 * them away again (the syscall layer owns both rules, since neither is the file
 * system's business). KRFFS_MODE_SUID shares the dirty flag's position by
 * design: the dirty flag lives only on the root node, which every permission
 * answer and every set-id clear excludes by node identity, not by
 * reading its mode, so the two can never be confused. The sticky bit is
 * enforced on a directory: with it set, only an entry's owner, the directory's
 * owner, or a privileged caller may remove or rename that entry away. */
#define KRFFS_MODE_SUID      0004000u
#define KRFFS_MODE_SGID      0002000u
#define KRFFS_MODE_STICKY    0001000u
#define KRFFS_ROOT_ID        0u /* logical id of the root directory (host nodes zero parent_id) */
#define KRFFS_PATH_MAX_DEPTH 64 /* max directory nesting krffs_node_path reconstructs */

/* A directory's mode is S_IFDIR | 0755; a regular file is just its perm bits
 * (S_IFREG is implied when no type bits are set, for backward compatibility
 * with v1 images that stored bare permission modes). */
#define KRFFS_IS_DIR(mode) (((mode) & KRFFS_S_IFMT) == KRFFS_S_IFDIR)

/* Node types in the file system. */
typedef enum {
    KRFFS_ROOT_NODE,    /* Root directory node */
    KRFFS_FREE_NODE,    /* Free block (available for allocation) */
    KRFFS_RESERVED_NODE /* Reserved block (file data) */
} krffs_node_type_t;

/* Node timestamp, packed for on-disk compatibility. */
#pragma pack(push, 1)
typedef struct {
    int64_t seconds;
    int64_t nanoseconds;
} krffs_node_time_t;
#pragma pack(pop)

/*
 * On-disk KRFFS node. Every node shares this header; its data follows it
 * immediately. Layout: [node header (357 bytes)][data (variable)].
 */
#pragma pack(push, 1)
typedef struct krffs_node {
    uint16_t magic;                            /* Magic number 0x4B52 "KR" */
    uint8_t type;                              /* Node type (see krffs_node_type_t) */
    uint64_t id;                               /* DJB hash of filename */
    uint8_t name[KRFFS_FILE_NAME_BUFFER_SIZE]; /* Null-terminated filename */
    uint16_t mode;                             /* File permissions */
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

/* Mounted file system: the in-memory image and its extent. */
typedef struct krffs_file_system {
    krffs_node_t *node; /* root node, the start of the file system */
    uint32_t size;      /* total size of the file system in bytes */
} krffs_file_system_t;

/*
 * How a mount reaches its image.
 *
 * The format is walked in offsets from the first byte of the region, and a
 * backend moves bytes between those offsets and wherever the image really is.
 * `read` and `write` answer 0 on success and -1 on a fault, and `write`
 * promises nothing about order beyond what the caller has already done: the
 * order that survives a stop is the caller's, and it is written down in
 * krffs.Readme.md. `sync` makes everything written so far durable.
 *
 * The memory backend is one memcpy either way. It is the only backend for a
 * root, which is a mapped image and cannot fault.
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
    void *ctx;
    uint32_t size;
    /* How many bytes the backend puts on the medium at a time, or 0 for one
     * that has no such unit. A field whose changed bytes fall in two of these
     * reaches the medium in two pieces, and a stop between them leaves a
     * value that was never written; the core refuses such a write rather
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
 * The format, on a mount.
 *
 * These are the file system itself: they address nodes by their offset from
 * the first byte of the region and reach the medium through the mount's
 * backend, so one implementation serves a root in memory and a volume on a
 * device. `KRFFS_NO_NODE` is the answer that is no node, and offset 0 is
 * always the root. The pointer interface below is the root's, and it is these
 * with the conversion done for it.
 */
int krffs_head(krffs_mount_t *mount, uint64_t offset, krffs_node_t *out);

/**
 * The inode number `stat` and `readdir` report for `node`.
 *
 * A directory's id is already `krffs_mix_id(parent, name)`, unique among
 * directories, so it serves. A regular file's id is the hash of its name
 * alone, kept for the on-disk format, so every file of one name shares it;
 * reported as the inode number, it made `cp -a` take same-named files in
 * different directories for hard links and copy them as one. The file is
 * given the directories' rule, the parent's id mixed with the name, which
 * does not move when KRFFS moves the file's bytes.
 */
uint64_t krffs_node_ino(const krffs_node_t *node);
int krffs_chain_is_valid(krffs_mount_t *mount);
int krffs_chain_is_walkable(krffs_mount_t *mount);

/* Which of the attributes a caller is setting. */
#define KRFFS_SET_UID   1
#define KRFFS_SET_GID   2
#define KRFFS_SET_ATIME 1
#define KRFFS_SET_MTIME 2

int krffs_set_mode_at(krffs_mount_t *mount, uint64_t offset, uint16_t mode);
int krffs_set_owner_at(krffs_mount_t *mount, uint64_t offset, uint32_t uid, uint32_t gid,
                       int which);
int krffs_set_time_at(krffs_mount_t *mount, uint64_t offset, int which, int64_t seconds,
                      int64_t nanoseconds);
uint64_t krffs_max_steps(krffs_mount_t *mount);
uint64_t krffs_next_offset(krffs_mount_t *mount, uint64_t offset, const krffs_node_t *head);
uint64_t krffs_walk(krffs_mount_t *mount, const char *path);
uint64_t krffs_child_at(krffs_mount_t *mount, uint64_t dir, const char *name);
uint64_t krffs_walk_err(krffs_mount_t *mount, const char *path, int *err);
uint64_t krffs_parent_offset(krffs_mount_t *mount, uint64_t offset);
int krffs_node_path_at(krffs_mount_t *mount, uint64_t offset, char *buf, uint32_t size);
int krffs_read_at(krffs_mount_t *mount, uint64_t node, char *buffer, uint32_t size, uint32_t at);
int krffs_write_at(krffs_mount_t *mount, uint64_t *node, const char *buffer, uint32_t size,
                   uint32_t at);
int krffs_create_at(krffs_mount_t *mount, const char *path);
int krffs_truncate_node_at(krffs_mount_t *mount, uint64_t node, uint32_t new_size,
                           uint64_t *node_out);
int krffs_truncate_at(krffs_mount_t *mount, const char *path, uint32_t new_size,
                      uint64_t *node_out);
int krffs_unlink_at(krffs_mount_t *mount, const char *path);
int krffs_rename_at(krffs_mount_t *mount, const char *old_path, const char *new_path);
int krffs_mkdir_at(krffs_mount_t *mount, const char *path);
int krffs_rmdir_at(krffs_mount_t *mount, const char *path);
int krffs_readdir_at(krffs_mount_t *mount, const char *path, uint32_t index, char *name_out,
                     uint32_t buf_size, int *is_dir_out);
void krffs_orphan_at(krffs_mount_t *mount, uint64_t offset);
uint32_t krffs_reap_orphans_at(krffs_mount_t *mount);
void krffs_stamp_at(krffs_mount_t *mount, uint64_t offset, int what);
uint64_t krffs_allocate_at(krffs_mount_t *mount, uint64_t size, uint64_t original);
uint64_t krffs_resize_at(krffs_mount_t *mount, uint64_t offset, uint64_t size);
int krffs_remove_at(krffs_mount_t *mount, uint64_t offset);
uint32_t krffs_count_at(krffs_mount_t *mount);
int krffs_set_dirty(krffs_mount_t *mount, int dirty);
int krffs_is_dirty(krffs_mount_t *mount);
int krffs_repair_at(krffs_mount_t *mount, uint32_t *links, uint32_t *orphans, uint32_t *doubles);
uint32_t krffs_free_at(krffs_mount_t *mount);

/* The mounted file system, or NULL before init_krffs. */
extern krffs_file_system_t *krffs_fs;

/* Node traversal: the next, previous, and wrapped position in the circular
 * list. */
krffs_node_t *krffs_get_next_node(krffs_file_system_t *fs, krffs_node_t *node);
krffs_node_t *krffs_wrap_node_position(krffs_file_system_t *fs, krffs_node_t *node);

/** The child of `dir` named `name` (one walk step); NULL if absent. */
krffs_node_t *krffs_child(krffs_node_t *dir, const char *name);

/** Resolve `path` to its node, or NULL if absent. */
krffs_node_t *krffs_resolve(const char *path);

/** Free every orphan a crash left behind; call at mount. Returns the count. */
uint32_t krffs_reap_orphans(void);

/** Take `node` out of the namespace without freeing it (see the definition). */
void krffs_orphan_node(krffs_node_t *node);
void krffs_unlink_held(krffs_node_t *node);

/** Build the absolute, canonical path of `node` into buf; 0 ok, -1 if it
 * does not fit. */
int krffs_node_path(krffs_node_t *node, char *buf, uint32_t size);

/* Read and write through a resolved node pointer, for the fd layer.
 * krffs_write_node updates *nodep if a grow relocates the node.
 *
 * It returns the bytes written, `KRFFS_EBAD` for an argument it cannot use, or
 * `KRFFS_ENOSPACE` when the file had to grow and the file system had no room.
 * The two are separate because the second is an ordinary full disk, which a
 * program is expected to handle, and the first is a fault in the caller. */
#define KRFFS_EBAD     (-1)
#define KRFFS_ENOSPACE (-2)
int krffs_read_node(krffs_node_t *node, char *buffer, uint32_t size, uint32_t offset);
int krffs_write_node(krffs_node_t **nodep, const char *buffer, uint32_t size, uint32_t offset);
int krffs_truncate_node(krffs_node_t **nodep, uint32_t new_size);

/* Read operations by path. */
int krffs_open(const char *path);
int krffs_read(const char *path, char *buffer, uint32_t size, uint32_t offset);
uint32_t krffs_get_file_size(const char *path);

/* Write operations by path. */
/* What krffs_stamp writes: data changed, or only the node's attributes. */
#define KRFFS_STAMP_DATA 1
#define KRFFS_STAMP_ATTR 2

void krffs_stamp(krffs_node_t *node, int what);
krffs_node_t *krffs_parent_node(krffs_node_t *node);

int krffs_create(const char *path);
int krffs_write(const char *path, const char *buffer, uint32_t size, uint32_t offset);
int krffs_truncate(const char *path, uint32_t new_size);
int krffs_unlink(const char *path);
int krffs_rename(const char *old_path, const char *new_path);

/* Directory operations: create, remove (must be empty), and enumerate
 * children. */
int krffs_mkdir(const char *path);
int krffs_rmdir(const char *path);
int krffs_readdir(const char *path, uint32_t index, char *name_out, uint32_t buf_size,
                  int *is_dir_out);

/* Node primitives (internal, exposed for testing). */
krffs_node_t *krffs_initialize_free_node(krffs_node_t *node, uint64_t size,
                                         uint64_t previous_node_size);

/* K&R first-fit allocator: allocate, resize, free a reserved node. */
krffs_node_t *krffs_allocate_reserved_node(krffs_file_system_t *fs, uint64_t size,
                                           krffs_node_t *original);
krffs_node_t *krffs_resize_reserved_node(krffs_file_system_t *fs, krffs_node_t *node,
                                         uint64_t size);
void krffs_remove_reserved_node(krffs_file_system_t *fs, krffs_node_t *node);

/* DJB hash of a NUL-terminated name; the legacy id of a regular file. */
uint64_t krffs_calculate_djb_hash(const uint8_t *str);

/** Mount the image at ramdisk_base; 0 ok, -1 on failure. */
int init_krffs(void *ramdisk_base, uint32_t ramdisk_size);

/* Count the reserved nodes, and list them to the serial console. */
uint32_t krffs_count_files(void);

/*
 * What the root was mounted from, and with what.
 *
 * Recorded at the moment a mount succeeds rather than derived afterwards: the
 * three branches that can mount this file system differ in ways nothing left
 * behind says, and a fixed line would call a RAM root a disk one on two boots
 * out of three. `/proc/mounts` is the reader.
 */
void krffs_set_mount(const char *source, const char *options);
const char *krffs_mount_source(void);
const char *krffs_mount_options(void);

/** Bytes the region holds, and bytes still free in it, for `statfs` and
 * `/proc/meminfo`'s file-system half. Zero before a mount. */
uint32_t krffs_total_bytes(void);
uint32_t krffs_free_bytes(void);

#endif /* _KRFFS_H_ */
