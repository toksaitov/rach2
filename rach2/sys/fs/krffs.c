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
        return -1;
    }

    /* Skip leading '/' if present */
    name = path;
    if (name[0] == '/') {
        name++;
    }

    node = krffs_find_reserved_node_by_name(krffs_fs, name);
    if (!node) {
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
 * ========== Phase 12: Write Operations ==========
 */

/*
 * Helper: copy memory (no memcpy in kernel)
 */
static void _krffs_memcpy(void *dest, const void *src, uint32_t n)
{
    char *d = (char *)dest;
    const char *s = (const char *)src;
    uint32_t i;
    for (i = 0; i < n; i++) {
        d[i] = s[i];
    }
}

/*
 * Helper: copy string with limit (no strncpy in kernel)
 */
static void _krffs_strncpy(char *dest, const char *src, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }
    for (; i < n; i++) {
        dest[i] = '\0';
    }
}

/*
 * Initialize a free node
 */
krffs_node_t *krffs_initialize_free_node(krffs_node_t *node, uint64_t size, uint64_t previous_node_size)
{
    if (!node) {
        return NULL;
    }

    node->magic = KRFFS_FILE_SYSTEM_MAGIC;
    node->type = KRFFS_FREE_NODE;
    node->size = size;
    node->previous_node_size = previous_node_size;
    node->data_size = 0;

    return node;
}

/*
 * Set reserved node name and compute DJB hash
 */
void krffs_set_reserved_node_name(krffs_node_t *node, const char *name)
{
    if (!node || !name) {
        return;
    }

    _krffs_strncpy((char *)node->name, name, KRFFS_FILE_NAME_BUFFER_SIZE);
    node->name[KRFFS_FILE_NAME_BUFFER_SIZE - 1] = '\0';
    node->id = krffs_calculate_djb_hash(node->name);
}

/*
 * K&R first-fit allocator: allocate a reserved node
 * If original_node is provided, copies metadata and data from it
 */
krffs_node_t *krffs_allocate_reserved_node(krffs_file_system_t *fs, uint64_t size, krffs_node_t *original)
{
    krffs_node_t *result = NULL;
    krffs_node_t *node;
    uint64_t size_with_header;

    if (!fs || !fs->node) {
        return NULL;
    }

    size_with_header = sizeof(krffs_node_t) + size;
    node = fs->node;

    do {
        if (node->type == KRFFS_FREE_NODE && node->size >= size_with_header) {
            if (node->size > size_with_header) {
                /* Found a larger block, splitting it */
                uint64_t total_size = node->size;
                uint64_t second_half_size = total_size - size_with_header;

                /* Second part must be big enough for a header */
                if (second_half_size >= sizeof(krffs_node_t)) {
                    krffs_node_t *next_node;

                    /* Mark first half as used */
                    node->size = size_with_header;

                    /* Move to second half */
                    next_node = krffs_get_next_node(fs, node);

                    /* Initialize second half as free */
                    krffs_initialize_free_node(next_node, second_half_size, size_with_header);

                    /* Update next block's previous_node_size */
                    next_node = krffs_get_next_node(fs, next_node);
                    next_node->previous_node_size = second_half_size;
                }
            }

            /* Initialize as reserved node */
            node->type = KRFFS_RESERVED_NODE;
            node->data_size = size;

            if (original) {
                /* Copy metadata from original */
                _krffs_strncpy((char *)node->name, (char *)original->name, KRFFS_FILE_NAME_BUFFER_SIZE);
                node->name[KRFFS_FILE_NAME_BUFFER_SIZE - 1] = '\0';
                node->id = original->id;
                node->mode = original->mode;
                node->uid = original->uid;
                node->gid = original->gid;
                node->access_time = original->access_time;
                node->modification_time = original->modification_time;
                node->attribute_change_time = original->attribute_change_time;

                /* Copy data if present */
                if (original->data_size > 0) {
                    _krffs_memcpy(node + 1, original + 1, (uint32_t)original->data_size);
                }
            }

            result = node;
        }
    } while (!result && (node = krffs_get_next_node(fs, node)) != fs->node);

    return result;
}

/*
 * K&R allocator: resize a reserved node
 */
