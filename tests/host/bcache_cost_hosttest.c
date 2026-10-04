/*
 * bcache_cost_hosttest.c: what the buffer cache reads to serve a get, to
 * choose a victim and to sync, and where it stops growing.
 *
 * host-deps: rach2/sys/fs/bcache.c rach2/sys/block/chain.c
 * host-deps: rach2/sys/lib/kstring.c
 * host-deps: rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/kheap_host.c tests/host/compat/blockmax_host.c
 * host-deps: tests/host/compat/pressure_host.c
 *
 * The cache counts every buffer a walk of one of its lists reads
 * (`bcache_stat_visits`), and these rows hold that count under bounds that
 * are the same on every machine. A get once walked every buffer of the cache
 * for the hold rules and again for its victim, and a sync walked it again
 * for every run it wrote; with sixteen thousand buffers on a machine with
 * memory, that walking was most of what a write cost (entry 25 of milestone
 * 50). The rules now read the caller's own holds, the victim is the head of
 * the release order, and a sync gathers the lowest 128 dirty blocks a walk.
 * Each row that measures the count also requires it to be at least the reads
 * the row names, since a counter that counted nothing would pass every upper
 * bound.
 *
 * The rows run on a cache of 1024 buffers keyed to consecutive blocks of one
 * device, which the index keeps in buckets of their own, so a lookup reads
 * its own buffer and no other. A hit with three buffers held reads the three
 * and its own; a miss reads the head of the release order and the bucket its
 * victim leaves; a release reads the caller's holds as far as the buffer it
 * lets go of. So a hit and its release read at least five buffers, and a miss
 * and its release at least three. The bound is eight buffers for a get and
 * its release, where one walk of the cache is 1024.
 *
 * A sync reads the dirty buffers and not the cache: they are on a list of
 * their own, since a sync once walked every buffer of the cache for each of
 * its passes and batches, and a small file's write, which syncs about nine
 * times, read the whole cache nine times over. A sync of 1024 dirty blocks,
 * on a device that takes eight blocks a request, gathers them all in one
 * walk into a batch the heap gives it, sorted in a heap where a block takes
 * at most ten steps to go in and ten to come out in order; it walks the list
 * once more for its pass over the firsts and twice to find nothing left, and
 * reads each block a few times more, for its lookup in the batch, its place
 * in a run and its hold across the request. The list has its newest buffer
 * first, so these blocks, dirtied in order, are read highest first, which
 * cost an insertion sort a shift of the whole batch for each. The bound
 * allows four walks of the cache, eight reads a block and the heap's steps,
 * where batches of 128 are 43940 and the insertion sort half a million
 * steps, and the count must be at least four walks' worth.
 *
 * Without room in the heap the sync gathers 128 blocks a walk on its stack,
 * one walk for each batch, in a list 128 shorter after each, and seven
 * steps of the heap a block each way. That bound allows twelve walks, eight
 * reads a block and those steps, where a walk for every run written is 128
 * walks, and the count must be at least eleven walks' worth. A sync of eight
 * dirty blocks in a full cache of 1024 reads the eight, at most twelve times
 * each, where a walk of the cache is 1024 reads.
 *
 * The cap row is entry 40's. A buffer is a page of the kernel heap, whose
 * region on i686 is 256 MiB whatever the machine's memory, and on a machine
 * of more than 2 GiB the caches' memory share is larger than the heap. The
 * row stands for a machine of 3 GiB, writes past half the heap's pages, and
 * requires the cache to stop there and make room by writing its oldest
 * buffers back. The last row does the same with a heap whose region is a
 * quarter of that, standing for a region the boot sized otherwise, as it
 * sizes x86-64's from the memory: the cap is half of whatever region the
 * heap has.
 *
 * No hosted headers, as in bcache_hosttest.c: the kernel headers define the
 * fixed-width types, and the libc calls used here are declared by hand. The
 * device keeps no bytes; no row reads data.
 */

#include "../../rach2/sys/fs/bcache.h"
#include "../../rach2/sys/mm/kheap.h"
#include "../../rach2/sys/mm/paging.h"
#include "../../rach2/sys/proc/process.h"

extern int printf(const char *fmt, ...);
extern void _exit(int status);

/* The caches' memory target, in frames, that the pressure stub reports. */
extern uint32_t host_cache_target;
/* The heap's region in pages, which the stand-in heap reports. */
extern uint32_t kheap_host_region_pages;
/* The largest allocation the stand-in heap grants, in bytes, or 0. */
extern unsigned long kheap_host_largest;

