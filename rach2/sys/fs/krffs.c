/*
 * KRFFS, the K&R first-fit file system: the format and its operations, and
 * the boot root's memory backend and pointer interface.
 *
 * The medium is one contiguous region holding a ring of nodes, linked by
 * their sizes both ways. Each node has a header (krffs_node_t) followed by
 * its data, and is the root, free, or reserved (a file, a directory, a FIFO
 * or a socket file). Allocation is first fit with split and coalesce, after
 * the storage allocator in The C Programming Language. A node's directory
 * is the one whose id is in its parent_id, as described below.
 *
 * The format is addressed in offsets from the first byte of the region and
 * reached through a backend, so one implementation serves an image in memory
 * and an image on a device. The order in which a change is written is what
 * lets a mount survive a machine that stops in the middle of one, and it is
 * written down in krffs.Readme.md.
 *
 * The boot root keeps a pointer interface over the memory backend: the
 * descriptor table, the system call layer and the boot code hold
 * krffs_node_t pointers into the mapped image, and the functions they call
 * are the facade at the end of this file.
 */

#include "../klog.h"
#include "krffs.h"

#include "../rtc.h"
#include "../serial.h"
#include "../lib/kerrno.h"

krffs_file_system_t *krffs_fs = NULL;

static krffs_file_system_t _krffs_fs_instance;

/** Copy `n` bytes from `src` to `dest`, which do not overlap. */
static void _krffs_memcpy(void *dest, const void *src, uint32_t n)
{
    char *d = (char *)dest;
    const char *s = (const char *)src;
    uint32_t i;
    for (i = 0; i < n; i++) {
        d[i] = s[i];
    }
}

/** Copy `src` into the `n` bytes of `dest` and fill the rest with NULs, as
 * strncpy does: `dest` ends in a NUL only when `src` is shorter than `n`. */
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
 * The boot root is mounted this way, whether it is a Multiboot module in
 * place or a module or a disk partition the page-cache window maps. The
 * window is demand-paged, so a copy through it can take a page fault, which
 * the page cache serves; the backend itself never reports a failure.
 */

/** Copy `size` bytes at `offset` of the image at `ctx` into `buffer`; 0. */
static int krffs_memory_read(void *ctx, uint64_t offset, void *buffer, uint32_t size)
{
    _krffs_memcpy(buffer, (const char *)ctx + offset, size);
    return 0;
}

/** Copy `size` bytes of `buffer` to `offset` of the image at `ctx`; 0. */
static int krffs_memory_write(void *ctx, uint64_t offset, const void *buffer, uint32_t size)
{
    _krffs_memcpy((char *)ctx + offset, buffer, size);
    return 0;
}

/** Nothing: the image is memory, and a root on a disk reaches the disk as
 * the page cache writes it back. 0. */
static int krffs_memory_sync(void *ctx)
{
    (void)ctx;
    return 0;
}

static const krffs_backend_t krffs_memory_backend = {krffs_memory_read, krffs_memory_write,
                                                     krffs_memory_sync};

/* The root's mount, and the mount every facade function below works on. */
static krffs_mount_t krffs_root_mount;

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
 * Returns 0, or -1 for a null argument, a header that would not fit inside
 * the region, or a read that fails.
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
 * the terminator, so the comparison reads len + 1 bytes, and a `len` of 0
 * matches an empty name. 0 otherwise, for a `len` the field cannot hold, and
 * for a read that fails.
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
 *
 * A read that fails answers 1. A caller frees a node without a name, or
 * removes a directory that has only such nodes in it, and a read that
 * failed must not be the reason a file is freed.
 */
static int krffs_has_name(krffs_mount_t *mount, uint64_t offset)
{
    uint8_t first = 0;

    if (mount->backend->read(mount->ctx, offset + KRFFS_FIELD_NAME, &first, 1) != 0) {
        return 1;
    }
    return first != '\0';
}

/**
 * Whether the bytes this write changes all fall in one unit of the medium.
 *
 * A backend that puts whole blocks on the medium writes a field that spans
 * two of them in two pieces, and a stop between them leaves half of each.
 * Only the bytes that change matter: a length whose top four bytes are zero
 * before and after is written in two pieces that say the same thing in one
 * of them, and that is a write like any other.
 *
 * A mount with no unit, a single byte, which cannot be torn, and a write of
 * more than 16 bytes, a name, which is written without the rule, answer 1;
 * a read of the old bytes that fails answers 0. The unit a byte falls in is
 * counted in 32 bits, since a region is addressed in 32 and this kernel has
 * no 64-bit division.
 */
