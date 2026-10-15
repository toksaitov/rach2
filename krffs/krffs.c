/*
 * krffs.c: the KRFFS FUSE driver, which mounts an image as one flat
 * directory.
 */

#include "krffs.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef WINDOWS
#include <io.h>
#else
#include <unistd.h>
#endif

/** The bytes of the image path that `main` keeps; a longer path is cut. */
#define KRFFS_PATH_BUFFER_LENGTH 1024

/**
 * Reports whether the region of `fs` is a whole ring of nodes, walked from
 * offset 0: each node has the magic number and a `size` of at least a header
 * that ends inside the region; the first is the root, and each after it is
 * free or reserved with the `size` of the node before as its
 * `previous_node_size`; a node that is not free has a NUL in its name and a
 * `data_size` that fits it; and the root's `previous_node_size` is the
 * last node's `size`. Returns 1 or 0.
 */
static int valid_node_ring(struct krffs_file_system *fs)
{
    uint64_t position = 0;
    uint64_t previous_size = 0;

    while (position < (uint64_t)fs->size) {
        struct krffs_node *node;
        uint64_t remaining = (uint64_t)fs->size - position;

        if (remaining < sizeof(*node)) {
            return 0;
        }
        node = (struct krffs_node *)((char *)fs->node + position);
        if (node->magic != KRFFS_File_System_Magic || node->size < sizeof(*node) ||
            node->size > remaining) {
            return 0;
        }
        if (position == 0) {
            if (node->type != KRFFS_Root_Node) {
                return 0;
            }
        } else if ((node->type != KRFFS_Free_Node && node->type != KRFFS_Reserved_Node) ||
                   node->previous_node_size != previous_size) {
            return 0;
        }
        if (node->type != KRFFS_Free_Node && (node->data_size > node->size - sizeof(*node) ||
                                              !memchr(node->name, '\0', sizeof(node->name)))) {
            return 0;
        }
        previous_size = node->size;
        position += node->size;
    }
    return fs->node->previous_node_size == previous_size;
}

/**
 * krffs IMAGE MOUNT_POINT [FUSE_OPTIONS]: mounts the KRFFS image IMAGE, the
 * whole file, under FUSE.
 *
 * With fewer than two arguments, or when the first or the second begins with
 * `-`, every argument goes to `fuse_main` as it is. Otherwise the image must
 * be a regular file of at least two headers that passes `valid_node_ring`.
 * It is mapped, its descriptor is closed, and the mapping becomes
 * `KRFFS_File_System`. Then `argv[1]` is replaced by `-s`, so that FUSE runs
 * the handlers, which take no lock, one at a time, and `fuse_main` runs.
 *
 * Returns what `fuse_main` returns, or `EXIT_FAILURE` when the image cannot
 * be opened, mapped, checked or closed.
 */
int main(int argc, char **argv)
{
    int exit_status = EXIT_SUCCESS;

    int file_descriptor = -1;
    struct krffs_file_system file_system = {.node = NULL};

    if (argc < 3) {
        exit_status = fuse_main(argc, argv, &krffs_operations, NULL);

        goto cleanup;
    } else {
        char *path = argv[1];
        char *mount_point = argv[2];

        if (*path == '-' || *mount_point == '-') {
            exit_status = fuse_main(argc, argv, &krffs_operations, NULL);

            goto cleanup;
        }
    }

    char path[KRFFS_PATH_BUFFER_LENGTH + 1];
    path[KRFFS_PATH_BUFFER_LENGTH] = '\0';
    strncpy(path, argv[1], KRFFS_PATH_BUFFER_LENGTH);

    if ((file_descriptor = PLATFORM_PREFIX(open(path, O_RDWR))) == -1) {
        fprintf(stderr, "Failed to open the file system file at '%s'.\n", path);

        exit_status = EXIT_FAILURE;

        goto cleanup;
    }

    struct stat file_info;
    if (fstat(file_descriptor, &file_info) == -1) {
        fprintf(stderr, "Failed to get file information for '%s'.\n", path);

        exit_status = EXIT_FAILURE;

        goto cleanup;
    }

    if (!S_ISREG(file_info.st_mode)) {
        fprintf(stderr, "The file system file at '%s' is not a regular file.\n", path);

        exit_status = EXIT_FAILURE;

        goto cleanup;
    }

    if ((uint64_t)file_info.st_size < sizeof(*file_system.node) * 2) {
        fprintf(stderr, "The file at '%s' is not big enough to contain a file system.\n", path);

        exit_status = EXIT_FAILURE;

        goto cleanup;
    }

    file_system.size = file_info.st_size;

    if ((file_system.node = krffs_map_file(file_descriptor, 0, file_system.size)) == (void *)-1) {
        fprintf(stderr, "Failed to map the file system file at '%s' into memory.\n", path);

        exit_status = EXIT_FAILURE;

        goto cleanup;
    }

    if (!valid_node_ring(&file_system)) {
        fprintf(stderr, "krffs: invalid node ring: '%s'\n", path);

        exit_status = EXIT_FAILURE;

        goto cleanup;
    }

    if (PLATFORM_PREFIX(close(file_descriptor)) == -1) {
        fprintf(stderr, "Failed to close the file system file at '%s'.\n", path);

        exit_status = EXIT_FAILURE;

        goto cleanup;
    }
    file_descriptor = -1;

    argv[1] = "-s";

    File_System_Store = file_system;
    KRFFS_File_System = &File_System_Store;

    exit_status = fuse_main(argc, argv, &krffs_operations, NULL);

cleanup:
    if (file_descriptor != -1) {
        if (PLATFORM_PREFIX(close(file_descriptor)) == -1) {
            fprintf(stderr, "Failed to close the file system file.\n");
        }
    }

    if (file_system.node != NULL && file_system.node != (void *)-1) {
        if (krffs_unmap_file(file_system.node, file_system.size) == -1) {
            fprintf(stderr, "Failed to unmap the file system file.\n");
        }
    }

    return exit_status;
}
