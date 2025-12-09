/*
 * KRFFS - K&R First-Fit File System Implementation
 * Read-only version for Phase 11
 *
 * Ported from the educational FUSE file system project.
 */

#include "krffs.h"
#include "../serial.h"

/*
 * Global file system instance
 */
krffs_file_system_t *krffs_fs = NULL;

/*
 * Local file system structure (static allocation)
 */
static krffs_file_system_t _krffs_fs_instance;

/*
 * DJB hash function for file name lookup
 * Same as original krffs implementation
 */
uint64_t krffs_calculate_djb_hash(const uint8_t *str)
{
    uint64_t hash = 5381;
    int c;

    if (!str) {
        return hash;
    }

    while ((c = *str++)) {
        hash = ((hash << 5) + hash) ^ (uint64_t)c;
    }

    return hash;
}

/*
 * Wrap node position to handle circular linked list
 */
krffs_node_t *krffs_wrap_node_position(krffs_file_system_t *fs, krffs_node_t *node)
{
    uintptr_t position;
    uintptr_t base;
    uintptr_t size;
    uintptr_t limit;

    if (!fs || !node) {
        return NULL;
    }

    position = (uintptr_t)node;
    base = (uintptr_t)fs->node;
    size = (uintptr_t)fs->size;
    limit = base + size;

    if (position >= limit) {
        position -= size;
    } else if (position < base) {
        position += size;
    }

    return (krffs_node_t *)position;
}

/*
 * Get the previous node in the linked list
 */
krffs_node_t *krffs_get_previous_node(krffs_file_system_t *fs, krffs_node_t *node)
{
    krffs_node_t *previous;

    if (!fs || !node) {
        return NULL;
    }

    previous = krffs_wrap_node_position(
        fs,
        (krffs_node_t *)(((char *)node) - node->previous_node_size)
    );

    return previous;
}

/*
 * Get the next node in the linked list
 */
krffs_node_t *krffs_get_next_node(krffs_file_system_t *fs, krffs_node_t *node)
{
    krffs_node_t *next;

    if (!fs || !node) {
        return NULL;
    }

    next = krffs_wrap_node_position(
        fs,
        (krffs_node_t *)(((char *)node) + node->size)
    );

    return next;
}

/*
 * Find a reserved (file) node by name
 */
krffs_node_t *krffs_find_reserved_node_by_name(krffs_file_system_t *fs, const char *name)
{
    krffs_node_t *node;
    uint64_t hash;

    if (!fs || !name) {
        return NULL;
    }

    hash = krffs_calculate_djb_hash((const uint8_t *)name);
    node = fs->node;

    do {
        if (node->type == KRFFS_RESERVED_NODE && node->id == hash) {
            return node;
        }
    } while ((node = krffs_get_next_node(fs, node)) != fs->node);

    return NULL;
}

/*
 * Find a reserved or root node by name
 */
krffs_node_t *krffs_find_reserved_or_root_node_by_name(krffs_file_system_t *fs, const char *name)
{
    krffs_node_t *node;
    uint64_t hash;

    if (!fs || !name) {
        return NULL;
    }

    hash = krffs_calculate_djb_hash((const uint8_t *)name);
    node = fs->node;

    do {
        if ((node->type == KRFFS_RESERVED_NODE || node->type == KRFFS_ROOT_NODE) &&
            node->id == hash) {
            return node;
        }
    } while ((node = krffs_get_next_node(fs, node)) != fs->node);

    return NULL;
}

/*
 * Open a file (checks if it exists)
 * Returns 0 on success, -1 if not found
 */
int krffs_open(const char *path)
{
    krffs_node_t *node;
    const char *name;

    if (!krffs_fs) {
        serial_printf("KRFFS: No file system mounted\n");
        return -1;
    }

    /* Skip leading '/' if present */
    name = path;
    if (name[0] == '/') {
        name++;
    }

    node = krffs_find_reserved_node_by_name(krffs_fs, name);
    if (!node) {
        serial_printf("KRFFS: File '%s' not found\n", name);
        return -1;
    }

    return 0;
}

/*
 * Read file contents into buffer
 * Returns number of bytes read, or -1 on error
 */
