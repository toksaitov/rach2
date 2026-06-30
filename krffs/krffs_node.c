#include "krffs_node.h"

#include "krffs_file_system.h"
#include "krffs_platform.h"
#include "krffs_utilities.h"

#include <assert.h>
#include <string.h>

/**
 * Reads the wall clock as a `krffs_node_time`.
 *
 * The seconds and nanoseconds are taken from `gettimeofday`, whose
 * microseconds are scaled to nanoseconds.
 */
struct krffs_node_time krffs_get_current_node_time()
{
    struct timeval time_of_day;
    krffs_get_time_of_day(&time_of_day);

    struct krffs_node_time node_time;
    node_time.seconds =
        time_of_day.tv_sec;
    node_time.nanoseconds =
        time_of_day.tv_usec * 1000;

    return node_time;
}

time_t krffs_get_time_from_node_time(
           const struct krffs_node_time *node_time
       )
{
    assert(node_time);

    return (time_t) node_time->seconds;
}

struct timespec krffs_get_timespec_from_node_time(
           const struct krffs_node_time *node_time
       )
{
    assert(node_time);

    struct timespec time;
    time.tv_sec =
        (long) node_time->seconds;
    time.tv_nsec =
        (long) node_time->nanoseconds;

    return time;
}

/**
 * Stamps `node` as a free node of the given `size` and `previous_node_size`,
 * then flushes the node header to the backing store.
 *
 * The KRFFS magic and the `KRFFS_Free_Node` type are written so the block can
 * be walked as part of the on-disk ring; `data_size` is cleared because a free
 * node holds no payload. Returns `node`.
 */
struct krffs_node *krffs_initialize_free_node(
                       struct krffs_node *node,
                       uint64_t size,
                       uint64_t previous_node_size
                   )
{
    assert(node);

    node->magic =
        KRFFS_File_System_Magic;
    node->type =
        KRFFS_Free_Node;
    node->size =
        size;
    node->previous_node_size =
        previous_node_size;
    node->data_size =
        0;

    krffs_sync_mapping(
        node,
        sizeof(*node)
    );

    return node;
}

/**
 * Folds a node pointer back into the file system's address range.
 *
 * The nodes form a ring over the mapping `[base, base + size)`. A pointer that
 * has run off the end is brought back by subtracting `size`, and one that has
 * run off the front by adding `size`, so node walks wrap instead of leaving the
 * mapping. Returns the wrapped pointer.
 */
struct krffs_node *krffs_wrap_node_position(
                       struct krffs_file_system *file_system,
                       struct krffs_node *node
                   )
{
    assert(file_system && node);

    uint64_t position =
        (uint64_t) node;
    uint64_t base =
        (uint64_t) file_system->node;
    uint64_t size =
        file_system->size;
    uint64_t limit =
        base + size;

    if (position >= limit) {
        position -=
            size;
    } else if (position < base) {
        position +=
            size;
    }

    return (struct krffs_node *) position;
}

/**
 * Stamps `node` as the root node of the file system.
 *
 * The root carries the reserved root name and the djb2 hash of that name as its
 * `id`, the root mode, and the owning uid and gid (zero on Windows, the
 * effective ids of the host process otherwise). `parent_id` is stamped to the
 * reserved root id (0): the kernel resolves ".." from the root by reading this
 * field (krffs.c), so it must be zero rather than left to whatever the backing
 * held, which a forced re-mkfs over a non-zeroed file would otherwise leave
 * stale. All three timestamps are set to the current time, `data_size` is
 * cleared, and `size` and `previous_node_size` place the node in the ring.
 * Unlike the other initializers this one does not flush; the caller syncs the
 * mapping. Returns `node`.
 */
