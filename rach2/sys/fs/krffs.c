/*
 * KRFFS: a K&R first-fit file system.
 *
 * The medium is one contiguous region holding a circular doubly linked list
 * of nodes. Each node has a header (krffs_node_t) followed by its data;
 * a node is either free or reserved (a file or directory). Allocation is
 * first-fit with split-and-coalesce, after the storage allocator in The C
 * Programming Language. Directories are hierarchical (the v2 parent_id
 * scheme described below).
 */

#include "krffs.h"

#include "../rtc.h"
#include "../serial.h"
#include "../lib/kerrno.h"

krffs_file_system_t *krffs_fs = NULL;

static krffs_file_system_t _krffs_fs_instance;

/**
 * Hash a file name with Bernstein's DJB algorithm.
 *
 * A NULL pointer hashes to the seed (5381). This is the legacy id of a regular
 * file, kept for on-disk compatibility; directory ids use krffs_mix_id instead.
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

/**
 * Wrap a node address back into the file system region.
 *
 * The node list is circular: walking off either end of [fs->node, fs->node +
 * fs->size) folds the address by one region size so it lands on the other end.
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

/**
 * Return the node preceding `node`, using its stored previous_node_size.
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

/**
 * Return the node following `node`, advancing by its own size.
 */
krffs_node_t *krffs_get_next_node(krffs_file_system_t *fs, krffs_node_t *node)
{
    krffs_node_t *next;

    if (!fs || !node) {
        return NULL;
    }

    /* A node smaller than its own header would not advance (an unbounded walk)
     * or fold to a bad address. The mount-time pass rejects such an image, so
     * this is a runtime backstop: report the root so a `!= fs->node` walk stops
     * cleanly instead of looping or reading out of bounds. */
    if (node->size < sizeof(krffs_node_t)) {
        return fs->node;
    }

    next = krffs_wrap_node_position(
        fs,
        (krffs_node_t *)(((char *)node) + node->size)
    );

    return next;
}

/**
 * Find a reserved node whose name hashes to that of `name`.
 *
 * Scans the whole list by DJB id, ignoring the directory hierarchy, so it
 * can collide with same-named files in different directories. The
 * hierarchical krffs_walk is the real path resolver; this remains for
 * flat-namespace callers.
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

/**
 * Like krffs_find_reserved_node_by_name, but also matches the root node.
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
 * Hierarchical path resolution (the KRFFS v2 on-disk format).
 *
 * A node's parent_id is its directory's logical id (KRFFS_ROOT_ID == 0 for
 * the root, whose own node is fs->node and has no parent reference). A
 * directory node's id is mix(parent_id, name), unique because sibling names
 * are unique, so children reference it unambiguously. Resolution matches
 * each path component by (name, parent_id), descending only into
 * directories. Regular files' ids are never referenced, so they keep the
 * legacy DJB(name) value for compatibility.
 */

/**
 * Combine a parent directory id and a child name into a unique node id.
 *
 * FNV-1a style mixing. Never returns 0, which is reserved for the root's
 * logical id (KRFFS_ROOT_ID).
 */
static uint64_t krffs_mix_id(uint64_t parent_id, const char *name)
{
    uint64_t h = (parent_id + 1) * 1099511628211ULL;

    h ^= krffs_calculate_djb_hash((const uint8_t *)name);
    h *= 1099511628211ULL;
    return h ? h : 1;
}

/**
 * Test whether the NUL-terminated `node_name` equals the component
 * comp[0..len).
 */
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

/**
 * Find the reserved node with logical id `id`, for ".." and directory descent.
 *
 * Returns NULL for KRFFS_ROOT_ID: the root has no reserved node (it is
 * fs->node), so callers must handle the root specially, not expect a
 * lookup here.
 */
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

/**
 * Find the child named comp[0..len) directly inside directory `dir_id`.
 */
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

/**
 * Resolve `path` to its node, honoring '.' and '..'.
 *
 * "/" (or "") resolves to the root node (fs->node). A non-final component that
 * is not a directory, or any missing component, fails. When `err` is non-NULL
 * it receives -ENOENT for a missing component or -ENOTDIR for a non-directory
 * in a non-final position. The thin krffs_walk wrapper below serves the common
 * callers that do not care which.
 *
 * `may_search`, when non-NULL, is asked about every directory the walk
 * enters, before the component inside it is matched, and its refusal becomes
 * the walk's error. It is charged on the root and on the '.' and '..' arms
 * too: traversing a directory is what the search bit governs, whichever
 * component names it. The target itself is never charged, since reaching a
 * name is not reading it.
 */
