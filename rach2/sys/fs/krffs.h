#ifndef _KRFFS_H_
#define _KRFFS_H_

/*
 * KRFFS - K&R First-Fit File System
 * Ported from the educational FUSE file system project for use in Rach2 kernel.
 * This is the read-only version for Phase 11.
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

/*
 * File system operations (read-only for Phase 11)
 */
int krffs_open(const char *path);
int krffs_read(const char *path, char *buffer, uint32_t size, uint32_t offset);
uint32_t krffs_get_file_size(const char *path);

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
