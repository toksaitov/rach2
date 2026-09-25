/*
 * KRFFS: a K&R first-fit file system.
 *
 * The medium is one contiguous region holding a circular doubly linked list
 * of nodes. Each node has a header (krffs_node_t) followed by its data;
 * a node is either free or reserved (a file or directory). Allocation is
 * first-fit with split-and-coalesce, after the storage allocator in The C
 * Programming Language. Directories are hierarchical (the v2 parent_id
 * scheme described below).
 *
 * The format is addressed in offsets from the first byte of the region and
 * reached through a backend, so one implementation serves an image in memory
 * and an image on a device. The order in which a change is written is what
 * lets a mount survive a machine that stops in the middle of one, and it is
 * written down in krffs.Readme.md.
 *
 * The root file system keeps a pointer interface over the memory backend: the
 * descriptor table, the syscall layer, the orphan scan and the dirty bit hold
 * krffs_node_t pointers into the mapped image, and the functions they call are
 * the facade at the end of this file.
 */

#include "../klog.h"
#include "krffs.h"

#include "../rtc.h"
#include "../serial.h"
#include "../lib/kerrno.h"

krffs_file_system_t *krffs_fs = NULL;

static krffs_file_system_t _krffs_fs_instance;

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

/*
 * The memory backend: the image is mapped and one memcpy reaches it.
 *
 * A root is mounted this way, whether it is a Multiboot module in place or a
 * disk region the page-cache window maps. The window is demand-paged, so a
 * copy through it can take a fault that the page cache fills; what it cannot
 * do is take a fault the kernel has no handler for.
 */

static int krffs_memory_read(void *ctx, uint64_t offset, void *buffer, uint32_t size)
{
    _krffs_memcpy(buffer, (const char *)ctx + offset, size);
    return 0;
}

static int krffs_memory_write(void *ctx, uint64_t offset, const void *buffer, uint32_t size)
{
    _krffs_memcpy((char *)ctx + offset, buffer, size);
    return 0;
}

static int krffs_memory_sync(void *ctx)
{
    (void)ctx;
    return 0;
}

static const krffs_backend_t krffs_memory_backend = {krffs_memory_read, krffs_memory_write,
                                                     krffs_memory_sync};

/* The root's mount, and the mount every facade function below works on. */
static krffs_mount_t krffs_root_mount;

/**
 * Read a node's header at `offset` into `out`.
 *
 * Returns 0, or -1 if the header would not fit inside the region, which is
 * what a walk that has left the ring looks like. Every reader goes through
 * this: a node on a device is a header the backend must fetch, not an address.
 */
int krffs_head(krffs_mount_t *mount, uint64_t offset, krffs_node_t *out)
{
    if (!mount || !out || offset > mount->size || mount->size - offset < sizeof(krffs_node_t)) {
        return -1;
    }
    return mount->backend->read(mount->ctx, offset, out, (uint32_t)sizeof(krffs_node_t));
}

/**
 * Read the fields a walk needs from the node at `offset`.
 *
 * A node's header is 357 bytes and 256 of them are its name, which a walk
 * needs only for the nodes it is about to match. Reading the rest is two
 * fetches of ninety-nine bytes together, and the walks below are the root's
 * hot path: a lookup reads every node in the ring.
 *
 * Returns 0, or -1 if the header would not fit inside the region.
 */
static int krffs_brief(krffs_mount_t *mount, uint64_t offset, krffs_brief_t *out)
{
    if (!mount || !out || offset > mount->size || mount->size - offset < sizeof(krffs_node_t)) {
        return -1;
    }
    if (mount->backend->read(mount->ctx, offset + KRFFS_FIELD_TYPE, &out->type,
                             (uint32_t)(KRFFS_FIELD_NAME - KRFFS_FIELD_TYPE)) != 0) {
        return -1;
    }
    return mount->backend->read(mount->ctx, offset + KRFFS_FIELD_MODE, &out->mode,
                                (uint32_t)(sizeof(krffs_node_t) - KRFFS_FIELD_MODE));
}

/**
 * Compare a node's name with comp[0..len), reading only what it takes.
 *
 * Returns 1 if the name is exactly that component. The byte after it must be
 * the terminator, so len + 1 bytes decide it.
 */
static int krffs_name_is(krffs_mount_t *mount, uint64_t offset, const char *comp, uint32_t len)
{
    uint8_t name[KRFFS_FILE_NAME_BUFFER_SIZE];
    uint32_t i;

    if (len + 1 > KRFFS_FILE_NAME_BUFFER_SIZE) {
        return 0;
    }
    if (mount->backend->read(mount->ctx, offset + KRFFS_FIELD_NAME, name, len + 1) != 0) {
        return 0;
    }
    for (i = 0; i < len; i++) {
        if (name[i] != (uint8_t)comp[i]) {
            return 0;
        }
    }
    return name[len] == '\0';
}

/**
 * Whether the node at `offset` has a name at all: an orphan has none.
 */
static int krffs_has_name(krffs_mount_t *mount, uint64_t offset)
{
    uint8_t first = 0;

    if (mount->backend->read(mount->ctx, offset + KRFFS_FIELD_NAME, &first, 1) != 0) {
        /* A name the medium would not give up is not a name that is not
         * there: the callers free such a node, refuse to remove a directory
         * over it, or count it, and a read that failed must not be the
         * reason a file is freed. */
        return 1;
    }
    return first != '\0';
}

/**
 * Write `size` bytes into the field at `field` of the node at `offset`.
 *
 * One call publishes one field. What may be written before what is the
 * update protocol, and the callers below keep it.
 */
/**
 * Whether the bytes this write changes all fall in one unit of the medium.
 *
 * A backend that puts whole blocks on the medium writes a field that spans
 * two of them in two pieces, and a stop between them leaves half of each.
 * Only the bytes that change matter: a length whose top four bytes are zero
 * before and after is written in two pieces that say the same thing in one
 * of them, and that is a write like any other.
 */
static int krffs_write_is_whole(krffs_mount_t *mount, uint64_t at, const void *value, uint32_t size)
{
    uint8_t old[16];
    const uint8_t *want = value;
    /* A region is addressed in 32 bits and this kernel has no 64-bit
     * division, so the unit a byte falls in is counted in 32. */
    uint32_t base = (uint32_t)at;
    uint32_t first;
    uint32_t last;
    uint32_t i;

    if (mount->grain == 0 || size <= 1 || size > sizeof(old)) {
        return 1; /* a byte cannot be torn, and a name is not published here */
    }
    if (base / mount->grain == (base + size - 1) / mount->grain) {
        return 1; /* the whole field is in one unit */
    }
    if (mount->backend->read(mount->ctx, at, old, size) != 0) {
        return 0;
    }
    first = size;
    last = 0;
    for (i = 0; i < size; i++) {
        if (old[i] != want[i]) {
            if (first == size) {
                first = i;
            }
            last = i;
        }
    }
    if (first == size) {
        return 1; /* nothing changes */
    }
    return (base + first) / mount->grain == (base + last) / mount->grain;
}

static int krffs_put(krffs_mount_t *mount, uint64_t offset, uint32_t field, const void *value,
                     uint32_t size)
{
    uint64_t at = offset + field;

    if (!krffs_write_is_whole(mount, at, value, size)) {
        klog(KLOG_WARN, "krffs", "refused: a field at %d would reach the medium in two pieces",
             (int)at);
        return -1;
    }
    return mount->backend->write(mount->ctx, at, value, size);
}

static int krffs_put_u64(krffs_mount_t *mount, uint64_t offset, uint32_t field, uint64_t value)
{
    return krffs_put(mount, offset, field, &value, (uint32_t)sizeof(value));
}

/**
 * A 64-bit field of a node no path reaches yet: a node being made, which
 * nothing reads until its name is written last, after a sync point, or a
 * copy being filled for a move, a free node until its type is written last.
 * A field torn across two units of the medium is seen by nobody, so the
 * whole-unit rule that guards a live node does not apply. Without this a
 * name whose hash happened to land astride a unit could not be created.
 */
static int krffs_put_new_u64(krffs_mount_t *mount, uint64_t offset, uint32_t field, uint64_t value)
{
    return mount->backend->write(mount->ctx, offset + field, &value, (uint32_t)sizeof(value));
}

static int krffs_put_u32(krffs_mount_t *mount, uint64_t offset, uint32_t field, uint32_t value)
{
    return krffs_put(mount, offset, field, &value, (uint32_t)sizeof(value));
}

static int krffs_put_u16(krffs_mount_t *mount, uint64_t offset, uint32_t field, uint16_t value)
{
    return krffs_put(mount, offset, field, &value, (uint32_t)sizeof(value));
}

static int krffs_put_u8(krffs_mount_t *mount, uint64_t offset, uint32_t field, uint8_t value)
{
    return krffs_put(mount, offset, field, &value, (uint32_t)sizeof(value));
}

/**
 * Read and write a node's data, `at` bytes into it.
 */
static int krffs_data_read(krffs_mount_t *mount, uint64_t offset, uint64_t at, void *buffer,
                           uint32_t size)
{
    return mount->backend->read(mount->ctx, offset + sizeof(krffs_node_t) + at, buffer, size);
}

static int krffs_data_write(krffs_mount_t *mount, uint64_t offset, uint64_t at, const void *buffer,
                            uint32_t size)
{
    return mount->backend->write(mount->ctx, offset + sizeof(krffs_node_t) + at, buffer, size);
}

/**
 * Fill `size` bytes of a node's data with `value`, `at` bytes into it.
 *
 * A hole left by a write past the end, and the growth a truncate makes, are
 * both zeroes nobody wrote, and the medium has whatever the space held before.
 */
static int krffs_data_fill(krffs_mount_t *mount, uint64_t offset, uint64_t at, uint8_t value,
                           uint64_t size)
{
    uint8_t block[64];
    uint32_t i;

    for (i = 0; i < sizeof(block); i++) {
        block[i] = value;
    }
    while (size > 0) {
        uint32_t chunk = size > sizeof(block) ? (uint32_t)sizeof(block) : (uint32_t)size;
        if (krffs_data_write(mount, offset, at, block, chunk) != 0) {
            return -1;
        }
        at += chunk;
        size -= chunk;
    }
    return 0;
}

/**
 * Make everything written so far reach the medium.
 *
 * The protocol's order is an order on the medium, not in a cache, so a write
 * that must be there before the field that publishes it is followed by this.
 * On a memory backend it does nothing, which is why a root pays nothing for
 * it. Answers 0, or -1 if the medium refused.
 */
static int krffs_sync_point(krffs_mount_t *mount)
{
    return mount->backend->sync(mount->ctx);
}

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
 * Fold a position back into the region.
 *
 * The node list is circular: walking off either end of [0, mount->size) folds
 * the position by one region size so it lands on the other end.
 */
static uint64_t krffs_wrap_offset(krffs_mount_t *mount, uint64_t position)
{
    if (position >= mount->size) {
        position -= mount->size;
    }
    return position;
}

/**
 * The offset of the node following the one whose header is `head`.
 *
 * A node smaller than its own header would not advance (an unbounded walk) or
 * fold to a bad position. The mount-time pass rejects such an image, so this
 * is a runtime backstop: report the root so a walk that stops at the root
 * stops cleanly instead of looping.
 */