struct krffs_node *krffs_initialize_root_node(
                       struct krffs_node *node,
                       uint64_t size,
                       uint64_t previous_node_size
                   )
{
    assert(node);

    node->magic =
        KRFFS_File_System_Magic;
    node->type =
        KRFFS_Root_Node;
    node->id =
        krffs_calculate_djb_hash(
            (uint8_t *) KRFFS_File_System_Root_Node_Name
        );

    strncpy(
        (char *) node->name,
        KRFFS_File_System_Root_Node_Name,
        KRFFS_FILE_NAME_BUFFER_SIZE
    );
    node->name[KRFFS_FILE_NAME_BUFFER_SIZE - 1] = '\0';

    node->mode =
        KRFFS_File_System_Root_Node_Mode;

#ifdef WINDOWS
    node->uid = 0;
    node->gid = 0;
#else
    node->uid =
        geteuid();
    node->gid =
        getegid();
#endif

    struct krffs_node_time current_node_time =
        krffs_get_current_node_time();

    node->access_time =
        current_node_time;
    node->modification_time =
        current_node_time;
    node->attribute_change_time =
        current_node_time;

    node->size =
        size;
    node->previous_node_size =
        previous_node_size;
    node->data_size =
        0;
    node->parent_id =
        0; /* reserved root id; the kernel reads it to resolve ".." from root */

    return node;
}

/**
 * Copies `name` into `node->name` and refreshes the `id` hash from it.
 *
 * The name is truncated to the name buffer and forced NUL-terminated, then
 * `id` is set to the djb2 hash of the stored name so name lookups match. The
 * node header is flushed to the backing store.
 */
void krffs_set_reserved_node_name(
         struct krffs_node *node,
         const char *name
     )
{
    assert(node && name);

    strncpy(
        (char *) node->name,
        name,
        KRFFS_FILE_NAME_BUFFER_SIZE
    );
    node->name[KRFFS_FILE_NAME_BUFFER_SIZE - 1] = '\0';

    node->id =
        krffs_calculate_djb_hash(
            node->name
        );

    krffs_sync_mapping(
        node,
        sizeof(*node)
    );
}

/**
 * Returns the node preceding `node` in the ring.
 *
 * The previous node is found by stepping back `previous_node_size` bytes and
 * wrapping into the file system range. The node signatures are not checked.
 */
struct krffs_node *krffs_get_previous_node(
                       struct krffs_file_system *file_system,
                       struct krffs_node *node
                   )
{
    assert(file_system && node);

    struct krffs_node *previous_node =
        krffs_wrap_node_position(
            file_system,
            (struct krffs_node *) (
                ((char *) node) -
                    node->previous_node_size
            )
        );

    return previous_node;
}

/**
 * Returns the node following `node` in the ring.
 *
 * The next node is found by stepping forward `size` bytes and wrapping into the
 * file system range. The node signatures are not checked.
 */
struct krffs_node *krffs_get_next_node(
                       struct krffs_file_system *file_system,
                       struct krffs_node *node
                   )
{
    assert(file_system && node);

    struct krffs_node *next_node =
        krffs_wrap_node_position(
            file_system,
            (struct krffs_node *) (
                ((char *) node) +
                    node->size
            )
        );

    return next_node;
}

/**
 * Returns the byte offset of `node` from the start of the file system mapping.
 */
uint64_t krffs_get_node_relative_position(
             struct krffs_file_system *file_system,
             struct krffs_node *node
         )
{
    assert(file_system && node);

    uint64_t position =
        (uint64_t) node;
    uint64_t base =
        (uint64_t) file_system->node;
    uint64_t relative_position =
        position - base;

    return relative_position;
}

/**
 * Reports whether the whole of `node`'s header lies within the file system's
 * mapping `[base, base + size)`.
 *
 * The header must fit in full, not merely start inside: a node positioned in the
 * last few bytes of a corrupt image would otherwise pass the check and then read
 * its `size`/`next` tail fields past the end of the mapping. The remaining-space
 * test is written as a subtraction so a position past `limit` cannot wrap it.
 */
bool krffs_is_node_in_file_system(
         struct krffs_file_system *file_system,
         struct krffs_node *node
     )
{
    assert(file_system && node);

    uint64_t position =
        (uint64_t) node;
    uint64_t base =
        (uint64_t) file_system->node;
    uint64_t size =
        file_system->size;
    uint64_t limit =
        base + size;

    bool is_inside =
        position >= base &&
            position < limit &&
            limit - position >= (uint64_t) sizeof(struct krffs_node);

    return is_inside;
}

