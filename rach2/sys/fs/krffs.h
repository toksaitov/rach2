#ifndef _KRFFS_H_
#define _KRFFS_H_

/*
 * KRFFS - K&R First-Fit File System
 * Ported from the educational FUSE file system project for use in Rach2 kernel.
 * Phase 11: Read-only operations
 * Phase 12: Write operations (create, write, truncate, unlink, rename)
 */

#include "../lib/kint.h"
#include "../lib/kdef.h"
#include "../lib/kstring.h"

/*
 * File system magic number: "KR" in little-endian
 */
#define KRFFS_FILE_SYSTEM_MAGIC 0x4B52

/*
 * Maximum file name length (including null terminator)
 */
#define KRFFS_FILE_NAME_BUFFER_SIZE 256

/*
 * Root node name is empty string (represents "/" directory)
 */
#define KRFFS_FILE_SYSTEM_ROOT_NODE_NAME ""

/*
 * Default root node permissions (rwxr-xr-x)
 */
#define KRFFS_FILE_SYSTEM_ROOT_NODE_MODE 0755

/*
 * Hierarchical KRFFS (v2): a node's directory membership is its parent
 * directory's unique id, and a directory is a reserved node whose mode carries
 * the S_IFDIR type bit. The root directory has a fixed id KRFFS_ROOT_ID. A
 * node's own id is mix(parent_id, name) -- unique because sibling names are
 * unique -- so parent_id references are unambiguous and survive node moves.
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

/*
 * Node types in the file system
 */
typedef enum {
    KRFFS_ROOT_NODE,      /* Root directory node */
    KRFFS_FREE_NODE,      /* Free block (available for allocation) */
    KRFFS_RESERVED_NODE   /* Reserved block (file data) */
} krffs_node_type_t;

/*
 * Time structure for node timestamps
 * Stored in packed format for disk compatibility
 */
#pragma pack(push, 1)
typedef struct {
    int64_t seconds;
    int64_t nanoseconds;
} krffs_node_time_t;
#pragma pack(pop)

/*
 * KRFFS node structure - stored on disk/RAM disk
 * All nodes have the same header structure.
 * File data immediately follows the header.
 *
 * Layout: [Node Header (344 bytes)][Data (variable)]
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

/*
 * File system structure - points to the RAM disk in memory
 */
typedef struct krffs_file_system {
    krffs_node_t *node;    /* Pointer to root node (start of file system) */
    uint32_t size;         /* Total size of file system in bytes */
} krffs_file_system_t;

/*
 * Global file system instance
 */
extern krffs_file_system_t *krffs_fs;

/*
 * Node traversal functions
 */
krffs_node_t *krffs_get_next_node(krffs_file_system_t *fs, krffs_node_t *node);
krffs_node_t *krffs_get_previous_node(krffs_file_system_t *fs, krffs_node_t *node);
krffs_node_t *krffs_wrap_node_position(krffs_file_system_t *fs, krffs_node_t *node);

/*
 * Node search functions
 */
krffs_node_t *krffs_find_reserved_node_by_name(krffs_file_system_t *fs, const char *name);
krffs_node_t *krffs_find_reserved_or_root_node_by_name(krffs_file_system_t *fs, const char *name);

/* Hierarchical path resolver (KRFFS v2): the node for `path`, or NULL if absent. */
krffs_node_t *krffs_resolve(const char *path);

/* As krffs_resolve, but *err distinguishes -ENOENT (missing) from -ENOTDIR (a
 * non-directory component); 0 on success. */
krffs_node_t *krffs_resolve_e(const char *path, int *err);

/* Reconstruct the absolute, canonical path of `node` into buf (chdir/getcwd and
 * dirfd resolution use it). Returns 0 on success, -1 if it does not fit. */
int krffs_node_path(krffs_node_t *node, char *buf, uint32_t size);

/*
 * Read/write directly via a resolved node pointer (used by the fd layer so a file
 * descriptor need not re-resolve a path -- which fails for a file in a subdir,
 * whose node->name is only the basename). krffs_write_node updates *nodep if a
 * grow relocates the node.
 */
int krffs_read_node(krffs_node_t *node, char *buffer, uint32_t size,
                    uint32_t offset);
int krffs_write_node(krffs_node_t **nodep, const char *buffer, uint32_t size,
                     uint32_t offset);

/*
 * File system operations - read
 */
int krffs_open(const char *path);
int krffs_read(const char *path, char *buffer, uint32_t size, uint32_t offset);
uint32_t krffs_get_file_size(const char *path);

/*
 * File system operations - write (Phase 12)
 */
int krffs_create(const char *path);
int krffs_write(const char *path, const char *buffer, uint32_t size, uint32_t offset);
int krffs_truncate(const char *path, uint32_t new_size);
int krffs_unlink(const char *path);
int krffs_rename(const char *old_path, const char *new_path);

/*
 * Directory operations (KRFFS v2). mkdir/rmdir create/remove an (empty)
 * directory; readdir returns the index-th child name of a directory.
 */
int krffs_mkdir(const char *path);
int krffs_rmdir(const char *path);
int krffs_readdir(const char *path, uint32_t index, char *name_out,
                  uint32_t buf_size, int *is_dir_out);

/*
 * Node manipulation functions (internal, but exposed for testing)
 */
krffs_node_t *krffs_initialize_free_node(krffs_node_t *node, uint64_t size, uint64_t previous_node_size);
void krffs_set_reserved_node_name(krffs_node_t *node, const char *name);

/*
 * K&R allocator functions
 */
krffs_node_t *krffs_allocate_reserved_node(krffs_file_system_t *fs, uint64_t size, krffs_node_t *original);
krffs_node_t *krffs_resize_reserved_node(krffs_file_system_t *fs, krffs_node_t *node, uint64_t size);
void krffs_remove_reserved_node(krffs_file_system_t *fs, krffs_node_t *node);

/*
 * Utility functions
 */
uint64_t krffs_calculate_djb_hash(const uint8_t *str);

/*
 * File system initialization
 * Returns 0 on success, -1 on failure
 */
int init_krffs(void *ramdisk_base, uint32_t ramdisk_size);

/*
 * Count and list files
 */
uint32_t krffs_count_files(void);
void krffs_list_files(void);

#endif /* _KRFFS_H_ */