static uint64_t krffs_step(krffs_mount_t *mount, uint64_t offset, uint64_t size)
{
    if (size < sizeof(krffs_node_t)) {
        return 0;
    }
    return krffs_wrap_offset(mount, offset + size);
}

uint64_t krffs_next_offset(krffs_mount_t *mount, uint64_t offset, const krffs_node_t *head)
{
    return krffs_step(mount, offset, head->size);
}

/**
 * The offset of the node before the one whose header is `head`.
 */
static uint64_t krffs_previous_offset(krffs_mount_t *mount, uint64_t offset,
                                      const krffs_node_t *head)
{
    if (head->previous_node_size > offset) {
        return mount->size - (head->previous_node_size - offset);
    }
    return offset - head->previous_node_size;
}

/**
 * The most nodes a region can hold, which bounds every walk.
 *
 * Derived from the image the way the mount check derives its own: a fixed cap
 * would stop short of a node on a large volume.
 */
uint64_t krffs_max_steps(krffs_mount_t *mount)
{
    return mount->size / (uint32_t)sizeof(krffs_node_t) + 1;
}

/*
 * Hierarchical path resolution (the KRFFS v2 on-disk format).
 *
 * A node's parent_id is its directory's logical id (KRFFS_ROOT_ID == 0 for
 * the root, whose own node is the first one and has no parent reference). A
 * directory node's id is mix(parent_id, name), so children reference it.
 * Two different names can mix to one id, since the mix goes through the
 * name's DJB hash, which two names can share, and two directories with one id
 * would share their children; mkdir looks for the id before it makes the
 * directory and refuses the second name. Resolution matches each path
 * component by (name, parent_id), descending only into directories. Regular
 * files' ids are never referenced, so they keep the legacy DJB(name) value
 * for compatibility.
 */

/**
 * Combine a parent directory id and a stored name hash into a node id.
 *
 * FNV-1a style mixing over the name's DJB hash. Never returns 0, which is
 * reserved for the root's logical id (KRFFS_ROOT_ID). The name goes in as its
 * 64-bit DJB hash, and two names can share that hash (`bC` and `cb` do), so
 * two names can give one id; `krffs_id_taken` is what keeps two directories
 * from holding it at once.
 */
static uint64_t krffs_mix_hash(uint64_t parent_id, uint64_t hash)
{
    uint64_t h = (parent_id + 1) * 1099511628211ULL;

    h ^= hash;
    h *= 1099511628211ULL;
    return h ? h : 1;
}

/** Mix a live name by the same rule as its stored hash. */
static uint64_t krffs_mix_id(uint64_t parent_id, const char *name)
{
    return krffs_mix_hash(parent_id, krffs_calculate_djb_hash((const uint8_t *)name));
}

/**
 * Return the inode number that `stat` and `getdents` report for a node.
 *
 * The root is 1, as the VFS reports it; it is recognised by its node type,
 * since its stored mode need not have the directory bits. A directory reports
 * its id. A file reports its parent's id mixed with its stored id, which is
 * the hash of its name, and not with the name itself: unlink clears the name
 * of a file that is still open and leaves the id, and the file's number must
 * not change while it is open.
 */
uint64_t krffs_node_ino(const krffs_node_t *node)
{
    if (node->type == KRFFS_ROOT_NODE) {
        return 1;
    }
    if (KRFFS_IS_DIR(node->mode)) {
        return node->id;
    }
    return krffs_mix_hash(node->parent_id, node->id);
}

/**
 * Find the reserved node with logical id `id`, for ".." and directory descent.
 *
 * Answers KRFFS_NO_NODE for KRFFS_ROOT_ID: the root is not a reserved node
 * (it is the first node), so callers must handle the root themselves rather
 * than expect a lookup here.
 */
static uint64_t krffs_find_offset_by_id(krffs_mount_t *mount, uint64_t id)
{
    uint64_t offset = 0;
    uint64_t steps = 0;
    uint64_t bound = krffs_max_steps(mount);
    krffs_brief_t brief;

    if (id == KRFFS_ROOT_ID) {
        return KRFFS_NO_NODE;
    }
    do {
        if (krffs_brief(mount, offset, &brief) != 0) {
            return KRFFS_NO_NODE;
        }
        if (brief.type == KRFFS_RESERVED_NODE && brief.id == id) {
            return offset;
        }
        offset = krffs_step(mount, offset, brief.size);
    } while (offset != 0 && ++steps < bound);

    return KRFFS_NO_NODE;
}

/**
 * Find the child named comp[0..len) directly inside directory `dir_id`.
 */
static uint64_t krffs_find_child(krffs_mount_t *mount, uint64_t dir_id, const char *comp,
                                 uint32_t len)
{
    uint64_t offset = 0;
    uint64_t steps = 0;
    uint64_t bound = krffs_max_steps(mount);
    krffs_brief_t brief;

    do {
        if (krffs_brief(mount, offset, &brief) != 0) {
            return KRFFS_NO_NODE;
        }
        if (brief.type == KRFFS_RESERVED_NODE && brief.parent_id == dir_id &&
            krffs_name_is(mount, offset, comp, len)) {
            return offset;
        }
        offset = krffs_step(mount, offset, brief.size);
    } while (offset != 0 && ++steps < bound);

    return KRFFS_NO_NODE;
}

/**
 * The child of the directory at `dir` named `name`, one step of a walk.
 *
 * `dir` is an offset, and offset 0 is the root directory. Answers
 * KRFFS_NO_NODE when there is no such name.
 */
uint64_t krffs_child_at(krffs_mount_t *mount, uint64_t dir, const char *name)
{
    uint64_t id = KRFFS_ROOT_ID;
    uint32_t len = 0;
    krffs_brief_t brief;

    if (!name) {
        return KRFFS_NO_NODE;
    }
    while (name[len] != '\0') {
        len++;
    }
    if (dir != 0 && dir != KRFFS_NO_NODE) {
        if (krffs_brief(mount, dir, &brief) != 0) {
            return KRFFS_NO_NODE;
        }
        id = brief.id;
    }
    return krffs_find_child(mount, id, name, len);
}

/**
 * Resolve `path` to a node offset, honoring '.' and '..'.
 *
 * "/" (or "") resolves to the root, which is offset 0. A non-final component
 * that is not a directory, or any missing component, fails with
 * KRFFS_NO_NODE. When `err` is non-NULL it receives -ENOENT for a missing
 * component or -ENOTDIR for a non-directory in a non-final position. Search
 * permission is the mount table's to check (vfs.c); the walk here charges
 * nothing.
 */
uint64_t krffs_walk_err(krffs_mount_t *mount, const char *path, int *err)
{
    uint64_t cur_id = KRFFS_ROOT_ID;
    uint64_t offset = 0; /* current directory; the root is 0 */
    const char *p = path ? path : "";
    krffs_brief_t brief;

    if (err) {
        *err = 0;
    }

    while (*p == '/') {
        p++;
    }
    if (*p == '\0') {
        return 0;
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
            /* "." stays in the current directory */
        } else if (len == 2 && start[0] == '.' && start[1] == '.') {
            /* A lookup by id cannot answer the root (it owns reserved id 0),
             * so resolve a root parent to offset 0 directly; otherwise ".."
             * back to the root would fail. The continuing id is ROOT_ID so the
             * root's children (parent_id == 0) still resolve after the "..".
             */
            uint64_t parent = KRFFS_ROOT_ID;

            if (offset != 0 && krffs_brief(mount, offset, &brief) == 0) {
                parent = brief.parent_id;
            }
            if (parent == (uint64_t)KRFFS_ROOT_ID) {
                offset = 0;
                cur_id = (uint64_t)KRFFS_ROOT_ID;
            } else {
                offset = krffs_find_offset_by_id(mount, parent);
                if (offset == KRFFS_NO_NODE) {
                    offset = 0;
                    cur_id = (uint64_t)KRFFS_ROOT_ID;
                } else if (krffs_brief(mount, offset, &brief) == 0) {
                    cur_id = brief.id;
                } else {
                    cur_id = (uint64_t)KRFFS_ROOT_ID;
                }
            }
        } else {
            offset = krffs_find_child(mount, cur_id, start, len);
            if (offset == KRFFS_NO_NODE) {
                if (err) {
                    *err = -ENOENT;
                }
                return KRFFS_NO_NODE;
            }
            if (krffs_brief(mount, offset, &brief) != 0) {
                if (err) {
                    *err = -ENOENT;
                }
                return KRFFS_NO_NODE;
            }
            if (!last && !KRFFS_IS_DIR(brief.mode)) {
                if (err) {
                    *err = -ENOTDIR;
                }
                return KRFFS_NO_NODE;
            }
            cur_id = brief.id;
        }

        if (last) {
            return offset;
        }
    }
}

/**
 * Resolve `path` to an offset, or KRFFS_NO_NODE, with no error distinction.
 */
