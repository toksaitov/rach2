/*
 * krffs_node.h: the KRFFS node header as the host tools see it in a mapped
 * region, and the walk of the ring of nodes.
 */
#ifndef KRFFS_NODE_H
#define KRFFS_NODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

struct krffs_file_system;

/** The bytes of a node's name field, the terminating NUL included. */
#define KRFFS_FILE_NAME_BUFFER_SIZE 256

/**
 * The types of node. The value is stored in each node's `type` byte, so the
 * order is fixed: `KRFFS_Root_Node` (0) is the root directory, the first node
 * of the region; `KRFFS_Free_Node` (1) is free space; and
 * `KRFFS_Reserved_Node` (2) is a file, a directory, a FIFO or a socket.
 */
enum krffs_node_type {
    KRFFS_Root_Node,
    KRFFS_Free_Node,
    KRFFS_Reserved_Node,
};

/**
 * The dirty flag, a bit of the root node's `mode`, which the kernel sets while
 * a writable mount may have changes that are not yet on the medium;
 * `KRFFS_MODE_DIRTY` in rach2/sys/fs/krffs.h gives the kernel's rules for it.
 * `fsck.krffs` reports a set flag and, except under `--strict`, clears it
 * once the image has passed the check. The FUSE driver neither shows it in
 * the root's mode nor changes it with `chmod`. The bit is above the nine
 * permission bits and below the 0170000 type field; on a node other than the
 * root it is the set-user-ID bit.
 */
#define KRFFS_MODE_DIRTY 0004000u

/**
 * The type field of a node's mode and a directory's value in it, as the
 * kernel writes them; the host's `S_IFDIR` need not be the same.
 */
#define KRFFS_MODE_IFDIR 0040000u
#define KRFFS_MODE_IFMT  0170000u

/** A node timestamp, byte-packed as part of the on-disk node record. */
#pragma pack(push, 1)
struct krffs_node_time {
    int64_t seconds;
    int64_t nanoseconds;
};
#pragma pack(pop)

/** Reads the wall clock, through `krffs_get_time_of_day`, as a node time. */
struct krffs_node_time krffs_get_current_node_time();

/** Returns the seconds field of `node_time` as a `time_t`. */
time_t krffs_get_time_from_node_time(const struct krffs_node_time *node_time);

/** Converts `node_time` to a `struct timespec`. */
struct timespec krffs_get_timespec_from_node_time(const struct krffs_node_time *node_time);

/**
 * The header of a node, 357 bytes, which the node's data follows. It is the
 * layout on the medium, packed, and field for field the kernel's
 * `krffs_node_t` (rach2/sys/fs/krffs.h).
 */
#pragma pack(push, 1)
struct krffs_node {
    /** `KRFFS_File_System_Magic`. */
    uint16_t magic;
    /** An `enum krffs_node_type`. */
    uint8_t type;
    /** The djb2 hash of the name, or the name's hash mixed with `parent_id`. */
    uint64_t id;
    /** NUL-terminated; empty for the root and for an orphan. */
    uint8_t name[KRFFS_FILE_NAME_BUFFER_SIZE];
    /** The type, set-ID, sticky and permission bits. */
    uint16_t mode;
    uint32_t uid;
    uint32_t gid;
    struct krffs_node_time access_time;
    struct krffs_node_time modification_time;
    struct krffs_node_time attribute_change_time;
    /** Bytes of header and data space: the step to the next node. */
    uint64_t size;
    /** The `size` of the node before; at the root, of the last node. */
    uint64_t previous_node_size;
    /** Bytes of data in use, at most `size` less the header. */
    uint64_t data_size;
    /** The `id` of the node's directory, 0 in the root directory. */
    uint64_t parent_id;
};
#pragma pack(pop)

/**
 * Makes `node` a free node of `size` bytes after a node of
 * `previous_node_size` bytes, and syncs its header to the file. Only the
 * magic number, the type, the two sizes and `data_size`, which becomes 0, are
 * written; the other fields keep what the bytes held. Returns `node`.
 */
struct krffs_node *krffs_initialize_free_node(struct krffs_node *node, uint64_t size,
                                              uint64_t previous_node_size);