static krffs_node_t *krffs_walk_x(krffs_file_system_t *fs, const char *path, int *err,
                                  krffs_search_fn may_search)
{
    uint64_t cur_id = KRFFS_ROOT_ID;
    krffs_node_t *node = fs->node; /* current directory's node; root == fs->node */
    const char *p = path ? path : "";

    if (err) {
        *err = 0;
    }

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

        if (may_search && node) {
            int srch = may_search(node);
            if (srch != 0) {
                if (err) {
                    *err = srch;
                }
                return NULL;
            }
        }

        while (start[len] != '\0' && start[len] != '/') {
            len++;
        }
        p = start + len;
        while (*p == '/') {
            p++;
        }
        last = (*p == '\0');

        if (len == 1 && start[0] == '.') {
            /* "." stays in the current directory */
        } else if (len == 2 && start[0] == '.' && start[1] == '.') {
            /* krffs_find_node_by_id cannot return the root (it owns reserved id
             * 0), so resolve a root parent to fs->node directly; otherwise ".."
             * back to the root would yield NULL. The continuing id is ROOT_ID
             * so the root's children (parent_id == 0) still resolve after the
             * "..".
             */
            uint64_t parent = node ? node->parent_id : (uint64_t)KRFFS_ROOT_ID;
            if (parent == (uint64_t)KRFFS_ROOT_ID) {
                node = fs->node;
                cur_id = (uint64_t)KRFFS_ROOT_ID;
            } else {
                node = krffs_find_node_by_id(fs, parent);
                cur_id = node ? node->id : (uint64_t)KRFFS_ROOT_ID;
            }
        } else {
            node = krffs_find_child(fs, cur_id, start, len);
            if (!node) {
                if (err) {
                    *err = -ENOENT;
                }
                return NULL;
            }
            if (!last && !KRFFS_IS_DIR(node->mode)) {
                if (err) {
                    *err = -ENOTDIR;
                }
                return NULL;
            }
            cur_id = node->id;
        }

        if (last) {
            return node;
        }
    }
}

/**
 * Resolve `path` to its node, or NULL, with no error distinction.
 */
static krffs_node_t *krffs_walk(krffs_file_system_t *fs, const char *path)
{
    return krffs_walk_x(fs, path, (int *)0, (krffs_search_fn)0);
}

/**
 * Split `path` into its directory part and final component.
 *
 * `dir` receives the parent path ("/" if the target sits at the root) and
 * `base` the last component. Both buffers are KRFFS_FILE_NAME_BUFFER_SIZE
 * and the output is truncated to fit.
 */
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

/**
 * Resolve a path that must name a directory and return its logical id.
 *
 * Returns KRFFS_ROOT_ID for "/", or (uint64_t)-1 if the path is not an existing
 * directory.
 */
static uint64_t krffs_resolve_dir_id(krffs_file_system_t *fs, const char *path)
{
    krffs_node_t *node = krffs_walk(fs, path);

    if (!node) {
        return (uint64_t)-1;
    }
    if (node == fs->node) {
        return KRFFS_ROOT_ID;
    }
    if (!KRFFS_IS_DIR(node->mode)) {
        return (uint64_t)-1;
    }
    return node->id;
}

/**
 * Resolve `path` to its node for the syscall layer.
 *
 * Returns the node for `path` (the root node for "/"), or NULL if any component
 * is missing. Callers attach the node to a file descriptor and inspect
 * node->mode for the type.
 */
krffs_node_t *krffs_resolve(const char *path)
{
    if (!krffs_fs) {
        return NULL;
    }
    return krffs_walk(krffs_fs, path);
}

/**
 * Like krffs_resolve, but report the failure kind through `err`.
 *
 * Sets *err to 0 on success, -ENOENT for a missing component, or -ENOTDIR for a
 * non-directory in the path. The syscall layer uses this to return the right
 * errno from open, stat, chdir, and the *at calls.
 */
krffs_node_t *krffs_resolve_e(const char *path, int *err)
{
    return krffs_resolve_x(path, err, (krffs_search_fn)0);
}

/**
 * Resolve `path`, charging `may_search` on every directory the walk enters.
 *
 * The predicate reads the calling process's credentials, so this is only valid
 * from syscall context. With a NULL predicate it is exactly krffs_resolve_e.
 */
krffs_node_t *krffs_resolve_x(const char *path, int *err, krffs_search_fn may_search)
{
    if (!krffs_fs) {
        if (err) {
            *err = -ENOENT;
        }
        return NULL;
    }
    return krffs_walk_x(krffs_fs, path, err, may_search);
}

/**
 * Build the absolute path of `node` into `buf` (size bytes).
 *
 * Walks the parent_id chain up to the root, then emits the basenames in forward
 * order. Canonical by construction: it follows real directory links, so it
 * collapses any '.' or '..' in the path used to reach the node. The root yields
 * "/". Returns 0 on success, -1 if the node is unreachable or the path does not
 * fit.
 */
