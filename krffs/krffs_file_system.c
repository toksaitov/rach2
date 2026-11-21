/*
 * krffs_file_system.c: the layout of an empty file system, as
 * krffs_file_system.h declares it.
 */

#include "krffs_file_system.h"

#include "krffs_node.h"
#include "krffs_platform.h"

#include <assert.h>
#include <stddef.h>
#include <stdio.h>

struct krffs_file_system *krffs_initialize_file_system(struct krffs_file_system *file_system)
{
    assert(file_system && file_system->node);

    struct krffs_node *root_node = file_system->node;

    uint64_t root_node_size = sizeof(*root_node);
    uint64_t free_node_size = file_system->size - root_node_size;

    krffs_initialize_root_node(root_node, root_node_size, free_node_size);

    struct krffs_node *free_node = krffs_get_next_node(file_system, root_node);

    krffs_initialize_free_node(free_node, free_node_size, root_node_size);

    return file_system;
}

struct krffs_node *krffs_map_ordinary(int fd, off_t offset, size_t size, bool readonly)
{
    void *base =
        readonly ? krffs_map_file_readonly(fd, offset, size) : krffs_map_file(fd, offset, size);
    size_t at = 0;

    if (!base || base == (void *)-1) {
        return (void *)-1;
    }
    while (size - at >= sizeof(struct krffs_node)) {
        const struct krffs_node *node = (const void *)((const char *)base + at);

        if (node->type == KRFFS_Resizing_Node) {
            fprintf(stderr, "krffs: unfinished resize at %llu: run fsck.krffs\n",
                    (unsigned long long)at);
            krffs_unmap_file(base, size);
            return (void *)-1;
        }
        if (node->size < sizeof(*node) || node->size > size - at) {
            break;
        }
        at += (size_t)node->size;
    }
    return base;
}
