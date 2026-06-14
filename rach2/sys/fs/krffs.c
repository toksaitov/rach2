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
 * ===== Hierarchical path resolution (KRFFS v2) =====
 *
 * A node's parent_id is its directory's logical id (KRFFS_ROOT_ID == 0 for the
 * root, whose own node is fs->node and carries no parent reference). A directory
 * node's id is mix(parent_id, name) -- unique because sibling names are unique --
 * so children reference it unambiguously. Resolution matches each path component
 * by (name, parent_id), descending only into directories. Regular files' ids are
 * never referenced, so they keep the legacy DJB(name) value for compatibility.
 */

/* Combine a parent directory id and a child name into a unique node id (FNV-1a
 * style). Never returns 0 (the root's logical id). */
static uint64_t krffs_mix_id(uint64_t parent_id, const char *name)
{
    uint64_t h = (parent_id + 1) * 1099511628211ULL;

    h ^= krffs_calculate_djb_hash((const uint8_t *)name);
    h *= 1099511628211ULL;
    return h ? h : 1;
}

/* Compare a NUL-terminated node name with the component comp[0..len). */
static int krffs_name_eq(const char *node_name, const char *comp, uint32_t len)
{
    uint32_t i;

    for (i = 0; i < len; i++) {
        if (node_name[i] == '\0' || node_name[i] != comp[i]) {
            return 0;
        }
    }
    return node_name[len] == '\0';
}

/* Find the reserved node with a given id (for ".." and directory descent).
 * Returns NULL for the root id (which has no reserved node). */
static krffs_node_t *krffs_find_node_by_id(krffs_file_system_t *fs, uint64_t id)
{
    krffs_node_t *node;

    if (id == KRFFS_ROOT_ID) {
        return NULL;
    }
    node = fs->node;
    do {
        if (node->type == KRFFS_RESERVED_NODE && node->id == id) {
            return node;
        }
    } while ((node = krffs_get_next_node(fs, node)) != fs->node);
    return NULL;
}

/* Find a child named comp[0..len) directly inside directory dir_id. */
static krffs_node_t *krffs_find_child(krffs_file_system_t *fs, uint64_t dir_id,
                                      const char *comp, uint32_t len)
{
    krffs_node_t *node = fs->node;

    do {
        if (node->type == KRFFS_RESERVED_NODE && node->parent_id == dir_id &&
            krffs_name_eq((const char *)node->name, comp, len)) {
            return node;
        }
    } while ((node = krffs_get_next_node(fs, node)) != fs->node);
    return NULL;
}

/*
 * Resolve a path to its node, or NULL if any component is missing or a non-final
 * component is not a directory. "/" (or "") resolves to the root node (fs->node).
 * '.' and '..' are honored.
 */
static krffs_node_t *krffs_walk(krffs_file_system_t *fs, const char *path)
{
    uint64_t cur_id = KRFFS_ROOT_ID;
    krffs_node_t *node = fs->node; /* the current directory's node; root = fs->node */
    const char *p = path ? path : "";

    while (*p == '/') {
        p++;
    }
    if (*p == '\0') {
        return fs->node;
    }

    for (;;) {
        const char *start = p;
        uint32_t len = 0;
        int last;

        while (start[len] != '\0' && start[len] != '/') {
            len++;
        }
        p = start + len;
        while (*p == '/') {
            p++;
        }
        last = (*p == '\0');

        if (len == 1 && start[0] == '.') {
            /* current directory: unchanged */
        } else if (len == 2 && start[0] == '.' && start[1] == '.') {
            node = krffs_find_node_by_id(fs, node ? node->parent_id : KRFFS_ROOT_ID);
            cur_id = node ? node->id : KRFFS_ROOT_ID;
        } else {
            node = krffs_find_child(fs, cur_id, start, len);
            if (!node) {
                return NULL;
            }
            if (!last && !KRFFS_IS_DIR(node->mode)) {
                return NULL; /* a non-final component must be a directory */
            }
            cur_id = node->id;
        }

        if (last) {
            return node;
        }
    }
}

/* Split path into its directory part and final component. dir gets the parent
 * path ("/" if the target is at the root), base gets the last component. */
