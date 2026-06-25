#ifndef KRFFS_ALLOCATOR_H
#define KRFFS_ALLOCATOR_H

#include <stdint.h>

struct krffs_file_system;
struct krffs_node;

/**
 * Finds and reserves a free block of at least `size` bytes.
 *
 * This is the `malloc` of the K&R first-fit allocator: it walks the node ring
 * and reserves the first free block large enough to hold `size` plus a node
 * header. `original_node`, when not `NULL`, identifies the block being
 * relocated so its contents can be carried over.
 */
struct krffs_node *krffs_allocate_reserved_node(
           struct krffs_file_system *file_system,
           uint64_t size,
           struct krffs_node *original_node
       );

/**
 * Resizes a reserved block to `size` bytes.
 *
 * This is the `realloc` of the K&R first-fit allocator. When `node` cannot grow
 * in place it relocates the block, copying the contents to a fresh reservation.
 */
struct krffs_node *krffs_resize_reserved_node(
           struct krffs_file_system *file_system,
           struct krffs_node *node,
           uint64_t size
       );

/**
 * Frees a reserved block, returning it to the free pool.
 *
 * This is the `free` of the K&R first-fit allocator: it marks `node` free and
 * coalesces it with adjacent free blocks.
 */
void krffs_remove_reserved_node(
         struct krffs_file_system *file_system,
         struct krffs_node *node
     );

#endif
