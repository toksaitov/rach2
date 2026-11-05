/**
 * ext2_reserve_hosttest.c: the blocks a write of rach2/sys/fs/ext2.c
 * reserves: the count it needs, pointer blocks included, decided before any
 * byte is taken, and the reserved blocks kept from a caller who is not root;
 * and what the batch and the allocator read and write under em->meta.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: rach2/sys/fs/sleeplock.c
 *
 * The test includes ext2.c whole. The mount, a group of 64 blocks and an
 * inode are built in memory, over a cache model with one buffer for each
 * block, so that the writer runs without a volume made by mke2fs and
 * without the mount's check, whose calls panic here. The model can say a
 * block is not cached, and keep saying so for a number of reads, as a cache
 * that evicts a block again after its fetch would.
 */
#include "fs/vfs.h"
#include "fs/ext2.c"

extern int memcmp(const void *a, const void *b, unsigned long size);
extern int printf(const char *format, ...);
extern void exit(int status);
extern void free(void *ptr);
extern void *malloc(unsigned long size);
extern void *calloc(unsigned long count, unsigned long size);

#define TEST_BLOCKS 64u
static uint8_t disk[TEST_BLOCKS][4096], saved[TEST_BLOCKS][4096];
static uint8_t storage[(EXT2_WB_DATA + EXT2_WB_META) * 4096];
static struct bbuf buffers[TEST_BLOCKS];
static struct ext2_mount volume;
static struct ext2_writeback batch;
static struct ext2_node file, other;
static struct vfs_mount mount;
static block_device_t device;
static uint8_t descriptors[32], payload[8192], result[8192];
static process_t process;
struct percpu cpu_table[NR_CPUS] = {{.current = &process}};
static int failures, writes;

/* The cache model's state: whether each block is missing from the cache, and
 * how many more reads leave it missing; the misses plans met; the order
 * class each block was last dirtied with and the dirty marks without one;
 * the calls of `bcache_reserve`, the buffers the last asked for, and the
 * event count at it and at the first blank get after it; and a hook run
 * once at the next read of a missing block. */
static int uncached[TEST_BLOCKS], refetch[TEST_BLOCKS];
static int misses;
static int order_of[TEST_BLOCKS], unordered[TEST_BLOCKS];
static int reserve_calls;
static uint32_t reserve_n;
static int events, reserve_at, blank_at;
static void (*fetch_hook)(void);

/** Count and print a failed row. */
static void check(int ok, const char *what)
{
    if (!ok) {
        printf("ext2-reserve-hosttest: fail: %s\n", what);
        failures++;
    }
}

/** Print the panic's message and exit with status 2. */
void k_panic(const char *what)
{
    printf("ext2-reserve-hosttest: panic: %s\n", what);
    exit(2);
}

/** Drop the record. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

/** A fixed time, one second after the epoch. */
void rtc_realtime(int64_t *seconds, int64_t *nanoseconds)
{
    *seconds = 1;
    *nanoseconds = 0;
}

/** Free `ptr`; a free of the test's own inode fails a row and is skipped. */
void kfree(void *ptr)
{
    check(ptr != &file, "writer retains the caller's inode reference");
    if (ptr != &file) {
        free(ptr);
    }
}

/** Allocate from the C library. */
void *kmalloc(size_t size)
{
    return malloc(size);
}

/** Allocate zeroed memory from the C library. */
void *kzalloc(size_t size)
{
    return calloc(1, size);
}

/**
 * Allocate `count` pages from the C library, with no physical address. The
 * rows set the batch's own pages in `reset`.
 */
void *kalloc_pages(uint32_t count, uintptr_t *phys)
{
    if (phys) {
        *phys = 0;
    }
    return malloc(4096ul * count);
}

/** Free pages `kalloc_pages` allocated. */
void kfree_pages(void *p, uint32_t count)
{
    (void)count;
    free(p);
}

/** Panic: the mount's check, whose workspace this is, is outside the rows. */
int fat_walk_acquire(uint32_t count)
{
    (void)count;
    k_panic("unexpected mount check");
    return 0;
}