int krffs_node_path(krffs_node_t *node, char *buf, uint32_t size)
{
    const char *names[KRFFS_PATH_MAX_DEPTH];
    int depth = 0;
    int i;
    uint32_t pos = 0;
    krffs_node_t *cur = node;

    if (!krffs_fs || !node || !buf || size == 0) {
        return -1;
    }

    /* Collect basenames from the node up toward the root. krffs_find_node_by_id
     * cannot return the root (it has the reserved id 0), so stop as soon as a
     * node's parent IS the root, not try to look the root up. */
    for (;;) {
        if (cur == krffs_fs->node || cur->id == (uint64_t)KRFFS_ROOT_ID) {
            break;
        }
        if (depth >= KRFFS_PATH_MAX_DEPTH) {
            return -1;
        }
        names[depth++] = (const char *)cur->name;
        if (cur->parent_id == (uint64_t)KRFFS_ROOT_ID) {
            break;
        }
        cur = krffs_find_node_by_id(krffs_fs, cur->parent_id);
        if (!cur) {
            return -1; /* a parent link was dangling */
        }
    }
    if (depth == 0) {
        if (size < 2) {
            return -1;
        }
        buf[0] = '/';
        buf[1] = '\0';
        return 0;
    }

    /* Emit "/name" for each component, deepest last. */
    for (i = depth - 1; i >= 0; i--) {
        uint32_t n = (uint32_t)k_strlen(names[i]);
        if (pos + 1 + n + 1 > size) {
            return -1; /* the path, including its NUL, would not fit */
        }
        buf[pos++] = '/';
        k_memcpy(buf + pos, names[i], n);
        pos += n;
    }
    buf[pos] = '\0';
    return 0;
}

/**
 * Check that `path` names an existing regular file.
 *
 * Returns 0 if so, -1 if the path is missing or names a directory (directories
 * are opened through the *at directory API). It does not retain any state; the
 * fd layer resolves and holds the node itself.
 */
int krffs_open(const char *path)
{
    krffs_node_t *node;

    if (!krffs_fs) {
        return -1;
    }

    node = krffs_walk(krffs_fs, path);
    if (!node || node == krffs_fs->node || KRFFS_IS_DIR(node->mode)) {
        return -1;
    }

    return 0;
}

/**
 * Read up to `size` bytes at `offset` from an already-resolved node.
 *
 * The fd layer has the node pointer, so it need not re-resolve a path,
 * which would fail for a file in a subdirectory whose node->name is only its
 * basename. Returns the number of bytes read (0 past end of file), or -1 on
 * error.
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
        return 0;
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

/**
 * Resolve `path` and read from it via krffs_read_node.
 */
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

/**
 * Return the data size of the file at `path`, or 0 if it does not exist.
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

/**
 * Count the reserved nodes (files and directories) in the file system.
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
        /* An orphan is not a file any more: no path names it, so counting it
         * would report a file the boot banner's reader cannot find. */
        if (node->type == KRFFS_RESERVED_NODE && node->name[0] != '\0') {
            count++;
        }
    } while ((node = krffs_get_next_node(krffs_fs, node)) != krffs_fs->node);

    return count;
}

/**
 * Print every reserved node's name and size to the serial console.
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
        if (node->type == KRFFS_RESERVED_NODE && node->name[0] != '\0') {
            serial_printf("  [%d] %s (%d bytes)\n",
                         count, node->name, (uint32_t)node->data_size);
            count++;
        }
    } while ((node = krffs_get_next_node(krffs_fs, node)) != krffs_fs->node);

    if (count == 0) {
        serial_printf("  (no files)\n");
    }
}

/* Freestanding: no libc, so the write path has its own memcpy and
 * strncpy. */

static void _krffs_memcpy(void *dest, const void *src, uint32_t n)
{
    char *d = (char *)dest;
    const char *s = (const char *)src;
    uint32_t i;
    for (i = 0; i < n; i++) {
        d[i] = s[i];
    }
}

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