krffs_node_t *krffs_resize_reserved_node(krffs_file_system_t *fs, krffs_node_t *node, uint64_t size)
{
    krffs_node_t *result = NULL;
    krffs_node_t *next_node;
    uint64_t size_with_header;

    if (!fs || !fs->node) {
        return NULL;
    }

    if (!node || node->type != KRFFS_RESERVED_NODE) {
        return NULL;
    }

    size_with_header = sizeof(krffs_node_t) + size;

    /* Same size - nothing to do */
    if (node->size == size_with_header) {
        return node;
    }

    next_node = krffs_get_next_node(fs, node);

    if (size_with_header > node->size) {
        /* Need to increase size */
        uint64_t difference = size_with_header - node->size;

        if (next_node != fs->node &&
            next_node->type == KRFFS_FREE_NODE &&
            next_node->size >= difference) {
            /* Extend into adjacent free block */
            uint64_t node_size;
            uint64_t next_node_size = next_node->size - difference;

            node->size += difference;
            node->data_size = size;
            result = node;

            node_size = node->size;

            if (next_node_size > 0) {
                if (next_node_size < sizeof(krffs_node_t)) {
                    /* Not enough for header, extend reserved node further */
                    node->size += next_node_size;
                } else {
                    /* Resize the free block */
                    node = krffs_get_next_node(fs, node);
                    krffs_initialize_free_node(node, next_node_size, node_size);
                }
            }

            next_node = krffs_get_next_node(fs, node);
            next_node->previous_node_size = node->size;
        } else {
            /* Not enough adjacent space, relocate */
            result = krffs_allocate_reserved_node(fs, size, node);
            if (result) {
                krffs_remove_reserved_node(fs, node);
            }
        }
    } else {
        /* Shrinking */
        uint64_t difference = node->size - size_with_header;

        node->size = size_with_header;
        node->data_size = size;
        result = node;

        if (next_node->type == KRFFS_FREE_NODE) {
            /* Combine with adjacent free block */
            node = krffs_get_next_node(fs, node);
            krffs_initialize_free_node(node, difference + next_node->size, size_with_header);
            next_node = krffs_get_next_node(fs, node);
        } else {
            if (difference < sizeof(krffs_node_t)) {
                /* Not enough for header, extend reserved node back */
                node->size += difference;
            } else {
                /* Create new free block */
                node = krffs_get_next_node(fs, node);
                krffs_initialize_free_node(node, difference, size_with_header);
            }
        }

        next_node->previous_node_size = node->size;
    }

    return result;
}

/*
 * K&R allocator: remove a reserved node (mark as free and coalesce)
 */
void krffs_remove_reserved_node(krffs_file_system_t *fs, krffs_node_t *node)
{
    krffs_node_t *previous_node = NULL;
    krffs_node_t *next_node;
    bool size_changed = false;

    if (!fs || !fs->node) {
        return;
    }

    if (!node || node->type != KRFFS_RESERVED_NODE) {
        return;
    }

    /* Get previous node (if not at start) */
    if (node != fs->node) {
        previous_node = krffs_get_previous_node(fs, node);
    }

    /* Get next node */
    next_node = krffs_get_next_node(fs, node);
    if (next_node == fs->node) {
        next_node = NULL;
    }

    /* Mark as free */
    node->type = KRFFS_FREE_NODE;

    /* Try to coalesce with next free block */
    if (next_node && next_node->type == KRFFS_FREE_NODE) {
        node->size += next_node->size;
        size_changed = true;
    }

    /* Try to coalesce with previous free block */
    if (previous_node && previous_node->type == KRFFS_FREE_NODE) {
        previous_node->size += node->size;
        node = previous_node;
        size_changed = true;
    }

    /* Update next block's previous_node_size if needed */
    if (size_changed) {
        next_node = krffs_get_next_node(fs, node);
        next_node->previous_node_size = node->size;
    }
}

/*
 * Create a new empty file
 * Returns 0 on success, -1 on error
 */
int krffs_create(const char *path)
{
    krffs_node_t *node;
    const char *name;

    if (!krffs_fs) {
        return -1;
    }

    /* Skip leading '/' if present */
    name = path;
    if (name[0] == '/') {
        name++;
    }

    /* Check if file already exists */
    node = krffs_find_reserved_node_by_name(krffs_fs, name);
    if (node) {
        /* File exists - this is OK for create, we just return success */
        return 0;
    }

    /* Allocate a new reserved node with 0 data size */
    node = krffs_allocate_reserved_node(krffs_fs, 0, NULL);
    if (!node) {
        return -1;
    }

    /* Set the file name */
    krffs_set_reserved_node_name(node, name);

    /* Set default mode and uid/gid */
    node->mode = 0644;  /* rw-r--r-- */
    node->uid = 0;
    node->gid = 0;

    /* Set timestamps to 0 (we don't have a real clock) */
    node->access_time.seconds = 0;
    node->access_time.nanoseconds = 0;
    node->modification_time.seconds = 0;
    node->modification_time.nanoseconds = 0;
    node->attribute_change_time.seconds = 0;
    node->attribute_change_time.nanoseconds = 0;

    return 0;
}