static int krffs_write_is_whole(krffs_mount_t *mount, uint64_t at, const void *value, uint32_t size)
{
    uint8_t old[16];
    const uint8_t *want = value;
    uint32_t base = (uint32_t)at;
    uint32_t first;
    uint32_t last;
    uint32_t i;

    if (mount->grain == 0 || size <= 1 || size > sizeof(old)) {
        return 1;
    }
    if (base / mount->grain == (base + size - 1) / mount->grain) {
        return 1;
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
        return 1;
    }
    return (base + first) / mount->grain == (base + last) / mount->grain;
}

/**
 * Write `size` bytes of `value` into the field at `field` of the node at
 * `offset`, when the bytes it changes fall in one unit of the medium.
 *
 * One call publishes one field. What may be written before what is the
 * update protocol, and the callers below keep it. Returns the backend's
 * answer, or -1 with a log record, and nothing written, when
 * `krffs_write_is_whole` answers 0.
 */
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

/** `krffs_put` of a 64-bit `value`. */
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

/**
 * A field of any width of a node no path reaches yet, written as its ids are
 * and for the same reason: a node being made is read by nobody until its
 * name is written after every other field, and a copy being filled for a
 * move is a free node until its type is written after its own. Every field
 * of the two goes this way. Under the whole-unit rule a field whose changed
 * bytes fell in two units could not be written at all, so a node could be
 * neither made nor moved to a place where one of its fields crosses a unit.
 */
static int krffs_put_new(krffs_mount_t *mount, uint64_t offset, uint32_t field, const void *value,
                         uint32_t size)
{
    return mount->backend->write(mount->ctx, offset + field, value, size);
}

/** `krffs_put` of a 32-bit `value`. */
static int krffs_put_u32(krffs_mount_t *mount, uint64_t offset, uint32_t field, uint32_t value)
{
    return krffs_put(mount, offset, field, &value, (uint32_t)sizeof(value));
}

/** `krffs_put` of a 16-bit `value`. */
static int krffs_put_u16(krffs_mount_t *mount, uint64_t offset, uint32_t field, uint16_t value)
{
    return krffs_put(mount, offset, field, &value, (uint32_t)sizeof(value));
}

/** `krffs_put` of a byte. */
static int krffs_put_u8(krffs_mount_t *mount, uint64_t offset, uint32_t field, uint8_t value)
{
    return krffs_put(mount, offset, field, &value, (uint32_t)sizeof(value));
}

/** Read `size` bytes of the data of the node at `offset`, `at` bytes into
 * it; the backend's answer. */
static int krffs_data_read(krffs_mount_t *mount, uint64_t offset, uint64_t at, void *buffer,
                           uint32_t size)
{
    return mount->backend->read(mount->ctx, offset + sizeof(krffs_node_t) + at, buffer, size);
}

/** Write `size` bytes into the data of the node at `offset`, `at` bytes into
 * it; the backend's answer. */
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
 * Returns 0, or -1 if a write fails.
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
 * it. Answers 0, or -1 if the backend's sync failed.
 */
static int krffs_sync_point(krffs_mount_t *mount)
{
    return mount->backend->sync(mount->ctx);
}

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
 * The ring is circular: a position at or past the end of the region, where
 * a step from the last node lands, is folded back by one region size.
 */
static uint64_t krffs_wrap_offset(krffs_mount_t *mount, uint64_t position)
{
    if (position >= mount->size) {
        position -= mount->size;
    }
    return position;
}

/**
 * The offset of the node after the one at `offset`, whose size is `size`.
 *
 * A node smaller than its own header would not advance (an unbounded walk) or
 * fold to a bad position. Such an image fails the mount's check, and for one
 * here this answers the root, so a walk that stops at the root ends instead
 * of looping.
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
 * The offset of the node before the one at `offset`, whose header is `head`,
 * by its backward link; before the root, the last node.
 */
static uint64_t krffs_previous_offset(krffs_mount_t *mount, uint64_t offset,
                                      const krffs_node_t *head)
{
    if (head->previous_node_size > offset) {
        return mount->size - (head->previous_node_size - offset);
    }
    return offset - head->previous_node_size;
}

uint64_t krffs_max_steps(krffs_mount_t *mount)
{
    return mount->size / (uint32_t)sizeof(krffs_node_t) + 1;
}

/*
 * Path resolution.
 *
 * A node's parent_id is its directory's logical id (KRFFS_ROOT_ID == 0 for
 * the root, whose own node is the first one and has no parent reference). A
 * directory node's id is mix(parent_id, name), so children reference it.
 * Two different names can mix to one id, since the mix goes through the
 * name's DJB hash, which two names can share, and two directories with one id
 * would share their children; mkdir looks for the id before it makes the
 * directory and fails for the second name. Resolution matches each path
 * component by (name, parent_id), descending only into directories. No node
 * refers to a regular file's id, and the kernel gives a file the DJB hash of
 * its name.
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

/** The id of a directory named `name` in the directory `parent_id`:
 * `krffs_mix_hash` of the parent's id and the name's DJB hash. */