/** Panic, as `fat_walk_acquire` does. */
uint32_t fat_walk_bytes(uint32_t count)
{
    (void)count;
    k_panic("unexpected mount check");
    return 0;
}

/** Panic, as `fat_walk_acquire` does. */
void fat_walk_release(void)
{
    k_panic("unexpected mount check");
}

/** Panic, as `fat_walk_acquire` does. */
int fat_walk_test_set(uint32_t block)
{
    (void)block;
    k_panic("unexpected mount check");
    return 0;
}

/** Panic, as `fat_walk_acquire` does. */
int fat_walk_marked(uint32_t block)
{
    (void)block;
    k_panic("unexpected mount check");
    return 0;
}

/** Clear a wait queue. */
void wait_queue_init(wait_queue_t *wq)
{
    k_memset(wq, 0, sizeof(*wq));
}

/** Do nothing: no process sleeps in this test. */
void wq_wake_all(wait_queue_t *wq)
{
    (void)wq;
}

/** Fail the test: a wait here would never end, since no other process runs. */
int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    (void)cond;
    (void)arg;
    check(0, "nothing sleeps");
    return -1;
}

/** Return 1: every buffer of this test is present. */
int vm_resident(uintptr_t addr, uintptr_t len, int write)
{
    (void)addr;
    (void)len;
    (void)write;
    return 1;
}

/** Return 1: nothing here is ever absent. */
int vm_prefault_range(uintptr_t addr, uintptr_t len, int write)
{
    (void)addr;
    (void)len;
    (void)write;
    return 1;
}

/** Return 0: no hold of a sleeping lock here is timed. */
uint32_t timer_now_us(void)
{
    return 0;
}

/** Take the namespace lock for the test's process, recursively. */
void vfs_ns_lock(struct vfs_mount *mnt)
{
    mnt->ns.holder = (int)process.pid;
    mnt->ns.depth++;
}

/** Release one level of the namespace lock; a release with none held fails. */
void vfs_ns_unlock(struct vfs_mount *mnt)
{
    check(mnt->ns.depth > 0, "namespace lock is balanced");
    if (--mnt->ns.depth == 0) {
        mnt->ns.holder = -1;
    }
}

/** Whether block `block` is in the cache, as the model says; a block it
 * lacks asked by the planner counts in `misses`. */
int bcache_cached(block_device_t *dev, uint64_t block)
{
    (void)dev;
    if (block < TEST_BLOCKS && uncached[block]) {
        misses += volume.planner == (int)process.pid;
        return 0;
    }
    return 1;
}

/**
 * Block `block` of the volume in memory; a block outside it fails a row and
 * gives NULL. A read of a block the model calls missing fails a row when
 * the caller plans, runs `fetch_hook` once, and leaves the block missing
 * for `refetch` more reads.
 */
struct bbuf *bcache_get(block_device_t *dev, uint64_t block)
{
    check(dev == &device && block < TEST_BLOCKS, "cache access is in the volume");
    if (block >= TEST_BLOCKS) {
        return NULL;
    }
    if (uncached[block]) {
        void (*hook)(void) = fetch_hook;

        check(volume.planner != (int)process.pid, "a plan reads no block the cache lacks");
        if (refetch[block] > 0) {
            refetch[block]--;
        } else {
            uncached[block] = 0;
        }
        fetch_hook = 0;
        if (hook) {
            hook();
        }
    }
    return &buffers[block];
}

/** As `bcache_get`, for a block the writer overwrites whole; the first after
 * a `bcache_reserve` is noted in `blank_at`. */
struct bbuf *bcache_get_blank(block_device_t *dev, uint64_t block)
{
    if (reserve_at && !blank_at) {
        blank_at = ++events;
    }
    return bcache_get(dev, block);
}

/** Note the call and the buffers asked for; the model has every one. */
uint32_t bcache_reserve(block_device_t *dev, uint32_t n)
{
    (void)dev;
    reserve_calls++;
    reserve_n = n;
    reserve_at = ++events;
    return n;
}