/*
 * Write data to a file
 * Returns number of bytes written, or -1 on error
 */
int krffs_write(const char *path, const char *buffer, uint32_t size, uint32_t offset)
{
    krffs_node_t *node;
    const char *name;
    void *base;
    uint64_t available_space;
    uint64_t new_size;

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
        serial_printf("KRFFS: File '%s' not found for write\n", name);
        return -1;
    }

    base = node + 1;
    available_space = node->size - sizeof(krffs_node_t);
    new_size = (uint64_t)offset + size;

    if (new_size <= available_space) {
        /* Fits in current allocation */
        if (new_size > node->data_size) {
            node->data_size = new_size;
        }

        _krffs_memcpy((char *)base + offset, buffer, size);
        return (int)size;
    } else {
        /* Need to resize */
        node = krffs_resize_reserved_node(krffs_fs, node, new_size);
        if (!node) {
            serial_printf("KRFFS: No space to expand file '%s'\n", name);
            return -1;
        }

        base = node + 1;
        _krffs_memcpy((char *)base + offset, buffer, size);
        return (int)size;
    }
}

/*
 * Truncate (resize) a file
 * Returns 0 on success, -1 on error
 */
int krffs_truncate(const char *path, uint32_t new_size)
{
    krffs_node_t *node;
    const char *name;
    uint64_t old_data_size;

    if (!krffs_fs) {
        return -1;
    }

    /* Skip leading '/' if present */
    name = path;
    if (name[0] == '/') {
        name++;
    }

    node = krffs_find_reserved_node_by_name(krffs_fs, name);
    if (!node) {
        serial_printf("KRFFS: File '%s' not found for truncate\n", name);
        return -1;
    }

    old_data_size = node->data_size;

    node = krffs_resize_reserved_node(krffs_fs, node, new_size);
    if (!node) {
        serial_printf("KRFFS: No space to resize file '%s'\n", name);
        return -1;
    }

    /* Zero-fill if extending */
    if ((uint64_t)new_size > old_data_size) {
        void *base = node + 1;
        char *dest = (char *)base + old_data_size;
        uint32_t fill_size = new_size - (uint32_t)old_data_size;
        k_memset(dest, 0, fill_size);
    }

    return 0;
}

/*
 * Delete a file
 * Returns 0 on success, -1 on error
 */
int krffs_unlink(const char *path)
{
    krffs_node_t *node;
    const char *name;

    if (!krffs_fs) {
        return -1;
    }

    /* Skip leading '/' if present */
    name = path;
    if (name[0] == '/') {
        name++;
    }

    node = krffs_find_reserved_node_by_name(krffs_fs, name);
    if (!node) {
        serial_printf("KRFFS: File '%s' not found for unlink\n", name);
        return -1;
    }

    krffs_remove_reserved_node(krffs_fs, node);
    return 0;
}

/*
 * Rename a file
 * Returns 0 on success, -1 on error
 */
int krffs_rename(const char *old_path, const char *new_path)
{
    krffs_node_t *node;
    krffs_node_t *existing;
    const char *old_name;
    const char *new_name;

    if (!krffs_fs) {
        return -1;
    }

    /* Skip leading '/' if present */
    old_name = old_path;
    if (old_name[0] == '/') {
        old_name++;
    }
    new_name = new_path;
    if (new_name[0] == '/') {
        new_name++;
    }

    node = krffs_find_reserved_node_by_name(krffs_fs, old_name);
    if (!node) {
        serial_printf("KRFFS: File '%s' not found for rename\n", old_name);
        return -1;
    }

    /* If new name exists, delete it first */
    existing = krffs_find_reserved_node_by_name(krffs_fs, new_name);
    if (existing) {
        krffs_remove_reserved_node(krffs_fs, existing);
    }

    /* Update the name */
    krffs_set_reserved_node_name(node, new_name);
    return 0;
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

    return 0;
}
