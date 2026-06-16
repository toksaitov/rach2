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

/* The root node's name is the empty string (it represents the "/" directory). */
#define KRFFS_FILE_SYSTEM_ROOT_NODE_NAME ""

/* Default root node permissions (rwxr-xr-x). */
#define KRFFS_FILE_SYSTEM_ROOT_NODE_MODE 0755

/*
 * Hierarchical KRFFS (v2): a node's directory membership is its parent
 * directory's unique id, and a directory is a reserved node whose mode carries
 * the S_IFDIR type bit. The root directory has a fixed id KRFFS_ROOT_ID. A
 * node's own id is mix(parent_id, name), unique because sibling names are
 * unique, so parent_id references are unambiguous and survive node moves.
 */
#define KRFFS_S_IFMT  0170000u  /* file-type mask within mode */
#define KRFFS_S_IFDIR 0040000u  /* directory */
#define KRFFS_S_IFREG 0100000u  /* regular file */
#define KRFFS_ROOT_ID 0u        /* logical id of the root directory (host nodes zero parent_id) */
#define KRFFS_PATH_MAX_DEPTH 64 /* max directory nesting krffs_node_path reconstructs */

/* A directory's mode is S_IFDIR | 0755; a regular file is just its perm bits
 * (S_IFREG is implied when no type bits are set, for backward compatibility with
 * v1 images that stored bare permission modes). */
#define KRFFS_IS_DIR(mode) (((mode) & KRFFS_S_IFMT) == KRFFS_S_IFDIR)

/* Node types in the file system. */
typedef enum {
    KRFFS_ROOT_NODE,      /* Root directory node */
    KRFFS_FREE_NODE,      /* Free block (available for allocation) */
    KRFFS_RESERVED_NODE   /* Reserved block (file data) */
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
 * immediately. Layout: [node header (344 bytes)][data (variable)].
 */
#pragma pack(push, 1)
typedef struct krffs_node {
    uint16_t magic;                                  /* Magic number 0x4B52 "KR" */
    uint8_t type;                                    /* Node type (see krffs_node_type_t) */
    uint64_t id;                                     /* DJB hash of filename */
    uint8_t name[KRFFS_FILE_NAME_BUFFER_SIZE];       /* Null-terminated filename */
    uint16_t mode;                                   /* File permissions */
    uint32_t uid;                                    /* Owner user ID */
    uint32_t gid;                                    /* Owner group ID */
    krffs_node_time_t access_time;                   /* Last access time */
    krffs_node_time_t modification_time;             /* Last modification time */
    krffs_node_time_t attribute_change_time;         /* Last attribute change time */
    uint64_t size;                                   /* Total node size (header + data space) */
    uint64_t previous_node_size;                     /* Size of previous node (for backward traversal) */
    uint64_t data_size;                              /* Actual data bytes used */
    uint64_t parent_id;                              /* id of the containing directory (KRFFS_ROOT_ID at the root) */
} krffs_node_t;
#pragma pack(pop)

/* Mounted file system: the in-memory image and its extent. */
typedef struct krffs_file_system {
    krffs_node_t *node;    /* root node, the start of the file system */
    uint32_t size;         /* total size of the file system in bytes */
} krffs_file_system_t;

/* The mounted file system, or NULL before init_krffs. */
extern krffs_file_system_t *krffs_fs;

/* Node traversal: the next, previous, and wrapped position in the circular list. */
krffs_node_t *krffs_get_next_node(krffs_file_system_t *fs, krffs_node_t *node);
krffs_node_t *krffs_get_previous_node(krffs_file_system_t *fs, krffs_node_t *node);
krffs_node_t *krffs_wrap_node_position(krffs_file_system_t *fs, krffs_node_t *node);

/* Flat-namespace lookup by name; the second variant also matches the root. */
krffs_node_t *krffs_find_reserved_node_by_name(krffs_file_system_t *fs, const char *name);
krffs_node_t *krffs_find_reserved_or_root_node_by_name(krffs_file_system_t *fs, const char *name);

/** Resolve `path` to its node, or NULL if absent. */
krffs_node_t *krffs_resolve(const char *path);

/** As krffs_resolve, but *err reports -ENOENT or -ENOTDIR, 0 on success. */
krffs_node_t *krffs_resolve_e(const char *path, int *err);

/** Build the absolute, canonical path of `node` into buf; 0 ok, -1 if it does not fit. */
int krffs_node_path(krffs_node_t *node, char *buf, uint32_t size);

/* Read and write through a resolved node pointer, for the fd layer.
 * krffs_write_node updates *nodep if a grow relocates the node. */
int krffs_read_node(krffs_node_t *node, char *buffer, uint32_t size,
                    uint32_t offset);
int krffs_write_node(krffs_node_t **nodep, const char *buffer, uint32_t size,
                     uint32_t offset);

/* Read operations by path. */
int krffs_open(const char *path);
int krffs_read(const char *path, char *buffer, uint32_t size, uint32_t offset);
uint32_t krffs_get_file_size(const char *path);

/* Write operations by path. */
int krffs_create(const char *path);
int krffs_write(const char *path, const char *buffer, uint32_t size, uint32_t offset);
int krffs_truncate(const char *path, uint32_t new_size);
int krffs_unlink(const char *path);
int krffs_rename(const char *old_path, const char *new_path);

/* Directory operations: create, remove (must be empty), and enumerate children. */
int krffs_mkdir(const char *path);
int krffs_rmdir(const char *path);
int krffs_readdir(const char *path, uint32_t index, char *name_out,
                  uint32_t buf_size, int *is_dir_out);

/* Node primitives (internal, exposed for testing). */
krffs_node_t *krffs_initialize_free_node(krffs_node_t *node, uint64_t size, uint64_t previous_node_size);
void krffs_set_reserved_node_name(krffs_node_t *node, const char *name);

/* K&R first-fit allocator: allocate, resize, free a reserved node. */
krffs_node_t *krffs_allocate_reserved_node(krffs_file_system_t *fs, uint64_t size, krffs_node_t *original);
krffs_node_t *krffs_resize_reserved_node(krffs_file_system_t *fs, krffs_node_t *node, uint64_t size);
void krffs_remove_reserved_node(krffs_file_system_t *fs, krffs_node_t *node);

/* DJB hash of a NUL-terminated name; the legacy id of a regular file. */
uint64_t krffs_calculate_djb_hash(const uint8_t *str);

/** Mount the image at ramdisk_base; 0 ok, -1 on failure. */
int init_krffs(void *ramdisk_base, uint32_t ramdisk_size);

/* Count the reserved nodes, and list them to the serial console. */
uint32_t krffs_count_files(void);
void krffs_list_files(void);

#endif /* _KRFFS_H_ */
