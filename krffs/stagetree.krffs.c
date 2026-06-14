#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "krffs_allocator.h"
#include "krffs_file_system.h"
#include "krffs_node.h"
#include "krffs_platform.h"
#include "krffs_utilities.h"

#ifdef WINDOWS
#include <io.h>
#else
#include <unistd.h>
#endif

/*
    stagetree.krffs

    Stage a small SAMPLE directory tree (/etc, with motd and hostname) into an
    existing KRFFS image, so a booted image ships a populated directory rather
    than only a flat root. It writes KRFFS v2 nodes directly -- a directory node
    (parent_id 0, S_IFDIR) and its children (parent_id = the directory's id),
    exactly as the kernel path resolver expects -- proving the host tools speak
    the hierarchical format.

    It is idempotent: if /etc already exists it does nothing, and it preflights
    free space so a run either stages the whole tree or leaves the image
    untouched. This is a fixed demonstration stager, not a general host-to-image
    copy; staging an arbitrary host directory recursively is future (Part II) work.

    Usage:
        stagetree.krffs <image>
 */

/* The KRFFS v2 directory type bit (mode & S_IFMT == S_IFDIR marks a directory).
   The host stat.h S_IFDIR may differ, so define the on-disk value explicitly. */
#define KRFFS_MODE_IFDIR 0040000u

static const char STAGE_MOTD[] =
    "Welcome to Rach2.\n";
static const char STAGE_HOSTNAME[] =
    "rach2\n";

/* Match the kernel's krffs_mix_id (rach2/sys/fs/krffs.c) so a host-staged node
   and the same node created in-guest get the same id: a stable mix of the
   parent's id and the node's name. */
static uint64_t krffs_mix_id(uint64_t parent_id, const char *name)
{
    uint64_t h =
        (parent_id + 1) * 1099511628211ULL;

    h ^= krffs_calculate_djb_hash((uint8_t *) name);
    h *= 1099511628211ULL;

    return h ? h : 1;
}

/* Find a child named `name` directly inside directory `parent_id` (mirrors the
   kernel's krffs_find_child), or NULL. Used to keep staging idempotent. */
static struct krffs_node *find_child(
                              struct krffs_file_system *file_system,
                              uint64_t parent_id,
                              const char *name
                          )
{
    struct krffs_node *node =
        file_system->node;

    do {
        if (node->type == KRFFS_Reserved_Node &&
            node->parent_id == parent_id &&
            strcmp((const char *) node->name, name) == 0) {
            return node;
        }
    } while ((node = krffs_get_next_node(file_system, node)) != file_system->node);

    return NULL;
}

/* Largest free block, in bytes (header included). The nodes are first-fit from a
   single big free block on a freshly-made image, so this bounds what will fit. */
static uint64_t largest_free_block(struct krffs_file_system *file_system)
{
    struct krffs_node *node =
        file_system->node;
    uint64_t largest =
        0;

    do {
        if (node->type == KRFFS_Free_Node && node->size > largest) {
            largest =
                node->size;
        }
    } while ((node = krffs_get_next_node(file_system, node)) != file_system->node);

    return largest;
}

/* Reserve one node named `name` inside directory `parent_id`, with `mode` and an
   optional `data` payload. Returns the new node's id, or 0 on failure. */
static uint64_t stage_node(
                    struct krffs_file_system *file_system,
                    uint64_t parent_id,
                    const char *name,
                    uint16_t mode,
                    const char *data,
                    uint64_t data_size
                )
{
    struct krffs_node *node;
    struct krffs_node_time zero_time = {
        0
    };

    if (data_size > (uint64_t) SIZE_MAX) {
        fprintf(stderr, "stagetree: '%s' is too large to stage.\n", name);

        return 0;
    }

    node =
        krffs_allocate_reserved_node(
            file_system,
            data_size,
            NULL
        );

    if (!node) {
        fprintf(stderr, "stagetree: not enough space for '%s'.\n", name);

        return 0;
    }

    /* set_reserved_node_name also stamps the v1 djb id; override it with the v2
       parent-aware id below. */
    krffs_set_reserved_node_name(node, name);

    node->parent_id =
        parent_id;
    node->id =
        krffs_mix_id(parent_id, name);
    node->mode =
        mode;
    node->uid =
        0;
    node->gid =
        0;
    node->data_size =
        data_size;

    /* allocate_reserved_node does not clear the timestamps, so set them to a
       deterministic zero rather than inherit stale free-block bytes. */
    node->access_time =
        zero_time;
    node->modification_time =
        zero_time;
    node->attribute_change_time =
        zero_time;

    if (data_size > 0 && data) {
        memcpy((char *) (node + 1), data, (size_t) data_size);
    }

    krffs_sync_mapping(node, sizeof(*node) + (size_t) data_size);

    return node->id;
}