/**
 * Finds the first reserved node whose name matches `name`.
 *
 * The walk hashes `name` with djb2 and compares it against each node's `id`,
 * matching only `KRFFS_Reserved_Node` blocks. Returns the matching node, or
 * `NULL` if the ring holds no such node.
 */
struct krffs_node *krffs_find_reserved_node_by_name(
                       struct krffs_file_system *file_system,
                       const char *name
                   )
{
    assert(file_system && name);

    struct krffs_node *result = NULL;

    uint64_t hash =
        krffs_calculate_djb_hash((uint8_t *) name);

    struct krffs_node *node = file_system->node;
    do {
        if (node->type == KRFFS_Reserved_Node &&
                node->id == hash) {
            result =
                node;

            break;
        }
    } while ((node =
                  krffs_get_next_node(
                      file_system,
                      node
                  )) != file_system->node);

    return result;
}

/**
 * Like `krffs_find_reserved_node_by_name`, but the root node is eligible too.
 *
 * The walk hashes `name` with djb2 and compares it against each node's `id`,
 * matching `KRFFS_Reserved_Node` and `KRFFS_Root_Node` blocks. Returns the
 * matching node, or `NULL` if the ring holds no such node.
 */
struct krffs_node *krffs_find_reserved_or_root_node_by_name(
                       struct krffs_file_system *file_system,
                       const char *name
                   )
{
    assert(file_system && name);

    struct krffs_node *result = NULL;

    uint64_t hash =
        krffs_calculate_djb_hash((uint8_t *) name);

    struct krffs_node *node = file_system->node;
    do {
        if ((node->type == KRFFS_Reserved_Node ||
             node->type == KRFFS_Root_Node) &&
                node->id == hash) {
            result =
                node;

            break;
        }
    } while ((node =
                  krffs_get_next_node(
                      file_system,
                      node
                  )) != file_system->node);

    return result;
}

/**
 * Returns the number of free nodes in the ring.
 */
size_t krffs_count_free_nodes(
           struct krffs_file_system *file_system
       )
{
    size_t result = 0;

    struct krffs_node *node = file_system->node;
    do {
        if (node->type == KRFFS_Free_Node) {
            ++result;
        }
    } while ((node =
                  krffs_get_next_node(
                      file_system,
                      node
                  )) != file_system->node);

    return result;
}

/**
 * Returns the total payload bytes available across all free nodes.
 *
 * Each free node contributes its `size` minus the node header, so the result
 * counts usable space, not the raw block sizes.
 */
uint64_t krffs_calculate_total_free_space(
             struct krffs_file_system *file_system
         )
{
    uint64_t result = 0;

    struct krffs_node *node = file_system->node;
    do {
        if (node->type == KRFFS_Free_Node) {
            result +=
                node->size -
                    sizeof(*node);
        }
    } while ((node =
                  krffs_get_next_node(
                      file_system,
                      node
                  )) != file_system->node);

    return result;
}

/**
 * Returns the number of reserved nodes in the ring.
 */
size_t krffs_count_reserved_nodes(
           struct krffs_file_system *file_system
       )
{
    size_t result = 0;

    struct krffs_node *node = file_system->node;
    do {
        if (node->type == KRFFS_Reserved_Node) {
            ++result;
        }
    } while ((node =
                  krffs_get_next_node(
                      file_system,
                      node
                  )) != file_system->node);

    return result;
}

/**
 * Returns the total payload bytes held by all reserved nodes.
 *
 * Each reserved node contributes its `size` minus the node header, so the
 * result counts used space, not the raw block sizes.
 */
uint64_t krffs_calculate_total_reserved_space(
             struct krffs_file_system *file_system
         )
{
    uint64_t result = 0;

    struct krffs_node *node = file_system->node;
    do {
        if (node->type == KRFFS_Reserved_Node) {
            result +=
                node->size -
                    sizeof(*node);
        }
    } while ((node =
                  krffs_get_next_node(
                      file_system,
                      node
                  )) != file_system->node);

    return result;
}