int krffs_read(const char *path, char *buffer, uint32_t size, uint32_t offset)
{
    krffs_node_t *node;
    const char *name;
    void *base;
    uint64_t data_size;

    if (!krffs_fs || !buffer) {
        return -1;
    }

    /* Skip leading '/' if present */
    name = path;
    if (name[0] == '/') {
        name++;
    }

    node = krffs_find_reserved_node_by_name(krffs_fs, name);
    if (!node) {
        serial_printf("KRFFS: File '%s' not found\n", name);
        return -1;
    }

    /* Data follows immediately after the node header */
    base = (void *)(node + 1);
    data_size = node->data_size;

    if ((uint64_t)offset >= data_size) {
        /* Offset past end of file */
        return 0;
    }

    if ((uint64_t)offset + size > data_size) {
        size = (uint32_t)(data_size - offset);
    }

    /* Copy data to buffer */
    {
        uint32_t i;
        char *src = ((char *)base) + offset;
        for (i = 0; i < size; i++) {
            buffer[i] = src[i];
        }
    }

    return (int)size;
}

/*
 * Get file size
 * Returns file size or 0 if not found
 */
uint32_t krffs_get_file_size(const char *path)
{
    krffs_node_t *node;
    const char *name;

    if (!krffs_fs) {
        return 0;
    }

    /* Skip leading '/' if present */
    name = path;
    if (name[0] == '/') {
        name++;
    }

    node = krffs_find_reserved_node_by_name(krffs_fs, name);
    if (!node) {
        return 0;
    }

    return (uint32_t)node->data_size;
}

/*
 * Count number of files (reserved nodes)
 */
uint32_t krffs_count_files(void)
{
    krffs_node_t *node;
    uint32_t count = 0;

    if (!krffs_fs) {
        return 0;
    }

    node = krffs_fs->node;
    do {
        if (node->type == KRFFS_RESERVED_NODE) {
            count++;
        }
    } while ((node = krffs_get_next_node(krffs_fs, node)) != krffs_fs->node);

    return count;
}

/*
 * List all files to serial output
 */
void krffs_list_files(void)
{
    krffs_node_t *node;
    uint32_t count = 0;

    if (!krffs_fs) {
        serial_printf("KRFFS: No file system mounted\n");
        return;
    }

    serial_printf("KRFFS files:\n");

    node = krffs_fs->node;
    do {
        if (node->type == KRFFS_RESERVED_NODE) {
            serial_printf("  [%d] %s (%d bytes)\n",
                         count, node->name, (uint32_t)node->data_size);
            count++;
        }
    } while ((node = krffs_get_next_node(krffs_fs, node)) != krffs_fs->node);

    if (count == 0) {
        serial_printf("  (no files)\n");
    }
}

/*
 * Initialize KRFFS from a RAM disk
 * Returns 0 on success, -1 on failure
 */
int init_krffs(void *ramdisk_base, uint32_t ramdisk_size)
{
    krffs_node_t *root;

    if (!ramdisk_base || ramdisk_size < sizeof(krffs_node_t)) {
        serial_printf("KRFFS: Invalid ramdisk (base=%p size=%d)\n",
                     ramdisk_base, ramdisk_size);
        return -1;
    }

    /* Point to the root node */
    root = (krffs_node_t *)ramdisk_base;

    /* Verify magic number */
    if (root->magic != KRFFS_FILE_SYSTEM_MAGIC) {
        serial_printf("KRFFS: Invalid magic 0x%X (expected 0x%X)\n",
                     root->magic, KRFFS_FILE_SYSTEM_MAGIC);
        return -1;
    }

    /* Verify this is a root node */
    if (root->type != KRFFS_ROOT_NODE) {
        serial_printf("KRFFS: First node is not root (type=%d)\n", root->type);
        return -1;
    }

    /* Set up the file system structure */
    _krffs_fs_instance.node = root;
    _krffs_fs_instance.size = ramdisk_size;
    krffs_fs = &_krffs_fs_instance;

    serial_printf("KRFFS: Mounted file system at 0x%X (%d bytes)\n",
                 (uintptr_t)ramdisk_base, ramdisk_size);

    /* List files for debug */
    krffs_list_files();

    return 0;
}
