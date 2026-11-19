/*
 * krffs_hosttest.c: the KRFFS core, rach2/sys/fs/krffs.c, over backends of
 * the test's own.
 *
 * host-deps: rach2/sys/lib/kstring.c
 * host-deps: rach2/sys/lib/kctypes.c
 *
 * The core addresses nodes by offset and reaches the medium through a
 * backend, so a test can put its own backend under it. Two are used here: a
 * plain memory image, and the same image behind a recorder that keeps every
 * write in order. The rows are the format's contract: what the mount check
 * accepts and what it rejects, the allocator's split, merge and relocation,
 * the ends of the region, a hole read as zeroes, the directory operations,
 * the orphan reaping, the order of writes, and the hashes, against the
 * vectors of krffs_hash_vectors.h.
 *
 * The order rows replay every prefix of the writes an operation makes, which
 * is what a machine stopping in the middle of one leaves on the medium. Each
 * prefix must leave a forward chain that closes with every node sound; the
 * backward link may lag, since it is derived and the check at the next mount
 * rebuilds it. That is the rule rach2/sys/fs/krffs.Readme.md states, and
 * these rows check that the code keeps it.
 *
 * The test includes no hosted header: the kernel headers define the
 * fixed-width types, and the C library calls it makes are declared below.
 */

#include "../../rach2/sys/fs/krffs.c"

#include "krffs_hash_vectors.h"

extern int printf(const char *fmt, ...);
extern void *malloc(unsigned long size);
extern void free(void *p);

/* Set to have `kmalloc` refuse, as a kernel short of memory does. */
static int kmalloc_refuses;

/* The most `kmalloc` gives at once, or 0 for no bound. */
static unsigned long kmalloc_limit;

/* The least `kmalloc` gives, or 0 for no bound: a floor above the sorted
   directory ids of an image and below its index of names refuses the
   first alone. */
static unsigned long kmalloc_floor;

/** Host memory for the repair's index, or NULL while `kmalloc_refuses`, or
 * for more than `kmalloc_limit` or less than `kmalloc_floor`. */
void *kmalloc(size_t size)
{
    if (kmalloc_refuses || (kmalloc_limit != 0 && size > kmalloc_limit) || size < kmalloc_floor) {
        return (void *)0;
    }
    return malloc(size);
}

/** Give back what `kmalloc` gave. */
void kfree(void *p)
{
    free(p);
}

/* ---- what krffs.c reaches outside itself ---- */

/** Discard the record. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

/** Discard the text. */
void console_printf(const char *format, ...)
{
    (void)format;
}

/* The time `rtc_realtime` reports, which the rows set. */
static int64_t fake_seconds = 1000;
static int64_t fake_nanoseconds;

/** Report `fake_seconds` and `fake_nanoseconds`; either pointer may be NULL. */
void rtc_realtime(int64_t *seconds, int64_t *nanoseconds)
{
    if (seconds) {
        *seconds = fake_seconds;
    }
    if (nanoseconds) {
        *nanoseconds = fake_nanoseconds;
    }
}

static int fails;

/** Count and print a failed row. */
static void check(int ok, const char *what)
{
    if (!ok) {
        fails++;
        printf("krffs_hosttest: FAIL: %s\n", what);
    }
}

/* ---- the image and its backends ---- */

#define IMAGE_SIZE 65536u

/* What free space is filled with in the order rows: no file writes it, so a
   byte of it inside a file's length is a length published too early. */
#define POISON 0xA5

static uint8_t image[IMAGE_SIZE];

/** Copy `size` bytes at `offset` of the image `ctx` into `buffer`. */
static int memory_read(void *ctx, uint64_t offset, void *buffer, uint32_t size)
{
    k_memcpy(buffer, (uint8_t *)ctx + offset, size);
    return 0;
}

/** Copy `size` bytes of `buffer` to `offset` of the image `ctx`. */
static int memory_write(void *ctx, uint64_t offset, const void *buffer, uint32_t size)
{
    k_memcpy((uint8_t *)ctx + offset, buffer, size);
    return 0;
}

/** Do nothing: a memory image has nothing to make durable. */
static int memory_sync(void *ctx)
{
    (void)ctx;
    return 0;
}

static const krffs_backend_t memory_backend = {memory_read, memory_write, memory_sync, 0};

/* The recorder: the same image, with every write kept in the order it was
   made, so a row can replay any prefix of an operation. */

#define WRITES_MAX 4096

static struct {
    uint64_t offset;
    uint32_t size;
    uint8_t bytes[512];
} recorded[WRITES_MAX];

static int recorded_count;
static int recording;
/* Which write fails, counted from the start of an operation, or -1 for a
   backend whose writes all succeed. One write and not every write after it:
   a device that fails one block and writes the next is what leaves a change
   published over a write that never landed. */
static int refused_write = -1;
/* The writes and syncs the backend has seen since the count was cleared. */
static int writes_seen;
/* The offset of the one write that fails, whatever its place, or -1. */
static uint64_t refused_offset = (uint64_t)-1;
/* The sync that fails, counted with the writes so that a row can fail
   either; -1 for none. */
static int refused_sync = -1;
/* The offset of a write after which the next sync fails, or -1. */
static uint64_t refused_sync_after = (uint64_t)-1;
/* The offset whose second write fails, or -1, and the writes seen there. */
static uint64_t refused_again_offset = (uint64_t)-1;
static int refused_again_seen;

/**
 * Write through to the image, keeping the write in `recorded` while
 * `recording` is set; fail with -1 when it is `refused_write`. A write the
 * recorder cannot keep counts as a failed row.
 */
static int record_write(void *ctx, uint64_t offset, const void *buffer, uint32_t size)
{
    if (writes_seen++ == refused_write) {
        return -1;
    }
    if (offset == refused_offset) {
        refused_offset = (uint64_t)-1;
        return -1;
    }
    if (offset == refused_sync_after) {
        refused_sync_after = (uint64_t)-1;
        refused_sync = writes_seen; /* the next event, the barrier after it */
    }
    if (offset == refused_again_offset && refused_again_seen++ == 1) {
        refused_again_offset = (uint64_t)-1;
        return -1;
    }
    if (recording && size <= sizeof(recorded[0].bytes) && recorded_count < WRITES_MAX) {
        recorded[recorded_count].offset = offset;
        recorded[recorded_count].size = size;
        k_memcpy(recorded[recorded_count].bytes, buffer, size);
        recorded_count++;
    } else if (recording) {
        printf("krffs_hosttest: FAIL: a write the recorder cannot hold\n");
        fails++;
    }
    return memory_write(ctx, offset, buffer, size);
}

/* Where each sync fell among the writes: what a cache has certainly written
   when the machine stops, everything after the last one being what it may
   still be holding. */
static int sync_marks[WRITES_MAX];
static int sync_count;

/**
 * Mark where the sync fell among the recorded writes while `recording` is
 * set, and fail with -1 when it is `refused_sync`.
 */
static int record_sync(void *ctx)
{
    if (recording && sync_count < WRITES_MAX) {
        sync_marks[sync_count++] = recorded_count;
    }
    if (writes_seen++ == refused_sync) {
        return -1;
    }
    return memory_sync(ctx);
}

static const krffs_backend_t recording_backend = {memory_read, record_write, record_sync, 0};

/** Whether a node's name is exactly `name`. */
static int k_strcmp_name(const uint8_t *field, const char *name)
{
    uint32_t i;

    for (i = 0; name[i] != '\0'; i++) {
        if (field[i] != (uint8_t)name[i]) {
            return 0;
        }
    }
    return field[i] == '\0';
}

/* ---- images ---- */

/**
 * Lay a fresh file system into `buffer`: a root node and one free node.
 */
static void make_image(uint8_t *buffer, uint32_t size)
{
    krffs_node_t *root = (krffs_node_t *)buffer;
    krffs_node_t *free_node = (krffs_node_t *)(buffer + sizeof(krffs_node_t));
    uint32_t i;

    for (i = 0; i < size; i++) {
        buffer[i] = 0;
    }
    root->magic = KRFFS_FILE_SYSTEM_MAGIC;
    root->type = KRFFS_ROOT_NODE;
    root->id = KRFFS_ROOT_ID;
    root->mode = (uint16_t)(KRFFS_S_IFDIR | 0755);
    root->size = sizeof(krffs_node_t);
    root->previous_node_size = size - sizeof(krffs_node_t);
    root->data_size = 0;

    free_node->magic = KRFFS_FILE_SYSTEM_MAGIC;
    free_node->type = KRFFS_FREE_NODE;
    free_node->size = size - sizeof(krffs_node_t);
    free_node->previous_node_size = sizeof(krffs_node_t);
    free_node->data_size = 0;
}

/** A mount of `size` bytes over `backend`, with no unit of the medium. */
static krffs_mount_t mount_over(const krffs_backend_t *backend, void *ctx, uint32_t size)
{
    krffs_mount_t mount;

    mount.backend = backend;
    mount.ctx = ctx;
    mount.size = size;
    mount.grain = 0; /* a memory image has no unit it reaches in pieces */
    mount.sector = 0;
    mount.sector_phase = 0;
    mount.stale_links = 0;
    mount.check_owed = 0;
    mount.unsettled = 0;
    mount.halted = 0;
    mount.withdrawn = 0;
    mount.orphans = 0;
    mount.on_halt = NULL;
    return mount;
}

/**
 * Walk the forward chain and apply to every node the mount check's tests but
 * the one of the backward link, which an interrupted operation may leave
 * behind.
 *
 * Returns 1 if the chain closes with every node sound, 0 if not.
 */