/* The cache the cost rows fill, and the most a get and its release may read. */
#define CACHE_BUFFERS 1024u
#define GET_MOST      8u
/* The least a hit with three buffers held and its release read, and the least
 * a miss and its release read. */
#define HIT_LEAST     5u
#define MISS_LEAST    3u
/* The most a sync of CACHE_BUFFERS dirty blocks may read with a batch of
 * them all: four walks of the cache, eight reads a block, and ten steps of
 * the heap for each block going in and ten coming out. The least is the
 * four walks: the one that gathers, one for the firsts and two to find
 * nothing left. */
#define SYNC_MOST     (CACHE_BUFFERS * (4u + 8u + 2u * 10u))
#define SYNC_LEAST    (CACHE_BUFFERS * 4u)
/* The most such a sync may read in batches of 128: twelve walks of the
 * cache and eight reads a block, and HEAP_STEPS steps for each block a walk
 * reads into the batch and for each block it takes out in order. The least
 * is eleven walks of the cache, one for the firsts, one for each batch of
 * 128 and two to find nothing left. */
#define BATCH_WALKS   (CACHE_BUFFERS / 128u + 4u)
#define BATCH_READS   (CACHE_BUFFERS * BATCH_WALKS + 8u * CACHE_BUFFERS)
#define HEAP_STEPS    7u
#define BATCH_MOST    (BATCH_READS + HEAP_STEPS * (CACHE_BUFFERS * BATCH_WALKS + CACHE_BUFFERS))
#define BATCH_LEAST   (CACHE_BUFFERS * (CACHE_BUFFERS / 128u + 3u))
/* A sync of a full cache with FEW_DIRTY dirty blocks reads the dirty ones,
 * at most twelve times each: its walks of the dirty buffers and its reads of
 * each block to write it. It reads them at least twice, once for the pass
 * over the firsts and once for the batch it writes. */
#define FEW_DIRTY     8u
#define FEW_MOST      (12u * FEW_DIRTY)
#define FEW_LEAST     (2u * FEW_DIRTY)
/* What the ATA disk takes in one request: a run of eight blocks. */
#define RUN_SECTORS   64u
/* The cap: half the kernel heap's pages. The cap row writes PAST_SHARE
 * blocks beyond it, and the device has room for them. */
#define HEAP_SHARE    ((KHEAP_FLOOR / PAGE_SIZE) / 2u)
/* A heap of a quarter of i686's region, and its cap. */
#define SMALL_REGION  (KHEAP_FLOOR / PAGE_SIZE / 4u)
#define SMALL_SHARE   (SMALL_REGION / 2u)
#define PAST_SHARE    256u
#define DISK_BLOCKS   (HEAP_SHARE + PAST_SHARE)
#define DISK_SECTORS  ((uint64_t)DISK_BLOCKS * 8u)
/* The caches' target on a machine of 3 GiB: a quarter of its frames. */
#define TARGET_3GIB   (3u * 262144u / 4u)

process_t proc_a;
struct percpu cpu_table[NR_CPUS] = {{.current = &proc_a}};
static int fails;
static uint32_t writes_made;
static uint64_t hit_visits, miss_visits, sync_visits, batch_visits, few_visits;

static void check(int ok, const char *what)
{
    if (!ok) {
        fails++;
        printf("bcache_cost_hosttest: FAIL: %s\n", what);
    }
}

void k_panic(const char *s)
{
    printf("bcache_cost_hosttest: unexpected panic: %s\n", s);
    _exit(44);
}

int vfs_ns_held_by_caller(void)
{
    return 0;
}

void vfs_device_failed(block_device_t *dev, uint64_t lba, int rc)
{
    (void)dev;
    (void)lba;
    (void)rc;
    check(0, "no write fails in these rows");
}

void klog(int level, const char *sys, const char *fmt, ...)
{
    (void)level;
    (void)sys;
    (void)fmt;
}

void wait_queue_init(wait_queue_t *wq)
{
    __builtin_memset(wq, 0, sizeof(*wq));
}

void wq_wake_all(wait_queue_t *wq)
{
    (void)wq;
}

/* One process runs here, so a wait would never end. */
int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    (void)cond;
    (void)arg;
    printf("bcache_cost_hosttest: FAIL: a row reached the sleeping wait\n");
    _exit(1);
    return 0;
}