/**
 * Stamp a region as a free node of `size` bytes whose predecessor was
 * `previous_node_size` bytes.
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

/**
 * Copy `name` into a reserved node and set its id to DJB(name).
 *
 * Used for regular files, whose id is never referenced by a child. Directory
 * ids must instead come from krffs_mix_id so that parent_id lookups stay
 * unique.
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

/**
 * Allocate a reserved node with room for `size` data bytes (K&R first-fit).
 *
 * Scans the list for the first free node large enough for the header plus
 * `size`. A larger free node is split, leaving the remainder free, unless the
 * remainder is too small to hold a header (then the whole node is taken). If
 * `original` is non-NULL its metadata and data are copied into the new node, as
 * the relocation half of a grow. Returns the node, or NULL if no block fits.
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
                uint64_t total_size = node->size;
                uint64_t second_half_size = total_size - size_with_header;

                /* Split only if the tail can hold a node header of its own;
                 * otherwise leave the slack inside this node. */
                if (second_half_size >= sizeof(krffs_node_t)) {
                    krffs_node_t *next_node;

                    node->size = size_with_header;

                    next_node = krffs_get_next_node(fs, node);
                    krffs_initialize_free_node(next_node, second_half_size, size_with_header);

                    /* Repair the following node's back link to the new tail. */
                    next_node = krffs_get_next_node(fs, next_node);
                    next_node->previous_node_size = second_half_size;
                }
            }

            node->type = KRFFS_RESERVED_NODE;
            node->data_size = size;

            if (original) {
                _krffs_strncpy((char *)node->name, (char *)original->name, KRFFS_FILE_NAME_BUFFER_SIZE);
                node->name[KRFFS_FILE_NAME_BUFFER_SIZE - 1] = '\0';
                node->id = original->id;
                node->parent_id = original->parent_id; /* keep its directory membership */
                node->mode = original->mode;
                node->uid = original->uid;
                node->gid = original->gid;
                node->access_time = original->access_time;
                node->modification_time = original->modification_time;
                node->attribute_change_time = original->attribute_change_time;

                if (original->data_size > 0) {
                    _krffs_memcpy(node + 1, original + 1, (uint32_t)original->data_size);
                }
            }

            result = node;
        }
    } while (!result && (node = krffs_get_next_node(fs, node)) != fs->node);

    return result;
}

/**
 * Resize a reserved node to hold `size` data bytes (K&R allocator).
 *
 * Growing extends into the adjacent free node when one is large enough;
 * otherwise the node is relocated to a fresh allocation and the old one freed,
 * so the returned pointer may differ from the argument. Shrinking returns the
 * surplus to the free list, coalescing with a following free node. In either
 * direction a remainder too small for a header is absorbed into the node rather
 * than left as an unusable fragment. Returns the (possibly moved) node, or NULL
 * if a relocation could not find space.
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

    if (node->size == size_with_header) {
        return node;
    }

    next_node = krffs_get_next_node(fs, node);

    if (size_with_header > node->size) {
        uint64_t difference = size_with_header - node->size;

        if (next_node != fs->node &&
            next_node->type == KRFFS_FREE_NODE &&
            next_node->size >= difference) {
            uint64_t node_size;
            uint64_t next_node_size = next_node->size - difference;

            node->size += difference;
            node->data_size = size;
            result = node;

            node_size = node->size;

            if (next_node_size > 0) {
                if (next_node_size < sizeof(krffs_node_t)) {
                    /* Remainder cannot hold a header: absorb it. */
                    node->size += next_node_size;
                } else {
                    node = krffs_get_next_node(fs, node);
                    krffs_initialize_free_node(node, next_node_size, node_size);
                }
            }

            next_node = krffs_get_next_node(fs, node);
            next_node->previous_node_size = node->size;
        } else {
            /* No adjacent room: relocate, then free the old node. */
            result = krffs_allocate_reserved_node(fs, size, node);
            if (result) {
                krffs_remove_reserved_node(fs, node);
            }
        }
    } else {
        uint64_t difference = node->size - size_with_header;

        node->size = size_with_header;
        node->data_size = size;
        result = node;

        if (next_node->type == KRFFS_FREE_NODE) {
            /* Merge the surplus with the following free node. */
            node = krffs_get_next_node(fs, node);
            krffs_initialize_free_node(node, difference + next_node->size, size_with_header);
            next_node = krffs_get_next_node(fs, node);
        } else {
            if (difference < sizeof(krffs_node_t)) {
                /* Surplus cannot hold a header: absorb it. */
                node->size += difference;
            } else {
                node = krffs_get_next_node(fs, node);
                krffs_initialize_free_node(node, difference, size_with_header);
            }
        }

        next_node->previous_node_size = node->size;
    }

    return result;
}

/**
 * Free a reserved node and coalesce it with adjacent free neighbors.
 *
 * Marks the node free, then merges forward with a free successor and
 * backward with a free predecessor, so the free list never has two
 * adjacent free nodes. When the merged extent changes size, the following
 * node's back link is repaired to point at the new size.
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

    /* The first node has no predecessor to merge with; the last has no
     * successor (next would wrap to the root). */
    if (node != fs->node) {
        previous_node = krffs_get_previous_node(fs, node);
    }
    next_node = krffs_get_next_node(fs, node);
    if (next_node == fs->node) {
        next_node = NULL;
    }

    node->type = KRFFS_FREE_NODE;

    if (next_node && next_node->type == KRFFS_FREE_NODE) {
        node->size += next_node->size;
        size_changed = true;
    }
    if (previous_node && previous_node->type == KRFFS_FREE_NODE) {
        previous_node->size += node->size;
        node = previous_node;
        size_changed = true;
    }

    if (size_changed) {
        next_node = krffs_get_next_node(fs, node);
        next_node->previous_node_size = node->size;
    }
}