static int forward_chain_closes(krffs_mount_t *mount)
{
    uint64_t position = 0;
    uint64_t total = 0;
    uint64_t steps = 0;
    krffs_node_t head;

    for (;;) {
        if (++steps > krffs_max_steps(mount)) {
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
        if (head.data_size > head.size - sizeof(krffs_node_t)) {
            return 0;
        }
        total += head.size;
        position += head.size;
        if (position == mount->size) {
            break;
        }
    }
    return total == mount->size;
}

/**
 * Rebuild every backward link from the forward chain, which is what the check
 * at the next mount does with the one fault an interrupted write can leave.
 */
static void rebuild_backward_links(krffs_mount_t *mount)
{
    uint64_t position = 0;
    uint64_t previous_size = 0;
    krffs_node_t head;
    uint64_t steps = 0;

    while (steps++ <= krffs_max_steps(mount)) {
        if (krffs_head(mount, position, &head) != 0) {
            return;
        }
        if (position != 0) {
            head.previous_node_size = previous_size;
            mount->backend->write(mount->ctx, position + KRFFS_FIELD_PREVIOUS, &previous_size,
                                  sizeof(previous_size));
        }
        previous_size = head.size;
        position += head.size;
        if (position >= mount->size) {
            break;
        }
    }
    mount->backend->write(mount->ctx, KRFFS_FIELD_PREVIOUS, &previous_size, sizeof(previous_size));
}

/* The block a cache keeps and loses as one. */
#define CACHE_BLOCK 4096u

/**
 * The writes that are certainly on the medium when a machine stops after
 * `prefix` of them.
 *
 * A sync recorded at `prefix` ran after that write and before the next, so at
 * the moment the machine stopped it may not have run yet: only a sync
 * recorded strictly earlier is one the state has certainly passed. Reading it
 * this way tests the state between a write and the sync that follows it,
 * which is exactly where a missing sync shows.
 */
static int epoch_of(int prefix)
{
    int epoch = 0;
    int i;

    for (i = 0; i < sync_count; i++) {
        if (sync_marks[i] < prefix) {
            epoch = sync_marks[i];
        }
    }
    return epoch;
}

/** `image` = `before` with the first `count` writes applied. */
static void build(uint8_t *image, const uint8_t *before, int count, uint32_t size)
{
    int i;

    k_memcpy(image, before, size);
    for (i = 0; i < count; i++) {
        k_memcpy(image + recorded[i].offset, recorded[i].bytes, recorded[i].size);
    }
}

/** The distinct units of `grain` bytes the writes in [from, to) touched. */
static int touched_blocks(int from, int to, uint32_t grain, uint64_t *seen, int cap)
{
    int count = 0;
    int i;

    for (i = from; i < to; i++) {
        uint64_t first = recorded[i].offset / grain;
        uint64_t last = (recorded[i].offset + recorded[i].size - 1) / grain;
        uint64_t blk;

        for (blk = first; blk <= last; blk++) {
            int k;
            int known = 0;

            for (k = 0; k < count; k++) {
                if (seen[k] == blk) {
                    known = 1;
                }
            }
            if (known || count == cap) {
                continue;
            }
            seen[count++] = blk;
        }
    }
    return count;
}

/**
 * Whether the state in `image`, read through `mount`, has what every state a
 * stop can leave must have: a forward chain that closes, no file claiming a
 * byte nobody wrote, and, once `krffs_repair_at` has done what the check at
 * the next mount does, the whole check.
 */
static int image_is_sound(krffs_mount_t *mount, const uint8_t *image)
{
    uint64_t offset = 0;
    uint64_t steps = 0;
    krffs_node_t head;

    if (!forward_chain_closes(mount)) {
        return 0;
    }
    do {
        if (krffs_head(mount, offset, &head) != 0) {
            return 0;
        }
        if (head.type == KRFFS_RESERVED_NODE) {
            uint64_t at;

            for (at = 0; at < head.data_size; at++) {
                if (image[offset + sizeof(krffs_node_t) + at] == POISON) {
                    return 0;
                }
            }
        }
        offset = krffs_next_offset(mount, offset, &head);
    } while (offset != 0 && ++steps < krffs_max_steps(mount));
    if (krffs_repair_at(mount, (uint32_t *)0, (uint32_t *)0, (uint32_t *)0) != 0) {
        return 0;
    }
    return krffs_chain_is_valid(mount);
}

/**
 * Every state a machine can stop in, block by block.
 *
 * A cache holds whole blocks and writes whole blocks, so what a stop can lose
 * is any block written since the last sync, whole: never half of one, and
 * never one of two writes that fell in the same block. Each such state must
 * leave a chain a mount can walk and no file claiming bytes nobody wrote,
 * which is what the syncs between a write and the field that publishes it
 * are for.
 */
static int every_stop_is_sound(const uint8_t *before, uint32_t size)
{
    static uint8_t state[IMAGE_SIZE];
    static uint8_t epoch[IMAGE_SIZE];
    krffs_mount_t mount = mount_over(&memory_backend, state, size);
    int prefix;

    for (prefix = 0; prefix <= recorded_count; prefix++) {
        uint64_t seen[64];
        int mark = epoch_of(prefix);
        int seen_count = touched_blocks(mark, prefix, CACHE_BLOCK, seen, 64);
        int variant;

        build(epoch, before, mark, size);
        for (variant = -1; variant < seen_count; variant++) {
            build(state, before, prefix, size);
            if (variant >= 0) {
                uint32_t at = (uint32_t)(seen[variant] * CACHE_BLOCK);
                uint32_t take = size - at > CACHE_BLOCK ? CACHE_BLOCK : size - at;

                k_memcpy(state + at, epoch + at, take);
            }
            if (!image_is_sound(&mount, state)) {
                printf("krffs_hosttest: %d of %d writes, having lost the block at %d, "
                       "is not an image a mount could repair\n",
                       prefix, recorded_count,
                       variant < 0 ? -1 : (int)(seen[variant] * CACHE_BLOCK));
                return 0;
            }
        }
    }
    return 1;
}

/**
 * Every state a stop can leave if one write since the last sync did not
 * happen.
 *
 * A cache loses whole blocks, so some of these states no cache can produce:
 * two writes that fall in one block are lost or kept together. They are
 * judged anyway, because the order this file system keeps is an order between
 * fields and not between blocks: what a header holds must be on the medium
 * before the field that reveals it, whether or not the two share a block. A
 * file system that depends on two fields sharing a block is one a different
 * image layout breaks.
 */
static int every_lost_write_is_sound(const uint8_t *before, uint32_t size)
{
    static uint8_t state[IMAGE_SIZE];
    krffs_mount_t mount = mount_over(&memory_backend, state, size);
    int prefix;

    for (prefix = 0; prefix <= recorded_count; prefix++) {
        int mark = epoch_of(prefix);
        int skip;

        for (skip = mark; skip < prefix; skip++) {
            int i;

            k_memcpy(state, before, size);
            for (i = 0; i < prefix; i++) {
                if (i != skip) {
                    k_memcpy(state + recorded[i].offset, recorded[i].bytes, recorded[i].size);
                }
            }
            if (!image_is_sound(&mount, state)) {
                printf("krffs_hosttest: %d of %d writes, without write %d, is not an "
                       "image a mount could repair\n",
                       prefix, recorded_count, skip);
                return 0;
            }
        }
    }
    return 1;
}

/**
 * Whether, in every state a stop can leave, the file `name` is whole.
 *
 * Whole means what a name promises: the directory it is in, the mode it was
 * made with, a length of at least `bytes`, and those bytes holding `value`. A
 * node that is half made must not be reachable by a name, so a state may have
 * no such node at all unless `must_exist`, which is what a move requires: the
 * file it moves is there before, during and after.
 *
 * The states are the one that lost nothing, then one for each block since
 * the last sync lost whole, then one for each write since it lost alone,
 * which is the stricter rule the order between fields needs. A state whose
 * forward chain does not close is left to `every_stop_is_sound`. Each state
 * is judged after `krffs_repair_at`, since what the check at the next mount
 * leaves is what a machine coming back up reads.
 */
static int every_stop_file_is_whole(const uint8_t *before, uint32_t size, const char *name,
                                    int must_exist, uint64_t parent, uint16_t mode, uint32_t bytes,
                                    uint8_t value)
{
    static uint8_t state[IMAGE_SIZE];
    static uint8_t epoch[IMAGE_SIZE];
    krffs_mount_t mount = mount_over(&memory_backend, state, size);
    int prefix;

    for (prefix = 0; prefix <= recorded_count; prefix++) {
        uint64_t seen[64];
        int mark = epoch_of(prefix);
        int seen_count = touched_blocks(mark, prefix, CACHE_BLOCK, seen, 64);
        int variant;

        build(epoch, before, mark, size);
        for (variant = -1; variant < seen_count + (prefix - mark); variant++) {
            uint64_t offset = 0;
            uint64_t steps = 0;
            krffs_node_t head;
            int whole = 0;

            if (variant < seen_count) {
                build(state, before, prefix, size);
                if (variant >= 0) {
                    uint32_t at = (uint32_t)(seen[variant] * CACHE_BLOCK);
                    uint32_t take = size - at > CACHE_BLOCK ? CACHE_BLOCK : size - at;

                    k_memcpy(state + at, epoch + at, take);
                }
            } else {
                int skip = mark + (variant - seen_count);
                int i;

                k_memcpy(state, before, size);
                for (i = 0; i < prefix; i++) {
                    if (i != skip) {
                        k_memcpy(state + recorded[i].offset, recorded[i].bytes, recorded[i].size);
                    }
                }
            }
            if (!forward_chain_closes(&mount)) {
                continue;
            }
            if (krffs_repair_at(&mount, (uint32_t *)0, (uint32_t *)0, (uint32_t *)0) != 0) {
                return 0;
            }
            do {
                if (krffs_head(&mount, offset, &head) != 0) {
                    return 0;
                }
                if (head.type == KRFFS_RESERVED_NODE && k_strcmp_name(head.name, name)) {
                    uint32_t at;

                    if (head.parent_id != parent) {
                        printf("krffs_hosttest: %d of %d writes left %s in a directory it "
                               "was never put in\n",
                               prefix, recorded_count, name);
                        return 0;
                    }
                    if ((head.mode & 07777u) != mode) {
                        printf("krffs_hosttest: %d of %d writes left %s with a mode it was "
                               "never given\n",
                               prefix, recorded_count, name);
                        return 0;
                    }
                    if (head.data_size < bytes) {
                        continue;
                    }
                    for (at = 0; at < bytes; at++) {
                        if (state[offset + sizeof(krffs_node_t) + at] != value) {
                            printf("krffs_hosttest: %d of %d writes left %s holding bytes "
                                   "it never had\n",
                                   prefix, recorded_count, name);
                            return 0;
                        }
                    }
                    whole = 1;
                }
                offset = krffs_next_offset(&mount, offset, &head);
            } while (offset != 0 && ++steps < krffs_max_steps(&mount));
            if (must_exist && !whole) {
                printf("krffs_hosttest: %d of %d writes left no whole file called %s\n", prefix,
                       recorded_count, name);
                return 0;
            }
        }
    }
    return 1;
}

/**
 * Whether no state a stop can leave shows the file called `name` holding,
 * inside the length it publishes, a byte other than `value`, the one it was
 * written with. Such a byte is what a shrink that put the surplus's free
 * header on the medium before the shorter length shows a reader with the old
 * length in hand. The states are those `every_stop_file_is_whole` judges.
 */
static int every_stop_keeps_its_bytes(const uint8_t *before, uint32_t size, const char *name,
                                      uint8_t value)
{
    static uint8_t state[IMAGE_SIZE];
    static uint8_t epoch[IMAGE_SIZE];
    krffs_mount_t mount = mount_over(&memory_backend, state, size);
    int prefix;

    for (prefix = 0; prefix <= recorded_count; prefix++) {
        uint64_t seen[64];
        int mark = epoch_of(prefix);
        int seen_count = touched_blocks(mark, prefix, CACHE_BLOCK, seen, 64);
        int variant;

        build(epoch, before, mark, size);
        for (variant = -1; variant < seen_count + (prefix - mark); variant++) {
            uint64_t offset = 0;
            uint64_t steps = 0;
            krffs_node_t head;

            if (variant < seen_count) {
                build(state, before, prefix, size);
                if (variant >= 0) {
                    uint32_t at = (uint32_t)(seen[variant] * CACHE_BLOCK);
                    uint32_t take = size - at > CACHE_BLOCK ? CACHE_BLOCK : size - at;

                    k_memcpy(state + at, epoch + at, take);
                }
            } else {
                int skip = mark + (variant - seen_count);
                int i;

                k_memcpy(state, before, size);
                for (i = 0; i < prefix; i++) {
                    if (i != skip) {
                        k_memcpy(state + recorded[i].offset, recorded[i].bytes, recorded[i].size);
                    }
                }
            }
            if (!forward_chain_closes(&mount)) {
                continue;
            }
            if (krffs_repair_at(&mount, (uint32_t *)0, (uint32_t *)0, (uint32_t *)0) != 0) {
                return 0;
            }
            do {
                if (krffs_head(&mount, offset, &head) != 0) {
                    return 0;
                }
                if (head.type == KRFFS_RESERVED_NODE && k_strcmp_name(head.name, name)) {
                    uint64_t at;

                    for (at = 0; at < head.data_size; at++) {
                        if (state[offset + sizeof(krffs_node_t) + at] != value) {
                            printf("krffs_hosttest: %d of %d writes left %s holding a byte "
                                   "of somebody else's at %d of %d\n",
                                   prefix, recorded_count, name, (int)at, (int)head.data_size);
                            return 0;
                        }
                    }
                }
                offset = krffs_next_offset(&mount, offset, &head);
            } while (offset != 0 && ++steps < krffs_max_steps(&mount));
        }
    }
    return 1;
}

/**
 * Whether no state a stop can leave, losing at most whole blocks, shows a
 * file claiming a byte past `old_length` that is not a zero: what a length
 * published before the zeroes it counts shows when a file grows into slack
 * it already had.
 */
static int every_stop_is_zero_filled(const uint8_t *before, uint32_t size, uint64_t old_length)
{
    static uint8_t state[IMAGE_SIZE];
    static uint8_t epoch[IMAGE_SIZE];
    krffs_mount_t mount = mount_over(&memory_backend, state, size);
    int prefix;

    for (prefix = 0; prefix <= recorded_count; prefix++) {
        uint64_t seen[64];
        int mark = epoch_of(prefix);
        int seen_count = touched_blocks(mark, prefix, CACHE_BLOCK, seen, 64);
        int variant;

        build(epoch, before, mark, size);
        for (variant = -1; variant < seen_count; variant++) {
            uint64_t offset = 0;
            uint64_t steps = 0;
            krffs_node_t head;

            build(state, before, prefix, size);
            if (variant >= 0) {
                uint32_t at = (uint32_t)(seen[variant] * CACHE_BLOCK);
                uint32_t take = size - at > CACHE_BLOCK ? CACHE_BLOCK : size - at;

                k_memcpy(state + at, epoch + at, take);
            }
            if (!forward_chain_closes(&mount)) {
                continue;
            }
            do {
                if (krffs_head(&mount, offset, &head) != 0) {
                    return 0;
                }
                if (head.type == KRFFS_RESERVED_NODE) {
                    uint64_t at;

                    for (at = old_length; at < head.data_size; at++) {
                        if (state[offset + sizeof(krffs_node_t) + at] != 0) {
                            printf("krffs_hosttest: %d of %d writes left a file claiming a "
                                   "byte it did not zero\n",
                                   prefix, recorded_count);
                            return 0;
                        }
                    }
                }
                offset = krffs_next_offset(&mount, offset, &head);
            } while (offset != 0 && ++steps < krffs_max_steps(&mount));
        }
    }
    return 1;
}

/**
 * Whether every named node in the image has its name to itself, which is what
 * an operation that reports success must have left.
 */
static int no_name_is_on_two_nodes(krffs_mount_t *mount)
{
    uint64_t offset = 0;
    uint64_t steps = 0;
    krffs_node_t head;

    do {
        if (krffs_head(mount, offset, &head) != 0) {
            return 0;
        }
        if (head.type == KRFFS_RESERVED_NODE && head.name[0] != '\0') {
            uint64_t scan = krffs_next_offset(mount, offset, &head);
            uint64_t inner = 0;
            krffs_node_t other;

            while (scan != 0 && ++inner < krffs_max_steps(mount)) {
                if (krffs_head(mount, scan, &other) != 0) {
                    return 0;
                }
                if (other.type == KRFFS_RESERVED_NODE && other.parent_id == head.parent_id &&
                    k_strcmp_name(other.name, (const char *)head.name)) {
                    return 0;
                }
                scan = krffs_next_offset(mount, scan, &other);
            }
        }
        offset = krffs_next_offset(mount, offset, &head);
    } while (offset != 0 && ++steps < krffs_max_steps(mount));
    return 1;
}

/**
 * Run `what` on `before`, copied into `img`, with one of the backend's calls
 * failing, and judge what the file system was left with.
 *
 * A non-negative `which` fails that write; a negative one fails the call at
 * position `-which - 1` if it is a sync, since a medium can fail either. The
 * core must not publish a change over a call that failed, and what it leaves
 * must be an image a mount can walk with no file half made under a name: the
 * file the create makes, "second", is in the root with mode 0644 wherever it
 * has its name.
 *
 * An operation that reports success must also leave no name on two nodes,
 * judged before the check has had the image: a move that could not free the
 * node it copied from would leave two nodes under one name, and a caller
 * told the operation succeeded would read whichever the walk reaches first.
 */
static int survives_refusal(uint8_t *img, const uint8_t *before, uint32_t size,
                            int (*what)(krffs_mount_t *), int which)
{
    krffs_mount_t mount = mount_over(&recording_backend, img, size);
    uint64_t offset = 0;
    uint64_t steps = 0;
    krffs_node_t head;
    int answer;

    k_memcpy(img, before, size);
    recording = 0;
    writes_seen = 0;
    if (which >= 0) {
        refused_write = which;
    } else {
        refused_sync = -which - 1;
    }
    answer = what(&mount);
    refused_write = -1;
    refused_sync = -1;

    if (!forward_chain_closes(&mount)) {
        return 0;
    }
    if (answer == 0 && !no_name_is_on_two_nodes(&mount)) {
        return 0;
    }
    if (krffs_repair_at(&mount, (uint32_t *)0, (uint32_t *)0, (uint32_t *)0) != 0) {
        return 0;
    }
    do {
        if (krffs_head(&mount, offset, &head) != 0) {
            return 0;
        }
        if (head.type == KRFFS_RESERVED_NODE) {
            uint64_t at;

            for (at = 0; at < head.data_size; at++) {
                if (img[offset + sizeof(krffs_node_t) + at] == POISON) {
                    return 0;
                }
            }
            if (k_strcmp_name(head.name, "second") &&
                (head.parent_id != 0 || (head.mode & 07777u) != 0644)) {
                return 0;
            }
        }
        offset = krffs_next_offset(&mount, offset, &head);
    } while (offset != 0 && ++steps < krffs_max_steps(&mount));
    return krffs_chain_is_valid(&mount);
}

/**
 * Create /second, a name the image does not have, so that the allocator
 * runs: a create of a name that exists returns success without taking a
 * node. This and the two operations after it are the ones the sweep of
 * failing calls runs; each returns what the core returned, and one that
 * reports success is held to having made its change.
 */
static int make_a_file(krffs_mount_t *mount)
{
    return krffs_create_at(mount, "/second");
}

/** Grow /refused to 3000 bytes. */
static int grow_a_file(krffs_mount_t *mount)
{
    return krffs_truncate_at(mount, "/refused", 3000, (uint64_t *)0);
}

/** Remove /refused. */
static int remove_a_file(krffs_mount_t *mount)
{
    return krffs_unlink_at(mount, "/refused");
}

/**
 * Check the kernel's `krffs_calculate_djb_hash` and `krffs_mix_id` against
 * the vectors krffs_hash_hosttest.c checks the host stager's hashes against.
 */
static void hash_rows(void)
{
#define HASH_VECTOR(parent, name, djb, mixed)                       \
    check(krffs_calculate_djb_hash((const uint8_t *)name) == djb && \
              krffs_mix_id(parent, name) == mixed,                  \
          "kernel hash vector");
    KRFFS_HASH_VECTORS
#undef HASH_VECTOR
}

/**
 * Check the inode numbers the root reports.
 *
 * Two regular files of one name in different directories must not share a
 * number: `cp -a` takes a shared number for a hard link and copies the pair
 * as one file. A file's number is its parent's id mixed with its name, and a
 * directory's number is its id, each folded to 32 bits.
 */
static void ino_rows(void)
{
    krffs_node_t a, b, dir;

    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    memset(&dir, 0, sizeof(dir));
    a.type = b.type = dir.type = KRFFS_RESERVED_NODE;
    memcpy(a.name, "authorized_keys", 16);
    memcpy(b.name, "authorized_keys", 16);
    a.id = b.id = krffs_calculate_djb_hash(a.name);
    a.parent_id = 11;
    b.parent_id = 12;
    check(krffs_node_ino(&a) != krffs_node_ino(&b),
          "files of one name in two directories have different inode numbers");
    check(krffs_node_ino(&a) == krffs_fold_ino(krffs_mix_id(a.parent_id, (const char *)a.name)),
          "a file's inode number is its parent's id mixed with its name, folded");
    check(krffs_node_ino(&a) <= 0xFFFFFFFFu && krffs_node_ino(&b) <= 0xFFFFFFFFu,
          "a file's inode number fits 32 bits");
    memcpy(dir.name, ".ssh", 5);
    dir.mode = KRFFS_S_IFDIR | 0700;
    dir.id = 424242;
    check(krffs_node_ino(&dir) == 424242, "a directory's inode number is its id");
}

/**
 * Check the inode numbers of the root and of unlinked open files. The root,
 * whose mode here has no type bits, is number 1 through every path that
 * reaches it, by offset and by pointer. An open file keeps its node after
 * an unlink, which `krffs_orphan_at` leaves allocated, and two such siblings
 * keep the distinct numbers they had.
 */
static void ino_lifetime_rows(void)
{
    krffs_mount_t mount;
    krffs_node_t a, b, root;
    uint64_t one, two, ino_one, ino_two;
    const char *paths[] = {"/", "/..", "/tmp/..", "/tmp/../.."};
    unsigned i;

    make_image(image, IMAGE_SIZE);
    ((krffs_node_t *)image)->mode = 0755;
    mount = mount_over(&memory_backend, image, IMAGE_SIZE);
    check(init_krffs(image, IMAGE_SIZE) == 0, "inode fixture mounts");
    check(krffs_mkdir_at(&mount, "/tmp", 0755, 0, 0) == 0, "inode fixture directory");
    for (i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        uint64_t at = krffs_walk(&mount, paths[i]);
        krffs_node_t *node = krffs_resolve(paths[i]);

        check(at == 0 && krffs_head(&mount, at, &root) == 0 && krffs_node_ino(&root) == 1,
              "root inode is 1 through offset paths");
        check(node == krffs_fs->node && krffs_node_ino(node) == 1,
              "root inode is 1 through pointer paths");
    }
    check(krffs_create_at(&mount, "/tmp/one") == 0 && krffs_create_at(&mount, "/tmp/two") == 0,
          "inode fixture siblings");
    one = krffs_walk(&mount, "/tmp/one");
    two = krffs_walk(&mount, "/tmp/two");
    check(krffs_head(&mount, one, &a) == 0 && krffs_head(&mount, two, &b) == 0,
          "inode fixture headers");
    ino_one = krffs_node_ino(&a);
    ino_two = krffs_node_ino(&b);
    check(ino_one != ino_two, "live siblings have distinct inode numbers");
    krffs_orphan_at(&mount, one);
    krffs_orphan_at(&mount, two);
    check(krffs_walk(&mount, "/tmp/one") == KRFFS_NO_NODE &&
              krffs_walk(&mount, "/tmp/two") == KRFFS_NO_NODE,
          "unlinked siblings have no names");
    check(krffs_head(&mount, one, &a) == 0 && krffs_head(&mount, two, &b) == 0 &&
              krffs_node_ino(&a) == ino_one && krffs_node_ino(&b) == ino_two &&
              krffs_node_ino(&a) != krffs_node_ino(&b),
          "open unlinked siblings keep distinct unchanged inode numbers");
}

/**
 * Make a node whose id falls across a unit of the medium, with the
 * whole-unit rule on.
 *
 * A node being made is reached by no path until its name is written, last
 * and after a sync point, so a torn id or parent in it is read by nobody,
 * and the rule does not apply to them: a create must succeed where its free
 * node puts the id astride a unit. The spacer puts the next node's id at
 * 4092, four of its bytes on each side of the line, and the new name's hash
 * changes bytes on both sides. A directory is made in the same way, and its
 * id, which its children name it by, is the mix of its parent's and its
 * name.
 */
static void straddle_create_rows(void)
{
    krffs_mount_t mount;
    krffs_node_t head;
    uint64_t across;
    uint64_t id = krffs_calculate_djb_hash((const uint8_t *)"across");
    const uint8_t *bytes = (const uint8_t *)&id;
    int below = 0, above = 0, i;

    make_image(image, IMAGE_SIZE);
    mount = mount_over(&memory_backend, image, IMAGE_SIZE);
    check(krffs_create_at(&mount, "/spacer") == 0 &&
              krffs_truncate_at(&mount, "/spacer", 3375, (uint64_t *)0) == 0,
          "a file grown so the next node's id falls across a block");
    for (i = 0; i < 8; i++) {
        if (image[4092 + i] != bytes[i]) {
            if (i < 4) {
                below = 1;
            } else {
                above = 1;
            }
        }
    }
    check(below && above, "the new id changes bytes on both sides of the line");
    mount.grain = 4096;
    check(krffs_create_at(&mount, "/across") == 0,
          "a node whose id falls across a block is made with the whole-unit rule on");
    mount.grain = 0;
    across = krffs_walk(&mount, "/across");
    check(across != KRFFS_NO_NODE && across + KRFFS_FIELD_ID == 4092,
          "and it is the node whose id straddles the block's end");
    check(krffs_head(&mount, across, &head) == 0 && head.id == id &&
              head.parent_id == KRFFS_ROOT_ID,
          "with its whole id and its parent");
    check(krffs_chain_is_valid(&mount) == 1, "the chain is sound after the create");

    make_image(image, IMAGE_SIZE);
    check(krffs_create_at(&mount, "/spacer") == 0 &&
              krffs_truncate_at(&mount, "/spacer", 3375, (uint64_t *)0) == 0,
          "a file grown so the next directory's id falls across a block");
    mount.grain = 4096;
    check(krffs_mkdir_at(&mount, "/across", 0755, 0, 0) == 0,
          "a directory whose id falls across a block is made with the whole-unit rule on");
    mount.grain = 0;
    across = krffs_walk(&mount, "/across");
    check(across != KRFFS_NO_NODE && across + KRFFS_FIELD_ID == 4092 &&
              krffs_head(&mount, across, &head) == 0 &&
              head.id == krffs_mix_id(KRFFS_ROOT_ID, "across"),
          "and it has its whole id");
    check(krffs_create_at(&mount, "/across/inner") == 0 &&
              krffs_walk(&mount, "/across/inner") != KRFFS_NO_NODE,
          "and a child made in it is reached through it");
}

/**
 * Check that a directory whose identifier another directory already has is
 * not made.
 *
 * A directory's identifier is its parent's identifier mixed with the DJB
 * hash of its name, and a child names its parent by that identifier alone.
 * `bC` and `cb` hash alike, so a second directory of the two in one parent
 * would share the first one's children. The first is made and given a
 * child; the second must fail with `KRFFS_ETAKEN`, a result of its own that
 * the adapters report as `EINVAL` and not as a full or a failing volume; the
 * first must keep its child, and the name that failed must reach nothing.
 */
static void id_collision_rows(void)
{
    krffs_mount_t mount;
    char name[64];
    int is_dir = 0;
    int rc;

    make_image(image, IMAGE_SIZE);
    mount = mount_over(&memory_backend, image, IMAGE_SIZE);
    check(krffs_mix_id(KRFFS_ROOT_ID, "bC") == krffs_mix_id(KRFFS_ROOT_ID, "cb"),
          "bC and cb mix to one identifier in one directory");
    check(krffs_mkdir_at(&mount, "/bC", 0755, 0, 0) == 0,
          "the first of two colliding directories is made");
    check(krffs_create_at(&mount, "/bC/kept") == 0, "and given a child");
    rc = krffs_mkdir_at(&mount, "/cb", 0755, 0, 0);
    check(rc != 0, "a second directory with the first one's identifier is refused");
    check(rc == KRFFS_ETAKEN,
          "with the result of a taken identifier, which names no full or failing volume");
    check(krffs_walk(&mount, "/cb") == KRFFS_NO_NODE, "the refused directory is not made");
    check(krffs_walk(&mount, "/cb/kept") == KRFFS_NO_NODE,
          "the refused name does not reach the first directory's child");
    check(krffs_walk(&mount, "/bC/kept") != KRFFS_NO_NODE, "the first directory keeps its child");
    check(krffs_readdir_at(&mount, "/", 0, name, sizeof(name), &is_dir) == 1 &&
              k_strcmp_name((const uint8_t *)name, "bC") && is_dir == 1 &&
              krffs_readdir_at(&mount, "/", 1, name, sizeof(name), &is_dir) == 0,
          "the root lists the first directory alone");
    check(krffs_chain_is_valid(&mount) == 1, "the chain is sound after the refusal");
}

/**
 * Check parent lookup when an earlier non-directory has the directory's id.
 *
 * The file name's DJB hash equals the mixed id of the directory name. Each
 * node is made through the core, with the non-directory first in the ring.
 * A socket also checks that directory lookup compares the whole mode type.
 */
static void parent_id_collision_rows(void)
{
    static const uint16_t modes[] = {0644, KRFFS_S_IFIFO | 0644, KRFFS_S_IFSOCK | 0644};
    const char *file = "/Qraaf`{k>y";
    const char *dir = "/dirkwyusovoyvxvwmj";
    const char *child = "/dirkwyusovoyvxvwmj/child";
    const char *leaf = "/dirkwyusovoyvxvwmj/child/leaf";
    uint32_t i;

    check(krffs_calculate_djb_hash((const uint8_t *)file + 1) ==
              krffs_mix_id(KRFFS_ROOT_ID, dir + 1),
          "the non-directory name hash equals the directory id");
    for (i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        krffs_mount_t mount;
        uint64_t file_at;
        uint64_t dir_at;
        uint64_t child_at;
        uint64_t leaf_at;
        char path[64];

        make_image(image, IMAGE_SIZE);
        mount = mount_over(&memory_backend, image, IMAGE_SIZE);
        check(krffs_create_node_at(&mount, file, modes[i], 0, 0) == 0,
              "the non-directory with the colliding hash is made first");
        check(krffs_mkdir_at(&mount, dir, 0755, 0, 0) == 0,
              "a non-directory id does not prevent directory creation");
        check(krffs_mkdir_at(&mount, child, 0755, 0, 0) == 0 && krffs_create_at(&mount, leaf) == 0,
              "the colliding directory has a child and grandchild");
        file_at = krffs_walk(&mount, file);
        dir_at = krffs_walk(&mount, dir);
        child_at = krffs_walk(&mount, child);
        leaf_at = krffs_walk(&mount, leaf);
        check(file_at != KRFFS_NO_NODE && dir_at != KRFFS_NO_NODE && file_at < dir_at,
              "the non-directory precedes the directory in the ring");
        check(krffs_walk(&mount, "/dirkwyusovoyvxvwmj/child/..") == dir_at,
              "dot-dot skips a non-directory with the parent's id");
        check(krffs_walk(&mount, "/dirkwyusovoyvxvwmj/child/../child/leaf") == leaf_at,
              "a walk continues through the directory after dot-dot");
        check(krffs_parent_offset(&mount, child_at) == dir_at &&
                  krffs_parent_offset(&mount, leaf_at) == child_at &&
                  krffs_parent_offset(&mount, dir_at) == 0,
              "parent offsets find directories and the root");
        check(krffs_node_path_at(&mount, child_at, path, sizeof(path)) == 0 &&
                  k_strcmp_name((const uint8_t *)path, child),
              "the child's path uses the directory with its parent's id");
        check(krffs_node_path_at(&mount, leaf_at, path, sizeof(path)) == 0 &&
                  k_strcmp_name((const uint8_t *)path, leaf),
              "the grandchild's path uses the directory with its ancestor's id");
        check(krffs_chain_is_valid(&mount) == 1, "the colliding ids leave a valid chain");
    }
}

/* The fields of a header whose values a node is made or moved with: the
   mode, the owner, the group, the three times and the length. */
static const uint32_t across_fields[] = {KRFFS_FIELD_MODE,     KRFFS_FIELD_UID,   KRFFS_FIELD_GID,
                                         KRFFS_FIELD_ATIME,    KRFFS_FIELD_MTIME, KRFFS_FIELD_CTIME,
                                         KRFFS_FIELD_DATA_SIZE};

/**
 * Whether the node at `at` has `mode`, the owner and group 1000, and the
 * clock's moment in all three times.
 */
static int has_given_fields(krffs_mount_t *mount, uint64_t at, uint16_t mode)
{
    krffs_node_t head;

    return krffs_head(mount, at, &head) == 0 && head.mode == mode && head.uid == 1000 &&
           head.gid == 1000 && head.access_time.seconds == fake_seconds &&
           head.access_time.nanoseconds == fake_nanoseconds &&
           head.modification_time.seconds == fake_seconds &&
           head.modification_time.nanoseconds == fake_nanoseconds &&
           head.attribute_change_time.seconds == fake_seconds &&
           head.attribute_change_time.nanoseconds == fake_nanoseconds;
}

/** Whether the node at `at` holds exactly the `size` bytes of `bytes`. */
static int holds_bytes(krffs_mount_t *mount, uint64_t at, const char *bytes, uint32_t size)
{
    char back[1024];
    krffs_node_t head;
    uint32_t i;

    if (size > sizeof(back) || krffs_head(mount, at, &head) != 0 || head.data_size != size ||
        krffs_read_at(mount, at, back, size, 0) != (int)size) {
        return 0;
    }
    for (i = 0; i < size; i++) {
        if (back[i] != bytes[i]) {
            return 0;
        }
    }
    return 1;
}

/**
 * Create `/spacer` and grow it so that the node after it starts `gap` bytes
 * past `at`. Returns 1 when both succeed.
 */
static int place_after_spacer(krffs_mount_t *mount, uint64_t at, uint32_t gap)
{
    return krffs_create_at(mount, "/spacer") == 0 &&
           krffs_truncate_at(mount, "/spacer", (uint32_t)(at - 2 * sizeof(krffs_node_t)) + gap,
                             (uint64_t *)0) == 0;
}

/**
 * Make and move nodes whose fields fall across a unit of the medium, with the
 * whole-unit rule on and a clock whose nanoseconds are not zero.
 *
 * For each of `across_fields`, the node is placed so that the field's first
 * byte is the last of one 4 KiB block and its second the first of the next,
 * and the value it is given differs in both of those bytes from what the
 * place had. A node being made is read by nobody until its name is written
 * after every other field, and a copy being filled for a move until its type
 * is, so their fields are written without the rule: a file and a directory
 * must be made at each place with every field they were given, a node must
 * be made over a removed node whose length crosses the line, and a file that
 * has to move to each place must move there with its fields and its bytes.
 *
 * For the move, the spacer holds the place and 2000 bytes after it while the
 * file and a node after the file are made; cut back, it leaves a free node
 * with a fresh header at the place, and the file, which cannot grow into the
 * node after it, has to move there. For the removed node, the length of the
 * file removed has its first two bytes on the two sides of the line, and the
 * new node's length of 0 changes both.
 */
static void across_rows(void)
{
    krffs_mount_t mount;
    char bytes[767];
    uint64_t at;
    uint64_t node;
    krffs_node_t head;
    uint32_t i;

    fake_nanoseconds = 123456789;
    for (i = 0; i < sizeof(bytes); i++) {
        bytes[i] = (char)(i * 7 + 1);
    }
    for (i = 0; i < sizeof(across_fields) / sizeof(across_fields[0]); i++) {
        at = 4096 - across_fields[i] - 1;

        make_image(image, IMAGE_SIZE);
        mount = mount_over(&memory_backend, image, IMAGE_SIZE);
        check(place_after_spacer(&mount, at, 0),
              "a file placed so a field of the next falls across");
        mount.grain = 4096;
        check(krffs_create_node_at(&mount, "/made", 0644, 1000, 1000) == 0,
              "a node whose field falls across a block is made with the whole-unit rule on");
        mount.grain = 0;
        check(krffs_walk(&mount, "/made") == at && has_given_fields(&mount, at, 0644),
              "and it is at the place with every field it was given");

        make_image(image, IMAGE_SIZE);
        check(place_after_spacer(&mount, at, 0),
              "a file placed so a field of the next falls across");
        mount.grain = 4096;
        check(krffs_mkdir_at(&mount, "/made", 0644, 1000, 1000) == 0,
              "a directory whose field falls across a block is made with the whole-unit rule on");
        mount.grain = 0;
        check(krffs_walk(&mount, "/made") == at &&
                  has_given_fields(&mount, at, (uint16_t)(KRFFS_S_IFDIR | 0644)),
              "and the directory is at the place with every field it was given");

        make_image(image, IMAGE_SIZE);
        check(place_after_spacer(&mount, at, 2000) &&
                  krffs_create_node_at(&mount, "/mover", 0644, 1000, 1000) == 0,
              "a file past the place");
        node = krffs_walk(&mount, "/mover");
        check(krffs_write_at(&mount, &node, bytes, 511, 0) == 511 &&
                  krffs_create_at(&mount, "/after") == 0 &&
                  krffs_truncate_at(&mount, "/spacer", (uint32_t)(at - 2 * sizeof(krffs_node_t)),
                                    (uint64_t *)0) == 0,
              "with a node after it and a free node at the place");
        mount.grain = 4096;
        check(krffs_write_at(&mount, &node, bytes + 511, 256, 511) == 256,
              "a file that has to move where its field falls across a block moves with the "
              "whole-unit rule on");
        mount.grain = 0;
        check(node == at && krffs_walk(&mount, "/mover") == at &&
                  has_given_fields(&mount, at, 0644) &&
                  holds_bytes(&mount, at, bytes, (uint32_t)sizeof(bytes)),
              "and it is at the place with every field and every byte");
        check(krffs_chain_is_valid(&mount) == 1, "the chain is sound after the move");
    }

    at = 4096 - KRFFS_FIELD_DATA_SIZE - 1;
    make_image(image, IMAGE_SIZE);
    check(place_after_spacer(&mount, at, 0) && krffs_create_at(&mount, "/old") == 0,
          "a file at the place");
    node = krffs_walk(&mount, "/old");
    check(krffs_write_at(&mount, &node, bytes, 511, 0) == 511 &&
              krffs_create_at(&mount, "/after") == 0 && krffs_unlink_at(&mount, "/old") == 0,
          "removed with a length across the line and a node after it");
    mount.grain = 4096;
    check(krffs_create_at(&mount, "/made") == 0,
          "a node made over a removed node's length across a block is made with the whole-unit "
          "rule on");
    mount.grain = 0;
    check(krffs_walk(&mount, "/made") == at && krffs_head(&mount, at, &head) == 0 &&
              head.data_size == 0,
          "and it is at the place with a length of 0");
    fake_nanoseconds = 0;
}

/**
 * Run the rows of the functions above, then these, and return the count of
 * failures, at most 255:
 *
 * - The mount check. A fresh image is sound; a bad magic, an unknown node
 *   type, a size that leaves the region or is below a header, data larger
 *   than its node, a backward link the forward chain contradicts, a root
 *   whose backward link is not the last node, and a name that fills its
 *   field, which has no end, are rejected. A free node's length is no fault
 *   whatever it says: a free node keeps the length of what was removed
 *   there, and a split leaves one larger than the node between the size it
 *   publishes and the length it writes after it. A size no region could hold
 *   allocates nothing, before the arithmetic that adds a header can wrap.
 * - The operations: a create, a write, a write past the end, whose hole
 *   reads as zeroes, a truncate, a grow, which reads as zeroes, and a grow
 *   with a node in the way, which moves the file with its contents; the
 *   directory operations; a rename; and an orphan, a node no path reaches,
 *   which the reaper frees.
 * - A field the medium would reach in two pieces. A backend that puts whole
 *   blocks on the medium writes a field across two of them in two writes,
 *   and a stop between them leaves a value nobody wrote, so with the
 *   whole-unit rule on the core fails such a write. Only the bytes that
 *   change count: on an owner field that ends one byte into the second
 *   block, a value that changes one byte on one side of the line is written.
 * - A rename over an existing name, where the rule fails the source's new
 *   id, at 4092 across the line, must fail before the destination is
 *   removed, or it would leave one name where there were two.
 * - One name on two nodes, as a rename that published its new parent and
 *   stopped before its new name leaves them. The repair keeps the name on
 *   the node with more data, then the larger, but never takes a directory's
 *   name: its children name it by its id, and clearing it would put a whole
 *   tree out of reach.
 * - The ends of the region: the whole free node can be taken and given
 *   back, and a remainder too small for a header of its own stays in the
 *   node.
 * - The order of writes, over free space filled with `POISON`: every stop
 *   inside a create, a write, a grow, a truncate, a grow of less than a
 *   header, a move and a removal leaves a chain a mount can walk. A grow of
 *   less than a header publishes two sizes: the tail's header lands where
 *   the free node's own header was, so the first size must be on the medium
 *   before it is written. The file a move copies is filled with a byte of
 *   its own, so that a copy that did not reach the medium shows in what the
 *   moved node holds.
 * - A length raised inside the slack a node already has shows no byte the
 *   file held before. A shrink whose surplus is large enough for a header of
 *   its own writes that header inside the bytes the old length counts, so
 *   the shorter length must be on the medium first, and the file, filled
 *   with a byte of its own, may show nothing else inside the length it
 *   publishes.
 * - A backend that fails one call: each write and each sync of a grow, a
 *   removal and a create, in turn. A second file right behind the first
 *   leaves the grow no room, so it must copy the file elsewhere, 64 bytes at
 *   a time, and free the node it came from, and the count of calls runs well
 *   past the fields; a call that never happens fails nothing.
 */
/**
 * A write to the offset of a node that has been freed is refused and leaves
 * the free node as it was. A move whose removal of the old node fails after
 * it stored the free type leaves the holders that offset, and a write there
 * would otherwise put the file's bytes, and a length, into free space.
 */
static void freed_write_rows(void)
{
    krffs_mount_t mount;
    krffs_node_t head;
    uint64_t gone;

    make_image(image, IMAGE_SIZE);
    mount = mount_over(&memory_backend, image, IMAGE_SIZE);
    check(krffs_create_at(&mount, "/gone") == 0, "a file to remove is created");
    gone = krffs_walk(&mount, "/gone");
    check(krffs_remove_at(&mount, gone) == 0, "and its node is freed");
    check(krffs_write_at(&mount, &gone, "0123456789abcdef", 16, 0) == KRFFS_EBAD,
          "a write to the freed node is refused");
    check(krffs_head(&mount, gone, &head) == 0 && head.type == KRFFS_FREE_NODE &&
              head.data_size == 0,
          "and the free node is left as it was");
    check(krffs_chain_is_valid(&mount) == 1, "the chain is sound after the refusal");
}

/* A disk-root fixture, in forward order, with NULL for a free node. */
#define DISK_IMAGE_SIZE (2 * IMAGE_SIZE)

static uint8_t disk_image[DISK_IMAGE_SIZE];

struct unlink_extent {
    uint32_t start;
    const char *name;
};

/** Write a dirty image of `size` bytes at `buffer` with these extents and
 * one byte in each file. */
static void make_extent_image(uint8_t *buffer, uint32_t size, const struct unlink_extent *extents,
                              unsigned count)
{
    uint32_t previous_size = size - extents[count - 1].start;
    unsigned i;

    k_memset(buffer, 0, size);
    for (i = 0; i < count; i++) {
        uint32_t start = extents[i].start;
        uint32_t end = i + 1 < count ? extents[i + 1].start : size;
        krffs_node_t *head = (krffs_node_t *)(buffer + start);
        const char *name = extents[i].name;

        head->magic = KRFFS_FILE_SYSTEM_MAGIC;
        head->size = end - start;
        head->previous_node_size = previous_size;
        if (i == 0) {
            head->type = KRFFS_ROOT_NODE;
            head->mode = 0755 | KRFFS_MODE_DIRTY;
        } else if (name) {
            head->type = KRFFS_RESERVED_NODE;
            head->mode = 0644;
            head->data_size = 1;
            k_memcpy(head->name, name, k_strlen(name) + 1);
            buffer[start + sizeof(*head)] = (uint8_t)name[0];
        } else {
            head->type = KRFFS_FREE_NODE;
        }
        previous_size = end - start;
    }
}

/** `make_extent_image` of the disk fixture. */
static void make_unlink_image(const struct unlink_extent *extents, unsigned count)
{
    make_extent_image(disk_image, DISK_IMAGE_SIZE, extents, count);
}

/** Mount each cut, including sectors of the first unlink when two ran. */
static void mount_unlink_cuts(const uint8_t *before, uint32_t grain, int first_unlink)
{
    static uint8_t after[DISK_IMAGE_SIZE];
    static uint8_t middle[DISK_IMAGE_SIZE];
    static uint8_t state[DISK_IMAGE_SIZE];
    int prefix;

    build(middle, before, first_unlink, DISK_IMAGE_SIZE);
    for (prefix = 0; prefix <= recorded_count; prefix++) {
        uint64_t units[8];
        int count = touched_blocks(0, prefix, grain, units, 8);
        const uint8_t *versions[] = {before, after, middle};
        unsigned version_count = first_unlink > 0 && prefix > first_unlink ? 3u : 2u;
        unsigned variants = 1;
        unsigned variant;
        int i;

        if (count == 8) {
            check(0, "unlink recorder touched too many units");
            return;
        }
        for (i = 0; i < count; i++) {
            variants *= version_count;
        }
        build(after, before, prefix, DISK_IMAGE_SIZE);
        for (variant = 0; variant < variants; variant++) {
            unsigned choices = variant;
            char byte;
            uint64_t kept;

            k_memcpy(state, before, DISK_IMAGE_SIZE);
            for (i = 0; i < count; i++) {
                uint32_t at = (uint32_t)units[i] * grain;
                const uint8_t *version = versions[choices % version_count];

                k_memcpy(state + at, version + at, grain);
                choices /= version_count;
            }
            if (init_krffs_disk_root(state, DISK_IMAGE_SIZE, 512, NULL, NULL) != 0 ||
                !krffs_chain_is_valid(&krffs_root_mount)) {
                printf("krffs_hosttest: unlink cut after %d writes, unit %u, variant %u\n", prefix,
                       grain, variant);
                check(0, "disk root mounts and repairs its links after an unlink cut");
                return;
            }
            kept = krffs_walk(&krffs_root_mount, "/kept");
            check(krffs_read_at(&krffs_root_mount, kept, &byte, 1, 0) == 1 && byte == 'k',
                  "every unlink cut keeps the neighbouring file's data");
            check(krffs_is_dirty(&krffs_root_mount), "repair keeps the disk root dirty");
        }
    }
}

/** Record a disk-root unlink and check the completed merge and its cuts. */
static void disk_unlink_case(const struct unlink_extent *extents, unsigned count, uint32_t free_at,
                             uint32_t free_size, const char *second)
{
    static uint8_t before[DISK_IMAGE_SIZE];
    krffs_node_t head;
    int first_unlink = 0;
    int result;

    make_unlink_image(extents, count);
    k_memcpy(before, disk_image, DISK_IMAGE_SIZE);
    result = init_krffs_disk_root(disk_image, DISK_IMAGE_SIZE, 512, NULL, NULL);
    check(result == 0, "disk unlink fixture mounts");
    if (result != 0) {
        return;
    }
    krffs_root_mount.backend = &recording_backend;
    recorded_count = 0;
    sync_count = 0;
    recording = 1;
    result = krffs_unlink_at(&krffs_root_mount, "/victim");
    if (second && result == 0) {
        first_unlink = recorded_count;
        result = krffs_unlink_at(&krffs_root_mount, second);
    }
    recording = 0;
    check(result == 0 && krffs_walk(&krffs_root_mount, "/victim") == KRFFS_NO_NODE &&
              (!second || krffs_walk(&krffs_root_mount, second) == KRFFS_NO_NODE) &&
              krffs_chain_is_valid(&krffs_root_mount),
          "completed disk-root unlink leaves a valid ring without the victim");
    check(krffs_head(&krffs_root_mount, free_at, &head) == 0 && head.type == KRFFS_FREE_NODE &&
              head.size == free_size,
          "unlink coalesces free space only where its size cannot tear");
    mount_unlink_cuts(before, CACHE_BLOCK, first_unlink);
    mount_unlink_cuts(before, 512, first_unlink);
}

/* The disk root's flushes `record_disk_flush` has seen, and of them those
   made while the root owed a check. */
static uint32_t rec_flushes;
static uint32_t rec_flushes_owed;

/** Record a disk writeback barrier in the order of recorded writes. */
static int record_disk_flush(void)
{
    rec_flushes++;
    rec_flushes_owed += krffs_root_check_owed();
    return record_sync(NULL);
}

/** A shrink puts the shorter length on disk before it reuses any data. */
static void disk_shrink_row(void)
{
    static const struct unlink_extent extents[] = {{0, NULL}, {1024, "victim"}, {9216, "kept"}};
    static const krffs_backend_t backend = {memory_read, record_write, memory_sync, 0};
    krffs_node_t *victim;
    uint32_t length = 3000;
    uint64_t result;

    make_unlink_image(extents, sizeof(extents) / sizeof(extents[0]));
    victim = (krffs_node_t *)(disk_image + extents[1].start);
    victim->data_size = victim->size - sizeof(*victim);
    k_memset((uint8_t *)victim + sizeof(*victim), 'v', (uint32_t)victim->data_size);
    if (init_krffs_disk_root(disk_image, DISK_IMAGE_SIZE, 512, record_disk_flush, NULL) != 0) {
        check(0, "disk shrink fixture mounts");
        return;
    }
    krffs_root_mount.backend = &backend;
    recorded_count = 0;
    sync_count = 0;
    recording = 1;
    result = krffs_resize_at(&krffs_root_mount, extents[1].start, length);
    recording = 0;
    check(result == extents[1].start && krffs_chain_is_valid(&krffs_root_mount),
          "disk shrink leaves a valid ring");
    check(recorded_count == 4 && sync_count == 2 && sync_marks[0] == 1 && sync_marks[1] == 2 &&
              recorded[0].offset == extents[1].start + KRFFS_FIELD_DATA_SIZE &&
              recorded[1].offset == extents[1].start + sizeof(*victim) + length &&
              recorded[2].offset == extents[1].start + KRFFS_FIELD_SIZE,
          "disk shrink flushes its length before the free header and the header before its size");
}

/* What a judge of a cut expects of the file it follows: its name, the
   lengths a stop may leave, the bytes below `cut_kept` it must keep, and
   the byte every byte from there to the length it has must be: what the
   operation wrote, or what the file had. */
static const char *cut_name;
static uint64_t cut_low;
static uint64_t cut_high;
static uint64_t cut_kept;
static uint8_t cut_byte;
static uint8_t cut_grown;

/** Whether the file `cut_name` names, read through `mount`, has a length in
 * [cut_low, cut_high], keeps its first `cut_kept` bytes, and has
 * `cut_grown` in every byte after them that its length counts. */
static int cut_file_is_whole(krffs_mount_t *mount)
{
    uint64_t node = krffs_walk(mount, cut_name);
    krffs_node_t head;
    uint8_t byte;
    uint64_t at;

    if (krffs_head(mount, node, &head) != 0 || head.data_size < cut_low ||
        head.data_size > cut_high) {
        return 0;
    }
    for (at = 0; at < head.data_size; at++) {
        if (krffs_read_at(mount, node, (char *)&byte, 1, (uint32_t)at) != 1 ||
            byte != (at < cut_kept ? cut_byte : cut_grown)) {
            return 0;
        }
    }
    return 1;
}

/** A cut of a disk root: it mounts, repairs its links, and keeps the file
 * and the neighbour /kept. */
static int disk_cut_is_sound(uint8_t *state, uint32_t size)
{
    uint64_t kept;
    char byte;

    if (init_krffs_disk_root(state, size, 512, NULL, NULL) != 0 ||
        !krffs_chain_is_valid(&krffs_root_mount)) {
        return 0;
    }
    kept = krffs_walk(&krffs_root_mount, "/kept");
    return krffs_read_at(&krffs_root_mount, kept, &byte, 1, 0) == 1 && byte == 'k' &&
           cut_file_is_whole(&krffs_root_mount);
}

/** A cut of a volume of 4 KiB blocks and 512-byte sectors: its chain
 * walks, the repair a writable mount runs makes it whole, and /kept is
 * there. */
static int volume_cut_is_sound(uint8_t *state, uint32_t size)
{
    krffs_mount_t mount = mount_over(&memory_backend, state, size);
    uint64_t kept;
    char byte;

    mount.grain = 4096;
    mount.sector = 512;
    if (!forward_chain_closes(&mount) ||
        krffs_repair_at(&mount, (uint32_t *)0, (uint32_t *)0, (uint32_t *)0) != 0 ||
        !krffs_chain_is_valid(&mount)) {
        return 0;
    }
    kept = krffs_walk(&mount, "/kept");
    return krffs_read_at(&mount, kept, &byte, 1, 0) == 1 && byte == 'k';
}

/**
 * Judge every state a stop can leave in the recorded writes, cut into units
 * of `unit` bytes: what was written before the last sync or flush is on the
 * medium, and each unit a later write touched holds what it held then or
 * what the writes so far made of it, in every mixture. Returns 1 when
 * `judge` accepts them all.
 */
static int every_unit_cut_is_sound(const uint8_t *before, uint32_t size, uint32_t unit,
                                   int (*judge)(uint8_t *, uint32_t))
{
    static uint8_t epoch[DISK_IMAGE_SIZE];
    static uint8_t after[DISK_IMAGE_SIZE];
    static uint8_t state[DISK_IMAGE_SIZE];
    int prefix;

    for (prefix = 0; prefix <= recorded_count; prefix++) {
        uint64_t units[10];
        int mark = epoch_of(prefix);
        int count = touched_blocks(mark, prefix, unit, units, 10);
        unsigned variant;
        int i;

        if (count == 10) {
            printf("krffs_hosttest: %d writes since a sync touch too many units\n", prefix - mark);
            return 0;
        }
        build(epoch, before, mark, size);
        build(after, before, prefix, size);
        for (variant = 0; variant < (1u << count); variant++) {
            k_memcpy(state, after, size);
            for (i = 0; i < count; i++) {
                if (variant & (1u << i)) {
                    uint32_t at = (uint32_t)units[i] * unit;

                    k_memcpy(state + at, epoch + at, unit);
                }
            }
            if (!judge(state, size)) {
                printf("krffs_hosttest: cut after %d of %d writes, unit %u, variant %u\n", prefix,
                       recorded_count, unit, variant);
                return 0;
            }
        }
    }
    return 1;
}

/** Fill the file at `at` of the disk fixture with `length` bytes of `byte`. */
static void fill_fixture_file(uint32_t at, uint32_t length, uint8_t byte)
{
    krffs_node_t *head = (krffs_node_t *)(disk_image + at);

    head->data_size = length;
    k_memset((uint8_t *)head + sizeof(*head), byte, length);
}

/**
 * Fill every byte of the disk fixture that no length counts, the free
 * nodes' and the slack after each file's data, with 's', so that a cut
 * that counts a byte nobody wrote shows it.
 */
static void stain_slack(const struct unlink_extent *extents, unsigned count)
{
    unsigned i;

    for (i = 1; i < count; i++) {
        uint32_t start = extents[i].start;
        uint32_t end = i + 1 < count ? extents[i + 1].start : DISK_IMAGE_SIZE;
        krffs_node_t *head = (krffs_node_t *)(disk_image + start);
        uint32_t from = start + (uint32_t)sizeof(*head);

        if (head->type == KRFFS_RESERVED_NODE) {
            from += (uint32_t)head->data_size;
        }
        k_memset(disk_image + from, 's', end - from);
    }
}

/* Set to have `disk_truncate_case` grow the file by writing 'w' bytes past
   its end instead of truncating it. */
static int case_writes;

/**
 * Truncate the disk root's file at `extents[1]` from `old` bytes to
 * `length`, or with `case_writes` write 'w' bytes from `old` to `length`,
 * recording its writes with the root's flush, and judge every cut at
 * sectors and at pages. The file must end at `at`, or anywhere else
 * for KRFFS_NO_NODE, in a node of `size` bytes, or any for 0. Returns the
 * truncate's answer.
 */
static int disk_truncate_case(const struct unlink_extent *extents, unsigned count, uint32_t old,
                              uint32_t length, uint64_t at, uint64_t size, const char *what)
{
    static const krffs_backend_t backend = {memory_read, record_write, memory_sync, 0};
    static uint8_t before[DISK_IMAGE_SIZE];
    static char path[64];
    uint64_t node = KRFFS_NO_NODE;
    krffs_node_t head;
    int rc;
    static char bytes[512];

    make_unlink_image(extents, count);
    fill_fixture_file(extents[1].start, old, 'v');
    stain_slack(extents, count);
    k_memcpy(before, disk_image, DISK_IMAGE_SIZE);
    if (init_krffs_disk_root(disk_image, DISK_IMAGE_SIZE, 512, record_disk_flush, NULL) != 0) {
        check(0, what);
        return KRFFS_EBAD;
    }
    krffs_root_mount.backend = &backend;
    recorded_count = 0;
    sync_count = 0;
    recording = 1;
    if (case_writes && length > old && length - old <= sizeof(bytes)) {
        k_memset(bytes, 'w', sizeof(bytes));
        node = extents[1].start;
        rc = krffs_write_at(&krffs_root_mount, &node, bytes, length - old, old);
        rc = rc == (int)(length - old) ? 0 : rc;
    } else {
        rc = krffs_truncate_node_at(&krffs_root_mount, extents[1].start, length, &node);
    }
    recording = 0;
    if (rc != 0) {
        return rc;
    }
    check(krffs_chain_is_valid(&krffs_root_mount) &&
              krffs_head(&krffs_root_mount, node, &head) == 0 && head.data_size == length &&
              (at == KRFFS_NO_NODE ? node != extents[1].start : node == at) &&
              (size == 0 || head.size == size),
          what);
    path[0] = '/';
    k_memcpy(path + 1, extents[1].name, k_strlen(extents[1].name) + 1);
    cut_name = path;
    cut_low = old < length ? old : length;
    cut_high = old < length ? length : old;
    cut_kept = cut_low;
    cut_byte = 'v';
    cut_grown = old < length ? (case_writes ? 'w' : 0) : 'v';
    check(every_unit_cut_is_sound(before, DISK_IMAGE_SIZE, 512, disk_cut_is_sound),
          "every sector cut of a disk-root truncate mounts and keeps the file");
    check(every_unit_cut_is_sound(before, DISK_IMAGE_SIZE, 4096, disk_cut_is_sound),
          "every page cut of a disk-root truncate mounts and keeps the file");
    return 0;
}

/** Whether a flush was recorded after the last write at `offset`. */
static int flushed_after(uint64_t offset)
{
    int i;
    int k;

    for (i = recorded_count - 1; i >= 0 && recorded[i].offset != offset; i--) {
    }
    for (k = 0; i >= 0 && k < sync_count; k++) {
        if (sync_marks[k] > i) {
            return 1;
        }
    }
    return 0;
}

/**
 * A grow of a disk-root file writes the free header, flushes, and only then
 * stores the size that takes the bytes before it, for a growth shorter than
 * a header, a long one, one that takes the whole free node, and one whose
 * size would cross a sector and moves instead.
 */
static void disk_grow_rows(void)
{
    static const struct unlink_extent split[] = {
        {0, NULL}, {1024, "victim"}, {2048, NULL}, {9216, "kept"}};
    static const struct unlink_extent whole[] = {
        {0, NULL}, {1024, "victim"}, {2048, NULL}, {2600, "kept"}, {4000, NULL}};
    static const struct unlink_extent crossing[] = {
        {0, NULL}, {1210, "victim"}, {2050, NULL}, {3050, "kept"}, {4000, NULL}};
    check(disk_truncate_case(split, 4, 667, 668, 1024, 1024 + sizeof(krffs_node_t),
                             "a one-byte grow takes a header's room, kept as room") == 0,
          "a one-byte grow succeeds");
    check(disk_truncate_case(split, 4, 667, 3667, 1024, sizeof(krffs_node_t) + 3667,
                             "a long grow splits the free node and takes what it asked") == 0,
          "a long grow succeeds");
    check(disk_truncate_case(whole, 5, 667, 668, 1024, 1576,
                             "a grow takes a free node too small to split whole") == 0,
          "a grow into a small free node succeeds");
    check(disk_truncate_case(crossing, 5, 483, 484, KRFFS_NO_NODE, 0,
                             "a grow whose size would cross a sector moves the file") == 0,
          "a grow that must move succeeds");
    case_writes = 1;
    check(disk_truncate_case(split, 4, 667, 668, 1024, 1024 + sizeof(krffs_node_t),
                             "a one-byte write past the end takes a header's room") == 0,
          "a one-byte write past the end succeeds");
    check(disk_truncate_case(split, 4, 667, 1100, 1024, 0,
                             "a write past the end splits the free node") == 0,
          "a write past the end succeeds");
    check(disk_truncate_case(crossing, 5, 483, 484, KRFFS_NO_NODE, 0,
                             "a write whose grow would cross a sector moves the file") == 0,
          "a write that must move succeeds");
    check(flushed_after(1210 + KRFFS_FIELD_TYPE),
          "and the old node's free type is flushed before the move returns");
    case_writes = 0;
}

/**
 * Unlink /victim of the disk fixture `extents` and then, with the merge
 * still only in the page cache, allocate `size` bytes, or grow /grower to
 * `size` bytes when `grow` is set, so that the free header either writes
 * falls over the header of the free node the merge took in; and judge every
 * sector cut.
 */
static void disk_after_unlink_case(const struct unlink_extent *extents, unsigned count,
                                   uint32_t size, int grow, const char *what)
{
    static const krffs_backend_t backend = {memory_read, record_write, memory_sync, 0};
    static uint8_t before[DISK_IMAGE_SIZE];
    int result;

    make_unlink_image(extents, count);
    k_memcpy(before, disk_image, DISK_IMAGE_SIZE);
    if (init_krffs_disk_root(disk_image, DISK_IMAGE_SIZE, 512, record_disk_flush, NULL) != 0) {
        check(0, what);
        return;
    }
    krffs_root_mount.backend = &backend;
    recorded_count = 0;
    sync_count = 0;
    recording = 1;
    result = krffs_unlink_at(&krffs_root_mount, "/victim");
    if (result == 0 && grow) {
        result = krffs_truncate_at(&krffs_root_mount, "/grower", size, (uint64_t *)0);
    } else if (result == 0) {
        result = krffs_allocate_at(&krffs_root_mount, size, KRFFS_NO_NODE) == 1024 ? 0 : -1;
    }
    recording = 0;
    check(result == 0 && krffs_chain_is_valid(&krffs_root_mount), what);
    cut_name = "/kept";
    cut_low = 1;
    cut_high = 1;
    cut_kept = 1;
    cut_byte = 'k';
    cut_grown = 'k';
    check(every_unit_cut_is_sound(before, DISK_IMAGE_SIZE, 512, disk_cut_is_sound),
          "every sector cut after an unlink whose merge was pending mounts");
}

/**
 * An allocation and a grow put the disk root's pending writes on the disk
 * before they write a free header inside free space an unlink just merged.
 */
static void disk_after_unlink_rows(void)
{
    static const struct unlink_extent allocate[] = {
        {0, NULL}, {1024, "victim"}, {2048, NULL}, {4096, "kept"}, {5000, NULL}};
    static const struct unlink_extent grow[] = {{0, NULL},    {1024, "grower"}, {2048, "victim"},
                                                {3000, NULL}, {5000, "kept"},   {6000, NULL}};

    disk_after_unlink_case(allocate, 5, 767, 0, "an allocation over a pending merge succeeds");
    disk_after_unlink_case(grow, 6, 1719, 1, "a grow over a pending merge succeeds");
}

/**
 * A length that crosses a sector, shorter or longer, goes in steps that
 * each leave a length between the old and the new, and when no such steps
 * exist the file moves. A move with no room answers KRFFS_ENOSPACE and keeps
 * the file.
 */
static void disk_split_length_rows(void)
{
    static const struct unlink_extent short_node[] = {{0, NULL}, {3754, "victim"}, {4879, "kept"}};
    static const struct unlink_extent long_node[] = {
        {0, NULL}, {3754, "victim"}, {5200, "kept"}, {6000, NULL}};
    static const struct unlink_extent full[] = {{0, NULL}, {3754, "victim"}, {5200, "kept"}};
    static const krffs_backend_t backend = {memory_read, record_write, memory_sync, 0};
    krffs_node_t *victim;
    int failure;

    check(3754 + KRFFS_FIELD_DATA_SIZE == 4095, "the fixture's length crosses a sector");
    make_unlink_image(short_node, 3);
    fill_fixture_file(3754, 768, 'v');
    victim = (krffs_node_t *)(disk_image + 3754);
    if (init_krffs_disk_root(disk_image, DISK_IMAGE_SIZE, 512, record_disk_flush, NULL) != 0) {
        check(0, "split length fixture mounts");
        return;
    }
    krffs_root_mount.backend = &backend;
    writes_seen = 0;
    refused_sync = 0;
    failure = krffs_truncate_node_at(&krffs_root_mount, 3754, 257, NULL);
    refused_sync = -1;
    check(failure == KRFFS_EIO && victim->data_size == 768, "shrink reports failed disk flush");

    check(disk_truncate_case(short_node, 3, 768, 257, 3754, 0,
                             "768 to 257 steps through 512 and 513") == 0 &&
              recorded_count >= 3 && recorded[0].offset == 4096 && recorded[0].bytes[0] == 2 &&
              recorded[1].offset == 4095 && recorded[1].bytes[0] == 1 &&
              recorded[2].offset == 4096 && recorded[2].bytes[0] == 1,
          "the high part steps to one above, then the low part, then the high part");
    check(disk_truncate_case(long_node, 4, 1023, 257, 3754, 0,
                             "1023 to 257 stores the high part first") == 0 &&
              recorded_count >= 2 && recorded[0].offset == 4096 && recorded[1].offset == 4095,
          "high part, then low part");
    check(disk_truncate_case(long_node, 4, 512, 511, KRFFS_NO_NODE, 0,
                             "512 to 511 has no safe steps and moves the file") == 0,
          "the shrink that moves succeeds");
    check(disk_truncate_case(long_node, 4, 257, 767, 3754, 0,
                             "257 to 767 stores the high part first") == 0,
          "a growth whose order is direct succeeds");
    check(disk_truncate_case(long_node, 4, 255, 600, 3754, 0,
                             "255 to 600 steps through 511 and 344") == 0 &&
              recorded_count >= 3,
          "a growth that steps succeeds");
    check(disk_truncate_case(long_node, 4, 255, 300, KRFFS_NO_NODE, 0,
                             "255 to 300 has no safe steps and moves the file") == 0,
          "a growth that moves succeeds");
    case_writes = 1;
    check(disk_truncate_case(long_node, 4, 257, 767, 3754, 0,
                             "a write from 257 to 767 stores the high part first") == 0,
          "a write whose length steps succeeds");
    check(disk_truncate_case(long_node, 4, 255, 300, KRFFS_NO_NODE, 0,
                             "a write from 255 to 300 moves the file with its bytes") == 0,
          "a write that moves for its length succeeds");
    case_writes = 0;

    make_unlink_image(full, 3);
    fill_fixture_file(3754, 512, 'v');
    victim = (krffs_node_t *)(disk_image + 3754);
    if (init_krffs_disk_root(disk_image, DISK_IMAGE_SIZE, 512, NULL, NULL) != 0) {
        check(0, "full fixture mounts");
        return;
    }
    check(krffs_truncate_node_at(&krffs_root_mount, 3754, 511, NULL) == KRFFS_ENOSPACE &&
              victim->data_size == 512 && krffs_chain_is_valid(&krffs_root_mount),
          "a shrink that must move and has no room answers ENOSPACE and keeps the file");
}

/**
 * On a volume a merge is held to the sector, not the block: a size whose
 * low byte ends one sector and whose next byte starts another, inside one
 * block, keeps the free nodes apart, and every sector cut walks.
 */
static void volume_merge_row(void)
{
    static const struct unlink_extent extents[] = {
        {0, NULL}, {698, "victim"}, {1723, NULL}, {2234, "kept"}, {3000, NULL}};
    static uint8_t before[DISK_IMAGE_SIZE];
    krffs_mount_t mount;
    krffs_node_t head;
    int result;

    make_unlink_image(extents, 5);
    k_memcpy(before, disk_image, DISK_IMAGE_SIZE);
    mount = mount_over(&recording_backend, disk_image, DISK_IMAGE_SIZE);
    mount.grain = 4096;
    mount.sector = 512;
    check(698 + KRFFS_FIELD_SIZE == 1023, "the victim's size ends a sector");
    recorded_count = 0;
    sync_count = 0;
    recording = 1;
    result = krffs_unlink_at(&mount, "/victim");
    recording = 0;
    check(result == 0 && krffs_head(&mount, 698, &head) == 0 && head.type == KRFFS_FREE_NODE &&
              head.size == 0x401 && krffs_chain_is_valid(&mount),
          "a merge whose size crosses a sector inside a block is not made");
    check(every_unit_cut_is_sound(before, DISK_IMAGE_SIZE, 512, volume_cut_is_sound),
          "every sector cut of the volume's unlink walks");
}

/** Fail the sync of `sync_fails_backend` while this is set. */
static int sync_fails;

/** A sync that fails while `sync_fails` is set. */
static int failing_sync(void *ctx)
{
    (void)ctx;
    return sync_fails ? -1 : 0;
}

static const krffs_backend_t sync_fails_backend = {memory_read, record_write, failing_sync, 0};

/* The recording backend with a unit of 512 bytes, as a block cache's. */
static const krffs_backend_t unit_backend = {memory_read, record_write, memory_sync, 512};

/**
 * A write in place goes in the backend's units, so one whose second unit
 * fails answers the bytes of the first, as Linux's short write does, and
 * leaves the rest of the file as it was; one whose first unit fails
 * answers EIO and changes nothing.
 */
static void short_write_rows(void)
{
    krffs_mount_t mount;
    uint64_t node;
    uint64_t start;
    uint64_t edge;
    char bytes[2000];
    char back[2000];
    int n;

    make_image(image, IMAGE_SIZE);
    mount = mount_over(&unit_backend, image, IMAGE_SIZE);
    k_memset(bytes, 'a', sizeof(bytes));
    check(krffs_create_at(&mount, "/s") == 0, "short write rows: a file");
    node = krffs_walk(&mount, "/s");
    check(krffs_write_at(&mount, &node, bytes, sizeof(bytes), 0) == (int)sizeof(bytes),
          "short write rows: its bytes");
    start = node + sizeof(krffs_node_t);
    edge = (start / 512 + 1) * 512;
    k_memset(bytes, 'b', sizeof(bytes));
    refused_offset = edge;
    n = krffs_write_at(&mount, &node, bytes, sizeof(bytes), 0);
    refused_offset = (uint64_t)-1;
    check(n == (int)(edge - start), "a write whose second unit fails answers the first's bytes");
    check(krffs_read_at(&mount, node, back, sizeof(back), 0) == (int)sizeof(back) &&
              back[0] == 'b' && back[edge - start - 1] == 'b' && back[edge - start] == 'a' &&
              back[sizeof(back) - 1] == 'a',
          "and the file holds those bytes and the old ones after them");
    refused_offset = start;
    check(krffs_write_at(&mount, &node, "zz", 2, 0) == KRFFS_EIO,
          "a write whose first unit fails answers EIO");
    refused_offset = (uint64_t)-1;
    check(krffs_read_at(&mount, node, back, 2, 0) == 2 && back[0] == 'b' && back[1] == 'b',
          "and changes nothing");
}

/**
 * A failed write of the size that publishes a shrink or a merge reports the
 * failure and marks the links stale, and a grow whose free header or its
 * sync failed reports it and leaves the chain as it was.
 */
static void writeback_error_rows(void)
{
    krffs_mount_t mount;
    krffs_node_t head;
    uint64_t node;
    uint64_t room;
    char bytes[2000];

    k_memset(bytes, 'w', sizeof(bytes));
    make_image(image, IMAGE_SIZE);
    mount = mount_over(&recording_backend, image, IMAGE_SIZE);
    check(krffs_create_at(&mount, "/shrunk") == 0, "error rows: a file");
    node = krffs_walk(&mount, "/shrunk");
    check(krffs_write_at(&mount, &node, bytes, sizeof(bytes), 0) == (int)sizeof(bytes),
          "error rows: its bytes");
    refused_offset = node + KRFFS_FIELD_SIZE;
    check(krffs_truncate_node_at(&mount, node, 100, NULL) == KRFFS_EIO && mount.stale_links,
          "a shrink whose size store fails answers EIO and marks the links stale");
    refused_offset = (uint64_t)-1;
    check(forward_chain_closes(&mount), "and its chain walks");

    make_image(image, IMAGE_SIZE);
    mount = mount_over(&recording_backend, image, IMAGE_SIZE);
    check(krffs_create_at(&mount, "/grown") == 0, "error rows: a file to grow");
    node = krffs_walk(&mount, "/grown");
    check(krffs_write_at(&mount, &node, bytes, 100, 0) == 100 &&
              krffs_head(&mount, node, &head) == 0,
          "error rows: its first bytes");
    room = head.size;
    refused_offset = node + sizeof(krffs_node_t) + 2000;
    check(krffs_truncate_node_at(&mount, node, 2000, NULL) == KRFFS_EIO,
          "a grow whose free header fails answers EIO");
    refused_offset = (uint64_t)-1;
    check(krffs_head(&mount, node, &head) == 0 && head.data_size == 100 && head.size == room &&
              krffs_chain_is_valid(&mount),
          "and leaves the file and the chain as they were");
    mount.backend = &sync_fails_backend;
    sync_fails = 1;
    check(krffs_truncate_node_at(&mount, node, 2000, NULL) == KRFFS_EIO,
          "a grow whose header sync fails answers EIO");
    sync_fails = 0;
    check(krffs_head(&mount, node, &head) == 0 && head.size == room && forward_chain_closes(&mount),
          "and publishes nothing");
    refused_offset = node + sizeof(krffs_node_t) + 2000;
    check(krffs_write_at(&mount, &node, bytes, 2000, 0) == KRFFS_EIO,
          "a write whose grow's free header fails answers EIO, not ENOSPACE");
    refused_offset = (uint64_t)-1;
    check(krffs_head(&mount, node, &head) == 0 && head.data_size == 100 && head.size == room &&
              krffs_chain_is_valid(&mount),
          "and leaves the file and the chain as they were");

    make_image(image, IMAGE_SIZE);
    mount = mount_over(&recording_backend, image, IMAGE_SIZE);
    check(krffs_create_at(&mount, "/merged") == 0 && krffs_create_at(&mount, "/after") == 0,
          "error rows: a file with a neighbour");
    node = krffs_walk(&mount, "/after");
    check(krffs_unlink_at(&mount, "/after") == 0, "error rows: the neighbour freed");
    node = krffs_walk(&mount, "/merged");
    refused_offset = node + KRFFS_FIELD_SIZE;
    check(krffs_unlink_at(&mount, "/merged") != 0 && mount.stale_links,
          "a removal whose merge size store fails reports it and marks the links stale");
    refused_offset = (uint64_t)-1;
    check(forward_chain_closes(&mount), "and its chain walks");
}

/**
 * A write that must move its file for the length's sake decides so before
 * it writes: with no room for the copy it answers KRFFS_ENOSPACE and the
 * byte it would have overwritten is as it was, and with room the copy has
 * the old bytes and the new.
 */
static void disk_moving_write_rows(void)
{
    static const struct unlink_extent full[] = {{0, NULL}, {3754, "victim"}, {5200, "kept"}};
    static const struct unlink_extent room[] = {
        {0, NULL}, {3754, "victim"}, {5200, "kept"}, {6000, NULL}};
    krffs_node_t head;
    uint64_t node = 3754;
    char byte;
    uint32_t at;
    int whole = 1;

    make_unlink_image(full, 3);
    fill_fixture_file(3754, 255, 'v');
    if (init_krffs_disk_root(disk_image, DISK_IMAGE_SIZE, 512, NULL, NULL) != 0) {
        check(0, "moving write fixture mounts");
        return;
    }
    check(krffs_write_at(&krffs_root_mount, &node, "ab", 2, 254) == KRFFS_ENOSPACE &&
              node == 3754 && disk_image[3754 + sizeof(krffs_node_t) + 254] == 'v' &&
              krffs_head(&krffs_root_mount, 3754, &head) == 0 && head.data_size == 255,
          "a write that must move and has no room answers ENOSPACE and changes no byte");

    make_unlink_image(room, 4);
    fill_fixture_file(3754, 255, 'v');
    if (init_krffs_disk_root(disk_image, DISK_IMAGE_SIZE, 512, NULL, NULL) != 0) {
        check(0, "moving write fixture with room mounts");
        return;
    }
    node = 3754;
    check(krffs_write_at(&krffs_root_mount, &node, "ab", 2, 254) == 2 && node != 3754 &&
              krffs_walk(&krffs_root_mount, "/victim") == node &&
              krffs_head(&krffs_root_mount, node, &head) == 0 && head.data_size == 256 &&
              krffs_chain_is_valid(&krffs_root_mount),
          "a write that must move puts its bytes in the copy");
    for (at = 0; at < 256; at++) {
        if (krffs_read_at(&krffs_root_mount, node, &byte, 1, at) != 1 ||
            byte != (at < 254    ? 'v'
                     : at == 254 ? 'a'
                                 : 'b')) {
            whole = 0;
        }
    }
    check(whole, "and the copy has the old bytes and the new");
}

/**
 * On a volume a shrink's last size is still in the cache when a grow of the
 * same file writes a free header one byte past the old free node's own: the
 * grow's barrier puts the size on the medium first, so every sector cut of
 * the two walks.
 */
static void volume_shrink_grow_row(void)
{
    static const struct unlink_extent extents[] = {
        {0, NULL}, {1024, "victim"}, {4096, NULL}, {6000, "kept"}, {7000, NULL}};
    static uint8_t before[DISK_IMAGE_SIZE];
    krffs_mount_t mount;
    krffs_node_t head;
    uint32_t grown = 4097 - 1024;
    int result;

    make_unlink_image(extents, 5);
    fill_fixture_file(1024, 4096 - 1024 - sizeof(krffs_node_t), 'v');
    k_memcpy(before, disk_image, DISK_IMAGE_SIZE);
    mount = mount_over(&recording_backend, disk_image, DISK_IMAGE_SIZE);
    mount.grain = 4096;
    mount.sector = 512;
    recorded_count = 0;
    sync_count = 0;
    recording = 1;
    result = krffs_truncate_node_at(&mount, 1024, 1000, NULL);
    if (result == 0) {
        result = krffs_truncate_node_at(&mount, 1024, grown - sizeof(krffs_node_t), NULL);
    }
    recording = 0;
    check(result == 0 && krffs_head(&mount, 1024, &head) == 0 && head.size == grown &&
              krffs_chain_is_valid(&mount),
          "a shrink and a grow put the grow's free header one byte past the old one");
    check(every_unit_cut_is_sound(before, DISK_IMAGE_SIZE, 512, volume_cut_is_sound),
          "every sector cut of a volume's shrink and grow walks");
}

/** Whether the `size` bytes at `a` and `b` are the same. */
static int same_bytes(const uint8_t *a, const uint8_t *b, uint32_t size)
{
    uint32_t i;

    for (i = 0; i < size; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

/* Flushes of the disk root that succeed before every later one fails, or -1
   for no limit, and the flushes seen. */
static int flush_budget = -1;
static int flushes_seen;

/** A disk-root flush that fails once `flush_budget` is spent. */
static int budget_flush(void)
{
    flushes_seen++;
    if (flush_budget == 0) {
        return -1;
    }
    if (flush_budget > 0) {
        flush_budget--;
    }
    return 0;
}

/**
 * A disk-root grow whose last flush fails answers the failure and leaves
 * the room it took only in the cache: a retry of a truncate or a write into
 * that room changes no byte while flushes fail, and succeeds once one does.
 */
static void failed_grow_barrier_row(void)
{
    static const struct unlink_extent split[] = {
        {0, NULL}, {1024, "victim"}, {2048, NULL}, {9216, "kept"}};
    static uint8_t held[DISK_IMAGE_SIZE];
    krffs_node_t head;
    uint64_t node = 1024;
    int flushes;

    make_unlink_image(split, 4);
    fill_fixture_file(1024, 667, 'v');
    k_memcpy(held, disk_image, DISK_IMAGE_SIZE);
    if (init_krffs_disk_root(disk_image, DISK_IMAGE_SIZE, 512, budget_flush, NULL) != 0) {
        check(0, "failed grow fixture mounts");
        return;
    }
    flush_budget = -1;
    flushes_seen = 0;
    check(krffs_grow_in_place(&krffs_root_mount, 1024, 668) == 0, "failed grow: a grow");
    flushes = flushes_seen;

    k_memcpy(disk_image, held, DISK_IMAGE_SIZE);
    if (init_krffs_disk_root(disk_image, DISK_IMAGE_SIZE, 512, budget_flush, NULL) != 0) {
        check(0, "failed grow fixture mounts again");
        return;
    }
    flush_budget = flushes - 1;
    check(krffs_grow_in_place(&krffs_root_mount, 1024, 668) < 0 && krffs_root_mount.unsettled &&
              krffs_head(&krffs_root_mount, 1024, &head) == 0 &&
              head.size > sizeof(krffs_node_t) + 667,
          "a grow whose last flush fails answers the failure with the room taken");
    k_memcpy(held, disk_image, DISK_IMAGE_SIZE);
    check(krffs_truncate_node_at(&krffs_root_mount, 1024, 668, NULL) == KRFFS_EIO &&
              krffs_write_at(&krffs_root_mount, &node, "x", 1, 667) == KRFFS_EIO &&
              same_bytes(held, disk_image, DISK_IMAGE_SIZE),
          "while flushes fail, a retry writes nothing into the room");
    flush_budget = -1;
    check(krffs_write_at(&krffs_root_mount, &node, "x", 1, 667) == 1 && node == 1024 &&
              !krffs_root_mount.unsettled && krffs_head(&krffs_root_mount, 1024, &head) == 0 &&
              head.data_size == 668 && krffs_chain_is_valid(&krffs_root_mount),
          "once a flush succeeds, the retry writes");
}

/* Set to have `split_write` put only the part of a write before a 4 KiB
   boundary and fail. */
static int split_fails;

/** Write through to the image; with `split_fails`, a write that crosses a
 * 4 KiB boundary puts the part before it and fails, as a cache whose second
 * block will not write does. */
static int split_write(void *ctx, uint64_t offset, const void *buffer, uint32_t size)
{
    uint64_t edge = (offset / 4096 + 1) * 4096;

    if (split_fails && offset + size > edge) {
        memory_write(ctx, offset, buffer, (uint32_t)(edge - offset));
        return -1;
    }
    return memory_write(ctx, offset, buffer, size);
}

static const krffs_backend_t split_backend = {memory_read, split_write, memory_sync, 0};

/**
 * A shrink whose size store reaches the medium in its first block, which
 * holds every byte it changes, and fails in its second answers EIO with the
 * links marked stale; the forward chain already has the new size, and the
 * repair makes the ring whole.
 */
static void partial_size_row(void)
{
    static const struct unlink_extent extents[] = {
        {0, NULL}, {3768, "victim"}, {6768, "kept"}, {7768, NULL}};
    krffs_mount_t mount;
    krffs_node_t head;
    uint32_t length = 100;

    check(3768 + KRFFS_FIELD_SIZE == 4093, "the victim's size crosses a block");
    make_unlink_image(extents, 4);
    fill_fixture_file(3768, 3000 - sizeof(krffs_node_t), 'v');
    mount = mount_over(&split_backend, disk_image, DISK_IMAGE_SIZE);
    mount.grain = 4096;
    mount.sector = 512;
    split_fails = 1;
    check(krffs_truncate_node_at(&mount, 3768, length, NULL) == KRFFS_EIO && mount.stale_links &&
              mount.unsettled,
          "a size store that fails in its second block answers EIO and marks the links stale "
          "and the mount unsettled");
    split_fails = 0;
    check(krffs_head(&mount, 3768, &head) == 0 && head.size == sizeof(krffs_node_t) + length &&
              forward_chain_closes(&mount) &&
              krffs_repair_at(&mount, (uint32_t *)0, (uint32_t *)0, (uint32_t *)0) == 0 &&
              krffs_chain_is_valid(&mount),
          "and the forward chain has the new size, which the repair makes whole");
}

/* How many times a mount told the row it halted. */
static int halts_seen;

/** Count a halt (`krffs_mount_t`'s `on_halt`). */
static void count_halt(krffs_mount_t *mount)
{
    (void)mount;
    halts_seen++;
}

/**
 * A write that moves its file, with each of its writes and syncs refused in
 * turn, whatever it answers leaves the file's name on one node and `*node`
 * on that node, so that the running kernel and the caller's holders agree.
 */
static void moving_write_refusals(void)
{
    static uint8_t before[IMAGE_SIZE];
    static char bytes[2000];
    krffs_mount_t mount;
    krffs_node_t head;
    uint64_t node;
    uint64_t old;
    uint64_t copy;
    int steps;
    int i;
    int sound = 1;

    k_memset(bytes, 'm', sizeof(bytes));
    make_image(image, IMAGE_SIZE);
    mount = mount_over(&recording_backend, image, IMAGE_SIZE);
    check(krffs_create_at(&mount, "/moved") == 0, "moving refusals: a file");
    node = krffs_walk(&mount, "/moved");
    check(krffs_write_at(&mount, &node, bytes, 100, 0) == 100 &&
              krffs_create_at(&mount, "/wall") == 0,
          "moving refusals: its bytes and a node after it");
    k_memcpy(before, image, IMAGE_SIZE);
    writes_seen = 0;
    old = krffs_walk(&mount, "/moved");
    node = old;
    check(krffs_write_at(&mount, &node, bytes, sizeof(bytes), 0) == (int)sizeof(bytes) &&
              node != old && krffs_walk(&mount, "/moved") == node,
          "moving refusals: the write moves the file");
    copy = node;
    steps = writes_seen;
    for (i = 0; i < 2 * steps; i++) {
        k_memcpy(image, before, IMAGE_SIZE);
        mount = mount_over(&recording_backend, image, IMAGE_SIZE);
        node = krffs_walk(&mount, "/moved");
        writes_seen = 0;
        if (i < steps) {
            refused_write = i;
        } else {
            refused_sync = i - steps;
        }
        krffs_write_at(&mount, &node, bytes, sizeof(bytes), 0);
        refused_write = -1;
        refused_sync = -1;
        if (!no_name_is_on_two_nodes(&mount) || krffs_head(&mount, node, &head) != 0 ||
            head.type != KRFFS_RESERVED_NODE || !k_strcmp_name(head.name, "moved")) {
            printf("krffs_hosttest: moving write refused at step %d\n", i);
            sound = 0;
        }
    }
    check(sound, "every refused step of a moving write leaves one node named and held");

    k_memcpy(image, before, IMAGE_SIZE);
    mount = mount_over(&recording_backend, image, IMAGE_SIZE);
    node = old;
    mount.on_halt = count_halt;
    halts_seen = 0;
    refused_offset = old + KRFFS_FIELD_TYPE;
    refused_again_offset = copy + KRFFS_FIELD_TYPE;
    refused_again_seen = 0;
    check(krffs_write_at(&mount, &node, bytes, sizeof(bytes), 0) == KRFFS_EIO && node == old &&
              mount.halted && mount.stale_links,
          "a move whose old node and copy both refuse their type halts the mount");
    check(halts_seen == 1, "and tells its owner once");
    refused_offset = (uint64_t)-1;
    refused_again_offset = (uint64_t)-1;
    {
        char first = 0;

        check(krffs_write_at(&mount, &node, "zz", 2, 0) == KRFFS_EIO &&
                  krffs_read_at(&mount, node, &first, 1, 0) == 1 && first == 'm',
              "a halted mount refuses a write inside the file's length");
    }
    check(krffs_write_at(&mount, &node, bytes, sizeof(bytes), 0) == KRFFS_EIO &&
              krffs_create_at(&mount, "/later") != 0,
          "and a halted mount writes no more");
    check(krffs_unlink_at(&mount, "/wall") != 0 && krffs_walk(&mount, "/wall") != KRFFS_NO_NODE &&
              krffs_rename_at(&mount, "/wall", "/fence") != 0 &&
              krffs_walk(&mount, "/fence") == KRFFS_NO_NODE,
          "and an unlink and a rename");
    mount.halted = 0;
    check(forward_chain_closes(&mount) &&
              krffs_repair_at(&mount, (uint32_t *)0, (uint32_t *)0, (uint32_t *)0) == 0 &&
              no_name_is_on_two_nodes(&mount) && krffs_chain_is_valid(&mount),
          "and the next mount's repair leaves the name on one node");
}

/**
 * A shrink that must move, whose old node refuses its type, withdraws the
 * copy by its type alone, unmerged, so that the running kernel finds the
 * file's name on the original only.
 */
static void withdrawn_shrink_row(void)
{
    static const struct unlink_extent extents[] = {
        {0, NULL}, {682, "victim"}, {1682, "kept"}, {2100, NULL}};
    krffs_mount_t mount;
    krffs_node_t head;
    krffs_node_t copy;

    check(682 + KRFFS_FIELD_DATA_SIZE == 1023, "the victim's length crosses a sector");
    make_unlink_image(extents, 4);
    fill_fixture_file(682, 256, 'v');
    mount = mount_over(&recording_backend, disk_image, DISK_IMAGE_SIZE);
    mount.sector = 512;
    refused_offset = 682 + KRFFS_FIELD_TYPE;
    check(krffs_truncate_node_at(&mount, 682, 255, NULL) == KRFFS_EIO,
          "a shrink that must move answers EIO when its old node refuses its type");
    refused_offset = (uint64_t)-1;
    check(krffs_head(&mount, 682, &head) == 0 && head.type == KRFFS_RESERVED_NODE &&
              head.data_size == 256 && krffs_head(&mount, 2100, &copy) == 0 &&
              copy.type == KRFFS_FREE_NODE && copy.size == sizeof(krffs_node_t) + 255 &&
              no_name_is_on_two_nodes(&mount) && krffs_chain_is_valid(&mount),
          "and the copy is withdrawn by its type, unmerged and smaller than the original");
}

/**
 * A write whose bytes readers can see once its call fails answers them: an
 * append whose length's high part is stored before its barrier fails
 * answers the bytes that length counts, and a moving write whose copy is
 * live before a barrier fails answers every byte; neither answers EIO.
 */
static void visible_write_rows(void)
{
    static const struct unlink_extent extents[] = {
        {0, NULL}, {682, "victim"}, {1682, "kept"}, {2100, NULL}};
    krffs_mount_t mount;
    uint64_t node = 682;
    char bytes[1000];
    char back[1000];
    int n;

    check(682 + KRFFS_FIELD_DATA_SIZE == 1023, "the victim's length crosses a sector");
    make_unlink_image(extents, 4);
    fill_fixture_file(682, 256, 'v');
    mount = mount_over(&recording_backend, disk_image, DISK_IMAGE_SIZE);
    mount.sector = 512;
    k_memset(bytes, 'n', sizeof(bytes));
    writes_seen = 0;
    refused_sync_after = 1024; /* the length's high part, in the next sector */
    n = krffs_write_at(&mount, &node, bytes, 256, 256);
    refused_sync = -1;
    refused_sync_after = (uint64_t)-1;
    check(n == 256 && node == 682 && mount.unsettled,
          "an append whose length's barrier fails answers the bytes the length counts");
    check(krffs_read_at(&mount, node, back, 512, 0) == 512 && back[255] == 'v' &&
              back[256] == 'n' && back[511] == 'n',
          "and readers see them");

    make_unlink_image(extents, 4);
    fill_fixture_file(682, 256, 'v');
    mount = mount_over(&recording_backend, disk_image, DISK_IMAGE_SIZE);
    mount.sector = 512;
    k_memset(bytes, 'm', sizeof(bytes));
    node = 682;
    writes_seen = 0;
    refused_sync_after = 682 + KRFFS_FIELD_TYPE; /* the old node freed */
    n = krffs_write_at(&mount, &node, bytes, sizeof(bytes), 0);
    refused_sync = -1;
    refused_sync_after = (uint64_t)-1;
    check(n == (int)sizeof(bytes) && node != 682,
          "a moving write whose barrier fails once its copy is live answers every byte");
    check(krffs_read_at(&mount, node, back, sizeof(back), 0) == (int)sizeof(back) &&
              back[0] == 'm' && back[sizeof(back) - 1] == 'm',
          "and readers see them in the copy");
}

/**
 * A shrink that must move takes a whole free node larger than its own
 * node. When the old node refuses its type and the copy refuses the type
 * that withdraws it, the copy stays published beside the original, and the
 * next mount's repair must keep the original, which has more data, since
 * the truncate answered EIO.
 */
static void withdrawn_larger_copy_row(void)
{
    static const struct unlink_extent extents[] = {
        {0, NULL}, {682, "victim"}, {1382, "kept"}, {1800, NULL}, {2700, "tail"}};
    krffs_mount_t mount;
    krffs_node_t head;
    uint64_t node;
    char byte = 0;

    make_unlink_image(extents, 5);
    fill_fixture_file(682, 256, 'v');
    mount = mount_over(&recording_backend, disk_image, DISK_IMAGE_SIZE);
    mount.sector = 512;
    refused_offset = 682 + KRFFS_FIELD_TYPE;
    refused_again_offset = 1800 + KRFFS_FIELD_TYPE;
    refused_again_seen = 0;
    check(krffs_truncate_node_at(&mount, 682, 255, NULL) == KRFFS_EIO && mount.halted,
          "a shrink whose old node and copy refuse their types answers EIO and halts");
    refused_offset = (uint64_t)-1;
    refused_again_offset = (uint64_t)-1;
    check(krffs_head(&mount, 1800, &head) == 0 && head.type == KRFFS_RESERVED_NODE &&
              head.size == 900 && head.data_size == 255,
          "and its copy, a whole free node larger than the original, stays published");
    {
        char name[64];
        uint32_t i;
        int listed = 0;

        for (i = 0; krffs_readdir_at(&mount, "/", i, name, sizeof(name), (int *)0) == 1; i++) {
            listed += k_strcmp_name((const uint8_t *)name, "victim");
        }
        check(krffs_hidden(&mount, 1800) && krffs_walk(&mount, "/victim") == 682 && listed == 1,
              "and the copy is hidden from lookups and listings while the mount is halted");
    }
    mount.halted = 0;
    mount.withdrawn = 0;
    check(forward_chain_closes(&mount) &&
              krffs_repair_at(&mount, (uint32_t *)0, (uint32_t *)0, (uint32_t *)0) == 0 &&
              no_name_is_on_two_nodes(&mount) && krffs_chain_is_valid(&mount),
          "and the repair leaves the name on one node");
    node = krffs_walk(&mount, "/victim");
    check(node == 682 && krffs_head(&mount, node, &head) == 0 && head.data_size == 256 &&
              krffs_read_at(&mount, node, &byte, 1, 255) == 1 && byte == 'v',
          "the original keeps the name, with the byte the failed truncate would drop");
}

/* The lengths `regrow_cut_is_sound` accepts: the file before its shrink,
   after it, and after the write that raised it again. */
static uint64_t regrow_old;
static uint64_t regrow_shrunk;
static uint64_t regrow_new;

/** A cut of the disk root of `disk_regrow_row`: it mounts, and /victim has
 * one of the three lengths with the bytes that length was stored over. */
static int regrow_cut_is_sound(uint8_t *state, uint32_t size)
{
    uint64_t node;
    krffs_node_t head;
    uint64_t at;
    uint8_t byte;

    if (init_krffs_disk_root(state, size, 512, NULL, NULL) != 0 ||
        !krffs_chain_is_valid(&krffs_root_mount)) {
        return 0;
    }
    node = krffs_walk(&krffs_root_mount, "/victim");
    if (krffs_head(&krffs_root_mount, node, &head) != 0 ||
        (head.data_size != regrow_old && head.data_size != regrow_shrunk &&
         head.data_size != regrow_new)) {
        return 0;
    }
    for (at = 0; at < head.data_size; at++) {
        uint8_t want = at < regrow_shrunk || head.data_size == regrow_old ? 'v' : 'w';

        if (krffs_read_at(&krffs_root_mount, node, (char *)&byte, 1, (uint32_t)at) != 1 ||
            byte != want) {
            return 0;
        }
    }
    return 1;
}

/**
 * A disk-root shrink that keeps its surplus as room, and then a write that
 * raises the length into that room again: the shorter length reaches the
 * disk before the new bytes, so that no cut, at sectors or at pages, shows
 * the old length counting them.
 */
static void disk_regrow_row(void)
{
    static const struct unlink_extent extents[] = {{0, NULL}, {1024, "victim"}, {4531, "kept"}};
    static const krffs_backend_t backend = {memory_read, record_write, memory_sync, 0};
    static uint8_t before[DISK_IMAGE_SIZE];
    static char bytes[50];
    uint64_t node = 1024;
    krffs_node_t head;
    int ok;

    make_unlink_image(extents, 3);
    fill_fixture_file(1024, 3100, 'v');
    stain_slack(extents, 3);
    k_memcpy(before, disk_image, DISK_IMAGE_SIZE);
    if (init_krffs_disk_root(disk_image, DISK_IMAGE_SIZE, 512, record_disk_flush, NULL) != 0) {
        check(0, "regrow fixture mounts");
        return;
    }
    krffs_root_mount.backend = &backend;
    k_memset(bytes, 'w', sizeof(bytes));
    recorded_count = 0;
    sync_count = 0;
    recording = 1;
    ok = krffs_truncate_node_at(&krffs_root_mount, 1024, 3000, NULL) == 0 &&
         krffs_write_at(&krffs_root_mount, &node, bytes, sizeof(bytes), 3000) == (int)sizeof(bytes);
    recording = 0;
    check(ok && node == 1024 && krffs_head(&krffs_root_mount, node, &head) == 0 &&
              head.size == 3507 && head.data_size == 3050,
          "a shrink kept as room and a write back into it stay in the file's node");
    regrow_old = 3100;
    regrow_shrunk = 3000;
    regrow_new = 3050;
    check(every_unit_cut_is_sound(before, DISK_IMAGE_SIZE, 512, regrow_cut_is_sound),
          "every sector cut of a shrink and a write back keeps the length over its bytes");
    check(every_unit_cut_is_sound(before, DISK_IMAGE_SIZE, 4096, regrow_cut_is_sound),
          "every page cut of a shrink and a write back keeps the length over its bytes");
}

/**
 * A shrink whose length store changes bytes of its first block only and
 * fails in its second may have left the shorter length in the cache, so the
 * mount is left unsettled, and the next write into the room puts a barrier
 * first.
 */
static void partial_length_row(void)
{
    static const struct unlink_extent extents[] = {
        {0, NULL}, {3751, "victim"}, {6751, "kept"}, {7751, NULL}};
    krffs_mount_t mount;

    check(3751 + KRFFS_FIELD_DATA_SIZE == 4092,
          "the victim's length crosses a block, not a sector");
    make_unlink_image(extents, 4);
    fill_fixture_file(3751, 300, 'v');
    mount = mount_over(&split_backend, disk_image, DISK_IMAGE_SIZE);
    mount.grain = 4096;
    mount.sector = 512;
    split_fails = 1;
    check(krffs_truncate_node_at(&mount, 3751, 200, NULL) == KRFFS_EIO && mount.unsettled,
          "a length store that fails in its second block leaves the mount unsettled");
    split_fails = 0;
}

/**
 * A dirty disk root with one name on two nodes, as a move stopped between
 * publishing its copy and freeing the original leaves, mounts with the name
 * on the node with more data.
 */
static void disk_double_name_row(void)
{
    static const struct unlink_extent extents[] = {
        {0, NULL}, {1024, "twin"}, {3000, "twin"}, {5000, "kept"}};
    krffs_node_t head;

    make_unlink_image(extents, 4);
    fill_fixture_file(3000, 10, 'a');
    fill_fixture_file(1024, 20, 'b');
    check(init_krffs_disk_root(disk_image, DISK_IMAGE_SIZE, 512, NULL, NULL) == 0,
          "a dirty disk root with one name on two nodes mounts");
    check(krffs_walk(&krffs_root_mount, "/twin") == 1024 &&
              krffs_head(&krffs_root_mount, 3000, &head) == 0 && head.name[0] == '\0',
          "and the node with more data keeps the name");
    make_unlink_image(extents, 4);
    fill_fixture_file(1024, 10, 'a');
    fill_fixture_file(3000, 20, 'b');
    check(init_krffs_disk_root(disk_image, DISK_IMAGE_SIZE, 512, NULL, NULL) == 0 &&
              krffs_walk(&krffs_root_mount, "/twin") == 3000 &&
              krffs_head(&krffs_root_mount, 1024, &head) == 0 && head.name[0] == '\0',
          "whichever comes first in the ring");
}

/** Whether `path` on `mount` is a file of `length` bytes of `byte`. */
static int mount_file_is(krffs_mount_t *mount, const char *path, uint32_t length, char byte)
{
    uint64_t node = krffs_walk(mount, path);
    krffs_node_t head;
    char got;
    uint32_t at;

    if (krffs_head(mount, node, &head) != 0 || head.data_size != length) {
        return 0;
    }
    for (at = 0; at < length; at++) {
        if (krffs_read_at(mount, node, &got, 1, at) != 1 || got != byte) {
            return 0;
        }
    }
    return 1;
}

/** Whether `path` on the boot root is a file of `length` bytes of `byte`. */
static int root_file_is(const char *path, uint32_t length, char byte)
{
    return mount_file_is(&krffs_root_mount, path, length, byte);
}

/** How many spare names of a rename in the directory `dir` ("/" or "/d")
 * hold the renamed file, ten bytes of 'm'. */
static int spares_holding(const char *dir)
{
    char name[64];
    char path[80];
    uint32_t i;
    int found = 0;

    for (i = 0; krffs_readdir_at(&krffs_root_mount, dir, i, name, sizeof(name), (int *)0) == 1;
         i++) {
        uint32_t n = 0;

        if (name[0] != '.' || name[1] != 'r' || name[2] != 'e' || name[3] != 'n') {
            continue;
        }
        if (dir[1] != '\0') {
            k_memcpy(path, dir, k_strlen(dir));
            n = k_strlen(dir);
        }
        path[n++] = '/';
        k_memcpy(path + n, name, k_strlen(name) + 1);
        found += root_file_is(path, 10, 'm');
    }
    return found;
}

/* Set when the rename rows put a file /y beside the source. */
static int rename_case_y;

/** A cut of a rename of /x to /d/y: it mounts, /d/x and any /y keep their
 * bytes, and the renamed file is under exactly one name. */
static int rename_cut_is_sound(uint8_t *state, uint32_t size)
{
    if (init_krffs_disk_root(state, size, 512, NULL, NULL) != 0 ||
        !krffs_chain_is_valid(&krffs_root_mount) || !root_file_is("/d/x", 1, 'o') ||
        (rename_case_y && !root_file_is("/y", 1, 'y'))) {
        return 0;
    }
    return root_file_is("/x", 10, 'm') + root_file_is("/y", 10, 'm') +
               root_file_is("/d/y", 10, 'm') + spares_holding("/") + spares_holding("/d") ==
           1;
}

/**
 * A rename of /x to /d/y beside a file /d/x, and with `with_y` a file /y
 * too, on the disk root: no cut between its writes, at sectors or pages,
 * puts two nodes under one name, which would have the repair clear the
 * name of /d/x or of the renamed file.
 */
static void disk_rename_case(int with_y, const char *what)
{
    static const struct unlink_extent extents[] = {
        {0, NULL}, {1024, "x"}, {2048, NULL}, {60000, "kept"}};
    static const krffs_backend_t backend = {memory_read, record_write, memory_sync, 0};
    static uint8_t before[DISK_IMAGE_SIZE];
    uint64_t node;
    int ok;

    make_unlink_image(extents, 4);
    fill_fixture_file(1024, 10, 'm');
    if (init_krffs_disk_root(disk_image, DISK_IMAGE_SIZE, 512, record_disk_flush, NULL) != 0) {
        check(0, what);
        return;
    }
    krffs_root_mount.backend = &memory_backend;
    ok = krffs_mkdir_at(&krffs_root_mount, "/d", 0755, 0, 0) == 0 &&
         krffs_create_at(&krffs_root_mount, "/d/x") == 0;
    node = krffs_walk(&krffs_root_mount, "/d/x");
    ok = ok && krffs_write_at(&krffs_root_mount, &node, "o", 1, 0) == 1;
    if (with_y) {
        ok = ok && krffs_create_at(&krffs_root_mount, "/y") == 0;
        node = krffs_walk(&krffs_root_mount, "/y");
        ok = ok && krffs_write_at(&krffs_root_mount, &node, "y", 1, 0) == 1;
    }
    k_memcpy(before, disk_image, DISK_IMAGE_SIZE);
    krffs_root_mount.backend = &backend;
    recorded_count = 0;
    sync_count = 0;
    recording = 1;
    ok = ok && krffs_rename_at(&krffs_root_mount, "/x", "/d/y") == 0;
    recording = 0;
    check(ok && root_file_is("/d/y", 10, 'm') && root_file_is("/d/x", 1, 'o') &&
              krffs_walk(&krffs_root_mount, "/x") == KRFFS_NO_NODE,
          what);
    rename_case_y = with_y;
    check(every_unit_cut_is_sound(before, DISK_IMAGE_SIZE, 512, rename_cut_is_sound),
          "every sector cut of a rename leaves each file under one name");
    check(every_unit_cut_is_sound(before, DISK_IMAGE_SIZE, 4096, rename_cut_is_sound),
          "every page cut of a rename leaves each file under one name");
    rename_case_y = 0;
}

/** How many names of the root directory of `mount` hold ten bytes of 'm',
 * the file the rename rows move. */
static int names_holding(krffs_mount_t *mount)
{
    char name[KRFFS_FILE_NAME_BUFFER_SIZE];
    char path[KRFFS_FILE_NAME_BUFFER_SIZE + 1];
    uint32_t i;
    int found = 0;

    for (i = 0; krffs_readdir_at(mount, "/", i, name, sizeof(name), (int *)0) == 1; i++) {
        path[0] = '/';
        k_memcpy(path + 1, name, k_strlen(name) + 1);
        found += mount_file_is(mount, path, 10, 'm');
    }
    return found;
}

/** `names_holding` of the boot root. */
static int root_names_holding(void)
{
    return names_holding(&krffs_root_mount);
}

/* Set when the torn-name rows put a file /ad beside the source. */
static int torn_case_ad;

/** A cut of a rename of /ab to /cd: it mounts, /cb and any /ad keep their
 * byte, and the renamed file is under exactly one name. */
static int torn_cut_is_sound(uint8_t *state, uint32_t size)
{
    if (init_krffs_disk_root(state, size, 512, NULL, NULL) != 0 ||
        !krffs_chain_is_valid(&krffs_root_mount) || !root_file_is("/cb", 1, 'c') ||
        (torn_case_ad && !root_file_is("/ad", 1, 'a'))) {
        return 0;
    }
    return root_names_holding() == 1;
}

/** A cut of a rename of /x over /y: it mounts, and the renamed file is
 * under exactly one name, which the repair would clear if it shared one
 * with /y's twenty bytes. */
static int replace_cut_is_sound(uint8_t *state, uint32_t size)
{
    if (init_krffs_disk_root(state, size, 512, NULL, NULL) != 0 ||
        !krffs_chain_is_valid(&krffs_root_mount)) {
        return 0;
    }
    return root_names_holding() == 1;
}

/**
 * Record a rename of `from` to `to` on the disk root built in the fixture,
 * whose file at 1024 or 1524 holds ten bytes of 'm', and judge every cut
 * of it, at sectors and at pages, by `judge`. Returns whether the rename
 * succeeded and every cut was sound.
 */
static int recorded_rename_is_sound(const char *from, const char *to,
                                    int (*judge)(uint8_t *, uint32_t))
{
    static const krffs_backend_t backend = {memory_read, record_write, memory_sync, 0};
    static uint8_t before[DISK_IMAGE_SIZE];
    int ok;

    if (init_krffs_disk_root(disk_image, DISK_IMAGE_SIZE, 512, record_disk_flush, NULL) != 0) {
        return 0;
    }
    k_memcpy(before, disk_image, DISK_IMAGE_SIZE);
    krffs_root_mount.backend = &backend;
    recorded_count = 0;
    sync_count = 0;
    recording = 1;
    ok = krffs_rename_at(&krffs_root_mount, from, to) == 0;
    recording = 0;
    krffs_root_mount.backend = &memory_backend;
    ok = ok && root_file_is(to, 10, 'm') && krffs_walk(&krffs_root_mount, from) == KRFFS_NO_NODE;
    return ok && every_unit_cut_is_sound(before, DISK_IMAGE_SIZE, 512, judge) &&
           every_unit_cut_is_sound(before, DISK_IMAGE_SIZE, 4096, judge);
}

/**
 * A rename of /ab to /cd on the disk root where a sector's edge cuts the
 * name field after its first byte, beside a file /cb, the halfway name of
 * the piece before the edge, and with `with_ad` a file /ad too, that of
 * the piece after it: no cut shows the renamed file under a name another
 * file has, which the repair would clear from one of the two.
 */
static void disk_torn_name_case(int with_ad, const char *what)
{
    static const struct unlink_extent plain[] = {
        {0, NULL}, {1524, "ab"}, {2048, "cb"}, {2560, NULL}, {60000, "kept"}};
    static const struct unlink_extent both[] = {{0, NULL},    {1524, "ab"}, {2048, "cb"},
                                                {2560, "ad"}, {3072, NULL}, {60000, "kept"}};

    if (with_ad) {
        make_unlink_image(both, 6);
    } else {
        make_unlink_image(plain, 5);
    }
    fill_fixture_file(1524, 10, 'm');
    check((1524 + KRFFS_FIELD_NAME) % 512 == 511, "the name field crosses a sector after a byte");
    torn_case_ad = with_ad;
    check(recorded_rename_is_sound("/ab", "/cd", torn_cut_is_sound), what);
    torn_case_ad = 0;
}

/**
 * A rename of /x over /y on the disk root, where /y holds more bytes: the
 * removal of /y reaches the medium before /x takes its name, so no cut
 * leaves the two under one name, which the repair would give /y.
 */
static void disk_replace_row(void)
{
    static const struct unlink_extent extents[] = {
        {0, NULL}, {1024, "x"}, {2048, "y"}, {3072, NULL}, {60000, "kept"}};

    make_unlink_image(extents, 5);
    fill_fixture_file(1024, 10, 'm');
    fill_fixture_file(2048, 20, 'z');
    check(recorded_rename_is_sound("/x", "/y", replace_cut_is_sound),
          "every cut of a rename over a larger file leaves the renamed file under one name");
}

/** A cut of a move of /e/x into /d: it mounts, the root's /x keeps its
 * byte, and the file is under exactly one of its two names. */
static int parent_cut_is_sound(uint8_t *state, uint32_t size)
{
    if (init_krffs_disk_root(state, size, 512, NULL, NULL) != 0 ||
        !krffs_chain_is_valid(&krffs_root_mount) || !root_file_is("/x", 1, 'r')) {
        return 0;
    }
    return root_file_is("/e/x", 10, 'm') + root_file_is("/d/x", 10, 'm') == 1;
}

/**
 * Build the disk root of the parent rows: /e/x at 1183 with ten bytes of
 * 'm', whose parent field crosses a sector, a directory /d, and a file /x
 * with one byte of 'r', and fill `*e` and `*d` with the two directories'
 * headers. Returns whether it was built with the two ids differing on both
 * sides of the sector.
 */
static int parent_fixture(krffs_node_t *e, krffs_node_t *d)
{
    static const struct unlink_extent extents[] = {
        {0, NULL}, {1183, "x"}, {2048, NULL}, {60000, "kept"}};
    uint64_t node;
    uint32_t at;

    make_unlink_image(extents, 4);
    fill_fixture_file(1183, 10, 'm');
    at = 1183 + KRFFS_FIELD_PARENT;
    check(at / 512 != (at + 7) / 512, "the parent field crosses a sector");
    if (init_krffs_disk_root(disk_image, DISK_IMAGE_SIZE, 512, NULL, NULL) != 0 ||
        krffs_mkdir_at(&krffs_root_mount, "/e", 0755, 0, 0) != 0 ||
        krffs_mkdir_at(&krffs_root_mount, "/d", 0755, 0, 0) != 0 ||
        krffs_rename_at(&krffs_root_mount, "/x", "/e/x") != 0 ||
        krffs_create_at(&krffs_root_mount, "/x") != 0) {
        check(0, "directories to move the file between");
        return 0;
    }
    node = krffs_walk(&krffs_root_mount, "/x");
    if (krffs_write_at(&krffs_root_mount, &node, "r", 1, 0) != 1 ||
        krffs_head(&krffs_root_mount, krffs_walk(&krffs_root_mount, "/e"), e) != 0 ||
        krffs_head(&krffs_root_mount, krffs_walk(&krffs_root_mount, "/d"), d) != 0 ||
        (uint32_t)e->id == (uint32_t)d->id || (e->id >> 32) == (d->id >> 32)) {
        check(0, "and the two directories' ids differ on both sides of the sector");
        return 0;
    }
    return 1;
}

/** Make the directory `path` on the boot root and give it the id `id`, as
 * if its name's hash had come out so. */
static int dir_with_id(const char *path, uint64_t id)
{
    uint64_t dir;

    if (krffs_mkdir_at(&krffs_root_mount, path, 0755, 0, 0) != 0) {
        return 0;
    }
    dir = krffs_walk(&krffs_root_mount, path);
    k_memcpy(disk_image + dir + KRFFS_FIELD_ID, &id, sizeof(id));
    return 1;
}

/**
 * The parent rows where a directory has the id of a halfway parent, part
 * of /e's id and part of /d's: the pieces go in the order whose halfway
 * parent no directory has, since the repair would leave the file in that
 * directory; with both halfway parents taken, the rename is refused before
 * anything is written.
 */
static void parent_halfway_rows(void)
{
    krffs_node_t e;
    krffs_node_t d;
    uint64_t low_first;
    uint64_t high_first;

    if (!parent_fixture(&e, &d)) {
        return;
    }
    low_first = (d.id & 0xFFFFFFFFull) | (e.id & ~0xFFFFFFFFull);
    high_first = (e.id & 0xFFFFFFFFull) | (d.id & ~0xFFFFFFFFull);
    check(dir_with_id("/f", low_first) &&
              recorded_rename_is_sound("/e/x", "/d/x", parent_cut_is_sound),
          "a halfway parent a directory has is not shown");
    if (!parent_fixture(&e, &d) || !dir_with_id("/f", low_first) ||
        !dir_with_id("/g", high_first)) {
        check(0, "directories with both halfway parents");
        return;
    }
    check(krffs_rename_at(&krffs_root_mount, "/e/x", "/d/x") != 0 &&
              root_file_is("/e/x", 10, 'm') && !krffs_root_mount.halted,
          "and with both taken the rename is refused");
}

/**
 * A rename of /e/x into /d on the disk root, whose parent field crosses a
 * sector with changed bytes on both sides, beside a file /x: the new parent
 * goes first into the id field, which lies in one sector, and a cut that
 * leaves a halfway parent, in no directory, is undone by the repair, which
 * puts the file in the directory its id names, not in the root beside /x,
 * also with no memory for the sorted directory ids. The file stays where
 * it is.
 */
static void disk_parent_move_row(void)
{
    krffs_node_t e;
    krffs_node_t d;

    if (!parent_fixture(&e, &d)) {
        return;
    }
    check(recorded_rename_is_sound("/e/x", "/d/x", parent_cut_is_sound) &&
              krffs_walk(&krffs_root_mount, "/d/x") == 1183,
          "every cut of a rename whose parent crosses a sector leaves the file in one directory");
    if (!parent_fixture(&e, &d)) {
        return;
    }
    rec_flushes = rec_flushes_owed = 0;
    check(recorded_rename_is_sound("/e/x", "/d/x", parent_cut_is_sound) && rec_flushes != 0 &&
              rec_flushes_owed == rec_flushes && !krffs_root_check_owed(),
          "the root owes a check at every flush of a rename, which a shutdown keeps the flag for");
    if (!parent_fixture(&e, &d)) {
        return;
    }
    kmalloc_floor = 3 * sizeof(krffs_name_key_t); /* two directories, five names */
    check(recorded_rename_is_sound("/e/x", "/d/x", parent_cut_is_sound),
          "and so does the repair with no memory for the directory ids");
    kmalloc_floor = 0;
    krffs_root_mount.check_owed = 0;
    if (!parent_fixture(&e, &d) || !recorded_rename_is_sound("/e/x", "/d/x", parent_cut_is_sound)) {
        check(0, "the file moved into /d again");
        return;
    }
    krffs_root_mount.backend = &recording_backend;
    refused_offset = 1536;
    check(krffs_rename_at(&krffs_root_mount, "/d/x", "/e/x") != 0 && krffs_root_mount.halted &&
              krffs_root_check_owed() && krffs_create_at(&krffs_root_mount, "/e/x") != 0,
          "a rename whose parent piece fails halts the mount and owes the check");
    refused_offset = (uint64_t)-1;
    krffs_root_mount.backend = &memory_backend;
    krffs_root_mount.check_owed = 0;
    krffs_root_mount.halted = 0;
}

/* The byte whose read `failing_read` fails, or none, and whether only the
 * first such read fails, as a medium that fails once and then reads. */
static uint64_t read_fails_at = (uint64_t)-1;
static int read_fails_once;

/** `memory_read`, failing a read that takes in `read_fails_at`. */
static int failing_read(void *ctx, uint64_t offset, void *buffer, uint32_t size)
{
    if (read_fails_at >= offset && read_fails_at < offset + size) {
        if (read_fails_once) {
            read_fails_at = (uint64_t)-1;
        }
        return -1;
    }
    return memory_read(ctx, offset, buffer, size);
}

/**
 * A reap that cannot read a node's name, or cannot free it, counts it and
 * halts the mount, so that the dirty flag stays for the next mount; so does
 * one whose removal cannot read a neighbour's header, even once, which it
 * does not take for a neighbour that is not free; and a parent lookup that
 * cannot read a matching directory's name answers EIO, not the root.
 */
static void reap_failure_rows(void)
{
    static const krffs_backend_t backend = {failing_read, record_write, memory_sync, 0};
    krffs_mount_t mount = mount_over(&backend, image, IMAGE_SIZE);
    krffs_node_t head;
    uint64_t node;
    uint64_t dir;
    uint64_t file;
    int err = 0;

    make_image(image, IMAGE_SIZE);
    check(krffs_create_at(&mount, "/a") == 0, "reap failure rows: a file");
    node = krffs_walk(&mount, "/a");
    check(krffs_orphan_at(&mount, node) == 0, "which loses its name");
    mount.orphans = 0; /* as the next mount finds it */
    read_fails_at = node + KRFFS_FIELD_NAME;
    check(krffs_reap_orphans_at(&mount) == 0 && mount.orphans == 1 && mount.halted &&
              mount.check_owed,
          "a reap that cannot read the name counts the node, halts the mount and owes the check");
    read_fails_at = (uint64_t)-1;
    mount.orphans = 0;
    mount.halted = 0; /* as the next mount finds it */
    mount.check_owed = 0;
    check(krffs_reap_orphans_at(&mount) == 1 && mount.orphans == 0 && !mount.halted,
          "and a reap that can frees it");

    make_image(image, IMAGE_SIZE);
    check(krffs_create_at(&mount, "/a") == 0, "reap failure rows: another file");
    node = krffs_walk(&mount, "/a");
    check(krffs_orphan_at(&mount, node) == 0, "which loses its name too");
    mount.orphans = 0;
    refused_offset = node + KRFFS_FIELD_TYPE;
    check(krffs_reap_orphans_at(&mount) == 0 && mount.orphans == 1 && mount.halted &&
              mount.check_owed,
          "a reap whose free fails counts the node, halts the mount and owes the check");
    refused_offset = (uint64_t)-1;
    mount.orphans = 0;
    mount.halted = 0;
    mount.check_owed = 0;
    read_fails_at = node + KRFFS_FIELD_TYPE;
    check(krffs_reap_orphans_at(&mount) == 0 && mount.orphans == 1 && mount.halted &&
              mount.check_owed,
          "and so does one a header read stops");
    read_fails_at = (uint64_t)-1;
    mount.halted = 0;
    mount.check_owed = 0;

    make_image(image, IMAGE_SIZE);
    check(krffs_create_at(&mount, "/a") == 0 && krffs_create_at(&mount, "/b") == 0,
          "reap failure rows: a file and its neighbour");
    node = krffs_walk(&mount, "/a");
    check(krffs_orphan_at(&mount, node) == 0, "the first loses its name");
    mount.orphans = 0;
    read_fails_at = krffs_walk(&mount, "/b") + KRFFS_FIELD_TYPE;
    read_fails_once = 1;
    check(krffs_reap_orphans_at(&mount) == 0 && mount.orphans == 1 && mount.halted &&
              mount.check_owed && krffs_head(&mount, node, &head) == 0 &&
              head.type == KRFFS_RESERVED_NODE,
          "a reap whose removal cannot read a neighbour once frees nothing and halts the mount");
    read_fails_at = (uint64_t)-1;
    read_fails_once = 0;
    mount.halted = 0;
    mount.check_owed = 0;

    make_image(image, IMAGE_SIZE);
    check(krffs_mkdir_at(&mount, "/d", 0755, 0, 0) == 0 && krffs_create_at(&mount, "/d/f") == 0,
          "reap failure rows: a file in a directory");
    dir = krffs_walk(&mount, "/d");
    file = krffs_walk(&mount, "/d/f");
    read_fails_at = dir + KRFFS_FIELD_NAME;
    check(krffs_parent_offset_err(&mount, file, &err) == KRFFS_NO_NODE && err == -EIO,
          "a parent lookup that cannot read the directory's name answers EIO");
    read_fails_at = (uint64_t)-1;
}

/**
 * A reap whose removal merges with a free node after it, and cannot repair
 * the backward link of the node after both, counts the orphan and halts the
 * mount; so does one that cannot read that node, even once, which it reads
 * before it frees anything.
 */
static void reap_link_failure_rows(void)
{
    static const krffs_backend_t backend = {failing_read, record_write, memory_sync, 0};
    krffs_mount_t mount = mount_over(&backend, image, IMAGE_SIZE);
    krffs_node_t head;
    uint64_t node;
    uint64_t file;

    make_image(image, IMAGE_SIZE);
    check(krffs_create_at(&mount, "/a") == 0 && krffs_create_at(&mount, "/b") == 0 &&
              krffs_create_at(&mount, "/c") == 0,
          "reap failure rows: an orphan, a free node and a file after them");
    node = krffs_walk(&mount, "/a");
    file = krffs_walk(&mount, "/c");
    check(krffs_unlink_at(&mount, "/b") == 0 && krffs_orphan_at(&mount, node) == 0,
          "the first loses its name and the second is freed");
    mount.orphans = 0;
    mount.stale_links = 0;
    refused_offset = file + KRFFS_FIELD_PREVIOUS;
    check(krffs_reap_orphans_at(&mount) == 0 && mount.orphans == 1 && mount.halted &&
              mount.check_owed && mount.stale_links,
          "a reap whose merge cannot repair the backward link after it halts the mount");
    refused_offset = (uint64_t)-1;
    mount.halted = 0;
    mount.check_owed = 0;
    mount.stale_links = 0;

    make_image(image, IMAGE_SIZE);
    check(krffs_create_at(&mount, "/a") == 0 && krffs_create_at(&mount, "/b") == 0 &&
              krffs_create_at(&mount, "/c") == 0,
          "reap failure rows: the same three again");
    node = krffs_walk(&mount, "/a");
    file = krffs_walk(&mount, "/c");
    check(krffs_unlink_at(&mount, "/b") == 0 && krffs_orphan_at(&mount, node) == 0,
          "the first loses its name and the second is freed again");
    mount.orphans = 0;
    read_fails_at = file + KRFFS_FIELD_TYPE;
    read_fails_once = 1;
    check(krffs_reap_orphans_at(&mount) == 0 && mount.orphans == 1 && mount.halted &&
              krffs_head(&mount, node, &head) == 0 && head.type == KRFFS_RESERVED_NODE,
          "and one that cannot read the node the merge repairs, even once, frees nothing");
    read_fails_at = (uint64_t)-1;
    read_fails_once = 0;
    mount.halted = 0;
    mount.check_owed = 0;
}

/**
 * The disk root's boot scan over a backend whose reads can fail, as the
 * page cache's copies answer a read the disk fails: a failed orphan free
 * and a header read that stops the scan each leave the orphan counted, so
 * that the shutdown keeps the dirty flag, and halt the root, which refuses
 * writes; the pointer interface answers the failure, a lookup, a walk, a
 * path and a lookup of `..` each answer EIO for a read that fails, and a
 * root node that cannot be read is refused at the mount, where the pointer
 * was read before.
 */
static void boot_scan_failure_rows(void)
{
    static const krffs_backend_t backend = {failing_read, record_write, memory_sync, 0};
    krffs_node_t head;
    char path[64];
    uint64_t node;
    uint64_t dir;
    int err = 0;
    int rows;

    for (rows = 0; rows < 2; rows++) {
        make_image(disk_image, DISK_IMAGE_SIZE);
        if (init_krffs_disk_root(disk_image, DISK_IMAGE_SIZE, 512, NULL, &backend) != 0 ||
            krffs_create("/a") != 0) {
            check(0, "boot scan rows: a disk root over a backend that can fail, and a file");
            return;
        }
        node = krffs_walk(&krffs_root_mount, "/a");
        check(krffs_orphan_at(&krffs_root_mount, node) == 0,
              "boot scan rows: the file loses its name");
        krffs_root_mount.orphans = 0; /* as the next boot finds it */
        if (rows == 0) {
            refused_offset = node + KRFFS_FIELD_TYPE;
        } else {
            read_fails_at = node + KRFFS_FIELD_TYPE;
        }
        check(krffs_reap_orphans() == 0 && krffs_root_has_orphans() && krffs_root_halted() &&
                  krffs_root_check_owed() && krffs_create("/b") != 0,
              rows == 0 ? "a boot scan whose orphan free fails counts it and halts the root"
                        : "a boot scan a header read stops counts the orphan and halts the root");
        check(rows == 0 || krffs_node_head((krffs_node_t *)(disk_image + node), &head) != 0,
              "a header the pointer interface cannot read answers the failure");
        refused_offset = (uint64_t)-1;
        read_fails_at = (uint64_t)-1;
    }

    make_image(disk_image, DISK_IMAGE_SIZE);
    if (init_krffs_disk_root(disk_image, DISK_IMAGE_SIZE, 512, NULL, &backend) != 0 ||
        krffs_mkdir_at(&krffs_root_mount, "/d", 0755, 0, 0) != 0 ||
        krffs_create_at(&krffs_root_mount, "/d/f") != 0) {
        check(0, "boot scan rows: a disk root with a file in a directory");
        return;
    }
    dir = krffs_walk(&krffs_root_mount, "/d");
    node = krffs_walk(&krffs_root_mount, "/d/f");
    read_fails_at = node + KRFFS_FIELD_NAME;
    check(krffs_child_err((krffs_node_t *)(disk_image + dir), "g", &err) == NULL && err == -EIO,
          "a lookup that cannot read a name in the directory answers EIO, not ENOENT");
    check(krffs_resolve_err("/d/g", &err) == NULL && err == -EIO, "and so does a walk of the path");
    check(krffs_node_path((krffs_node_t *)(disk_image + node), path, sizeof(path)) == -EIO,
          "and the path of the file whose name cannot be read");
    read_fails_at = dir + KRFFS_FIELD_NAME;
    check(krffs_parent_node_err((krffs_node_t *)(disk_image + node), &err) == NULL && err == -EIO,
          "a lookup of `..` that cannot read the directory's name answers EIO, not the root");
    read_fails_at = (uint64_t)-1;
    check(krffs_child_err((krffs_node_t *)(disk_image + dir), "g", &err) == NULL &&
              err == -ENOENT &&
              krffs_parent_node_err((krffs_node_t *)(disk_image + node), &err) ==
                  (krffs_node_t *)(disk_image + dir) &&
              err == 0,
          "and once the reads succeed, a missing name is ENOENT and `..` the directory");

    make_image(disk_image, DISK_IMAGE_SIZE);
    read_fails_at = KRFFS_FIELD_MODE;
    check(init_krffs_disk_root(disk_image, DISK_IMAGE_SIZE, 512, NULL, &backend) != 0,
          "a disk root whose root node cannot be read is refused");
    read_fails_at = (uint64_t)-1;
}

/* The node the holder test of the refusal rows was asked about. */
static uint64_t held_asked = KRFFS_NO_NODE;

/** A holder test that holds every node and keeps the one it was asked. */
static int hold_all(void *arg, uint64_t offset)
{
    (void)arg;
    held_asked = offset;
    return 1;
}

/**
 * The count of nodes without a name, which keeps the dirty flag at an
 * unmount or a shutdown: the copy of an orphan that a grow moves stays
 * counted until it is freed; a create whose write or sync fails anywhere
 * counts every node without a name it leaves; and a node's directory is
 * never a directory without a name that has the id of a later one.
 */
static void orphan_count_rows(void)
{
    krffs_mount_t mount;
    uint64_t node;
    uint64_t moved;
    uint64_t dir;
    uint64_t file;
    uint32_t left;
    uint32_t reaped;
    int which;
    int n;
    int total;
    int short_count = 0;

    make_image(image, IMAGE_SIZE);
    mount = mount_over(&memory_backend, image, IMAGE_SIZE);
    check(krffs_create_at(&mount, "/o") == 0 && krffs_create_at(&mount, "/b") == 0,
          "orphan count rows: two files");
    node = krffs_walk(&mount, "/o");
    check(mount.orphans == 0 && krffs_orphan_at(&mount, node) == 0 && mount.orphans == 1,
          "an orphan is counted");
    moved = krffs_resize_at(&mount, node, 600);
    check(moved != KRFFS_NO_NODE && moved != node && mount.orphans == 1,
          "and its copy stays counted when a grow moves it");
    check(krffs_remove_at(&mount, moved) == 0 && mount.orphans == 0,
          "and leaves the count when it is freed");

    for (which = 0; which < 2; which++) {
        for (n = 0;; n++) {
            make_image(image, IMAGE_SIZE);
            mount = mount_over(&recording_backend, image, IMAGE_SIZE);
            writes_seen = 0;
            if (which == 0) {
                refused_write = n;
            } else {
                refused_sync = n;
            }
            (void)krffs_create_at(&mount, "/f");
            total = writes_seen;
            refused_write = -1;
            refused_sync = -1;
            left = mount.orphans;
            mount.unsettled = 0;
            reaped = krffs_reap_orphans_at(&mount);
            if (left < reaped) {
                short_count++;
            }
            if (n >= total) {
                break;
            }
        }
    }
    check(short_count == 0, "a create that fails anywhere counts the nodes without a name it left");

    make_image(image, IMAGE_SIZE);
    mount = mount_over(&memory_backend, image, IMAGE_SIZE);
    check(krffs_mkdir_at(&mount, "/d", 0755, 0, 0) == 0, "orphan count rows: a directory");
    dir = krffs_walk(&mount, "/d");
    check(krffs_orphan_at(&mount, dir) == 0, "which loses its name");
    check(krffs_mkdir_at(&mount, "/d", 0755, 0, 0) == 0 && krffs_create_at(&mount, "/d/f") == 0,
          "a later directory with its id, and a file in it");
    file = krffs_walk(&mount, "/d/f");
    check(krffs_walk(&mount, "/d") != dir &&
              krffs_parent_offset(&mount, file) == krffs_walk(&mount, "/d"),
          "the file's directory is the named one");
}

/**
 * A rename whose destination's header or name cannot be read is refused
 * before it writes, since taking it for no destination would give the name
 * to two nodes; a rename whose plan finds no order is refused before an
 * open destination loses its name; and the holder test is asked of the
 * destination the core found, which keeps its node without a name.
 */
static void rename_refusal_rows(void)
{
    static const krffs_backend_t backend = {failing_read, memory_write, memory_sync, 0};
    static const struct unlink_extent extents[] = {{0, NULL},    {1524, "ab"}, {2048, "cb"},
                                                   {2560, "ad"}, {3072, "cd"}, {3584, NULL}};
    krffs_mount_t mount = mount_over(&backend, image, IMAGE_SIZE);
    krffs_node_t head;
    uint64_t orphaned;
    uint64_t x;
    uint64_t y;
    char path[24];
    int i;

    make_image(image, IMAGE_SIZE);
    check(krffs_create_at(&mount, "/x") == 0 && krffs_create_at(&mount, "/y") == 0,
          "refusal rows: two files");
    x = krffs_walk(&mount, "/x");
    y = krffs_walk(&mount, "/y");
    check(x < y, "and the destination comes after the source");
    read_fails_at = y + KRFFS_FIELD_TYPE;
    check(krffs_rename_at(&mount, "/x", "/y") != 0,
          "a rename whose destination cannot be read is refused");
    read_fails_at = y + KRFFS_FIELD_NAME + 1;
    check(krffs_rename_at(&mount, "/x", "/y") != 0,
          "and one whose destination's name cannot be read");
    read_fails_at = (uint64_t)-1;
    check(krffs_walk(&mount, "/x") == x && krffs_walk(&mount, "/y") == y,
          "and both files keep their names");
    check(krffs_rename_err(&mount, "/x", "/y", hold_all, NULL, &orphaned) == 0 && held_asked == y &&
              orphaned == y && krffs_walk(&mount, "/y") == x,
          "a held destination is the one the core found");
    check(krffs_head(&mount, y, &head) == 0 && head.type == KRFFS_RESERVED_NODE &&
              head.name[0] == '\0',
          "and keeps its node without a name");

    make_unlink_image(extents, 6);
    check(init_krffs_disk_root(disk_image, DISK_IMAGE_SIZE, 512, NULL, NULL) == 0,
          "refusal rows: a root with both halfway names taken");
    for (i = 0; i < 16; i++) {
        k_memcpy(path, "/.rename-000005f4", 17);
        path[17] = "0123456789abcdef"[i];
        path[18] = '\0';
        check(krffs_create_at(&krffs_root_mount, path) == 0, "and every spare name");
    }
    y = krffs_walk(&krffs_root_mount, "/cd");
    check(krffs_rename_err(&krffs_root_mount, "/ab", "/cd", hold_all, NULL, &orphaned) != 0 &&
              orphaned == KRFFS_NO_NODE,
          "a rename with no order whose halfway names are free is refused");
    check(krffs_walk(&krffs_root_mount, "/cd") == y && krffs_walk(&krffs_root_mount, "/ab") == 1524,
          "and its open destination keeps its name");
}

/* Where the image of the phase row lies in the disk fixture: the data of
   the file /img at 1024, 357 bytes into a sector. */
#define INNER_BASE (1024u + (uint32_t)sizeof(krffs_node_t))
#define INNER_SIZE 8192u

/** `record_write` of the image of the phase row, at its place in the
 * fixture, so that the cuts fall at the fixture's sectors. */
static int inner_record_write(void *ctx, uint64_t offset, const void *buffer, uint32_t size)
{
    (void)ctx;
    return record_write(disk_image, INNER_BASE + offset, buffer, size);
}

/** A mount of the image of the phase row in the fixture state `outer`, as
 * a loop device over /img gives it: 4 KiB blocks, 512-byte sectors, and
 * the file's phase. */
static krffs_mount_t inner_mount(const krffs_backend_t *backend, uint8_t *outer)
{
    krffs_mount_t mount = mount_over(backend, outer + INNER_BASE, INNER_SIZE);

    mount.grain = 4096;
    mount.sector = 512;
    mount.sector_phase = INNER_BASE % 512u;
    return mount;
}

/** A cut of the rename of the phase row: the image in /img walks, repairs,
 * keeps /cb, and has the renamed file under exactly one name. */
static int inner_cut_is_sound(uint8_t *state, uint32_t size)
{
    krffs_mount_t mount = inner_mount(&memory_backend, state);

    (void)size;
    if (!forward_chain_closes(&mount) ||
        krffs_repair_at(&mount, (uint32_t *)0, (uint32_t *)0, (uint32_t *)0) != 0 ||
        !mount_file_is(&mount, "/cb", 1, 'c')) {
        return 0;
    }
    return names_holding(&mount) == 1;
}

/**
 * A rename of /ab to /cd in a KRFFS image that is the file /img of the
 * disk fixture, whose data starts 357 bytes into a sector: the name field
 * lies within one sector of the image, but a sector of the medium cuts it
 * after its first byte, and /cb, the halfway name, is there. The mount
 * takes the phase, so no cut of the medium shows the file as /cb.
 */
static void phase_row(void)
{
    static const struct unlink_extent outer[] = {
        {0, NULL}, {1024, "img"}, {10240, NULL}, {60000, "kept"}};
    static const struct unlink_extent inner[] = {
        {0, NULL}, {655, "ab"}, {1536, "cb"}, {2048, NULL}};
    static const krffs_backend_t backend = {memory_read, inner_record_write, record_sync, 0};
    static uint8_t before[DISK_IMAGE_SIZE];
    krffs_mount_t mount;
    uint64_t node;
    int ok;

    make_unlink_image(outer, 4);
    ((krffs_node_t *)(disk_image + 1024))->data_size = INNER_SIZE;
    make_extent_image(disk_image + INNER_BASE, INNER_SIZE, inner, 4);
    check((655 + KRFFS_FIELD_NAME) / 512 == (655 + KRFFS_FIELD_NAME + 255) / 512 &&
              (INNER_BASE + 655 + KRFFS_FIELD_NAME + 1) % 512 == 0,
          "phase row: a name field in one sector of the image, cut by one of the medium");
    mount = inner_mount(&memory_backend, disk_image);
    node = krffs_walk(&mount, "/ab");
    ok = krffs_write_at(&mount, &node, "mmmmmmmmmm", 10, 0) == 10 && node == 655;
    k_memcpy(before, disk_image, DISK_IMAGE_SIZE);
    mount = inner_mount(&backend, disk_image);
    recorded_count = 0;
    sync_count = 0;
    recording = 1;
    ok = ok && krffs_rename_at(&mount, "/ab", "/cd") == 0;
    recording = 0;
    mount = inner_mount(&memory_backend, disk_image);
    check(ok && mount_file_is(&mount, "/cd", 10, 'm') &&
              every_unit_cut_is_sound(before, DISK_IMAGE_SIZE, 512, inner_cut_is_sound) &&
              every_unit_cut_is_sound(before, DISK_IMAGE_SIZE, 4096, inner_cut_is_sound),
          "every cut of the medium under a rename in an image at a phase leaves one name");
}

/** The rename rows of names and parents a sector cuts, of a destination's
 * removal, and of refusals before anything is written. */
static void rename_unit_rows(void)
{
    disk_torn_name_case(0, "a rename whose name a sector cuts writes the free half first");
    disk_torn_name_case(1, "and by way of a spare name when both halves are taken");
    disk_replace_row();
    disk_parent_move_row();
    parent_halfway_rows();
    rename_refusal_rows();
    orphan_count_rows();
    reap_failure_rows();
    reap_link_failure_rows();
    boot_scan_failure_rows();
    phase_row();
}

/* The type field the watching sync looks at, whether it fails once more,
   and the syncs it saw after that type was stored as free. */
static uint64_t watched_type = (uint64_t)-1;
static int watch_armed;
static int syncs_after_free;

/** Fail the first sync after the watched type reads free, and count the
 * syncs after it. */
static int watch_sync(void *ctx)
{
    if (watched_type != (uint64_t)-1 && ((uint8_t *)ctx)[watched_type] == KRFFS_FREE_NODE) {
        if (watch_armed) {
            watch_armed = 0;
            return -1;
        }
        syncs_after_free++;
    }
    return 0;
}

/**
 * A move whose old node's type is stored as free and whose removal's sync
 * then fails still puts a barrier before it answers, so that the type is
 * on the medium, or the mount unsettled, before a rename can name the copy.
 */
static void failed_move_barrier_row(void)
{
    static const krffs_backend_t backend = {memory_read, memory_write, watch_sync, 0};
    static char bytes[2000];
    krffs_mount_t mount;
    uint64_t node;
    uint64_t old;

    k_memset(bytes, 'm', sizeof(bytes));
    make_image(image, IMAGE_SIZE);
    mount = mount_over(&backend, image, IMAGE_SIZE);
    check(krffs_create_at(&mount, "/moved") == 0, "failed move barrier: a file");
    node = krffs_walk(&mount, "/moved");
    check(krffs_write_at(&mount, &node, bytes, 100, 0) == 100 &&
              krffs_create_at(&mount, "/wall") == 0,
          "failed move barrier: its bytes and a node after it");
    old = node;
    watched_type = old + KRFFS_FIELD_TYPE;
    watch_armed = 1;
    syncs_after_free = 0;
    check(krffs_write_at(&mount, &node, bytes, sizeof(bytes), 0) == KRFFS_EIO && node != old &&
              syncs_after_free >= 1,
          "a move whose removal's sync fails follows the copy and puts a barrier first");
    watched_type = (uint64_t)-1;
}

/**
 * A dirty volume of more named nodes than the old pairwise search took has
 * its links rebuilt and its repeated name found; one whose index cannot be
 * allocated has its links rebuilt and owes the check of names instead.
 */
static void many_names_row(void)
{
    enum { NAMES = 4100 };
    uint32_t node_size = sizeof(krffs_node_t) + 1;
    uint32_t size = sizeof(krffs_node_t) + NAMES * node_size + 1024;
    uint8_t *big = malloc(size);
    krffs_node_t *head;
    krffs_mount_t mount;
    uint32_t links = 0;
    uint32_t doubles = 0;
    uint32_t at;
    int i;

    if (!big) {
        check(0, "many names: memory");
        return;
    }
    k_memset(big, 0, size);
    head = (krffs_node_t *)big;
    head->magic = KRFFS_FILE_SYSTEM_MAGIC;
    head->type = KRFFS_ROOT_NODE;
    head->mode = (uint16_t)(KRFFS_S_IFDIR | 0755 | KRFFS_MODE_DIRTY);
    head->size = sizeof(krffs_node_t);
    head->previous_node_size = 1024;
    at = sizeof(krffs_node_t);
    for (i = 0; i < NAMES; i++) {
        head = (krffs_node_t *)(big + at);
        head->magic = KRFFS_FILE_SYSTEM_MAGIC;
        head->type = KRFFS_RESERVED_NODE;
        head->mode = 0644;
        head->size = node_size;
        head->previous_node_size = i == 0 ? sizeof(krffs_node_t) : node_size;
        head->name[0] = (uint8_t)('a' + i % 26);
        head->name[1] = (uint8_t)('a' + i / 26 % 26);
        head->name[2] = (uint8_t)('a' + i / 676);
        at += node_size;
    }
    k_memcpy(head->name, ((krffs_node_t *)(big + sizeof(krffs_node_t)))->name, 4);
    head = (krffs_node_t *)(big + at);
    head->magic = KRFFS_FILE_SYSTEM_MAGIC;
    head->type = KRFFS_FREE_NODE;
    head->size = 1024;
    head->previous_node_size = 0;
    mount = mount_over(&memory_backend, big, size);
    check(!krffs_chain_is_valid(&mount), "many names: a link is torn");
    kmalloc_limit = 64 * sizeof(krffs_name_key_t);
    check(krffs_repair_at(&mount, &links, (uint32_t *)0, &doubles) == 0 && links == 1 &&
              doubles == 1 && !mount.check_owed && krffs_chain_is_valid(&mount) &&
              ((krffs_node_t *)(big + at - node_size))->name[0] == '\0',
          "a volume of more names than the index holds has its links rebuilt and its repeated "
          "name found in passes");
    kmalloc_limit = 0;
    ((krffs_node_t *)(big + sizeof(krffs_node_t)))->previous_node_size = 0;
    kmalloc_refuses = 1;
    check(krffs_repair_at(&mount, &links, (uint32_t *)0, &doubles) == 0 && links == 1 &&
              doubles == 0 && mount.check_owed && krffs_chain_is_valid(&mount),
          "and one whose index cannot be allocated has its links rebuilt and owes the name check");
    kmalloc_refuses = 0;
    free(big);
}

/** Check page and sector writeback cuts, and the limits of disk-root repair. */
static void disk_unlink_rows(void)
{
    static const struct unlink_extent both[] = {
        {0, NULL}, {1024, NULL}, {5125, "victim"}, {9227, NULL}, {16050, "kept"}};
    static const struct unlink_extent tail[] = {
        {0, NULL}, {1024, "kept"}, {5125, NULL}, {9227, "victim"}, {16050, NULL}};
    static const struct unlink_extent split_next[] = {
        {0, NULL}, {1024, "kept"}, {3770, "victim"}, {4795, NULL}, {6074, "last"}};
    static const struct unlink_extent split_previous[] = {
        {0, NULL}, {1024, "kept"}, {3770, NULL}, {4794, "victim"}, {6074, "last"}};
    static const struct unlink_extent pending[] = {
        {0, NULL}, {3769, NULL}, {4793, "victim"}, {70329, "second"}, {70841, "kept"}};
    krffs_node_t *root;
    krffs_node_t *free_node;

    disk_unlink_case(both, sizeof(both) / sizeof(both[0]), 1024, 16050 - 1024, NULL);
    disk_unlink_case(tail, sizeof(tail) / sizeof(tail[0]), 5125, DISK_IMAGE_SIZE - 5125, NULL);
    disk_unlink_case(split_next, sizeof(split_next) / sizeof(split_next[0]), 3770, 1025, NULL);
    disk_unlink_case(split_previous, sizeof(split_previous) / sizeof(split_previous[0]), 3770, 1024,
                     NULL);
    disk_unlink_case(pending, sizeof(pending) / sizeof(pending[0]), 3769, 1024, "/second");
    disk_shrink_row();
    disk_split_length_rows();
    disk_grow_rows();
    disk_after_unlink_rows();
    volume_merge_row();
    writeback_error_rows();
    short_write_rows();
    visible_write_rows();
    disk_moving_write_rows();
    volume_shrink_grow_row();
    failed_grow_barrier_row();
    partial_size_row();
    moving_write_refusals();
    withdrawn_shrink_row();
    withdrawn_larger_copy_row();
    disk_regrow_row();
    partial_length_row();
    disk_rename_case(0, "a rename beside a file of the old name in the new directory completes");
    disk_rename_case(1, "a rename where both halves meet a name completes");
    rename_unit_rows();
    failed_move_barrier_row();
    disk_double_name_row();
    many_names_row();

    make_image(image, IMAGE_SIZE);
    root = (krffs_node_t *)image;
    free_node = (krffs_node_t *)(image + sizeof(*root));
    free_node->previous_node_size = 0;
    check(init_krffs_disk_root(image, IMAGE_SIZE, 512, NULL, NULL) != 0,
          "a clean disk root with a bad link is refused");
    root->mode |= KRFFS_MODE_DIRTY;
    check(init_krffs(image, IMAGE_SIZE) != 0, "a RAM root with a bad link is refused");
    root->previous_node_size = 0;
    free_node->size--;
    check(init_krffs_disk_root(image, IMAGE_SIZE, 512, NULL, NULL) != 0 &&
              free_node->previous_node_size == 0 && root->previous_node_size == 0,
          "a dirty disk root with a broken forward chain is refused before repair");
}

/* An ordinal in the successful repair's sequence of reads, stores and
 * barriers. Fail that one operation and count subsequent mutations. */
static int io_at, io_fail, io_broken, io_late;

/** Refuse the selected operation once and count writes or syncs after it. */
static int refuse_io(int mutation)
{
    if (io_broken && mutation) {
        io_late++;
    }
    if (++io_at == io_fail) {
        io_broken = 1;
        return 1;
    }
    return 0;
}

/** The image read with a single injected error. */
static int repair_read(void *ctx, uint64_t at, void *buf, uint32_t n)
{
    return refuse_io(0) ? -1 : memory_read(ctx, at, buf, n);
}

/** The image store with a single injected error. */
static int repair_store(void *ctx, uint64_t at, const void *buf, uint32_t n)
{
    return refuse_io(1) ? -1 : memory_write(ctx, at, buf, n);
}

/** The image barrier with a single injected error. */
static int repair_sync(void *ctx)
{
    return refuse_io(1) ? -1 : memory_sync(ctx);
}

/**
 * Fail each I/O of a boot or volume repair once. The fixture needs a link
 * repair, a parent repair, duplicate resolution, and a reap with free
 * space on both sides. A later I/O would succeed.
 */
static void repair_io_rows(int boot)
{
    static const krffs_backend_t backend = {repair_read, repair_store, repair_sync, 0};
    static const struct unlink_extent layout[] = {{0, NULL},      {600, "same"}, {1200, NULL},
                                                  {1800, "same"}, {2400, NULL},  {3000, "keep"}};
    int i, n = 0;

    for (i = 0; i <= n; i++) {
        krffs_mount_t mount = mount_over(&backend, disk_image, DISK_IMAGE_SIZE);
        krffs_mount_t *fs = boot ? &krffs_root_mount : &mount;
        int rc;

        make_unlink_image(layout, 6);
        ((krffs_node_t *)disk_image)->previous_node_size = 0;
        ((krffs_node_t *)(disk_image + 3000))->parent_id = 77;
        io_at = io_late = io_broken = 0;
        io_fail = i;
        rc = boot ? init_krffs_disk_root(disk_image, DISK_IMAGE_SIZE, 512, NULL, &backend)
                  : krffs_repair_at(&mount, NULL, NULL, NULL);
        if (i == 0) {
            check(rc == 0 && !fs->halted, "the complete repair succeeds before injection");
            n = io_at;
        } else if (!(io_broken && rc != 0 && fs->halted && fs->check_owed && !io_late &&
                     (!boot || !krffs_fs))) {
            printf("krffs-hosttest: %s I/O %d: return %d, halt %d, late writes or syncs %d\n",
                   boot ? "boot" : "volume", i, rc, fs->halted, io_late);
            check(0, "the first repair error halts before any more stores or barriers");
        }
    }
    io_fail = 0;
}

/** Fail only a separate read of the old size across a sector edge. */
static int refuse_size_read(void *ctx, uint64_t at, void *buf, uint32_t n)
{
    if (at == 1020 && n == sizeof(uint64_t)) {
        return -1;
    }
    return memory_read(ctx, at, buf, n);
}

/** Use the old header for each split decision even at a sector edge. */
static void extent_decision_rows(void)
{
    static const krffs_backend_t backend = {refuse_size_read, memory_write, memory_sync, 0};
    static const struct unlink_extent layout[] = {{0, NULL}, {695, "edge"}, {2000, NULL}};
    krffs_mount_t mount = mount_over(&backend, disk_image, DISK_IMAGE_SIZE);
    krffs_node_t *h = (krffs_node_t *)(disk_image + 695);
    krffs_node_t *next = (krffs_node_t *)(disk_image + 2000);
    uint64_t whole = DISK_IMAGE_SIZE - 695;

    mount.sector = 512;
    make_unlink_image(layout, 3);
    check(krffs_grow_into(&mount, 695, 2000, h, next) == 0 && h->size == 2000,
          "split growth compares the size from the node header");
    make_unlink_image(layout, 3);
    check(krffs_grow_into(&mount, 695, whole, h, next) == 0 && h->size == whole,
          "whole extent growth compares the size from the node header");
    make_unlink_image(layout, 3);
    check(krffs_shrink_at(&mount, 695, 2, h, next) == 0 && h->size == sizeof(*h) + 2,
          "shrink compares the size from the node header");
    make_unlink_image(layout, 3);
    h->type = KRFFS_FREE_NODE;
    check(krffs_take_free(&mount, 695, h->size, 600) == 0 && h->size == 600,
          "allocation compares the size from the free header");
}

/** Preserve I/O errors from write, truncate, validation and last close. */
static void header_failure_rows(void)
{
    static const krffs_backend_t backend = {failing_read, record_write, memory_sync, 0};
    static const struct unlink_extent layout[] = {{0, NULL}, {600, "edge"}, {2000, NULL}};
    krffs_mount_t mount = mount_over(&backend, disk_image, DISK_IMAGE_SIZE);
    uint64_t node = 600, live;

    make_unlink_image(layout, 3);
    read_fails_at = node + KRFFS_FIELD_TYPE;
    check(krffs_write_at(&mount, &node, "x", 1, 0) == KRFFS_EIO,
          "write preserves a header I/O error");
    check(krffs_truncate_node_at(&mount, node, 0, NULL) == KRFFS_EIO,
          "truncate preserves a header I/O error");
    check(krffs_resize_err(&mount, node, 0, &live) == KRFFS_EIO,
          "a failed resize header read is an I/O error");
    read_fails_at = (uint64_t)-1;
    check(init_krffs_disk_root(disk_image, DISK_IMAGE_SIZE, 512, NULL, &backend) == 0,
          "publish a root before refusing its replacement");
    read_fails_at = node + KRFFS_FIELD_TYPE;
    read_fails_once = 1;
    check(init_krffs_disk_root(disk_image, DISK_IMAGE_SIZE, 512, NULL, &backend) != 0 &&
              krffs_root_halted() && !krffs_fs,
          "a transient validation read error stops root writeback and clears the published root");
    read_fails_at = (uint64_t)-1;
    read_fails_once = 0;
    check(init_krffs_disk_root(disk_image, DISK_IMAGE_SIZE, 512, NULL, &backend) == 0 &&
              krffs_orphan_at(&krffs_root_mount, node) == 0,
          "mount root and orphan its file");
    read_fails_at = node + KRFFS_FIELD_TYPE;
    krffs_remove_reserved_node(krffs_fs, krffs_node_of(node));
    check(krffs_root_halted() && krffs_root_check_owed() && krffs_root_has_orphans(),
          "the last holder's failed reap leaves a halted dirty root");
    read_fails_at = (uint64_t)-1;
}

/** Read the root link before publishing a merge at the end of the ring. */
static void last_extent_row(void)
{
    static const krffs_backend_t backend = {failing_read, record_write, memory_sync, 0};
    static const struct unlink_extent layout[] = {{0, NULL}, {600, NULL}, {1200, "last"}};
    krffs_mount_t mount = mount_over(&backend, disk_image, DISK_IMAGE_SIZE);
    int before = writes_seen;

    make_unlink_image(layout, 3);
    read_fails_at = 0;
    check(krffs_remove_at(&mount, 1200) < 0 && writes_seen == before,
          "an unreadable root link stops the last extent's merge before its first store");
    read_fails_at = (uint64_t)-1;
}

/** A failed withdrawal records recovery debt and blocks backend sync. */
static void withdrawn_sync_row(void)
{
    krffs_mount_t mount = mount_over(&recording_backend, disk_image, DISK_IMAGE_SIZE);
    int before;

    refused_offset = 600 + KRFFS_FIELD_TYPE;
    krffs_withdraw(&mount, 600);
    check(mount.halted && mount.check_owed && mount.withdrawn == 600,
          "a failed withdrawal records the hidden copy and recovery debt");
    before = writes_seen;
    check(krffs_sync_point(&mount) != 0 && writes_seen == before,
          "a halted core refuses a sync before calling the backend");
}

/** Run the disk failure rows in isolation or as part of the host suite. */
static void disk_failure_rows(void)
{
    repair_io_rows(0);
    repair_io_rows(1);
    last_extent_row();
    withdrawn_sync_row();
    extent_decision_rows();
    header_failure_rows();
}

/** Run the image and namespace cases. */
static int image_rows(void)
{
    static uint8_t before[IMAGE_SIZE];
    krffs_mount_t mount;
    krffs_node_t head;
    uint64_t node;
    uint64_t other;
    char buffer[1024];
    char name[64];
    int is_dir;
    int i;

    hash_rows();
    ino_rows();
    ino_lifetime_rows();
    straddle_create_rows();
    id_collision_rows();
    parent_id_collision_rows();
    across_rows();
    freed_write_rows();
    disk_unlink_rows();

    make_image(image, IMAGE_SIZE);
    mount = mount_over(&memory_backend, image, IMAGE_SIZE);
    check(krffs_chain_is_valid(&mount) == 1, "a fresh image is sound");

    image[0] = 0;
    check(krffs_chain_is_valid(&mount) == 0, "a bad magic is refused");
    make_image(image, IMAGE_SIZE);

    image[sizeof(krffs_node_t) + KRFFS_FIELD_TYPE] = 42;
    check(krffs_chain_is_valid(&mount) == 0, "an unknown node type is refused");
    make_image(image, IMAGE_SIZE);

    {
        uint64_t huge = IMAGE_SIZE;

        k_memcpy(image + sizeof(krffs_node_t) + KRFFS_FIELD_SIZE, &huge, sizeof(huge));
        check(krffs_chain_is_valid(&mount) == 0, "a size that leaves the region is refused");
        make_image(image, IMAGE_SIZE);
    }
    {
        uint64_t small = sizeof(krffs_node_t) - 1;

        k_memcpy(image + sizeof(krffs_node_t) + KRFFS_FIELD_SIZE, &small, sizeof(small));
        check(krffs_chain_is_valid(&mount) == 0, "a size below a header is refused");
        make_image(image, IMAGE_SIZE);
    }
    {
        uint64_t data = IMAGE_SIZE;

        krffs_create_at(&mount, "/data.bin");
        k_memcpy(image + sizeof(krffs_node_t) + KRFFS_FIELD_DATA_SIZE, &data, sizeof(data));
        check(krffs_chain_is_valid(&mount) == 0, "data larger than its node is refused");
        make_image(image, IMAGE_SIZE);
    }
    {
        uint64_t data = IMAGE_SIZE;

        k_memcpy(image + sizeof(krffs_node_t) + KRFFS_FIELD_DATA_SIZE, &data, sizeof(data));
        check(krffs_chain_is_valid(&mount) == 1,
              "a free node's length is not a fault whatever it says");
        make_image(image, IMAGE_SIZE);
    }
    {
        uint64_t wrong = 12345;

        k_memcpy(image + sizeof(krffs_node_t) + KRFFS_FIELD_PREVIOUS, &wrong, sizeof(wrong));
        check(krffs_chain_is_valid(&mount) == 0,
              "a backward link the forward chain contradicts is refused");
        make_image(image, IMAGE_SIZE);
    }
    {
        uint64_t wrong = 12345;

        k_memcpy(image + KRFFS_FIELD_PREVIOUS, &wrong, sizeof(wrong));
        check(krffs_chain_is_valid(&mount) == 0,
              "a root whose backward link is not the last node is refused");
        make_image(image, IMAGE_SIZE);
    }
    {
        uint64_t offset = sizeof(krffs_node_t);

        image[offset + KRFFS_FIELD_TYPE] = KRFFS_RESERVED_NODE;
        for (i = 0; i < KRFFS_FILE_NAME_BUFFER_SIZE; i++) {
            image[offset + KRFFS_FIELD_NAME + i] = 'x';
        }
        check(krffs_chain_is_valid(&mount) == 0, "a name that never ends is refused");
        make_image(image, IMAGE_SIZE);
    }

    check(krffs_allocate_at(&mount, (uint64_t)-1, KRFFS_NO_NODE) == KRFFS_NO_NODE,
          "a size larger than the region allocates nothing");
    check(krffs_allocate_at(&mount, IMAGE_SIZE, KRFFS_NO_NODE) == KRFFS_NO_NODE,
          "and neither does one exactly the region's size");
    check(krffs_chain_is_valid(&mount) == 1, "and the chain is what it was before both");

    check(krffs_create_at(&mount, "/alpha") == 0, "a file is created");
    node = krffs_walk(&mount, "/alpha");
    check(krffs_resize_at(&mount, node, (uint64_t)-1) == KRFFS_NO_NODE,
          "a resize to more than the region answers nothing");
    check(krffs_chain_is_valid(&mount) == 1, "and changes nothing");
    check(krffs_chain_is_valid(&mount) == 1, "the chain is sound after it");
    node = krffs_walk(&mount, "/alpha");
    check(node != KRFFS_NO_NODE && node != 0, "and the walk finds it");
    check(krffs_head(&mount, node, &head) == 0 && head.data_size == 0, "a new file is empty");

    for (i = 0; i < (int)sizeof(buffer); i++) {
        buffer[i] = (char)((i % 127) + 1);
    }
    check(krffs_write_at(&mount, &node, buffer, 16, 0) == 16, "sixteen bytes are written");
    check(krffs_head(&mount, node, &head) == 0 && head.data_size == 16,
          "and the file is sixteen bytes long");
    k_memset(buffer, 0, sizeof(buffer));
    check(krffs_read_at(&mount, node, buffer, 16, 0) == 16 && buffer[0] == 1 && buffer[15] == 16,
          "and they read back");
    check(krffs_read_at(&mount, node, buffer, 16, 16) == 0, "a read past the end answers nothing");
    check(krffs_chain_is_valid(&mount) == 1, "the chain is sound after a write");

    buffer[0] = 'z';
    check(krffs_write_at(&mount, &node, buffer, 1, 100) == 1,
          "a write past the end grows the file");
    k_memset(buffer, 0x5A, sizeof(buffer));
    check(krffs_read_at(&mount, node, buffer, 32, 20) == 32, "the hole reads");
    for (i = 0; i < 32; i++) {
        if (buffer[i] != 0) {
            break;
        }
    }
    check(i == 32, "and every byte of it is zero");

    check(krffs_truncate_at(&mount, "/alpha", 4, (uint64_t *)0) == 0, "the file is truncated");
    node = krffs_walk(&mount, "/alpha");
    check(krffs_head(&mount, node, &head) == 0 && head.data_size == 4, "and is four bytes long");
    check(krffs_chain_is_valid(&mount) == 1, "the chain is sound after a truncate");

    check(krffs_truncate_at(&mount, "/alpha", 4096, (uint64_t *)0) == 0, "the file grows");
    node = krffs_walk(&mount, "/alpha");
    check(krffs_head(&mount, node, &head) == 0 && head.data_size == 4096, "to what was asked");
    check(krffs_read_at(&mount, node, buffer, 8, 64) == 8 && buffer[0] == 0,
          "and the growth is zeroes");
    check(krffs_chain_is_valid(&mount) == 1, "the chain is sound after a grow");

    check(krffs_create_at(&mount, "/beta") == 0, "a second file is created");
    other = krffs_walk(&mount, "/beta");
    check(krffs_write_at(&mount, &other, buffer, 8, 0) == 8, "and written");
    node = krffs_walk(&mount, "/alpha");
    check(krffs_write_at(&mount, &node, buffer, 8, 8192) == 8,
          "the first file grows past its neighbour");
    check(krffs_walk(&mount, "/alpha") != KRFFS_NO_NODE, "and the name still finds it");
    check(krffs_walk(&mount, "/beta") != KRFFS_NO_NODE, "and the neighbour is where it was");
    check(krffs_chain_is_valid(&mount) == 1, "the chain is sound after a relocation");
    {
        uint64_t alpha = krffs_walk(&mount, "/alpha");
        uint64_t beta = krffs_walk(&mount, "/beta");

        check(alpha != beta, "the two files are two nodes");
        check(krffs_head(&mount, alpha, &head) == 0 && head.data_size == 8200,
              "the moved file has the length the write made");
    }

    check(krffs_mkdir_at(&mount, "/dir", 0755, 0, 0) == 0, "a directory is made");
    check(krffs_mkdir_at(&mount, "/dir", 0755, 0, 0) == -1, "and not twice");
    check(krffs_create_at(&mount, "/dir/inner") == 0, "a file inside it");
    check(krffs_walk(&mount, "/dir/inner") != KRFFS_NO_NODE, "resolves");
    check(krffs_rmdir_at(&mount, "/dir") == -1, "a directory with a child is not removed");
    check(krffs_readdir_at(&mount, "/dir", 0, name, sizeof(name), &is_dir) == 1 && name[0] == 'i' &&
              is_dir == 0,
          "its one child is listed");
    check(krffs_readdir_at(&mount, "/dir", 1, name, sizeof(name), &is_dir) == 0,
          "and there is no second");
    check(krffs_node_path_at(&mount, krffs_walk(&mount, "/dir/inner"), name, sizeof(name)) == 0 &&
              name[0] == '/' && name[1] == 'd',
          "the child's path is rebuilt from its parents");
    check(krffs_unlink_at(&mount, "/dir/inner") == 0, "the child is removed");
    check(krffs_rmdir_at(&mount, "/dir") == 0, "and then the directory is");
    check(krffs_chain_is_valid(&mount) == 1, "the chain is sound after the directory rows");

    check(krffs_rename_at(&mount, "/beta", "/gamma") == 0, "a file is renamed");
    check(krffs_walk(&mount, "/beta") == KRFFS_NO_NODE, "the old name is gone");
    check(krffs_walk(&mount, "/gamma") != KRFFS_NO_NODE, "the new one is not");
    krffs_orphan_at(&mount, krffs_walk(&mount, "/gamma"));
    check(krffs_walk(&mount, "/gamma") == KRFFS_NO_NODE, "an orphan is reached by no path");
    check(krffs_reap_orphans_at(&mount) == 1, "and the reaper frees it");
    check(krffs_reap_orphans_at(&mount) == 0, "and finds nothing the next time");
    check(krffs_chain_is_valid(&mount) == 1, "the chain is sound after reaping");

    make_image(image, IMAGE_SIZE);
    check(krffs_create_at(&mount, "/spacer") == 0, "a file to place the next one");
    check(krffs_truncate_at(&mount, "/spacer", 3110, (uint64_t *)0) == 0,
          "grown so the next node's owner field falls across a block");
    check(krffs_create_at(&mount, "/across") == 0, "and the file that lands there");
    {
        uint64_t across = krffs_walk(&mount, "/across");
        krffs_node_t head;

        check(across != KRFFS_NO_NODE, "the file is there");
        check(across + KRFFS_FIELD_UID == 4093,
              "and its owner field ends one byte into the next block");
        mount.grain = 4096;
        check(krffs_set_owner_at(&mount, across, 0x01020304u, 0, KRFFS_SET_UID) != 0,
              "an owner whose every byte changes is refused");
        check(krffs_head(&mount, across, &head) == 0 && head.uid == 0,
              "and nothing of it reached the medium");
        check(krffs_set_owner_at(&mount, across, 5, 0, KRFFS_SET_UID) == 0,
              "an owner that changes one byte on one side of the line is written");
        check(krffs_head(&mount, across, &head) == 0 && head.uid == 5, "and it is there");
        mount.grain = 0;
    }

    make_image(image, IMAGE_SIZE);
    check(krffs_create_at(&mount, "/spacer") == 0, "a file to place the next one");
    check(krffs_truncate_at(&mount, "/spacer", 3375, (uint64_t *)0) == 0,
          "grown so the next node's id field falls across a block");
    check(krffs_create_at(&mount, "/across") == 0, "and the file that lands there");
    check(krffs_create_at(&mount, "/kept") == 0, "and the destination");
    {
        uint64_t across = krffs_walk(&mount, "/across");
        uint64_t kept = krffs_walk(&mount, "/kept");
        uint64_t new_id = krffs_calculate_djb_hash((const uint8_t *)"kept");
        uint64_t old_id;
        char buffer[16];
        const uint8_t *lo = (const uint8_t *)&new_id;
        int differs_below = 0, differs_above = 0, i;

        check(across != KRFFS_NO_NODE && kept != KRFFS_NO_NODE, "both are there");
        check(across + KRFFS_FIELD_ID == 4092, "and the id field straddles the block's end");
        check(krffs_write_at(&mount, &kept, "keep", 4, 0) == 4, "the destination has its bytes");
        k_memcpy(&old_id, image + across + KRFFS_FIELD_ID, sizeof(old_id));
        for (i = 0; i < 8; i++) {
            if (((const uint8_t *)&old_id)[i] != lo[i]) {
                if (i < 4) {
                    differs_below = 1;
                } else {
                    differs_above = 1;
                }
            }
        }
        check(differs_below && differs_above, "the new id changes bytes on both sides of the line");
        mount.grain = 4096;
        check(krffs_rename_at(&mount, "/across", "/kept") == 0,
              "the rename goes ahead, since nothing finds a file by its id");
        k_memcpy(&old_id, image + across + KRFFS_FIELD_ID, sizeof(old_id));
        check(krffs_walk(&mount, "/across") == KRFFS_NO_NODE &&
                  krffs_walk(&mount, "/kept") == across && old_id == new_id,
              "and the source has the name and its id");
        check(krffs_read_at(&mount, across, buffer, 4, 0) == 0, "and its own bytes, none");
        mount.grain = 0;
    }

    make_image(image, IMAGE_SIZE);
    check(krffs_mkdir_at(&mount, "/both", 0755, 0, 0) == 0, "a directory is made");
    check(krffs_create_at(&mount, "/file") == 0, "and a file beside it");
    check(krffs_truncate_at(&mount, "/file", 2048, (uint64_t *)0) == 0,
          "grown past the directory's own size");
    {
        uint64_t dir = krffs_walk(&mount, "/both");
        uint64_t file = krffs_walk(&mount, "/file");
        krffs_node_t dir_head;
        krffs_node_t file_head;

        check(dir != KRFFS_NO_NODE && file != KRFFS_NO_NODE, "both are there");
        image[file + KRFFS_FIELD_NAME + 0] = 'b';
        image[file + KRFFS_FIELD_NAME + 1] = 'o';
        image[file + KRFFS_FIELD_NAME + 2] = 't';
        image[file + KRFFS_FIELD_NAME + 3] = 'h';
        image[file + KRFFS_FIELD_NAME + 4] = '\0';
        check(krffs_repair_at(&mount, (uint32_t *)0, (uint32_t *)0, (uint32_t *)0) == 0,
              "the check repairs the volume");
        check(krffs_head(&mount, dir, &dir_head) == 0 && KRFFS_IS_DIR(dir_head.mode) &&
                  dir_head.name[0] == 'b',
              "and the directory keeps its name, whatever the sizes say");
        check(krffs_head(&mount, file, &file_head) != 0 || file_head.type != KRFFS_RESERVED_NODE ||
                  file_head.name[0] == '\0',
              "and the file that took its name lost it");
    }

    make_image(image, IMAGE_SIZE);
    {
        uint32_t before_free = krffs_free_at(&mount);
        uint64_t taken;

        check(before_free == IMAGE_SIZE - 2 * sizeof(krffs_node_t),
              "a fresh image is free but for its two headers");
        taken = krffs_allocate_at(&mount, before_free, KRFFS_NO_NODE);
        check(taken != KRFFS_NO_NODE, "the whole free node can be taken");
        check(krffs_free_at(&mount) == 0, "and nothing is free after it");
        check(krffs_chain_is_valid(&mount) == 1, "the region's last node ends at its edge");
        check(krffs_allocate_at(&mount, 1, KRFFS_NO_NODE) == KRFFS_NO_NODE,
              "a full region allocates nothing");
        krffs_remove_at(&mount, taken);
        check(krffs_free_at(&mount) == before_free, "and the space comes back");
        check(krffs_chain_is_valid(&mount) == 1, "sound again");
    }
    {
        uint32_t free_bytes = krffs_free_at(&mount);
        uint64_t taken = krffs_allocate_at(&mount, free_bytes - 8, KRFFS_NO_NODE);

        check(taken != KRFFS_NO_NODE, "a node that leaves eight bytes is taken");
        check(krffs_head(&mount, taken, &head) == 0 &&
                  head.size == IMAGE_SIZE - sizeof(krffs_node_t),
              "the remainder stays inside it rather than becoming a fragment");
        check(krffs_chain_is_valid(&mount) == 1, "and the chain is sound");
        krffs_remove_at(&mount, taken);
    }

    make_image(image, IMAGE_SIZE);
    for (i = 2 * (int)sizeof(krffs_node_t); i < (int)IMAGE_SIZE; i++) {
        image[i] = POISON;
    }
    mount = mount_over(&recording_backend, image, IMAGE_SIZE);

    k_memcpy(before, image, IMAGE_SIZE);
    recorded_count = 0;
    sync_count = 0;
    recording = 1;
    krffs_create_at(&mount, "/one");
    recording = 0;
    check(every_stop_is_sound(before, IMAGE_SIZE) && every_lost_write_is_sound(before, IMAGE_SIZE),
          "every stop inside a create leaves a chain a mount can walk");
    check(every_stop_file_is_whole(before, IMAGE_SIZE, "one", 0, 0, 0644, 0, 0),
          "and never a file half made under a name");

    k_memcpy(before, image, IMAGE_SIZE);
    recorded_count = 0;
    sync_count = 0;
    recording = 1;
    node = krffs_walk(&mount, "/one");
    krffs_write_at(&mount, &node, buffer, 512, 0);
    recording = 0;
    check(every_stop_is_sound(before, IMAGE_SIZE) && every_lost_write_is_sound(before, IMAGE_SIZE),
          "every stop inside a write leaves a chain a mount can walk");

    k_memcpy(before, image, IMAGE_SIZE);
    recorded_count = 0;
    sync_count = 0;
    recording = 1;
    krffs_truncate_at(&mount, "/one", 8192, (uint64_t *)0);
    recording = 0;
    check(every_stop_is_sound(before, IMAGE_SIZE) && every_lost_write_is_sound(before, IMAGE_SIZE),
          "every stop inside a grow leaves a chain a mount can walk");

    k_memcpy(before, image, IMAGE_SIZE);
    recorded_count = 0;
    sync_count = 0;
    recording = 1;
    krffs_truncate_at(&mount, "/one", 16, (uint64_t *)0);
    recording = 0;
    check(every_stop_is_sound(before, IMAGE_SIZE) && every_lost_write_is_sound(before, IMAGE_SIZE),
          "every stop inside a truncate leaves a chain a mount can walk");

    k_memcpy(before, image, IMAGE_SIZE);
    recorded_count = 0;
    sync_count = 0;
    recording = 1;
    krffs_truncate_at(&mount, "/one", 40, (uint64_t *)0);
    recording = 0;
    check(every_stop_is_sound(before, IMAGE_SIZE) && every_lost_write_is_sound(before, IMAGE_SIZE),
          "every stop inside a grow of less than a header leaves a chain a mount can walk");

    for (i = 0; i < 16; i++) {
        buffer[i] = (char)0xC3;
    }
    node = krffs_walk(&mount, "/one");
    krffs_write_at(&mount, &node, buffer, 16, 0);
    for (i = 0; i < (int)sizeof(buffer); i++) {
        buffer[i] = (char)((i % 127) + 1);
    }
    krffs_create_at(&mount, "/two");
    node = krffs_walk(&mount, "/two");
    krffs_write_at(&mount, &node, buffer, 16, 0);
    k_memcpy(before, image, IMAGE_SIZE);
    recorded_count = 0;
    sync_count = 0;
    recording = 1;
    node = krffs_walk(&mount, "/one");
    other = node;
    krffs_write_at(&mount, &node, buffer, 16, 4096);
    recording = 0;
    check(node != other, "the grow with a node in the way moved the file");
    check(every_stop_file_is_whole(before, IMAGE_SIZE, "one", 1, 0, 0644, 16, 0xC3),
          "and no stop inside it left the moved file without its own bytes");
    check(every_stop_is_sound(before, IMAGE_SIZE) && every_lost_write_is_sound(before, IMAGE_SIZE),
          "every stop inside a move leaves a chain a mount can walk");

    k_memcpy(before, image, IMAGE_SIZE);
    recorded_count = 0;
    sync_count = 0;
    recording = 1;
    krffs_unlink_at(&mount, "/two");
    recording = 0;
    check(every_stop_is_sound(before, IMAGE_SIZE) && every_lost_write_is_sound(before, IMAGE_SIZE),
          "every stop inside a removal leaves a chain a mount can walk");

    make_image(image, IMAGE_SIZE);
    for (i = 2 * (int)sizeof(krffs_node_t); i < (int)IMAGE_SIZE; i++) {
        image[i] = POISON;
    }
    mount = mount_over(&recording_backend, image, IMAGE_SIZE);
    krffs_create_at(&mount, "/slack");
    node = krffs_walk(&mount, "/slack");
    krffs_write_at(&mount, &node, buffer, 108, 0);
    krffs_truncate_at(&mount, "/slack", 100, (uint64_t *)0);
    node = krffs_walk(&mount, "/slack");
    check(krffs_head(&mount, node, &head) == 0 && head.data_size == 100 &&
              head.size == 2 * sizeof(krffs_node_t),
          "a node keeps the room a short grow took and a truncate of a few bytes leaves");

    k_memcpy(before, image, IMAGE_SIZE);
    recorded_count = 0;
    sync_count = 0;
    recording = 1;
    krffs_truncate_at(&mount, "/slack", 104, (uint64_t *)0);
    recording = 0;
    check(every_stop_is_sound(before, IMAGE_SIZE) && every_lost_write_is_sound(before, IMAGE_SIZE),
          "every stop inside a growth into that slack leaves a chain a mount "
          "can walk");
    check(every_stop_is_zero_filled(before, IMAGE_SIZE, 100),
          "and none of them shows a length over bytes the file used to hold");
    node = krffs_walk(&mount, "/slack");
    check(krffs_read_at(&mount, node, buffer, 4, 100) == 4 && buffer[0] == 0 && buffer[3] == 0,
          "and the bytes it grew by are zeroes, not what the file held before");

    make_image(image, IMAGE_SIZE);
    check(krffs_create_at(&mount, "/cut") == 0, "the file to be cut short is made");
    for (i = 0; i < 4096; i++) {
        buffer[i % (int)sizeof(buffer)] = (char)0x5A;
    }
    node = krffs_walk(&mount, "/cut");
    check(krffs_truncate_at(&mount, "/cut", 4096, (uint64_t *)0) == 0, "and grown");
    node = krffs_walk(&mount, "/cut");
    for (i = 0; i < 4096; i += (int)sizeof(buffer)) {
        check(krffs_write_at(&mount, &node, buffer, sizeof(buffer), (uint32_t)i) ==
                  (int)sizeof(buffer),
              "and filled with a byte of its own");
    }
    k_memcpy(before, image, IMAGE_SIZE);
    recorded_count = 0;
    sync_count = 0;
    recording = 1;
    krffs_truncate_at(&mount, "/cut", 1024, (uint64_t *)0);
    recording = 0;
    check(every_stop_is_sound(before, IMAGE_SIZE) && every_lost_write_is_sound(before, IMAGE_SIZE),
          "every stop inside a shrink leaves a chain a mount can walk");
    check(every_stop_keeps_its_bytes(before, IMAGE_SIZE, "cut", 0x5A),
          "and none of them shows the file holding the allocator's bytes");

    make_image(image, IMAGE_SIZE);
    for (i = 2 * (int)sizeof(krffs_node_t); i < (int)IMAGE_SIZE; i++) {
        image[i] = POISON;
    }
    mount = mount_over(&recording_backend, image, IMAGE_SIZE);
    krffs_create_at(&mount, "/refused");
    node = krffs_walk(&mount, "/refused");
    krffs_write_at(&mount, &node, buffer, 64, 0);
    krffs_create_at(&mount, "/box");
    k_memcpy(before, image, IMAGE_SIZE);

    {
        int which;
        int sound = 1;

        for (which = -120; which < 120; which++) {
            if (!survives_refusal(image, before, IMAGE_SIZE, grow_a_file, which) ||
                !survives_refusal(image, before, IMAGE_SIZE, remove_a_file, which) ||
                !survives_refusal(image, before, IMAGE_SIZE, make_a_file, which)) {
                printf("krffs_hosttest: a backend that refused write %d left "
                       "an image no mount could walk\n",
                       which);
                sound = 0;
                break;
            }
        }
        check(sound, "a refused write never leaves a chain a mount cannot walk");
        k_memcpy(image, before, IMAGE_SIZE);
    }

    if (fails == 0) {
        printf("krffs_hosttest: OK\n");
    }
    return fails > 255 ? 255 : fails;
}

/** Run the full fixture or just the disk failure cases. */
int main(int argc, char **argv)
{
    disk_failure_rows();
    if (argc == 2 && __builtin_strcmp(argv[1], "failures") == 0) {
        printf("krffs-hosttest: failure rows: %d failures\n", fails);
        return fails != 0;
    }
    return image_rows();
}