/** Do nothing: the model evicts no block. */
void bcache_release(struct bbuf *buffer)
{
    (void)buffer;
}

/** Count a write to the cache in `writes`, and one without an order class
 * in `unordered`. */
void bcache_dirty(struct bbuf *buffer)
{
    writes++;
    unordered[buffer - buffers]++;
}

/** Count the write and note its block's class in `order_of`. */
void bcache_dirty_ordered(struct bbuf *buffer, int order)
{
    writes++;
    order_of[buffer - buffers] = order;
}

/** Count the write, which succeeds. */
int bcache_write(struct bbuf *buffer)
{
    bcache_dirty(buffer);
    return 0;
}

/** Count the write, which succeeds. */
int bcache_write_unflushed(struct bbuf *buffer)
{
    bcache_dirty(buffer);
    return 0;
}

/** Succeed: the model has nothing to sync. */
int bcache_sync(block_device_t *dev)
{
    (void)dev;
    return 0;
}

/** Succeed: the model has nothing to sync. */
int bcache_sync_unflushed(block_device_t *dev)
{
    (void)dev;
    return 0;
}

/** Return 0: the rows never fill the cache with dirty buffers. */
int bcache_dirty_heavy(void)
{
    return 0;
}

/** Succeed: the model's device has no cache. */
int block_flush(block_device_t *dev)
{
    (void)dev;
    return 0;
}

/**
 * Build the volume afresh: one group of 64 blocks of 4 KiB, of which the
 * last `available` are free; its descriptor names the block bitmap at
 * block 2, the inode bitmap at 3 and the inode table at 4; 8 blocks are
 * reserved for root. The batch is empty, the file is inode 12, a regular
 * file of mode 0600 and no blocks, and the caller has euid 1000.
 */
static void reset(uint32_t available)
{
    uint32_t i;

    k_memset(disk, 0, sizeof(disk));
    k_memset(&volume, 0, sizeof(volume));
    k_memset(&file, 0, sizeof(file));
    k_memset(&other, 0, sizeof(other));
    k_memset(uncached, 0, sizeof(uncached));
    k_memset(refetch, 0, sizeof(refetch));
    k_memset(order_of, 0, sizeof(order_of));
    k_memset(unordered, 0, sizeof(unordered));
    misses = reserve_calls = 0;
    reserve_n = 0;
    events = reserve_at = blank_at = 0;
    fetch_hook = 0;
    k_memset(&mount, 0, sizeof(mount));
    k_memset(&batch, 0, sizeof(batch));
    k_memset(descriptors, 0, sizeof(descriptors));
    for (i = 0; i < TEST_BLOCKS; i++) {
        buffers[i].data = disk[i];
        buffers[i].bytes = 4096;
        if (i < TEST_BLOCKS - available) {
            disk[2][i / 8] |= (uint8_t)(1u << (i % 8));
        }
    }
    LE32_PUT(descriptors, 2);
    LE32_PUT(descriptors + 4, 3);
    LE32_PUT(descriptors + 8, 4);
    LE16_PUT(descriptors + 12, available);
    k_memcpy(disk[1], descriptors, sizeof(descriptors));
    volume.dev = &device;
    volume.mnt = &mount;
    volume.block_size = 4096;
    volume.block_shift = 12;
    volume.file_limit = (uint64_t)1 << 41;
    volume.blocks = volume.bpg = TEST_BLOCKS;
    volume.ipg = volume.inodes = 32;
    volume.groups = 1;
    volume.inode_size = 128;
    volume.gd = descriptors;
    volume.free_blocks = available;
    volume.r_blocks = 8;
    volume.writable = 1;
    volume.ro_compat = EXT2_RO_LARGE_FILE;
    volume.wb = &batch;
    for (i = 0; i < EXT2_WB_DATA + EXT2_WB_META; i++) {
        batch.slot[i] = storage + i * 4096;
    }
    mount.priv = &volume;
    mount.ns.holder = -1;
    em_locks_init(&volume);
    volume.alloc_hint = 0;
    file.ino = 12;
    file.mode = S_IFREG_ | 0600;
    file.refs = file.nlink = 1;
    other.ino = 13;
    other.mode = S_IFREG_ | 0640;
    other.refs = other.nlink = 1;
    process.euid = 1000;
    process.pid = 1;
    writes = 0;
    k_memset(payload, 0x6b, sizeof(payload));
}