static uint64_t krffs_mix_id(uint64_t parent_id, const char *name)
{
    return krffs_mix_hash(parent_id, krffs_calculate_djb_hash((const uint8_t *)name));
}

/**
 * The root is recognised by its node type, since its stored mode need not
 * have the directory bits. A file's number mixes its stored id, the hash of
 * its name, rather than the name itself: unlink clears the name of a file
 * that is still open and leaves the id, and the file's number must not
 * change while it is open.
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
 * Find the reserved node with logical id `id`, for "..", the directory of a
 * node and the climb of `krffs_node_path_at`; KRFFS_NO_NODE when there is
 * none or a read fails.
 *
 * Answers KRFFS_NO_NODE for KRFFS_ROOT_ID: the root is not a reserved node
 * (it is the first node), so a caller handles the root itself rather than
 * look it up here.
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
 * Find the child named comp[0..len) directly inside directory `dir_id`; its
 * offset, or KRFFS_NO_NODE when there is none or a read fails. A `len` of 0
 * matches an orphan, whose name is empty.
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
 * A `.` leaves the walk where it is. A `..` whose parent is the root goes to
 * offset 0 directly, since a lookup by id cannot answer the root, which owns
 * the reserved id 0, and `..` back to the root would otherwise fail; the
 * directory id goes on as KRFFS_ROOT_ID, so the root's children (parent_id
 * 0) still resolve after it.
 */
uint64_t krffs_walk_err(krffs_mount_t *mount, const char *path, int *err)
{
    uint64_t cur_id = KRFFS_ROOT_ID;
    uint64_t offset = 0;
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
        } else if (len == 2 && start[0] == '.' && start[1] == '.') {
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

uint64_t krffs_walk(krffs_mount_t *mount, const char *path)
{
    return krffs_walk_err(mount, path, (int *)0);
}

/**
 * Split `path` into its directory part and final component.
 *
 * `dir` receives the parent path, everything before the last slash that
 * comes before a component, or "/" for a name in the root directory, and
 * `base` the last component, without a slash after it. Both buffers are
 * KRFFS_FILE_NAME_BUFFER_SIZE and the output is truncated to fit.
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
            n = 1;
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
 * The ancestors are collected as offsets, from the node up toward the root,
 * and their names read one at a time, since a name is 256 bytes and
 * sixty-four of them are not a stack. The climb stops at a node whose
 * parent is the root rather than look the root up, since a lookup by id
 * cannot answer the root, which has the reserved id 0.
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
            return -1;
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

    for (i = depth - 1; i >= 0; i--) {
        uint32_t n;

        if (krffs_head(mount, chain[i], &head) != 0) {
            return -1;
        }
        for (n = 0; n < KRFFS_FILE_NAME_BUFFER_SIZE - 1 && head.name[n] != '\0'; n++) {
            /* bounded: the field is 256 bytes and need not end in one */
        }
        if (pos + 1 + n + 1 > size) {
            return -1;
        }
        buf[pos++] = '/';
        k_memcpy(buf + pos, head.name, n);
        pos += n;
    }
    buf[pos] = '\0';
    return 0;
}

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
 * Give a node being made all three of its times, the moment it came into
 * being: a file created and never written still has one, and a build tool
 * compares against it. They are written as the node's other fields are,
 * through `krffs_put_new`, since nothing reads them until its name is written
 * after them. Answers 0, or -1 if a write failed.
 */
static int krffs_stamp_new(krffs_mount_t *mount, uint64_t node)
{
    krffs_node_time_t now;
    int64_t seconds = 0;
    int64_t nanoseconds = 0;
    int rc = 0;

    rtc_realtime(&seconds, &nanoseconds);
    now.seconds = seconds;
    now.nanoseconds = nanoseconds;
    rc |= krffs_put_new(mount, node, KRFFS_FIELD_ATIME, &now, (uint32_t)sizeof(now));
    rc |= krffs_put_new(mount, node, KRFFS_FIELD_MTIME, &now, (uint32_t)sizeof(now));
    rc |= krffs_put_new(mount, node, KRFFS_FIELD_CTIME, &now, (uint32_t)sizeof(now));
    return rc;
}

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
 * two. Its other fields are zero. Returns the backend's answer.
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
 * Copy `size` bytes of data from the node at `from` to the node at `to`; 0,
 * or -1 if a read or a write fails.
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
 * A write that fails is not reported, and leaves the same fault.
 */
static void krffs_fix_backward_link(krffs_mount_t *mount, uint64_t offset, uint64_t size)
{
    krffs_put_u64(mount, krffs_wrap_offset(mount, offset + size), KRFFS_FIELD_PREVIOUS, size);
}

