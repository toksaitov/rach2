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
/* The root identity a login server (dropbear, Phase 23) reads via getpwnam. The
 * auth policy is PUBLIC KEY (authorized_keys, staged with the dropbear build):
 * Rach2 has no /etc/shadow and no password hashing, so the password field is "x"
 * and login is by key, not password. uid/gid 0, home /root, shell /bin/sh (which
 * Phase 23 points at busybox). */
static const char STAGE_PASSWD[] =
    "root:x:0:0:root:/root:/sh\n";
/* The list of valid login shells. dropbear validates a user's /etc/passwd shell
 * against getusershell() (which reads this file) before allowing login; without
 * /sh listed here it rejects root with "invalid shell". /bin/sh is listed too so
 * a future /bin/sh login shell also passes. */
static const char STAGE_SHELLS[] =
    "/sh\n/bin/sh\n";

/* Read a whole file into a malloc'd buffer (NUL-terminated, *len excludes the
 * terminator). Returns NULL if the file is absent/unreadable -- used to stage an
 * optional authorized_keys (the pubkey the dropbear SSH login authenticates). */
static char *read_whole_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    long n;
    char *buf;

    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    buf = (char *) malloc((size_t) n + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t) n, f) != (size_t) n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    buf[n] = '\0';
    *len = (size_t) n;
    return buf;
}

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

/* Idempotent stage: return the id of `name` inside directory `parent_id`,
   creating it (with `mode` and the optional `data` payload) only if it is absent.
   Returns 0 only on a genuine allocation failure -- so a re-run completes a
   partially staged tree instead of duplicating nodes or refusing to recover. */
static uint64_t ensure_node(
                    struct krffs_file_system *file_system,
                    uint64_t parent_id,
                    const char *name,
                    uint16_t mode,
                    const char *data,
                    uint64_t data_size
                )
{
    struct krffs_node *existing =
        find_child(file_system, parent_id, name);

    if (existing != NULL) {
        return existing->id;
    }

    return stage_node(file_system, parent_id, name, mode, data, data_size);
}

/* Free-space cost (one header plus payload) of staging `name` inside `parent_id`,
   or 0 if it already exists. The preflight sums this over the tree so it counts
   only what a run will actually allocate -- an all-or-nothing guarantee that still
   holds on a re-run that needs to add just a missing node. */
static uint64_t stage_cost(
                    struct krffs_file_system *file_system,
                    uint64_t parent_id,
                    const char *name,
                    uint64_t payload
                )
{
    if (find_child(file_system, parent_id, name) != NULL) {
        return 0;
    }

    return (uint64_t) sizeof(struct krffs_node) + payload;
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
    uint64_t etc_pre;
    uint64_t needed;
    char *ak = NULL;
    size_t ak_len = 0;

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

    /* Read the optional authorized_keys up front so the preflight covers the
       /root/.ssh subtree too; the whole tree is staged all-or-nothing. */
    ak =
        read_whole_file("authorized_keys", &ak_len);

    /* Preflight: sum the cost of every node THIS run will allocate (stage_cost is
       0 for a node that already exists, so a re-run adding only a missing piece is
       not rejected for the space the existing nodes hold). The ids are recomputed
       with krffs_mix_id, matching how staged children reference their parent. */
    etc_pre =
        krffs_mix_id(0, "etc");
    needed =
        stage_cost(&file_system, 0, "etc", 0) +
        stage_cost(&file_system, etc_pre, "motd", sizeof(STAGE_MOTD) - 1) +
        stage_cost(&file_system, etc_pre, "hostname", sizeof(STAGE_HOSTNAME) - 1) +
        stage_cost(&file_system, etc_pre, "passwd", sizeof(STAGE_PASSWD) - 1) +
        stage_cost(&file_system, etc_pre, "shells", sizeof(STAGE_SHELLS) - 1) +
        stage_cost(&file_system, etc_pre, "dropbear", 0);

    if (ak) {
        uint64_t root_pre =
            krffs_mix_id(0, "root");
        uint64_t ssh_pre =
            krffs_mix_id(root_pre, ".ssh");

        needed +=
            stage_cost(&file_system, 0, "root", 0) +
            stage_cost(&file_system, root_pre, ".ssh", 0) +
            stage_cost(&file_system, ssh_pre, "authorized_keys", ak_len);
    }

    if (largest_free_block(&file_system) < needed) {
        fprintf(stderr, "stagetree: not enough free space to stage the tree in '%s'.\n", argv[1]);

        exit_status =
            EXIT_FAILURE;

        goto cleanup;
    }

    /* /etc and its files, idempotently (find-or-create), so a re-run completes a
       partially staged tree. /etc/dropbear is where dropbear -R writes its host
       keys (0700); the dir must exist or the key create fails (krffs reports it as
       ENOSPC). */
    etc_id =
        ensure_node(&file_system, 0, "etc", (uint16_t) (KRFFS_MODE_IFDIR | 0755), NULL, 0);

    if (etc_id == 0 ||
        ensure_node(&file_system, etc_id, "motd", 0644, STAGE_MOTD, sizeof(STAGE_MOTD) - 1) == 0 ||
        ensure_node(
            &file_system, etc_id, "hostname", 0644, STAGE_HOSTNAME, sizeof(STAGE_HOSTNAME) - 1
        ) == 0 ||
        ensure_node(
            &file_system, etc_id, "passwd", 0644, STAGE_PASSWD, sizeof(STAGE_PASSWD) - 1
        ) == 0 ||
        ensure_node(
            &file_system, etc_id, "shells", 0644, STAGE_SHELLS, sizeof(STAGE_SHELLS) - 1
        ) == 0 ||
        ensure_node(&file_system, etc_id, "dropbear",
                    (uint16_t) (KRFFS_MODE_IFDIR | 0700), NULL, 0) == 0) {
        exit_status =
            EXIT_FAILURE;

        goto cleanup;
    }

    printf("stagetree: staged /etc (motd, hostname, passwd, shells, dropbear) into '%s'.\n", argv[1]);

    /* Stage /root/.ssh/authorized_keys for the dropbear SSH login when a pubkey was
     * found (read above). The home dir + .ssh are 0700 and the key 0600 so
     * dropbear's strict permission check accepts them; the krffs FUSE mount cannot
     * mkdir, so this directory tree is written here as v2 nodes. */
    if (ak) {
        uint64_t root_id = ensure_node(&file_system, 0, "root",
                                       (uint16_t) (KRFFS_MODE_IFDIR | 0755), NULL, 0);
        uint64_t ssh_id = (root_id != 0)
            ? ensure_node(&file_system, root_id, ".ssh",
                          (uint16_t) (KRFFS_MODE_IFDIR | 0700), NULL, 0)
            : 0;
        if (root_id == 0 || ssh_id == 0 ||
            ensure_node(&file_system, ssh_id, "authorized_keys", 0600, ak, ak_len) == 0) {
            fprintf(stderr, "stagetree: failed to stage /root/.ssh/authorized_keys.\n");
            exit_status = EXIT_FAILURE;
            goto cleanup;
        }
        printf("stagetree: staged /root/.ssh/authorized_keys (%lu bytes).\n",
               (unsigned long) ak_len);
    }

cleanup:
    free(ak);

    if (file_descriptor != -1) {
        PLATFORM_PREFIX(close(file_descriptor));
    }

    if (file_system.node != NULL && file_system.node != (void *) -1) {
        krffs_unmap_file(file_system.node, file_system.size);
    }

    return exit_status;
}
