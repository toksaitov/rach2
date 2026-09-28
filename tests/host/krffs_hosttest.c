/**
 * krffs_hosttest.c: the file system's core over a backend of its own.
 *
 * host-deps: rach2/sys/lib/kstring.c
 * host-deps: rach2/sys/lib/kctypes.c
 *
 * The core addresses nodes by offset and reaches the medium through a
 * backend, so a test can put its own backend under it. Two are used here: a
 * plain memory image, and the same image behind a recorder that keeps every
 * write in order. The rows are the format's contract: what the mount check
 * accepts and what it refuses, the allocator's split, merge and relocation,
 * the ends of the region, a hole read as zeroes, the directory operations,
 * the orphan reaping, and the order of writes.
 *
 * The order rows replay every prefix of the writes an operation makes, which
 * is what a machine stopping in the middle of one leaves on the medium. Each
 * prefix must leave a forward chain that closes with every node sound; the
 * backward link may lag, since it is derived and the check at the next mount
 * rebuilds it. That is the rule krffs.Readme.md states, and these rows are
 * what say the code keeps it.
 *
 * No hosted headers: the kernel headers define the fixed-width types, and the
 * libc calls used here are declared by hand the way the other host tests
 * declare them.
 */

#include "../../rach2/sys/fs/krffs.c"

#include "krffs_hash_vectors.h"

extern int printf(const char *fmt, ...);
extern void *malloc(unsigned long size);
extern void free(void *p);

/* ---- what krffs.c reaches outside itself ---- */

void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

void console_printf(const char *format, ...)
{
    (void)format;
}

static int64_t fake_seconds = 1000;
static int64_t fake_nanoseconds;

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

static int memory_read(void *ctx, uint64_t offset, void *buffer, uint32_t size)
{
    k_memcpy(buffer, (uint8_t *)ctx + offset, size);
    return 0;
}

static int memory_write(void *ctx, uint64_t offset, const void *buffer, uint32_t size)
{
    k_memcpy((uint8_t *)ctx + offset, buffer, size);
    return 0;
}

static int memory_sync(void *ctx)
{
    (void)ctx;
    return 0;
}

static const krffs_backend_t memory_backend = {memory_read, memory_write, memory_sync};

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
/* Which write the backend refuses, counted from the start of an operation, or
   -1 for a backend that refuses none. One write and not every write after it:
   a device that refuses a block and takes the next is what leaves a change
   published over a write that never landed. */
static int refused_write = -1;
static int writes_seen;

