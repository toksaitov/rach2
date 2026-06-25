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

/** Capacity of the on-stack buffer that holds the file system file path. */
#define KRFFS_PATH_BUFFER_LENGTH 1024

/**
 * Mounts a KRFFS file in a file under FUSE.
 *
 * `krffs <file> <mount point> <FUSE options>`. The first argument names the
 * backing file; the rest are FUSE's mount point and options. When `<file>` is
 * absent, or either of the first two arguments looks like an option (begins
 * with `-`), the arguments pass to FUSE untouched.
 *
 * The backing file must be a regular file at least two nodes long and carry
 * the KRFFS signature. It is opened, mapped, validated, and then closed; the
 * mapping survives the close. The mapped file system is published through
 * `KRFFS_File_System` before `fuse_main` runs. `argv[1]` is rewritten to `-s`
 * so FUSE runs single-threaded.
 */
int main(int argc, char **argv)
{
    int exit_status =
        EXIT_SUCCESS;

    int file_descriptor = -1;
    struct krffs_file_system file_system = {
        .node = NULL
    };

    /*
        Without our `<file>` argument, or when an argument is an option, pass
        everything untouched to FUSE.
     */
    if (argc < 3) {
        exit_status =
            fuse_main(
                argc,
                argv,
                &krffs_operations,
                NULL
            );

        goto cleanup;
    } else {
        char *path =
            argv[1];
        char *mount_point =
            argv[2];

        if (*path == '-' || *mount_point == '-') {
            exit_status =
                fuse_main(
                    argc,
                    argv,
                    &krffs_operations,
                    NULL
                );

            goto cleanup;
        }
    }

    /*
        Copy the path before we overwrite `argv[1]` ahead of the FUSE call.
     */
    char path[KRFFS_PATH_BUFFER_LENGTH + 1];
    path[KRFFS_PATH_BUFFER_LENGTH] = '\0';
    strncpy(
        path,
        argv[1],
        KRFFS_PATH_BUFFER_LENGTH
    );

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

    if (file_info.st_size < sizeof(*file_system.node) * 2) {
        fprintf(
            stderr,
            "The file at '%s' is not big enough to contain a file system.\n",
            path
        );

        exit_status =
            EXIT_FAILURE;

        goto cleanup;
    }

    file_system.size =
        file_info.st_size;

    /*
        Map the file into memory. After a successful `krffs_map_file`, writes to
        `file_system.node` reach the file (eagerly, or on `krffs_unmap_file` or
        `krffs_sync_mapping`).
     */
    if ((file_system.node =
             krffs_map_file(
                 file_descriptor,
                 0,
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
        The KRFFS signature is the magic of the first node at the start of the
        file.
     */
    if (file_system.node->magic != KRFFS_File_System_Magic) {
        fprintf(
            stderr,
            "The file system was not found at '%s'. The file system was not "
            "created or the file is corrupted.\n"
            "\n"
            "To create a file system use the `mkfs.krffs` program.\n",
            path
        );

        exit_status =
            EXIT_FAILURE;

        goto cleanup;
    }

    /*
        The mapping outlives the descriptor, so close it now.
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

    /*
        `-s` forces FUSE single-threaded; this overwrites our `<file>` argument,
        which FUSE no longer needs.
     */
    argv[1] = "-s";

    File_System_Store =
        file_system;
    KRFFS_File_System =
        &File_System_Store;

    exit_status =
        fuse_main(
            argc,
            argv,
            &krffs_operations,
            NULL
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
