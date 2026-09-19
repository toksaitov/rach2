#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "krffs_file_system.h"
#include "krffs_node.h"
#include "krffs_platform.h"
#include "krffs_utilities.h"

#ifdef WINDOWS
#include <io.h>
#else
#include <unistd.h>
#endif

/**
 * Edits one node of a KRFFS file system held in a file.
 *
 * The node is located by its byte position relative to the start of the file
 * system, then each `--set-*` option overwrites a field of the on-disk node
 * record in place. The file is mapped writable, so the edit is committed by a
 * `krffs_sync_mapping` of the touched node. `--set-name` also recomputes the
 * node's `id` from the name via the djb hash, matching how the file system
 * derives ids.
 *
 * Usage:
 *
 *     edfs.krffs -h
 *     edfs.krffs <file> <node position> \
 *         [--set-magic <magic>]
 *         [--set-type root|free|reserved]
 *         [--set-id <id>]
 *         [--set-name <name>]
 *         [--set-size <size>]
 *         [--set-prev-size <size>]
 *         [--set-data-size <size>]
 *
 * `--set-magic` sets the node signature to a 16-bit unsigned number,
 * `--set-type` sets the type to `root`, `free`, or `reserved`, `--set-id` sets
 * the id to a 64-bit unsigned number, `--set-name` sets the name to a string of
 * up to 255 bytes, `--set-size`, `--set-prev-size`, and `--set-data-size` set
 * the size, previous-node-size, and data-size fields to 64-bit unsigned
 * numbers, and `-h` prints help and exits.
 */
