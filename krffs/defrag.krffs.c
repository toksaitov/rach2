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

/* Name of the file system check utility, invoked before defragmenting. */
#define KRFFS_FILE_SYSTEM_CHECK_UTILITY_NAME "./fsck.krffs "

/* Size of the buffer that holds the `fsck.krffs` command line. */
#define KRFFS_COMMAND_BUFFER_LENGTH 1024

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
    int exit_status =
        EXIT_SUCCESS;

    int file_descriptor = -1;
    struct krffs_file_system file_system = {
        .node = NULL
    };

    bool has_help_option  = false;
    for (int i = 1; i < argc; ++i) {
        if (strncmp(argv[i], "-h", 2) == 0) {
            has_help_option = true;
            break;
        }
    }

    if (argc <= 1 || has_help_option) {
        fprintf(
            stderr,
            "Usage: %s <file>\n",
            argc >= 1 ?
                argv[0] : "defrag.krffs"
        );

        goto cleanup;
    } else if (argv[1][0] == '-') {
        fprintf(
            stderr,
            "The first parameter is invalid.\n"
            "\n"
            "Usage: %s <file>\n",
            argc >= 1 ?
                argv[0] : "defrag.krffs"
        );

        exit_status =
            EXIT_FAILURE;

        goto cleanup;
    }

    char *path =
        argv[1];

    /* Refuse to defragment a file system that fails `fsck.krffs`. */
    char command_buffer[KRFFS_COMMAND_BUFFER_LENGTH + 1];
    command_buffer[KRFFS_COMMAND_BUFFER_LENGTH] = '\0';

    size_t command_length =
        strlen(KRFFS_FILE_SYSTEM_CHECK_UTILITY_NAME);

    strncpy(
        command_buffer,
        KRFFS_FILE_SYSTEM_CHECK_UTILITY_NAME,
        KRFFS_COMMAND_BUFFER_LENGTH
    );
    strncat(
        command_buffer,
        path,
        KRFFS_COMMAND_BUFFER_LENGTH -
            command_length
    );

    int fsck_result =
        system(command_buffer);

    if (fsck_result == -1 ||
            fsck_result == 127) {
        command_length =
            strlen("./"KRFFS_FILE_SYSTEM_CHECK_UTILITY_NAME);

        strncpy(
            command_buffer,
            "./"KRFFS_FILE_SYSTEM_CHECK_UTILITY_NAME,
            KRFFS_COMMAND_BUFFER_LENGTH
        );
        strncat(
            command_buffer,
            path,
            KRFFS_COMMAND_BUFFER_LENGTH -
                command_length
        );
        fsck_result =
            system(command_buffer);
    }

    if (fsck_result != 0) {
        fprintf(
            stderr,
            "Failed to check the file system file at '%s'.\n",
            path
        );

        exit_status =
            EXIT_FAILURE;

        goto cleanup;
    }

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

    /* The file system lives in a regular file, not a directory or socket. */
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

    /* A file system needs at least two nodes: one used and a trailing free. */
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
        to memory at `file_system.node` reach the file directly, either right
        away or at `krffs_unmap_file` or `krffs_sync_mapping`. The kernel backs
        the mapping with its virtual memory system.
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

    /* The mapping outlives the descriptor, so the file can be closed now. */
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

    /* The KRFFS magic signature heads the file when one is present. */
    if (file_system.node->magic != KRFFS_File_System_Magic) {
        fprintf(
            stderr,
            "The file system was not found at '%s'. The file system was not "
            "created or the file is corrupted.\n",
            path
        );

        exit_status =
            EXIT_FAILURE;

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

    struct krffs_node *node =
        file_system.node;
    struct krffs_node *previous_node =
        krffs_get_previous_node(
            &file_system,
            node
        );

    struct krffs_node *free_node = NULL;
    do {
        if (node->type == KRFFS_Free_Node) {
            free_node =
                node;

            break;
        }

        previous_node =
            node;
        node =
            krffs_get_next_node(
                &file_system,
                node
            );
    } while (node != file_system.node);

    if (free_node) {
        node =
            krffs_get_next_node(
                &file_system,
                free_node
            );

        struct krffs_node *reserved_node = NULL;
        do {
            reserved_node = NULL;
            do {
                if (node->type == KRFFS_Reserved_Node) {
                    reserved_node =
                        node;

                    break;
                }

                if (node->type != KRFFS_Free_Node) {
                    break;
                }

                free_node->size +=
                    node->size;

                krffs_sync_mapping(
                    free_node,
                    sizeof(*free_node)
                );
            } while ((node =
                          krffs_get_next_node(
                              &file_system,
                              node
                          )) != file_system.node);

            if (reserved_node) {
                uint64_t reserved_node_size =
                    reserved_node->size;
                uint64_t free_node_size =
                    free_node->size;

                memmove(
                    free_node,
                    reserved_node,
                    reserved_node_size
                );

                reserved_node =
                    free_node;
                reserved_node->previous_node_size =
                    previous_node->size;

                krffs_sync_mapping(
                    reserved_node,
                    reserved_node_size
                );

                previous_node =
                    reserved_node;

                free_node =
                    krffs_get_next_node(
                        &file_system,
                        reserved_node
                    );

                krffs_initialize_free_node(
                    free_node,
                    free_node_size,
                    reserved_node_size
                );
            }
        } while (reserved_node &&
                    (node =
                         krffs_get_next_node(
                             &file_system,
                            free_node
                         )) != file_system.node);

        node->previous_node_size =
            free_node->size;

        krffs_sync_mapping(
            node,
            sizeof(*node)
        );
    }

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