/**
 * Create an empty regular file at `path`.
 *
 * Idempotent like creat: an existing name (file or directory) returns success.
 * The parent directory must already exist. Returns 0 on success, -1 if the
 * parent is missing or no node could be allocated.
 */
/**
 * Stamp a node's times with the current moment.
 *
 * `KRFFS_STAMP_DATA` is what a write does: the modification time and the
 * attribute-change time both move, because changing a file changes the file.
 * `KRFFS_STAMP_ATTR` is what a chmod does: only the attribute-change time.
 *
 * Access time is never stamped here, and that is a decision, not an
 * omission. This file system lives behind a write-back page cache, so touching
 * a node on every READ would dirty a page on every read and turn a reading
 * workload into a writing one. An atime set on purpose through `utimensat` is
 * kept and reported; nothing advances it by itself. That is a `noatime` mount,
 * and it is written down in the Readme as the contract it is.
 */
void krffs_stamp(krffs_node_t *node, int what)
{
    int64_t seconds = 0;
    int64_t nanoseconds = 0;

    if (!node) {
        return;
    }
    rtc_realtime(&seconds, &nanoseconds);

    if (what == KRFFS_STAMP_DATA) {
        node->modification_time.seconds = seconds;
        node->modification_time.nanoseconds = nanoseconds;
    }
    node->attribute_change_time.seconds = seconds;
    node->attribute_change_time.nanoseconds = nanoseconds;
}

/**
 * The directory a node lives in.
 *
 * A parent id of `KRFFS_ROOT_ID` names the root, which is not in the node table
 * as a child of anything. Returns NULL only when the chain is broken, which is
 * a corrupt image, not an ordinary case.
 */
krffs_node_t *krffs_parent_node(krffs_node_t *node)
{
    if (!krffs_fs || !node) {
        return (krffs_node_t *)0;
    }
    if (node->parent_id == KRFFS_ROOT_ID) {
        return krffs_fs->node;
    }

    return krffs_find_node_by_id(krffs_fs, node->parent_id);
}