int main(int argc, char **argv)
{
    int exit_status = EXIT_SUCCESS;

    int file_descriptor = -1;
    struct krffs_file_system file_system = {
        .node = NULL
    };

    bool has_help_option = false;

    bool has_set_magic_option = false;
    uint16_t magic = (uint16_t) -1;

    bool has_set_type_option = false;
    uint8_t type = (uint8_t) -1;

    bool has_set_id_option = false;
    uint64_t new_id = (uint64_t) -1;

    bool has_set_name_option = false;
    uint8_t name[KRFFS_FILE_NAME_BUFFER_SIZE];
    name[0] = '\0';

    bool has_set_size_option = false;
    uint64_t size = (uint64_t) -1;

    bool has_set_prev_size_option = false;
    uint64_t previous_node_size = (uint64_t) -1;

    bool has_set_data_size_option = false;
    uint64_t data_size = (uint64_t) -1;

    bool failed_to_parse_options = false;

    char *end_ptr = NULL;
    int base = 10;

    /* `-h` anywhere requests help, even on its own without a file and
     * position. */
    {
        int i;
        for (i = 1; i < argc; ++i) {
            if (strcmp(argv[i], "-h") == 0) {
                has_help_option = true;
            }
        }
    }

    uint64_t pos;
    if (argc > 2) {
        errno = 0;
        pos = (uint64_t) strtoll(argv[2], &end_ptr, base);
        if (argv[2] == end_ptr || *end_ptr != '\0' || errno != 0) {
            failed_to_parse_options = true;
        }
    }

    for (int i = 3; !failed_to_parse_options && i < argc; ++i) {
        if (strcmp(argv[i], "-h") == 0) {
            has_help_option = true;
        } else if (!has_set_magic_option && strcmp(argv[i], "--set-magic") == 0) {
            has_set_magic_option = true;
            if (i + 1 < argc) {
                unsigned long long magic_value;
                errno = 0;
                magic_value = strtoull(argv[i + 1], &end_ptr, base);
                if (argv[i + 1] == end_ptr || *end_ptr != '\0' || errno != 0 ||
                        magic_value > 0xFFFFu) {
                    failed_to_parse_options = true; /* not a 16-bit magic */
                } else {
                    magic = (uint16_t) magic_value;
                }
            } else {
                failed_to_parse_options = true;
            }
            ++i;
        } else if (!has_set_type_option && strcmp(argv[i], "--set-type") == 0) {
            has_set_type_option = true;
            if (i + 1 < argc) {
                for (char *curs = argv[i + 1]; *curs; ++curs) {
                    *curs = tolower(*curs);
                }
                if (strcmp(argv[i + 1], "root") == 0) {
                    type = KRFFS_Root_Node;
                } else if (strcmp(argv[i + 1], "reserved") == 0) {
                    type = KRFFS_Reserved_Node;
                } else if (strcmp(argv[i + 1], "free") == 0) {
                    type = KRFFS_Free_Node;
                } else {
                    failed_to_parse_options = true;
                }
            } else {
                failed_to_parse_options = true;
            }
            ++i;
        } else if (!has_set_id_option && strcmp(argv[i], "--set-id") == 0) {
            has_set_id_option = true;
            if (i + 1 < argc) {
                errno = 0;
                new_id = (uint64_t) strtoull(argv[i + 1], &end_ptr, base);
                if (argv[i + 1] == end_ptr || *end_ptr != '\0' || errno != 0) {
                    failed_to_parse_options = true;
                }
            } else {
                failed_to_parse_options = true;
            }
            ++i;
        } else if (!has_set_name_option && strcmp(argv[i], "--set-name") == 0) {
            has_set_name_option = true;
            if (i + 1 < argc) {
                strncpy(
                    (char *) name,
                    argv[i + 1],
                    KRFFS_FILE_NAME_BUFFER_SIZE
                );
                name[KRFFS_FILE_NAME_BUFFER_SIZE - 1] = '\0';
            } else {
                failed_to_parse_options = true;
            }
            ++i;
        } else if (!has_set_size_option && strcmp(argv[i], "--set-size") == 0) {
            has_set_size_option = true;
            if (i + 1 < argc) {
                errno = 0;
                size = (uint64_t) strtoull(argv[i + 1], &end_ptr, base);
                if (argv[i + 1] == end_ptr || *end_ptr != '\0' || errno != 0) {
                    failed_to_parse_options = true;
                }
            } else {
                failed_to_parse_options = true;
            }
            ++i;
        } else if (!has_set_prev_size_option && strcmp(argv[i], "--set-prev-size") == 0) {
            has_set_prev_size_option = true;
            if (i + 1 < argc) {
                errno = 0;
                previous_node_size = (uint64_t) strtoull(argv[i + 1], &end_ptr, base);
                if (argv[i + 1] == end_ptr || *end_ptr != '\0' || errno != 0) {
                    failed_to_parse_options = true;
                }
            } else {
                failed_to_parse_options = true;
            }
            ++i;
        } else if (!has_set_data_size_option && strcmp(argv[i], "--set-data-size") == 0) {
            has_set_data_size_option = true;
            if (i + 1 < argc) {
                errno = 0;
                data_size = (uint64_t) strtoull(argv[i + 1], &end_ptr, base);
                if (argv[i + 1] == end_ptr || *end_ptr != '\0' || errno != 0) {
                    failed_to_parse_options = true;
                }
            } else {
                failed_to_parse_options = true;
            }
            ++i;
        } else if (strcmp(argv[i], "--offset") == 0 || strcmp(argv[i], "--size") == 0) {
            ++i; /* a shared region option; krffs_parse_region_args consumes it below */
        } else {
            failed_to_parse_options = true; /* an unknown or duplicate option */
        }
    }

    if (has_help_option) {
        fprintf(
            stderr,
            "Usage: %s <file> <node position> <options>\n",
            argv[0]
        );

        goto cleanup;  /* help requested: a success exit */
    } else if (argc <= 2 || failed_to_parse_options) {
        /* A missing or unparseable argument must report failure, not exit 0 as
         * if the edit succeeded; a calling script relies on the status. */
        fprintf(
            stderr,
            "edfs.krffs: an option or its argument was missing or invalid\n"
            "Usage: %s <file> <node position> <options>\n",
            argv[0]
        );

        exit_status =
            EXIT_FAILURE;

        goto cleanup;
    } else if (argv[1][0] == '-' || argv[2][0] == '-') {
        fprintf(
            stderr,
            "Not all first parameters are valid.\n"
            "\n"
            "Usage: %s <file> <node position> <options>\n",
            argv[0]
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
        Writes to memory at `file_system.node` after a successful
        `krffs_map_file` reach the file directly, either at once or at
        `krffs_unmap_file` or `krffs_sync_mapping`.
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
        The mapping survives closing the file; the pages stay backed by it.
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
    do {
        if (krffs_get_node_relative_position(&file_system, node) == pos) {
            if (has_set_magic_option) {
                node->magic = magic;
            }

            if (has_set_type_option) {
                node->type = type;
            }

            if (has_set_id_option) {
                node->id = new_id;
            }

            if (has_set_name_option) {
                strncpy(
                    (char *) node->name,
                    (const char *) name,
                    KRFFS_FILE_NAME_BUFFER_SIZE
                );
                node->name[KRFFS_FILE_NAME_BUFFER_SIZE - 1] = '\0';

                node->id =
                    krffs_calculate_djb_hash(
                        (uint8_t *) node->name
                    );
            }

            if (has_set_size_option) {
                node->size = size;
            }

            if (has_set_prev_size_option) {
                node->previous_node_size = previous_node_size;
            }

            if (has_set_data_size_option) {
                node->data_size = data_size;
            }

            krffs_sync_mapping(
                node,
                sizeof(*node)
            );

            break;
        }
    } while ((node =
                krffs_get_next_node(
                    &file_system,
                    node
                )) != file_system.node);

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