/**
 * Write two blocks from each start in `rows`, which crosses a boundary
 * between pointer levels or between indirect blocks and needs `needed`
 * blocks, data and pointer blocks together. With one block too few above
 * the 8 reserved, a caller who is not root gets `ENOSPC` with nothing
 * written, counted or changed, and root may take a reserved block. With
 * exactly enough, the write takes them all, each missing pointer block
 * counted once, and reads back, and an overwrite needs no free block.
 */
static void boundary_rows(void)
{
    static const struct {
        uint32_t first, needed;
    } rows[] = {{11, 3},      {1035, 5},           {2059, 5},
                {1049611, 7}, {1049612 + 1023, 6}, {1049612 + 1048575, 7}};
    unsigned i;

    for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        uint64_t offset = (uint64_t)rows[i].first * 4096;
        struct ext2_node before;
        int rc;

        reset(8 + rows[i].needed - 1);
        before = file;
        k_memcpy(saved, disk, sizeof(saved));
        rc = ext2_write(&mount, &file, payload, sizeof(payload), offset);
        check(rc == -ENOSPC && writes == 0 && volume.free_blocks == 8 + rows[i].needed - 1 &&
                  memcmp(&before, &file, sizeof(file)) == 0 &&
                  memcmp(saved, disk, sizeof(saved)) == 0 && batch.count == 0,
              "boundary shortage is refused before accepting bytes");
        check(volume.free_blocks >= volume.r_blocks,
              "unprivileged boundary write preserves reserved blocks");
        process.euid = 0;
        check(ext2_write(&mount, &file, payload, sizeof(payload), offset) == sizeof(payload) &&
                  volume.free_blocks == 7,
              "root may allocate the reserved boundary block");

        reset(8 + rows[i].needed);
        check(ext2_write(&mount, &file, payload, sizeof(payload), offset) == sizeof(payload) &&
                  volume.free_blocks == 8 && file.blocks == rows[i].needed * 8,
              "boundary exact quota counts each missing pointer once");
        check(read_data(&volume, &file, offset, result, sizeof(result)) == 0 &&
                  memcmp(result, payload, sizeof(result)) == 0,
              "accepted boundary bytes read back");
        check(ext2_write(&mount, &file, payload, sizeof(payload), offset) == sizeof(payload),
              "allocated overwrites need no free blocks");
    }
}

/**
 * Pointer blocks that exist, or are pending in the batch, are not counted
 * again: with the double-indirect block on the volume, two blocks into a
 * new leaf need three; and an append after one that left the
 * double-indirect block pending needs two.
 */
static void existing_rows(void)
{
    reset(20);
    check(ext2_write(&mount, &file, payload, 4096, (uint64_t)2059 * 4096) == 4096,
          "create the double-indirect root and one leaf");
    volume.r_blocks = (uint32_t)volume.free_blocks - 3;
    check(ext2_write(&mount, &file, payload, 8192, (uint64_t)2060 * 4096) == 8192 &&
              volume.free_blocks == volume.r_blocks,
          "existing root and missing leaf use exactly three blocks");

    reset(20);
    file.size = (uint64_t)2059 * 4096;
    check(ext2_write(&mount, &file, payload, 4096, file.size) == 4096 && batch.count == 1,
          "append retains a pending pointer path");
    volume.r_blocks = (uint32_t)volume.free_blocks - 2;
    check(ext2_write(&mount, &file, payload, 4096, file.size) == 4096 &&
              volume.free_blocks == volume.r_blocks && batch.count == 1,
          "pending root is counted once across append calls");
}