static void krffs_split_path(const char *path, char *dir, char *base)
{
    const char *p = path;
    const char *last_slash = NULL;
    const char *comp;
    uint32_t i;

    while (*p == '/') {
        p++;
    }
    /* Find the last '/' that precedes the final non-empty component. */
    last_slash = NULL;
    {
        const char *s = path;
        for (; *s != '\0'; s++) {
            if (*s == '/' && s[1] != '\0' && s[1] != '/') {
                last_slash = s;
            }
        }
    }

    if (!last_slash) {
        dir[0] = '/';
        dir[1] = '\0';
        comp = p;
    } else {
        uint32_t n = (uint32_t)(last_slash - path);
        if (n == 0) {
            n = 1; /* keep the leading '/' as the root dir */
        }
        if (n > KRFFS_FILE_NAME_BUFFER_SIZE - 1) {
            n = KRFFS_FILE_NAME_BUFFER_SIZE - 1;
        }
        for (i = 0; i < n; i++) {
            dir[i] = path[i];
        }
        dir[n] = '\0';
        comp = last_slash + 1;
    }

    for (i = 0; i + 1 < KRFFS_FILE_NAME_BUFFER_SIZE && comp[i] != '\0' &&
                comp[i] != '/';
         i++) {
        base[i] = comp[i];
    }
    base[i] = '\0';
}

/* Resolve a path that must name a directory; returns its logical id, or
 * (uint64_t)-1 if the path is not an existing directory. */
static uint64_t krffs_resolve_dir_id(krffs_file_system_t *fs, const char *path)
{
    krffs_node_t *node = krffs_walk(fs, path);

    if (!node) {
        return (uint64_t)-1;
    }
    if (node == fs->node) {
        return KRFFS_ROOT_ID; /* the root directory */
    }
    if (!KRFFS_IS_DIR(node->mode)) {
        return (uint64_t)-1;
    }
    return node->id;
}

/*
 * Public path resolver: returns the node for `path` (the root node for "/"), or
 * NULL if any component is missing. The syscall layer uses it to attach a file or
 * directory to a file descriptor; callers inspect node->mode for the type.
 */
krffs_node_t *krffs_resolve(const char *path)
{
    if (!krffs_fs) {
        return NULL;
    }
    return krffs_walk(krffs_fs, path);
}

/*
 * Open a file (checks if it exists)
 * Returns 0 on success, -1 if not found
 */
int krffs_open(const char *path)
{
    krffs_node_t *node;

    if (!krffs_fs) {
        return -1;
    }

    node = krffs_walk(krffs_fs, path);
    if (!node || node == krffs_fs->node || KRFFS_IS_DIR(node->mode)) {
        return -1; /* missing, or a directory (opened through the *at dir API) */
    }

    return 0;
}

/*
 * Read file contents into buffer
 * Returns number of bytes read, or -1 on error
 */
/*
 * Read directly from an already-resolved node (the fd layer holds the node
 * pointer, so it need not re-resolve a path -- which would fail for a file in a
 * subdirectory, whose node->name is only its basename). Returns bytes read.
 */
int krffs_read_node(krffs_node_t *node, char *buffer, uint32_t size,
                    uint32_t offset)
{
    char *src;
    uint64_t data_size;
    uint32_t i;

    if (!node || !buffer) {
        return -1;
    }
    data_size = node->data_size;
    if ((uint64_t)offset >= data_size) {
        return 0; /* offset past end of file */
    }
    if ((uint64_t)offset + size > data_size) {
        size = (uint32_t)(data_size - offset);
    }
    src = ((char *)(node + 1)) + offset;
    for (i = 0; i < size; i++) {
        buffer[i] = src[i];
    }
    return (int)size;
}

int krffs_read(const char *path, char *buffer, uint32_t size, uint32_t offset)
{
    krffs_node_t *node;

    if (!krffs_fs || !buffer) {
        return -1;
    }
    node = krffs_walk(krffs_fs, path);
    if (!node || node == krffs_fs->node || KRFFS_IS_DIR(node->mode)) {
        return -1;
    }
    return krffs_read_node(node, buffer, size, offset);
}

/*
 * Get file size
 * Returns file size or 0 if not found
 */
