#ifndef KRFFS_NODE_H
#define KRFFS_NODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

struct krffs_file_system;

/** Maximum size of a file name buffer in a metadata node. */
#define KRFFS_FILE_NAME_BUFFER_SIZE 256

/**
 * Types of metadata node that make up the file system.
 *
 * The value is stored in each node's `type` byte on disk, so the order is
 * fixed: `KRFFS_Root_Node` (0) is the root directory, `KRFFS_Free_Node` (1) is
 * a free, unused block, and `KRFFS_Reserved_Node` (2) is a reserved block that
 * represents a file.
 */
enum krffs_node_type {
    KRFFS_Root_Node,
    KRFFS_Free_Node,
    KRFFS_Reserved_Node,
};

/**
 * Reserved bit in the root node's `mode` that flags an unclean mount.
 *
 * The kernel sets it (write-through) when it mounts a disk file system for
 * writing, and clears it on every route that stops the machine on purpose: the
 * debug shell's `halt`, `poweroff` and `reboot`, and `reboot(2)`, which the
 * userland `halt`, `poweroff` and `reboot` reach, bare through init or forced
 * with `-f`. The clear is conditional on the data, since the shutdown flushes
 * the whole cache first and touches the bit only if that returned 0; a
 * write-back walks the cache by slot and fails whole, so clearing first could
 * publish the cleared root page while another frame's write failed. What leaves
 * the bit set is a boot that was killed rather than stopped, or one whose flush
 * failed, which `fsck.krffs` reads as a file system that may not have flushed.
 * `sync` flushes the cache but does not clear it. It lives in the
 * root node's `mode` at bit `0004000`, which sits above the `0777` permission
 * bits the kernel keeps and below the `0170000` file-type field that
 * `KRFFS_IS_DIR` tests, so no reader's masked comparison of `mode` ever sees it.
 */
#define KRFFS_MODE_DIRTY 0004000u

/** A node timestamp, byte-packed as part of the on-disk node record. */
#pragma pack(push,1)
struct krffs_node_time {
    int64_t seconds;
    int64_t nanoseconds;
};
#pragma pack(pop)

/** Reads the wall clock as a `krffs_node_time`. */
struct krffs_node_time krffs_get_current_node_time();

/** Returns the seconds field of `node_time` as a `time_t`. */
time_t krffs_get_time_from_node_time(
           const struct krffs_node_time *node_time
       );

/** Converts `node_time` to a `struct timespec`. */
struct timespec krffs_get_timespec_from_node_time(
           const struct krffs_node_time *node_time
       );

/**
 * Metadata node describing one block of the file system. This is the on-disk
 * record: `#pragma pack(1)` removes padding, so the field order and widths here
 * are the KRFFS v2 ABI, shared byte-for-byte with the kernel.
 */
#pragma pack(push,1)
struct krffs_node {
    uint16_t magic;
    uint8_t type;
    uint64_t id;
    uint8_t name[KRFFS_FILE_NAME_BUFFER_SIZE];
    uint16_t mode;
    uint32_t uid;
    uint32_t gid;
    struct krffs_node_time access_time;
    struct krffs_node_time modification_time;
    struct krffs_node_time attribute_change_time;
    uint64_t size;
    uint64_t previous_node_size;
    uint64_t data_size;
    uint64_t parent_id;  /* id of the containing directory (v2 hierarchy) */
};
#pragma pack(pop)

/** Stamps `node` as a free node and flushes its header. */
struct krffs_node *krffs_initialize_free_node(
                       struct krffs_node *node,
                       uint64_t size,
                       uint64_t previous_node_size
                   );

/** Stamps `node` as the root node of the file system. */
struct krffs_node *krffs_initialize_root_node(
                       struct krffs_node *node,
                       uint64_t size,
                       uint64_t previous_node_size
                   );

/** Copies `name` into `node->name` and refreshes the `id` hash from it. */
void krffs_set_reserved_node_name(
         struct krffs_node *node,
         const char *name
     );

/** Folds a node pointer back into the file system's address range. */
struct krffs_node *krffs_wrap_node_position(
                       struct krffs_file_system *file_system,
                       struct krffs_node *node
                   );

/** Returns the node preceding `node` in the ring. */
struct krffs_node *krffs_get_previous_node(
                       struct krffs_file_system *file_system,
                       struct krffs_node *node
                   );

/** Returns the node following `node` in the ring. */
struct krffs_node *krffs_get_next_node(
                       struct krffs_file_system *file_system,
                       struct krffs_node *node
                   );

/** Returns the byte offset of `node` from the start of the mapping. */
uint64_t krffs_get_node_relative_position(
             struct krffs_file_system *file_system,
             struct krffs_node *node
         );

/** Reports whether `node` lies within the file system's mapping. */
bool krffs_is_node_in_file_system(
         struct krffs_file_system *file_system,
         struct krffs_node *node
     );

/** Returns the first reserved node named `name`, or `NULL` if none. */
struct krffs_node *krffs_find_reserved_node_by_name(
                       struct krffs_file_system *file_system,
                       const char *name
                   );

/** Like `krffs_find_reserved_node_by_name`, but also matches the root node. */
struct krffs_node *krffs_find_reserved_or_root_node_by_name(
                       struct krffs_file_system *file_system,
                       const char *name
                   );

/** Counts the free nodes in the file system. */
size_t krffs_count_free_nodes(
           struct krffs_file_system *file_system
       );

/** Sums the payload space available across all free nodes. */
uint64_t krffs_calculate_total_free_space(
             struct krffs_file_system *file_system
         );

/** Counts the reserved nodes in the file system. */
size_t krffs_count_reserved_nodes(
           struct krffs_file_system *file_system
       );

/** Sums the payload space allocated across all reserved nodes. */
uint64_t krffs_calculate_total_reserved_space(
             struct krffs_file_system *file_system
         );

#endif