static int disk_read(block_device_t *d, uint64_t lba, uint32_t count, void *buf)
{
    (void)d;
    (void)lba;
    (void)count;
    (void)buf;
    return 0;
}

static int disk_write(block_device_t *d, uint64_t lba, uint32_t count, const void *buf)
{
    (void)d;
    (void)lba;
    (void)count;
    (void)buf;
    writes_made++;
    return 0;
}

/* The members in order, as in bcache_hosttest.c: the sector size, the count,
 * the flags, the deadline's period and its periods, the most sectors a
 * request moves, the two polled calls, the two a sleeping device has, the
 * device this one's bytes live on, and the driver's own pointer. */
static block_device_t disk = {
    512, DISK_SECTORS, BLOCK_FAULT_SAFE, 0, 0, RUN_SECTORS, disk_read, disk_write, 0, 0, 0, 0};

/* The request pool is i386 code; a host request calls the device. */
int block_request(block_device_t *device, int write, uint64_t lba, uint32_t count, void *data)
{
    if (write) {
        return device->write(device, lba, count, data);
    }
    return device->read(device, lba, count, data);
}

/* Key `count` blocks from `first` on with the blank get, each let go at
 * once, dirty or not. */
static void key_blocks(uint64_t first, uint64_t count, int dirty)
{
    uint64_t i;

    for (i = first; i < first + count; i++) {
        struct bbuf *b = bcache_get_blank(&disk, i);

        if (!b) {
            check(0, "a blank get is served");
            return;
        }
        if (dirty) {
            bcache_dirty(b);
        }
        bcache_release(b);
    }
}

/* The cache full of blocks 0 to 1023; blocks 0, 1 and 2 held while block
 * 1023 is got and let go. */
static void hit_row(void)
{
    struct bbuf *held[3];
    struct bbuf *b;
    uint64_t before;
    int i;

    bcache_set_limit(CACHE_BUFFERS);
    key_blocks(0, CACHE_BUFFERS, 0);
    check(bcache_resident() == CACHE_BUFFERS, "the rows have a cache of 1024 buffers");
    for (i = 0; i < 3; i++) {
        held[i] = bcache_get(&disk, (uint64_t)i);
        check(held[i] != 0, "a held block is a hit");
    }
    before = bcache_stat_visits();
    b = bcache_get(&disk, CACHE_BUFFERS - 1u);
    if (b) {
        bcache_release(b);
    }
    hit_visits = bcache_stat_visits() - before;
    check(b != 0 && hit_visits <= GET_MOST,
          "a hit with three buffers held reads its holds and its own buffer, not the cache");
    check(hit_visits >= HIT_LEAST, "the counter counts the buffers a hit and its release read");
    for (i = 2; i >= 0; i--) {
        if (held[i]) {
            bcache_release(held[i]);
        }
    }
}

/* A miss for each buffer of the full cache: blocks 1024 to 2047, each
 * taking the least recently released buffer. */
static void miss_row(void)
{
    uint64_t before = bcache_stat_visits();

    key_blocks(CACHE_BUFFERS, CACHE_BUFFERS, 0);
    miss_visits = bcache_stat_visits() - before;
    check(bcache_resident() == CACHE_BUFFERS, "the misses reuse the cache's buffers");
    check(miss_visits <= (uint64_t)GET_MOST * CACHE_BUFFERS,
          "a miss on a full cache reads the head of the release order, not the cache");
    check(miss_visits >= (uint64_t)MISS_LEAST * CACHE_BUFFERS,
          "the counter counts the buffers each miss and its release read");
}

/* Every buffer of the cache dirty, blocks 0 to 1023, and a sync. */
static void sync_row(void)
{
    uint64_t before;

    check(bcache_invalidate(&disk) == 0, "the cache is emptied for the sync");
    key_blocks(0, CACHE_BUFFERS, 1);
    writes_made = 0;
    before = bcache_stat_visits();
    check(bcache_sync(&disk) == 0, "the sync writes every dirty block");
    sync_visits = bcache_stat_visits() - before;
    check(writes_made == CACHE_BUFFERS / 8u, "in runs of eight blocks");
    check(sync_visits <= SYNC_MOST,
          "a sync gathers its dirty blocks in one walk and sorts them in a heap");
    check(sync_visits >= SYNC_LEAST, "the counter counts the sync's four walks of the cache");
}