uint32_t krffs_get_file_size(const char *path)
{
    krffs_node_t *node;

    if (!krffs_fs) {
        return 0;
    }

    node = krffs_walk(krffs_fs, path);
    if (!node || node == krffs_fs->node) {
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
    uint64_t parent_id;
    char dir[KRFFS_FILE_NAME_BUFFER_SIZE];
    char base[KRFFS_FILE_NAME_BUFFER_SIZE];

    if (!krffs_fs) {
        return -1;
    }

    /* Already exists (file or directory of that name) -> success, like creat. */
    if (krffs_walk(krffs_fs, path)) {
        return 0;
    }

    /* Resolve the parent directory; the leaf must be created inside it. */
    krffs_split_path(path, dir, base);
    if (base[0] == '\0') {
        return -1;
    }
    parent_id = krffs_resolve_dir_id(krffs_fs, dir);
    if (parent_id == (uint64_t)-1) {
        return -1; /* parent directory does not exist */
    }

    /* Allocate a new reserved node with 0 data size */
    node = krffs_allocate_reserved_node(krffs_fs, 0, NULL);
    if (!node) {
        return -1;
    }

    /* Set the leaf name (sets id = DJB(base), never referenced for a file) and
     * its containing directory. */
    krffs_set_reserved_node_name(node, base);
    node->parent_id = parent_id;

    /* Set default mode and uid/gid */
    node->mode = 0644;  /* rw-r--r-- regular file */
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
/*
 * Write directly to an already-resolved node. The node may be relocated by a
 * grow (krffs_resize_reserved_node), so *nodep is updated in place -- the fd
 * layer must store the result so its descriptor keeps pointing at the file.
 */
int krffs_write_node(krffs_node_t **nodep, const char *buffer, uint32_t size,
                     uint32_t offset)
{
    krffs_node_t *node;
    void *base;
    uint64_t available_space;
    uint64_t new_size;

    if (!krffs_fs || !nodep || !*nodep || !buffer) {
        return -1;
    }
    node = *nodep;
    base = node + 1;
    available_space = node->size - sizeof(krffs_node_t);
    new_size = (uint64_t)offset + size;

    if (new_size > available_space) {
        node = krffs_resize_reserved_node(krffs_fs, node, new_size);
        if (!node) {
            return -1;
        }
        *nodep = node; /* the node may have moved */
        base = node + 1;
    }
    if (new_size > node->data_size) {
        node->data_size = new_size;
    }
    _krffs_memcpy((char *)base + offset, buffer, size);
    return (int)size;
}

int krffs_write(const char *path, const char *buffer, uint32_t size, uint32_t offset)
{
    krffs_node_t *node;

    if (!krffs_fs || !buffer) {
        return -1;
    }
    node = krffs_walk(krffs_fs, path);
    if (!node || node == krffs_fs->node || KRFFS_IS_DIR(node->mode)) {
        serial_printf("KRFFS: File '%s' not found for write\n", path);
        return -1;
    }
    return krffs_write_node(&node, buffer, size, offset);
}

/*
 * Truncate (resize) a file
 * Returns 0 on success, -1 on error
 */
int krffs_truncate(const char *path, uint32_t new_size)
{
    krffs_node_t *node;
    uint64_t old_data_size;

    if (!krffs_fs) {
        return -1;
    }

    node = krffs_walk(krffs_fs, path);
    if (!node || node == krffs_fs->node || KRFFS_IS_DIR(node->mode)) {
        serial_printf("KRFFS: File '%s' not found for truncate\n", path);
        return -1;
    }

    old_data_size = node->data_size;

    node = krffs_resize_reserved_node(krffs_fs, node, new_size);
    if (!node) {
        serial_printf("KRFFS: No space to resize file '%s'\n", path);
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

    if (!krffs_fs) {
        return -1;
    }

    node = krffs_walk(krffs_fs, path);
    if (!node || node == krffs_fs->node || KRFFS_IS_DIR(node->mode)) {
        serial_printf("KRFFS: File '%s' not found for unlink\n", path);
        return -1; /* not a regular file (rmdir removes directories) */
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
    uint64_t parent_id;
    char dir[KRFFS_FILE_NAME_BUFFER_SIZE];
    char base[KRFFS_FILE_NAME_BUFFER_SIZE];

    if (!krffs_fs) {
        return -1;
    }

    node = krffs_walk(krffs_fs, old_path);
    if (!node || node == krffs_fs->node) {
        serial_printf("KRFFS: File '%s' not found for rename\n", old_path);
        return -1;
    }
    /* A directory's id is mix(parent,name) and its children reference it, so a
     * rename/move would orphan them. Renaming directories is unsupported. */
    if (KRFFS_IS_DIR(node->mode)) {
        return -1;
    }

    krffs_split_path(new_path, dir, base);
    if (base[0] == '\0') {
        return -1;
    }
    parent_id = krffs_resolve_dir_id(krffs_fs, dir);
    if (parent_id == (uint64_t)-1) {
        return -1; /* destination directory does not exist */
    }

    /* If the destination name exists (and is not the source), drop it. */
    existing = krffs_walk(krffs_fs, new_path);
    if (existing && existing != node) {
        krffs_remove_reserved_node(krffs_fs, existing);
    }

    /* Move: new name + new containing directory. */
    krffs_set_reserved_node_name(node, base);
    node->parent_id = parent_id;
    return 0;
}

/*
 * Create a directory at `path`. Returns 0 on success, -1 if the parent directory
 * is missing or the name is already taken.
 */
int krffs_mkdir(const char *path)
{
    krffs_node_t *node;
    uint64_t parent_id;
    char dir[KRFFS_FILE_NAME_BUFFER_SIZE];
    char base[KRFFS_FILE_NAME_BUFFER_SIZE];

    if (!krffs_fs) {
        return -1;
    }
    if (krffs_walk(krffs_fs, path)) {
        return -1; /* a file or directory of that name already exists */
    }

    krffs_split_path(path, dir, base);
    if (base[0] == '\0') {
        return -1;
    }
    parent_id = krffs_resolve_dir_id(krffs_fs, dir);
    if (parent_id == (uint64_t)-1) {
        return -1;
    }

    node = krffs_allocate_reserved_node(krffs_fs, 0, NULL);
    if (!node) {
        return -1;
    }

    /* A directory carries its name, the S_IFDIR mode, an empty body, and a UNIQUE
     * id (mix of parent + name) that its children reference as their parent_id. */
    _krffs_strncpy((char *)node->name, base, KRFFS_FILE_NAME_BUFFER_SIZE);
    node->name[KRFFS_FILE_NAME_BUFFER_SIZE - 1] = '\0';
    node->parent_id = parent_id;
    node->id = krffs_mix_id(parent_id, base);
    node->mode = (uint16_t)(KRFFS_S_IFDIR | 0755);
    node->uid = 0;
    node->gid = 0;
    node->access_time.seconds = 0;
    node->access_time.nanoseconds = 0;
    node->modification_time.seconds = 0;
    node->modification_time.nanoseconds = 0;
    node->attribute_change_time.seconds = 0;
    node->attribute_change_time.nanoseconds = 0;
    return 0;
}

/*
 * Remove an empty directory. Returns 0, or -1 if `path` is not a directory or
 * still has children.
 */
int krffs_rmdir(const char *path)
{
    krffs_node_t *node;
    krffs_node_t *scan;

    if (!krffs_fs) {
        return -1;
    }
    node = krffs_walk(krffs_fs, path);
    if (!node || node == krffs_fs->node || !KRFFS_IS_DIR(node->mode)) {
        return -1;
    }

    /* A directory is empty only if no node names it as a parent. */
    scan = krffs_fs->node;
    do {
        if (scan->type == KRFFS_RESERVED_NODE && scan->parent_id == node->id) {
            return -1; /* not empty */
        }
    } while ((scan = krffs_get_next_node(krffs_fs, scan)) != krffs_fs->node);

    krffs_remove_reserved_node(krffs_fs, node);
    return 0;
}

/*
 * Directory iteration for getdents: copy the index-th child name of the directory
 * at `path` into name_out (NUL-terminated, up to buf_size) and report whether it
 * is itself a directory via is_dir_out. Returns 1 if a child was returned, 0 past
 * the end, -1 on error. Linear per index -- a teaching simplification.
 */
int krffs_readdir(const char *path, uint32_t index, char *name_out,
                  uint32_t buf_size, int *is_dir_out)
{
    krffs_node_t *node;
    krffs_node_t *scan;
    uint64_t dir_id;
    uint32_t seen = 0;

    if (!krffs_fs || !name_out || buf_size == 0) {
        return -1;
    }
    node = krffs_walk(krffs_fs, path);
    if (!node) {
        return -1;
    }
    if (node != krffs_fs->node && !KRFFS_IS_DIR(node->mode)) {
        return -1; /* not a directory */
    }
    dir_id = (node == krffs_fs->node) ? KRFFS_ROOT_ID : node->id;

    scan = krffs_fs->node;
    do {
        if (scan->type == KRFFS_RESERVED_NODE && scan->parent_id == dir_id) {
            if (seen == index) {
                uint32_t i;
                for (i = 0; i + 1 < buf_size && scan->name[i] != '\0'; i++) {
                    name_out[i] = (char)scan->name[i];
                }
                name_out[i] = '\0';
                if (is_dir_out) {
                    *is_dir_out = KRFFS_IS_DIR(scan->mode) ? 1 : 0;
                }
                return 1;
            }
            seen++;
        }
    } while ((scan = krffs_get_next_node(krffs_fs, scan)) != krffs_fs->node);

    return 0; /* past the last entry */
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