/**
 * Call `writeback_piece`, the batch's admission of a piece, directly. A
 * piece that needs more blocks than are free once an earlier pending
 * piece's reservation is counted fails with `ENOSPC` and leaves the earlier
 * piece pending and intact; it then drains and reads back. Root cannot
 * reserve more blocks than are free.
 */
static void reservation_rows(void)
{
    struct ext2_node before;
    uint32_t remaining;
    int metas, datas;

    reset(11);
    vfs_ns_lock(&mount);
    check(writeback_piece(&volume, &file, 0, payload, 4096) == 0, "accept an earlier piece");
    before = file;
    remaining = (uint32_t)volume.free_blocks;
    metas = batch.metas;
    datas = batch.datas;
    check(writeback_piece(&volume, &file, (uint64_t)1049612 * 4096, payload, 4096) == -ENOSPC,
          "piece quota includes uncommitted reservations");
    check(volume.free_blocks == remaining && memcmp(&before, &file, sizeof(file)) == 0 &&
              batch.metas == metas && batch.datas == datas && batch.count == 1 && writes == 0,
          "refused piece preserves the earlier pending piece");
    check(writeback_drain(&volume) == 0 && read_data(&volume, &file, 0, result, 4096) == 0 &&
              memcmp(result, payload, 4096) == 0,
          "earlier piece survives a refused reservation");
    vfs_ns_unlock(&mount);

    reset(2);
    process.euid = 0;
    vfs_ns_lock(&mount);
    check(writeback_piece(&volume, &file, (uint64_t)1036 * 4096, payload, 4096) == -ENOSPC &&
              volume.free_blocks == 2 && batch.count == 0,
          "root cannot reserve more blocks than are free");
    vfs_ns_unlock(&mount);
}

/** Whether block `b`'s bit is set in the model disk's block bitmap. */
static int bit_on(uint32_t b)
{
    return (disk[2][b / 8] >> (b % 8)) & 1u;
}

/**
 * A plan under em->meta reads no block the cache lacks. The bitmap is
 * fetched before the plan and gone again when the plan asks for it: the
 * piece gives back the image it took, fetches the bitmap with the lock let
 * go and starts again, once, and ends with one image of each block. A
 * bitmap the cache keeps losing is met PLAN_TRIES times, and then the plan
 * reads it under the lock, one request.
 */
static void miss_rows(void)
{
    reset(20);
    uncached[2] = 1;
    refetch[2] = 1;
    check(ext2_write(&mount, &file, payload, 4096, 0) == 4096, "a piece whose bitmap is evicted");
    check(misses == 1, "meets the miss once and restarts");
    check(batch.metas == 3 && batch.datas == 1 && batch.count == 1,
          "and keeps one image of each block it took");
    check(writeback_drain(&volume) == 0 && bit_on(TEST_BLOCKS - 20) &&
              read_data(&volume, &file, 0, result, 4096) == 0 && memcmp(result, payload, 4096) == 0,
          "and the restarted piece drains and reads back");

    reset(20);
    uncached[2] = 1;
    refetch[2] = 1000;
    check(ext2_write(&mount, &file, payload, 4096, 0) == 4096 && misses == PLAN_TRIES,
          "a bitmap the cache keeps losing is met PLAN_TRIES times");
    check(batch.metas == 3 && writeback_drain(&volume) == 0 &&
              read_data(&volume, &file, 0, result, 4096) == 0 && memcmp(result, payload, 4096) == 0,
          "and the last plan reads it under the lock");
}

/**
 * A write into a block the batch holds reaches the batch's image too, so
 * that the drain, which copies the image over the cache's block, keeps it:
 * another inode stored into the pending inode's table block, and a block
 * allocated beside the batch's reservation in its bitmap and descriptor.
 */
