/**
 * ext2_reserve_hosttest.c: the blocks a write of rach2/sys/fs/ext2.c
 * reserves: the count it needs, pointer blocks included, decided before any
 * byte is taken, and the reserved blocks kept from a caller who is not root;
 * what the batch and the allocators read and write under their locks,
 * `em->batch` and the groups'; and that a
 * change of a file is admitted only once it has the file's lock; a sync
 * between an inode encode and store, and an interrupt at its odd size count;
 * and a loop device's write, which a SIGKILL pending for its caller does not
 * end.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: rach2/sys/fs/sleeplock.c rach2/sys/fs/ext2xattr.c
 * host-deps: tests/host/compat/spin_none.c
 * host-deps: tests/host/compat/atomic_host.c tests/host/compat/bkl_held.c
 * host-deps: tests/host/compat/fsize_none.c
 *
 * The test includes the driver's files whole. The mount, a group of 64 blocks
 * and an inode are built in memory, over a cache model with one buffer for each
 * block, so that the writer runs without a volume made by mke2fs and
 * without the mount's check, whose calls panic here. The model can say a
 * block is not cached, and keep saying so for a number of reads, as a cache
 * that evicts a block again after its fetch would.
 */
#include "fs/vfs.h"
#include "arch/arch.h"
#include "proc/tgroup.h"

static unsigned int row_xchg(volatile unsigned int *p, unsigned int value);
#define arch_atomic_xchg row_xchg
#include "fs/ext2.c"
#undef arch_atomic_xchg
#include "fs/ext2_alloc.c"
#include "fs/ext2_batch.c"
#include "fs/ext2_check.c"
#include "fs/ext2_dir.c"
#include "fs/ext2_file.c"
#include "ext2_ids_first.h"

extern int memcmp(const void *a, const void *b, unsigned long size);
extern int printf(const char *format, ...);
extern void exit(int status);
extern void free(void *ptr);
extern void *malloc(unsigned long size);
extern void *calloc(unsigned long count, unsigned long size);
extern int bkl_host_free;
extern int bkl_host_elsewhere;

#define TEST_BLOCKS 64u
static uint8_t disk[TEST_BLOCKS][4096], saved[TEST_BLOCKS][4096];
static uint8_t storage[(EXT2_WB_DATA + EXT2_WB_META) * 4096];
static struct bbuf buffers[TEST_BLOCKS];
static struct ext2_mount volume;
static struct ext2_writeback batch;
static struct ext2_node file, other;
static struct vfs_mount mount;
static block_device_t device;
/* The one group's descriptor, then its byte that marks its block bitmap
 * checked (`bitmap_check`), set so that the rows' reads are the plans'. */
static uint8_t descriptors[33], payload[8192], result[8192];
/* The process's thread group, whose pending signals a killable wait reads. */
static struct tgroup group;
static process_t process = {.tg = &group};
struct percpu cpu_table[NR_CPUS] = {{.current = &process}};
static int failures, writes;

/* The forced inode-store race: a sync before the second table get, and
 * a separate durable copy of that block, changed only by a table sync. */
static int table_tracking, table_gets, table_syncs, store_pauses;
static uint8_t durable_table[4096];

/* A device interrupt attempted at the odd size sequence, deferred until
 * interrupts are restored. An unsafe attempt is counted without spinning
 * forever on the big lock that the modeled reader holds. */
static int irq_on = 1, size_irq, irq_pending, irq_blocked, irq_readers;
static uint64_t irq_size;

/* The cache model's state: whether each block is missing from the cache, and
 * how many more reads leave it missing; the misses plans met; whether each
 * block is held by another process, as a sync holds one it writes, the
 * waits for such a block with the allocators' locks let go and the gets of
 * one under them, which would wait out the holder's write there; the order
 * class each block was last dirtied with and the dirty marks without one;
 * the calls of `bcache_reserve`, the buffers the last asked for, and the
 * event count at it and at the first blank get after it; and a hook run
 * once at the next read of a missing block. */
static int uncached[TEST_BLOCKS], refetch[TEST_BLOCKS];
static int misses;
static int busy[TEST_BLOCKS], busy_waits, busy_gets;
static int order_of[TEST_BLOCKS], unordered[TEST_BLOCKS];
static int reserve_calls;
static int dirty_level, unflushed_syncs, writebacks;
static uint32_t reserve_n;
static int events, reserve_at, blank_at;
static void (*fetch_hook)(void);

/* A hook run once in place of the next sleep, which gives back the lock
 * another process holds, and the writers admitted at that sleep. */
static void (*wait_hook)(void);
static int writers_at_wait;

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

/** No sleeper is on any queue: no other process runs here. */
int wq_active(const wait_queue_t *wq)
{
    (void)wq;
    return 0;
}