int krffs_create(const char *path)
{
    krffs_node_t *node;
    uint64_t parent_id;
    char dir[KRFFS_FILE_NAME_BUFFER_SIZE];
    char base[KRFFS_FILE_NAME_BUFFER_SIZE];

    if (!krffs_fs) {
        return -1;
    }

    if (krffs_walk(krffs_fs, path)) {
        return 0;
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

    krffs_set_reserved_node_name(node, base);
    node->parent_id = parent_id;

    node->mode = 0644;  /* rw-r--r-- regular file */
    node->uid = 0;
    node->gid = 0;

    /* Born now, in all three: a file created and never written still has a
     * moment it came into being, and a build tool compares against it. */
    krffs_stamp(node, KRFFS_STAMP_DATA);
    node->access_time = node->modification_time;

    /* And the directory it appeared in changed, which is what a program
     * watching a directory for new files is reading. */
    krffs_stamp(krffs_parent_node(node), KRFFS_STAMP_DATA);

    return 0;
}

/**
 * Write `size` bytes at `offset` into an already-resolved node.
 *
 * A write past the node's capacity grows it, which may relocate the node, so
 * *nodep is updated in place. The fd layer must store the result so its
 * descriptor keeps pointing at the file. Returns bytes written, or -1 on error.
 */
int krffs_write_node(krffs_node_t **nodep, const char *buffer, uint32_t size,
                     uint32_t offset)
{
    krffs_node_t *node;
    void *base;
    uint64_t available_space;
    uint64_t new_size;
    uint64_t old_data_size;

    if (!krffs_fs || !nodep || !*nodep || !buffer) {
        return -1;
    }
    node = *nodep;
    base = node + 1;
    /* Taken BEFORE the resize below, which sets `data_size` itself when it can
     * grow the node in place. Read afterwards it is already the new size, and
     * the gap test below would never fire. */
    old_data_size = node->data_size;
    available_space = node->size - sizeof(krffs_node_t);
    /* A write of nothing does nothing. POSIX says a successful zero-byte write
     * to a regular file has no other result, and without this the arithmetic
     * below would still grow the file to `offset` and publish whatever the
     * allocator last left in those bytes. */
    if (size == 0) {
        return 0;
    }

    new_size = (uint64_t)offset + size;

    if (new_size > available_space) {
        node = krffs_resize_reserved_node(krffs_fs, node, new_size);
        if (!node) {
            return -1;
        }
        *nodep = node;
        base = node + 1;
    }
    /* A write starting past the end leaves a hole, and the bytes under it are
     * whatever the free space held: another file's contents, until they are
     * cleared. `krffs_truncate` already zeroes what it grows; this is the same
     * rule for the other way a file grows. */
    if ((uint64_t)offset > old_data_size) {
        k_memset((char *)base + old_data_size, 0,
                 (uint32_t)((uint64_t)offset - old_data_size));
    }
    if (new_size > node->data_size) {
        node->data_size = new_size;
    }
    _krffs_memcpy((char *)base + offset, buffer, size);
    krffs_stamp(node, KRFFS_STAMP_DATA);
    return (int)size;
}

/**
 * Resolve `path` and write to it via krffs_write_node.
 */
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

/**
 * Resize the file at `path` to `new_size` bytes, zero-filling any growth.
 *
 * Returns 0 on success, -1 if the path is not a regular file or there is no
 * space to grow it.
 *
 * A relocation cannot reach a descriptor through this call: growth is the only
 * thing `krffs_resize_reserved_node` ever moves a node for, and every caller in
 * the tree truncates to zero (the O_TRUNC and creat paths, and the debug
 * shell). A shrink returns the same node, so the holders the syscall layer
 * tracks stay valid. A future caller that grows a file here would have to
 * repoint them, the way the write path does.
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

    if ((uint64_t)new_size > old_data_size) {
        void *base = node + 1;
        char *dest = (char *)base + old_data_size;
        uint32_t fill_size = new_size - (uint32_t)old_data_size;
        k_memset(dest, 0, fill_size);
    }
    krffs_stamp(node, KRFFS_STAMP_DATA);

    return 0;
}

/**
 * Remove the regular file at `path`.
 *
 * Returns 0 on success, -1 if the path is not a regular file (directories are
 * removed by krffs_rmdir).
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
        return -1;
    }

    /* Stamped before the node goes: its parent is read from it. */
    krffs_stamp(krffs_parent_node(node), KRFFS_STAMP_DATA);
    krffs_remove_reserved_node(krffs_fs, node);
    return 0;
}

/**
 * Take `node` out of the namespace without freeing it.
 *
 * Clears the name, which is what makes it unreachable: `krffs_walk` never hands
 * `krffs_find_child` a component shorter than one character, so a node with an
 * empty name cannot be matched by any path. That is a property of the parser,
 * not of a hash, which is why an empty name is used and not a sentinel
 * `parent_id`: `krffs_mix_id` may return any nonzero value, so a reserved id
 * would rest on a collision that cannot be ruled out. The node keeps its type,
 * its data and its parent id, so it is still a live file for whoever has a
 * descriptor on it, and still an ordinary reserved node to the allocator.
 *
 * The scans that enumerate by `parent_id`, not by name skip an empty
 * name for the same reason, since being unmatchable covers the resolver and
 * nothing else. The caller frees the node when the last descriptor closes.
 */
void krffs_orphan_node(krffs_node_t *node)
{
    if (node && node != krffs_fs->node) {
        node->name[0] = '\0';
    }
}

/**
 * Remove a name whose file somebody still holds open, and stamp the directory
 * it left.
 *
 * The stamp belongs here, not at the call sites, because there are two
 * of them (the `unlink` syscall and the rescue shell's own `rm`) and that is
 * precisely how one of them ends up not stamping. Removing a name changes the
 * directory whether or not the file it named goes with it.
 */
void krffs_unlink_held(krffs_node_t *node)
{
    krffs_stamp(krffs_parent_node(node), KRFFS_STAMP_DATA);
    krffs_orphan_node(node);
}

/**
 * Free every orphan left on the medium by a crash. Returns how many it reaped.
 *
 * An orphan is a reserved node no path names, kept alive only by the
 * descriptors holding it, and freed by the last close. A crash between the two
 * leaves one on a persistent image with nobody left to free it: the descriptors
 * died with the machine. At mount there are no descriptors at all, so a
 * nameless reserved node is unambiguously garbage and reaping it needs no
 * bookkeeping to consult.
 *
 * Without this the space is lost for good, since nothing else in the tree
 * looks for a node with no name, and a machine that crashed often enough
 * during a credential rewrite would fill its disk with files no path could
 * reach.
 */
