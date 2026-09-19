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

/* Name of the file system check utility, invoked before defragmenting. */
#define KRFFS_FILE_SYSTEM_CHECK_UTILITY_NAME "fsck.krffs"

/**
 * Run `program` on `path` without a shell and return its exit status.
 *
 * The path is passed as a distinct argv element, so no byte of it is ever
 * interpreted by a shell: this is what closes the command injection a
 * `system()` with the path concatenated into the command string would open.
 * Returns the utility's exit status, or 127 when it could not be launched
 * (so the caller can fall back to a `./`-prefixed name), or -1 on a
 * fork/wait failure.
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
 * Defragments a KRFFS FUSE file system stored in a file.
 *
 * Runs `fsck.krffs` first and refuses to proceed unless it reports the file
 * system as valid. Maps the file, then slides every free block toward the end
 * by relocating the reserved blocks that follow it, coalescing free blocks as
 * it goes; the order of the reserved blocks is preserved.
 *
 * Usage:
 *     defrag.krffs -h
 *     defrag.krffs <file>
 *
 * Options:
 *     -h    show help and exit
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

    /* Refuse to defragment a file system that fails `fsck.krffs`. Try the
     * utility by name first (a PATH lookup), then the `./`-prefixed form for a
     * build that runs the tools out of the current directory. */
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

    /* The file system lives in a regular file, not a directory or socket. */
    if (!S_ISREG(file_info.st_mode)) {
        fprintf(stderr, "The file system file at '%s' is not a regular file.\n", path);

        exit_status = EXIT_FAILURE;

        goto cleanup;
    }

    /* A file system needs at least two nodes: one used and a trailing free. */
    off_t region_offset;
    size_t region_size;
    if (krffs_parse_region_args(argc, argv, (uint64_t)file_info.st_size,
                                sizeof(*file_system.node) * 2, &region_offset, &region_size) != 0) {
        exit_status = EXIT_FAILURE;

        goto cleanup;
    }

    file_system.size = region_size;

    /*
        Map the file into memory. After a successful `krffs_map_file`, writes
        to memory at `file_system.node` reach the file directly, either right
        away or at `krffs_unmap_file` or `krffs_sync_mapping`. The kernel backs
        the mapping with its virtual memory system.
     */
    if ((file_system.node = krffs_map_file(file_descriptor, region_offset, file_system.size)) ==
        (void *)-1) {
        fprintf(stderr, "Failed to map the file system file at '%s' into memory.\n", path);

        exit_status = EXIT_FAILURE;

        goto cleanup;
    }

    /* The mapping outlives the descriptor, so the file can be closed now. */
    if (PLATFORM_PREFIX(close(file_descriptor)) == -1) {
        fprintf(stderr, "Failed to close the file system file at '%s'.\n", path);

        exit_status = EXIT_FAILURE;

        goto cleanup;
    }
    file_descriptor = -1;

    /* The KRFFS magic signature heads the file when one is present. */
    if (file_system.node->magic != KRFFS_File_System_Magic) {
        fprintf(stderr,
                "The file system was not found at '%s'. The file system was not "
                "created or the file is corrupted.\n",
                path);

        exit_status = EXIT_FAILURE;

        goto cleanup;
    }

    /*
        Defragment a file system by moving free space between used (reserved)
        blocks toward the end. Do not change the order of used (reserved)
        blocks.

        Example
            node: [type, size in bytes]
                   free
                   used

                [free, 2]{  }[used, 5]{     }[free, 3]{   }[used, 2]{  }

            Move the first free block toward the end while relocating reserved
            (used) blocks. Combine consecutive free blocks to the right.

            Find the first free block.

                [free, 2]{  }[used, 5]{     }[free, 3]{   }[used, 2]{  }
                ^
                |
                -

            Move it one step toward the end.

                [free, 2]{  }[used, 5]{     }[free, 3]{   }[used, 2]{  }
                |                            ^
                |____________________________|

            Combine consecutive free blocks.

                [used, 5]{     }[free, 2]{  }[free, 3]{   }[used, 2]{  }
                                |            ^
                                |____________|
                                       +

                [used, 5]{     }[free, 5 (2+3)]{     }[used, 2]{  }

            Continue moving toward the end.

                [used, 5]{     }[free, 5]{     }[used, 2]{  }
                                |                            ^
                                |____________________________|

                [used, 5]{     }[used, 2]{  }[free, 5]{     }
    */

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