/**
 * Makes `node` the root of an empty file system, a node of `size` bytes whose
 * predecessor, the last node, is `previous_node_size` bytes: the empty name
 * with its djb2 hash as `id`, mode 0755, the effective user and group of the
 * process (0:0 on Windows), the current time as all three times, no data,
 * and `parent_id` 0, which a forced `mkfs.krffs` over an old image would
 * otherwise leave as it was. The header is not synced. Returns `node`.
 */
struct krffs_node *krffs_initialize_root_node(struct krffs_node *node, uint64_t size,
                                              uint64_t previous_node_size);

/**
 * Copies `name`, cut to 255 bytes, into `node->name`, sets `id` to the djb2
 * hash of the stored name, and syncs the header to the file.
 */
void krffs_set_reserved_node_name(struct krffs_node *node, const char *name);

/**
 * Wraps `node` into the region: a pointer at or past its end is moved back
 * by the region's size, and one before its start forward by it, so a step of
 * less than the region's size from a node lands in the ring.
 */
struct krffs_node *krffs_wrap_node_position(struct krffs_file_system *file_system,
                                            struct krffs_node *node);

/**
 * Returns the node `previous_node_size` bytes before `node`, wrapped into the
 * ring. Nothing in the node found is checked.
 */
struct krffs_node *krffs_get_previous_node(struct krffs_file_system *file_system,
                                           struct krffs_node *node);

/**
 * Returns the node `size` bytes after `node`, wrapped into the ring, so the
 * last node's next is the root. Nothing in the node found is checked, and a
 * `size` of 0 returns `node`.
 */
struct krffs_node *krffs_get_next_node(struct krffs_file_system *file_system,
                                       struct krffs_node *node);

/** Returns the byte offset of `node` from the start of the region. */
uint64_t krffs_get_node_relative_position(struct krffs_file_system *file_system,
                                          struct krffs_node *node);

/** Reports whether the whole header of `node` lies within the mapping. */
bool krffs_is_node_in_file_system(struct krffs_file_system *file_system, struct krffs_node *node);

/**
 * Reports whether `node` is a directory: the root, or a reserved node whose
 * mode has the type `KRFFS_MODE_IFDIR`.
 */
bool krffs_is_directory(struct krffs_file_system *file_system, const struct krffs_node *node);

/**
 * Returns the id that the nodes in the directory `node` have as their
 * `parent_id`: 0 for the root, and the directory's own `id` otherwise.
 */
uint64_t krffs_get_directory_id(struct krffs_file_system *file_system,
                                const struct krffs_node *node);

/**
 * Returns the first reserved node in ring order whose `parent_id` is
 * `directory_id` and whose name is `name`, or NULL; the kernel's
 * `krffs_find_child` matches the same way. An empty `name` finds nothing: a
 * node with an empty name is an orphan, which the kernel unlinked while a
 * descriptor held it, and no path reaches it.
 */
struct krffs_node *krffs_find_child(struct krffs_file_system *file_system, uint64_t directory_id,
                                    const char *name);

/**
 * Returns the directory whose `id` is `id`, or NULL. As in the kernel's
 * `krffs_id_taken`, it is a reserved node with a directory's mode and a
 * name, since a node with no name is an orphan and no path reaches it.
 */
struct krffs_node *krffs_find_directory_by_id(struct krffs_file_system *file_system, uint64_t id);

/**
 * Returns the node at the absolute path made of the first `length` bytes of
 * `path`, or NULL. The walk starts at the root and finds each component in
 * the directory before it with `krffs_find_child`, as the kernel's walk
 * does, so a component before the last must be a directory. Slashes may
 * repeat or trail, and a path of slashes alone, or of none, is the root.
 * `.` and `..` are names like any other, and a component longer than 255
 * bytes is not found.
 */
struct krffs_node *krffs_find_node_by_path(struct krffs_file_system *file_system, const char *path,
                                           size_t length);

/** Counts the free nodes in the file system. */
size_t krffs_count_free_nodes(struct krffs_file_system *file_system);

/** Sums the sizes of the free nodes less one header each. */
uint64_t krffs_calculate_total_free_space(struct krffs_file_system *file_system);

/** Counts the reserved nodes in the file system. */
size_t krffs_count_reserved_nodes(struct krffs_file_system *file_system);

/**
 * Sums the sizes of the reserved nodes less one header each: the space
 * reserved, not the `data_size` in use.
 */
uint64_t krffs_calculate_total_reserved_space(struct krffs_file_system *file_system);

#endif
