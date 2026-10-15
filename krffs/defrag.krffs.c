/*
 * defrag.krffs.c: defrag.krffs, which moves the free space of a KRFFS image
 * to its end.
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
#include <process.h>
#else
#include <unistd.h>
#include <sys/wait.h>
#endif

/** The checker `main` runs before it changes the image. */
#define KRFFS_FILE_SYSTEM_CHECK_UTILITY_NAME "fsck.krffs"

/**
 * Runs `program` with the one argument `path` and waits for it: `fork` and
 * `execvp` on Unix, `_spawnvp` on Windows. No shell reads `path`, so no byte
 * of it can change the command. Returns the exit status, 127 when the
 * program could not be started, or -1 when `fork` or `waitpid` failed or the
 * program ended on a signal.
 */
static int krffs_run_check_utility(const char *program, const char *path)
{
    const char *argv[3];

    argv[0] = program;
    argv[1] = path;
    argv[2] = NULL;

#ifdef WINDOWS
    {
        intptr_t result = _spawnvp(_P_WAIT, program, argv);

        return result < 0 ? 127 : (int)result;
    }
#else
    {
        pid_t pid = fork();

        if (pid < 0) {
            return -1;
        }

        if (pid == 0) {
            execvp(program, (char *const *)argv);
            _exit(127);
        }

        {
            int status = 0;

            if (waitpid(pid, &status, 0) < 0) {
                return -1;
            }

            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
    }
#endif
}

/**
 * defrag.krffs IMAGE [--offset BYTES --size BYTES]: moves the free space of
 * a KRFFS image to its end, as one free node.
 *
 * `fsck.krffs IMAGE` runs first, found on the PATH or else as `./fsck.krffs`,
 * and a status other than 0 stops the tool; the check is given IMAGE alone,
 * without the region options. Then, from the first free node on, each
 * reserved node is moved down over the free space before it, keeping the
 * order of the reserved nodes, and the free nodes passed are merged. With
 * sizes in bytes:
 *
 *     [free 2][used 5][free 3][used 2]
 *     [used 5][free 2][free 3][used 2]   the reserved node moves down
 *     [used 5][free 5][used 2]           the free nodes merge
 *     [used 5][used 2][free 5]           the next reserved node moves down
 *
 * With no arguments, or with one that begins with `-h`, the usage goes to
 * stderr and the exit status is 0; a first argument that begins with `-` is
 * an error. Returns `EXIT_SUCCESS` or `EXIT_FAILURE`.
 */
int main(int argc, char **argv)
{
    int exit_status = EXIT_SUCCESS;

    int file_descriptor = -1;
    struct krffs_file_system file_system = {.node = NULL};

    bool has_help_option = false;
    for (int i = 1; i < argc; ++i) {
        if (strncmp(argv[i], "-h", 2) == 0) {
            has_help_option = true;
            break;
        }
    }

    if (argc <= 1 || has_help_option) {
        fprintf(stderr, "Usage: %s <file>\n", argv[0]);

        goto cleanup;
    } else if (argv[1][0] == '-') {
        fprintf(stderr,
                "The first parameter is invalid.\n"
                "\n"
                "Usage: %s <file>\n",
                argv[0]);

        exit_status = EXIT_FAILURE;

        goto cleanup;
    }

    char *path = argv[1];

    int fsck_result = krffs_run_check_utility(KRFFS_FILE_SYSTEM_CHECK_UTILITY_NAME, path);

    if (fsck_result == -1 || fsck_result == 127) {
        fsck_result = krffs_run_check_utility("./" KRFFS_FILE_SYSTEM_CHECK_UTILITY_NAME, path);
    }

    if (fsck_result != 0) {
        fprintf(stderr, "Failed to check the file system file at '%s'.\n", path);

        exit_status = EXIT_FAILURE;

        goto cleanup;
    }

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

    if (file_system.node->magic != KRFFS_File_System_Magic) {
        fprintf(stderr,
                "The file system was not found at '%s'. The file system was not "
                "created or the file is corrupted.\n",
                path);

        exit_status = EXIT_FAILURE;

        goto cleanup;
    }

    struct krffs_node *node = file_system.node;
    struct krffs_node *previous_node = krffs_get_previous_node(&file_system, node);

    struct krffs_node *free_node = NULL;
    do {
        if (node->type == KRFFS_Free_Node) {
            free_node = node;

            break;
        }

        previous_node = node;
        node = krffs_get_next_node(&file_system, node);
    } while (node != file_system.node);

    if (free_node) {
        node = krffs_get_next_node(&file_system, free_node);

        struct krffs_node *reserved_node = NULL;
        do {
            reserved_node = NULL;
            do {
                if (node->type == KRFFS_Reserved_Node) {
                    reserved_node = node;

                    break;
                }

                if (node->type != KRFFS_Free_Node) {
                    break;
                }

                free_node->size += node->size;

                krffs_sync_mapping(free_node, sizeof(*free_node));
            } while ((node = krffs_get_next_node(&file_system, node)) != file_system.node);

            if (reserved_node) {
                uint64_t reserved_node_size = reserved_node->size;
                uint64_t free_node_size = free_node->size;

                memmove(free_node, reserved_node, reserved_node_size);

                reserved_node = free_node;
                reserved_node->previous_node_size = previous_node->size;

                krffs_sync_mapping(reserved_node, reserved_node_size);

                previous_node = reserved_node;

                free_node = krffs_get_next_node(&file_system, reserved_node);

                krffs_initialize_free_node(free_node, free_node_size, reserved_node_size);
            }
        } while (reserved_node &&
                 (node = krffs_get_next_node(&file_system, free_node)) != file_system.node);

        node->previous_node_size = free_node->size;

        krffs_sync_mapping(node, sizeof(*node));
    }

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
