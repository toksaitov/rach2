#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "krffs_file_system.h"
#include "krffs_node.h"
#include "krffs_platform.h"

#ifdef WINDOWS
#include <io.h>
#else
#include <unistd.h>
#endif

/**
 * Writes a fresh KRFFS file system into an existing file.
 *
 * The target file must already exist and be a regular file at least two
 * `struct krffs_node` records long. Its contents are mapped, initialized as an
 * empty file system, and synced back through the mapping.
 *
 * Refuses to overwrite a file that already holds a valid KRFFS unless the force
 * (`-f`) option is given. With `-h`, or no arguments, it prints usage and
 * exits successfully.
 *
 * Usage:
 *
 *     mkfs.krffs -h
 *     mkfs.krffs <file> [-f]
 */
int main(int argc, char **argv)
{
    int exit_status =
        EXIT_SUCCESS;

    int file_descriptor = -1;
    struct krffs_file_system file_system = {
        .node = NULL
    };

    bool has_force_option = false;
    bool has_help_option  = false;
    for (int i = 1; i < argc; ++i) {
        if (!has_force_option && strncmp(argv[i], "-f", 2) == 0) {
            has_force_option = true;
        } else if (!has_help_option && strncmp(argv[i], "-h", 2) == 0) {
            has_help_option = true;
        }
    }

    if (argc <= 1 || has_help_option) {
        fprintf(
            stderr,
            "Usage: %s <file> <options>\n",
            argc >= 1 ?
                argv[0] : "mkfs.krffs"
        );

        goto cleanup;
    } else if (argv[1][0] == '-') {
        fprintf(
            stderr,
            "The first parameter is invalid.\n"
            "\n"
            "Usage: %s <file> <options>\n",
            argc >= 1 ?
                argv[0] : "mkfs.krffs"
        );

        exit_status =
            EXIT_FAILURE;

        goto cleanup;
    }

    char *path =
        argv[1];

    if ((file_descriptor = PLATFORM_PREFIX(open(path, O_RDWR))) == -1) {
        fprintf(
            stderr,
            "Failed to open the file system file at '%s'.\n",
            path
        );

        exit_status =
            EXIT_FAILURE;

        goto cleanup;
    }

    struct stat file_info;
    if (fstat(file_descriptor, &file_info) == -1) {
        fprintf(
            stderr,
            "Failed to get file information for '%s'.\n",
            path
        );

        exit_status =
            EXIT_FAILURE;

        goto cleanup;
    }

    if (!S_ISREG(file_info.st_mode)) {
        fprintf(
            stderr,
            "The file system file at '%s' is not a regular file.\n",
            path
        );

        exit_status =
            EXIT_FAILURE;

        goto cleanup;
    }

    off_t region_offset;
    size_t region_size;
    if (krffs_parse_region_args(
            argc, argv, (uint64_t) file_info.st_size,
            sizeof(*file_system.node) * 2, &region_offset, &region_size) != 0) {
        exit_status =
            EXIT_FAILURE;

        goto cleanup;
    }

    file_system.size =
        region_size;

    /*
        Map the file into memory. After a successful `krffs_map_file`, writes
        to `file_system.node` reach the file directly, either right away or on
        `krffs_unmap_file` or `krffs_sync_mapping`. The kernel backs the mapping
        with its virtual memory system.
     */
    if ((file_system.node =
             krffs_map_file(
                 file_descriptor,
                 region_offset,
                 file_system.size
             )) == (void *) -1) {
        fprintf(
            stderr,
            "Failed to map the file system file at '%s' into memory.\n",
            path
        );

        exit_status =
            EXIT_FAILURE;

        goto cleanup;
    }

    /*
        The descriptor may be closed once the mapping exists; the pages stay
        mapped to the file.
     */
    if (PLATFORM_PREFIX(close(file_descriptor)) == -1) {
        fprintf(
            stderr,
            "Failed to close the file system file at '%s'.\n",
            path
        );

        exit_status =
            EXIT_FAILURE;

        goto cleanup;
    }
    file_descriptor = -1;

    struct krffs_node *node = file_system.node;
    if (node->magic == KRFFS_File_System_Magic && !has_force_option) {
        fprintf(
            stderr,
            "Found a valid file system at '%s'. Will not proceed without the "
            "Force ('-f') option.\n",
            path
        );

        exit_status =
            EXIT_FAILURE;

        goto cleanup;
    }

    krffs_initialize_file_system(
        &file_system
    );

cleanup:
    if (file_descriptor != -1) {
        if (PLATFORM_PREFIX(close(file_descriptor)) == -1) {
            fprintf(
                stderr,
                "Failed to close the file system file.\n"
            );
        }
    }

    if (file_system.node != NULL && file_system.node != (void *) -1) {
        if (krffs_unmap_file(
                file_system.node,
                file_system.size
            ) == -1) {
            fprintf(
                stderr,
                "Failed to unmap the file system file.\n"
            );
        }
    }

    return exit_status;
}
