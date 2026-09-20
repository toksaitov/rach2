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

/** Capacity of the on-stack buffer that has the file system file path. */
#define KRFFS_PATH_BUFFER_LENGTH 1024

/** Validate the complete node ring before mounting it. */
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
        if (node->magic != KRFFS_File_System_Magic ||
            node->size < sizeof(*node) || node->size > remaining) {
            return 0;
        }
        if (position == 0) {
            if (node->type != KRFFS_Root_Node) {
                return 0;
            }
        } else if ((node->type != KRFFS_Free_Node &&
                    node->type != KRFFS_Reserved_Node) ||
                   node->previous_node_size != previous_size) {
            return 0;
        }
        if (node->type != KRFFS_Free_Node &&
            (node->data_size > node->size - sizeof(*node) ||
             !memchr(node->name, '\0', sizeof(node->name)))) {
            return 0;
        }
        previous_size = node->size;
        position += node->size;
    }
    return fs->node->previous_node_size == previous_size;
}

/**
 * Mounts a KRFFS file in a file under FUSE.
 *
 * `krffs <file> <mount point> <FUSE options>`. The first argument names the
 * backing file; the rest are FUSE's mount point and options. When `<file>` is
 * absent, or either of the first two arguments looks like an option (begins
 * with `-`), the arguments pass to FUSE untouched.
 *
 * The backing file must be a regular file at least two nodes long and have
 * a valid node ring. It is opened, mapped, validated, and then closed; the
 * mapping survives the close. The mapped file system is published through
 * `KRFFS_File_System` before `fuse_main` runs. `argv[1]` is rewritten to `-s`
 * so FUSE runs single-threaded.
 */
int main(int argc, char **argv)
{
    int exit_status = EXIT_SUCCESS;

    int file_descriptor = -1;
    struct krffs_file_system file_system = {.node = NULL};

    /*
        Without our `<file>` argument, or when an argument is an option, pass
        everything untouched to FUSE.
     */
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

    /*
        Copy the path before we overwrite `argv[1]` ahead of the FUSE call.
     */
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

    if (file_info.st_size < sizeof(*file_system.node) * 2) {
        fprintf(stderr, "The file at '%s' is not big enough to contain a file system.\n", path);

        exit_status = EXIT_FAILURE;

        goto cleanup;
    }

    file_system.size = file_info.st_size;

    /*
        Map the file into memory. After a successful `krffs_map_file`, writes to
        `file_system.node` reach the file (eagerly, or on `krffs_unmap_file` or
        `krffs_sync_mapping`).
     */
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

    /*
        The mapping outlives the descriptor, so close it now.
     */
    if (PLATFORM_PREFIX(close(file_descriptor)) == -1) {
        fprintf(stderr, "Failed to close the file system file at '%s'.\n", path);

        exit_status = EXIT_FAILURE;

        goto cleanup;
    }
    file_descriptor = -1;

    /*
        `-s` forces FUSE single-threaded; this overwrites our `<file>` argument,
        which FUSE no longer needs.
     */
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
