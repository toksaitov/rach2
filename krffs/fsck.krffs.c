#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "krffs_file_system.h"
#include "krffs_node.h"
#include "krffs_platform.h"
#include "krffs_utilities.h"
#include "krffs_allocator.h"

#ifdef WINDOWS
#include <io.h>
#else
#include <unistd.h>
#endif

static const int KRFFS_Invalid_Link_Error            = -10,
                 KRFFS_Out_of_Range_Node_Error       = -11,
                 KRFFS_Invalid_Magic_Signature_Error = -12,
                 KRFFS_Unknown_Node_Type_Error       = -13;

/**
 * Checks the consistency of a KRFFS file system stored in a file.
 *
 * Usage: `fsck.krffs <file>`, or `fsck.krffs -h` for help. The check maps the
 * file, validates the file system magic and root node, then walks the node
 * list once, verifying that links are consecutive, stay within the file system
 * space, carry a valid signature, and name a known node type, and that the last
 * node links back to the first.
 *
 * Returns `EXIT_SUCCESS` on a clean file system and `EXIT_FAILURE` for a usage
 * or I/O error. A structural fault yields a specific negative status: `-10` for
 * a nonconsecutive link, `-11` for a link outside the file system space, `-12`
 * for an invalid signature, and `-13` for an unknown node type. Per-node debug
 * output goes to stdout and can be silenced with `> /dev/null`.
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
                argv[0] : "fsck.krffs"
        );

        goto cleanup;
    } else if (argv[1][0] == '-') {
        fprintf(
            stderr,
            "The first parameter is invalid.\n"
            "\n"
            "Usage: %s <file>\n",
            argc >= 1 ?
                argv[0] : "fsck.krffs"
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
        Map the file into memory through the kernel's virtual memory system.
        Writes to `file_system.node` reach the file directly, either right away
        or at `krffs_unmap_file` or `krffs_sync_mapping`.
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
        The mapping outlives the descriptor, so close the file now; the pages
        stay mapped to it.
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

    if (file_system.node->magic != KRFFS_File_System_Magic) {
        fprintf(
            stderr,
            "The file system was not found at '%s'. "
            "The file system was not created or the file is corrupted.\n",
            path
        );

        exit_status =
            EXIT_FAILURE;

        goto cleanup;
    }

    puts("The filesystem signature is correct.");

    if (file_system.node->type != KRFFS_Root_Node) {
        fprintf(
            stderr,
            "The root file system node was not found at '%s'. "
            "The file system was not created or the file is corrupted.\n",
            path
        );

        exit_status =
            EXIT_FAILURE;

        goto cleanup;
    }

    puts("The filesystem has a correct root node.");

    if (file_system.node->mode & KRFFS_MODE_DIRTY) {
        puts("The filesystem was not cleanly unmounted. Clearing the dirty flag.");
        file_system.node->mode &= (uint16_t) ~KRFFS_MODE_DIRTY;
    }

    uint64_t orphans = 0;

    struct krffs_node *previous_node =
        file_system.node;
    struct krffs_node *node =
        previous_node;

    /* A node whose size is zero (or too small) does not advance the walk, so cap
     * the iterations at the most nodes the mapping could hold; a chain that does
     * not return to the root within that bound is corrupt, not endless. */
    uint64_t steps =
        0;
    uint64_t max_steps =
        file_system.size / sizeof(struct krffs_node) + 1;

    puts("Checking the filesystem structure...");

    do {
        if (++steps > max_steps) {
            fprintf(
                stderr,
                "The node chain does not return to the root within the file "
                "system bound. The file is corrupt. Won't proceed.\n"
            );

            exit_status =
                KRFFS_Invalid_Link_Error;

            break;
        }

        if (previous_node > node) {
            printf(
                "- invalid link to %"PRIu64" "
                "where base is %"PRIu64" "
                "and limit %"PRIu64"\n"
                "\n",
                (uint64_t) node,
                (uint64_t) file_system.node,
                (uint64_t) file_system.node + file_system.size
            );

            fprintf(
                stderr,
                "The previous node link makes an invalid loop. "
                "Should point to the first node. "
                "Won't proceed.\n"
            );

            exit_status =
                KRFFS_Invalid_Link_Error;

            break;
        }

        if (!krffs_is_node_in_file_system(&file_system, node)) {
            printf(
                "- out of range node at %"PRIu64" "
                "where base is %"PRIu64" "
                "and limit %"PRIu64"\n"
                "\n",
                (uint64_t) node,
                (uint64_t) file_system.node,
                (uint64_t) file_system.node +
                file_system.size
            );

            fprintf(
                stderr,
                "Found an out of range node. Won't proceed.\n"
            );

            exit_status =
                KRFFS_Out_of_Range_Node_Error;

            break;
        }

        if (node->magic != KRFFS_File_System_Magic) {
            printf(
                "- invalid file system signature at %"PRIu64" "
                "in a node of type '%s'\n"
                "\n",
                (uint64_t) node,
                node->type == KRFFS_Free_Node ?
                    "free" : (node->type == KRFFS_Reserved_Node ? "reserved" : "unknown")
            );

            fprintf(
                stderr,
                "Found an invalid file system signature. Won't proceed.\n"
            );

            exit_status =
                KRFFS_Invalid_Magic_Signature_Error;

            break;
        }

        if (node->type == KRFFS_Root_Node) {
            printf(
                "* root node at position %"PRIu64" (owner %u:%u)\n",
                krffs_get_node_relative_position(&file_system, node),
                (unsigned) node->uid,
                (unsigned) node->gid
            );
        } else if (node->type == KRFFS_Reserved_Node &&
                   node->name[0] == '\0') {
            /* An orphan: a reserved node no path names, left by a crash between
               the last unlink or rename and the last close. The kernel reaps
               these when it mounts a dirty image; report them here so an
               operator reading fsck's output is not left wondering what the
               space is. */
            orphans++;
            printf(
                "+ orphan node at position %"PRIu64": %"PRIu64" bytes "
                "(%"PRIu64" bytes for data, owner %u:%u), reclaimed below\n",
                krffs_get_node_relative_position(&file_system, node),
                node->size,
                node->data_size,
                (unsigned) node->uid,
                (unsigned) node->gid
            );
        } else if (node->type == KRFFS_Reserved_Node) {
            printf(
                "+ reserved node with ID %"PRIu64" at position %"PRIu64": "
                "%"PRIu64" bytes (%"PRIu64" bytes for data, owner %u:%u) - [%s]\n",
                node->id,
                krffs_get_node_relative_position(&file_system, node),
                node->size,
                node->data_size,
                (unsigned) node->uid,
                (unsigned) node->gid,
                node->name
            );
        } else if (node->type == KRFFS_Free_Node) {
            printf(
                "- free node at position %"PRIu64": %"PRIu64" bytes\n",
                krffs_get_node_relative_position(&file_system, node),
                node->size
            );
        } else {
            printf(
                "- unknown node at position %"PRIu64"\n"
                "\n",
                krffs_get_node_relative_position(&file_system, node)
            );

            fprintf(
                stderr,
                "Found a node of an unknown type. Won't proceed.\n"
            );

            exit_status =
                KRFFS_Unknown_Node_Type_Error;

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

    /* Reclaim every orphan: a reserved node with no name, kept alive in a
       running system by the descriptors holding it and freed by the last close.
       A shutdown that closed none of them leaves one here with nothing left to
       do the freeing. The kernel reaps them at mount; doing it here as well is
       what makes the offline repair a repair.

       Only after the walk above has validated the chain end to end. Following
       node->size and calling into the allocator on an image whose links are
       corrupt is how a checker hangs, reads outside its mapping, or damages the
       thing it was asked to inspect; a structurally bad image is reported and
       left exactly as it was found. One bounded pass: a removal coalesces with
       whichever neighbours are free, and the merged region begins at the
       previous node when that was free and at this one otherwise, so the walk
       resumes there and steps over it by its new size. */
    if (exit_status == EXIT_SUCCESS) {
        uint64_t reclaimed = 0;
        uint64_t walked = 0;
        struct krffs_node *scan = krffs_get_next_node(&file_system,
                                                      file_system.node);
        struct krffs_node *prev = file_system.node;

        while (scan != file_system.node && walked <= max_steps) {
            walked++;
            if (scan->type == KRFFS_Reserved_Node && scan->name[0] == '\0') {
                struct krffs_node *merged =
                    (prev->type == KRFFS_Free_Node) ? prev : scan;

                krffs_remove_reserved_node(&file_system, scan);
                reclaimed++;
                scan = merged;
            }
            prev = scan;
            scan = krffs_get_next_node(&file_system, scan);
        }
        if (reclaimed != 0) {
            printf("Reclaimed %"PRIu64" orphan node(s) no path could reach.\n",
                   reclaimed);
        }
    }

    if (exit_status == EXIT_SUCCESS) {
        puts("The filesystem check has been completed "
             "successfully. There are no errors found.");
    } else {
        fprintf(stderr, "The file system check found errors.\n");
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