uint32_t krffs_reap_orphans(void)
{
    krffs_node_t *node;
    krffs_node_t *previous;
    uint32_t reaped = 0;
    uint64_t steps = 0;
    uint64_t max_steps;

    if (!krffs_fs) {
        return 0;
    }
    /* Bounded so a malformed chain cannot spin, and derived from the image the
     * way the mount validator derives its own: a fixed cap would silently stop
     * short of an orphan on a large disk (the shipped 48 MB partition has far
     * more than any round number one would pick) and leak it on every boot. */
    max_steps = krffs_fs->size / sizeof(krffs_node_t) + 1;
    /* One bounded pass. Removing a node coalesces it with whichever of its
     * neighbours are free, so the merged region begins at the previous node
     * when that was free and at this node otherwise; either way it is a free
     * node, which the next iteration steps over by its (now larger) size. That
     * is what lets this resume in place instead of restarting at the root after
     * each removal, which would be a scan per orphan and, on a disk larger than
     * the page cache, a great many polled reads at boot. */
    previous = krffs_fs->node;
    node = krffs_get_next_node(krffs_fs, previous);
    while (node && node != krffs_fs->node && steps < max_steps) {
        steps++;
        if (node->type == KRFFS_RESERVED_NODE && node->name[0] == '\0') {
            krffs_node_t *merged =
                (previous->type == KRFFS_FREE_NODE) ? previous : node;

            krffs_remove_reserved_node(krffs_fs, node);
            reaped++;
            node = merged;          /* free; the step below advances past it */
        }
        previous = node;
        node = krffs_get_next_node(krffs_fs, node);
    }
    return reaped;
}

/**
 * Rename or move the regular file `old_path` to `new_path`.
 *
 * The destination directory must exist; an existing destination *file* is
 * replaced. Renaming a directory source is rejected, and so is overwriting a
 * directory destination: a directory's children reference its id, so removing
 * it to make room would orphan the whole subtree. Returns 0 on success, -1 on
 * error.
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
        return -1;
    }

    /* Renaming a file to the name it already has does nothing at all, which is
     * what POSIX requires: no removal, no relink, and no timestamp moves. */
    existing = krffs_walk(krffs_fs, new_path);
    if (existing == node) {
        return 0;
    }

    /* Replace any existing destination, but never clobber a directory: freeing
     * it would orphan its children. */
    if (existing) {
        if (KRFFS_IS_DIR(existing->mode)) {
            return -1;
        }
        krffs_remove_reserved_node(krffs_fs, existing);
    }

    /* Both ends change: the name left one directory and arrived in another,
     * and they are the same directory in the ordinary case. The node itself
     * kept its contents, so only its attribute-change time moves. */
    krffs_stamp(krffs_parent_node(node), KRFFS_STAMP_DATA);
    krffs_set_reserved_node_name(node, base);
    node->parent_id = parent_id;
    krffs_stamp(krffs_parent_node(node), KRFFS_STAMP_DATA);
    krffs_stamp(node, KRFFS_STAMP_ATTR);
    return 0;
}

/**
 * Create a directory at `path`.
 *
 * Returns 0 on success, -1 if the parent directory is missing or the name is
 * already taken.
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
        return -1;
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

    /* A directory has its name, the S_IFDIR mode, an empty body, and a
     * unique id (mix of parent and name) that its children reference as their
     * parent_id. */
    _krffs_strncpy((char *)node->name, base, KRFFS_FILE_NAME_BUFFER_SIZE);
    node->name[KRFFS_FILE_NAME_BUFFER_SIZE - 1] = '\0';
    node->parent_id = parent_id;
    node->id = krffs_mix_id(parent_id, base);
    node->mode = (uint16_t)(KRFFS_S_IFDIR | 0755);
    node->uid = 0;
    node->gid = 0;
    krffs_stamp(node, KRFFS_STAMP_DATA);
    node->access_time = node->modification_time;
    krffs_stamp(krffs_parent_node(node), KRFFS_STAMP_DATA);
    return 0;
}

/**
 * Remove the empty directory at `path`.
 *
 * Returns 0, or -1 if `path` is not a directory or still has children.
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

    /* A directory is empty only if no node names it as a parent. An orphan does
     * not count: it keeps the parent id it had, but no path reaches it, so a
     * directory holding only orphans is empty as far as any caller can tell and
     * rmdir must succeed the way POSIX says. */
    scan = krffs_fs->node;
    do {
        if (scan->type == KRFFS_RESERVED_NODE && scan->parent_id == node->id &&
            scan->name[0] != '\0') {
            return -1;
        }
    } while ((scan = krffs_get_next_node(krffs_fs, scan)) != krffs_fs->node);

    krffs_stamp(krffs_parent_node(node), KRFFS_STAMP_DATA);
    krffs_remove_reserved_node(krffs_fs, node);
    return 0;
}

