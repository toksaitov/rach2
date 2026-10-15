/*
 * fsck.krffs.c: fsck.krffs, which checks a KRFFS image and repairs the faults
 * that can be repaired.
 */

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

/** The exit statuses of the faults the walk of `main` finds. */
static const int KRFFS_Invalid_Link_Error = -10, KRFFS_Out_of_Range_Node_Error = -11,
                 KRFFS_Invalid_Magic_Signature_Error = -12, KRFFS_Unknown_Node_Type_Error = -13,
                 KRFFS_Node_Size_Error = -14, KRFFS_Repairs_Needed_Error = -15;

/** The type field of a node's mode and a directory's value in it. */
#define FSCK_S_IFMT  0170000u
#define FSCK_S_IFDIR 0040000u

/**
 * A reserved node with a name, gathered by the walk to find a name repeated
 * in one directory. A file is found by its parent and its name, so two nodes
 * with one `parent_id` and one name are the fault.
 */
struct krffs_name_entry {
    uint64_t parent_id;
    /** The djb2 hash of the name, which sorts the entries; the names decide. */
    uint64_t hash;
    /** The node's byte offset in the region. */
    uint64_t position;
    uint64_t size;
    bool is_dir;
    /** Set when the node loses the repeated name. */
    bool drop;
};

/** Returns the node `position` bytes into the region of `file_system`. */
static struct krffs_node *krffs_node_at(struct krffs_file_system *file_system, uint64_t position)
{
    return (struct krffs_node *)((char *)file_system->node + position);
}

/**
 * Orders entries by directory, then by name hash, then by position, so every
 * repeated name stands in one run.
 */
static int krffs_compare_name_entries(const void *left, const void *right)
{
    const struct krffs_name_entry *a = left;
    const struct krffs_name_entry *b = right;

    if (a->parent_id != b->parent_id) {
        return a->parent_id < b->parent_id ? -1 : 1;
    }
    if (a->hash != b->hash) {
        return a->hash < b->hash ? -1 : 1;
    }
    if (a->position != b->position) {
        return a->position < b->position ? -1 : 1;
    }
    return 0;
}

/**
 * fsck.krffs IMAGE [--strict] [--offset BYTES --size BYTES]: checks a KRFFS
 * image as the kernel checks a volume before it mounts it, and repairs the
 * faults that can be repaired. It shares no code with the kernel, and the
 * suites use it to judge the images the kernel writes. A line for each node
 * goes to stdout and each fault to stderr.
 *
 * The root must have the magic number and the root type. One walk then
 * follows the ring from the root by each node's `size`, for at most one step
 * more than the headers the region could hold, and requires of each node the
 * magic number, a known type, and a `size` of at least a header that ends
 * inside the region; of a node that is not free, a `data_size` that fits,
 * since a free node keeps the length of what was removed there; and of a
 * reserved node, a NUL in its name. A walk that returns to the root has so
 * spanned the region. A fault of these is fatal: it is reported and the
 * image is left as it was.
 *
 * Four faults can be repaired: the dirty flag of an image that was not
 * cleanly unmounted; an orphan, a reserved node with no name, which a crash
 * leaves when the last descriptor of an unlinked file was never closed; a
 * name repeated in one directory, which an interrupted move leaves, since a
 * move gives the name to a larger node before it frees the smaller; and a
 * `previous_node_size` that is not the size of the node before. They are
 * repaired only once the walk has passed. The backward links are rebuilt
 * from the forward chain first, since freeing a node merges it with the node
 * before by following that link. Of two nodes with one name, the directory
 * keeps it, since its children name it by its `id`, or else the larger; the
 * other loses its name. Every node with no name is then freed with
 * `krffs_remove_reserved_node`, and the dirty flag is cleared. Under
 * `--strict` a repairable fault is reported and nothing is changed.
 *
 * Returns `EXIT_SUCCESS` for an image that passed, and for no arguments or an
 * argument that begins with `-h`; `EXIT_FAILURE` for a bad argument, an image
 * that cannot be opened or mapped, a root with the wrong magic number or
 * type, or no memory; and, which the shell sees as 246 to 241, -10 for a
 * chain that does not return to the root, -11 for a node outside the region,
 * -12 for a wrong magic number, -13 for an unknown type, -14 for a size, data
 * size or name that does not fit, and -15 for a repair owed under
 * `--strict`. A failure to close or unmap is printed and does not change the
 * status.
 */
