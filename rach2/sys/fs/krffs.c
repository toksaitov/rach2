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
#include "../mm/kheap.h"

krffs_file_system_t *krffs_fs = NULL;
void (*krffs_throttle)(void);

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
                                                     krffs_memory_sync, 0};

/* The root's mount, and the mount every facade function below works on. */
static krffs_mount_t krffs_root_mount;
/* The root namespace lock serializes last-close removals. A device write
 * during one must halt before reclaim can try another dirty page. */
static int krffs_root_in_reap;
/* Writes the disk root's dirty pages to the disk, or NULL for a root in
 * memory; 0 on success. */
static int (*krffs_root_flush)(void);

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
 * matches an empty name. 0 otherwise, and for a `len` the field cannot
 * hold; -1 for a read that fails.
 */
static int krffs_name_is(krffs_mount_t *mount, uint64_t offset, const char *comp, uint32_t len)
{
    uint8_t name[KRFFS_FILE_NAME_BUFFER_SIZE];
    uint32_t i;

    if (len + 1 > KRFFS_FILE_NAME_BUFFER_SIZE) {
        return 0;
    }
    if (mount->backend->read(mount->ctx, offset + KRFFS_FIELD_NAME, name, len + 1) != 0) {
        return -1;
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

uint32_t krffs_file_phase(const krffs_mount_t *mount, uint64_t offset)
{
    uint32_t sector = mount->sector;
    uint32_t data = (uint32_t)(offset + sizeof(krffs_node_t));

    return sector ? (data % sector + mount->sector_phase) % sector : 0;
}

/**
 * Whether a stop can cut the medium just before byte `at` of the region: at
 * an edge of the grain, the block a cache writes at a time, or with `all`
 * at an edge of a sector, which falls where `at + phase` is a multiple of
 * the sector. With a phase of 0 every grain's edge is a sector's.
 */
static int krffs_cut_before(const krffs_mount_t *mount, uint32_t at, int all)
{
    uint32_t phase = mount->sector_phase;

    if (mount->grain != 0 && at % mount->grain == 0) {
        return 1;
    }
    return all && mount->sector != 0 && at % mount->sector == (phase ? mount->sector - phase : 0);
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
 * The units are the grain's, and with `all` the sectors' too
 * (`krffs_cut_before`). A write of a single byte, which cannot be torn, and
 * of more than 16 bytes, a name, which is written without the rule, answer
 * 1; a read of the old bytes that fails answers 0. Offsets are counted in
 * 32 bits, since a region is addressed in 32 and this kernel has no 64-bit
 * division.
 */
static int krffs_write_fits(krffs_mount_t *mount, uint64_t at, const void *value, uint32_t size,
                            int all)
{
    uint8_t old[16];
    const uint8_t *want = value;
    uint32_t base = (uint32_t)at;
    uint32_t first;
    uint32_t last;
    uint32_t i;

    if (size <= 1 || size > sizeof(old)) {
        return 1;
    }
    for (i = 1; i < size && !krffs_cut_before(mount, base + i, all); i++) {
    }
    if (i == size) {
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
    for (i = first + 1; first < size && i <= last; i++) {
        if (krffs_cut_before(mount, base + i, all)) {
            return 0;
        }
    }
    return 1;
}

/**
 * Write `size` bytes of `buffer` at `at` of the region through the backend.
 * Every write of the core comes here. A halted mount (`krffs_withdraw`) has
 * one name on two nodes in the running kernel, so it takes no write at all
 * until the next mount's repair, as Linux's ext4 turns read-only after an
 * error: a write to either node, a removal or a rename would act on one
 * node of the two. Returns the backend's answer, or -1 when halted.
 */
static int krffs_backend_write(krffs_mount_t *mount, uint64_t at, const void *buffer, uint32_t size)
{
    if (mount->halted) {
        return -1;
    }
    return mount->backend->write(mount->ctx, at, buffer, size);
}

/** `krffs_write_fits` in the mount's grain, the rule `krffs_put` keeps. */
static int krffs_write_is_whole(krffs_mount_t *mount, uint64_t at, const void *value, uint32_t size)
{
    return krffs_write_fits(mount, at, value, size, 0);
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
    return krffs_backend_write(mount, at, value, size);
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
    return krffs_backend_write(mount, offset + field, &value, (uint32_t)sizeof(value));
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
    return krffs_backend_write(mount, offset + field, value, size);
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
    return krffs_backend_write(mount, offset + sizeof(krffs_node_t) + at, buffer, size);
}

/** Call `krffs_throttle` on the boot root once `*since` has passed
 * KRFFS_THROTTLE_BYTES, and start the count again. */
static void krffs_throttle_after(krffs_mount_t *mount, uint32_t *since)
{
    if (*since >= KRFFS_THROTTLE_BYTES) {
        *since = 0;
        if (mount == &krffs_root_mount && krffs_throttle) {
            krffs_throttle();
        }
    }
}

/**
 * `krffs_data_write` in pieces of KRFFS_THROTTLE_BYTES, with the throttle
 * between them, each cut again at the edges of the backend's `unit`, which
 * it stores whole or not at all: 0, or -1 if a write fails. `*done`, when
 * not NULL, is how many bytes from the start went in, all of `size` or the
 * pieces before the one that failed.
 */
static int krffs_data_write_long(krffs_mount_t *mount, uint64_t offset, uint64_t at,
                                 const char *buffer, uint32_t size, uint32_t *done)
{
    uint32_t unit = mount->backend->unit;
    uint64_t base = offset + sizeof(krffs_node_t);
    uint32_t put = 0, since = 0;

    while (put < size) {
        uint32_t n = size - put;

        if (n > KRFFS_THROTTLE_BYTES - since) {
            n = KRFFS_THROTTLE_BYTES - since;
        }
        if (unit != 0 && n > unit - ((uint32_t)(base + at + put) & (unit - 1u))) {
            n = unit - ((uint32_t)(base + at + put) & (unit - 1u));
        }
        if (krffs_data_write(mount, offset, at + put, buffer + put, n) != 0) {
            break;
        }
        put += n;
        since += n;
        if (put < size) {
            krffs_throttle_after(mount, &since);
        }
    }
    if (done) {
        *done = put;
    }
    return put == size ? 0 : -1;
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
    uint32_t i, since = 0;

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
        since += chunk;
        krffs_throttle_after(mount, &since);
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
    return mount->halted ? -1 : mount->backend->sync(mount->ctx);
}

/**
 * Put everything written so far on the medium, wherever the mount keeps it.
 *
 * A volume's sync point does. The disk root's backend is memory that the
 * page cache writes back a page at a time in any order, so its sync point
 * does nothing and the root's flush writes the pages. A write that must
 * reach the medium before the field that publishes it is followed by this,
 * and an operation that writes inside free space starts with it, since a
 * size an earlier operation stored, which may say where that free space
 * begins, may still be only in the cache. A failure sets `unsettled`, and
 * the next success clears it. A mount that could not withdraw a copy
 * (`halted`) answers -1, as it refuses every write, until the next mount's
 * repair. Answers 0, or -1 if either failed.
 */
static int krffs_barrier(krffs_mount_t *mount)
{
    if (mount->halted || krffs_sync_point(mount) != 0 ||
        (mount == &krffs_root_mount && krffs_root_flush && krffs_root_flush() != 0)) {
        mount->unsettled = 1;
        return -1;
    }
    mount->unsettled = 0;
    return 0;
}

/**
 * Before a write into a node's room, put a barrier when the last one
 * failed or a shrink left its shorter length in the cache: a size that gave
 * the node that room may still be only in the cache, and the bytes written
 * over what was a free node's header could reach the medium first; or the
 * old length may still count the bytes the write puts back. Answers 0, or
 * -1 while barriers still fail.
 */
static int krffs_settled(krffs_mount_t *mount)
{
    return mount->unsettled ? krffs_barrier(mount) : 0;
}

/* `krffs_field_split`'s answer for a field that two edges cut. */
#define KRFFS_SPLIT_MANY 5u

/**
 * The bytes of the 32-bit field at `at` that lie before an edge where a
 * stop can cut the medium (`krffs_cut_before`, sectors and grains), when
 * one passes through the field; 0 when none does, and KRFFS_SPLIT_MANY
 * when two do, as a sector's and a grain's edge can on a volume whose
 * phase is not 0. A region is under 4 GiB, so a size or a length changes
 * these four low bytes and no other.
 */
static uint32_t krffs_field_split(krffs_mount_t *mount, uint64_t at)
{
    uint32_t split = 0;
    uint32_t i;

    for (i = 1; i < sizeof(uint32_t); i++) {
        if (krffs_cut_before(mount, (uint32_t)at + i, 1)) {
            if (split != 0) {
                return KRFFS_SPLIT_MANY;
            }
            split = i;
        }
    }
    return split;
}

/**
 * Whether storing `size` in the node at `offset` changes bytes of one unit
 * only, of the sectors and of the grain `krffs_put` checks, so that a stop
 * leaves the old size or the new one. The caller holds the mount lock and
 * supplies the old size from the header it used to locate this extent.
 */
static int krffs_size_fits(krffs_mount_t *mount, uint64_t offset, uint64_t old, uint64_t size)
{
    uint64_t changed = old ^ size;
    uint32_t at = (uint32_t)offset + KRFFS_FIELD_SIZE;
    int seen = 0;

    while (changed != 0) {
        if (seen && krffs_cut_before(mount, at, 1)) {
            return 0;
        }
        if (changed & 0xffu) {
            seen = 1;
        }
        changed >>= 8;
        at++;
    }
    return 1;
}

/**
 * Store a size that publishes a change to the chain. On error keep the
 * dirty flag and require a barrier before reuse. An earlier size change
 * in this operation may still need its backward link or its writeback.
 * Return 0 or -1.
 */
static int krffs_publish_size(krffs_mount_t *mount, uint64_t offset, uint64_t size)
{
    if (krffs_put_u64(mount, offset, KRFFS_FIELD_SIZE, size) != 0) {
        mount->stale_links = 1;
        mount->unsettled = 1;
        return -1;
    }
    return 0;
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

/** Find the preceding node by a bounded forward walk, or KRFFS_NO_NODE. */
static uint64_t krffs_find_previous(krffs_mount_t *mount, uint64_t offset)
{
    uint64_t at = 0;
    uint64_t steps;
    uint64_t bound = krffs_max_steps(mount);
    krffs_brief_t head;

    for (steps = 0; steps < bound; steps++) {
        uint64_t next;

        if (krffs_brief(mount, at, &head) != 0) {
            break;
        }
        next = krffs_step(mount, at, head.size);
        if (next == offset) {
            return at;
        }
        if (next == 0) {
            break;
        }
        at = next;
    }
    return KRFFS_NO_NODE;
}

/**
 * The offset of the node before the one at `offset`, whose header is `head`,
 * by its backward link, or by a forward walk after a failed link write.
 */
static uint64_t krffs_previous_offset(krffs_mount_t *mount, uint64_t offset,
                                      const krffs_node_t *head)
{
    if (mount->stale_links) {
        return krffs_find_previous(mount, offset);
    }
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

/** Fold the 64-bit identifier `h` into an inode number of 32 bits that is
 * neither 0 nor the root's 1. */
static uint64_t krffs_fold_ino(uint64_t h)
{
    uint32_t f = (uint32_t)(h ^ (h >> 32));

    return f > 1u ? f : f + 2u;
}

/**
 * The root is recognised by its node type, since its stored mode need not
 * have the directory bits. A file's number mixes its stored id, the hash of
 * its name, rather than the name itself: unlink clears the name of a file
 * that is still open and leaves the id, and the file's number must not
 * change while it is open. Every number fits 32 bits, as ext4's and tmpfs's
 * do on Linux by default, since i386's stat and getdents on the x86-64
 * kernel fail with EOVERFLOW for a wider one.
 */
uint64_t krffs_node_ino(const krffs_node_t *node)
{
    if (node->type == KRFFS_ROOT_NODE) {
        return 1;
    }
    if (KRFFS_IS_DIR(node->mode)) {
        return krffs_fold_ino(node->id);
    }
    return krffs_fold_ino(krffs_mix_hash(node->parent_id, node->id));
}

/**
 * Find the reserved directory with logical id `id`, for "..", a node's
 * directory and the climb of `krffs_node_path_at`; KRFFS_NO_NODE when there is
 * none or a read fails.
 *
 * A directory without a name is passed over: a mkdir that failed before
 * its name may have left one with the id a later mkdir gives its own.
 *
 * Answers KRFFS_NO_NODE for KRFFS_ROOT_ID: the root is not a reserved node
 * (it is the first node), so a caller handles the root itself rather than
 * look it up here. `*failed` is set to 1 for a read that failed, so that a
 * caller can tell it from no such directory, and left alone otherwise.
 */
static uint64_t krffs_find_offset_by_id_err(krffs_mount_t *mount, uint64_t id, int *failed)
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
            *failed = 1;
            return KRFFS_NO_NODE;
        }
        if (brief.type == KRFFS_RESERVED_NODE && KRFFS_IS_DIR(brief.mode) && brief.id == id) {
            int unnamed = krffs_name_is(mount, offset, "", 0);

            if (unnamed < 0) {
                *failed = 1;
                return KRFFS_NO_NODE;
            }
            if (!unnamed) {
                return offset;
            }
        }
        offset = krffs_step(mount, offset, brief.size);
    } while (offset != 0 && ++steps < bound);

    return KRFFS_NO_NODE;
}

/** Whether the node at `offset` is the copy a halted mount could not
 * withdraw, which lookups and listings pass over. */
int krffs_hidden(krffs_mount_t *mount, uint64_t offset)
{
    return mount->withdrawn != 0 && offset == mount->withdrawn;
}

/**
 * Find the child named comp[0..len) directly inside directory `dir_id`; its
 * offset, or KRFFS_NO_NODE when there is none or a read fails, which sets
 * `*err`, when not NULL, to -EIO; it is 0 otherwise. A `len` of 0 matches
 * an orphan, whose name is empty.
 */
static uint64_t krffs_find_child_err(krffs_mount_t *mount, uint64_t dir_id, const char *comp,
                                     uint32_t len, int *err)
{
    uint64_t offset = 0;
    uint64_t steps = 0;
    uint64_t bound = krffs_max_steps(mount);
    krffs_brief_t brief;

    if (err) {
        *err = 0;
    }
    do {
        if (krffs_brief(mount, offset, &brief) != 0) {
            if (err) {
                *err = -EIO;
            }
            return KRFFS_NO_NODE;
        }
        if (brief.type == KRFFS_RESERVED_NODE && brief.parent_id == dir_id &&
            !krffs_hidden(mount, offset)) {
            int is = krffs_name_is(mount, offset, comp, len);

            if (is > 0) {
                return offset;
            }
            if (is < 0 && err) {
                *err = -EIO;
                return KRFFS_NO_NODE;
            }
        }
        offset = krffs_step(mount, offset, brief.size);
    } while (offset != 0 && ++steps < bound);

    return KRFFS_NO_NODE;
}

/**
 * One step of `krffs_walk_err`: the child named comp[0..len) of directory
 * `dir_id`, or KRFFS_NO_NODE with `*err`, when not NULL, -EIO for a read
 * that failed before it was found and -ENOENT when there is none.
 */
static uint64_t krffs_walk_step(krffs_mount_t *mount, uint64_t dir_id, const char *comp,
                                uint32_t len, int *err)
{
    int failed;
    uint64_t offset = krffs_find_child_err(mount, dir_id, comp, len, &failed);

    if (offset == KRFFS_NO_NODE && err) {
        *err = failed != 0 ? failed : -ENOENT;
    }
    return offset;
}

uint64_t krffs_child_at_err(krffs_mount_t *mount, uint64_t dir, const char *name, int *err)
{
    uint64_t id = KRFFS_ROOT_ID;
    uint32_t len = 0;
    krffs_brief_t brief;
    uint64_t found;
    int failed;

    if (err) {
        *err = -ENOENT;
    }
    if (!name) {
        return KRFFS_NO_NODE;
    }
    while (name[len] != '\0') {
        len++;
    }
    if (dir != 0 && dir != KRFFS_NO_NODE) {
        if (krffs_brief(mount, dir, &brief) != 0) {
            if (err) {
                *err = -EIO;
            }
            return KRFFS_NO_NODE;
        }
        if (!KRFFS_IS_DIR(brief.mode)) {
            return KRFFS_NO_NODE;
        }
        id = brief.id;
    }
    found = krffs_walk_step(mount, id, name, len, &failed);
    if (err) {
        *err = found == KRFFS_NO_NODE ? failed : 0;
    }
    return found;
}

uint64_t krffs_child_at(krffs_mount_t *mount, uint64_t dir, const char *name)
{
    return krffs_child_at_err(mount, dir, name, NULL);
}

/**
 * The `..` step of `krffs_walk_err` from the node at `*offset`, directory
 * `*cur_id`: its parent, or the root when the parent is the root or no
 * directory has its id. 0, or -1 for a read that failed.
 */
static int krffs_walk_up(krffs_mount_t *mount, uint64_t *offset, uint64_t *cur_id)
{
    uint64_t parent = KRFFS_ROOT_ID;
    krffs_brief_t brief;

    if (*offset != 0) {
        if (krffs_brief(mount, *offset, &brief) != 0) {
            return -1;
        }
        parent = brief.parent_id;
    }
    *offset = 0;
    *cur_id = (uint64_t)KRFFS_ROOT_ID;
    if (parent != (uint64_t)KRFFS_ROOT_ID) {
        int failed = 0;
        uint64_t up = krffs_find_offset_by_id_err(mount, parent, &failed);

        if (failed) {
            return -1;
        }
        if (up != KRFFS_NO_NODE) {
            if (krffs_brief(mount, up, &brief) != 0) {
                return -1;
            }
            *offset = up;
            *cur_id = brief.id;
        }
    }
    return 0;
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
            if (krffs_walk_up(mount, &offset, &cur_id) != 0) {
                if (err) {
                    *err = -EIO;
                }
                return KRFFS_NO_NODE;
            }
        } else {
            offset = krffs_walk_step(mount, cur_id, start, len, err);
            if (offset == KRFFS_NO_NODE) {
                return KRFFS_NO_NODE;
            }
            if (krffs_brief(mount, offset, &brief) != 0) {
                if (err) {
                    *err = -EIO;
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
    int failed = 0;
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
            return -EIO;
        }
        if (KRFFS_IS_DIR(brief.mode) && brief.id == (uint64_t)KRFFS_ROOT_ID) {
            break;
        }
        if (depth >= KRFFS_PATH_MAX_DEPTH) {
            return -1;
        }
        chain[depth++] = cur;
        if (brief.parent_id == (uint64_t)KRFFS_ROOT_ID) {
            break;
        }
        cur = krffs_find_offset_by_id_err(mount, brief.parent_id, &failed);
        if (failed) {
            return -EIO;
        }
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
            return -EIO;
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

/** Read the real-time clock into a KRFFS timestamp. */
static krffs_node_time_t krffs_now(void)
{
    krffs_node_time_t now;
    int64_t seconds, nanoseconds;

    rtc_realtime(&seconds, &nanoseconds);
    now.seconds = seconds;
    now.nanoseconds = nanoseconds;
    return now;
}

int krffs_stamp_at(krffs_mount_t *mount, uint64_t offset, int what)
{
    krffs_node_time_t now;

    if (offset == KRFFS_NO_NODE) {
        return 0;
    }
    now = krffs_now();
    if (what == KRFFS_STAMP_DATA &&
        krffs_put(mount, offset, KRFFS_FIELD_MTIME, &now, (uint32_t)sizeof(now)) != 0) {
        return -1;
    }
    return krffs_put(mount, offset, KRFFS_FIELD_CTIME, &now, (uint32_t)sizeof(now));
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
    krffs_node_time_t now = krffs_now();
    int rc = 0;

    rc |= krffs_put_new(mount, node, KRFFS_FIELD_ATIME, &now, (uint32_t)sizeof(now));
    rc |= krffs_put_new(mount, node, KRFFS_FIELD_MTIME, &now, (uint32_t)sizeof(now));
    rc |= krffs_put_new(mount, node, KRFFS_FIELD_CTIME, &now, (uint32_t)sizeof(now));
    return rc;
}

uint64_t krffs_parent_offset_err(krffs_mount_t *mount, uint64_t offset, int *err)
{
    krffs_node_t head;
    int failed = 0;
    uint64_t found;

    *err = 0;
    if (offset == KRFFS_NO_NODE) {
        return KRFFS_NO_NODE;
    }
    if (krffs_head(mount, offset, &head) != 0) {
        *err = -EIO;
        return KRFFS_NO_NODE;
    }
    if (head.parent_id == KRFFS_ROOT_ID) {
        return 0;
    }
    found = krffs_find_offset_by_id_err(mount, head.parent_id, &failed);
    if (failed) {
        *err = -EIO;
    }
    return found;
}

uint64_t krffs_parent_offset(krffs_mount_t *mount, uint64_t offset)
{
    int err;

    return krffs_parent_offset_err(mount, offset, &err);
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

    return krffs_backend_write(mount, offset, &head, (uint32_t)sizeof(head));
}

/**
 * Copy `size` bytes of data from the node at `from` to the node at `to`; 0,
 * or -1 if a read or a write fails.
 */
static int krffs_copy_data(krffs_mount_t *mount, uint64_t to, uint64_t from, uint64_t size)
{
    uint8_t block[256];
    uint64_t at = 0;
    uint32_t since = 0;

    while (at < size) {
        uint32_t chunk =
            size - at > sizeof(block) ? (uint32_t)sizeof(block) : (uint32_t)(size - at);
        if (krffs_data_read(mount, from, at, block, chunk) != 0 ||
            krffs_data_write(mount, to, at, block, chunk) != 0) {
            return -1;
        }
        at += chunk;
        since += chunk;
        krffs_throttle_after(mount, &since);
    }
    return 0;
}

/**
 * Store a backward link, which mount recovery can rebuild even if torn.
 * Returns the backend's answer.
 */
static int krffs_write_backward_link(krffs_mount_t *mount, uint64_t offset, uint64_t size)
{
    return krffs_backend_write(mount, offset + KRFFS_FIELD_PREVIOUS, &size, (uint32_t)sizeof(size));
}

/**
 * Repair the backward link of the node following the one at `offset`.
 *
 * The forward chain is the truth and this link is derived from it, so every
 * operation that changes a size fixes it afterwards; a machine that stops
 * before this leaves the one fault the check at the next mount can rebuild.
 * A failed write marks the links stale for both removal and unmount, and
 * answers -1 for a caller that must know the medium failed; 0 otherwise.
 */
static int krffs_fix_backward_link(krffs_mount_t *mount, uint64_t offset, uint64_t size)
{
    if (krffs_write_backward_link(mount, krffs_wrap_offset(mount, offset + size), size) != 0) {
        mount->stale_links = 1;
        return -1;
    }
    return 0;
}

/* What a move writes into its copy before the copy is published: zeroes from
 * the copied length up to `at`, then `size` bytes of `buffer` at `at`, none
 * when `buffer` is NULL. The copy's length becomes at least `at + size`. */
typedef struct krffs_fill {
    const char *buffer;
    uint64_t at;
    uint32_t size;
} krffs_fill_t;

/**
 * Write `fill` into the copy at `offset`, whose length is `*data_size`, and
 * raise `*data_size` to the end of what was written. The copy is free until
 * its type is written, so its room may hold anything. Returns 0, or -1 when
 * a write failed.
 */
static int krffs_fill_copy(krffs_mount_t *mount, uint64_t offset, const krffs_fill_t *fill,
                           uint64_t *data_size)
{
    uint64_t end = fill->at + fill->size;

    if (fill->at > *data_size &&
        krffs_data_fill(mount, offset, *data_size, 0, fill->at - *data_size) != 0) {
        return -1;
    }
    if (fill->buffer && fill->size > 0 &&
        krffs_data_write_long(mount, offset, fill->at, fill->buffer, fill->size, NULL) != 0) {
        return -1;
    }
    if (end > *data_size) {
        *data_size = end;
    }
    return 0;
}

/**
 * Take the free node at `offset`, of `free_size` bytes, for a node of
 * `size_with_header`. A barrier comes first, since a size an earlier operation
 * stored may still be only in the cache. The rest is split off as a free node
 * when it can hold a header: its header, a barrier, and the shortened size,
 * when that changes bytes of one sector; otherwise the whole node is taken.
 * Returns 0, or -1 when a write, a barrier or the size's store failed.
 */
static int krffs_take_free(krffs_mount_t *mount, uint64_t offset, uint64_t free_size,
                           uint64_t size_with_header)
{
    uint64_t rest = free_size - size_with_header;
    uint64_t tail = krffs_wrap_offset(mount, offset + size_with_header);

    if (krffs_barrier(mount) != 0) {
        return -1;
    }
    if (rest < sizeof(krffs_node_t)) {
        return 0;
    }
    if (krffs_write_free_header(mount, tail, rest, size_with_header) != 0 ||
        krffs_barrier(mount) != 0) {
        return -1;
    }
    if (!krffs_size_fits(mount, offset, free_size, size_with_header)) {
        return 0;
    }
    if (krffs_publish_size(mount, offset, size_with_header) != 0) {
        return -1;
    }
    (void)krffs_fix_backward_link(mount, tail, rest);
    return 0;
}

/**
 * Copy into the free node at `offset` the name, ids, mode, owner and times
 * of the node at `original` and its data up to the room `*data_size`
 * gives. Set `*data_size` to the number of bytes copied. The node is
 * free until its type is written, so nothing here goes through the
 * whole-unit rule. A write or truncate sets `modified` to give the copy
 * new modification and change times. A plain resize keeps the old times.
 * `*nameless` becomes 1 when the original has no name.
 * Returns 0, or -1 when a read or a write failed.
 */
static int krffs_copy_original(krffs_mount_t *mount, uint64_t offset, uint64_t original,
                               uint64_t *data_size, int modified, int *nameless)
{
    krffs_node_t from;
    uint64_t length;
    int rc = 0;

    if (krffs_head(mount, original, &from) != 0) {
        return -1;
    }
    *nameless = from.name[0] == '\0';
    if (modified) {
        from.modification_time = krffs_now();
        from.attribute_change_time = from.modification_time;
    }
    rc |= krffs_put_new(mount, offset, KRFFS_FIELD_NAME, from.name, KRFFS_FILE_NAME_BUFFER_SIZE);
    rc |= krffs_put_new_u64(mount, offset, KRFFS_FIELD_ID, from.id);
    rc |= krffs_put_new_u64(mount, offset, KRFFS_FIELD_PARENT, from.parent_id);
    rc |= krffs_put_new(mount, offset, KRFFS_FIELD_MODE, &from.mode, (uint32_t)sizeof(from.mode));
    rc |= krffs_put_new(mount, offset, KRFFS_FIELD_UID, &from.uid, (uint32_t)sizeof(from.uid));
    rc |= krffs_put_new(mount, offset, KRFFS_FIELD_GID, &from.gid, (uint32_t)sizeof(from.gid));
    rc |= krffs_put_new(mount, offset, KRFFS_FIELD_ATIME, &from.access_time,
                        (uint32_t)sizeof(from.access_time));
    rc |= krffs_put_new(mount, offset, KRFFS_FIELD_MTIME, &from.modification_time,
                        (uint32_t)sizeof(from.modification_time));
    rc |= krffs_put_new(mount, offset, KRFFS_FIELD_CTIME, &from.attribute_change_time,
                        (uint32_t)sizeof(from.attribute_change_time));
    if (rc != 0) {
        return -1;
    }
    length = from.data_size;
    if (length > *data_size) {
        length = *data_size;
    }
    *data_size = length;
    if (length > 0 && krffs_copy_data(mount, offset, original, length) != 0) {
        return -1;
    }
    return 0;
}

/**
 * Halt the mount after a write that failed left the volume in a state only
 * the repair undoes: every write fails until the next mount, which the
 * owed check (`check_owed`) has repair the volume, since it keeps the
 * dirty flag, and the owner turns the mounts read-only (`on_halt`).
 */
static void krffs_halt(krffs_mount_t *mount)
{
    mount->check_owed = 1;
    if (!mount->halted) {
        mount->halted = 1;
        if (mount->on_halt) {
            mount->on_halt(mount);
        }
    }
}

/**
 * Withdraw the reserved node at `offset`, a copy its caller was told does
 * not exist, by storing its type as free and nothing else, since a merge
 * could make it the larger of two nodes of one name. Either type may reach
 * the medium; a copy published there beside the file is one name on two
 * nodes, and the repair keeps the node with more data: the original of a
 * shrink, the copy of a grow, which holds the original's bytes too. When
 * the store fails the copy stays published beside the file in the running
 * kernel, so the mount is halted (`krffs_backend_write`), the copy is
 * hidden from lookups and listings (`withdrawn`), and the links are marked
 * stale, which keeps the dirty flag for the next mount's repair.
 */
static void krffs_withdraw(krffs_mount_t *mount, uint64_t offset)
{
    if (krffs_put_u8(mount, offset, KRFFS_FIELD_TYPE, KRFFS_FREE_NODE) != 0) {
        mount->stale_links = 1;
        mount->withdrawn = offset;
        krffs_halt(mount);
        return;
    }
    (void)krffs_barrier(mount);
}

/**
 * Allocate a reserved node with room for `size` data bytes (K&R first-fit).
 *
 * Scans the ring for the first free node large enough for the header plus
 * `size`. A larger free node is split, leaving the remainder free, unless the
 * remainder is too small to hold a header or its size would change bytes of
 * two sectors (`krffs_size_fits`); then the whole node is taken. If
 * `original` is a node, copy its metadata and data up to `size`. A non-null
 * `fill` gives the copy new modification and change times and supplies the
 * data to add (`krffs_fill_copy`). Its length counts that too. Without an
 * original the node has an empty name and a data length of `size`.
 * Return its offset, or KRFFS_NO_NODE with `*err` set to KRFFS_ENOSPACE
 * for a `size` larger than the region or when the ring has no free node
 * large enough, and KRFFS_EIO when a read, a write or a barrier failed, a
 * write would have reached the medium in two pieces (`krffs_put`), or the
 * walk did not come back around. `size` is compared with the region before
 * the header is added to it, since a size near the top of the range would
 * otherwise wrap into a small one.
 *
 * The split writes the tail's header and puts a barrier before the size that
 * reveals it, and a header whose write fails ends the allocation, since a
 * ring that pointed at bytes nobody wrote could not be walked. The node is
 * published by its type, written last, so an interrupted allocation leaves a
 * chain a mount can walk. Until then the node is a free one, whose fields no
 * reader looks at, so every field written before the type, a copy's name,
 * ids, mode, owner and times and the length of either, goes through
 * `krffs_put_new`.
 *
 * Everything the node is made of is on the medium before its type: the bytes
 * a move copied, the length that counts them, and a name. A new node's name
 * is the empty one, because a free node keeps whatever name it had when it
 * was freed and publishing that would bring a removed file back. A node
 * published without a name is one no path reaches, and the check reclaims it;
 * every caller names it next. Each of these orders is kept by a barrier, so
 * that it holds on the disk root, whose pages the page cache writes back in
 * any order. When the barrier after the type fails, the node is withdrawn
 * (`krffs_withdraw`), so that the running kernel never has a copy its caller
 * was told does not exist; either type may be on the medium, and a copy
 * published there is one name on two nodes, which the check resolves.
 * A node published without a name, a new one or the copy of an orphan, is
 * counted in `orphans` from its type on, until its name is published
 * (`krffs_publish_name`) or it is freed.
 */
static uint64_t krffs_allocate_err(krffs_mount_t *mount, uint64_t size, uint64_t original,
                                   const krffs_fill_t *fill, int *err)
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
            uint8_t empty = '\0';
            int nameless = 1;
            int rc;

            if (krffs_take_free(mount, offset, brief.size, size_with_header) != 0) {
                return KRFFS_NO_NODE;
            }
            if (original != KRFFS_NO_NODE) {
                rc = krffs_copy_original(mount, offset, original, &data_size, fill != NULL,
                                         &nameless);
                if (rc == 0 && fill) {
                    rc = krffs_fill_copy(mount, offset, fill, &data_size);
                }
            } else {
                rc = krffs_put_new(mount, offset, KRFFS_FIELD_NAME, &empty, 1);
            }
            if (rc != 0 || krffs_barrier(mount) != 0 ||
                krffs_put_new_u64(mount, offset, KRFFS_FIELD_DATA_SIZE, data_size) != 0 ||
                krffs_barrier(mount) != 0 ||
                krffs_put_u8(mount, offset, KRFFS_FIELD_TYPE, KRFFS_RESERVED_NODE) != 0) {
                return KRFFS_NO_NODE;
            }
            if (nameless) {
                mount->orphans++; /* a withdrawn one stays counted */
            }
            if (krffs_barrier(mount) != 0) {
                krffs_withdraw(mount, offset);
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

    return krffs_allocate_err(mount, size, original, NULL, &err);
}

/**
 * Publish a merge only if its size reaches the medium whole.
 *
 * A size fits in 32 bits, and all four low bytes must share one sector and
 * one grain (`krffs_field_split`), whatever this merge changes: on the disk
 * root another merge of the same node may still be in the cache, and the
 * two can change different bytes. Returns 0 when the size is stored, 1 when
 * the nodes must stay separate, or -1 when the store fails
 * (`krffs_publish_size`).
 */
static int krffs_merge_size(krffs_mount_t *mount, uint64_t offset, uint64_t size)
{
    if (krffs_field_split(mount, offset + KRFFS_FIELD_SIZE) != 0) {
        return 1;
    }
    return krffs_publish_size(mount, offset, size);
}

/** Write the bytes [from, to) of the 32-bit `value` into the field at `at`;
 * the backend's answer. */
static int krffs_put_bytes(krffs_mount_t *mount, uint64_t at, uint32_t value, uint32_t from,
                           uint32_t to)
{
    const uint8_t *bytes = (const uint8_t *)&value;

    return krffs_backend_write(mount, at + from, bytes + from, to - from);
}

/**
 * The middle step of storing `length` over `old` in the node at `offset`, as
 * `krffs_store_length` describes: the value the length holds after its
 * first piece is stored, which lies between the two. A length whose four low
 * bytes cross no sector, or that does not change, steps straight to
 * `length`. Returns 0 with `*mid` set, or 1 when no order exists, which a
 * field that two edges cut has too.
 */
static int krffs_length_steps(krffs_mount_t *mount, uint64_t offset, uint64_t old, uint64_t length,
                              uint32_t *mid)
{
    uint32_t low = krffs_field_split(mount, offset + KRFFS_FIELD_DATA_SIZE);
    uint32_t shift;
    uint32_t mask;
    uint32_t want = (uint32_t)length;
    uint32_t was = (uint32_t)old;
    uint32_t top = want > was ? want : was;
    uint32_t bottom = want > was ? was : want;
    uint32_t step;

    *mid = want;
    if (low == 0 || was == want) {
        return 0;
    }
    if (low == KRFFS_SPLIT_MANY) {
        return 1;
    }
    shift = low * 8;
    mask = ((uint32_t)1 << shift) - 1;
    step = (want & ~mask) | (was & mask);
    if (step < bottom || step > top) {
        if ((top >> shift) - (bottom >> shift) < 2) {
            return 1;
        }
        step = want > was ? want - ((uint32_t)1 << shift) : want + ((uint32_t)1 << shift);
        step = (step & ~mask) | (was & mask);
    }
    *mid = step;
    return 0;
}

/**
 * Store a new length, `length`, over `old`, so that a stop leaves a length
 * between the two; `krffs_store_length` is the entry.
 *
 * A length whose four low bytes cross a sector reaches the medium in two
 * pieces, the low bytes in one sector and the high ones in the next, and a
 * stop between them leaves the old value's piece beside the new one's: a
 * length below both, which loses bytes the file had, or above both, which
 * counts bytes nobody wrote or that do not fit the node. So the old value
 * is put on the medium first, and then each piece goes with a barrier
 * between, in an order whose every step lies between `old` and `length`.
 * When the new high part with the old low part lies between them, the high
 * part goes first and the low part after. Otherwise, when the high parts
 * differ by two or more, the high part steps to one short of the new one,
 * then the low part goes, then the high part. When they differ by one no
 * order exists (`krffs_length_steps`), and the caller moves the file
 * instead; a caller that raises a length asks first, before it writes the
 * bytes the length will count.
 *
 * Returns 0 with the length stored, and on the medium when it crosses a
 * sector; 1 with nothing written when no order exists; or -1 when a write or
 * a barrier failed. `*shown` is the length the field holds afterwards, which
 * readers see: `length` on success, and on a failure `old` or the step a
 * piece stored before a later piece or its barrier failed. Each piece lies
 * in one sector, which a write stores whole or not at all.
 */
static int krffs_store_length_in(krffs_mount_t *mount, uint64_t offset, uint64_t old,
                                 uint64_t length, uint64_t *shown)
{
    uint64_t at = offset + KRFFS_FIELD_DATA_SIZE;
    uint32_t low = krffs_field_split(mount, at);
    uint32_t mask;
    uint32_t want = (uint32_t)length;
    uint32_t was = (uint32_t)old;
    uint32_t mid;

    *shown = old;
    if (low == 0 || was == want) {
        if (krffs_put_u64(mount, offset, KRFFS_FIELD_DATA_SIZE, length) != 0) {
            return -1;
        }
        *shown = length;
        return 0;
    }
    if (krffs_length_steps(mount, offset, old, length, &mid) != 0) {
        return 1;
    }
    mask = ((uint32_t)1 << (low * 8)) - 1;
    if (krffs_barrier(mount) != 0) {
        return -1;
    }
    if ((mid & ~mask) != (was & ~mask)) {
        if (krffs_put_bytes(mount, at, mid, low, sizeof(mid)) != 0) {
            return -1;
        }
        *shown = (mid & ~mask) | (was & mask);
        if (krffs_barrier(mount) != 0) {
            return -1;
        }
    }
    if ((mid & mask) != (want & mask)) {
        if (krffs_put_bytes(mount, at, want, 0, low) != 0) {
            return -1;
        }
        *shown = (mid & ~mask) | (want & mask);
        if (krffs_barrier(mount) != 0) {
            return -1;
        }
    }
    if ((mid & ~mask) != (want & ~mask)) {
        if (krffs_put_bytes(mount, at, want, low, sizeof(want)) != 0) {
            return -1;
        }
        *shown = length;
        if (krffs_barrier(mount) != 0) {
            return -1;
        }
    }
    *shown = length;
    return 0;
}

/**
 * Store the length and return the visible EOF in `shown` when supplied.
 * A failed piece or barrier can leave an earlier piece visible in cache.
 * Settle it before another operation reuses bytes beyond the shorter EOF.
 */
static int krffs_store_length(krffs_mount_t *mount, uint64_t offset, uint64_t old, uint64_t length,
                              uint64_t *shown)
{
    uint64_t seen;
    int rc = krffs_store_length_in(mount, offset, old, length, &seen);

    if (shown) {
        *shown = seen;
    }

    if (rc < 0) {
        mount->unsettled = 1;
    }
    return rc;
}

/**
 * The reads a removal of the node at `offset`, whose header is `head`, makes
 * before it stores anything: the header of the node before it into `before`,
 * with its offset in `*previous` (KRFFS_NO_NODE for the root's own node),
 * the header after it into `after` unless the root follows, and the header
 * whose backward link a merge repairs, including the root after a merge
 * at the end of the volume. Read it before the merge is published.
 * Return 0, or -1 for a read that failed or a lost link.
 */
static int krffs_remove_reads(krffs_mount_t *mount, uint64_t offset, const krffs_node_t *head,
                              uint64_t *previous, krffs_node_t *before, krffs_node_t *after)
{
    krffs_node_t beyond;
    uint64_t next = krffs_next_offset(mount, offset, head);

    *previous = KRFFS_NO_NODE;
    if (offset != 0) {
        *previous = krffs_previous_offset(mount, offset, head);
        if (*previous == KRFFS_NO_NODE || krffs_head(mount, *previous, before) != 0) {
            return -1;
        }
    }
    if (next == 0) {
        if (*previous != KRFFS_NO_NODE && before->type == KRFFS_FREE_NODE) {
            return krffs_head(mount, 0, &beyond);
        }
        return 0;
    }
    if (krffs_head(mount, next, after) != 0) {
        return -1;
    }
    if (after->type == KRFFS_FREE_NODE &&
        krffs_head(mount, krffs_step(mount, next, after->size), &beyond) != 0) {
        return -1;
    }
    return 0;
}

/**
 * `krffs_remove_at`, with `*typed` set once the node's type is stored as
 * free, after which the node is no longer the file whatever else fails.
 *
 * Both neighbours' headers, and the header of the node after a free one
 * that the merge takes in, whose backward link the merge repairs, are read
 * before anything is stored, so that a read that fails is -1 with nothing
 * written, and not a merge passed over as if the neighbour were reserved: a
 * medium that fails one read may pass the next, and a caller that took the
 * removal for a success would not know the medium failed. A repair of that
 * link that still fails is -1 too.
 */
static int krffs_remove_err(krffs_mount_t *mount, uint64_t offset, int *typed)
{
    krffs_node_t head;
    krffs_node_t after;
    krffs_node_t before;
    uint64_t previous;
    uint64_t next;
    uint64_t size;
    int size_changed = 0;
    int rc = 1;

    *typed = 0;
    if (offset == KRFFS_NO_NODE || krffs_head(mount, offset, &head) != 0 ||
        head.type != KRFFS_RESERVED_NODE ||
        krffs_remove_reads(mount, offset, &head, &previous, &before, &after) != 0) {
        return -1;
    }
    next = krffs_next_offset(mount, offset, &head);
    size = head.size;

    if (krffs_put_u8(mount, offset, KRFFS_FIELD_TYPE, KRFFS_FREE_NODE) != 0) {
        return -1;
    }
    *typed = 1;
    if (head.name[0] == '\0' && mount->orphans != 0) {
        mount->orphans--; /* an orphan of this mount, or one reaped at its start */
    }

    if (next != 0 && after.type == KRFFS_FREE_NODE) {
        rc = krffs_merge_size(mount, offset, size + after.size);
        if (rc == 0) {
            size += after.size;
            size_changed = 1;
        }
    }
    if (rc >= 0 && previous != KRFFS_NO_NODE && before.type == KRFFS_FREE_NODE) {
        rc = krffs_merge_size(mount, previous, size + before.size);
        if (rc == 0) {
            size += before.size;
            offset = previous;
            size_changed = 1;
        }
    }

    if (rc < 0 || (size_changed && krffs_fix_backward_link(mount, offset, size) != 0)) {
        mount->stale_links = 1;
        mount->unsettled = 1;
        return -1;
    }
    if (krffs_sync_point(mount) != 0) {
        return -1;
    }
    return 0;
}

/** Free a node and coalesce the free space as described in krffs.h. */
int krffs_remove_at(krffs_mount_t *mount, uint64_t offset)
{
    int typed;

    return krffs_remove_err(mount, offset, &typed);
}

/**
 * Free a nameless node after its last reference goes. A failed removal
 * leaves the mount dirty and halted until the next check. Return 0 or -1.
 */
int krffs_reap_at(krffs_mount_t *mount, uint64_t offset)
{
    int typed;
    uint32_t owed = mount->orphans;

    if (mount->halted || krffs_remove_err(mount, offset, &typed) != 0) {
        mount->orphans = owed ? owed : 1;
        krffs_halt(mount);
        return -1;
    }
    return 0;
}

/**
 * Shrink the file at `offset`, reclaiming a surplus that can hold a header.
 *
 * The shorter length goes first (`krffs_store_length`), since the
 * surplus's header is written inside bytes the old length counts, and a
 * barrier puts it on the medium before the header is written; another puts
 * the header there before the size that publishes it. A surplus whose size
 * would change bytes of two sectors stays inside the file's node. A surplus
 * too small for a header stays there with no barrier, so the mount is left
 * unsettled: the shorter length may be only in the cache, and a write that
 * raises the length again into those bytes puts a barrier first
 * (`krffs_settled`), or its bytes could reach the medium under the old
 * length, which still counts them.
 *
 * Returns 0; 1 with nothing written when the length can be stored in no
 * safe order and the caller must move the file; or -1 on a failed read,
 * write or barrier. A failed size store leaves the links stale
 * (`krffs_publish_size`).
 */
static int krffs_shrink_at(krffs_mount_t *mount, uint64_t offset, uint64_t length,
                           const krffs_node_t *head)
{
    uint64_t size = sizeof(krffs_node_t) + length;
    uint64_t free_size = head->size - size;
    uint64_t surplus = krffs_wrap_offset(mount, offset + size);
    krffs_node_t next;
    int rc;

    if (krffs_head(mount, krffs_next_offset(mount, offset, head), &next) != 0) {
        return -1;
    }
    if (length < head->data_size) {
        rc = krffs_store_length(mount, offset, head->data_size, length, NULL);
        if (rc != 0) {
            return rc;
        }
    }
    if (free_size < sizeof(krffs_node_t)) {
        if (length < head->data_size) {
            mount->unsettled = 1;
        }
        return 0;
    }
    if (krffs_barrier(mount) != 0) {
        return -1;
    }
    if (!krffs_size_fits(mount, offset, head->size, size)) {
        return 0;
    }
    if (next.type == KRFFS_FREE_NODE) {
        free_size += next.size;
    }
    if (krffs_write_free_header(mount, surplus, free_size, size) != 0 ||
        krffs_barrier(mount) != 0 || krffs_publish_size(mount, offset, size) != 0) {
        return -1;
    }
    (void)krffs_fix_backward_link(mount, surplus, free_size);
    return 0;
}

/**
 * Take room from the free node after the node at `offset` for a grow to
 * `size_with_header`, as one size that publishes it.
 *
 * A barrier comes first, since the size that says where the free node begins, a
 * shrink's or a removal's, may still be only in the cache. When the free node
 * can give the room and still keep a header, its tail is split off: a free
 * header written inside the free node, a barrier, and the one size that takes
 * the bytes before it. The header goes at least a header past the free node's
 * own, so that writing it changes nothing the chain reaches yet; a smaller
 * growth takes a header's worth, and the node keeps the rest as room. Otherwise
 * the whole free node is taken by one size. A size that would change bytes of
 * two sectors is not stored. A barrier ends the grow, since the caller then
 * writes bytes over the free node's old header, which could otherwise reach the
 * medium before the size that took it.
 *
 * Returns 0 with the node grown, 1 with the chain unchanged when no size
 * could be stored whole, or -1 when a write or a barrier failed.
 */
static int krffs_grow_into(krffs_mount_t *mount, uint64_t offset, uint64_t size_with_header,
                           const krffs_node_t *head, const krffs_node_t *next_head)
{
    uint64_t whole = head->size + next_head->size;
    uint64_t grown = size_with_header;

    if (grown - head->size < sizeof(krffs_node_t)) {
        grown = head->size + sizeof(krffs_node_t);
    }
    if (krffs_barrier(mount) != 0) {
        return -1;
    }
    if (whole >= grown && whole - grown >= sizeof(krffs_node_t) &&
        krffs_size_fits(mount, offset, head->size, grown)) {
        uint64_t tail = krffs_wrap_offset(mount, offset + grown);

        if (krffs_write_free_header(mount, tail, whole - grown, grown) != 0 ||
            krffs_barrier(mount) != 0 || krffs_publish_size(mount, offset, grown) != 0) {
            return -1;
        }
        (void)krffs_fix_backward_link(mount, tail, whole - grown);
        return krffs_barrier(mount) == 0 ? 0 : -1;
    }
    if (!krffs_size_fits(mount, offset, head->size, whole)) {
        return 1;
    }
    if (krffs_publish_size(mount, offset, whole) != 0) {
        return -1;
    }
    (void)krffs_fix_backward_link(mount, offset, whole);
    return krffs_barrier(mount) == 0 ? 0 : -1;
}

/**
 * Move the node at `offset` to a new allocation of `size` data bytes, with
 * its data cut to fit. If `fill` is supplied, stamp the copy and apply
 * `fill` before publication. Then free the old node.
 *
 * The copy is published by its type before the old node is freed, so a stop
 * between the two leaves one name on two nodes, which the check resolves,
 * and never the file with neither. The old node must go, since a running
 * kernel would walk to it and read what the file used to be, so a removal
 * that fails fails the move.
 *
 * Returns 0, KRFFS_ENOSPACE or KRFFS_EIO. `*live` is the node that holds the
 * file afterwards, where the caller points its holders whatever the answer:
 * the copy once the old node's type is stored as free, even when the
 * removal's merge or sync then failed, and the old node otherwise, in which
 * case the copy is withdrawn (`krffs_withdraw`), so that the running kernel
 * does not find the file's name on two nodes. A barrier ends a move, a
 * failed one too once the old node's type is stored, so that the type is on
 * the medium, or the mount unsettled, before a rename can give the copy
 * another name, which would leave the file under two names.
 */
static int krffs_move(krffs_mount_t *mount, uint64_t offset, uint64_t size,
                      const krffs_fill_t *fill, uint64_t *live)
{
    int err;
    int typed;
    uint64_t moved = krffs_allocate_err(mount, size, offset, fill, &err);

    *live = offset;
    if (moved == KRFFS_NO_NODE) {
        return err;
    }
    if (krffs_remove_err(mount, offset, &typed) == 0) {
        *live = moved;
        return krffs_barrier(mount) == 0 ? 0 : KRFFS_EIO;
    }
    if (typed) {
        *live = moved;
        (void)krffs_barrier(mount);
    } else {
        krffs_withdraw(mount, moved);
    }
    return KRFFS_EIO;
}

/**
 * Grow the node at `offset` to room for `size` data bytes from the free node
 * after it (`krffs_grow_into`), without moving it. Returns 0 with the node
 * grown; 1 with nothing changed when no free node follows, it is too small,
 * or no size could be stored whole, so the node must move; or -1 when a
 * read, a write or a barrier failed.
 */
static int krffs_grow_in_place(krffs_mount_t *mount, uint64_t offset, uint64_t size)
{
    uint64_t size_with_header = sizeof(krffs_node_t) + size;
    krffs_node_t head;
    krffs_node_t next_head;
    uint64_t next;

    if (krffs_head(mount, offset, &head) != 0) {
        return -1;
    }
    if (size_with_header <= head.size) {
        return 0;
    }
    next = krffs_next_offset(mount, offset, &head);
    if (next == 0) {
        return 1;
    }
    if (krffs_head(mount, next, &next_head) != 0) {
        return -1;
    }
    if (next_head.type != KRFFS_FREE_NODE || next_head.size < size_with_header - head.size) {
        return 1;
    }
    return krffs_grow_into(mount, offset, size_with_header, &head, &next_head);
}

/**
 * Prepare a write or truncate whose resulting file length is `length`.
 * Return 0 once there is room and both timestamps have been stored, 1 when
 * the caller must stamp and fill a copy, or -1 on an I/O error.
 */
static int krffs_prepare_data(krffs_mount_t *mount, uint64_t offset, const krffs_node_t *head,
                              uint64_t length)
{
    uint32_t mid;
    int rc;

    if (length != head->data_size &&
        krffs_length_steps(mount, offset, head->data_size, length, &mid) != 0) {
        return 1;
    }
    if (length > head->size - sizeof(*head)) {
        rc = krffs_grow_in_place(mount, offset, length);
        if (rc != 0) {
            return rc;
        }
    }
    return krffs_stamp_at(mount, offset, KRFFS_STAMP_DATA) == 0 ? 0 : -1;
}

/**
 * Give the node at `offset` room for `size` data bytes.
 *
 * A grow takes room from the free node after this one (`krffs_grow_in_place`)
 * or moves the node. A shrink stores the shorter length and gives back the
 * surplus (`krffs_shrink_at`), or moves the node when the length can be
 * stored in no safe order. A surplus that cannot be written back as a free
 * node stays inside the node: space nobody can allocate, and a sound chain.
 * A resize that raises the length inside the room a node already has stores
 * no length; the caller writes the bytes and the length after them, as after
 * a grow.
 *
 * Returns 0; KRFFS_ENOSPACE for a `size` larger than the region, an offset
 * that is no reserved node, or a move that found no room; or KRFFS_EIO when
 * a read, a write or a barrier failed. `*live` is where the node is
 * afterwards, as `krffs_move` sets it.
 */
static int krffs_resize_err(krffs_mount_t *mount, uint64_t offset, uint64_t size, uint64_t *live)
{
    uint64_t size_with_header = sizeof(krffs_node_t) + size;
    krffs_node_t head;
    int rc;

    *live = offset;
    if (size > mount->size || offset == KRFFS_NO_NODE) {
        return KRFFS_ENOSPACE;
    }
    if (krffs_head(mount, offset, &head) != 0) {
        return KRFFS_EIO;
    }
    if (head.type != KRFFS_RESERVED_NODE) {
        return KRFFS_ENOSPACE;
    }
    if (head.size == size_with_header) {
        return 0;
    }

    if (size_with_header > head.size) {
        rc = krffs_grow_in_place(mount, offset, size);
        if (rc < 0) {
            return KRFFS_EIO;
        }
        if (rc > 0) {
            return krffs_move(mount, offset, size, NULL, live);
        }
        return 0;
    }

    rc = krffs_shrink_at(mount, offset, size, &head);
    if (rc > 0) {
        return krffs_move(mount, offset, size, NULL, live);
    }
    return rc < 0 ? KRFFS_EIO : 0;
}

/** Resize a node when the caller needs only the new offset. */
uint64_t krffs_resize_at(krffs_mount_t *mount, uint64_t offset, uint64_t size)
{
    uint64_t live;

    return krffs_resize_err(mount, offset, size, &live) == 0 ? live : KRFFS_NO_NODE;
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
 * they are cleared, so they and the bytes written are on the medium before
 * the length that counts them.
 *
 * Whether the file must move is settled before any byte is written: a write
 * that changed bytes the file has and then failed to move would change the
 * file and answer an error. A moving write puts its bytes in the copy,
 * which nobody reads until its type is written.
 *
 * A failure after a byte became visible is a short write, as Linux's
 * buffered write counts what it committed: the bytes below the length
 * readers see, which a failed length store reports (`krffs_store_length`),
 * or all of them once a moving write's copy is live. KRFFS_EIO only when no
 * byte written can be read.
 */
int krffs_write_at(krffs_mount_t *mount, uint64_t *node, const char *buffer, uint32_t size,
                   uint32_t at)
{
    krffs_node_t head;
    uint64_t offset;
    uint64_t old_data_size;
    uint64_t new_size;
    uint64_t live;
    uint64_t length; /* the length readers see after a failed store */
    uint64_t shown;  /* bytes written below `length` */
    uint32_t done;
    int rc;

    if (!node || *node == KRFFS_NO_NODE || !buffer) {
        return KRFFS_EBAD;
    }
    if (krffs_head(mount, *node, &head) != 0) {
        return KRFFS_EIO;
    }
    if (head.type != KRFFS_RESERVED_NODE) {
        return KRFFS_EBAD;
    }
    offset = *node;
    old_data_size = head.data_size;

    if (size == 0) {
        return 0;
    }
    if (krffs_settled(mount) != 0) {
        return KRFFS_EIO;
    }

    new_size = (uint64_t)at + size;
    rc = krffs_prepare_data(mount, offset, &head,
                            new_size > old_data_size ? new_size : old_data_size);
    if (rc < 0) {
        return KRFFS_EIO;
    }
    if (rc > 0) {
        krffs_fill_t fill;

        fill.buffer = buffer;
        fill.at = at;
        fill.size = size;
        rc = krffs_move(mount, offset, new_size, &fill, &live);
        *node = live;
        if (rc != 0 && live == offset) {
            return rc;
        }
        /* The copy holds every byte, and once it is live readers see them
         * whatever failed after. */
        return (int)size;
    }

    if ((uint64_t)at > old_data_size &&
        krffs_data_fill(mount, offset, old_data_size, 0, (uint64_t)at - old_data_size) != 0) {
        return KRFFS_EIO;
    }
    if (krffs_data_write_long(mount, offset, at, buffer, size, &done) != 0) {
        if (done == 0) {
            return KRFFS_EIO;
        }
        size = done; /* a short write of the bytes that went in */
        new_size = (uint64_t)at + size;
    }
    length = old_data_size;
    if (new_size > old_data_size) {
        rc = krffs_barrier(mount) != 0 ||
             krffs_store_length(mount, offset, old_data_size, new_size, &length) != 0;
    } else {
        rc = krffs_sync_point(mount) != 0;
    }
    if (rc) {
        /* The bytes the length readers see now counts. */
        shown = length > (uint64_t)at ? length - at : 0;
        if (shown == 0) {
            return KRFFS_EIO;
        }
        if (shown < size) {
            size = (uint32_t)shown;
        }
    }
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
 * Publish a new node's name by its first byte. The rest of the name and
 * the node's attributes must reach the medium first, since a sector or a
 * cache block may end anywhere in the name. The node leaves `orphans` once
 * its first byte is stored. 0, or -1 on a write failure.
 */
static int krffs_publish_name(krffs_mount_t *mount, uint64_t node, const uint8_t *name)
{
    if (krffs_put_new(mount, node, KRFFS_FIELD_NAME + 1, name + 1,
                      KRFFS_FILE_NAME_BUFFER_SIZE - 1) != 0 ||
        krffs_barrier(mount) != 0 || krffs_put_u8(mount, node, KRFFS_FIELD_NAME, name[0]) != 0) {
        return -1;
    }
    if (mount->orphans != 0) {
        mount->orphans--;
    }
    return krffs_sync_point(mount);
}

/**
 * Create a named node and stamp its directory. `krffs_publish_name` writes
 * the first byte last, after the other bytes and attributes reach the
 * medium. A stop before it leaves an orphan for repair.
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

    if (krffs_walk_err(mount, path, &rc) != KRFFS_NO_NODE) {
        return 0;
    }
    if (rc == -EIO) {
        return KRFFS_EIO;
    }
    rc = 0;

    krffs_split_path(path, dir, base);
    if (base[0] == '\0') {
        return KRFFS_EBAD;
    }
    parent_id = krffs_resolve_dir_id(mount, dir);
    if (parent_id == (uint64_t)-1) {
        return KRFFS_EBAD;
    }

    node = krffs_allocate_err(mount, 0, KRFFS_NO_NODE, NULL, &rc);
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

    if (rc != 0 || krffs_publish_name(mount, node, name) != 0) {
        return KRFFS_EIO;
    }

    krffs_stamp_at(mount, krffs_parent_offset(mount, node), KRFFS_STAMP_DATA);

    return 0;
}

/**
 * A grow is settled as a write past the end is (`krffs_write_at`): whether
 * the file moves is decided first, and a moving grow zeroes the growth in
 * the copy. Otherwise the growth is zeroed and put on the medium before the
 * length that counts it. A shrink stores its length in `krffs_shrink_at`.
 */
int krffs_truncate_node_at(krffs_mount_t *mount, uint64_t node, uint32_t new_size,
                           uint64_t *node_out)
{
    uint64_t old_data_size;
    uint64_t live;
    krffs_node_t head;
    int rc;

    if (node_out) {
        *node_out = node;
    }
    if (node == KRFFS_NO_NODE || node == 0) {
        return KRFFS_EBAD;
    }
    if (krffs_head(mount, node, &head) != 0) {
        return KRFFS_EIO;
    }
    if (head.type != KRFFS_RESERVED_NODE || KRFFS_IS_DIR(head.mode)) {
        return KRFFS_EBAD;
    }
    if (krffs_settled(mount) != 0) {
        return KRFFS_EIO;
    }

    old_data_size = head.data_size;
    rc = krffs_prepare_data(mount, node, &head, new_size);
    if (rc < 0) {
        return KRFFS_EIO;
    }
    if (rc > 0) {
        krffs_fill_t zeroes;

        zeroes.buffer = NULL;
        zeroes.at = new_size;
        zeroes.size = 0;
        rc = krffs_move(mount, node, new_size, &zeroes, &live);
        if (node_out) {
            *node_out = live;
        }
        return rc;
    }
    if (sizeof(head) + (uint64_t)new_size < head.size &&
        krffs_shrink_at(mount, node, new_size, &head) != 0) {
        return KRFFS_EIO;
    }
    if ((uint64_t)new_size > old_data_size &&
        (krffs_data_fill(mount, node, old_data_size, 0, (uint64_t)new_size - old_data_size) != 0 ||
         krffs_barrier(mount) != 0 ||
         krffs_store_length(mount, node, old_data_size, new_size, NULL) != 0)) {
        return KRFFS_EIO;
    }
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
 * `krffs_walk` never hands `krffs_find_child_err` a component shorter than one
 * character, so a node with an empty name cannot be matched by any path.
 * That is a property of the parser rather than of a hash, which is why an
 * empty name is used and not a sentinel `parent_id`: `krffs_mix_id` may
 * return any nonzero value, so a reserved id would rest on a collision that
 * cannot be ruled out. The scans that enumerate by `parent_id`, not by name,
 * skip an empty name themselves, since being unmatchable covers the resolver
 * and nothing else.
 */
int krffs_orphan_at(krffs_mount_t *mount, uint64_t offset)
{
    uint8_t empty = '\0';

    if (offset == KRFFS_NO_NODE || offset == 0 ||
        krffs_put(mount, offset, KRFFS_FIELD_NAME, &empty, 1) != 0) {
        return -1;
    }
    mount->orphans++;
    return 0;
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

/* The writes of a rename, in `struct krffs_rename`'s `steps`. */
#define KRFFS_STEP_PARENT 1 /* the new parent */
#define KRFFS_STEP_SPARE  2 /* the spare name */
#define KRFFS_STEP_NAME   3 /* the new name */

/* A name write that goes whole, in `struct krffs_rename`'s `orders`; any
 * other value is a row of `krffs_piece_orders`. */
#define KRFFS_ORDER_WHOLE 0xFFu

/* The orders in which the up to three pieces of a field go. */
static const uint8_t krffs_piece_orders[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2},
                                                 {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};

/* The most edges `krffs_field_edges` reports: a sector's and a grain's. */
#define KRFFS_EDGES_MAX 2

/* What a rename settles before it changes anything. */
struct krffs_rename {
    uint64_t node;        /* the file renamed */
    uint64_t existing;    /* the destination file it replaces, or KRFFS_NO_NODE */
    uint64_t parent_id;   /* the directory it enters */
    uint64_t old_parent;  /* the directory it leaves */
    int parent_cut;       /* the parent's changed bytes fall in two units */
    uint8_t parent_order; /* then the row of `krffs_piece_orders` its pieces take */
    uint8_t steps[3];     /* the writes in order, KRFFS_STEP_*, 0 after the last */
    uint8_t orders[3];    /* of each name write, KRFFS_ORDER_WHOLE or a row */
    uint8_t old_name[KRFFS_FILE_NAME_BUFFER_SIZE]; /* the field as the medium has it */
    uint8_t name[KRFFS_FILE_NAME_BUFFER_SIZE];
    uint8_t spare[KRFFS_FILE_NAME_BUFFER_SIZE];
};

/**
 * Where in the field of `size` bytes at `at` a stop can cut the medium
 * (`krffs_cut_before`, sectors and grains): the offsets into the field of
 * those edges, in order, in `edges`, and their count, at most
 * KRFFS_EDGES_MAX; KRFFS_EDGES_MAX + 1 for a field more edges cut, which
 * a sector of 512 bytes or more and a field of 256 or fewer never are.
 */
static uint32_t krffs_field_edges(krffs_mount_t *mount, uint64_t at, uint32_t size, uint32_t *edges)
{
    uint32_t count = 0;
    uint32_t i;

    for (i = 1; i < size; i++) {
        if (krffs_cut_before(mount, (uint32_t)at + i, 1)) {
            if (count == KRFFS_EDGES_MAX) {
                return KRFFS_EDGES_MAX + 1;
            }
            edges[count++] = i;
        }
    }
    return count;
}

/** The bounds [`*from`, `*to`) of piece `piece` of a field of `size` bytes
 * that `count` edges, at `edges`, cut. */
static void krffs_piece(const uint32_t *edges, uint32_t count, uint32_t size, uint32_t piece,
                        uint32_t *from, uint32_t *to)
{
    *from = piece == 0 ? 0 : edges[piece - 1];
    *to = piece == count ? size : edges[piece];
}

/**
 * Settle what a rename of `old_path` to `new_path` can refuse before it
 * looks at the halfway states: a source that is a file, a destination
 * directory and name, a destination that is no directory, and a mount that
 * is settled (`krffs_settled`). A read that fails in the lookup of the
 * destination refuses too, since taking it for no destination would put
 * the name on two nodes. Returns 0 with `*r` filled, 1 when the
 * destination is the source and there is nothing to do, or -1 to refuse.
 */
static int krffs_rename_prepare(krffs_mount_t *mount, const char *old_path, const char *new_path,
                                struct krffs_rename *r)
{
    char dir[KRFFS_FILE_NAME_BUFFER_SIZE];
    char base[KRFFS_FILE_NAME_BUFFER_SIZE];
    krffs_node_t head;
    krffs_brief_t victim;
    int err;

    r->node = krffs_walk(mount, old_path);
    if (r->node == KRFFS_NO_NODE || r->node == 0) {
        klog(KLOG_DEBUG, "krffs", "rename: no such file: %s", old_path);
        return -1;
    }
    if (krffs_settled(mount) != 0 || krffs_head(mount, r->node, &head) != 0 ||
        KRFFS_IS_DIR(head.mode)) {
        return -1;
    }
    krffs_split_path(new_path, dir, base);
    if (base[0] == '\0' ||
        (base[0] == '.' && (base[1] == '\0' || (base[1] == '.' && base[2] == '\0')))) {
        return -1;
    }
    r->parent_id = krffs_resolve_dir_id(mount, dir);
    if (r->parent_id == (uint64_t)-1) {
        return -1;
    }
    r->existing = krffs_find_child_err(mount, r->parent_id, base, (uint32_t)k_strlen(base), &err);
    if (err != 0) {
        return -1;
    }
    if (r->existing == r->node) {
        return 1;
    }
    if (r->existing != KRFFS_NO_NODE &&
        (krffs_brief(mount, r->existing, &victim) != 0 || KRFFS_IS_DIR(victim.mode))) {
        return -1;
    }
    r->old_parent = head.parent_id;
    k_memcpy(r->old_name, head.name, KRFFS_FILE_NAME_BUFFER_SIZE);
    k_memset(r->name, 0, KRFFS_FILE_NAME_BUFFER_SIZE);
    k_memcpy(r->name, base, k_strlen(base));
    return 0;
}

/** Whether directory `dir_id` has a node other than `self` named `name`:
 * 1 or 0, or -1 for a read that failed. */
static int krffs_name_taken(krffs_mount_t *mount, uint64_t dir_id, const uint8_t *name,
                            uint64_t self)
{
    int err;
    uint64_t found = krffs_find_child_err(mount, dir_id, (const char *)name,
                                          (uint32_t)k_strlen((const char *)name), &err);

    if (err != 0) {
        return -1;
    }
    return found != KRFFS_NO_NODE && found != self;
}

/**
 * Whether the node at `node` may stand, if a stop comes now, under the name
 * field `half` in directory `dir`, on its way from `from` to `to`: it is
 * one of the two, or a name that ends within the field, is no "." or "..",
 * and `dir` does not have. 1 or 0, or -1 for a read that failed.
 */
static int krffs_half_ok(krffs_mount_t *mount, uint64_t node, uint64_t dir, const uint8_t *half,
                         const uint8_t *from, const uint8_t *to)
{
    uint32_t len = 0;
    int taken;

    if (krffs_names_match(half, from) || krffs_names_match(half, to)) {
        return 1;
    }
    while (len < KRFFS_FILE_NAME_BUFFER_SIZE && half[len] != '\0') {
        len++;
    }
    if (len == KRFFS_FILE_NAME_BUFFER_SIZE ||
        (half[0] == '.' && (len == 1 || (len == 2 && half[1] == '.')))) {
        return 0;
    }
    taken = krffs_name_taken(mount, dir, half, node);
    return taken < 0 ? -1 : !taken;
}

/**
 * Whether the node at `node`, in directory `dir`, can go from the name
 * field `from` to `to` without showing a name another node of `dir` has.
 * A field whose changed bytes no edge cuts (`krffs_field_edges`) is
 * written whole. Otherwise it goes in pieces between the edges, and a stop
 * between two shows a halfway name, part of each, which must pass
 * `krffs_half_ok`; the first order of the pieces whose every halfway name
 * passes is taken. Returns 1 with `*order` set (KRFFS_ORDER_WHOLE or a row
 * of `krffs_piece_orders`), 0 when no order will do, or -1 for a read that
 * failed.
 */
static int krffs_name_step(krffs_mount_t *mount, uint64_t node, uint64_t dir, const uint8_t *from,
                           const uint8_t *to, uint8_t *order)
{
    uint8_t half[KRFFS_FILE_NAME_BUFFER_SIZE];
    uint32_t edges[KRFFS_EDGES_MAX];
    uint32_t count =
        krffs_field_edges(mount, node + KRFFS_FIELD_NAME, KRFFS_FILE_NAME_BUFFER_SIZE, edges);
    uint32_t pieces = 0;
    uint32_t row;
    uint32_t k;
    uint32_t lo;
    uint32_t hi;
    int ok;

    *order = KRFFS_ORDER_WHOLE;
    if (count > KRFFS_EDGES_MAX) {
        return 0;
    }
    for (k = 0; k <= count; k++) {
        krffs_piece(edges, count, KRFFS_FILE_NAME_BUFFER_SIZE, k, &lo, &hi);
        while (lo < hi && from[lo] == to[lo]) {
            lo++;
        }
        pieces += lo < hi;
    }
    if (pieces < 2) {
        return 1;
    }
    for (row = 0; row < 6; row++) {
        k_memcpy(half, from, KRFFS_FILE_NAME_BUFFER_SIZE);
        ok = 1;
        for (k = 0; k < 3 && ok > 0; k++) {
            if (krffs_piece_orders[row][k] > count) {
                continue;
            }
            krffs_piece(edges, count, KRFFS_FILE_NAME_BUFFER_SIZE, krffs_piece_orders[row][k], &lo,
                        &hi);
            k_memcpy(half + lo, to + lo, hi - lo);
            ok = krffs_half_ok(mount, node, dir, half, from, to);
        }
        if (ok < 0) {
            return -1;
        }
        if (ok > 0) {
            *order = (uint8_t)row;
            return 1;
        }
    }
    return 0;
}

/**
 * Plan a rename across directories as the parent write and the name write,
 * the parent first when `parent_first`: the state between them, the old
 * name in the new directory or the new name in the old one, must show a
 * name its directory does not have, and the name write must pass
 * `krffs_name_step` in the directory it happens in. 1 with the steps set,
 * 0 when the order will not do, or -1 for a read that failed.
 */
static int krffs_plan_order(krffs_mount_t *mount, struct krffs_rename *r, int parent_first)
{
    uint64_t dir = parent_first ? r->parent_id : r->old_parent;
    int rc = krffs_name_taken(mount, dir, parent_first ? r->old_name : r->name, r->node);

    if (rc != 0) {
        return rc < 0 ? -1 : 0;
    }
    rc = krffs_name_step(mount, r->node, dir, r->old_name, r->name, &r->orders[parent_first]);
    if (rc <= 0) {
        return rc;
    }
    r->steps[0] = parent_first ? KRFFS_STEP_PARENT : KRFFS_STEP_NAME;
    r->steps[1] = parent_first ? KRFFS_STEP_NAME : KRFFS_STEP_PARENT;
    return 1;
}

/**
 * Plan a rename by way of a spare name, for one whose direct writes would
 * show a name already there: the spare, the parent when it changes, then
 * the new name. A spare is a dot, "rename-", the node's offset in hex and
 * one more digit, of which the first that neither directory has and that
 * both name writes can reach (`krffs_name_step`) is taken. 1 with the
 * steps set, 0 when none of the sixteen will do, or -1 for a read that
 * failed.
 */
static int krffs_plan_spare(krffs_mount_t *mount, struct krffs_rename *r)
{
    static const char digits[] = "0123456789abcdef";
    static const char stem[] = ".rename-";
    uint32_t last = r->parent_id == r->old_parent ? 1 : 2;
    uint32_t n;
    uint32_t i;
    int shift;
    int rc;

    for (n = 0; n < 16; n++) {
        k_memset(r->spare, 0, KRFFS_FILE_NAME_BUFFER_SIZE);
        k_memcpy(r->spare, stem, sizeof(stem) - 1);
        i = sizeof(stem) - 1;
        for (shift = 28; shift >= 0; shift -= 4) {
            r->spare[i++] = (uint8_t)digits[((uint32_t)r->node >> shift) & 15u];
        }
        r->spare[i] = (uint8_t)digits[n];
        rc = krffs_name_taken(mount, r->old_parent, r->spare, r->node);
        if (rc == 0) {
            rc = krffs_name_taken(mount, r->parent_id, r->spare, r->node);
        }
        if (rc == 0) {
            rc = 1 - krffs_name_step(mount, r->node, r->old_parent, r->old_name, r->spare,
                                     &r->orders[0]);
        }
        if (rc == 0) {
            rc = 1 -
                 krffs_name_step(mount, r->node, r->parent_id, r->spare, r->name, &r->orders[last]);
        }
        if (rc < 0 || rc == 2) {
            return -1;
        }
        if (rc == 0) {
            r->steps[0] = KRFFS_STEP_SPARE;
            r->steps[1] = KRFFS_STEP_PARENT;
            r->steps[last] = KRFFS_STEP_NAME;
            return 1;
        }
    }
    return 0;
}

/** Whether `id` is the root's or a directory's: 1 or 0, or -1 for a read
 * that failed. */
static int krffs_dir_id_known(krffs_mount_t *mount, uint64_t id)
{
    uint64_t offset = 0;
    uint64_t steps = 0;
    uint64_t bound = krffs_max_steps(mount);
    krffs_brief_t brief;

    if (id == KRFFS_ROOT_ID) {
        return 1;
    }
    do {
        if (krffs_brief(mount, offset, &brief) != 0) {
            return -1;
        }
        if (brief.type == KRFFS_RESERVED_NODE && KRFFS_IS_DIR(brief.mode) && brief.id == id) {
            return 1;
        }
        offset = krffs_step(mount, offset, brief.size);
    } while (offset != 0 && ++steps < bound);
    return 0;
}

/* A test of whether `id` is the root's or a directory's: `known` answers 1
 * or 0, or -1 for a read that failed, given `ctx`. */
struct krffs_dir_test {
    int (*known)(krffs_mount_t *mount, void *ctx, uint64_t id);
    void *ctx;
};

/** A `struct krffs_dir_test` by a walk of the chain (`krffs_dir_id_known`). */
static int krffs_dir_scan(krffs_mount_t *mount, void *ctx, uint64_t id)
{
    (void)ctx;
    return krffs_dir_id_known(mount, id);
}

/**
 * Choose the order of the pieces of a parent field at `at` that goes from
 * `from` to `to` in pieces between its edges (`krffs_field_edges`): the
 * first row of `krffs_piece_orders` whose every halfway parent, part of
 * each value, is `from`, `to` or names no directory by `test`, the root
 * included, since the repair takes a file under a directory's id as in
 * that directory and would not move it on. Returns 0 with `*order` set, 1
 * when no order will do, or -1 for a read that failed.
 */
static int krffs_parent_pieces(krffs_mount_t *mount, uint64_t at, uint64_t from, uint64_t to,
                               const struct krffs_dir_test *test, uint8_t *order)
{
    uint8_t half[sizeof(uint64_t)];
    uint32_t edges[KRFFS_EDGES_MAX];
    uint32_t count = krffs_field_edges(mount, at, (uint32_t)sizeof(uint64_t), edges);
    uint64_t value;
    uint32_t row;
    uint32_t k;
    uint32_t lo;
    uint32_t hi;
    int hit;

    for (row = 0; row < 6 && count <= KRFFS_EDGES_MAX; row++) {
        k_memcpy(half, &from, sizeof(half));
        hit = 0;
        for (k = 0; k < 2 && !hit; k++) {
            if (krffs_piece_orders[row][k] > count) {
                continue;
            }
            krffs_piece(edges, count, (uint32_t)sizeof(half), krffs_piece_orders[row][k], &lo, &hi);
            k_memcpy(half + lo, (const uint8_t *)&to + lo, hi - lo);
            k_memcpy(&value, half, sizeof(value));
            if (value != from && value != to) {
                hit = test->known(mount, test->ctx, value);
            }
        }
        if (hit < 0) {
            return -1;
        }
        if (!hit) {
            *order = (uint8_t)row;
            return 0;
        }
    }
    return 1;
}

/**
 * Choose the order of the pieces of a parent write the plan found cut
 * (`krffs_parent_pieces`). 0 with `parent_order` set, or -1 when no order
 * will do or a read failed.
 */
static int krffs_parent_order(krffs_mount_t *mount, struct krffs_rename *r)
{
    static const struct krffs_dir_test scan = {krffs_dir_scan, NULL};

    return krffs_parent_pieces(mount, r->node + KRFFS_FIELD_PARENT, r->old_parent, r->parent_id,
                               &scan, &r->parent_order) == 0
               ? 0
               : -1;
}

/**
 * Settle how the parent write goes. A parent whose changed bytes an edge
 * cuts goes in pieces, which a stop can leave naming no directory, so the
 * new parent is first written into the id field, which nothing reads of a
 * file but the repair that puts such a file into the directory its id
 * names (`krffs_repair_parents`). That field must then take the new parent
 * whole; the two fields are 346 bytes apart, so one sector's edge cuts at
 * most one of them, and only a grain's edge in the other refuses. The
 * pieces go in an order whose halfway parents name no directory
 * (`krffs_parent_order`). Returns 0, or -1 to refuse, or for a read that
 * failed.
 */
static int krffs_plan_parent(krffs_mount_t *mount, struct krffs_rename *r)
{
    uint64_t at = r->node + KRFFS_FIELD_PARENT;

    r->parent_cut = 0;
    if (r->parent_id == r->old_parent ||
        krffs_write_fits(mount, at, &r->parent_id, (uint32_t)sizeof(r->parent_id), 1)) {
        return 0;
    }
    r->parent_cut = 1;
    if (!krffs_write_fits(mount, r->node + KRFFS_FIELD_ID, &r->parent_id,
                          (uint32_t)sizeof(r->parent_id), 1)) {
        klog(KLOG_WARN, "krffs",
             "rename: refused: both the parent and the id of the node at %d span two units",
             (int)r->node);
        return -1;
    }
    return krffs_parent_order(mount, r);
}

/**
 * Plan the writes of a rename (`krffs_rename_err`): a name alone within one
 * directory, a parent alone for one name, else the parent first or the
 * name first; failing those, by way of a spare name. The first whose
 * halfway states show no name already there is taken. The destination's
 * name is the new name, which no halfway state shows, so the plan holds
 * once the destination is gone. Returns 0, or -1 when no order will do or
 * a read failed.
 */
static int krffs_rename_plan(krffs_mount_t *mount, struct krffs_rename *r)
{
    int rc;

    k_memset(r->steps, 0, sizeof(r->steps));
    k_memset(r->orders, KRFFS_ORDER_WHOLE, sizeof(r->orders));
    if (krffs_plan_parent(mount, r) != 0) {
        return -1;
    }
    if (r->parent_id == r->old_parent) {
        r->steps[0] = KRFFS_STEP_NAME;
        rc = krffs_name_step(mount, r->node, r->old_parent, r->old_name, r->name, &r->orders[0]);
    } else if (krffs_names_match(r->old_name, r->name)) {
        r->steps[0] = KRFFS_STEP_PARENT;
        return 0;
    } else {
        rc = krffs_plan_order(mount, r, 1);
        if (rc == 0) {
            rc = krffs_plan_order(mount, r, 0);
        }
    }
    if (rc == 0) {
        rc = krffs_plan_spare(mount, r);
    }
    return rc > 0 ? 0 : -1;
}

/**
 * Write the bytes of the `size`-byte field at `at` that `value` holds, in
 * the pieces its edges make (`krffs_field_edges`), in the order of row
 * `order` of `krffs_piece_orders`, with a barrier after each. 0 or -1.
 */
static int krffs_put_pieces(krffs_mount_t *mount, uint64_t at, const uint8_t *value, uint32_t size,
                            uint8_t order)
{
    uint32_t edges[KRFFS_EDGES_MAX];
    uint32_t count = krffs_field_edges(mount, at, size, edges);
    uint32_t k;
    uint32_t lo;
    uint32_t hi;

    if (count > KRFFS_EDGES_MAX) {
        return -1;
    }
    for (k = 0; k < 3; k++) {
        if (krffs_piece_orders[order][k] > count) {
            continue;
        }
        krffs_piece(edges, count, size, krffs_piece_orders[order][k], &lo, &hi);
        if (krffs_backend_write(mount, at + lo, value + lo, hi - lo) != 0 ||
            krffs_barrier(mount) != 0) {
            return -1;
        }
    }
    return 0;
}

/**
 * Store `name` and its hash as the node's id, then a barrier, the name
 * whole or in the pieces `order` gives (`krffs_name_step`). The id goes
 * without the whole-unit rule: nothing finds a file by it, and a torn one
 * changes only the boot root's inode number, which a rename changes
 * anyway. 0 or -1.
 */
static int krffs_rename_name(krffs_mount_t *mount, uint64_t node, const uint8_t *name,
                             uint8_t order)
{
    uint64_t id = krffs_calculate_djb_hash(name);

    if (krffs_backend_write(mount, node + KRFFS_FIELD_ID, &id, (uint32_t)sizeof(id)) != 0) {
        return -1;
    }
    if (order != KRFFS_ORDER_WHOLE) {
        return krffs_put_pieces(mount, node + KRFFS_FIELD_NAME, name, KRFFS_FILE_NAME_BUFFER_SIZE,
                                order);
    }
    if (krffs_put(mount, node, KRFFS_FIELD_NAME, name, KRFFS_FILE_NAME_BUFFER_SIZE) != 0) {
        return -1;
    }
    return krffs_barrier(mount);
}

/**
 * Store the node's parent, then a barrier. The plan has checked that a
 * parent it did not find cut reaches the medium whole, so the write goes
 * straight to the backend: a check now, after the destination has gone,
 * could fail on a read and leave the rename half made. A parent the plan
 * found cut (`krffs_plan_parent`) goes in pieces after the new parent has
 * reached the id field, and the id then goes back to the hash of `name`,
 * the name the field holds. 0 or -1.
 */
static int krffs_rename_parent(krffs_mount_t *mount, const struct krffs_rename *r,
                               const uint8_t *name)
{
    uint64_t id = krffs_calculate_djb_hash(name);

    if (!r->parent_cut) {
        if (krffs_backend_write(mount, r->node + KRFFS_FIELD_PARENT, &r->parent_id,
                                (uint32_t)sizeof(r->parent_id)) != 0) {
            return -1;
        }
        return krffs_barrier(mount);
    }
    if (krffs_backend_write(mount, r->node + KRFFS_FIELD_ID, &r->parent_id,
                            (uint32_t)sizeof(r->parent_id)) != 0 ||
        krffs_barrier(mount) != 0 ||
        krffs_put_pieces(mount, r->node + KRFFS_FIELD_PARENT, (const uint8_t *)&r->parent_id,
                         (uint32_t)sizeof(r->parent_id), r->parent_order) != 0) {
        return -1;
    }
    return krffs_backend_write(mount, r->node + KRFFS_FIELD_ID, &id, (uint32_t)sizeof(id));
}

/** Make the planned writes of a rename, in order; 0 or -1. */
static int krffs_rename_run(krffs_mount_t *mount, const struct krffs_rename *r)
{
    const uint8_t *held = r->old_name;
    uint32_t i;
    int rc;

    for (i = 0; i < 3 && r->steps[i] != 0; i++) {
        if (r->steps[i] == KRFFS_STEP_PARENT) {
            rc = krffs_rename_parent(mount, r, held);
        } else {
            held = r->steps[i] == KRFFS_STEP_SPARE ? r->spare : r->name;
            rc = krffs_rename_name(mount, r->node, held, r->orders[i]);
        }
        if (rc != 0) {
            return -1;
        }
    }
    return 0;
}

/**
 * A rename stores the new parent and the new name in separate writes, and a
 * stop between two of them leaves the file under a halfway state: the new
 * parent with the old name, the old parent with the new name, a spare name,
 * part of one name and part of the other where an edge of the medium cuts
 * the name field, or a parent that names no directory, which the repair
 * undoes. A halfway state that showed a name already there would put two
 * nodes under one name, which the repair resolves by clearing one of them,
 * an unrelated file among them; so every one is checked against the
 * directory before anything is written (`krffs_rename_plan`), and the
 * destination goes, with a barrier, only once the rename is sure to try. A
 * write that fails among the rename's own leaves such a state in the
 * running system, perhaps a file in no directory whose name a new file
 * could then take, so it halts the mount (`krffs_halt`) until the next
 * mount's repair. While the writes run, which sleep in their barriers,
 * `check_owed` is set, so that a shutdown or an unmount that comes
 * meanwhile keeps the dirty flag for that repair.
 */
int krffs_rename_err(krffs_mount_t *mount, const char *old_path, const char *new_path,
                     int (*held)(void *arg, uint64_t offset), void *arg, uint64_t *orphaned)
{
    struct krffs_rename r;
    int rc = krffs_rename_prepare(mount, old_path, new_path, &r);
    int owed = mount->check_owed;
    int keep;

    if (orphaned) {
        *orphaned = KRFFS_NO_NODE;
    }
    if (rc != 0) {
        return rc > 0 ? 0 : -1;
    }
    if (krffs_rename_plan(mount, &r) != 0) {
        return -1;
    }
    mount->check_owed = 1;
    if (r.existing != KRFFS_NO_NODE) {
        keep = held && held(arg, r.existing);
        rc = keep ? krffs_orphan_at(mount, r.existing) : krffs_remove_at(mount, r.existing);
        if (rc != 0) {
            mount->check_owed = owed;
            return -1;
        }
        if (keep && orphaned) {
            *orphaned = r.existing;
        }
    }
    if (krffs_barrier(mount) != 0) {
        mount->check_owed = owed;
        return -1;
    }
    krffs_stamp_at(mount, krffs_parent_offset(mount, r.node), KRFFS_STAMP_DATA);
    if (krffs_rename_run(mount, &r) != 0) {
        krffs_halt(mount);
        return -1;
    }
    mount->check_owed = owed;
    krffs_stamp_at(mount, krffs_parent_offset(mount, r.node), KRFFS_STAMP_DATA);
    krffs_stamp_at(mount, r.node, KRFFS_STAMP_ATTR);
    return 0;
}

int krffs_rename_at(krffs_mount_t *mount, const char *old_path, const char *new_path)
{
    return krffs_rename_err(mount, old_path, new_path, NULL, NULL, NULL);
}

/**
 * Whether a directory on the medium already has the logical id `id`.
 *
 * Two names can mix to one id. A second directory with a taken id would share
 * its children with the first, since a child names its parent by id alone, so
 * the creation fails instead. A directory with no name does not count. 1 or
 * 0, or -1 for a read that failed, which the creation refuses too, since the
 * node it could not read may have the id.
 */
static int krffs_id_taken(krffs_mount_t *mount, uint64_t id)
{
    uint64_t scan = 0;
    uint64_t steps = 0;
    uint64_t bound = krffs_max_steps(mount);
    krffs_brief_t entry;

    do {
        if (krffs_brief(mount, scan, &entry) != 0) {
            return -1;
        }
        if (entry.type == KRFFS_RESERVED_NODE && KRFFS_IS_DIR(entry.mode) && entry.id == id) {
            int unnamed = krffs_name_is(mount, scan, "", 0);

            if (unnamed < 0) {
                return -1;
            }
            if (!unnamed) {
                return 1;
            }
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

    if (krffs_walk_err(mount, path, &rc) != KRFFS_NO_NODE) {
        return KRFFS_EBAD;
    }
    if (rc == -EIO) {
        return KRFFS_EIO;
    }
    rc = 0;

    krffs_split_path(path, dir, base);
    if (base[0] == '\0') {
        return KRFFS_EBAD;
    }
    parent_id = krffs_resolve_dir_id(mount, dir);
    if (parent_id == (uint64_t)-1) {
        return KRFFS_EBAD;
    }

    rc = krffs_id_taken(mount, krffs_mix_id(parent_id, base));
    if (rc != 0) {
        return rc < 0 ? KRFFS_EIO : KRFFS_ETAKEN;
    }

    node = krffs_allocate_err(mount, 0, KRFFS_NO_NODE, NULL, &rc);
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
    if (rc != 0 || krffs_publish_name(mount, node, name) != 0) {
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
            !krffs_hidden(mount, scan) && krffs_has_name(mount, scan)) {
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
 * The pass is one bounded walk. After removing a node, step over the
 * resulting free region from its start, the previous node if it absorbed
 * this one. Check its new size, since an unsafe merge leaves both nodes.
 * That is what lets this resume in place instead of restarting at the root
 * after each removal, which would be a scan per orphan and, on a disk larger
 * than the page cache, a great many polled reads at boot.
 *
 * The first read, name read or removal that fails ends the walk, counts the
 * node it was at in `orphans` and halts the mount (`krffs_halt`), which owes
 * the check: a medium that fails a read or a write takes no more writes, as
 * Linux's ext4 turns read-only after an error, and the dirty flag stays for
 * the next mount, which scans again.
 */
uint32_t krffs_reap_orphans_at(krffs_mount_t *mount)
{
    uint64_t previous = 0;
    uint64_t node;
    uint64_t steps = 0;
    uint64_t bound = krffs_max_steps(mount);
    uint32_t reaped = 0;
    int failed = 0;
    krffs_brief_t head;
    krffs_brief_t before;

    if (mount->halted) {
        return 0;
    }
    if (krffs_brief(mount, previous, &head) != 0) {
        mount->orphans++;
        krffs_halt(mount);
        return 0;
    }
    node = krffs_step(mount, previous, head.size);
    while (node != 0 && steps < bound) {
        int unnamed;

        steps++;
        if (krffs_brief(mount, node, &head) != 0) {
            failed = 1;
            break;
        }
        unnamed = head.type == KRFFS_RESERVED_NODE ? krffs_name_is(mount, node, "", 0) : 0;
        if (unnamed < 0) {
            failed = 1;
            break;
        }
        if (unnamed) {
            if (krffs_reap_at(mount, node) != 0) {
                return reaped;
            }
            reaped++;
            if (krffs_brief(mount, previous, &before) != 0) {
                failed = 1;
                break;
            }
            if (before.size > node - previous) {
                node = previous;
            }
            if (krffs_brief(mount, node, &head) != 0) {
                failed = 1;
                break;
            }
        }
        previous = node;
        node = krffs_step(mount, node, head.size);
    }
    if (failed) {
        mount->orphans++; /* after the removals, which count orphans down */
        krffs_halt(mount);
    }
    return reaped;
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
 * The most named nodes the search for one name on two nodes indexes: 16
 * bytes each, 4 MiB of index at the most. A volume with more is left to
 * `fsck.krffs`, and the mount says so. A host test may define a smaller
 * bound to carry a small volume down that path.
 */
#ifndef KRFFS_REPAIR_NAMES
#define KRFFS_REPAIR_NAMES 262144u
#endif

/* A named node in the index of `krffs_repair_names`. */
typedef struct krffs_name_key {
    uint64_t parent_id;
    uint32_t hash;   /* the low half of the name's DJB hash */
    uint32_t offset; /* the node's */
} krffs_name_key_t;

/* One past the largest 32-bit name hash: the end of the range searched. */
#define KRFFS_HASH_END ((uint64_t)1 << 32)

/** Whether `a` sorts before `b`: by directory, by name hash, by offset. */
static int krffs_key_before(const krffs_name_key_t *a, const krffs_name_key_t *b)
{
    if (a->parent_id != b->parent_id) {
        return a->parent_id < b->parent_id;
    }
    if (a->hash != b->hash) {
        return a->hash < b->hash;
    }
    return a->offset < b->offset;
}

/** Sift `keys[root]` down the heap of the first `count` keys. */
static void krffs_sift_key(krffs_name_key_t *keys, uint32_t root, uint32_t count)
{
    for (;;) {
        uint32_t child = 2 * root + 1;
        krffs_name_key_t swap;

        if (child >= count) {
            return;
        }
        if (child + 1 < count && krffs_key_before(&keys[child], &keys[child + 1])) {
            child++;
        }
        if (!krffs_key_before(&keys[root], &keys[child])) {
            return;
        }
        swap = keys[root];
        keys[root] = keys[child];
        keys[child] = swap;
        root = child;
    }
}

/** Sort `count` keys in place by `krffs_key_before`, a heap sort, which
 * needs no memory and no recursion. */
static void krffs_sort_keys(krffs_name_key_t *keys, uint32_t count)
{
    krffs_name_key_t swap;
    uint32_t i;

    for (i = count / 2; i > 0; i--) {
        krffs_sift_key(keys, i - 1, count);
    }
    for (i = count; i > 1; i--) {
        swap = keys[0];
        keys[0] = keys[i - 1];
        keys[i - 1] = swap;
        krffs_sift_key(keys, 0, i - 1);
    }
}

/**
 * Of the reserved nodes `a` at `at` and `b` at `bt`, `a` first in the ring,
 * both with one name in one directory, the offset of the one that loses the
 * name. A directory keeps it over a file, since its children name it by its
 * id. Otherwise the node with more data keeps it: an interrupted move left
 * the two, and the copy of a grow holds the original's bytes and what the
 * grow added, the original of a shrink the bytes its copy left out. The
 * node sizes cannot decide, since a copy may take a whole free node larger
 * than the original. Of two with equal data the larger keeps it, and then
 * the first.
 */
static uint64_t krffs_name_loser(const krffs_node_t *a, uint64_t at, const krffs_node_t *b,
                                 uint64_t bt)
{
    if (KRFFS_IS_DIR(a->mode) != KRFFS_IS_DIR(b->mode)) {
        return KRFFS_IS_DIR(a->mode) ? bt : at;
    }
    if (a->data_size != b->data_size) {
        return b->data_size > a->data_size ? at : bt;
    }
    return b->size > a->size ? at : bt;
}

/** Count the named reserved nodes into `*named`; 0, or -1 for a read that
 * failed. */
static int krffs_count_names(krffs_mount_t *mount, uint32_t *named)
{
    uint64_t offset = 0;
    uint64_t steps = 0;
    uint64_t bound = krffs_max_steps(mount);
    krffs_brief_t brief;

    *named = 0;
    do {
        if (krffs_brief(mount, offset, &brief) != 0) {
            return -1;
        }
        if (brief.type == KRFFS_RESERVED_NODE) {
            int empty = krffs_name_is(mount, offset, "", 0);

            if (empty < 0) {
                return -1;
            }
            *named += !empty;
        }
        offset = krffs_step(mount, offset, brief.size);
    } while (offset != 0 && ++steps < bound);
    return 0;
}

/* What the name comparisons of one run read through, and whether a read
   failed. */
struct krffs_run {
    krffs_mount_t *mount;
    int failed;
};

/** Whether `a` sorts before `b` by the names on the medium, then by offset;
 * a read that fails sets `run->failed`. */
static int krffs_name_before(struct krffs_run *run, const krffs_name_key_t *a,
                             const krffs_name_key_t *b)
{
    uint8_t x[KRFFS_FILE_NAME_BUFFER_SIZE];
    uint8_t y[KRFFS_FILE_NAME_BUFFER_SIZE];
    krffs_mount_t *mount = run->mount;
    uint32_t i;

    if (mount->backend->read(mount->ctx, a->offset + KRFFS_FIELD_NAME, x, sizeof(x)) != 0 ||
        mount->backend->read(mount->ctx, b->offset + KRFFS_FIELD_NAME, y, sizeof(y)) != 0) {
        run->failed = 1;
        return 0;
    }
    for (i = 0; i < sizeof(x); i++) {
        if (x[i] != y[i]) {
            return x[i] < y[i];
        }
        if (x[i] == '\0') {
            break;
        }
    }
    return a->offset < b->offset;
}

/** Sift `keys[root]` down the heap of the first `count` keys by name. */
static void krffs_sift_name(struct krffs_run *run, krffs_name_key_t *keys, uint32_t root,
                            uint32_t count)
{
    while (!run->failed) {
        uint32_t child = 2 * root + 1;
        krffs_name_key_t swap;

        if (child >= count) {
            return;
        }
        if (child + 1 < count && krffs_name_before(run, &keys[child], &keys[child + 1])) {
            child++;
        }
        if (!krffs_name_before(run, &keys[root], &keys[child])) {
            return;
        }
        swap = keys[root];
        keys[root] = keys[child];
        keys[child] = swap;
        root = child;
    }
}

/**
 * Resolve one run of `count` keys of one directory and name hash: sort it
 * by name, a heap sort that reads each name it compares, so that a run of
 * many names of one hash costs n log n reads, and clear the name of each
 * node that loses a name to the one before it with the same name
 * (`krffs_name_loser`), counting it in `*doubles`. Returns 0, or -1 for a
 * read or a write that failed.
 */
static int krffs_resolve_run(krffs_mount_t *mount, krffs_name_key_t *keys, uint32_t count,
                             uint32_t *doubles)
{
    struct krffs_run run;
    krffs_name_key_t swap;
    krffs_node_t first;
    krffs_node_t second;
    uint8_t empty = '\0';
    uint32_t win = 0;
    uint32_t i;

    run.mount = mount;
    run.failed = 0;
    for (i = count / 2; i > 0; i--) {
        krffs_sift_name(&run, keys, i - 1, count);
    }
    for (i = count; i > 1; i--) {
        swap = keys[0];
        keys[0] = keys[i - 1];
        keys[i - 1] = swap;
        krffs_sift_name(&run, keys, 0, i - 1);
    }
    if (run.failed) {
        return -1;
    }
    for (i = 1; i < count; i++) {
        const krffs_name_key_t *a = keys[win].offset < keys[i].offset ? &keys[win] : &keys[i];
        const krffs_name_key_t *b = a == &keys[win] ? &keys[i] : &keys[win];
        uint64_t loser;

        if (krffs_head(mount, a->offset, &first) != 0 ||
            krffs_head(mount, b->offset, &second) != 0) {
            return -1;
        }
        if (!krffs_names_match(first.name, second.name)) {
            win = i;
            continue;
        }
        loser = krffs_name_loser(&first, a->offset, &second, b->offset);
        if (krffs_put(mount, loser, KRFFS_FIELD_NAME, &empty, 1) != 0) {
            return -1;
        }
        (*doubles)++;
        if (loser == keys[win].offset) {
            win = i;
        }
    }
    return 0;
}

/** Sift `keys[root]` down the heap of the first `count` keys, largest name
 * hash on top. */
static void krffs_sift_hash(krffs_name_key_t *keys, uint32_t root, uint32_t count)
{
    for (;;) {
        uint32_t child = 2 * root + 1;
        krffs_name_key_t swap;

        if (child >= count) {
            return;
        }
        if (child + 1 < count && keys[child].hash < keys[child + 1].hash) {
            child++;
        }
        if (keys[root].hash >= keys[child].hash) {
            return;
        }
        swap = keys[root];
        keys[root] = keys[child];
        keys[child] = swap;
        root = child;
    }
}

/**
 * Put in `keys` the `cap` named reserved nodes with the smallest name
 * hashes of at least `lo`, kept as a heap with the largest on top, and set
 * `*count` to how many it holds and `*omitted` to the smallest hash of a
 * node at or above `lo` it left out, one it passed over or one it put out
 * of the heap for a smaller, or KRFFS_HASH_END when it left out none.
 * Returns 0, or -1 for a read that failed.
 */
static int krffs_select_names(krffs_mount_t *mount, krffs_name_key_t *keys, uint32_t cap,
                              uint64_t lo, uint32_t *count, uint64_t *omitted)
{
    uint64_t offset = 0;
    uint64_t steps = 0;
    uint64_t bound = krffs_max_steps(mount);
    krffs_name_key_t key;
    krffs_node_t head;
    uint32_t i;

    *count = 0;
    *omitted = KRFFS_HASH_END;
    do {
        if (krffs_head(mount, offset, &head) != 0) {
            return -1;
        }
        if (head.type == KRFFS_RESERVED_NODE && head.name[0] != '\0') {
            key.parent_id = head.parent_id;
            key.hash = (uint32_t)krffs_calculate_djb_hash(head.name);
            key.offset = (uint32_t)offset;
            if (key.hash >= lo && *count < cap) {
                for (i = (*count)++; i > 0 && keys[(i - 1) / 2].hash < key.hash; i = (i - 1) / 2) {
                    keys[i] = keys[(i - 1) / 2];
                }
                keys[i] = key;
            } else if (key.hash >= lo) {
                uint32_t out = key.hash;

                if (key.hash < keys[0].hash) {
                    out = keys[0].hash;
                    keys[0] = key;
                    krffs_sift_hash(keys, 0, *count);
                }
                if (out < *omitted) {
                    *omitted = out;
                }
            }
        }
        offset = krffs_step(mount, offset, head.size);
    } while (offset != 0 && ++steps < bound);
    return 0;
}

/**
 * Sort the `count` keys and resolve each run of one directory and name
 * hash (`krffs_resolve_run`). Returns 0, or -1 for a read or a write that
 * failed.
 */
static int krffs_resolve_keys(krffs_mount_t *mount, krffs_name_key_t *keys, uint32_t count,
                              uint32_t *doubles)
{
    uint32_t i, end;

    krffs_sort_keys(keys, count);
    for (i = 0; i < count; i = end) {
        end = i + 1;
        while (end < count && keys[end].parent_id == keys[i].parent_id &&
               keys[end].hash == keys[i].hash) {
            end++;
        }
        if (end - i > 1 && krffs_resolve_run(mount, keys + i, end - i, doubles) != 0) {
            return -1;
        }
    }
    return 0;
}

/**
 * Clear the name of each node that loses a name it has with another node of
 * its directory (`krffs_name_loser`); a pass of `krffs_reap_orphans_at`
 * frees it. The named nodes are indexed by directory and name hash, 16
 * bytes each, and sorted, so that only nodes whose hashes agree are
 * compared, and those by sorting on their names. The index holds at most
 * KRFFS_REPAIR_NAMES nodes, fewer when memory is short. A volume with more
 * is searched in passes: each takes the nodes with the smallest hashes
 * from where the last stopped, and resolves those below the smallest hash
 * it left out, every node of which it holds, so that the next pass starts
 * at that hash. Only a single hash value with more nodes than the index
 * holds, or no memory for even a small index, leaves the search to
 * `fsck.krffs` with `check_owed` set, which keeps the dirty flag. Returns
 * 0 with `*doubles` counting the names cleared, or -1 for a read or a write
 * that failed.
 */
static int krffs_repair_names(krffs_mount_t *mount, uint32_t *doubles)
{
    uint32_t named;
    uint32_t cap;
    uint32_t count;
    uint32_t kept;
    uint32_t i;
    uint64_t lo = 0;
    uint64_t omitted;
    krffs_name_key_t *keys = NULL;

    *doubles = 0;
    if (krffs_count_names(mount, &named) != 0) {
        return -1;
    }
    if (named < 2) {
        return 0;
    }
    cap = named < KRFFS_REPAIR_NAMES ? named : KRFFS_REPAIR_NAMES;
    for (;;) {
        keys = kmalloc(cap * sizeof(*keys));
        if (keys || cap <= 64) {
            break;
        }
        cap /= 2;
    }
    if (!keys) {
        mount->check_owed = 1;
        return 0;
    }
    while (lo < KRFFS_HASH_END) {
        if (krffs_select_names(mount, keys, cap, lo, &count, &omitted) != 0) {
            kfree(keys);
            return -1;
        }
        kept = 0;
        for (i = 0; i < count; i++) {
            if (keys[i].hash < omitted) {
                keys[kept++] = keys[i];
            }
        }
        if (kept == 0 && omitted != KRFFS_HASH_END) {
            mount->check_owed = 1; /* one hash with more nodes than `cap` */
            omitted++;
        }
        lo = omitted;
        if (krffs_resolve_keys(mount, keys, kept, doubles) != 0) {
            kfree(keys);
            return -1;
        }
    }
    kfree(keys);
    return 0;
}

/** Whether directory id `id` is among the `count` sorted keys at `keys`, or
 * is the root's. */
static int krffs_dir_known(const krffs_name_key_t *keys, uint32_t count, uint64_t id)
{
    uint32_t lo = 0;
    uint32_t hi = count;

    if (id == KRFFS_ROOT_ID) {
        return 1;
    }
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;

        if (keys[mid].parent_id == id) {
            return 1;
        }
        if (keys[mid].parent_id < id) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return 0;
}

/**
 * Fill `keys`, room for `cap`, with the ids of the directories, sorted, and
 * set `*count`. 0, or -1 for a read that failed or more directories than
 * `cap`.
 */
static int krffs_dir_keys(krffs_mount_t *mount, krffs_name_key_t *keys, uint32_t cap,
                          uint32_t *count)
{
    uint64_t offset = 0;
    uint64_t steps = 0;
    uint64_t bound = krffs_max_steps(mount);
    krffs_brief_t brief;

    *count = 0;
    do {
        if (krffs_brief(mount, offset, &brief) != 0) {
            return -1;
        }
        if (brief.type == KRFFS_RESERVED_NODE && KRFFS_IS_DIR(brief.mode)) {
            if (*count == cap) {
                return -1;
            }
            keys[*count].parent_id = brief.id;
            keys[*count].hash = 0;
            keys[(*count)++].offset = 0;
        }
        offset = krffs_step(mount, offset, brief.size);
    } while (offset != 0 && ++steps < bound);
    krffs_sort_keys(keys, *count);
    return 0;
}

/* The sorted directory ids `krffs_repair_parents` tests against. */
struct krffs_dir_index {
    const krffs_name_key_t *keys;
    uint32_t count;
};

/** A `struct krffs_dir_test` by the sorted ids of a `struct
 * krffs_dir_index`, or by a walk of the chain when there was no memory for
 * them. */
static int krffs_dir_indexed(krffs_mount_t *mount, void *ctx, uint64_t id)
{
    const struct krffs_dir_index *index = ctx;

    if (!index->keys) {
        return krffs_dir_id_known(mount, id);
    }
    return krffs_dir_known(index->keys, index->count, id);
}

/**
 * Put each named file whose parent names no directory into the directory
 * its id names, or the root when the id names none. A rename whose parent
 * an edge of the medium cuts first writes the new parent into the id field
 * (`krffs_rename_parent`), so a stop within the parent's pieces leaves the
 * file here, and this finishes its move. Nothing else reads a file's id.
 * The repair's own parent write goes in pieces, with a barrier after each,
 * in an order whose halfway parents name no directory
 * (`krffs_parent_pieces`), so that a second stop leaves the file where the
 * next repair finds it again; the order that finishes the rename's own is
 * one. With no memory for the sorted directory ids, each id is tested by a
 * walk of the chain, so that no file is left in no directory, where a new
 * file could take its name. Returns 0 with `*adopted` counting the files
 * moved, or -1 for a read or a write that failed or a parent with no such
 * order.
 */
static int krffs_repair_parents(krffs_mount_t *mount, uint32_t *adopted)
{
    uint64_t offset = 0;
    uint64_t steps = 0;
    uint64_t bound = krffs_max_steps(mount);
    uint32_t dirs = 0;
    struct krffs_dir_index index;
    struct krffs_dir_test test;
    krffs_name_key_t *keys;
    krffs_brief_t brief;
    uint64_t to;
    uint8_t order = KRFFS_ORDER_WHOLE; /* set by krffs_parent_pieces before use */
    int known;
    int rc = 0;

    *adopted = 0;
    do {
        if (krffs_brief(mount, offset, &brief) != 0) {
            return -1;
        }
        dirs += brief.type == KRFFS_RESERVED_NODE && KRFFS_IS_DIR(brief.mode);
        offset = krffs_step(mount, offset, brief.size);
    } while (offset != 0 && ++steps < bound);
    keys = kmalloc((dirs ? dirs : 1) * sizeof(*keys));
    index.keys = keys;
    index.count = 0;
    test.known = krffs_dir_indexed;
    test.ctx = &index;
    if (keys && krffs_dir_keys(mount, keys, dirs, &index.count) != 0) {
        kfree(keys);
        return -1;
    }
    offset = 0;
    steps = 0;
    do {
        if (krffs_brief(mount, offset, &brief) != 0) {
            rc = -1;
            break;
        }
        known = 1;
        if (brief.type == KRFFS_RESERVED_NODE && !KRFFS_IS_DIR(brief.mode)) {
            known = krffs_name_is(mount, offset, "", 0);
            if (known == 0) {
                known = krffs_dir_indexed(mount, &index, brief.parent_id);
            }
        }
        if (known == 0) {
            known = krffs_dir_indexed(mount, &index, brief.id);
            to = known > 0 ? brief.id : KRFFS_ROOT_ID;
            rc = known < 0 ? -1
                           : krffs_parent_pieces(mount, offset + KRFFS_FIELD_PARENT,
                                                 brief.parent_id, to, &test, &order);
            if (rc > 0) {
                klog(KLOG_ERR, "krffs",
                     "check: no order puts back the file at %d without a halfway parent",
                     (int)offset);
            }
            if (rc != 0 ||
                krffs_put_pieces(mount, offset + KRFFS_FIELD_PARENT, (const uint8_t *)&to,
                                 (uint32_t)sizeof(to), order) != 0) {
                rc = -1;
                break;
            }
            (*adopted)++;
        }
        if (known < 0) {
            rc = -1;
            break;
        }
        offset = krffs_step(mount, offset, brief.size);
    } while (offset != 0 && ++steps < bound);
    kfree(keys);
    return rc;
}

/**
 * Rebuild the backward links of a validated forward chain, including the
 * root's link to the last node. Returns 0 with the number of links repaired
 * in `fixed`, or -1 if a read or write fails. The caller syncs the writes.
 */
static int krffs_repair_links(krffs_mount_t *mount, uint32_t *fixed)
{
    uint64_t offset = 0;
    uint64_t previous_size = 0;
    uint64_t steps = 0;
    uint64_t bound = krffs_max_steps(mount);
    krffs_node_t head;
    uint64_t root_link;

    *fixed = 0;
    if (krffs_head(mount, 0, &head) != 0) {
        return -1;
    }
    root_link = head.previous_node_size;
    do {
        if (offset != 0 && head.previous_node_size != previous_size) {
            if (krffs_write_backward_link(mount, offset, previous_size) != 0) {
                return -1;
            }
            (*fixed)++;
        }
        previous_size = head.size;
        offset = krffs_step(mount, offset, head.size);
        if (offset != 0 && (++steps >= bound || krffs_head(mount, offset, &head) != 0)) {
            return -1;
        }
    } while (offset != 0);
    if (root_link != previous_size) {
        if (krffs_write_backward_link(mount, 0, previous_size) != 0) {
            return -1;
        }
        (*fixed)++;
    }
    mount->stale_links = 0;
    return 0;
}

/**
 * The backward links are rebuilt from the forward chain first, since the
 * removal of an orphan follows them. Then each name on two nodes of one
 * directory loses one of its nodes (`krffs_repair_names`); the pass of
 * `krffs_reap_orphans_at` at the end frees it with the other nodes that
 * have no name. A rename stopped between the parent it stored and the
 * name it had still to write puts a file beside a directory of the same
 * name, which is why a directory keeps its name whatever the sizes say.
 */
int krffs_repair_at(krffs_mount_t *mount, uint32_t *links, uint32_t *orphans, uint32_t *doubles)
{
    uint32_t fixed_links = 0;
    uint32_t fixed_doubles = 0;
    uint32_t adopted = 0;

    if (links) {
        *links = 0;
    }
    if (orphans) {
        *orphans = 0;
    }
    if (doubles) {
        *doubles = 0;
    }
    if (krffs_repair_links(mount, &fixed_links) != 0 ||
        krffs_repair_parents(mount, &adopted) != 0 ||
        krffs_repair_names(mount, &fixed_doubles) != 0) {
        krffs_halt(mount);
        return -1;
    }
    if (adopted != 0) {
        klog(KLOG_INFO, "krffs", "check: repaired: %d file(s) put back in a directory",
             (int)adopted);
    }

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
    if (krffs_sync_point(mount) != 0) {
        krffs_halt(mount);
        return -1;
    }
    return 0;
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
 * on the root. Returns 1 if sound, 0 if malformed, or -EIO on a read error.
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
int krffs_chain_walk(krffs_mount_t *mount, int with_links)
{
    uint64_t position = 0;
    uint64_t previous_size = 0;
    uint64_t total = 0;
    uint64_t bound = krffs_max_steps(mount);
    uint64_t count = 0;
    krffs_node_t head;
    krffs_node_t root;

    if (krffs_head(mount, 0, &root) != 0) {
        return -EIO;
    }

    for (;;) {
        if (++count > bound) {
            return 0;
        }
        if (mount->size - position < sizeof(krffs_node_t)) {
            return 0;
        }
        if (krffs_head(mount, position, &head) != 0) {
            return -EIO;
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
    return krffs_chain_walk(mount, 1) == 1;
}

int krffs_chain_is_walkable(krffs_mount_t *mount)
{
    return krffs_chain_walk(mount, 0) == 1;
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

int krffs_node_head(const krffs_node_t *node, krffs_node_t *out)
{
    uint64_t offset = krffs_offset_of(node);

    if (offset == KRFFS_NO_NODE || !out) {
        return -1;
    }
    return krffs_head(&krffs_root_mount, offset, out);
}

uint64_t krffs_ino_of(const krffs_node_t *node)
{
    krffs_node_t head;

    if (krffs_fs && node == krffs_fs->node) {
        return 1;
    }
    return krffs_node_head(node, &head) == 0 ? krffs_node_ino(&head) : 0;
}

krffs_node_t *krffs_next_node(const krffs_node_t *node, const krffs_node_t *head)
{
    uint64_t offset = krffs_offset_of(node);

    if (offset == KRFFS_NO_NODE || !head) {
        return krffs_fs ? krffs_fs->node : NULL;
    }
    return krffs_node_of(krffs_next_offset(&krffs_root_mount, offset, head));
}

/** Store `size` bytes of `value` at `field` of `node`, a node of the boot
 * root, through the backend: 0, or -1 for a pointer outside the image, on a
 * halted root, or when the store fails. */
static int krffs_node_store(krffs_node_t *node, uint32_t field, const void *value, uint32_t size)
{
    uint64_t offset = krffs_offset_of(node);

    if (offset == KRFFS_NO_NODE) {
        return -1;
    }
    return krffs_backend_write(&krffs_root_mount, offset + field, value, size);
}

int krffs_node_set_mode(krffs_node_t *node, uint16_t mode)
{
    if (!krffs_fs || node == krffs_fs->node) {
        return -1;
    }
    return krffs_node_store(node, KRFFS_FIELD_MODE, &mode, (uint32_t)sizeof(mode));
}

int krffs_node_set_owner(krffs_node_t *node, uint32_t uid, uint32_t gid, int which)
{
    int rc = 0;

    if (which & KRFFS_SET_UID) {
        rc |= krffs_node_store(node, KRFFS_FIELD_UID, &uid, (uint32_t)sizeof(uid));
    }
    if (which & KRFFS_SET_GID) {
        rc |= krffs_node_store(node, KRFFS_FIELD_GID, &gid, (uint32_t)sizeof(gid));
    }
    return rc;
}

int krffs_node_set_time(krffs_node_t *node, uint32_t field, int64_t seconds, int64_t nanoseconds)
{
    krffs_node_time_t when;

    if (field != KRFFS_FIELD_ATIME && field != KRFFS_FIELD_MTIME && field != KRFFS_FIELD_CTIME) {
        return -1;
    }
    when.seconds = seconds;
    when.nanoseconds = nanoseconds;
    return krffs_node_store(node, field, &when, (uint32_t)sizeof(when));
}

int krffs_node_is_dir(const krffs_node_t *node)
{
    krffs_node_t head;

    if (krffs_fs && node == krffs_fs->node) {
        return 1;
    }
    if (krffs_node_head(node, &head) != 0) {
        return -1;
    }
    return KRFFS_IS_DIR(head.mode) ? 1 : 0;
}

/** Whether `node` is a node of the boot root that holds a file's bytes: not
 * the root, not a directory, and with a header that can be read. */
static int krffs_is_file_node(const krffs_node_t *node)
{
    krffs_node_t head;

    if (!node || node == krffs_fs->node || krffs_node_head(node, &head) != 0) {
        return 0;
    }
    return !KRFFS_IS_DIR(head.mode);
}

krffs_node_t *krffs_child_err(krffs_node_t *dir, const char *name, int *err)
{
    uint64_t at = 0;
    int rc;
    krffs_node_t *found;

    if (!krffs_fs) {
        rc = -ENOENT;
        found = NULL;
    } else {
        if (dir && dir != krffs_fs->node) {
            at = krffs_offset_of(dir);
            if (at == KRFFS_NO_NODE) {
                if (err) {
                    *err = -ENOENT;
                }
                return NULL;
            }
        }
        found = krffs_node_of(krffs_child_at_err(&krffs_root_mount, at, name, &rc));
    }
    if (err) {
        *err = found ? 0 : rc;
    }
    return found;
}

krffs_node_t *krffs_child(krffs_node_t *dir, const char *name)
{
    return krffs_child_err(dir, name, NULL);
}

krffs_node_t *krffs_resolve_err(const char *path, int *err)
{
    int rc = -ENOENT;
    krffs_node_t *found = NULL;

    if (krffs_fs) {
        found = krffs_node_of(krffs_walk_err(&krffs_root_mount, path, &rc));
    }
    if (err) {
        *err = found ? 0 : rc;
    }
    return found;
}

krffs_node_t *krffs_resolve(const char *path)
{
    return krffs_resolve_err(path, NULL);
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
    return krffs_is_file_node(node) ? 0 : -1;
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
    if (!krffs_is_file_node(node)) {
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
    if (!krffs_is_file_node(node)) {
        klog(KLOG_DEBUG, "krffs", "write: no such file: %s", path);
        return -1;
    }
    return krffs_write_node(&node, buffer, size, offset);
}

uint32_t krffs_get_file_size(const char *path)
{
    krffs_node_t *node;
    krffs_node_t head;

    if (!krffs_fs) {
        return 0;
    }
    node = krffs_resolve(path);
    if (!node || node == krffs_fs->node || krffs_node_head(node, &head) != 0) {
        return 0;
    }
    return (uint32_t)head.data_size;
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

/* A holder test on the boot root's node addresses, for `krffs_rename_err`. */
struct krffs_held_node {
    int (*held)(const krffs_node_t *node);
};

/** Whether the boot root's node at `offset` is held, by the test `arg`
 * carries. */
static int krffs_held_at(void *arg, uint64_t offset)
{
    const struct krffs_held_node *h = (const struct krffs_held_node *)arg;

    return h->held(krffs_node_of(offset));
}

int krffs_rename_node(const char *old_path, const char *new_path,
                      int (*held)(const krffs_node_t *node))
{
    struct krffs_held_node h;

    if (!krffs_fs) {
        return -1;
    }
    h.held = held;
    return krffs_rename_err(&krffs_root_mount, old_path, new_path, held ? krffs_held_at : NULL, &h,
                            NULL);
}

uint32_t krffs_node_phase(const krffs_node_t *node)
{
    uint64_t offset = krffs_offset_of(node);

    return offset == KRFFS_NO_NODE ? 0 : krffs_file_phase(&krffs_root_mount, offset);
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

krffs_node_t *krffs_parent_node_err(krffs_node_t *node, int *err)
{
    int rc = 0;
    krffs_node_t *found = NULL;

    if (krffs_fs && node) {
        found =
            krffs_node_of(krffs_parent_offset_err(&krffs_root_mount, krffs_offset_of(node), &rc));
    }
    if (err) {
        *err = rc;
    }
    return found;
}

krffs_node_t *krffs_parent_node(krffs_node_t *node)
{
    return krffs_parent_node_err(node, NULL);
}

int krffs_orphan_node(krffs_node_t *node)
{
    if (!krffs_fs || !node) {
        return -1;
    }
    return krffs_orphan_at(&krffs_root_mount, krffs_offset_of(node));
}

int krffs_unlink_held(krffs_node_t *node)
{
    krffs_stamp(krffs_parent_node(node), KRFFS_STAMP_DATA);
    return krffs_orphan_node(node);
}

uint32_t krffs_reap_orphans(void)
{
    if (!krffs_fs) {
        return 0;
    }
    return krffs_reap_orphans_at(&krffs_root_mount);
}

krffs_node_t *krffs_resize_reserved_node(krffs_file_system_t *fs, krffs_node_t *node, uint64_t size)
{
    if (!fs || fs != krffs_fs || !fs->node || !node) {
        return NULL;
    }
    return krffs_node_of(krffs_resize_at(&krffs_root_mount, krffs_offset_of(node), size));
}

void krffs_remove_reserved_node(krffs_file_system_t *fs, krffs_node_t *node)
{
    if (!fs || fs != krffs_fs || !fs->node || !node) {
        return;
    }
    krffs_root_in_reap = 1;
    (void)krffs_reap_at(&krffs_root_mount, krffs_offset_of(node));
    krffs_root_in_reap = 0;
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

/** Forget the boot root's mount after a mount that is refused. */
static void krffs_unmount_root(void)
{
    if (krffs_root_mount.sector) {
        krffs_halt(&krffs_root_mount);
    }
    krffs_fs = NULL;
    krffs_root_flush = NULL;
    krffs_root_mount.backend = NULL;
    krffs_root_mount.ctx = NULL;
    krffs_root_mount.size = 0;
}

/** Mount a memory or disk image as described in krffs.h, through `backend`,
 * or the memory backend when it is NULL. */
static int krffs_mount_root(void *base, uint32_t size, uint32_t disk_grain, int (*flush)(void),
                            const krffs_backend_t *backend)
{
    krffs_node_t root;
    uint32_t fixed;
    int valid;

    if (!base || size < sizeof(krffs_node_t)) {
        klog(KLOG_ERR, "krffs", "mount: refused: no image at %x, %d bytes",
             (unsigned)(uintptr_t)base, size);
        return -1;
    }

    krffs_root_mount.backend = backend ? backend : &krffs_memory_backend;
    krffs_root_mount.ctx = base;
    krffs_root_mount.size = size;
    krffs_root_mount.grain = 0;
    krffs_root_mount.sector = disk_grain;
    krffs_root_mount.sector_phase = 0;
    krffs_root_mount.stale_links = 0;
    krffs_root_mount.check_owed = 0;
    krffs_root_mount.unsettled = 0;
    krffs_root_mount.halted = 0;
    krffs_root_mount.withdrawn = 0;
    krffs_root_mount.orphans = 0;
    krffs_root_mount.on_halt = NULL; /* the VFS mount sets it (`krffs_root_on_halt`) */
    krffs_root_flush = flush;

    if (krffs_head(&krffs_root_mount, 0, &root) != 0) {
        klog(KLOG_ERR, "krffs", "mount: refused: the root node cannot be read");
        krffs_unmount_root();
        return -1;
    }
    if (root.magic != KRFFS_FILE_SYSTEM_MAGIC) {
        klog(KLOG_ERR, "krffs", "mount: refused: magic 0x%X, expected 0x%X", root.magic,
             KRFFS_FILE_SYSTEM_MAGIC);
        krffs_unmount_root();
        return -1;
    }
    if (root.type != KRFFS_ROOT_NODE) {
        klog(KLOG_ERR, "krffs", "mount: refused: first node is type %d, not the root", root.type);
        krffs_unmount_root();
        return -1;
    }

    valid = krffs_chain_walk(&krffs_root_mount, 1);
    if (valid != 1) {
        if (valid < 0 || disk_grain == 0 || !(root.mode & KRFFS_MODE_DIRTY) ||
            !krffs_chain_is_walkable(&krffs_root_mount) ||
            krffs_repair_links(&krffs_root_mount, &fixed) != 0) {
            klog(KLOG_ERR, "krffs", "mount: refused: the node chain is malformed");
            krffs_unmount_root();
            return -1;
        }
        klog(KLOG_INFO, "krffs", "check: repaired: %d backward link(s)", (int)fixed);
    }
    if (disk_grain != 0 && (root.mode & KRFFS_MODE_DIRTY)) {
        uint32_t doubles = 0;
        uint32_t adopted = 0;

        if (krffs_repair_parents(&krffs_root_mount, &adopted) != 0 ||
            krffs_repair_names(&krffs_root_mount, &doubles) != 0) {
            klog(KLOG_ERR, "krffs", "mount: refused: the names cannot be checked");
            krffs_unmount_root();
            return -1;
        }
        if (adopted != 0) {
            klog(KLOG_INFO, "krffs", "check: repaired: %d file(s) put back in a directory",
                 (int)adopted);
        }
        if (doubles != 0) {
            klog(KLOG_INFO, "krffs", "check: repaired: %d repeated name(s)", (int)doubles);
        }
        if (krffs_root_mount.check_owed) {
            /* Two nodes may still share a name, which a write could worsen. */
            klog(KLOG_WARN, "krffs", "check: left unfinished; writes refused; run fsck.krffs");
            krffs_halt(&krffs_root_mount);
        }
    }

    _krffs_fs_instance.node = (krffs_node_t *)base;
    _krffs_fs_instance.size = size;
    krffs_fs = &_krffs_fs_instance;

    return 0;
}

int krffs_root_check_owed(void)
{
    return krffs_root_mount.check_owed != 0;
}

/** Halt the disk root after its orphan repair failed to reach the disk. */
void krffs_root_halt(void)
{
    krffs_halt(&krffs_root_mount);
}

/** Whether a last holder is removing its orphan on the mounted root. */
int krffs_root_reaping(void)
{
    return krffs_root_in_reap;
}

int krffs_root_halted(void)
{
    return krffs_root_mount.halted != 0;
}

void krffs_root_on_halt(void (*fn)(krffs_mount_t *mount))
{
    krffs_root_mount.on_halt = fn;
}

int krffs_root_has_orphans(void)
{
    return krffs_root_mount.orphans != 0;
}

int init_krffs(void *ramdisk_base, uint32_t ramdisk_size)
{
    return krffs_mount_root(ramdisk_base, ramdisk_size, 0, NULL, NULL);
}

int init_krffs_disk_root(void *base, uint32_t size, uint32_t sector_bytes, int (*flush)(void),
                         const krffs_backend_t *backend)
{
    return krffs_mount_root(base, size, sector_bytes, flush, backend);
}
