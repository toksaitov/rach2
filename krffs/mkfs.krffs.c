/*
 * mkfs.krffs.c: mkfs.krffs, which writes an empty KRFFS file system into a
 * file.
 */

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
 * mkfs.krffs IMAGE [-f] [--offset BYTES --size BYTES]: writes an empty file
 * system, with `krffs_initialize_file_system`, over the region of IMAGE, an
 * existing regular file. The region is the whole file unless the region
 * options place it, and must be at least two headers.
 *
 * A region whose first node has the magic number is left alone unless an
 * argument begins with `-f`. With no arguments, or with one that begins with
 * `-h`, the usage goes to stderr and the exit status is 0; a first argument
 * that begins with `-` is an error. Returns `EXIT_SUCCESS` or `EXIT_FAILURE`.
 */
int main(int argc, char **argv)
{
    int exit_status = EXIT_SUCCESS;

    int file_descriptor = -1;
    struct krffs_file_system file_system = {.node = NULL};

    bool has_force_option = false;
    bool has_help_option = false;
    for (int i = 1; i < argc; ++i) {
        if (!has_force_option && strncmp(argv[i], "-f", 2) == 0) {
            has_force_option = true;
        } else if (!has_help_option && strncmp(argv[i], "-h", 2) == 0) {
            has_help_option = true;
        }
    }

    if (argc <= 1 || has_help_option) {
        fprintf(stderr, "Usage: %s <file> <options>\n", argv[0]);

        goto cleanup;
    } else if (argv[1][0] == '-') {
        fprintf(stderr,
                "The first parameter is invalid.\n"
                "\n"
                "Usage: %s <file> <options>\n",
                argv[0]);

        exit_status = EXIT_FAILURE;

        goto cleanup;
    }

    char *path = argv[1];

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

    off_t region_offset;
    size_t region_size;
    if (krffs_parse_region_args(argc, argv, (uint64_t)file_info.st_size,
                                sizeof(*file_system.node) * 2, &region_offset, &region_size) != 0) {
        exit_status = EXIT_FAILURE;

        goto cleanup;
    }

    file_system.size = region_size;

    if ((file_system.node = krffs_map_file(file_descriptor, region_offset, file_system.size)) ==
        (void *)-1) {
        fprintf(stderr, "Failed to map the file system file at '%s' into memory.\n", path);

        exit_status = EXIT_FAILURE;

        goto cleanup;
    }

    if (PLATFORM_PREFIX(close(file_descriptor)) == -1) {
        fprintf(stderr, "Failed to close the file system file at '%s'.\n", path);

        exit_status = EXIT_FAILURE;

        goto cleanup;
    }
    file_descriptor = -1;

    struct krffs_node *node = file_system.node;
    if (node->magic == KRFFS_File_System_Magic && !has_force_option) {
        fprintf(stderr,
                "Found a valid file system at '%s'. Will not proceed without the "
                "Force ('-f') option.\n",
                path);

        exit_status = EXIT_FAILURE;

        goto cleanup;
    }

    krffs_initialize_file_system(&file_system);

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