/**
 * Allocate a reserved node with room for `size` data bytes (K&R first-fit).
 *
 * Scans the ring for the first free node large enough for the header plus
 * `size`. A larger free node is split, leaving the remainder free, unless the
 * remainder is too small to hold a header (then the whole node is taken). If
 * `original` is a node, its metadata and data are copied into the new one, as
 * the relocation half of a grow, and the new node keeps the original's data
 * length until the caller writes the rest; otherwise the new node has an
 * empty name and a data length of `size`. Answers the new node's offset, or
 * KRFFS_NO_NODE with `*err` set: KRFFS_ENOSPACE for a `size` larger than the
 * region or when the whole ring was walked and no free node fits, and
 * KRFFS_EIO when a read, a write or a sync failed on the way, a write would
 * have reached the medium in two pieces (`krffs_put`), or the walk did not
 * come back around. `size` is compared with the region before the header is
 * added to it, since a size near the top of the range would otherwise wrap
 * into a small one.
 *
 * The split writes the tail's header and syncs before the size that reveals
 * it, and a header whose write fails ends the allocation, since a ring that
 * pointed at bytes nobody wrote could not be walked. The node is published by
 * its type, written last, so an interrupted allocation leaves a chain a mount
 * can walk. Until then the node is a free one, whose fields no reader looks
 * at, so every field written before the type, a copy's name, ids, mode, owner
 * and times and the length of either, goes through `krffs_put_new`.
 *
 * Everything the node is made of is on the medium before its type: the bytes
 * a move copied, the length that counts them, and a name. A new node's name
 * is the empty one, because a free node keeps whatever name it had when it
 * was freed and publishing that would bring a removed file back. A node
 * published without a name is one no path reaches, and the check reclaims it;
 * every caller names it next.
 */