static int record_write(void *ctx, uint64_t offset, const void *buffer, uint32_t size)
{
    if (writes_seen++ == refused_write) {
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

/* A sync the medium refuses, counted with the writes so a row can refuse
   either. */
static int refused_sync = -1;

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

static const krffs_backend_t recording_backend = {memory_read, record_write, record_sync};

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

static krffs_mount_t mount_over(const krffs_backend_t *backend, void *ctx, uint32_t size)
{
    krffs_mount_t mount;

    mount.backend = backend;
    mount.ctx = ctx;
    mount.size = size;
    mount.grain = 0; /* a memory image has no unit it reaches in pieces */
    return mount;
}

/**
 * Walk the forward chain, asking of every node what the check asks except the
 * backward link, which an interrupted operation is allowed to leave behind.
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

/** The distinct cache blocks the writes in [from, to) touched. */
static int touched_blocks(int from, int to, uint64_t *seen, int cap)
{
    int count = 0;
    int i;

    for (i = from; i < to; i++) {
        uint64_t first = recorded[i].offset / CACHE_BLOCK;
        uint64_t last = (recorded[i].offset + recorded[i].size - 1) / CACHE_BLOCK;
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
 * What every state a stop can leave must answer: a forward chain that closes,
 * no file claiming a byte nobody wrote, and the whole check once the backward
 * links are rebuilt from that chain, which is what the check at the next
 * mount does.
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
    /* What the next mount does with it, and then what that mount requires. */
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
        int seen_count = touched_blocks(mark, prefix, seen, 64);
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
 * Every state a stop can leave, asked whether the file `name` is whole.
 *
 * Whole means what a name promises: the directory it is in, the mode it was
 * made with, a length of at least `bytes`, and those bytes holding `value`. A
 * node that is half made must not be reachable by a name, so a state may have
 * no such node at all unless `must_exist`, which is what a move requires: the
 * file it moves is there before, during and after.
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
        int seen_count = touched_blocks(mark, prefix, seen, 64);
        int variant;

        build(epoch, before, mark, size);
        /* -1 is the state that lost nothing; then each block since the last
           sync, and then each write since it, which is the stricter rule the
           order between fields asks for. */
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
                continue; /* the row that asks for a chain judges this */
            }
            /* Judged after the check at the next mount has had it, since that
               is what a machine coming back up would read. */
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
 * Every stop, asked whether the file called `name` ever holds, inside the
 * length it publishes, a byte other than the one it was written with: what a
 * shrink that put the surplus's free header on the medium before the shorter
 * length looks like to a reader with the old length in hand.
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
        int seen_count = touched_blocks(mark, prefix, seen, 64);
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
 * Every stop, asked whether a file ever claims a byte past `old_length` that
 * is not a zero: what a length published before the zeroes it counts looks
 * like when a file grows into slack it already had.
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
        int seen_count = touched_blocks(mark, prefix, seen, 64);
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
 * Run `what` with the backend refusing one of its calls, and judge what the
 * file system was left with.
 *
 * A non-negative `which` refuses that write; a negative one refuses the sync
 * at that position, since a medium can refuse either. The core must not
 * publish a change over a call that was refused, and what it leaves must be
 * an image a mount can walk with no file half made under a name.
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
        refused_sync = -which - 1; /* a refused sync, counted the same way */
    }
    answer = what(&mount);
    refused_write = -1;
    refused_sync = -1;

    if (!forward_chain_closes(&mount)) {
        return 0;
    }
    /* Before the check has had it, and only of an operation that says it
       worked: a move that could not free the node it copied from would leave
       two nodes under one name, and a caller told the operation succeeded
       would read whichever the walk reaches first. */
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
            /* A name is only on a node that is finished: the file the row
               makes is in the root, with the mode it was made with. */
            if (k_strcmp_name(head.name, "second") &&
                (head.parent_id != 0 || (head.mode & 07777u) != 0644)) {
                return 0;
            }
        }
        offset = krffs_next_offset(&mount, offset, &head);
    } while (offset != 0 && ++steps < krffs_max_steps(&mount));
    return krffs_chain_is_valid(&mount);
}

/* The three operations the refusal sweep runs, each answering what the core
   answered: an operation that reports success has made its change, and the
   sweep holds it to that. */
static int make_a_file(krffs_mount_t *mount)
{
    /* A name the image does not have, so the allocator really runs: creating
       one that exists answers success without taking a node. */
    return krffs_create_at(mount, "/second");
}

static int grow_a_file(krffs_mount_t *mount)
{
    return krffs_truncate_at(mount, "/refused", 3000, (uint64_t *)0);
}

static int remove_a_file(krffs_mount_t *mount)
{
    return krffs_unlink_at(mount, "/refused");
}

/** Check the same fixed hashes as the host image stager. */
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
 * as one file, which merged every account's `authorized_keys` when an update
 * copied the kept homes on the recovery system. A directory keeps its id.
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
    check(krffs_node_ino(&a) == krffs_mix_id(a.parent_id, (const char *)a.name),
          "a file's inode number is its parent's id mixed with its name");
    memcpy(dir.name, ".ssh", 5);
    dir.mode = KRFFS_S_IFDIR | 0700;
    dir.id = 424242;
    check(krffs_node_ino(&dir) == 424242, "a directory's inode number is its id");
}

/**
 * Open files retain their nodes after unlink, so test the orphan operation
 * that leaves them allocated. Root walks must ignore its missing type bits.
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
 * and the rule is not asked of them: a create whose free node put the id
 * astride a unit was refused before (entry 15 of milestone 50). The spacer
 * puts the next node's id at 4092, four of its bytes on each side of the
 * line, and the new name's hash changes bytes on both sides.
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

    /* A directory is a node being made in the same way, and its id is the
       mix of its parent's and its name, which children name it by. */
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
 * Refuse a directory whose identifier another directory already has.
 *
 * A directory's identifier is its parent's identifier mixed with the DJB
 * hash of its name, and a child names its parent by that identifier alone.
 * `bC` and `cb` hash alike, so a second directory of the two in one parent
 * would share the first one's children (entry 125 of milestone 50). The
 * first is made and given a child; the second must be refused with
 * `KRFFS_ETAKEN`, a result of its own that the adapters report as EINVAL and
 * not as a full or a failing volume (entry 243), the first must keep its
 * child, and the refused name must reach nothing.
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