/** The killable wait, which no row kills: the uninterruptible one. */
int wq_wait_event_killable(wait_queue_t *wq, wq_cond_fn cond, void *arg, const char *name)
{
    (void)name;
    return wq_wait_event_nointr(wq, cond, arg);
}

/** Fail the test, unless a row armed `wait_hook`: any other wait here would
 * never end, since no other process runs. */
int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    void (*hook)(void) = wait_hook;

    (void)wq;
    (void)cond;
    (void)arg;
    wait_hook = 0;
    check(hook != 0, "nothing sleeps");
    if (hook) {
        hook();
        return 0;
    }
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

/** The cycle counter: the clock's microseconds, a cycle each. */
uint64_t arch_cycles(void)
{
    return timer_now_us();
}

/** A cycle is a microsecond here. */
uint64_t timer_cycles_us(uint64_t cycles)
{
    return cycles;
}

/** The cycles here are the clock's microseconds, the same on every
 * processor. */
int timer_cycles_agree(void)
{
    return 1;
}

/** Nothing: there is no scheduler here. */
void sched_service_point(void)
{}

/** Return 0: no row closes the mount, whose wait for its writers alone
 * reads the clock. */
uint64_t tick_count(void)
{
    return 0;
}

/** Whether the caller holds `em->batch` or any group's lock of the volume. */
static int alloc_locks_held(void)
{
    int i;

    for (i = 0; i < EXT2_GROUP_LOCKS; i++) {
        if (sleep_lock_held(&volume.group_locks[i])) {
            return 1;
        }
    }
    return sleep_lock_held(&volume.batch);
}

/** Whether block `block` is in the cache, as the model says; a block it
 * lacks asked by a plan counts in `misses`. */
int bcache_cached(block_device_t *dev, uint64_t block)
{
    (void)dev;
    if (block < TEST_BLOCKS && uncached[block]) {
        misses += process.fs_plan == &volume;
        return 0;
    }
    return 1;
}

/** The state cachestat reads, which no row asks for: the block is cached
 * and clean as `bcache_cached` says. */
int bcache_state(block_device_t *dev, uint64_t block)
{
    (void)dev;
    return block < TEST_BLOCKS && uncached[block] ? 0 : 1;
}

/** Return -1, clean: the model writes every block at once, so an inode's
 * bit never waits for its bitmap's write. */
int bcache_order(block_device_t *dev, uint64_t block)
{
    (void)dev;
    (void)block;
    return -1;
}

/** No row reads a file into page frames, which `read_frames` alone does. */
int block_read_frames(block_device_t *dev, uint64_t lba, uint32_t count, const uintptr_t *frames)
{
    (void)dev;
    (void)lba;
    (void)count;
    (void)frames;
    return -EIO;
}

/** No row reads a file into page frames without a wait either. */
int block_read_frames_async(block_device_t *dev, uint64_t lba, uint32_t count,
                            const uintptr_t *frames, const struct block_frames_done *done)
{
    (void)dev;
    (void)lba;
    (void)count;
    (void)frames;
    (void)done;
    return -EAGAIN;
}

/** No read is in flight without a waiter, so a punch waits for none. */
void block_settle(block_device_t *dev, uint64_t lba, uint64_t count)
{
    (void)dev;
    (void)lba;
    (void)count;
}

/** A host frame is mapped where it is. */
void *kmap_temp(uintptr_t phys)
{
    return (void *)phys;
}

/** Nothing to unmap: `kmap_temp` maps nothing. */
void kunmap_temp(void *va)
{
    (void)va;
}

/** Clear the page at `phys`. 0. */
int paging_zero_frame(uintptr_t phys)
{
    k_memset((void *)phys, 0, PAGE_SIZE);
    return 0;
}

/** Copy only resident model bytes, without changing cache state. */
int bcache_copy_cached(block_device_t *dev, uint64_t block, uint32_t offset, void *out,
                       uint32_t count)
{
    (void)dev;
    if (block >= TEST_BLOCKS || uncached[block] || busy[block] || offset > BCACHE_BLOCK_BYTES ||
        count > BCACHE_BLOCK_BYTES - offset) {
        return 0;
    }
    k_memcpy(out, buffers[block].data + offset, count);
    return 1;
}

/** Whether another process holds block `block`, as the model says. */
int bcache_busy(block_device_t *dev, uint64_t block)
{
    (void)dev;
    return block < TEST_BLOCKS && busy[block];
}

/** Wait for block `block`'s holder, counted in `busy_waits` when it held
 * one; the holder is done after it. */
void bcache_wait_unheld(block_device_t *dev, uint64_t block)
{
    (void)dev;
    if (block < TEST_BLOCKS && busy[block]) {
        busy_waits += !alloc_locks_held();
        busy[block] = 0;
    }
}