int main(int argc, char **argv)
{
    int exit_status =
        EXIT_SUCCESS;
    int file_descriptor =
        -1;
    struct krffs_file_system file_system = {
        .node = NULL
    };
    struct stat file_info;
    uint64_t etc_id;
    uint64_t needed;

    if (argc <= 1 || argv[1][0] == '-') {
        fprintf(
            stderr,
            "Usage: %s <image>\n",
            argc >= 1 ?
                argv[0] : "stagetree.krffs"
        );

        return EXIT_FAILURE;
    }

    if ((file_descriptor = PLATFORM_PREFIX(open(argv[1], O_RDWR))) == -1) {
        fprintf(stderr, "stagetree: cannot open '%s'.\n", argv[1]);

        return EXIT_FAILURE;
    }

    if (fstat(file_descriptor, &file_info) == -1 || !S_ISREG(file_info.st_mode)) {
        fprintf(stderr, "stagetree: '%s' is not a regular file.\n", argv[1]);

        exit_status =
            EXIT_FAILURE;

        goto cleanup;
    }

    if (file_info.st_size < (off_t) (sizeof(struct krffs_node) * 2)) {
        fprintf(stderr, "stagetree: '%s' is too small to hold a file system.\n", argv[1]);

        exit_status =
            EXIT_FAILURE;

        goto cleanup;
    }

    file_system.size =
        file_info.st_size;

    if ((file_system.node =
             krffs_map_file(
                 file_descriptor,
                 0,
                 file_system.size
             )) == (void *) -1) {
        fprintf(stderr, "stagetree: cannot map '%s'.\n", argv[1]);

        file_system.node =
            NULL;
        exit_status =
            EXIT_FAILURE;

        goto cleanup;
    }

    PLATFORM_PREFIX(close(file_descriptor));
    file_descriptor =
        -1;

    if (file_system.node->magic != KRFFS_File_System_Magic) {
        fprintf(stderr, "stagetree: no KRFFS file system in '%s'.\n", argv[1]);

        exit_status =
            EXIT_FAILURE;

        goto cleanup;
    }

    /* Idempotent: a staged image already has /etc, so there is nothing to do. */
    if (find_child(&file_system, 0, "etc") != NULL) {
        printf("stagetree: '/etc' already present in '%s'; nothing to do.\n", argv[1]);

        goto cleanup;
    }

    /* Preflight space for the whole tree so a run is all-or-nothing: three node
       headers plus the two file payloads must fit one free block. */
    needed =
        (uint64_t) sizeof(struct krffs_node) * 3 +
        (sizeof(STAGE_MOTD) - 1) +
        (sizeof(STAGE_HOSTNAME) - 1);

    if (largest_free_block(&file_system) < needed) {
        fprintf(stderr, "stagetree: not enough free space to stage /etc in '%s'.\n", argv[1]);

        exit_status =
            EXIT_FAILURE;

        goto cleanup;
    }

    /* /etc (a directory) and two regular files inside it. */
    etc_id =
        stage_node(&file_system, 0, "etc", (uint16_t) (KRFFS_MODE_IFDIR | 0755), NULL, 0);

    if (etc_id == 0 ||
        stage_node(&file_system, etc_id, "motd", 0644, STAGE_MOTD, sizeof(STAGE_MOTD) - 1) == 0 ||
        stage_node(
            &file_system, etc_id, "hostname", 0644, STAGE_HOSTNAME, sizeof(STAGE_HOSTNAME) - 1
        ) == 0) {
        exit_status =
            EXIT_FAILURE;

        goto cleanup;
    }

    printf("stagetree: staged /etc/motd and /etc/hostname into '%s'.\n", argv[1]);

cleanup:
    if (file_descriptor != -1) {
        PLATFORM_PREFIX(close(file_descriptor));
    }

    if (file_system.node != NULL && file_system.node != (void *) -1) {
        krffs_unmap_file(file_system.node, file_system.size);
    }

    return exit_status;
}