int main(int argc, char **argv)
{
    int exit_status = EXIT_SUCCESS;

    int file_descriptor = -1;
    struct krffs_name_entry *entries = NULL;
    struct krffs_file_system file_system = {.node = NULL};

    bool has_help_option = false;
    bool is_strict = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--strict") == 0) {
            is_strict = true;
        }
    }
    for (int i = 1; i < argc; ++i) {
        if (strncmp(argv[i], "-h", 2) == 0) {
            has_help_option = true;
            break;
        }
    }

    if (argc <= 1 || has_help_option) {
        fprintf(stderr, "Usage: %s <file> [--offset N --size N] [--strict]\n", argv[0]);

        goto cleanup;
    } else if (argv[1][0] == '-') {
        fprintf(stderr,
                "The first parameter is invalid.\n"
                "\n"
                "Usage: %s <file> [--offset N --size N] [--strict]\n",
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

    if (file_system.node->magic != KRFFS_File_System_Magic) {
        fprintf(stderr,
                "The file system was not found at '%s'. "
                "The file system was not created or the file is corrupted.\n",
                path);

        exit_status = EXIT_FAILURE;

        goto cleanup;
    }

    puts("The filesystem signature is correct.");

    if (file_system.node->type != KRFFS_Root_Node) {
        fprintf(stderr,
                "The root file system node was not found at '%s'. "
                "The file system was not created or the file is corrupted.\n",
                path);

        exit_status = EXIT_FAILURE;

        goto cleanup;
    }

    puts("The filesystem has a correct root node.");

    bool was_dirty = (file_system.node->mode & KRFFS_MODE_DIRTY) != 0;

    if (was_dirty) {
        puts("The filesystem was not cleanly unmounted.");
    }

    uint64_t orphans = 0;
    uint64_t backlinks = 0;
    uint64_t duplicates = 0;

    size_t entry_count = 0;
    size_t entry_capacity = 0;

    struct krffs_node *previous_node = file_system.node;
    struct krffs_node *node = previous_node;

    uint64_t steps = 0;
    uint64_t max_steps = file_system.size / sizeof(struct krffs_node) + 1;

    puts("Checking the filesystem structure...");

    do {
        if (++steps > max_steps) {
            fprintf(stderr, "The node chain does not return to the root within the file "
                            "system bound. The file is corrupt. Won't proceed.\n");

            exit_status = KRFFS_Invalid_Link_Error;

            break;
        }

        if (previous_node > node) {
            printf("- invalid link to %" PRIu64 " "
                   "where base is %" PRIu64 " "
                   "and limit %" PRIu64 "\n"
                   "\n",
                   (uint64_t)node, (uint64_t)file_system.node,
                   (uint64_t)file_system.node + file_system.size);

            fprintf(stderr, "The previous node link makes an invalid loop. "
                            "Should point to the first node. "
                            "Won't proceed.\n");

            exit_status = KRFFS_Invalid_Link_Error;

            break;
        }

        if (!krffs_is_node_in_file_system(&file_system, node)) {
            printf("- out of range node at %" PRIu64 " "
                   "where base is %" PRIu64 " "
                   "and limit %" PRIu64 "\n"
                   "\n",
                   (uint64_t)node, (uint64_t)file_system.node,
                   (uint64_t)file_system.node + file_system.size);

            fprintf(stderr, "Found an out of range node. Won't proceed.\n");

            exit_status = KRFFS_Out_of_Range_Node_Error;

            break;
        }

        if (node->magic != KRFFS_File_System_Magic) {
            printf("- invalid file system signature at %" PRIu64 " "
                   "in a node of type '%s'\n"
                   "\n",
                   (uint64_t)node,
                   node->type == KRFFS_Free_Node
                       ? "free"
                       : (node->type == KRFFS_Reserved_Node ? "reserved" : "unknown"));

            fprintf(stderr, "Found an invalid file system signature. Won't proceed.\n");

            exit_status = KRFFS_Invalid_Magic_Signature_Error;

            break;
        }

        if (node->size < sizeof(struct krffs_node) ||
            node->size > file_system.size - krffs_get_node_relative_position(&file_system, node)) {
            printf("- node at position %" PRIu64 ": a size of %" PRIu64 " bytes in "
                   "a region of %" PRIu64 "\n",
                   krffs_get_node_relative_position(&file_system, node), node->size,
                   (uint64_t)file_system.size);
            fprintf(stderr, "Found a node whose size is not one this region can hold. "
                            "Won't proceed.\n");
            exit_status = KRFFS_Node_Size_Error;
            break;
        }

        if (node->type != KRFFS_Free_Node &&
            node->data_size > node->size - sizeof(struct krffs_node)) {
            printf("- node at position %" PRIu64 ": %" PRIu64 " bytes of data in "
                   "%" PRIu64 " bytes of node\n",
                   krffs_get_node_relative_position(&file_system, node), node->data_size,
                   node->size);
            fprintf(stderr, "Found a node whose data does not fit in it. Won't proceed.\n");
            exit_status = KRFFS_Node_Size_Error;
            break;
        }

        if (node->type == KRFFS_Reserved_Node) {
            uint32_t i;

            for (i = 0; i < KRFFS_FILE_NAME_BUFFER_SIZE; i++) {
                if (node->name[i] == '\0') {
                    break;
                }
            }
            if (i == KRFFS_FILE_NAME_BUFFER_SIZE) {
                printf("- node at position %" PRIu64 " has a name with no end\n"
                       "\n",
                       krffs_get_node_relative_position(&file_system, node));
                fprintf(stderr, "Found a node whose name is not terminated. "
                                "Won't proceed.\n");
                exit_status = KRFFS_Node_Size_Error;
                break;
            }
        }

        if (steps > 1 && node->previous_node_size != previous_node->size) {
            printf("! backward link at position %" PRIu64 ": %" PRIu64 " bytes, "
                   "the node before is %" PRIu64 "\n",
                   krffs_get_node_relative_position(&file_system, node), node->previous_node_size,
                   previous_node->size);
            backlinks++;
        }

        if (node->type == KRFFS_Root_Node) {
            printf("* root node at position %" PRIu64 " (owner %u:%u)\n",
                   krffs_get_node_relative_position(&file_system, node), (unsigned)node->uid,
                   (unsigned)node->gid);
        } else if (node->type == KRFFS_Reserved_Node && node->name[0] == '\0') {
            orphans++;
            printf("+ orphan node at position %" PRIu64 ": %" PRIu64 " bytes "
                   "(%" PRIu64 " bytes for data, owner %u:%u), to be reclaimed\n",
                   krffs_get_node_relative_position(&file_system, node), node->size,
                   node->data_size, (unsigned)node->uid, (unsigned)node->gid);
        } else if (node->type == KRFFS_Reserved_Node) {
            if (entry_count == entry_capacity) {
                size_t wanted = entry_capacity ? entry_capacity * 2 : 64;
                struct krffs_name_entry *grown = realloc(entries, wanted * sizeof(*entries));

                if (!grown) {
                    fprintf(stderr, "Out of memory reading the names.\n");
                    exit_status = EXIT_FAILURE;
                    break;
                }
                entries = grown;
                entry_capacity = wanted;
            }
            entries[entry_count].parent_id = node->parent_id;
            entries[entry_count].hash = krffs_calculate_djb_hash(node->name);
            entries[entry_count].position = krffs_get_node_relative_position(&file_system, node);
            entries[entry_count].size = node->size;
            entries[entry_count].is_dir = (node->mode & FSCK_S_IFMT) == FSCK_S_IFDIR;
            entries[entry_count].drop = false;
            entry_count++;

            printf("+ reserved node with ID %" PRIu64 " at position %" PRIu64 ": "
                   "%" PRIu64 " bytes (%" PRIu64 " bytes for data, owner %u:%u) - [%.255s]\n",
                   node->id, krffs_get_node_relative_position(&file_system, node), node->size,
                   node->data_size, (unsigned)node->uid, (unsigned)node->gid, node->name);
        } else if (node->type == KRFFS_Free_Node) {
            printf("- free node at position %" PRIu64 ": %" PRIu64 " bytes\n",
                   krffs_get_node_relative_position(&file_system, node), node->size);
        } else {
            printf("- unknown node at position %" PRIu64 "\n"
                   "\n",
                   krffs_get_node_relative_position(&file_system, node));

            fprintf(stderr, "Found a node of an unknown type. Won't proceed.\n");

            exit_status = KRFFS_Unknown_Node_Type_Error;

            break;
        }

        previous_node = node;
        node = krffs_get_next_node(&file_system, node);
    } while (node != file_system.node);

    if (exit_status == EXIT_SUCCESS) {
        if (file_system.node->previous_node_size != previous_node->size) {
            printf("! backward link at the root: %" PRIu64 " bytes, "
                   "the last node is %" PRIu64 "\n",
                   file_system.node->previous_node_size, previous_node->size);
            backlinks++;
        }
    }

    if (exit_status == EXIT_SUCCESS && entry_count > 1) {
        size_t i;

        qsort(entries, entry_count, sizeof(*entries), krffs_compare_name_entries);

        for (i = 0; i + 1 < entry_count; i++) {
            size_t j;

            for (j = i + 1;
                 !entries[i].drop && j < entry_count &&
                 entries[j].parent_id == entries[i].parent_id && entries[j].hash == entries[i].hash;
                 j++) {
                struct krffs_node *first = krffs_node_at(&file_system, entries[i].position);
                struct krffs_node *second = krffs_node_at(&file_system, entries[j].position);
                size_t keep;
                size_t drop;

                if (entries[j].drop ||
                    strncmp((const char *)first->name, (const char *)second->name,
                            KRFFS_FILE_NAME_BUFFER_SIZE) != 0) {
                    continue;
                }

                if (entries[i].is_dir != entries[j].is_dir) {
                    keep = entries[i].is_dir ? i : j;
                } else {
                    keep = entries[i].size >= entries[j].size ? i : j;
                }
                drop = keep == i ? j : i;
                entries[drop].drop = true;
                duplicates++;

                printf("! name [%.255s] twice in one directory: "
                       "position %" PRIu64 " (%" PRIu64 " bytes) kept, "
                       "position %" PRIu64 " (%" PRIu64 " bytes) dropped\n",
                       first->name, entries[keep].position, entries[keep].size,
                       entries[drop].position, entries[drop].size);
            }
        }
    }

    if (exit_status == EXIT_SUCCESS && is_strict &&
        (was_dirty || orphans != 0 || duplicates != 0 || backlinks != 0)) {
        fprintf(stderr,
                "The file system needs repair: %s%" PRIu64 " orphan node(s), "
                "%" PRIu64 " repeated name(s), %" PRIu64 " backward link(s).\n",
                was_dirty ? "not cleanly unmounted, " : "", orphans, duplicates, backlinks);
        exit_status = KRFFS_Repairs_Needed_Error;
    }

    if (exit_status == EXIT_SUCCESS && backlinks != 0) {
        struct krffs_node *scan = file_system.node;
        struct krffs_node *behind = NULL;
        uint64_t walked = 0;
        uint64_t fixed = 0;

        do {
            if (behind != NULL && scan->previous_node_size != behind->size) {
                scan->previous_node_size = behind->size;
                fixed++;
            }
            behind = scan;
            scan = krffs_get_next_node(&file_system, scan);
        } while (scan != file_system.node && ++walked <= max_steps);

        if (behind != NULL && file_system.node->previous_node_size != behind->size) {
            file_system.node->previous_node_size = behind->size;
            fixed++;
        }
        printf("Rebuilt %" PRIu64 " backward link(s) from the forward chain.\n", fixed);
    }

    if (exit_status == EXIT_SUCCESS && duplicates != 0) {
        size_t i;

        for (i = 0; i < entry_count; i++) {
            if (entries[i].drop) {
                krffs_node_at(&file_system, entries[i].position)->name[0] = '\0';
            }
        }
        printf("Dropped %" PRIu64 " repeated name(s) an interrupted move left.\n", duplicates);
    }

    if (exit_status == EXIT_SUCCESS) {
        uint64_t reclaimed = 0;
        uint64_t walked = 0;
        struct krffs_node *scan = krffs_get_next_node(&file_system, file_system.node);
        struct krffs_node *prev = file_system.node;

        while (scan != file_system.node && walked <= max_steps) {
            walked++;
            if (scan->type == KRFFS_Reserved_Node && scan->name[0] == '\0') {
                struct krffs_node *merged = (prev->type == KRFFS_Free_Node) ? prev : scan;

                krffs_remove_reserved_node(&file_system, scan);
                reclaimed++;
                scan = merged;
            }
            prev = scan;
            scan = krffs_get_next_node(&file_system, scan);
        }
        if (reclaimed != 0) {
            printf("Reclaimed %" PRIu64 " orphan node(s) no path could reach.\n", reclaimed);
        }
    }

    if (exit_status == EXIT_SUCCESS && was_dirty) {
        puts("Clearing the dirty flag.");
        file_system.node->mode &= (uint16_t)~KRFFS_MODE_DIRTY;
    }

    if (exit_status == EXIT_SUCCESS) {
        puts("The filesystem check has been completed "
             "successfully. There are no errors found.");
    } else {
        fprintf(stderr, "The file system check found errors.\n");
    }

cleanup:
    free(entries);

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