/** The drop of an O_DIRECT range's buffers, which the model has none of. */
void bcache_forget(block_device_t *dev, uint64_t blk)
{
    (void)dev;
    (void)blk;
}

/** `bcache_wait_unheld`, which no row kills. */
int bcache_wait_unheld_killable(block_device_t *dev, uint64_t block)
{
    bcache_wait_unheld(dev, block);
    return 0;
}

/**
 * Block `block` of the volume in memory; a block outside it fails a row and
 * gives NULL. A get of a block another process holds waits for it, and
 * counts in `busy_gets` when the caller holds an allocator's lock. A read of a
 * block the model calls missing fails a row when the caller plans, runs
 * `fetch_hook` once, and leaves the block missing for `refetch` more reads.
 */
struct bbuf *bcache_get(block_device_t *dev, uint64_t block)
{
    check(dev == &device && block < TEST_BLOCKS, "cache access is in the volume");
    if (block >= TEST_BLOCKS) {
        return NULL;
    }
    if (busy[block]) {
        busy_gets += alloc_locks_held();
        busy[block] = 0;
    }
    if (uncached[block]) {
        void (*hook)(void) = fetch_hook;

        check(process.fs_plan != &volume, "a plan reads no block the cache lacks");
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
    if (table_tracking && block == 4 && ++table_gets == 2) {
        store_pauses++;
        check(file.unstored == EXT2_STORING && sleep_lock_held(&file.lock),
              "between the inode encode and its store the node owes a store and its lock is held");
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

/** Do nothing: no row reads the write-back errors the VFS reports. */
void vfs_wb_err_set(struct vfs_mount *m, int rc)
{
    (void)m;
    (void)rc;
}

/** Do nothing: no row records a file's own write-back error. */
void vfs_ino_err_forget(struct vfs_mount *mnt, uint64_t ino)
{
    (void)mnt;
    (void)ino;
}

/** Do nothing: no shared mapping holds a node of these rows. */
void filemap_evict_unlinked(struct vfs_mount *mnt, uint64_t ino, void *node)
{
    (void)mnt;
    (void)ino;
    (void)node;
}

/** Count a write to the cache in `writes`, and one without an order class
 * in `unordered`. */
void bcache_dirty(struct bbuf *buffer)
{
    writes++;
    if (table_tracking) {
        buffer->dirty = 1;
    }
    unordered[buffer - buffers]++;
}

/** Count the write and note its block's class in `order_of`. */
void bcache_dirty_ordered(struct bbuf *buffer, int order)
{
    writes++;
    order_of[buffer - buffers] = order;
}

/** Count the write as `bcache_dirty` does; the model keeps no owner. */
void bcache_dirty_owned(struct bbuf *buffer, uint64_t owner)
{
    (void)owner;
    bcache_dirty(buffer);
}

/** Count the write as `bcache_dirty` does; the model keeps no level. */
void bcache_dirty_pointer(struct bbuf *buffer, uint64_t owner, int level)
{
    (void)owner;
    (void)level;
    bcache_dirty(buffer);
}

/** Count the write as `bcache_dirty` does; the model keeps no entry mark. */
void bcache_dirty_entry(struct bbuf *buffer)
{
    bcache_dirty(buffer);
}

/** Keep the order: the model writes nothing to a device, so every order
 * holds. */
int bcache_order_after(block_device_t *dev, uint64_t blk)
{
    (void)dev;
    (void)blk;
    return 0;
}

/** Nothing to end: the model keeps no orders. */
void bcache_order_end(void)
{}

/** Succeed: the model keeps no orders, so a change needs no image. */
int bcache_change(struct bbuf *b)
{
    (void)b;
    return 0;
}

/** Succeed: no block of the model waits by an order. */
int bcache_sync_waiting(block_device_t *dev, uint64_t blk)
{
    (void)dev;
    (void)blk;
    return 0;
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

/** Count a write-back of the cache; the model has nothing to sync. */
int bcache_sync_unflushed(block_device_t *dev)
{
    (void)dev;
    unflushed_syncs++;
    return 0;
}

/** Succeed: the model has nothing to sync. */
int bcache_sync_owned(block_device_t *dev, uint64_t owner)
{
    (void)dev;
    (void)owner;
    return 0;
}

/** Sync a dirty table block to the durable copy in the forced store row;
 * otherwise succeed without a write, as the reservation rows require. */
int bcache_sync_block(block_device_t *dev, uint64_t blk)
{
    (void)dev;
    if (table_tracking && blk == 4 && buffers[blk].dirty) {
        k_memcpy(durable_table, buffers[blk].data, sizeof(durable_table));
        buffers[blk].dirty = 0;
        table_syncs++;
    }
    return 0;
}

/** The model's write of one block keeps no flush, so the ordered write is
 * the same write. */
int bcache_sync_block_ordered(block_device_t *dev, uint64_t blk)
{
    return bcache_sync_block(dev, blk);
}

/** Return 0: the model starts every write at once. */
int bcache_sync_block_ready(block_device_t *dev, uint64_t blk)
{
    (void)dev;
    (void)blk;
    return 0;
}

/** Do nothing: the model's syncs write nothing, in no order. */
void bcache_sync_last(block_device_t *dev, int (*last)(void *arg, uint64_t blk),
                      int (*names)(void *arg, uint64_t blk, uint64_t owner), void *arg)
{
    (void)dev;
    (void)last;
    (void)names;
    (void)arg;
}

/** Return what balance_rows sets; the other rows leave the cache light. */
int bcache_dirty_level(void)
{
    return dirty_level;
}

/** Count no marks: the model keeps no dirty buffers. */
uint64_t bcache_marks(void)
{
    return 1;
}

/** Count a writer's write-back of the cache; the model has nothing to write. */
int bcache_writeback(block_device_t *dev, uint64_t since)
{
    (void)dev;
    (void)since;
    writebacks++;
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

    table_tracking = 0;
    k_memset(disk, 0, sizeof(disk));
    k_memset(&volume, 0, sizeof(volume));
    k_memset(&file, 0, sizeof(file));
    k_memset(&other, 0, sizeof(other));
    k_memset(uncached, 0, sizeof(uncached));
    k_memset(refetch, 0, sizeof(refetch));
    k_memset(busy, 0, sizeof(busy));
    busy_waits = busy_gets = 0;
    k_memset(order_of, 0, sizeof(order_of));
    k_memset(unordered, 0, sizeof(unordered));
    misses = reserve_calls = 0;
    reserve_n = 0;
    events = reserve_at = blank_at = 0;
    fetch_hook = 0;
    k_memset(&mount, 0, sizeof(mount));
    k_memset(&batch, 0, sizeof(batch));
    k_memset(descriptors, 0, sizeof(descriptors));
    descriptors[32] = 1;
    for (i = 0; i < TEST_BLOCKS; i++) {
        buffers[i].data = disk[i];
        buffers[i].bytes = 4096;
        buffers[i].dirty = 0;
        if (i < TEST_BLOCKS - available) {
            disk[2][i / 8] |= (uint8_t)(1u << (i % 8));
        }
    }
    LE32_PUT(descriptors, 2);
    LE32_PUT(descriptors + 4, 3);
    LE32_PUT(descriptors + 8, 4);
    LE16_PUT(descriptors + 12, available);
    k_memcpy(disk[1], descriptors, 32);
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
    volume.gd_first = 1;
    volume.sb_at = 1024;
    volume.sb_block = 1;
    volume.opts = EXT2_OPT_RESERVATION;
    volume.mode = EXT2_MODE_RW;
    volume.ro_compat = EXT2_RO_LARGE_FILE;
    volume.wb = &batch;
    for (i = 0; i < EXT2_WB_DATA + EXT2_WB_META; i++) {
        batch.slot[i] = storage + i * 4096;
    }
    mount.priv = &volume;
    mount.ns.holder = -1;
    em_locks_init(&volume);
    sleep_lock_init(&file.lock, &node_record, SLEEP_LOCK_FILE);
    sleep_lock_init(&other.lock, &node_record, SLEEP_LOCK_FILE);
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
        before.lock = file.lock; /* its place counters move with each take */
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
        check(ext2_read_data(&volume, &file, offset, result, sizeof(result), 0) == 0 &&
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
    ext2_node_lock(&file);
    ext2_node_size_set(&file, (uint64_t)2059 * 4096);
    ext2_node_unlock(&file);
    check(ext2_write(&mount, &file, payload, 4096, file.size) == 4096 && batch.count == 1,
          "append retains a pending pointer path");
    volume.r_blocks = (uint32_t)volume.free_blocks - 2;
    check(ext2_write(&mount, &file, payload, 4096, file.size) == 4096 &&
              volume.free_blocks == volume.r_blocks && batch.count == 1,
          "pending root is counted once across append calls");
}

/**
 * Call `ext2_writeback_fit`, the batch's admission of a piece, directly. A
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
    ext2_node_lock(&file);
    ext2_batch_lock(&volume);
    check(ext2_writeback_fit(&volume, &file, 0, payload, 4096) == 0, "accept an earlier piece");
    before = file;
    remaining = (uint32_t)volume.free_blocks;
    metas = batch.metas;
    datas = batch.datas;
    check(ext2_writeback_fit(&volume, &file, (uint64_t)1049612 * 4096, payload, 4096) == -ENOSPC,
          "piece quota includes uncommitted reservations");
    check(volume.free_blocks == remaining && memcmp(&before, &file, sizeof(file)) == 0 &&
              batch.metas == metas && batch.datas == datas && batch.count == 1 && writes == 0,
          "refused piece preserves the earlier pending piece");
    check(ext2_writeback_drain(&volume) == 0 &&
              ext2_read_data(&volume, &file, 0, result, 4096, 0) == 0 &&
              memcmp(result, payload, 4096) == 0,
          "earlier piece survives a refused reservation");
    ext2_batch_unlock(&volume);
    ext2_node_unlock(&file);

    reset(2);
    process.euid = 0;
    ext2_node_lock(&file);
    ext2_batch_lock(&volume);
    check(ext2_writeback_fit(&volume, &file, (uint64_t)1036 * 4096, payload, 4096) == -ENOSPC &&
              volume.free_blocks == 2 && batch.count == 0,
          "root cannot reserve more blocks than are free");
    ext2_batch_unlock(&volume);
    ext2_node_unlock(&file);
}

/** Whether block `b`'s bit is set in the model disk's block bitmap. */
static int bit_on(uint32_t b)
{
    return (disk[2][b / 8] >> (b % 8)) & 1u;
}

/**
 * A plan under its locks reads no block the cache lacks. The bitmap is
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
    check(ext2_writeback_drain(&volume) == 0 && bit_on(TEST_BLOCKS - 20) &&
              ext2_read_data(&volume, &file, 0, result, 4096, 0) == 0 &&
              memcmp(result, payload, 4096) == 0,
          "and the restarted piece drains and reads back");

    reset(20);
    uncached[2] = 1;
    refetch[2] = 1000;
    check(ext2_write(&mount, &file, payload, 4096, 0) == 4096 && misses == PLAN_TRIES,
          "a bitmap the cache keeps losing is met PLAN_TRIES times");
    check(batch.metas == 3 && ext2_writeback_drain(&volume) == 0 &&
              ext2_read_data(&volume, &file, 0, result, 4096, 0) == 0 &&
              memcmp(result, payload, 4096) == 0,
          "and the last plan reads it under the lock");
}

/**
 * A block another process holds, as a sync or an fsync holds one for the
 * length of its write, is waited for with the locks let go and never under
 * it: by a plan that meets the bitmap held, which starts again, and by a
 * drain whose bitmap and inode table blocks are held when it would copy.
 */
static void busy_rows(void)
{
    reset(20);
    busy[2] = 1;
    check(ext2_write(&mount, &file, payload, 4096, 0) == 4096 && busy_waits == 1 && busy_gets == 0,
          "a plan waits for a held bitmap with its locks let go");
    check(ext2_writeback_drain(&volume) == 0 && bit_on(TEST_BLOCKS - 20) &&
              ext2_read_data(&volume, &file, 0, result, 4096, 0) == 0 &&
              memcmp(result, payload, 4096) == 0,
          "and the restarted piece drains and reads back");

    reset(20);
    check(ext2_write(&mount, &file, payload, 4096, 0) == 4096, "a pending piece");
    busy[2] = busy[4] = 1;
    check(ext2_writeback_drain(&volume) == 0 && busy_waits == 2 && busy_gets == 0,
          "a drain waits for its held blocks with em->batch let go");
    check(bit_on(TEST_BLOCKS - 20) && ext2_le32(disk[4] + (file.ino - 1) * 128 + 4) == 4096,
          "and then copies the bitmap and the inode");
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
    ext2_node_lock(&other);
    ext2_node_size_set(&other, 77);
    check(ext2_inode_store(&volume, &other) == 0, "another inode of that block is stored");
    ext2_node_unlock(&other);
    im = writeback_find(&batch, 4);
    check(im && ext2_le16(im->data + at) == other.mode && ext2_le32(im->data + at + 4) == 77,
          "into the batch's image as well as the cache");
    check(ext2_alloc_block(&volume, volume.alloc_hint, &b) == 0 && b == TEST_BLOCKS - 19,
          "a block is allocated beside the batch's reservation");
    check(ext2_writeback_drain(&volume) == 0, "the batch drains");
    check(ext2_le16(disk[4] + at) == other.mode && ext2_le32(disk[4] + at + 4) == 77 &&
              ext2_le32(disk[4] + (file.ino - 1) * 128 + 4) == 4096,
          "and the table block has both inodes");
    check(bit_on(TEST_BLOCKS - 20) && bit_on(TEST_BLOCKS - 19) && ext2_le16(disk[1] + 12) == 18 &&
              volume.free_blocks == 18,
          "and the bitmap and descriptor both allocations");
}

/**
 * A drain on a mount that writes back has the cache make room for its
 * copies, one buffer each, before it takes em->batch and copies.
 */
static void drain_reserve_row(void)
{
    reset(20);
    check(ext2_write(&mount, &file, payload, 4096, 0) == 4096, "a pending piece of four images");
    check(ext2_writeback_drain(&volume) == 0 && reserve_calls == 1 && reserve_n == 4,
          "a drain reserves a buffer for each changed image");
    check(reserve_at != 0 && blank_at > reserve_at, "before its first copy");
}

/**
 * A cache past its dirty limit is written back by those who wrote it: a
 * process that wrote less than its share pays nothing, as one that only
 * closes or exits does, and a writer past its share pays once. Past the
 * background threshold alone, the flusher writes, and no writer does.
 */
static void balance_rows(void)
{
    reset(20);
    writebacks = 0;
    unflushed_syncs = 0;
    process.fs_written = EXT2_BALANCE_SHARE;
    ext2_em_balance(&volume);
    check(writebacks == 0 && process.fs_written == 0,
          "a light cache writes nothing back and forgets the share");
    process.fs_written = EXT2_BALANCE_SHARE;
    dirty_level = 1;
    ext2_em_balance(&volume);
    check(writebacks == 0 && process.fs_written == 0,
          "past the background threshold the writer leaves the write-back to the flusher");
    dirty_level = 2;
    ext2_em_balance(&volume);
    check(writebacks == 0, "a process that wrote nothing pays for no other's writes");
    process.fs_written = EXT2_BALANCE_SHARE - 1;
    ext2_em_balance(&volume);
    check(writebacks == 0 && process.fs_written == EXT2_BALANCE_SHARE - 1,
          "nor does one short of its share");
    process.fs_written = EXT2_BALANCE_SHARE;
    ext2_em_balance(&volume);
    check(writebacks == 1 && process.fs_written == 0,
          "a writer past its share writes back once and begins a new share");
    check(unflushed_syncs == 0, "and no writer syncs the device whole");
    dirty_level = 0;
}

/* The nested punch's file, freed while the outer punch fetches a bitmap. */
static void punch_other(void)
{
    ext2_node_lock(&other);
    check(ext2_punch(&volume, &other, 0) == 0, "the nested punch frees its blocks");
    ext2_node_unlock(&other);
}

/**
 * Two punches of two files, the second run while the first fetches its
 * bitmap with no lock held, as two processes would interleave at that
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
              ext2_write(&mount, &other, payload, 8192, 0) == 8192 &&
              ext2_writeback_drain(&volume) == 0,
          "two files of two blocks each");
    check(volume.free_blocks == 16, "take four blocks");
    k_memset(order_of, 0, sizeof(order_of));
    k_memset(unordered, 0, sizeof(unordered));
    uncached[2] = 1;
    fetch_hook = punch_other;
    ext2_node_lock(&file);
    check(ext2_punch(&volume, &file, 0) == 0 && !fetch_hook, "the outer punch frees its blocks");
    ext2_node_unlock(&file);
    check(volume.free_blocks == 20 && ext2_le16(disk[1] + 12) == 20 && file.blocks == 0 &&
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

/* The descriptors of `inode_rows`' volume: group 128's, the first with a free
 * inode, is in the second block of the table. After them, a byte a group,
 * each set as `descriptors` sets its own. */
#define WIDE_GROUPS 129u
static uint8_t wide_descriptors[WIDE_GROUPS * 33];

/**
 * Point the volume at `WIDE_GROUPS` groups of 32 inodes, of which only group
 * 128 has any free, with its inode bitmap in block 6 and its table in block
 * 7, and name in `file` a directory of the first group, whose group is full.
 */
static void wide_volume(void)
{
    reset(20);
    k_memset(wide_descriptors, 0, sizeof(wide_descriptors));
    k_memset(wide_descriptors + WIDE_GROUPS * 32, 1, WIDE_GROUPS);
    LE32_PUT(wide_descriptors + 128 * 32 + 4, 6);
    LE32_PUT(wide_descriptors + 128 * 32 + 8, 7);
    LE16_PUT(wide_descriptors + 128 * 32 + 14, 5);
    volume.gd = wide_descriptors;
    volume.groups = WIDE_GROUPS;
    volume.ipg = 32;
    volume.inodes = WIDE_GROUPS * 32;
    volume.first_ino = 11;
    volume.free_inodes = 5;
    k_memset(disk[6], 0, sizeof(disk[6]));
    k_memset(disk[7], 0, sizeof(disk[7]));
}

/**
 * A new inode whose parent's group is full looks in another group, whose
 * inode bitmap and descriptor block the cache lacks. Neither is read under
 * the group's lock: the search lets it go and fetches each, starting again,
 * and the bit and count reach the second block of descriptors. A bitmap the
 * cache keeps losing is met PLAN_TRIES times, and then read under the lock.
 */
static void inode_rows(void)
{
    struct ext2_node *n = 0;

    wide_volume();
    uncached[2] = 1;
    uncached[6] = 1;
    check(inode_new(&volume, &file, S_IFREG_ | 0600, 1, &n) == 0 && n && n->ino == 128 * 32 + 1,
          "an inode comes from a group two cold blocks away");
    check(misses == 2, "whose bitmap and descriptor block a search meets and fetches");
    check((disk[6][0] & 1) && volume.free_inodes == 4 && ext2_le16(disk[2] + 14) == 4,
          "and the bit and the counts reach them");
    nodes_free(&volume);

    wide_volume();
    uncached[6] = 1;
    refetch[6] = 1000;
    check(inode_new(&volume, &file, S_IFREG_ | 0600, 1, &n) == 0 && misses == PLAN_TRIES,
          "a bitmap the cache keeps losing is met PLAN_TRIES times");
    check(disk[6][0] & 1, "and the last search reads it under the lock");
    nodes_free(&volume);
}

/** Be the process that holds `file`'s lock and give it back, once the
 * writer that sleeps on it was seen. */
static void lock_holder_gives(void)
{
    writers_at_wait = (int)volume.writers;
    process.pid = 2;
    ext2_node_unlock(&file);
    process.pid = 1;
}

/** Hold `file`'s lock as another process, for the next sleep to give back. */
static void lock_held_elsewhere(void)
{
    reset(20);
    process.pid = 2;
    ext2_node_lock(&file);
    process.pid = 1;
    wait_hook = lock_holder_gives;
    writers_at_wait = -1;
}

/**
 * A change of one file, which not every caller locks, sleeps on the file's
 * lock before admission and not after. Admitted first, it would count as a
 * writer while it slept on a lock whose holder, an unlink before its own
 * admission, waits for a remount that waits for the writers.
 */
static void admission_rows(void)
{
    struct vfs_stat st;
    struct ext2_handle h;

    k_memset(&st, 0, sizeof(st));
    k_memset(&h, 0, sizeof(h));
    h.node = &file;
    h.cursor = ~0u;
    lock_held_elsewhere();
    check(admit_fallocate(&mount, &h, 0, 0, 4096) == 0 && writers_at_wait == 0,
          "a fallocate sleeps on the file's lock before admission");
    lock_held_elsewhere();
    check(admit_write(&mount, &h, payload, 4096, 0) == 4096 && writers_at_wait == 0,
          "a write sleeps on the file's lock before admission");
    lock_held_elsewhere();
    check(admit_truncate(&mount, &h, 4096, 1) == 0 && writers_at_wait == 0,
          "a truncate sleeps on the file's lock before admission");
    lock_held_elsewhere();
    st.mode = 0640;
    check(admit_setattr(&mount, &file, VFS_SET_MODE, &st) == 0 && writers_at_wait == 0,
          "a change of attributes sleeps on the file's lock before admission");
    check(volume.writers == 0 && process.file_locks == 0, "and each leaves the mount and the lock");
}

/** Attempt an interrupt at the first odd store of the armed size writer.
 * A reader holding the big lock cannot finish until that writer resumes. */
static unsigned int row_xchg(volatile unsigned int *p, unsigned int value)
{
    unsigned int old = arch_atomic_xchg(p, value);

    if (size_irq && p == &file.size_seq && (value & 1u)) {
        size_irq = 0;
        if (irq_on) {
            check(!bkl_try_enter(), "the device interrupt waits for the reader's big lock");
            irq_blocked++;
        } else {
            irq_pending = 1;
        }
    }
    return old;
}

/** Save and disable the model's interrupt flag. */
uint32_t kirq_save(void)
{
    uint32_t flags = irq_on;

    irq_on = 0;
    return flags;
}

/** Restore the flag and deliver the pending interrupt only after the size
 * sequence is even, when its big-lock holder can finish `node_size`. */
void kirq_restore(uint32_t flags)
{
    irq_on = flags != 0;
    if (irq_on && irq_pending) {
        irq_pending = 0;
        check(!(file.size_seq & 1u), "a size reader's interrupt waits for the even sequence");
        if (!(file.size_seq & 1u)) {
            irq_size = node_size(&file);
            irq_readers++;
            bkl_host_elsewhere = 0;
            check(bkl_try_enter(), "the deferred interrupt takes the lock after the reader ends");
            bkl_exit();
        }
    }
}

/** Whether the modeled processor admits device interrupts. */
int kirq_enabled(void)
{
    return irq_on;
}

/** Stop a truncation between its inode encode and table store, where an
 * fdatasync of another process, which reads the node's word without the
 * lock, finds it owes a store (EXT2_STORING) and waits for the store on the
 * node's lock, which the truncation holds; then require that fdatasync, run
 * once the store is done, to put the completed size change on disk. */
static void inode_store_sync_row(void)
{
    uint32_t at;

    reset(56);
    at = (uint32_t)ext2_inode_offset(&volume, file.ino) & 4095u;
    inode_encode(&volume, &file, disk[4] + at);
    k_memcpy(durable_table, disk[4], sizeof(durable_table));
    table_gets = table_syncs = store_pauses = 0;
    table_tracking = 1;
    check(ext2_truncate(&mount, &file, 8192, 1) == 0 && store_pauses == 1,
          "a growth stores its inode, stopped once between its encode and its store");
    check(ext2_le32(durable_table + at + 4) == 0 && table_syncs == 0,
          "the truncation leaves the size store in the cache");
    check(file.dirty_meta != 0, "the completed size store retains its metadata dirty mark");
    check(ext2_fdatasync(&mount, &file) == 0 && table_syncs == 1 &&
              ext2_le32(durable_table + at + 4) == 8192 && !file.dirty_meta,
          "the following fdatasync persists the grown size and takes its dirty mark");
    table_tracking = 0;
}

/** Force a device interrupt at the odd size sequence, with interrupts
 * initially on and off; only the even sequence may restore admission. */
static void inode_size_irq_row(void)
{
    int enabled;

    for (enabled = 0; enabled <= 1; enabled++) {
        reset(56);
        irq_on = enabled;
        bkl_host_free = bkl_host_elsewhere = 1;
        irq_pending = irq_blocked = irq_readers = 0;
        size_irq = 1;
        ext2_node_lock(&file);
        ext2_node_size_set(&file, ((uint64_t)1 << 32) + 7);
        ext2_node_unlock(&file);
        check(!size_irq && !irq_blocked && irq_on == enabled,
              "the odd size sequence defers a device interrupt and restores the entry flag");
        if (!enabled) {
            check(irq_pending && !irq_readers, "an irq-off caller keeps its interrupt deferred");
            kirq_restore(1);
        }
        check(irq_readers == 1 && !irq_pending && irq_size == ((uint64_t)1 << 32) + 7,
              "the big-lock reader finishes with the whole new size before the deferred interrupt");
        bkl_host_free = bkl_host_elsewhere = 0;
    }
}

/** Take `lock` with interrupts off: one thread, so nothing waits. */
uint32_t spin_lock_irqsave(struct spinlock *lock)
{
    uint32_t flags = kirq_save();

    lock->locked = 1;
    return flags;
}

/** Give `lock` back. */
void spin_unlock_irqrestore(struct spinlock *lock, uint32_t flags)
{
    lock->locked = 0;
    kirq_restore(flags);
}

/**
 * Write 4 KiB into the hole at 13.9 GiB of an empty file, through a triple
 * indirect block, with SIGKILL pending for the caller, as a loop device's
 * file takes the write of a dying thread that writes the cache above it
 * back. A descriptor's write is -EINTR with nothing taken; the same write
 * inside a loop device's transfer (`device_io`) takes its data block and
 * three pointer blocks and reads back, since a refusal there would fail the
 * device's write and make the file system on it read-only.
 */
static void killed_rows(void)
{
    uint64_t offset = (uint64_t)29155608 * 512;

    reset(56);
    process.sig_pending = KSIG_BIT(SIGKILL);
    check(ext2_write(&mount, &file, payload, 4096, offset) == -EINTR && volume.free_blocks == 56 &&
              file.blocks == 0 && batch.count == 0,
          "a killed caller's write is EINTR with nothing taken");
    process.device_io = 1;
    check(ext2_write(&mount, &file, payload, 4096, offset) == 4096 && volume.free_blocks == 52 &&
              file.blocks == 4 * 8 && file.size == offset + 4096,
          "a loop device's write with SIGKILL pending takes its data and three pointer blocks");
    check(ext2_read_data(&volume, &file, offset, result, 4096, 0) == 0 &&
              memcmp(result, payload, 4096) == 0,
          "and reads back");
    process.device_io = 0;
    process.sig_pending = 0;
}

/** Run the rows; 0 when every row passed. */
int main(void)
{
    boundary_rows();
    existing_rows();
    reservation_rows();
    miss_rows();
    busy_rows();
    held_block_rows();
    drain_reserve_row();
    balance_rows();
    overlap_punch_row();
    inode_rows();
    admission_rows();
    inode_store_sync_row();
    inode_size_irq_row();
    killed_rows();
    if (!failures) {
        printf("ext2-reserve-hosttest: result: pass: boundaries, reservation quotas, plans and "
               "frees under their locks, admission, inode stores, size interrupts and a loop "
               "device's write with SIGKILL pending\n");
    }
    return failures != 0;
}
