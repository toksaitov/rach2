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

#ifdef WINDOWS
#include <io.h>
#else
#include <unistd.h>
#endif

static const int KRFFS_Invalid_Link_Error            = -10,
                 KRFFS_Out_of_Range_Node_Error       = -11,
                 KRFFS_Invalid_Magic_Signature_Error = -12,
                 KRFFS_Unknown_Node_Type_Error       = -13;

/*
    fsck.krffs

    Usage:
        fsck.krffs -h
        fsck.krffs <file>

    Options:
        -h    show help and exit
 */
int main(int argc, char **argv)
{
    int exit_status = EXIT_SUCCESS;
    int file_descriptor = -1;
    struct krffs_file_system file_system = { .node = NULL };

    bool has_help_option = false;
    for (int i = 1; i < argc; ++i) {
        if (strncmp(argv[i], "-h", 2) == 0) {
            has_help_option = true;
            break;
        }
    }

    if (argc <= 1 || has_help_option) {
        fprintf(stderr, "Usage: %s <file>\n", argc >= 1 ? argv[0] : "fsck.krffs");
        goto cleanup;
    } else if (argv[1][0] == '-') {
        fprintf(stderr, "The first parameter is invalid.\n\nUsage: %s <file>\n",
                argc >= 1 ? argv[0] : "fsck.krffs");
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

    if (file_info.st_size < sizeof(*file_system.node) * 2) {
        fprintf(stderr, "The file at '%s' is not big enough to contain a file system.\n", path);
        exit_status = EXIT_FAILURE;
        goto cleanup;
    }

    file_system.size = file_info.st_size;

    if ((file_system.node = krffs_map_file(file_descriptor, 0, file_system.size)) == (void *) -1) {
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
        fprintf(stderr, "The file system was not found at '%s'. "
                "The file system was not created or the file is corrupted.\n", path);
        exit_status = EXIT_FAILURE;
        goto cleanup;
    }

    puts("The filesystem signature is correct.");

    if (file_system.node->type != KRFFS_Root_Node) {
        fprintf(stderr, "The root file system node was not found at '%s'. "
                "The file system was not created or the file is corrupted.\n", path);
        exit_status = EXIT_FAILURE;
        goto cleanup;
    }

    puts("The filesystem has a correct root node.");

    struct krffs_node *previous_node = file_system.node;
    struct krffs_node *node = previous_node;

    puts("Checking the filesystem structure...");

    do {
        if (previous_node > node) {
            printf("- invalid link to %"PRIuPTR" where base is %"PRIuPTR" and limit %"PRIuPTR"\n\n",
                   (uintptr_t) node, (uintptr_t) file_system.node,
                   (uintptr_t) file_system.node + (uintptr_t) file_system.size);
            fprintf(stderr, "The previous node link makes an invalid loop. "
                    "Should point to the first node. Won't proceed.\n");
            exit_status = KRFFS_Invalid_Link_Error;
            break;
        }

        if (!krffs_is_node_in_file_system(&file_system, node)) {
            printf("- out of range node at %"PRIuPTR" where base is %"PRIuPTR" and limit %"PRIuPTR"\n\n",
                   (uintptr_t) node, (uintptr_t) file_system.node,
                   (uintptr_t) file_system.node + (uintptr_t) file_system.size);
            fprintf(stderr, "Found an out of range node. Won't proceed.\n");
            exit_status = KRFFS_Out_of_Range_Node_Error;
            break;
        }

        if (node->magic != KRFFS_File_System_Magic) {
            printf("- invalid file system signature at %"PRIuPTR" in a node of type '%s'\n\n",
                   (uintptr_t) node,
                   node->type == KRFFS_Free_Node ? "free" :
                       (node->type == KRFFS_Reserved_Node ? "reserved" : "unknown"));
            fprintf(stderr, "Found an invalid file system signature. Won't proceed.\n");
            exit_status = KRFFS_Invalid_Magic_Signature_Error;
            break;
        }

        if (node->type == KRFFS_Root_Node) {
            printf("* root node at position %"PRIu64"\n",
                   krffs_get_node_relative_position(&file_system, node));
        } else if (node->type == KRFFS_Reserved_Node) {
            printf("+ reserved node with ID %"PRIu64" at position %"PRIu64": "
                   "%"PRIu64" bytes (%"PRIu64" bytes for data) - [%s]\n",
                   node->id, krffs_get_node_relative_position(&file_system, node),
                   node->size, node->data_size, node->name);
        } else if (node->type == KRFFS_Free_Node) {
            printf("- free node at position %"PRIu64": %"PRIu64" bytes\n",
                   krffs_get_node_relative_position(&file_system, node), node->size);
        } else {
            printf("- unknown node at position %"PRIu64"\n\n",
                   krffs_get_node_relative_position(&file_system, node));
            fprintf(stderr, "Found a node of an unknown type. Won't proceed.\n");
            exit_status = KRFFS_Unknown_Node_Type_Error;
            break;
        }

        previous_node = node;
        node = krffs_get_next_node(&file_system, node);
    } while (node != file_system.node);

    puts("The filesystem check has been completed successfully. There are no errors found.");

cleanup:
    if (file_descriptor != -1) {
        if (PLATFORM_PREFIX(close(file_descriptor)) == -1) {
            fprintf(stderr, "Failed to close the file system file.\n");
        }
    }

    if (file_system.node != NULL && file_system.node != (void *) -1) {
        if (krffs_unmap_file(file_system.node, file_system.size) == -1) {
            fprintf(stderr, "Failed to unmap the file system file.\n");
        }
    }

    return exit_status;
}