uint64_t krffs_walk(krffs_mount_t *mount, const char *path)
{
    return krffs_walk_err(mount, path, (int *)0);
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

    for (i = 0; i + 1 < KRFFS_FILE_NAME_BUFFER_SIZE && comp[i] != '\0' && comp[i] != '/'; i++) {
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
static uint64_t krffs_resolve_dir_id(krffs_mount_t *mount, const char *path)
{
    uint64_t offset = krffs_walk(mount, path);
    krffs_brief_t brief;

    if (offset == KRFFS_NO_NODE) {
        return (uint64_t)-1;
    }
    if (offset == 0) {
        return KRFFS_ROOT_ID;
    }
    if (krffs_brief(mount, offset, &brief) != 0 || !KRFFS_IS_DIR(brief.mode)) {
        return (uint64_t)-1;
    }
    return brief.id;
}

/**
 * Build the absolute, canonical path of the node at `offset` into `buf`.
 *
 * Returns 0, or -1 if the path does not fit or a parent link is dangling.
 * The ancestors are collected as offsets and their names read one at a time,
 * since a name is 256 bytes and sixty-four of them are not a stack.
 */
int krffs_node_path_at(krffs_mount_t *mount, uint64_t offset, char *buf, uint32_t size)
{
    uint64_t chain[KRFFS_PATH_MAX_DEPTH];
    int depth = 0;
    int i;
    uint32_t pos = 0;
    uint64_t cur = offset;
    krffs_brief_t brief;
    krffs_node_t head;

    if (!buf || size == 0 || offset == KRFFS_NO_NODE) {
        return -1;
    }

    /* Collect the ancestors from the node up toward the root. A lookup by id
     * cannot answer the root (it has the reserved id 0), so stop as soon as a
     * node's parent IS the root, rather than try to look the root up. */
    for (;;) {
        if (cur == 0) {
            break;
        }
        if (krffs_brief(mount, cur, &brief) != 0) {
            return -1;
        }
        if (brief.id == (uint64_t)KRFFS_ROOT_ID) {
            break;
        }
        if (depth >= KRFFS_PATH_MAX_DEPTH) {
            return -1;
        }
        chain[depth++] = cur;
        if (brief.parent_id == (uint64_t)KRFFS_ROOT_ID) {
            break;
        }
        cur = krffs_find_offset_by_id(mount, brief.parent_id);
        if (cur == KRFFS_NO_NODE) {
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
        uint32_t n;

        if (krffs_head(mount, chain[i], &head) != 0) {
            return -1;
        }
        for (n = 0; n < KRFFS_FILE_NAME_BUFFER_SIZE - 1 && head.name[n] != '\0'; n++) {
            /* bounded: the field is 256 bytes and need not end in one */
        }
        if (pos + 1 + n + 1 > size) {
            return -1; /* the path, including its NUL, would not fit */
        }
        buf[pos++] = '/';
        k_memcpy(buf + pos, head.name, n);
        pos += n;
    }
    buf[pos] = '\0';
    return 0;
}

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
void krffs_stamp_at(krffs_mount_t *mount, uint64_t offset, int what)
{
    krffs_node_time_t now;
    int64_t seconds = 0;
    int64_t nanoseconds = 0;

    if (offset == KRFFS_NO_NODE) {
        return;
    }
    rtc_realtime(&seconds, &nanoseconds);
    now.seconds = seconds;
    now.nanoseconds = nanoseconds;

    if (what == KRFFS_STAMP_DATA) {
        krffs_put(mount, offset, KRFFS_FIELD_MTIME, &now, (uint32_t)sizeof(now));
    }
    krffs_put(mount, offset, KRFFS_FIELD_CTIME, &now, (uint32_t)sizeof(now));
}

/**
 * The offset of the directory a node lives in.
 *
 * A parent id of `KRFFS_ROOT_ID` names the root, which is not in the node
 * table as a child of anything. Answers KRFFS_NO_NODE only when the chain is
 * broken, which is a corrupt image, not an ordinary case.
 */
uint64_t krffs_parent_offset(krffs_mount_t *mount, uint64_t offset)
{
    krffs_node_t head;

    if (offset == KRFFS_NO_NODE || krffs_head(mount, offset, &head) != 0) {
        return KRFFS_NO_NODE;
    }
    if (head.parent_id == KRFFS_ROOT_ID) {
        return 0;
    }
    return krffs_find_offset_by_id(mount, head.parent_id);
}

/**
 * Write a free node's whole header at `offset`.
 *
 * The header goes down in one write, before whatever size reveals it, which
 * is what makes a split or a merge survive a machine that stops between the
 * two.
 */
static int krffs_write_free_header(krffs_mount_t *mount, uint64_t offset, uint64_t size,
                                   uint64_t previous_node_size)
{
    krffs_node_t head;
    uint32_t i;

    for (i = 0; i < sizeof(head); i++) {
        ((uint8_t *)&head)[i] = 0;
    }
    head.magic = KRFFS_FILE_SYSTEM_MAGIC;
    head.type = KRFFS_FREE_NODE;
    head.size = size;
    head.previous_node_size = previous_node_size;
    head.data_size = 0;

    return mount->backend->write(mount->ctx, offset, &head, (uint32_t)sizeof(head));
}

/**
 * Copy `size` bytes of data from one node to another.
 */
static int krffs_copy_data(krffs_mount_t *mount, uint64_t to, uint64_t from, uint64_t size)
{
    uint8_t block[256];
    uint64_t at = 0;

    while (at < size) {
        uint32_t chunk =
            size - at > sizeof(block) ? (uint32_t)sizeof(block) : (uint32_t)(size - at);
        if (krffs_data_read(mount, from, at, block, chunk) != 0 ||
            krffs_data_write(mount, to, at, block, chunk) != 0) {
            return -1;
        }
        at += chunk;
    }
    return 0;
}

/**
 * Repair the backward link of the node following the one at `offset`.
 *
 * The forward chain is the truth and this link is derived from it, so every
 * operation that changes a size fixes it afterwards; a machine that stops
 * before this leaves the one fault the check at the next mount can rebuild.
 */
static void krffs_fix_backward_link(krffs_mount_t *mount, uint64_t offset, uint64_t size)
{
    krffs_put_u64(mount, krffs_wrap_offset(mount, offset + size), KRFFS_FIELD_PREVIOUS, size);
}

/**
 * Allocate a reserved node with room for `size` data bytes (K&R first-fit).
 *
 * Scans the list for the first free node large enough for the header plus
 * `size`. A larger free node is split, leaving the remainder free, unless the
 * remainder is too small to hold a header (then the whole node is taken). If
 * `original` is a node, its metadata and data are copied into the new one, as
 * the relocation half of a grow, and the new node keeps the original's data
 * length until the caller writes the rest. Answers the new node's offset, or
 * KRFFS_NO_NODE if no block fits.
 *
 * The split writes the tail's header before the size that reveals it, and the
 * node is published by its type, written last, so an interrupted allocation
 * leaves a chain a mount can walk. A node published without a name is one no
 * path reaches, and the check reclaims it; every caller names it next.
 */
uint64_t krffs_allocate_at(krffs_mount_t *mount, uint64_t size, uint64_t original)
{
    uint64_t size_with_header = sizeof(krffs_node_t) + size;
    uint64_t offset = 0;
    uint64_t steps = 0;
    uint64_t bound = krffs_max_steps(mount);
    krffs_brief_t brief;

    /* Bounded before the addition above can matter: nothing larger than the
     * region can be allocated, and a size near the top of the range would
     * otherwise wrap into a small one. */
    if (size > mount->size) {
        return KRFFS_NO_NODE;
    }

    do {
        if (krffs_brief(mount, offset, &brief) != 0) {
            return KRFFS_NO_NODE;
        }
        if (brief.type == KRFFS_FREE_NODE && brief.size >= size_with_header) {
            uint64_t data_size = size;

            if (brief.size > size_with_header) {
                uint64_t second_half_size = brief.size - size_with_header;

                /* Split only if the tail can hold a node header of its own;
                 * otherwise leave the slack inside this node. */
                if (second_half_size >= sizeof(krffs_node_t)) {
                    uint64_t tail = krffs_wrap_offset(mount, offset + size_with_header);

                    /* The tail's header must be on the medium before the size
                     * that reveals it. A backend that refused the header and
                     * accepted the size would leave the ring pointing at
                     * bytes nobody wrote, so the failure ends the
                     * allocation. */
                    if (krffs_write_free_header(mount, tail, second_half_size, size_with_header) !=
                            0 ||
                        krffs_sync_point(mount) != 0) {
                        return KRFFS_NO_NODE;
                    }
                    if (krffs_put_u64(mount, offset, KRFFS_FIELD_SIZE, size_with_header) != 0) {
                        return KRFFS_NO_NODE;
                    }
                    krffs_fix_backward_link(mount, tail, second_half_size);
                }
            }

            if (original != KRFFS_NO_NODE) {
                krffs_node_t from;
                int rc = 0;

                if (krffs_head(mount, original, &from) != 0) {
                    return KRFFS_NO_NODE;
                }
                rc |= krffs_put(mount, offset, KRFFS_FIELD_NAME, from.name,
                                KRFFS_FILE_NAME_BUFFER_SIZE);
                rc |= krffs_put_new_u64(mount, offset, KRFFS_FIELD_ID, from.id);
                /* keep its directory membership */
                rc |= krffs_put_new_u64(mount, offset, KRFFS_FIELD_PARENT, from.parent_id);
                rc |= krffs_put_u16(mount, offset, KRFFS_FIELD_MODE, from.mode);
                rc |= krffs_put_u32(mount, offset, KRFFS_FIELD_UID, from.uid);
                rc |= krffs_put_u32(mount, offset, KRFFS_FIELD_GID, from.gid);
                rc |= krffs_put(mount, offset, KRFFS_FIELD_ATIME, &from.access_time,
                                (uint32_t)sizeof(from.access_time));
                rc |= krffs_put(mount, offset, KRFFS_FIELD_MTIME, &from.modification_time,
                                (uint32_t)sizeof(from.modification_time));
                rc |= krffs_put(mount, offset, KRFFS_FIELD_CTIME, &from.attribute_change_time,
                                (uint32_t)sizeof(from.attribute_change_time));
                if (rc != 0) {
                    return KRFFS_NO_NODE;
                }
                if (from.data_size > 0 &&
                    krffs_copy_data(mount, offset, original, from.data_size) != 0) {
                    return KRFFS_NO_NODE;
                }
                data_size = from.data_size;
            }

            /* The type is the node, so everything the node is made of must
             * be on the medium before it: the bytes a move copied, the length
             * that counts them, and a name. The name written here is the
             * empty one, because a free node keeps whatever name it had when
             * it was freed and publishing that would bring a removed file
             * back; the caller writes the real name after, and a stop between
             * leaves a node no path reaches, which the check reclaims. */
            if (original == KRFFS_NO_NODE) {
                uint8_t empty = '\0';

                if (krffs_put(mount, offset, KRFFS_FIELD_NAME, &empty, 1) != 0) {
                    return KRFFS_NO_NODE;
                }
            }
            if (krffs_sync_point(mount) != 0) {
                return KRFFS_NO_NODE;
            }
            if (krffs_put_u64(mount, offset, KRFFS_FIELD_DATA_SIZE, data_size) != 0 ||
                krffs_sync_point(mount) != 0) {
                return KRFFS_NO_NODE;
            }
            if (krffs_put_u8(mount, offset, KRFFS_FIELD_TYPE, KRFFS_RESERVED_NODE) != 0 ||
                krffs_sync_point(mount) != 0) {
                return KRFFS_NO_NODE;
            }
            return offset;
        }
        offset = krffs_step(mount, offset, brief.size);
    } while (offset != 0 && ++steps < bound);

    return KRFFS_NO_NODE;
}

/**
 * Free a reserved node and coalesce it with adjacent free neighbours.
 *
 * Marks the node free, then merges forward with a free successor and backward
 * with a free predecessor, so the free list never has two adjacent free nodes.
 * When the merged extent changes size, the following node's back link is
 * repaired to point at the new size. One field publishes each merge, so a
 * machine that stops between them leaves a chain that can still be walked.
 */
int krffs_remove_at(krffs_mount_t *mount, uint64_t offset)
{
    krffs_node_t head;
    krffs_node_t neighbour;
    uint64_t previous = KRFFS_NO_NODE;
    uint64_t next;
    uint64_t size;
    int size_changed = 0;

    if (offset == KRFFS_NO_NODE || krffs_head(mount, offset, &head) != 0 ||
        head.type != KRFFS_RESERVED_NODE) {
        return -1;
    }

    /* The first node has no predecessor to merge with; the last has no
     * successor (next would wrap to the root). */
    if (offset != 0) {
        previous = krffs_previous_offset(mount, offset, &head);
    }
    next = krffs_next_offset(mount, offset, &head);
    size = head.size;

    if (krffs_put_u8(mount, offset, KRFFS_FIELD_TYPE, KRFFS_FREE_NODE) != 0) {
        return -1; /* still a reserved node, and still a sound chain */
    }

    if (next != 0 && krffs_head(mount, next, &neighbour) == 0 &&
        neighbour.type == KRFFS_FREE_NODE) {
        if (krffs_put_u64(mount, offset, KRFFS_FIELD_SIZE, size + neighbour.size) == 0) {
            size += neighbour.size;
            size_changed = 1;
        }
    }
    if (previous != KRFFS_NO_NODE && krffs_head(mount, previous, &neighbour) == 0 &&
        neighbour.type == KRFFS_FREE_NODE) {
        if (krffs_put_u64(mount, previous, KRFFS_FIELD_SIZE, size + neighbour.size) == 0) {
            size += neighbour.size;
            offset = previous;
            size_changed = 1;
        }
    }

    if (size_changed) {
        krffs_fix_backward_link(mount, offset, size);
    }
    return krffs_sync_point(mount);
}

/**
 * Resize a reserved node to hold `size` data bytes (K&R allocator).
 *
 * Growing extends into the adjacent free node when one is large enough;
 * otherwise the node is relocated to a fresh allocation and the old one freed,
 * so the answer may be a different offset. Shrinking returns the surplus to
 * the free list, coalescing with a following free node. In either direction a
 * remainder too small for a header is absorbed into the node rather than left
 * as an unusable fragment. Answers the node's offset, or KRFFS_NO_NODE if a
 * relocation could not find space.
 *
 * A grow changes the node's capacity and not its length: `data_size` stays
 * what it was, and the caller publishes the new length once the bytes are
 * written. A shrink publishes the shorter length first, since the bytes beyond
 * it stay inside the node until the size that frees them is written.
 */
uint64_t krffs_resize_at(krffs_mount_t *mount, uint64_t offset, uint64_t size)
{
    uint64_t size_with_header = sizeof(krffs_node_t) + size;
    krffs_node_t head;
    krffs_node_t next_head;
    uint64_t next;

    if (size > mount->size) {
        return KRFFS_NO_NODE;
    }
    if (offset == KRFFS_NO_NODE || krffs_head(mount, offset, &head) != 0 ||
        head.type != KRFFS_RESERVED_NODE) {
        return KRFFS_NO_NODE;
    }

    if (head.size == size_with_header) {
        return offset;
    }

    next = krffs_next_offset(mount, offset, &head);
    if (krffs_head(mount, next, &next_head) != 0) {
        return KRFFS_NO_NODE;
    }

    if (size_with_header > head.size) {
        uint64_t difference = size_with_header - head.size;

        if (next != 0 && next_head.type == KRFFS_FREE_NODE && next_head.size >= difference) {
            uint64_t whole = head.size + next_head.size;
            uint64_t tail_size = whole - size_with_header;

            /* The free node is taken whole first, and the surplus split back
             * off after. Writing the tail's header while the free node is
             * still in the chain would land on that node's own header
             * whenever the growth is shorter than a header, and a machine
             * stopping there would leave a chain with a torn node in it. Two
             * published sizes, each leaving a chain that can be walked, cost
             * one more write and have no such state. */
            if (krffs_put_u64(mount, offset, KRFFS_FIELD_SIZE, whole) != 0) {
                return KRFFS_NO_NODE;
            }
            krffs_fix_backward_link(mount, offset, whole);
            /* On the medium before the tail's header is written: that header
             * lands where the free node's own header was whenever the growth
             * is shorter than a header, and a chain still pointing at the old
             * one would then point at a torn node. */
            if (krffs_sync_point(mount) != 0) {
                return KRFFS_NO_NODE;
            }

            if (tail_size >= sizeof(krffs_node_t)) {
                uint64_t tail = krffs_wrap_offset(mount, offset + size_with_header);

                /* The node already holds what was asked. If the surplus
                 * cannot be written back as a free node, it stays inside the
                 * node: space nobody can allocate, and a sound chain, which
                 * is the better of the two answers. */
                if (krffs_write_free_header(mount, tail, tail_size, size_with_header) == 0 &&
                    krffs_sync_point(mount) == 0 &&
                    krffs_put_u64(mount, offset, KRFFS_FIELD_SIZE, size_with_header) == 0) {
                    krffs_fix_backward_link(mount, tail, tail_size);
                }
            }
            /* A surplus too small to hold a header of its own stays in the
             * node rather than becoming an unusable fragment. */
            return offset;
        }

        /* No adjacent room: relocate, then free the old node. */
        {
            uint64_t moved = krffs_allocate_at(mount, size, offset);

            /* The copy is on the medium before the node it replaces is
             * freed, which the allocation above has already seen to: it syncs
             * after the type that publishes a node. A stop between the two
             * leaves one name on two nodes, which the check resolves, and
             * never leaves the file with neither. */
            /* The old node must go: two nodes with one name is a state the
             * check resolves at the next mount, but a running kernel would
             * walk to the old one and read what the file used to be, so the
             * caller is told this did not work. Where the caller then sends
             * its holders is its own decision; the old offset may be inside a
             * free node by now. */
            if (moved != KRFFS_NO_NODE && krffs_remove_at(mount, offset) != 0) {
                return KRFFS_NO_NODE;
            }
            return moved;
        }
    }

    {
        uint64_t difference = head.size - size_with_header;

        /* Only a shorter file is published here. A resize that raises the
         * length inside the slack a node already has publishes nothing: the
         * caller writes the bytes and publishes the length after them, the
         * way a grow does. */
        /* On the medium before the surplus's header is written: that header
         * lands inside the bytes the length still on the medium counts, so a
         * stop with the header written and the length not would give the
         * file the allocator's bytes to read. */
        if (size < head.data_size &&
            (krffs_put_u64(mount, offset, KRFFS_FIELD_DATA_SIZE, size) != 0 ||
             krffs_sync_point(mount) != 0)) {
            return KRFFS_NO_NODE;
        }

        if (difference >= sizeof(krffs_node_t)) {
            uint64_t free_size = difference;
            uint64_t surplus = krffs_wrap_offset(mount, offset + size_with_header);

            if (next_head.type == KRFFS_FREE_NODE) {
                /* Merge the surplus with the following free node. */
                free_size += next_head.size;
            }
            if (krffs_write_free_header(mount, surplus, free_size, size_with_header) == 0 &&
                krffs_sync_point(mount) == 0 &&
                krffs_put_u64(mount, offset, KRFFS_FIELD_SIZE, size_with_header) == 0) {
                krffs_fix_backward_link(mount, surplus, free_size);
            }
        }
        /* A surplus too small to hold a header of its own stays in the node,
         * where a later shrink or a removal reclaims it. */
        return offset;
    }
}

/**
 * Read up to `size` bytes at `offset` into a node's data.
 *
 * Answers the number of bytes read (0 past the end of the file), or -1 on
 * error.
 */
int krffs_read_at(krffs_mount_t *mount, uint64_t node, char *buffer, uint32_t size, uint32_t at)
{
    krffs_node_t head;

    if (node == KRFFS_NO_NODE || !buffer || krffs_head(mount, node, &head) != 0) {
        return -1;
    }
    /* A handle that named a file whose node has since been freed reads
     * nothing rather than whatever the space now holds. */
    if (head.type != KRFFS_RESERVED_NODE) {
        return -1;
    }
    if ((uint64_t)at >= head.data_size) {
        return 0;
    }
    if ((uint64_t)at + size > head.data_size) {
        size = (uint32_t)(head.data_size - at);
    }
    if (krffs_data_read(mount, node, at, buffer, size) != 0) {
        return -1;
    }
    return (int)size;
}

/**
 * Write `size` bytes at `at` into a node, growing it when it must.
 *
 * `*node` is updated when a grow relocates the node. Answers the bytes
 * written, `KRFFS_EBAD` for an argument it cannot use, or `KRFFS_ENOSPACE`
 * when the file had to grow and the file system had no room.
 */
int krffs_write_at(krffs_mount_t *mount, uint64_t *node, const char *buffer, uint32_t size,
                   uint32_t at)
{
    krffs_node_t head;
    uint64_t offset;
    uint64_t old_data_size;
    uint64_t new_size;

    if (!node || *node == KRFFS_NO_NODE || !buffer || krffs_head(mount, *node, &head) != 0) {
        return KRFFS_EBAD;
    }
    offset = *node;
    /* The length before the write, which the hole below is measured from and
     * which a relocation carries to the node it answers with. */
    old_data_size = head.data_size;

    /* A write of nothing does nothing. POSIX says a successful zero-byte write
     * to a regular file has no other result, and without this the arithmetic
     * below would still grow the file to `at` and publish whatever the
     * allocator last left in those bytes. */
    if (size == 0) {
        return 0;
    }

    new_size = (uint64_t)at + size;

    if (new_size > head.size - sizeof(krffs_node_t)) {
        offset = krffs_resize_at(mount, offset, new_size);
        if (offset == KRFFS_NO_NODE) {
            return KRFFS_ENOSPACE;
        }
        *node = offset;
    }
    /* A write starting past the end leaves a hole, and the bytes under it are
     * whatever the free space held: another file's contents, until they are
     * cleared. `krffs_truncate` already zeroes what it grows; this is the same
     * rule for the other way a file grows. */
    if ((uint64_t)at > old_data_size &&
        krffs_data_fill(mount, offset, old_data_size, 0, (uint64_t)at - old_data_size) != 0) {
        return KRFFS_EBAD;
    }
    if (krffs_data_write(mount, offset, at, buffer, size) != 0 || krffs_sync_point(mount) != 0) {
        return KRFFS_EBAD;
    }
    if (krffs_head(mount, offset, &head) != 0) {
        return KRFFS_EBAD;
    }
    if (new_size > head.data_size &&
        krffs_put_u64(mount, offset, KRFFS_FIELD_DATA_SIZE, new_size) != 0) {
        return KRFFS_EBAD; /* the bytes are there and the length is not */
    }
    krffs_stamp_at(mount, offset, KRFFS_STAMP_DATA);
    return (int)size;
}

/**
 * Create an empty regular file at `path`.
 *
 * Idempotent like creat: an existing name (file or directory) returns success.
 * The parent directory must already exist. Returns 0 on success, -1 if the
 * parent is missing or no node could be allocated.
 */
int krffs_create_at(krffs_mount_t *mount, const char *path)
{
    uint64_t node;
    uint64_t parent_id;
    krffs_node_t head;
    char dir[KRFFS_FILE_NAME_BUFFER_SIZE];
    char base[KRFFS_FILE_NAME_BUFFER_SIZE];
    uint8_t name[KRFFS_FILE_NAME_BUFFER_SIZE];
    int rc = 0;

    if (krffs_walk(mount, path) != KRFFS_NO_NODE) {
        return 0;
    }

    krffs_split_path(path, dir, base);
    if (base[0] == '\0') {
        return -1;
    }
    parent_id = krffs_resolve_dir_id(mount, dir);
    if (parent_id == (uint64_t)-1) {
        return -1;
    }

    node = krffs_allocate_at(mount, 0, KRFFS_NO_NODE);
    if (node == KRFFS_NO_NODE) {
        return -1;
    }

    _krffs_strncpy((char *)name, base, KRFFS_FILE_NAME_BUFFER_SIZE);
    name[KRFFS_FILE_NAME_BUFFER_SIZE - 1] = '\0';
    rc |= krffs_put_new_u64(mount, node, KRFFS_FIELD_ID, krffs_calculate_djb_hash(name));
    rc |= krffs_put_new_u64(mount, node, KRFFS_FIELD_PARENT, parent_id);
    rc |= krffs_put_u16(mount, node, KRFFS_FIELD_MODE, 0644); /* rw-r--r-- */
    rc |= krffs_put_u32(mount, node, KRFFS_FIELD_UID, 0);
    rc |= krffs_put_u32(mount, node, KRFFS_FIELD_GID, 0);

    /* Born now, in all three: a file created and never written still has a
     * moment it came into being, and a build tool compares against it. */
    krffs_stamp_at(mount, node, KRFFS_STAMP_DATA);
    if (krffs_head(mount, node, &head) == 0) {
        rc |= krffs_put(mount, node, KRFFS_FIELD_ATIME, &head.modification_time,
                        (uint32_t)sizeof(head.modification_time));
    }

    /* The name last, and on the medium after everything it names: a node
     * reached by a path must already have the directory, the mode and the
     * owner it will be read with. A stop before the name leaves a node no
     * path reaches, and the check reclaims it. */
    if (rc != 0 || krffs_sync_point(mount) != 0 ||
        krffs_put(mount, node, KRFFS_FIELD_NAME, name, KRFFS_FILE_NAME_BUFFER_SIZE) != 0 ||
        krffs_sync_point(mount) != 0) {
        return -1;
    }

    /* And the directory it appeared in changed, which is what a program
     * watching a directory for new files is reading. */
    krffs_stamp_at(mount, krffs_parent_offset(mount, node), KRFFS_STAMP_DATA);

    return 0;
}

/**
 * Resize the node at `node` to `new_size` bytes, zero-filling any growth.
 *
 * The node and not a path, because a file whose name has been taken away is
 * still a file its holder may truncate, and there is no path that names it:
 * a walk of the path a nameless node would spell reaches its parent
 * directory instead. `node_out` answers where the node is afterwards, which
 * a grow with no room after it changes.
 *
 * Returns 0 on success, `KRFFS_EBAD` if it is not a regular file and
 * `KRFFS_ENOSPACE` if there is no space to grow it.
 */
int krffs_truncate_node_at(krffs_mount_t *mount, uint64_t node, uint32_t new_size,
                           uint64_t *node_out)
{
    uint64_t old_data_size;
    krffs_node_t head;

    if (node_out) {
        *node_out = node;
    }
    if (node == KRFFS_NO_NODE || node == 0 || krffs_head(mount, node, &head) != 0 ||
        head.type != KRFFS_RESERVED_NODE || KRFFS_IS_DIR(head.mode)) {
        return KRFFS_EBAD;
    }

    old_data_size = head.data_size;

    {
        uint64_t moved = krffs_resize_at(mount, node, new_size);

        /* Where the node is now, for the caller to repoint its holders
         * from. A resize that could not move the file left it where it was,
         * unless it had already freed it to make room, in which case there is
         * no node to point at and the holders keep what they have. */
        if (node_out) {
            krffs_node_t still;

            if (moved != KRFFS_NO_NODE) {
                *node_out = moved;
            } else if (krffs_head(mount, node, &still) == 0 && still.type == KRFFS_RESERVED_NODE) {
                *node_out = node;
            } else {
                *node_out = KRFFS_NO_NODE;
            }
        }
        if (moved == KRFFS_NO_NODE) {
            return KRFFS_ENOSPACE;
        }
        node = moved;
    }

    if ((uint64_t)new_size > old_data_size &&
        (krffs_data_fill(mount, node, old_data_size, 0, (uint64_t)new_size - old_data_size) != 0 ||
         krffs_sync_point(mount) != 0)) {
        return KRFFS_EBAD;
    }
    if (krffs_put_u64(mount, node, KRFFS_FIELD_DATA_SIZE, new_size) != 0) {
        return KRFFS_EBAD;
    }
    krffs_stamp_at(mount, node, KRFFS_STAMP_DATA);

    return 0;
}

/**
 * Resize the file at `path`, which is the same work reached by its name.
 */
int krffs_truncate_at(krffs_mount_t *mount, const char *path, uint32_t new_size, uint64_t *node_out)
{
    uint64_t node = krffs_walk(mount, path);
    int rc = krffs_truncate_node_at(mount, node, new_size, node_out);

    if (rc == KRFFS_EBAD) {
        klog(KLOG_DEBUG, "krffs", "truncate: no such file: %s", path);
    } else if (rc == KRFFS_ENOSPACE) {
        klog(KLOG_WARN, "krffs", "truncate: no space to resize: %s", path);
    }
    return rc;
}

/**
 * Take the node at `offset` out of the namespace without freeing it.
 *
 * Clears the name, which is what makes it unreachable: `krffs_walk` never
 * hands `krffs_find_child` a component shorter than one character, so a node
 * with an empty name cannot be matched by any path. That is a property of the
 * parser, not of a hash, which is why an empty name is used and not a sentinel
 * `parent_id`: `krffs_mix_id` may return any nonzero value, so a reserved id
 * would rest on a collision that cannot be ruled out. The node keeps its type,
 * its data and its parent id, so it is still a live file for whoever has a
 * descriptor on it, and still an ordinary reserved node to the allocator.
 *
 * The scans that enumerate by `parent_id`, not by name, skip an empty name for
 * the same reason, since being unmatchable covers the resolver and nothing
 * else. The caller frees the node when the last descriptor closes.
 */
void krffs_orphan_at(krffs_mount_t *mount, uint64_t offset)
{
    uint8_t empty = '\0';

    if (offset != KRFFS_NO_NODE && offset != 0) {
        krffs_put(mount, offset, KRFFS_FIELD_NAME, &empty, 1);
    }
}

/**
 * Remove the regular file at `path`.
 *
 * Returns 0 on success, -1 if the path is not a regular file (directories are
 * removed by krffs_rmdir).
 */
int krffs_unlink_at(krffs_mount_t *mount, const char *path)
{
    uint64_t node = krffs_walk(mount, path);
    krffs_node_t head;

    if (node == KRFFS_NO_NODE || node == 0 || krffs_head(mount, node, &head) != 0 ||
        KRFFS_IS_DIR(head.mode)) {
        klog(KLOG_DEBUG, "krffs", "unlink: no such file: %s", path);
        return -1;
    }

    /* Stamped before the node goes: its parent is read from it. */
    krffs_stamp_at(mount, krffs_parent_offset(mount, node), KRFFS_STAMP_DATA);
    return krffs_remove_at(mount, node);
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
int krffs_rename_at(krffs_mount_t *mount, const char *old_path, const char *new_path)
{
    uint64_t node;
    uint64_t existing;
    uint64_t parent_id;
    uint64_t new_id;
    krffs_node_t head;
    char dir[KRFFS_FILE_NAME_BUFFER_SIZE];
    char base[KRFFS_FILE_NAME_BUFFER_SIZE];
    uint8_t name[KRFFS_FILE_NAME_BUFFER_SIZE];

    node = krffs_walk(mount, old_path);
    if (node == KRFFS_NO_NODE || node == 0) {
        klog(KLOG_DEBUG, "krffs", "rename: no such file: %s", old_path);
        return -1;
    }
    if (krffs_head(mount, node, &head) != 0) {
        return -1;
    }
    /* A directory's id is mix(parent,name) and its children reference it, so a
     * rename/move would orphan them. Renaming directories is unsupported. */
    if (KRFFS_IS_DIR(head.mode)) {
        return -1;
    }

    krffs_split_path(new_path, dir, base);
    if (base[0] == '\0') {
        return -1;
    }
    parent_id = krffs_resolve_dir_id(mount, dir);
    if (parent_id == (uint64_t)-1) {
        return -1;
    }

    /* Renaming a file to the name it already has does nothing at all, which is
     * what POSIX requires: no removal, no relink, and no timestamp moves. */
    existing = krffs_walk(mount, new_path);
    if (existing == node) {
        return 0;
    }

    /* The id and the parent the rename will write, asked for before anything
     * is removed: a field whose changed bytes would reach the medium in two
     * units is refused here, with both names and their bytes as they were,
     * and not after the destination is gone. */
    _krffs_strncpy((char *)name, base, KRFFS_FILE_NAME_BUFFER_SIZE);
    name[KRFFS_FILE_NAME_BUFFER_SIZE - 1] = '\0';
    new_id = krffs_calculate_djb_hash(name);
    if (!krffs_write_is_whole(mount, node + KRFFS_FIELD_ID, &new_id, sizeof(new_id)) ||
        !krffs_write_is_whole(mount, node + KRFFS_FIELD_PARENT, &parent_id, sizeof(parent_id))) {
        klog(KLOG_WARN, "krffs",
             "rename: refused: a field of the node at %d would reach the medium in two pieces",
             (int)node);
        return -1;
    }
    /* Replace any existing destination, but never clobber a directory: freeing
     * it would orphan its children. */
    if (existing != KRFFS_NO_NODE) {
        krffs_node_t victim;

        if (krffs_head(mount, existing, &victim) != 0 || KRFFS_IS_DIR(victim.mode)) {
            return -1;
        }
        /* The name is about to be given to another node, so the one that has
         * it must lose it first: a destination that would not go leaves two
         * nodes under one name. */
        if (krffs_remove_at(mount, existing) != 0) {
            return -1;
        }
    }

    /* Both ends change: the name left one directory and arrived in another,
     * and they are the same directory in the ordinary case. The node itself
     * kept its contents, so only its attribute-change time moves. */
    krffs_stamp_at(mount, krffs_parent_offset(mount, node), KRFFS_STAMP_DATA);
    if (krffs_put_u64(mount, node, KRFFS_FIELD_ID, new_id) != 0 ||
        krffs_put_u64(mount, node, KRFFS_FIELD_PARENT, parent_id) != 0 ||
        krffs_sync_point(mount) != 0 ||
        krffs_put(mount, node, KRFFS_FIELD_NAME, name, KRFFS_FILE_NAME_BUFFER_SIZE) != 0 ||
        krffs_sync_point(mount) != 0) {
        return -1;
    }
    krffs_stamp_at(mount, krffs_parent_offset(mount, node), KRFFS_STAMP_DATA);
    krffs_stamp_at(mount, node, KRFFS_STAMP_ATTR);
    return 0;
}

/**
 * Create a directory at `path`.
 *
 * Returns 0 on success, -1 if the parent directory is missing or the name is
 * already taken.
 */
/**
 * Whether a directory on the medium already has the logical id `id`.
 *
 * Two names can mix to one id. A second directory with a taken id would share
 * its children with the first, since a child names its parent by id alone, so
 * the creation is refused instead.
 */
static int krffs_id_taken(krffs_mount_t *mount, uint64_t id)
{
    uint64_t scan = 0;
    uint64_t steps = 0;
    uint64_t bound = krffs_max_steps(mount);
    krffs_brief_t entry;

    do {
        if (krffs_brief(mount, scan, &entry) != 0) {
            return 0;
        }
        if (entry.type == KRFFS_RESERVED_NODE && KRFFS_IS_DIR(entry.mode) && entry.id == id &&
            krffs_has_name(mount, scan)) {
            return 1;
        }
        scan = krffs_step(mount, scan, entry.size);
    } while (scan != 0 && ++steps < bound);

    return 0;
}

int krffs_mkdir_at(krffs_mount_t *mount, const char *path)
{
    uint64_t node;
    uint64_t parent_id;
    krffs_node_t head;
    char dir[KRFFS_FILE_NAME_BUFFER_SIZE];
    char base[KRFFS_FILE_NAME_BUFFER_SIZE];
    uint8_t name[KRFFS_FILE_NAME_BUFFER_SIZE];
    int rc = 0;

    if (krffs_walk(mount, path) != KRFFS_NO_NODE) {
        return -1;
    }

    krffs_split_path(path, dir, base);
    if (base[0] == '\0') {
        return -1;
    }
    parent_id = krffs_resolve_dir_id(mount, dir);
    if (parent_id == (uint64_t)-1) {
        return -1;
    }

    if (krffs_id_taken(mount, krffs_mix_id(parent_id, base))) {
        return -1;
    }

    node = krffs_allocate_at(mount, 0, KRFFS_NO_NODE);
    if (node == KRFFS_NO_NODE) {
        return -1;
    }

    /* A directory has its name, the S_IFDIR mode, an empty body, and an id
     * (the mix of parent and name, checked free above) that its children
     * reference as their parent_id. */
    _krffs_strncpy((char *)name, base, KRFFS_FILE_NAME_BUFFER_SIZE);
    name[KRFFS_FILE_NAME_BUFFER_SIZE - 1] = '\0';
    rc |= krffs_put_new_u64(mount, node, KRFFS_FIELD_PARENT, parent_id);
    rc |= krffs_put_new_u64(mount, node, KRFFS_FIELD_ID, krffs_mix_id(parent_id, base));
    rc |= krffs_put_u16(mount, node, KRFFS_FIELD_MODE, (uint16_t)(KRFFS_S_IFDIR | 0755));
    rc |= krffs_put_u32(mount, node, KRFFS_FIELD_UID, 0);
    rc |= krffs_put_u32(mount, node, KRFFS_FIELD_GID, 0);
    krffs_stamp_at(mount, node, KRFFS_STAMP_DATA);
    if (krffs_head(mount, node, &head) == 0) {
        rc |= krffs_put(mount, node, KRFFS_FIELD_ATIME, &head.modification_time,
                        (uint32_t)sizeof(head.modification_time));
    }
    if (rc != 0 || krffs_sync_point(mount) != 0 ||
        krffs_put(mount, node, KRFFS_FIELD_NAME, name, KRFFS_FILE_NAME_BUFFER_SIZE) != 0 ||
        krffs_sync_point(mount) != 0) {
        return -1;
    }
    krffs_stamp_at(mount, krffs_parent_offset(mount, node), KRFFS_STAMP_DATA);
    return 0;
}

/**
 * Remove the empty directory at `path`.
 *
 * Returns 0, or -1 if `path` is not a directory or still has children.
 */
int krffs_rmdir_at(krffs_mount_t *mount, const char *path)
{
    uint64_t node = krffs_walk(mount, path);
    uint64_t scan = 0;
    uint64_t steps = 0;
    uint64_t bound = krffs_max_steps(mount);
    krffs_brief_t head;
    krffs_brief_t entry;
    uint64_t dir_id;

    if (node == KRFFS_NO_NODE || node == 0 || krffs_brief(mount, node, &head) != 0 ||
        !KRFFS_IS_DIR(head.mode)) {
        return -1;
    }
    dir_id = head.id;

    /* A directory is empty only if no node names it as a parent. An orphan does
     * not count: it keeps the parent id it had, but no path reaches it, so a
     * directory holding only orphans is empty as far as any caller can tell and
     * rmdir must succeed the way POSIX says. */
    do {
        if (krffs_brief(mount, scan, &entry) != 0) {
            return -1;
        }
        if (entry.type == KRFFS_RESERVED_NODE && entry.parent_id == dir_id &&
            krffs_has_name(mount, scan)) {
            return -1;
        }
        scan = krffs_step(mount, scan, entry.size);
    } while (scan != 0 && ++steps < bound);

    krffs_stamp_at(mount, krffs_parent_offset(mount, node), KRFFS_STAMP_DATA);
    return krffs_remove_at(mount, node);
}

/**
 * Return the `index`-th child of the directory at `path`, for getdents.
 *
 * Copies the child name into `name_out` (NUL-terminated, up to buf_size) and
 * reports through is_dir_out whether the child is itself a directory. Returns 1
 * if a child was returned, 0 past the end, -1 on error. Each call rescans the
 * whole node list for the index, a teaching simplification.
 */
int krffs_readdir_at(krffs_mount_t *mount, const char *path, uint32_t index, char *name_out,
                     uint32_t buf_size, int *is_dir_out)
{
    uint64_t node = krffs_walk(mount, path);
    uint64_t scan = 0;
    uint64_t steps = 0;
    uint64_t bound = krffs_max_steps(mount);
    uint64_t dir_id;
    uint32_t seen = 0;
    krffs_brief_t head;
    krffs_brief_t entry;

    if (!name_out || buf_size == 0 || node == KRFFS_NO_NODE) {
        return -1;
    }
    if (krffs_brief(mount, node, &head) != 0) {
        return -1;
    }
    if (node != 0 && !KRFFS_IS_DIR(head.mode)) {
        return -1;
    }
    dir_id = (node == 0) ? (uint64_t)KRFFS_ROOT_ID : head.id;

    do {
        if (krffs_brief(mount, scan, &entry) != 0) {
            return -1;
        }
        if (entry.type == KRFFS_RESERVED_NODE && entry.parent_id == dir_id &&
            krffs_has_name(mount, scan)) {
            if (seen == index) {
                uint8_t name[KRFFS_FILE_NAME_BUFFER_SIZE];
                uint32_t i;

                if (mount->backend->read(mount->ctx, scan + KRFFS_FIELD_NAME, name,
                                         KRFFS_FILE_NAME_BUFFER_SIZE) != 0) {
                    return -1;
                }
                for (i = 0; i + 1 < buf_size && name[i] != '\0'; i++) {
                    name_out[i] = (char)name[i];
                }
                name_out[i] = '\0';
                if (is_dir_out) {
                    *is_dir_out = KRFFS_IS_DIR(entry.mode) ? 1 : 0;
                }
                return 1;
            }
            seen++;
        }
        scan = krffs_step(mount, scan, entry.size);
    } while (scan != 0 && ++steps < bound);

    return 0;
}

/**
 * Free every orphan left on the medium by a crash. Returns how many it freed;
 * one a write refused is left where it is and counted as none.
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
 *
 * One bounded pass. Removing a node coalesces it with whichever of its
 * neighbours are free, so the merged region begins at the previous node when
 * that was free and at this node otherwise; either way it is a free node,
 * which the next iteration steps over by its (now larger) size. That is what
 * lets this resume in place instead of restarting at the root after each
 * removal, which would be a scan per orphan and, on a disk larger than the
 * page cache, a great many polled reads at boot.
 */
uint32_t krffs_reap_orphans_at(krffs_mount_t *mount)
{
    uint64_t previous = 0;
    uint64_t node;
    uint64_t steps = 0;
    uint64_t bound = krffs_max_steps(mount);
    uint32_t reaped = 0;
    krffs_brief_t head;
    krffs_brief_t before;

    if (krffs_brief(mount, previous, &head) != 0) {
        return 0;
    }
    node = krffs_step(mount, previous, head.size);
    while (node != 0 && steps < bound) {
        steps++;
        if (krffs_brief(mount, node, &head) != 0) {
            break;
        }
        if (head.type == KRFFS_RESERVED_NODE && !krffs_has_name(mount, node)) {
            uint64_t merged = node;

            if (krffs_brief(mount, previous, &before) == 0 && before.type == KRFFS_FREE_NODE) {
                merged = previous;
            }
            if (krffs_remove_at(mount, node) == 0) {
                reaped++;
                node = merged;
            }
            if (krffs_brief(mount, node, &head) != 0) {
                break;
            }
        }
        previous = node;
        node = krffs_step(mount, node, head.size);
    }
    return reaped;
}

/** Whether two name fields hold the same name. */
static int krffs_names_match(const uint8_t *a, const uint8_t *b)
{
    uint32_t i;

    for (i = 0; i < KRFFS_FILE_NAME_BUFFER_SIZE; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
        if (a[i] == '\0') {
            return 1;
        }
    }
    return 1;
}

/**
 * Set or clear the root's dirty flag, and make it reach the medium.
 *
 * A writable mount sets it before its first change and clears it when
 * everything it wrote has been written back. An image found with it set says
 * a check is owed, not what is wrong.
 */
int krffs_set_dirty(krffs_mount_t *mount, int dirty)
{
    krffs_node_t root;
    uint16_t mode;

    if (krffs_head(mount, 0, &root) != 0) {
        return -1;
    }
    mode = dirty ? (uint16_t)(root.mode | KRFFS_MODE_DIRTY)
                 : (uint16_t)(root.mode & ~KRFFS_MODE_DIRTY);
    if (mode == root.mode) {
        return 0;
    }
    if (krffs_put_u16(mount, 0, KRFFS_FIELD_MODE, mode) != 0) {
        return -1;
    }
    return krffs_sync_point(mount);
}

/** Whether the root says a check is owed. */
int krffs_is_dirty(krffs_mount_t *mount)
{
    krffs_node_t root;

    if (krffs_head(mount, 0, &root) != 0) {
        return 0;
    }
    return (root.mode & KRFFS_MODE_DIRTY) != 0;
}

/**
 * The most named nodes the repair below will compare with each other.
 *
 * Finding one name on two nodes takes a comparison of every named node with
 * every other, which is what a volume of this size can afford at a mount. A
 * volume with more named nodes than this is left to `fsck.krffs`, whose
 * sorting has no such bound, and the mount says so.
 */
#define KRFFS_REPAIR_NAMES 4096u

/**
 * Repair what a machine that stopped inside a change can leave.
 *
 * Three things, and they are the three the format's order of writes can
 * produce: a backward link the forward chain contradicts, a reserved node
 * with no name that no path reaches, and one name on two nodes of a
 * directory, which is a move that was interrupted. The counts go to
 * `links`, `orphans` and `doubles`, any of which may be NULL.
 *
 * The forward chain must already be sound: this repairs what the check would
 * otherwise refuse, and a chain it cannot walk is not one it can repair.
 * Answers 0, or -1 if a write was refused or the volume has more named nodes
 * than the pass compares.
 */
int krffs_repair_at(krffs_mount_t *mount, uint32_t *links, uint32_t *orphans, uint32_t *doubles)
{
    uint64_t offset = 0;
    uint64_t previous_size = 0;
    uint64_t steps = 0;
    uint64_t bound = krffs_max_steps(mount);
    uint32_t named = 0;
    uint32_t fixed_links = 0;
    uint32_t fixed_doubles = 0;
    krffs_node_t head;

    /* Counted before anything is written: a volume with more names than the
     * pass below compares is left whole for the host tool rather than half
     * repaired. */
    do {
        if (krffs_head(mount, offset, &head) != 0) {
            return -1;
        }
        if (head.type == KRFFS_RESERVED_NODE && head.name[0] != '\0') {
            named++;
        }
        offset = krffs_step(mount, offset, head.size);
    } while (offset != 0 && ++steps < bound);
    if (named > KRFFS_REPAIR_NAMES) {
        return -1;
    }
    offset = 0;
    steps = 0;
    named = 0;

    if (links) {
        *links = 0;
    }
    if (orphans) {
        *orphans = 0;
    }
    if (doubles) {
        *doubles = 0;
    }

    /* The backward links, from the forward chain that was just walked. */
    do {
        if (krffs_head(mount, offset, &head) != 0) {
            return -1;
        }
        if (offset != 0 && head.previous_node_size != previous_size) {
            if (krffs_put_u64(mount, offset, KRFFS_FIELD_PREVIOUS, previous_size) != 0) {
                return -1;
            }
            fixed_links++;
        }
        if (head.type == KRFFS_RESERVED_NODE && head.name[0] != '\0') {
            named++;
        }
        previous_size = head.size;
        offset = krffs_step(mount, offset, head.size);
    } while (offset != 0 && ++steps < bound);

    if (krffs_head(mount, 0, &head) == 0 && head.previous_node_size != previous_size) {
        if (krffs_put_u64(mount, 0, KRFFS_FIELD_PREVIOUS, previous_size) != 0) {
            return -1;
        }
        fixed_links++;
    }

    if (named > KRFFS_REPAIR_NAMES) {
        return -1; /* too many names to compare here; the host tool has them */
    }

    /* One name on two nodes: the larger is the copy the move was making,
     * since a move only ever makes a node larger, so the smaller loses its
     * name and the pass below reclaims it with the other nameless nodes. */
    offset = 0;
    steps = 0;
    do {
        krffs_node_t other;
        uint64_t scan;
        uint64_t inner = 0;

        if (krffs_head(mount, offset, &head) != 0) {
            return -1;
        }
        if (head.type == KRFFS_RESERVED_NODE && head.name[0] != '\0') {
            scan = krffs_step(mount, offset, head.size);
            while (scan != 0 && ++inner < bound) {
                if (krffs_head(mount, scan, &other) != 0) {
                    return -1;
                }
                if (other.type == KRFFS_RESERVED_NODE && other.parent_id == head.parent_id &&
                    other.name[0] != '\0' && krffs_names_match(other.name, head.name)) {
                    /* The larger is the copy a move was making, since a
                     * move only ever makes a node larger. A directory is
                     * never the loser whatever the sizes say: its children
                     * name it by its id, and a rename interrupted between
                     * the parent it published and the name it had still to
                     * write is what puts a file beside a directory of the
                     * same name. */
                    uint64_t loser;
                    uint8_t empty = '\0';

                    if (KRFFS_IS_DIR(head.mode) != KRFFS_IS_DIR(other.mode)) {
                        loser = KRFFS_IS_DIR(head.mode) ? scan : offset;
                    } else {
                        loser = other.size > head.size ? offset : scan;
                    }

                    if (krffs_put(mount, loser, KRFFS_FIELD_NAME, &empty, 1) != 0) {
                        return -1;
                    }
                    fixed_doubles++;
                    if (loser == offset) {
                        break;
                    }
                }
                scan = krffs_step(mount, scan, other.size);
            }
        }
        offset = krffs_step(mount, offset, head.size);
    } while (offset != 0 && ++steps < bound);

    if (links) {
        *links = fixed_links;
    }
    if (doubles) {
        *doubles = fixed_doubles;
    }
    if (orphans) {
        *orphans = krffs_reap_orphans_at(mount);
    } else {
        krffs_reap_orphans_at(mount);
    }
    return krffs_sync_point(mount);
}

/**
 * Walk the whole circular node chain once, bounding every step.
 *
 * The walkers trust each node's `size` to advance, so a malformed or
 * partially written image (a node whose size overshoots the region, is below
 * a header, or a chain that never closes) would drive them out of bounds or
 * into an infinite loop. Validate the structure before publishing it: each
 * node sits within the region with a header that fits, has the magic and
 * a known type, has a size of at least one header that does not run past the
 * end, data that fits inside it, and a backward link consistent with the
 * previous node; the sizes must tile the region exactly and the chain must
 * close back on the root. Returns 1 if sound, 0 if not.
 */
static int krffs_chain_walk(krffs_mount_t *mount, int with_links)
{
    uint64_t position = 0;
    uint64_t previous_size = 0;
    uint64_t total = 0;
    uint64_t bound = krffs_max_steps(mount);
    uint64_t count = 0;
    krffs_node_t head;
    krffs_node_t root;

    if (krffs_head(mount, 0, &root) != 0) {
        return 0;
    }

    for (;;) {
        if (++count > bound) {
            return 0; /* a cyclic or non-closing chain */
        }
        /* position stays in [0, size] (it starts at 0 and only advances by a
         * size already bounded below), so size - position never underflows.
         * Compare by subtraction so a hostile 64-bit size cannot wrap
         * position past the end. */
        if (mount->size - position < sizeof(krffs_node_t)) {
            return 0;
        }
        if (krffs_head(mount, position, &head) != 0) {
            return 0;
        }
        if (head.magic != KRFFS_FILE_SYSTEM_MAGIC) {
            return 0;
        }
        if (head.type != KRFFS_ROOT_NODE && head.type != KRFFS_FREE_NODE &&
            head.type != KRFFS_RESERVED_NODE) {
            return 0;
        }
        if (head.size < sizeof(krffs_node_t) || head.size > mount->size - position) {
            return 0;
        }
        /* Only a node that has data: a free node keeps whatever length the
         * file removed there had, and the allocator publishes the smaller
         * size of a split before it writes the new length, so a free node
         * whose length is larger than it is is an ordinary state and not a
         * fault. */
        if (head.type != KRFFS_FREE_NODE && head.data_size > head.size - sizeof(krffs_node_t)) {
            return 0;
        }
        if (with_links && count > 1 && head.previous_node_size != previous_size) {
            return 0;
        }
        if (head.type == KRFFS_RESERVED_NODE) {
            uint32_t i;

            for (i = 0; i < KRFFS_FILE_NAME_BUFFER_SIZE; i++) {
                if (head.name[i] == '\0') {
                    break;
                }
            }
            if (i == KRFFS_FILE_NAME_BUFFER_SIZE) {
                return 0; /* a name that never ends is read past by everyone */
            }
        }
        total += head.size;
        previous_size = head.size;
        position += head.size;
        if (position == mount->size) {
            break; /* the last node ends at the region edge; the list wraps */
        }
    }
    if (total != mount->size) {
        return 0;
    }
    if (with_links && root.previous_node_size != previous_size) {
        return 0;
    }
    return 1;
}

/**
 * The whole check: the structure and the backward links.
 */
int krffs_chain_is_valid(krffs_mount_t *mount)
{
    return krffs_chain_walk(mount, 1);
}

/**
 * The structure alone, without the backward links.
 *
 * What a mount asks of a volume it will not repair: the links are derived
 * from the forward chain, nothing that only reads one follows them, and a
 * machine that stopped inside a change is allowed to have left them behind.
 * A mount that writes repairs them and then asks for the whole check.
 */
int krffs_chain_is_walkable(krffs_mount_t *mount)
{
    return krffs_chain_walk(mount, 0);
}

/**
 * A node's permission bits, its owner, and the times a caller sets.
 *
 * The kind of a node is in the same field as its permissions and is never
 * changed by these: a directory stays a directory. The root is excluded by
 * its caller, since its mode is the format's and carries the dirty flag.
 */
int krffs_set_mode_at(krffs_mount_t *mount, uint64_t offset, uint16_t mode)
{
    krffs_node_t head;

    if (krffs_head(mount, offset, &head) != 0) {
        return -1;
    }
    return krffs_put_u16(mount, offset, KRFFS_FIELD_MODE,
                         (uint16_t)((head.mode & KRFFS_S_IFMT) | (mode & 07777u)));
}

int krffs_set_owner_at(krffs_mount_t *mount, uint64_t offset, uint32_t uid, uint32_t gid, int which)
{
    int rc = 0;

    if (which & KRFFS_SET_UID) {
        rc |= krffs_put_u32(mount, offset, KRFFS_FIELD_UID, uid);
    }
    if (which & KRFFS_SET_GID) {
        rc |= krffs_put_u32(mount, offset, KRFFS_FIELD_GID, gid);
    }
    return rc;
}

int krffs_set_time_at(krffs_mount_t *mount, uint64_t offset, int which, int64_t seconds,
                      int64_t nanoseconds)
{
    krffs_node_time_t when;

    when.seconds = seconds;
    when.nanoseconds = nanoseconds;
    return krffs_put(mount, offset,
                     which == KRFFS_SET_ATIME ? KRFFS_FIELD_ATIME : KRFFS_FIELD_MTIME, &when,
                     (uint32_t)sizeof(when));
}

/**
 * Count the reserved nodes (files and directories) in the file system.
 */
uint32_t krffs_count_at(krffs_mount_t *mount)
{
    uint64_t offset = 0;
    uint64_t steps = 0;
    uint64_t bound = krffs_max_steps(mount);
    uint32_t count = 0;
    krffs_brief_t brief;

    do {
        if (krffs_brief(mount, offset, &brief) != 0) {
            break;
        }
        /* An orphan is not a file any more: no path names it, so counting it
         * would report a file the boot banner's reader cannot find. */
        if (brief.type == KRFFS_RESERVED_NODE && krffs_has_name(mount, offset)) {
            count++;
        }
        offset = krffs_step(mount, offset, brief.size);
    } while (offset != 0 && ++steps < bound);

    return count;
}

/**
 * The bytes still free in the region.
 *
 * Free space is the sum of the free nodes' capacity, which is what a write can
 * actually use: a free node carries a header of its own, so its usable bytes
 * are its size less that header. A region with no free node at all reports
 * zero, which is the truth.
 */
uint32_t krffs_free_at(krffs_mount_t *mount)
{
    uint64_t offset = 0;
    uint64_t steps = 0;
    uint64_t bound = krffs_max_steps(mount);
    uint32_t free_bytes = 0;
    krffs_brief_t brief;

    do {
        if (krffs_brief(mount, offset, &brief) != 0) {
            break;
        }
        if (brief.type == KRFFS_FREE_NODE && brief.size > sizeof(krffs_node_t)) {
            free_bytes += (uint32_t)(brief.size - sizeof(krffs_node_t));
        }
        offset = krffs_step(mount, offset, brief.size);
    } while (offset != 0 && ++steps < bound);

    return free_bytes;
}

/*
 * The root's pointer interface.
 *
 * Everything above works in offsets on a mount. The root is a mapped image,
 * and the descriptor table, the syscall layer and the boot code hold pointers
 * into it, so the functions they call take and answer with pointers and turn
 * them into offsets here. The conversion holds only for the root, which is the
 * only mount with an address.
 */

static uint64_t krffs_offset_of(const krffs_node_t *node)
{
    uintptr_t base;
    uintptr_t position;

    if (!node || !krffs_fs) {
        return KRFFS_NO_NODE;
    }
    base = (uintptr_t)krffs_fs->node;
    position = (uintptr_t)node;
    /* Compared as addresses, not subtracted as pointers: a pointer that is
     * not in this image is not a node here, and answering an offset for it
     * would put a foreign address into the file system. */
    if (position < base || position - base >= (uintptr_t)krffs_fs->size) {
        return KRFFS_NO_NODE;
    }
    return (uint64_t)(position - base);
}

static krffs_node_t *krffs_node_of(uint64_t offset)
{
    if (offset == KRFFS_NO_NODE || !krffs_fs || offset >= krffs_fs->size) {
        return NULL;
    }
    return (krffs_node_t *)((char *)krffs_fs->node + offset);
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
 * Return the node following `node`, advancing by its own size.
 */
krffs_node_t *krffs_get_next_node(krffs_file_system_t *fs, krffs_node_t *node)
{
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

    return krffs_wrap_node_position(fs, (krffs_node_t *)(((char *)node) + node->size));
}

/**
 * The child of `dir` named `name`, one step of a walk, for the VFS lookup.
 * `dir` NULL or the root node means the root directory. NULL when absent.
 */
krffs_node_t *krffs_child(krffs_node_t *dir, const char *name)
{
    uint64_t id;
    uint32_t len = 0;

    if (!krffs_fs) {
        return NULL;
    }
    while (name[len] != '\0') {
        len++;
    }
    id = (!dir || dir == krffs_fs->node) ? (uint64_t)KRFFS_ROOT_ID : dir->id;
    return krffs_node_of(krffs_find_child(&krffs_root_mount, id, name, len));
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
    return krffs_node_of(krffs_walk(&krffs_root_mount, path));
}

/**
 * Build the absolute, canonical path of `node` into buf; 0 ok, -1 if it does
 * not fit.
 */
int krffs_node_path(krffs_node_t *node, char *buf, uint32_t size)
{
    if (!krffs_fs || !node) {
        return -1;
    }
    return krffs_node_path_at(&krffs_root_mount, krffs_offset_of(node), buf, size);
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
    node = krffs_resolve(path);
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
int krffs_read_node(krffs_node_t *node, char *buffer, uint32_t size, uint32_t offset)
{
    if (!node || !buffer) {
        return -1;
    }
    return krffs_read_at(&krffs_root_mount, krffs_offset_of(node), buffer, size, offset);
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
    node = krffs_resolve(path);
    if (!node || node == krffs_fs->node || KRFFS_IS_DIR(node->mode)) {
        klog(KLOG_DEBUG, "krffs", "read: no such file: %s", path);
        return -1;
    }
    return krffs_read_node(node, buffer, size, offset);
}

/**
 * Write `size` bytes at `offset` into an already-resolved node.
 *
 * Updates *nodep if a grow relocated the node.
 */
int krffs_write_node(krffs_node_t **nodep, const char *buffer, uint32_t size, uint32_t offset)
{
    uint64_t node;
    int written;

    if (!krffs_fs || !nodep || !*nodep || !buffer) {
        return KRFFS_EBAD;
    }
    node = krffs_offset_of(*nodep);
    written = krffs_write_at(&krffs_root_mount, &node, buffer, size, offset);
    *nodep = krffs_node_of(node);
    return written;
}

/**
 * Truncate an open node and report its location after a possible relocation.
 */
int krffs_truncate_node(krffs_node_t **nodep, uint32_t new_size)
{
    uint64_t node;
    int rc;

    if (!krffs_fs || !nodep || !*nodep) {
        return KRFFS_EBAD;
    }
    node = krffs_offset_of(*nodep);
    rc = krffs_truncate_node_at(&krffs_root_mount, node, new_size, &node);
    *nodep = krffs_node_of(node);
    return rc;
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
    node = krffs_resolve(path);
    if (!node || node == krffs_fs->node || KRFFS_IS_DIR(node->mode)) {
        klog(KLOG_DEBUG, "krffs", "write: no such file: %s", path);
        return -1;
    }
    return krffs_write_node(&node, buffer, size, offset);
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
    node = krffs_resolve(path);
    if (!node || node == krffs_fs->node) {
        return 0;
    }
    return (uint32_t)node->data_size;
}

/**
 * Resize the file at `path` to `new_size` bytes, zero-filling any growth.
 *
 * This path interface reports status alone. Descriptor truncation uses
 * krffs_truncate_node, which also reports a relocated node.
 */
int krffs_truncate(const char *path, uint32_t new_size)
{
    if (!krffs_fs) {
        return -1;
    }
    return krffs_truncate_at(&krffs_root_mount, path, new_size, (uint64_t *)0) == 0 ? 0 : -1;
}

int krffs_create(const char *path)
{
    if (!krffs_fs) {
        return -1;
    }
    return krffs_create_at(&krffs_root_mount, path);
}

int krffs_unlink(const char *path)
{
    if (!krffs_fs) {
        return -1;
    }
    return krffs_unlink_at(&krffs_root_mount, path);
}

int krffs_rename(const char *old_path, const char *new_path)
{
    if (!krffs_fs) {
        return -1;
    }
    return krffs_rename_at(&krffs_root_mount, old_path, new_path);
}

int krffs_mkdir(const char *path)
{
    if (!krffs_fs) {
        return -1;
    }
    return krffs_mkdir_at(&krffs_root_mount, path);
}

int krffs_rmdir(const char *path)
{
    if (!krffs_fs) {
        return -1;
    }
    return krffs_rmdir_at(&krffs_root_mount, path);
}

int krffs_readdir(const char *path, uint32_t index, char *name_out, uint32_t buf_size,
                  int *is_dir_out)
{
    if (!krffs_fs) {
        return -1;
    }
    return krffs_readdir_at(&krffs_root_mount, path, index, name_out, buf_size, is_dir_out);
}

void krffs_stamp(krffs_node_t *node, int what)
{
    if (!krffs_fs || !node) {
        return;
    }
    krffs_stamp_at(&krffs_root_mount, krffs_offset_of(node), what);
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
        return NULL;
    }
    return krffs_node_of(krffs_parent_offset(&krffs_root_mount, krffs_offset_of(node)));
}

void krffs_orphan_node(krffs_node_t *node)
{
    if (!krffs_fs || !node) {
        return;
    }
    krffs_orphan_at(&krffs_root_mount, krffs_offset_of(node));
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

uint32_t krffs_reap_orphans(void)
{
    if (!krffs_fs) {
        return 0;
    }
    return krffs_reap_orphans_at(&krffs_root_mount);
}

/**
 * Stamp a node as free: the header primitive the tests and the allocator
 * share.
 */
krffs_node_t *krffs_initialize_free_node(krffs_node_t *node, uint64_t size,
                                         uint64_t previous_node_size)
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

krffs_node_t *krffs_allocate_reserved_node(krffs_file_system_t *fs, uint64_t size,
                                           krffs_node_t *original)
{
    if (fs != krffs_fs || !fs->node) {
        return NULL; /* the pointer interface is the root's */
    }
    return krffs_node_of(krffs_allocate_at(&krffs_root_mount, size,
                                           original ? krffs_offset_of(original) : KRFFS_NO_NODE));
}

krffs_node_t *krffs_resize_reserved_node(krffs_file_system_t *fs, krffs_node_t *node, uint64_t size)
{
    if (fs != krffs_fs || !fs->node || !node) {
        return NULL; /* the pointer interface is the root's */
    }
    return krffs_node_of(krffs_resize_at(&krffs_root_mount, krffs_offset_of(node), size));
}

void krffs_remove_reserved_node(krffs_file_system_t *fs, krffs_node_t *node)
{
    if (fs != krffs_fs || !fs->node || !node) {
        return; /* the pointer interface is the root's */
    }
    krffs_remove_at(&krffs_root_mount, krffs_offset_of(node));
}

uint32_t krffs_count_files(void)
{
    if (!krffs_fs) {
        return 0;
    }
    return krffs_count_at(&krffs_root_mount);
}

/* What the root was mounted from, and with what; see krffs.h. Fixed buffers
 * because there is one mount and it never changes after boot. */
static char krffs_mount_src[24] = "none";
static char krffs_mount_opt[32] = "ro";

void krffs_set_mount(const char *source, const char *options)
{
    uint32_t i;

    for (i = 0; i + 1 < sizeof(krffs_mount_src) && source && source[i]; i++) {
        krffs_mount_src[i] = source[i];
    }
    krffs_mount_src[i] = '\0';
    for (i = 0; i + 1 < sizeof(krffs_mount_opt) && options && options[i]; i++) {
        krffs_mount_opt[i] = options[i];
    }
    krffs_mount_opt[i] = '\0';
}

const char *krffs_mount_source(void)
{
    return krffs_mount_src;
}

const char *krffs_mount_options(void)
{
    return krffs_mount_opt;
}

uint32_t krffs_total_bytes(void)
{
    return krffs_fs ? (uint32_t)krffs_fs->size : 0;
}

uint32_t krffs_free_bytes(void)
{
    if (!krffs_fs) {
        return 0;
    }
    return krffs_free_at(&krffs_root_mount);
}

/**
 * Mount a KRFFS image of `ramdisk_size` bytes whose first byte is at
 * `ramdisk_base`.
 *
 * The base is wherever the image is addressable: the RAM-disk module in
 * place, or PCWIN_BASE when the page cache demand-maps the module stub
 * (rach2.fswindow) or a real ATA partition (rach2.fsdisk). The parameter
 * names are historical; a root is reached through the memory backend, which
 * is one memcpy over the base it is handed. The first node is the root: it
 * must carry KRFFS_FILE_SYSTEM_MAGIC and be of type KRFFS_ROOT_NODE. On
 * success the global krffs_fs points at the mounted file system. Returns 0 on
 * success, -1 if the image is too small or malformed.
 */
int init_krffs(void *ramdisk_base, uint32_t ramdisk_size)
{
    krffs_node_t *root;

    if (!ramdisk_base || ramdisk_size < sizeof(krffs_node_t)) {
        klog(KLOG_ERR, "krffs", "mount: refused: no image at %x, %d bytes",
             (unsigned)(uintptr_t)ramdisk_base, ramdisk_size);
        return -1;
    }

    root = (krffs_node_t *)ramdisk_base;

    if (root->magic != KRFFS_FILE_SYSTEM_MAGIC) {
        klog(KLOG_ERR, "krffs", "mount: refused: magic 0x%X, expected 0x%X", root->magic,
             KRFFS_FILE_SYSTEM_MAGIC);
        return -1;
    }
    if (root->type != KRFFS_ROOT_NODE) {
        klog(KLOG_ERR, "krffs", "mount: refused: first node is type %d, not the root", root->type);
        return -1;
    }

    krffs_root_mount.backend = &krffs_memory_backend;
    krffs_root_mount.ctx = ramdisk_base;
    krffs_root_mount.size = ramdisk_size;

    if (!krffs_chain_is_valid(&krffs_root_mount)) {
        klog(KLOG_ERR, "krffs", "mount: refused: the node chain is malformed");
        krffs_root_mount.backend = NULL;
        krffs_root_mount.ctx = NULL;
        krffs_root_mount.size = 0;
        return -1;
    }

    _krffs_fs_instance.node = root;
    _krffs_fs_instance.size = ramdisk_size;
    krffs_fs = &_krffs_fs_instance;

    return 0;
}