/* The fields of a header whose values a node is made or moved with: the
   mode, the owner, the group, the three times and the length. */
static const uint32_t across_fields[] = {KRFFS_FIELD_MODE,     KRFFS_FIELD_UID,   KRFFS_FIELD_GID,
                                         KRFFS_FIELD_ATIME,    KRFFS_FIELD_MTIME, KRFFS_FIELD_CTIME,
                                         KRFFS_FIELD_DATA_SIZE};

/** Whether the node at `at` has `mode`, the owner and group 1000, and the
 * clock's moment in all three times. */
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

/** Grow `/spacer` so that the node after it starts at `at`, with `gap` more
 * bytes of it before that node. */
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
 * is, so their fields are written without the rule (entry 244 of milestone
 * 50): a file and a directory must be made at each place with every field
 * they were given, a node must be made over a removed node whose length
 * crosses the line, and a file that has to move to each place must move
 * there with its fields and its bytes.
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

        /* The spacer holds the place and 2000 bytes after it while the file
           and a node after the file are made; cut back, it leaves a free
           node with a fresh header at the place, and the file, which cannot
           grow into the node after it, has to move there. */
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

    /* A node made where a removed file left a length whose first two bytes
       are on the two sides of the line: the new node's length of 0 changes
       both. */
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

int main(void)
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
    across_rows();

    /* ---- the mount check ---- */

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

        /* On a node that has data: the second node of a fresh image is the
           free one, and a free node keeps the length of whatever was removed
           there, which the allocator rewrites when it takes it. */
        krffs_create_at(&mount, "/data.bin");
        k_memcpy(image + sizeof(krffs_node_t) + KRFFS_FIELD_DATA_SIZE, &data, sizeof(data));
        check(krffs_chain_is_valid(&mount) == 0, "data larger than its node is refused");
        make_image(image, IMAGE_SIZE);
    }
    {
        uint64_t data = IMAGE_SIZE;

        /* And not on one that has none: a free node whose length is larger
           than the node is what a split leaves between the size it publishes
           and the length it writes after it. */
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
        /* A name that fills its field has no end, and everything that reads
           a name would read past it. */
        uint64_t offset = sizeof(krffs_node_t);

        image[offset + KRFFS_FIELD_TYPE] = KRFFS_RESERVED_NODE;
        for (i = 0; i < KRFFS_FILE_NAME_BUFFER_SIZE; i++) {
            image[offset + KRFFS_FIELD_NAME + i] = 'x';
        }
        check(krffs_chain_is_valid(&mount) == 0, "a name that never ends is refused");
        make_image(image, IMAGE_SIZE);
    }

    /* A size no region could hold is refused before the arithmetic that adds
       a header to it can wrap. */
    check(krffs_allocate_at(&mount, (uint64_t)-1, KRFFS_NO_NODE) == KRFFS_NO_NODE,
          "a size larger than the region allocates nothing");
    check(krffs_allocate_at(&mount, IMAGE_SIZE, KRFFS_NO_NODE) == KRFFS_NO_NODE,
          "and neither does one exactly the region's size");
    check(krffs_chain_is_valid(&mount) == 1, "and the chain is what it was before both");

    /* ---- the operations ---- */

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

    /* A write past the end leaves a hole, and a hole reads as zeroes. */
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

    /* A grow with a node in the way relocates, and the contents move. */
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

    /* Directories. */
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

    /* A rename moves the name, and an orphan is a node no path reaches. */
    check(krffs_rename_at(&mount, "/beta", "/gamma") == 0, "a file is renamed");
    check(krffs_walk(&mount, "/beta") == KRFFS_NO_NODE, "the old name is gone");
    check(krffs_walk(&mount, "/gamma") != KRFFS_NO_NODE, "the new one is not");
    krffs_orphan_at(&mount, krffs_walk(&mount, "/gamma"));
    check(krffs_walk(&mount, "/gamma") == KRFFS_NO_NODE, "an orphan is reached by no path");
    check(krffs_reap_orphans_at(&mount) == 1, "and the reaper frees it");
    check(krffs_reap_orphans_at(&mount) == 0, "and finds nothing the next time");
    check(krffs_chain_is_valid(&mount) == 1, "the chain is sound after reaping");

    /* ---- a field the medium would reach in two pieces ---- */

    /* A backend that puts whole blocks on the medium writes a field that
       spans two of them twice, and a stop between the two leaves a value
       nobody wrote. The core refuses such a write. Only the bytes that
       change count: a field whose changed byte is on one side of the line is
       written like any other. The node below is placed so that its owner
       field ends one byte into the second block. */
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

    /* ---- a rename the whole-unit rule refuses changes nothing ---- */

    /* The id field of a node placed across a block, and a rename over an
       existing name: the rule refuses the id, and it must refuse before
       the destination is removed, or a refused rename leaves one name where
       there were two. The spacer's size puts the next node's id at 4092,
       four of its bytes on each side of the line. */
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
        check(krffs_rename_at(&mount, "/across", "/kept") != 0,
              "the rename over the name is refused");
        check(krffs_walk(&mount, "/across") == across, "and the source keeps its name");
        check(krffs_walk(&mount, "/kept") == kept, "and the destination keeps its name");
        check(krffs_read_at(&mount, kept, buffer, 4, 0) == 4 && buffer[0] == 'k' &&
                  buffer[3] == 'p',
              "and its bytes");
        mount.grain = 0;
    }

    /* ---- one name on two nodes, when one of them is a directory ---- */

    /* A rename that published its new parent and stopped before its new name
       leaves the file it moved beside whatever already had that name there.
       The repair keeps the larger, which is the copy a move was making, but
       never a directory's name: its children name it by its id, and clearing
       it would put a whole tree out of reach. */
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

    /* ---- the ends of the region ---- */

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
        /* A remainder too small for a header of its own stays in the node. */
        uint32_t free_bytes = krffs_free_at(&mount);
        uint64_t taken = krffs_allocate_at(&mount, free_bytes - 8, KRFFS_NO_NODE);

        check(taken != KRFFS_NO_NODE, "a node that leaves eight bytes is taken");
        check(krffs_head(&mount, taken, &head) == 0 &&
                  head.size == IMAGE_SIZE - sizeof(krffs_node_t),
              "the remainder stays inside it rather than becoming a fragment");
        check(krffs_chain_is_valid(&mount) == 1, "and the chain is sound");
        krffs_remove_at(&mount, taken);
    }

    /* ---- the order of writes ---- */

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

    /* A growth shorter than a header, which is the case the grow publishes two
       sizes for: the tail's header lands where the free node's own header was,
       so the first size must be on the medium before it is written. */
    k_memcpy(before, image, IMAGE_SIZE);
    recorded_count = 0;
    sync_count = 0;
    recording = 1;
    krffs_truncate_at(&mount, "/one", 40, (uint64_t *)0);
    recording = 0;
    check(every_stop_is_sound(before, IMAGE_SIZE) && every_lost_write_is_sound(before, IMAGE_SIZE),
          "every stop inside a grow of less than a header leaves a chain a mount can walk");

    /* A move: a second file boxes the first in, so the grow relocates it.
       The file to be moved is filled with a byte of its own, so a copy that
       did not reach the medium is visible in what the moved node holds. */
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

    /* ---- a length raised inside the slack a node already has ---- */

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
              head.size == sizeof(krffs_node_t) + 108,
          "a node keeps the slack a truncate of a few bytes leaves");

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

    /* A shrink that leaves a surplus large enough for a header of its own:
       that header is written inside the bytes the length still on the medium
       counts, so the shorter length must be there first. The file is filled
       with a byte of its own, and no stop may show it holding anything else
       inside whatever length it publishes. */
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

    /* ---- a backend that refuses a write ---- */

    make_image(image, IMAGE_SIZE);
    for (i = 2 * (int)sizeof(krffs_node_t); i < (int)IMAGE_SIZE; i++) {
        image[i] = POISON;
    }
    mount = mount_over(&recording_backend, image, IMAGE_SIZE);
    krffs_create_at(&mount, "/refused");
    node = krffs_walk(&mount, "/refused");
    krffs_write_at(&mount, &node, buffer, 64, 0);
    /* A second file right behind the first, so the grow below has no room
       after it and must copy the file elsewhere and free the node it came
       from. That freeing is a write like any other and the sweep refuses it
       in its turn. */
    krffs_create_at(&mount, "/box");
    k_memcpy(before, image, IMAGE_SIZE);

    {
        int which;
        int sound = 1;

        /* Every call of each operation, in both kinds: a grow that must move
           the file writes its bytes 64 at a time and then frees the node it
           came from, so the count runs well past the fields; a call that
           never happens refuses nothing. */
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