static void held_block_rows(void)
{
    uint32_t b = 0;
    uint64_t at = (uint64_t)(other.ino - 1) * 128;
    struct ext2_image *im;

    reset(20);
    check(ext2_write(&mount, &file, payload, 4096, 0) == 4096 && batch.count == 1,
          "a pending piece holds the inode table's block");
    other.size = 77;
    vfs_ns_lock(&mount);
    check(inode_store(&volume, &other) == 0, "another inode of that block is stored");
    im = writeback_find(&volume, 4);
    check(im && le16(im->data + at) == other.mode && le32(im->data + at + 4) == 77,
          "into the batch's image as well as the cache");
    check(alloc_block(&volume, volume.alloc_hint, &b) == 0 && b == TEST_BLOCKS - 19,
          "a block is allocated beside the batch's reservation");
    vfs_ns_unlock(&mount);
    check(writeback_drain(&volume) == 0, "the batch drains");
    check(le16(disk[4] + at) == other.mode && le32(disk[4] + at + 4) == 77 &&
              le32(disk[4] + (file.ino - 1) * 128 + 4) == 4096,
          "and the table block has both inodes");
    check(bit_on(TEST_BLOCKS - 20) && bit_on(TEST_BLOCKS - 19) && le16(disk[1] + 12) == 18 &&
              volume.free_blocks == 18,
          "and the bitmap and descriptor both allocations");
}

/**
 * A drain on a mount that writes back has the cache make room for its
 * copies, one buffer each, before it takes em->meta and copies.
 */
static void drain_reserve_row(void)
{
    reset(20);
    check(ext2_write(&mount, &file, payload, 4096, 0) == 4096, "a pending piece of four images");
    check(writeback_drain(&volume) == 0 && reserve_calls == 1 && reserve_n == 4,
          "a drain reserves a buffer for each changed image");
    check(reserve_at != 0 && blank_at > reserve_at, "before its first copy");
}

/* The nested punch's file, freed while the outer punch fetches a bitmap. */
static void punch_other(void)
{
    check(punch(&volume, &other, 0) == 0, "the nested punch frees its blocks");
}

/**
 * Two punches of two files, the second run while the first fetches its
 * bitmap with em->meta let go, as two processes would interleave at that
 * disk wait. Each keeps its own list: every block of both files is freed,
 * the counts come back whole, and the inode table block is dirtied first
 * and the bitmap and descriptor after, with no mark outside the two
 * classes, so that no pointer on the disk names a block whose bit is clear.
 */
static void overlap_punch_row(void)
{
    int i;

    reset(20);
    check(ext2_write(&mount, &file, payload, 8192, 0) == 8192 &&
              ext2_write(&mount, &other, payload, 8192, 0) == 8192 && writeback_drain(&volume) == 0,
          "two files of two blocks each");
    check(volume.free_blocks == 16, "take four blocks");
    k_memset(order_of, 0, sizeof(order_of));
    k_memset(unordered, 0, sizeof(unordered));
    uncached[2] = 1;
    fetch_hook = punch_other;
    vfs_ns_lock(&mount);
    check(punch(&volume, &file, 0) == 0 && !fetch_hook, "the outer punch frees its blocks");
    vfs_ns_unlock(&mount);
    check(volume.free_blocks == 20 && le16(disk[1] + 12) == 20 && file.blocks == 0 &&
              other.blocks == 0,
          "and the counts are whole");
    for (i = 0; i < 4; i++) {
        uint32_t b = TEST_BLOCKS - 20 + (uint32_t)i;

        check(!bit_on(b), "each block's bit is clear");
    }
    check(file.block[0] == 0 && file.block[1] == 0 && other.block[0] == 0 && other.block[1] == 0,
          "and no pointer names one");
    check(order_of[4] == BCACHE_ORDER_FIRST && order_of[2] == BCACHE_ORDER_AFTER &&
              order_of[1] == BCACHE_ORDER_AFTER && !unordered[1] && !unordered[2] && !unordered[4],
          "the inodes go first and the bitmap and descriptor after");
}

/** Run the rows; 0 when every row passed. */
int main(void)
{
    boundary_rows();
    existing_rows();
    reservation_rows();
    miss_rows();
    held_block_rows();
    drain_reserve_row();
    overlap_punch_row();
    if (!failures) {
        printf("ext2-reserve-hosttest: result: pass: boundaries, reservation quotas, plans and "
               "frees under em->meta\n");
    }
    return failures != 0;
}