/**
 * Return the `index`-th child of the directory at `path`, for getdents.
 *
 * Copies the child name into `name_out` (NUL-terminated, up to buf_size) and
 * reports through is_dir_out whether the child is itself a directory. Returns 1
 * if a child was returned, 0 past the end, -1 on error. Each call rescans the
 * whole node list for the index, a teaching simplification.
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
        return -1;
    }
    dir_id = (node == krffs_fs->node) ? KRFFS_ROOT_ID : node->id;

    scan = krffs_fs->node;
    do {
        if (scan->type == KRFFS_RESERVED_NODE && scan->parent_id == dir_id &&
            scan->name[0] != '\0') {
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

    return 0;
}

/**
 * Mount a KRFFS image of `ramdisk_size` bytes whose first byte is at
 * `ramdisk_base`.
 *
 * The base is wherever the image is addressable: the RAM-disk module in
 * place, or PCWIN_BASE when the page cache demand-maps the module stub
 * (rach2.fswindow) or a real ATA partition (rach2.fsdisk). The parameter
 * names are historical; the file-system code just pointer-walks the base it
 * is handed. The first node is the root: it must carry
 * KRFFS_FILE_SYSTEM_MAGIC and be of type KRFFS_ROOT_NODE. On success the
 * global krffs_fs points at the mounted file system. Returns 0 on success,
 * -1 if the image is too small or malformed.
 */
/**
 * Walk the whole circular node chain once, bounding every step.
 *
 * The walkers trust each node's `size` to advance, so a malformed or
 * partially written image (a node whose size overshoots the region, is below
 * a header, or a chain that never closes) would drive them out of bounds or
 * into an infinite loop. Validate the structure before publishing it: each
 * node sits within the region with a header that fits, has the magic and
 * a known type, has a size of at least one header that does not run past the
 * end, and a backward link consistent with the previous node; the sizes must
 * tile the region exactly and the chain must close back on the root. Returns
 * 1 if sound, 0 if not.
 */
static int krffs_chain_is_valid(krffs_node_t *root, uint32_t size)
{
    uint64_t base = (uint64_t)(uintptr_t)root;
    uint64_t limit = base + size;
    uint64_t pos = base;
    uint64_t prev_size = 0;
    uint64_t total = 0;
    uint32_t max_nodes = size / (uint32_t)sizeof(krffs_node_t) + 1;
    uint32_t count = 0;

    for (;;) {
        krffs_node_t *n = (krffs_node_t *)(uintptr_t)pos;

        if (++count > max_nodes) {
            return 0; /* a cyclic or non-closing chain */
        }
        /* pos stays in [base, limit] (it starts at base and only advances by a
         * size already bounded below), so limit - pos never underflows. Compare
         * by subtraction so a hostile 64-bit size cannot wrap pos past limit.
         */
        if (limit - pos < sizeof(krffs_node_t)) {
            return 0;
        }
        if (n->magic != KRFFS_FILE_SYSTEM_MAGIC) {
            return 0;
        }
        if (n->type != KRFFS_ROOT_NODE && n->type != KRFFS_FREE_NODE &&
            n->type != KRFFS_RESERVED_NODE) {
            return 0;
        }
        if (n->size < sizeof(krffs_node_t) || n->size > limit - pos) {
            return 0;
        }
        if (count > 1 && n->previous_node_size != prev_size) {
            return 0;
        }
        total += n->size;
        prev_size = n->size;
        pos += n->size;
        if (pos == limit) {
            break; /* the last node ends at the region edge; the list wraps here */
        }
    }
    if (total != (uint64_t)size || root->previous_node_size != prev_size) {
        return 0;
    }
    return 1;
}

int init_krffs(void *ramdisk_base, uint32_t ramdisk_size)
{
    krffs_node_t *root;

    if (!ramdisk_base || ramdisk_size < sizeof(krffs_node_t)) {
        serial_printf("KRFFS: Invalid ramdisk (base=%p size=%d)\n",
                     ramdisk_base, ramdisk_size);
        return -1;
    }

    root = (krffs_node_t *)ramdisk_base;

    if (root->magic != KRFFS_FILE_SYSTEM_MAGIC) {
        serial_printf("KRFFS: Invalid magic 0x%X (expected 0x%X)\n",
                     root->magic, KRFFS_FILE_SYSTEM_MAGIC);
        return -1;
    }
    if (root->type != KRFFS_ROOT_NODE) {
        serial_printf("KRFFS: First node is not root (type=%d)\n", root->type);
        return -1;
    }
    if (!krffs_chain_is_valid(root, ramdisk_size)) {
        serial_printf("KRFFS: node chain is malformed; refusing to mount\n");
        return -1;
    }

    _krffs_fs_instance.node = root;
    _krffs_fs_instance.size = ramdisk_size;
    krffs_fs = &_krffs_fs_instance;

    return 0;
}