/* The same, with a heap that grants nothing longer than a page: the sync
 * gathers its batches on its stack. */
static void batch_row(void)
{
    uint64_t before;

    check(bcache_invalidate(&disk) == 0, "the cache is emptied for the batched sync");
    key_blocks(0, CACHE_BUFFERS, 1);
    writes_made = 0;
    kheap_host_largest = PAGE_SIZE;
    before = bcache_stat_visits();
    check(bcache_sync(&disk) == 0, "the batched sync writes every dirty block");
    batch_visits = bcache_stat_visits() - before;
    kheap_host_largest = 0;
    check(writes_made == CACHE_BUFFERS / 8u, "in runs of eight blocks, batched");
    check(batch_visits <= BATCH_MOST,
          "without room for one batch, a sync reads the cache once for each 128 dirty blocks, "
          "not once for each run, and sorts each batch in a heap");
    check(batch_visits >= BATCH_LEAST, "the counter counts the sync's eleven walks of the cache");
}

/* The cache full of clean blocks 0 to 1023 but blocks 2048 to 2055, which
 * are dirty, and a sync. */
static void few_row(void)
{
    uint64_t before;

    check(bcache_invalidate(&disk) == 0, "the cache is emptied for the few dirty blocks");
    key_blocks(0, CACHE_BUFFERS - FEW_DIRTY, 0);
    key_blocks(2u * CACHE_BUFFERS, FEW_DIRTY, 1);
    check(bcache_resident() == CACHE_BUFFERS, "the cache is full");
    writes_made = 0;
    before = bcache_stat_visits();
    check(bcache_sync(&disk) == 0, "the sync writes the dirty blocks");
    few_visits = bcache_stat_visits() - before;
    check(writes_made == 1, "in one run of eight blocks");
    check(few_visits <= FEW_MOST,
          "a sync of a full cache with eight dirty blocks reads the eight, not the cache");
    check(few_visits >= FEW_LEAST, "the counter counts the sync's two walks of the eight");
}

/* Every block of the device written through the cache, on a machine whose
 * memory share is three times the cap. */
static void cap_row(void)
{
    host_cache_target = TARGET_3GIB;
    bcache_set_limit(0);
    writes_made = 0;
    key_blocks(0, DISK_BLOCKS, 1);
    check(bcache_resident() == HEAP_SHARE,
          "a machine whose memory share passes the kernel heap has a cache of half the heap's "
          "pages");
    check(writes_made == PAST_SHARE,
          "and the blocks written past it make room by writing the oldest back");
    host_cache_target = 128;
}

/* The same on a heap of a quarter of the region: the cap is half of what
 * the heap has, not of i686's region. */
static void region_row(void)
{
    uint32_t written = 0;

    check(bcache_sync(&disk) == 0 && bcache_invalidate(&disk) == 0 &&
              bcache_shrink(0xFFFFFFFFu, 0, &written) == HEAP_SHARE && bcache_resident() == 0,
          "the cache is emptied and its buffers given back for a heap of another size");
    kheap_host_region_pages = SMALL_REGION;
    host_cache_target = TARGET_3GIB;
    writes_made = 0;
    key_blocks(0, SMALL_SHARE + PAST_SHARE, 1);
    check(bcache_resident() == SMALL_SHARE,
          "a heap of another size has a cache of half its own pages");
    check(writes_made == PAST_SHARE,
          "and the blocks written past that make room by writing the oldest back");
    host_cache_target = 128;
    kheap_host_region_pages = KHEAP_FLOOR / PAGE_SIZE;
}

int main(void)
{
    proc_a.pid = 1;
    hit_row();
    miss_row();
    sync_row();
    batch_row();
    few_row();
    cap_row();
    region_row();
    printf("bcache_cost_hosttest: visits: a hit %llu, 1024 misses %llu, a sync of 1024 blocks "
           "%llu, batched %llu, a sync of 8 of 1024 %llu\n",
           (unsigned long long)hit_visits, (unsigned long long)miss_visits,
           (unsigned long long)sync_visits, (unsigned long long)batch_visits,
           (unsigned long long)few_visits);
    if (fails == 0) {
        printf("bcache_cost_hosttest: result: pass: a get, a victim and a sync read what they "
               "need, and the cache stops at half the heap\n");
    }
    return fails != 0;
}