static uint64_t krffs_allocate_err(krffs_mount_t *mount, uint64_t size, uint64_t original, int *err)
{
    uint64_t size_with_header = sizeof(krffs_node_t) + size;
    uint64_t offset = 0;
    uint64_t steps = 0;
    uint64_t bound = krffs_max_steps(mount);
    krffs_brief_t brief;

    *err = KRFFS_ENOSPACE;
    if (size > mount->size) {
        return KRFFS_NO_NODE;
    }
    *err = KRFFS_EIO;

    do {
        if (krffs_brief(mount, offset, &brief) != 0) {
            return KRFFS_NO_NODE;
        }
        if (brief.type == KRFFS_FREE_NODE && brief.size >= size_with_header) {
            uint64_t data_size = size;

            if (brief.size > size_with_header) {
                uint64_t second_half_size = brief.size - size_with_header;

                if (second_half_size >= sizeof(krffs_node_t)) {
                    uint64_t tail = krffs_wrap_offset(mount, offset + size_with_header);

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
                rc |= krffs_put_new(mount, offset, KRFFS_FIELD_NAME, from.name,
                                    KRFFS_FILE_NAME_BUFFER_SIZE);
                rc |= krffs_put_new_u64(mount, offset, KRFFS_FIELD_ID, from.id);
                rc |= krffs_put_new_u64(mount, offset, KRFFS_FIELD_PARENT, from.parent_id);
                rc |= krffs_put_new(mount, offset, KRFFS_FIELD_MODE, &from.mode,
                                    (uint32_t)sizeof(from.mode));
                rc |= krffs_put_new(mount, offset, KRFFS_FIELD_UID, &from.uid,
                                    (uint32_t)sizeof(from.uid));
                rc |= krffs_put_new(mount, offset, KRFFS_FIELD_GID, &from.gid,
                                    (uint32_t)sizeof(from.gid));
                rc |= krffs_put_new(mount, offset, KRFFS_FIELD_ATIME, &from.access_time,
                                    (uint32_t)sizeof(from.access_time));
                rc |= krffs_put_new(mount, offset, KRFFS_FIELD_MTIME, &from.modification_time,
                                    (uint32_t)sizeof(from.modification_time));
                rc |= krffs_put_new(mount, offset, KRFFS_FIELD_CTIME, &from.attribute_change_time,
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

            if (original == KRFFS_NO_NODE) {
                uint8_t empty = '\0';

                if (krffs_put_new(mount, offset, KRFFS_FIELD_NAME, &empty, 1) != 0) {
                    return KRFFS_NO_NODE;
                }
            }
            if (krffs_sync_point(mount) != 0) {
                return KRFFS_NO_NODE;
            }
            if (krffs_put_new_u64(mount, offset, KRFFS_FIELD_DATA_SIZE, data_size) != 0 ||
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

    if (offset == 0) {
        *err = KRFFS_ENOSPACE;
    }
    return KRFFS_NO_NODE;
}

/** `krffs_allocate_err` for a caller that needs no reason. */
uint64_t krffs_allocate_at(krffs_mount_t *mount, uint64_t size, uint64_t original)
{
    int err;

    return krffs_allocate_err(mount, size, original, &err);
}

/**
 * The first node, the root, has no predecessor to merge with, and the last
 * node has no successor, since the next offset wraps to the root.
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

    if (offset != 0) {
        previous = krffs_previous_offset(mount, offset, &head);
    }
    next = krffs_next_offset(mount, offset, &head);
    size = head.size;

    if (krffs_put_u8(mount, offset, KRFFS_FIELD_TYPE, KRFFS_FREE_NODE) != 0) {
        return -1;
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
 * A grow into the free node after this one takes that node whole with one
 * size, and splits the surplus back off after: its free header, a sync, and
 * the shortened size. Writing the surplus's header while the free node was
 * still in the chain would land on that node's own header whenever the
 * growth is shorter than a header, and a machine stopping there would leave
 * a chain with a torn node in it; two published sizes, each leaving a chain
 * that can be walked, cost one more write and have no such state. The whole
 * node is synced before the surplus's header is written for the same
 * reason. A surplus that cannot be written back as a free node stays inside
 * the node: space nobody can allocate, and a sound chain.
 *
 * A move copies the node with `krffs_allocate_at`, which syncs after the
 * type that publishes the copy, and only then frees the old node, so a stop
 * between the two leaves one name on two nodes, which the check resolves,
 * and never the file with neither. The old node must go, since a running
 * kernel would walk to it and read what the file used to be, so a removal
 * that fails fails the resize, and the caller sets where its holders point
 * afterwards.
 *
 * A shrink stores and syncs the shorter length before the surplus's header,
 * which lands inside the bytes the length still on the medium counts: a stop
 * with the header written and the length not would give the file the
 * allocator's bytes to read. A resize that raises the length inside the
 * slack a node already has stores no length; the caller writes the bytes and
 * the length after them, as after a grow.
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

            if (krffs_put_u64(mount, offset, KRFFS_FIELD_SIZE, whole) != 0) {
                return KRFFS_NO_NODE;
            }
            krffs_fix_backward_link(mount, offset, whole);
            if (krffs_sync_point(mount) != 0) {
                return KRFFS_NO_NODE;
            }

            if (tail_size >= sizeof(krffs_node_t)) {
                uint64_t tail = krffs_wrap_offset(mount, offset + size_with_header);

                if (krffs_write_free_header(mount, tail, tail_size, size_with_header) == 0 &&
                    krffs_sync_point(mount) == 0 &&
                    krffs_put_u64(mount, offset, KRFFS_FIELD_SIZE, size_with_header) == 0) {
                    krffs_fix_backward_link(mount, tail, tail_size);
                }
            }
            return offset;
        }

        {
            uint64_t moved = krffs_allocate_at(mount, size, offset);

            if (moved != KRFFS_NO_NODE && krffs_remove_at(mount, offset) != 0) {
                return KRFFS_NO_NODE;
            }
            return moved;
        }
    }

    {
        uint64_t difference = head.size - size_with_header;

        if (size < head.data_size &&
            (krffs_put_u64(mount, offset, KRFFS_FIELD_DATA_SIZE, size) != 0 ||
             krffs_sync_point(mount) != 0)) {
            return KRFFS_NO_NODE;
        }

        if (difference >= sizeof(krffs_node_t)) {
            uint64_t free_size = difference;
            uint64_t surplus = krffs_wrap_offset(mount, offset + size_with_header);

            if (next_head.type == KRFFS_FREE_NODE) {
                free_size += next_head.size;
            }
            if (krffs_write_free_header(mount, surplus, free_size, size_with_header) == 0 &&
                krffs_sync_point(mount) == 0 &&
                krffs_put_u64(mount, offset, KRFFS_FIELD_SIZE, size_with_header) == 0) {
                krffs_fix_backward_link(mount, surplus, free_size);
            }
        }
        return offset;
    }
}

int krffs_read_at(krffs_mount_t *mount, uint64_t node, char *buffer, uint32_t size, uint32_t at)
{
    krffs_node_t head;

    if (node == KRFFS_NO_NODE || !buffer || krffs_head(mount, node, &head) != 0) {
        return -1;
    }
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
 * Without its own test for a write of nothing, the arithmetic below would
 * still grow the file to `at`. The hole below `at` is zeroed by the rule
 * `krffs_truncate_node_at` keeps for the other way a file grows: the bytes
 * under it are whatever the free space held, another file's contents until
 * they are cleared.
 */
int krffs_write_at(krffs_mount_t *mount, uint64_t *node, const char *buffer, uint32_t size,
                   uint32_t at)
{
    krffs_node_t head;
    uint64_t offset;
    uint64_t old_data_size;
    uint64_t new_size;

    if (!node || *node == KRFFS_NO_NODE || !buffer || krffs_head(mount, *node, &head) != 0 ||
        head.type != KRFFS_RESERVED_NODE) {
        return KRFFS_EBAD;
    }
    offset = *node;
    old_data_size = head.data_size;

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
        return KRFFS_EBAD;
    }
    krffs_stamp_at(mount, offset, KRFFS_STAMP_DATA);
    return (int)size;
}

int krffs_create_at(krffs_mount_t *mount, const char *path)
{
    return krffs_create_node_at(mount, path, 0644, 0, 0);
}

uint32_t krffs_mode_type(uint16_t mode)
{
    uint32_t type = (uint32_t)mode & KRFFS_S_IFMT;

    if (type == KRFFS_S_IFDIR || type == KRFFS_S_IFIFO || type == KRFFS_S_IFSOCK) {
        return type;
    }
    return KRFFS_S_IFREG;
}

/**
 * Every field before the name is written through `krffs_put_new`, since no
 * path reaches the node before its name. The name is written last, after a
 * sync, and synced: a node reached by a path must already have the
 * directory, the mode and the owner it will be read with, and a stop before
 * the name leaves a node no path reaches, which the check reclaims. The
 * directory is stamped because a program watching a directory for new files
 * reads its times.
 */
int krffs_create_node_at(krffs_mount_t *mount, const char *path, uint16_t mode, uint32_t uid,
                         uint32_t gid)
{
    uint64_t node;
    uint64_t parent_id;
    char dir[KRFFS_FILE_NAME_BUFFER_SIZE];
    char base[KRFFS_FILE_NAME_BUFFER_SIZE];
    uint8_t name[KRFFS_FILE_NAME_BUFFER_SIZE];
    int rc = 0;

    if (krffs_walk(mount, path) != KRFFS_NO_NODE) {
        return 0;
    }

    krffs_split_path(path, dir, base);
    if (base[0] == '\0') {
        return KRFFS_EBAD;
    }
    parent_id = krffs_resolve_dir_id(mount, dir);
    if (parent_id == (uint64_t)-1) {
        return KRFFS_EBAD;
    }

    node = krffs_allocate_err(mount, 0, KRFFS_NO_NODE, &rc);
    if (node == KRFFS_NO_NODE) {
        return rc;
    }
    rc = 0;

    _krffs_strncpy((char *)name, base, KRFFS_FILE_NAME_BUFFER_SIZE);
    name[KRFFS_FILE_NAME_BUFFER_SIZE - 1] = '\0';
    rc |= krffs_put_new_u64(mount, node, KRFFS_FIELD_ID, krffs_calculate_djb_hash(name));
    rc |= krffs_put_new_u64(mount, node, KRFFS_FIELD_PARENT, parent_id);
    rc |= krffs_put_new(mount, node, KRFFS_FIELD_MODE, &mode, (uint32_t)sizeof(mode));
    rc |= krffs_put_new(mount, node, KRFFS_FIELD_UID, &uid, (uint32_t)sizeof(uid));
    rc |= krffs_put_new(mount, node, KRFFS_FIELD_GID, &gid, (uint32_t)sizeof(gid));
    rc |= krffs_stamp_new(mount, node);

    if (rc != 0 || krffs_sync_point(mount) != 0 ||
        krffs_put(mount, node, KRFFS_FIELD_NAME, name, KRFFS_FILE_NAME_BUFFER_SIZE) != 0 ||
        krffs_sync_point(mount) != 0) {
        return KRFFS_EIO;
    }

    krffs_stamp_at(mount, krffs_parent_offset(mount, node), KRFFS_STAMP_DATA);

    return 0;
}

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
 * `krffs_walk` never hands `krffs_find_child` a component shorter than one
 * character, so a node with an empty name cannot be matched by any path.
 * That is a property of the parser rather than of a hash, which is why an
 * empty name is used and not a sentinel `parent_id`: `krffs_mix_id` may
 * return any nonzero value, so a reserved id would rest on a collision that
 * cannot be ruled out. The scans that enumerate by `parent_id`, not by name,
 * skip an empty name themselves, since being unmatchable covers the resolver
 * and nothing else.
 */
void krffs_orphan_at(krffs_mount_t *mount, uint64_t offset)
{
    uint8_t empty = '\0';

    if (offset != KRFFS_NO_NODE && offset != 0) {
        krffs_put(mount, offset, KRFFS_FIELD_NAME, &empty, 1);
    }
}

/** The directory is stamped before the node goes, since it is found from the
 * node. */
int krffs_unlink_at(krffs_mount_t *mount, const char *path)
{
    uint64_t node = krffs_walk(mount, path);
    krffs_node_t head;

    if (node == KRFFS_NO_NODE || node == 0 || krffs_head(mount, node, &head) != 0 ||
        KRFFS_IS_DIR(head.mode)) {
        klog(KLOG_DEBUG, "krffs", "unlink: no such file: %s", path);
        return -1;
    }

    krffs_stamp_at(mount, krffs_parent_offset(mount, node), KRFFS_STAMP_DATA);
    return krffs_remove_at(mount, node);
}

/**
 * The id and the parent the rename will write are put to the whole-unit
 * rule before anything is removed, so a failure there leaves both names and
 * their bytes as they were, and does not come after the destination is gone.
 * The name is about to be given to another node, so a destination that has
 * it must lose it first, and one whose removal fails fails the rename rather
 * than leave two nodes under one name. The node itself keeps its contents,
 * so only its change time moves.
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

    existing = krffs_walk(mount, new_path);
    if (existing == node) {
        return 0;
    }

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
    if (existing != KRFFS_NO_NODE) {
        krffs_node_t victim;

        if (krffs_head(mount, existing, &victim) != 0 || KRFFS_IS_DIR(victim.mode)) {
            return -1;
        }
        if (krffs_remove_at(mount, existing) != 0) {
            return -1;
        }
    }

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
 * Whether a directory on the medium already has the logical id `id`.
 *
 * Two names can mix to one id. A second directory with a taken id would share
 * its children with the first, since a child names its parent by id alone, so
 * the creation fails instead. A directory with no name does not count, and a
 * read that fails ends the search with 0.
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

int krffs_mkdir_at(krffs_mount_t *mount, const char *path, uint16_t mode, uint32_t uid,
                   uint32_t gid)
{
    uint16_t dmode = (uint16_t)(KRFFS_S_IFDIR | (mode & 07777u));
    uint64_t node;
    uint64_t parent_id;
    char dir[KRFFS_FILE_NAME_BUFFER_SIZE];
    char base[KRFFS_FILE_NAME_BUFFER_SIZE];
    uint8_t name[KRFFS_FILE_NAME_BUFFER_SIZE];
    int rc = 0;

    if (krffs_walk(mount, path) != KRFFS_NO_NODE) {
        return KRFFS_EBAD;
    }

    krffs_split_path(path, dir, base);
    if (base[0] == '\0') {
        return KRFFS_EBAD;
    }
    parent_id = krffs_resolve_dir_id(mount, dir);
    if (parent_id == (uint64_t)-1) {
        return KRFFS_EBAD;
    }

    if (krffs_id_taken(mount, krffs_mix_id(parent_id, base))) {
        return KRFFS_ETAKEN;
    }

    node = krffs_allocate_err(mount, 0, KRFFS_NO_NODE, &rc);
    if (node == KRFFS_NO_NODE) {
        return rc;
    }
    rc = 0;

    _krffs_strncpy((char *)name, base, KRFFS_FILE_NAME_BUFFER_SIZE);
    name[KRFFS_FILE_NAME_BUFFER_SIZE - 1] = '\0';
    rc |= krffs_put_new_u64(mount, node, KRFFS_FIELD_PARENT, parent_id);
    rc |= krffs_put_new_u64(mount, node, KRFFS_FIELD_ID, krffs_mix_id(parent_id, base));
    rc |= krffs_put_new(mount, node, KRFFS_FIELD_MODE, &dmode, (uint32_t)sizeof(dmode));
    rc |= krffs_put_new(mount, node, KRFFS_FIELD_UID, &uid, (uint32_t)sizeof(uid));
    rc |= krffs_put_new(mount, node, KRFFS_FIELD_GID, &gid, (uint32_t)sizeof(gid));
    rc |= krffs_stamp_new(mount, node);
    if (rc != 0 || krffs_sync_point(mount) != 0 ||
        krffs_put(mount, node, KRFFS_FIELD_NAME, name, KRFFS_FILE_NAME_BUFFER_SIZE) != 0 ||
        krffs_sync_point(mount) != 0) {
        return KRFFS_EIO;
    }
    krffs_stamp_at(mount, krffs_parent_offset(mount, node), KRFFS_STAMP_DATA);
    return 0;
}

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
 * The pass is one bounded walk. Removing a node coalesces it with whichever
 * of its neighbours are free, so the merged region begins at the previous
 * node when that was free and at this node otherwise; either way it is a
 * free node, which the next iteration steps over by its (now larger) size.
 * That is what lets this resume in place instead of restarting at the root
 * after each removal, which would be a scan per orphan and, on a disk larger
 * than the page cache, a great many polled reads at boot.
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
 * The named nodes are counted before anything is written, so a volume with
 * more names than the search for one name on two nodes compares is left
 * whole for the host tool rather than half repaired. The backward links are
 * rebuilt from the forward chain as it is walked. The search compares each
 * named node with every node after it; the loser's name is cleared, and the
 * pass of `krffs_reap_orphans_at` at the end frees it with the other nodes
 * that have no name. A rename stopped between the parent it stored and the
 * name it had still to write puts a file beside a directory of the same
 * name, which is why a directory keeps its name whatever the sizes say.
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
        return -1;
    }

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
 * Walk the whole circular node chain once, bounding every step, and check
 * it, the backward links too when `with_links` is set.
 *
 * The walkers advance by each node's `size`, so a malformed or partially
 * written image (a node whose size overshoots the region, is below a header,
 * or a chain that never closes) would drive them out of bounds or into an
 * infinite loop. The structure is checked before it is published: each node
 * lies within the region with a header that fits, has the magic and a known
 * type, has a size of at least one header that does not run past the end,
 * data that fits inside it, and a backward link consistent with the previous
 * node; the sizes must tile the region exactly and the chain must close back
 * on the root. Returns 1 if sound, 0 if not or if a read fails.
 *
 * The position stays in [0, size], since it starts at 0 and advances only
 * by a size already checked against what is left, so what is left is
 * computed by subtraction, which a hostile 64-bit size cannot wrap past the
 * end. A free node's data length is not checked: a free node keeps whatever
 * length the file removed there had, and the allocator publishes the smaller
 * size of a split before it writes the new length, so a free node whose
 * length is larger than it is is an ordinary state. A reserved node's name
 * must end within its field, since every reader would read past one that
 * does not.
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
            return 0;
        }
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
                return 0;
            }
        }
        total += head.size;
        previous_size = head.size;
        position += head.size;
        if (position == mount->size) {
            break;
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

int krffs_chain_is_valid(krffs_mount_t *mount)
{
    return krffs_chain_walk(mount, 1);
}

int krffs_chain_is_walkable(krffs_mount_t *mount)
{
    return krffs_chain_walk(mount, 0);
}

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
        if (brief.type == KRFFS_RESERVED_NODE && krffs_has_name(mount, offset)) {
            count++;
        }
        offset = krffs_step(mount, offset, brief.size);
    } while (offset != 0 && ++steps < bound);

    return count;
}

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

/**
 * The offset of `node` in the boot root's image, or KRFFS_NO_NODE for NULL,
 * before the mount, or for a pointer outside the image. The pointers are
 * compared as addresses and not subtracted as pointers: a pointer that is
 * not in this image is not a node here, and answering an offset for it
 * would put a foreign address into the file system.
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
    if (position < base || position - base >= (uintptr_t)krffs_fs->size) {
        return KRFFS_NO_NODE;
    }
    return (uint64_t)(position - base);
}

/** The address of the node at `offset` in the boot root's image, or NULL for
 * KRFFS_NO_NODE, an offset past the image, or before the mount. */
static krffs_node_t *krffs_node_of(uint64_t offset)
{
    if (offset == KRFFS_NO_NODE || !krffs_fs || offset >= krffs_fs->size) {
        return NULL;
    }
    return (krffs_node_t *)((char *)krffs_fs->node + offset);
}

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

krffs_node_t *krffs_get_next_node(krffs_file_system_t *fs, krffs_node_t *node)
{
    if (!fs || !node) {
        return NULL;
    }

    if (node->size < sizeof(krffs_node_t)) {
        return fs->node;
    }

    return krffs_wrap_node_position(fs, (krffs_node_t *)(((char *)node) + node->size));
}

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

krffs_node_t *krffs_resolve(const char *path)
{
    if (!krffs_fs) {
        return NULL;
    }
    return krffs_node_of(krffs_walk(&krffs_root_mount, path));
}

int krffs_node_path(krffs_node_t *node, char *buf, uint32_t size)
{
    if (!krffs_fs || !node) {
        return -1;
    }
    return krffs_node_path_at(&krffs_root_mount, krffs_offset_of(node), buf, size);
}

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

int krffs_read_node(krffs_node_t *node, char *buffer, uint32_t size, uint32_t offset)
{
    if (!node || !buffer) {
        return -1;
    }
    return krffs_read_at(&krffs_root_mount, krffs_offset_of(node), buffer, size, offset);
}

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

int krffs_create_node(const char *path, uint16_t mode)
{
    if (!krffs_fs) {
        return -1;
    }
    return krffs_create_node_at(&krffs_root_mount, path, mode, 0, 0);
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
    return krffs_mkdir_at(&krffs_root_mount, path, 0755, 0, 0);
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
        return NULL;
    }
    return krffs_node_of(krffs_allocate_at(&krffs_root_mount, size,
                                           original ? krffs_offset_of(original) : KRFFS_NO_NODE));
}

krffs_node_t *krffs_resize_reserved_node(krffs_file_system_t *fs, krffs_node_t *node, uint64_t size)
{
    if (fs != krffs_fs || !fs->node || !node) {
        return NULL;
    }
    return krffs_node_of(krffs_resize_at(&krffs_root_mount, krffs_offset_of(node), size));
}

void krffs_remove_reserved_node(krffs_file_system_t *fs, krffs_node_t *node)
{
    if (fs != krffs_fs || !fs->node || !node) {
        return;
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
