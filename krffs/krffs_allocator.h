/*
 * krffs_allocator.h: first fit over the ring of nodes, with split and
 * coalesce, for the stager, fsck.krffs and the FUSE driver.
 */
#ifndef KRFFS_ALLOCATOR_H
#define KRFFS_ALLOCATOR_H

#include <stdint.h>

struct krffs_file_system;
struct krffs_node;

/**
 * Reserves a node with room for `size` bytes of data, as the `malloc` of
 * K&R's first-fit allocator does: the first free node after the root that
 * holds a header and `size` bytes is split, the rest staying free, or taken
 * whole when the rest could not hold a header. The node gets the reserved
 * type and a `data_size` of `size`.
 *
 * With `original_node`, the node being moved by a resize, its name, `id`,
 * mode, owner, times and data are copied in and the header is synced.
 * Without it, the other fields keep the bytes the free node had, and the
 * caller sets them and syncs the header.
 *
 * Returns the node, or NULL when no free node is large enough or
 * `file_system` has no mapping.
 */
struct krffs_node *krffs_allocate_reserved_node(struct krffs_file_system *file_system,
                                                uint64_t size, struct krffs_node *original_node);

/**
 * Sets the data size of the reserved `node` to `size`, as the `realloc` of
 * K&R's allocator does. A node that shrinks gives its tail to a free node
 * after it, or makes a free node of the tail, or keeps a tail too small for a
 * header. A node that grows takes the room from a free node after it, the
 * whole of it when the rest could not hold a header; with no such room it
 * moves to a node from `krffs_allocate_reserved_node`, and `node` is freed.
 * The bytes past the old data are not cleared.
 *
 * Returns the node, which has moved when it is not `node`, or NULL, with
 * `node` as it was, when `node` is not reserved or no free node is large
 * enough.
 */
struct krffs_node *krffs_resize_reserved_node(struct krffs_file_system *file_system,
                                              struct krffs_node *node, uint64_t size);

/**
 * Frees the reserved `node`, as the `free` of K&R's allocator does: it becomes
 * a free node, merged with a free node after it and one before it, and the
 * node after the result gets its `previous_node_size`. The merge follows
 * `previous_node_size` back, so the links must be right. The freed header
 * keeps its name and `data_size`. A `node` that is not reserved is left
 * alone.
 */
void krffs_remove_reserved_node(struct krffs_file_system *file_system, struct krffs_node *node);

#endif
