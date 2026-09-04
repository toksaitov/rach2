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
                 KRFFS_Unknown_Node_Type_Error       = -13,
                 KRFFS_Node_Size_Error               = -14,
                 KRFFS_Repairs_Needed_Error          = -15;

/**
 * One named node, gathered by the walk so the check can find two of them.
 *
 * A file is found by its parent and its name, so a name repeated in one
 * directory is the fault; `hash` is only what sorts the entries, and the names
 * themselves decide.
 */
struct krffs_name_entry {
    uint64_t parent_id;
    uint64_t hash;
    uint64_t position;
    uint64_t size;
    bool     drop;
};

/** The node `position` bytes into the mapped file system. */
static struct krffs_node *krffs_node_at(
                              struct krffs_file_system *file_system,
                              uint64_t position
                          )
{
    return (struct krffs_node *) ((char *) file_system->node + position);
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
 * Checks the consistency of a KRFFS file system stored in a file.
 *
 * Usage: `fsck.krffs <file> [--offset N --size N] [--strict]`, or
 * `fsck.krffs -h` for help. The check maps the file, validates the file system
 * magic and root node, then walks the node list once, verifying that links are
 * consecutive, stay within the file system space, carry a valid signature, name
 * a known node type, and that the last node links back to the first.
 *
 * The walk asks of every node what the kernel asks of it before it mounts an
 * image: each node's `previous_node_size` is the size of the node before it,
 * the root's is the size of the last, and the sizes together span the region
 * exactly. Without those the checker accepted images the kernel refuses, which
 * is no use as an independent judge of what the kernel writes.
 *
 * Two kinds of fault. A fatal one, an unreadable signature, an unknown type, a
 * size that leaves the region, a data size larger than the node holding it, or
 * a chain that does not close, is reported and the image is left as it was.
 * A repairable one,
 * the dirty flag of an image that was not cleanly unmounted, an orphan node no
 * path reaches, a name repeated in one directory by a move that was
 * interrupted, and a backward link that disagrees with the forward chain, is
 * repaired after the walk has passed, and each repair is named. Nothing is
 * repaired before the walk: writing to an image whose structure has not been
 * read through is how a checker damages what it was asked to inspect.
 *
 * `--strict` reports the repairable faults and changes nothing, which is what
 * a test asks when it wants to know that the kernel left an image clean rather
 * than repairable.
 *
 * Returns `EXIT_SUCCESS` on a clean file system and `EXIT_FAILURE` for a usage
 * or I/O error. A structural fault yields a specific negative status: `-10` for
 * a nonconsecutive link, `-11` for a link outside the file system space, `-12`
 * for an invalid signature, `-13` for an unknown node type, `-14` for a node
 * whose data does not fit in it, and `-15` for repairs owed under `--strict`.
 * Per-node debug output goes to stdout and can be silenced with `> /dev/null`.
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
    bool is_strict        = false;
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

    /* Read before written: the dirty flag is reported here and cleared after
       the walk, so an image whose chain is broken keeps the evidence of both
       faults. */
    bool was_dirty = (file_system.node->mode & KRFFS_MODE_DIRTY) != 0;

    if (was_dirty) {
        puts("The filesystem was not cleanly unmounted.");
    }

    uint64_t orphans = 0;
    uint64_t backlinks = 0;  /* backward links the forward chain contradicts */
    uint64_t duplicates = 0; /* names repeated in one directory */

    struct krffs_name_entry *entries = NULL;
    size_t entry_count = 0;
    size_t entry_capacity = 0;

    struct krffs_node *previous_node =
        file_system.node;
    struct krffs_node *node =
        previous_node;

    /* A node whose size is zero (or too small) does not advance the walk, so
     * cap the iterations at the most nodes the mapping could hold; a chain that
     * does not return to the root within that bound is corrupt, not endless. */
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

        /* A node's data lives between its header and its end, so a data size
           that does not fit is a node whose bytes are somebody else's. Fatal:
           nothing in the image says what the size should have been. */
        if (node->data_size > node->size - sizeof(struct krffs_node)) {
            printf(
                "- node at position %"PRIu64": %"PRIu64" bytes of data in "
                "%"PRIu64" bytes of node\n",
                krffs_get_node_relative_position(&file_system, node),
                node->data_size,
                node->size
            );
            fprintf(
                stderr,
                "Found a node whose data does not fit in it. Won't proceed.\n"
            );
            exit_status = KRFFS_Node_Size_Error;
            break;
        }

        /* What the kernel checks before it mounts: the backward link is the
           size of the node before, and the sizes span the region. The forward
           chain is the truth; a backward link that disagrees is repairable
           because it can be derived from the walk that just passed it. */
        if (steps > 1 && node->previous_node_size != previous_node->size) {
            printf(
                "! backward link at position %"PRIu64": %"PRIu64" bytes, "
                "the node before is %"PRIu64"\n",
                krffs_get_node_relative_position(&file_system, node),
                node->previous_node_size,
                previous_node->size
            );
            backlinks++;
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
            if (entry_count == entry_capacity) {
                size_t wanted = entry_capacity ? entry_capacity * 2 : 64;
                struct krffs_name_entry *grown =
                    realloc(entries, wanted * sizeof(*entries));

                if (!grown) {
                    fprintf(stderr, "Out of memory reading the names.\n");
                    exit_status = EXIT_FAILURE;
                    break;
                }
                entries = grown;
                entry_capacity = wanted;
            }
            entries[entry_count].parent_id = node->parent_id;
            entries[entry_count].hash =
                krffs_calculate_djb_hash(node->name);
            entries[entry_count].position =
                krffs_get_node_relative_position(&file_system, node);
            entries[entry_count].size = node->size;
            entries[entry_count].drop = false;
            entry_count++;

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

    /* The chain closed at the root, so `previous_node` is the last node and its
       size is what the root's backward link must say. That the sizes span the
       region is not checked separately: the walk steps by each node's size and
       ends only where the chain returns to the root, and every size is bounded
       against the region above, so a walk that closes has spanned it. */
    if (exit_status == EXIT_SUCCESS) {
        if (file_system.node->previous_node_size != previous_node->size) {
            printf(
                "! backward link at the root: %"PRIu64" bytes, "
                "the last node is %"PRIu64"\n",
                file_system.node->previous_node_size,
                previous_node->size
            );
            backlinks++;
        }
    }

    /* A name repeated in one directory is what an interrupted move leaves. The
       node the move is making is published under the name before the node it
       replaces is freed, so a machine that stops between the two keeps both.
       A move only ever happens to grow a file into a larger node, so the
       larger of the two is the one the move was making: the smaller loses its
       name and the orphan pass below reclaims it. */
    if (exit_status == EXIT_SUCCESS && entry_count > 1) {
        size_t i;

        qsort(entries, entry_count, sizeof(*entries),
              krffs_compare_name_entries);

        for (i = 0; i + 1 < entry_count; i++) {
            size_t j;

            for (j = i + 1;
                 !entries[i].drop && j < entry_count &&
                 entries[j].parent_id == entries[i].parent_id &&
                 entries[j].hash == entries[i].hash;
                 j++) {
                struct krffs_node *first = krffs_node_at(&file_system, entries[i].position);
                struct krffs_node *second = krffs_node_at(&file_system, entries[j].position);
                size_t keep;
                size_t drop;

                if (entries[j].drop ||
                    strncmp((const char *) first->name,
                            (const char *) second->name,
                            KRFFS_FILE_NAME_BUFFER_SIZE) != 0) {
                    continue;
                }

                keep = entries[i].size >= entries[j].size ? i : j;
                drop = keep == i ? j : i;
                entries[drop].drop = true;
                duplicates++;

                printf(
                    "! name [%s] twice in one directory: "
                    "position %"PRIu64" (%"PRIu64" bytes) kept, "
                    "position %"PRIu64" (%"PRIu64" bytes) dropped\n",
                    first->name,
                    entries[keep].position,
                    entries[keep].size,
                    entries[drop].position,
                    entries[drop].size
                );
            }
        }
    }

    /* Under `--strict` a repairable fault is the answer, and the image is left
       as it was found. */
    if (exit_status == EXIT_SUCCESS && is_strict &&
        (was_dirty || orphans != 0 || duplicates != 0 || backlinks != 0)) {
        fprintf(
            stderr,
            "The file system needs repair: %s%"PRIu64" orphan node(s), "
            "%"PRIu64" repeated name(s), %"PRIu64" backward link(s).\n",
            was_dirty ? "not cleanly unmounted, " : "",
            orphans,
            duplicates,
            backlinks
        );
        exit_status = KRFFS_Repairs_Needed_Error;
    }

    /* The loser of a repeated name gives up its name, which makes it the same
       thing as an orphan and leaves the pass below one kind of node to
       reclaim. */
    if (exit_status == EXIT_SUCCESS && duplicates != 0) {
        size_t i;

        for (i = 0; i < entry_count; i++) {
            if (entries[i].drop) {
                krffs_node_at(&file_system, entries[i].position)->name[0] = '\0';
            }
        }
        printf("Dropped %"PRIu64" repeated name(s) an interrupted move left.\n",
               duplicates);
    }

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

    /* The backward links, rebuilt from the forward chain the walk validated.
       The orphan pass above merges nodes, so this runs after it and reads the
       chain as it now stands. */
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
        printf("Rebuilt %"PRIu64" backward link(s) from the forward chain.\n",
               fixed);
    }

    if (exit_status == EXIT_SUCCESS && was_dirty) {
        puts("Clearing the dirty flag.");
        file_system.node->mode &= (uint16_t) ~KRFFS_MODE_DIRTY;
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
