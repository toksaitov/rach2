/*
 * bcache_lock_hosttest.c: the buffer cache's write side under its own lock,
 * with host threads that dirty, write, sync, reserve and shrink one
 * device's buffers at once and no big lock anywhere.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/host_threads.c
 * host-cflags: -pthread
 *
 * The test includes rach2/sys/fs/bcache.c whole, with `current_proc` a
 * thread's own process, the spinlock an atomic exchange, the mutex an
 * atomic owner that a waiter yields the processor for, and a sleep a loop
 * that yields until its condition holds. The big lock is gone: `bkl_held`
 * answers no, and a take of it fails the row, since a write that does not
 * fail never takes it. The device keeps its blocks in memory and yields
 * inside every request, as a disk's sleep lets other callers run, and in
 * its test of a block written last, which every walk of the dirty lists
 * makes for the blocks it passes, as another processor runs while a walk
 * goes on.
 *
 * Every block of the device holds one version: its number in the first and
 * last eight bytes and its low byte in every byte between. A writer takes a
 * block, checks that it holds the version the block's last holder stored,
 * stores a new one, marks the block in one of the cache's ways and lets it
 * go. The device fails the row on a write of a block whose bytes are not one
 * version, which a write of a buffer another process is changing would be,
 * and keeps the version of each block it has. A sync must leave on the
 * device every version let go before it began. At each row's end a last
 * sync leaves every block's last version on the device and the dirty lists
 * empty and whole.
 *
 * Run with RACH2_BCACHE_NO_WB set in the environment, the mutex does
 * nothing, which is the cache with the big lock taken out and no lock in
 * its place: the rows then fail, which shows that they reach what the lock
 * guards, and a walk of a dirty list whose links another thread broke goes
 * round it until the count of blocks a step's walks pass stops the test.
 */

#include "fs/bcache.h"
#include "proc/process.h"

/* Each host thread is a process of its own: `caller_pid` and the holds'
 * lists read `current_proc`, which a processor's record gives in the
 * kernel. The cache's own include of process.h finds it included. */
#undef current_proc
static __thread process_t *thread_proc;
#define current_proc thread_proc

#include "fs/bcache.c"
#include "host_threads.h"

extern int printf(const char *format, ...);
extern void *calloc(unsigned long count, unsigned long size);
extern void *malloc(unsigned long n);
extern void free(void *ptr);
extern char *getenv(const char *name);
extern void abort(void);
extern int fflush(void *stream);
extern int sched_yield(void);

static int fails;

#define CHECK(cond, what)                                                                   \
    do {                                                                                    \
        if (!(cond)) {                                                                      \
            printf("bcache_lock_hosttest: FAIL: line %d: %s: %s\n", __LINE__, what, #cond); \
            fflush(0);                                                                      \
            __atomic_add_fetch(&fails, 1, __ATOMIC_SEQ_CST);                                \
        }                                                                                   \
    } while (0)

/* Set from the environment: the mutex does nothing. */
static int no_wb;

/* A deterministic clean reclaim at the release of a just-written buffer. */
static struct bbuf *reclaim_target;
static uint32_t release_reclaims;
static int release_hooks;

/* A first held by another process until the victim search has given way
 * sixty-four times, including the modeled sleep; every other buffer is
 * an after behind it. */
static struct bbuf *delayed_first;
static process_t delayed_proc;
static int delayed_turns;
static int irqs_off;

/* Reuse a waited buffer before the wait condition sees its release. */
static struct bbuf *reuse_target, *reuse_held;
static process_t reuse_owner, reuse_taker;
static int reuse_check;
static void reuse_progress(void);

/** The kernel's panic: print the text and stop. */
void k_panic(const char *s)
{
    printf("bcache_lock_hosttest: panic: %s\n", s);
    fflush(0);
    abort();
}

/** The kernel log; the records are dropped. */
void klog(int level, const char *sys, const char *fmt, ...)
{
    (void)level;
    (void)sys;
    (void)fmt;
}

/** Take `lock` by an atomic exchange, yielding while another thread holds
 * it. The flags are unused. */
uint32_t spin_lock_irqsave(struct spinlock *lock)
{
    while (__atomic_exchange_n(&lock->locked, 1u, __ATOMIC_ACQUIRE) != 0) {
        sched_yield();
    }
    return 0;
}

/** Release `lock`. */
void spin_unlock_irqrestore(struct spinlock *lock, uint32_t flags)
{
    int reclaim = lock == &cache_spin && reclaim_target && reclaim_target->holder == -1 &&
                  !reclaim_target->dirty;

    (void)flags;
    if (reclaim) {
        reclaim_target = 0;
    }
    __atomic_store_n(&lock->locked, 0u, __ATOMIC_RELEASE);
    if (reclaim) {
        uint32_t written = 0;

        release_hooks++;
        release_reclaims += bcache_shrink(100000, 0, &written);
    }
}

/** Take `m`, yielding while another thread holds it; a thread that holds it
 * already fails the test, as the kernel's would never wake. */
void mutex_lock(struct mutex *m)
{
    uintptr_t none = 0;

    if (no_wb) {
        return;
    }
    if (__atomic_load_n(&m->owner, __ATOMIC_ACQUIRE) == (uintptr_t)thread_proc) {
        k_panic("mutex: taken again by its holder");
    }
    while (!__atomic_compare_exchange_n(&m->owner, &none, (uintptr_t)thread_proc, 0,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        none = 0;
        sched_yield();
    }
}

/** Give `m` back; it must be the caller's. */
void mutex_unlock(struct mutex *m)
{
    if (no_wb) {
        return;
    }
    if (__atomic_load_n(&m->owner, __ATOMIC_ACQUIRE) != (uintptr_t)thread_proc) {
        k_panic("mutex: an unlock by a thread that does not hold it");
    }
    __atomic_store_n(&m->owner, (uintptr_t)0, __ATOMIC_RELEASE);
}

/** Whether the calling thread holds `m`. */
int mutex_held(const struct mutex *m)
{
    return !no_wb && __atomic_load_n(&m->owner, __ATOMIC_ACQUIRE) == (uintptr_t)thread_proc;
}

/** Clear a wait queue. */
void wait_queue_init(wait_queue_t *wq)
{
    __builtin_memset(wq, 0, sizeof(*wq));
}

/** Advance the delayed holder at a yield or a modeled sleep. */
static void delayed_progress(void)
{
    if (delayed_first && ++delayed_turns == 64) {
        process_t *was = thread_proc;
        struct bbuf *b = delayed_first;

        delayed_first = 0;
        thread_proc = &delayed_proc;
        bcache_release(b);
        thread_proc = was;
    }
}

/** The killable wait, which no row kills: the uninterruptible one. */
int wq_wait_event_killable(wait_queue_t *wq, wq_cond_fn cond, void *arg, const char *name)
{
    (void)name;
    return wq_wait_event_nointr(wq, cond, arg);
}

/** Sleep until `cond(arg)` holds, yielding the processor meanwhile. */
int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    CHECK(!irqs_off && !mutex_held(&wb_lock),
          "a buffer wait has interrupts on and gives the write lock up");
    if (reuse_check) {
        reuse_check = 0;
        CHECK(!cond(arg), "the original holder has not released the buffer");
        reuse_progress();
        CHECK(cond(arg), "a release ends the wait even after reuse by a lower block");
        if (!cond(arg) && reuse_held) {
            process_t *was = thread_proc;

            thread_proc = &reuse_taker;
            bcache_release(reuse_held);
            reuse_held = 0;
            thread_proc = was;
        }
    }
    while (!cond(arg)) {
        delayed_progress();
        sched_yield();
    }
    return 0;
}

/** A wake, which nobody needs: every sleeper looks again as it yields. */
void wq_wake_all_deferred(wait_queue_t *wq, uint64_t *cpus)
{
    (void)wq;
    (void)cpus;
}

/** The kick of the woken processors, which there are none of. */
void sched_kick_cpus(uint64_t cpus)
{
    (void)cpus;
}

/** A service point: yield. */
void sched_service_point(void)
{
    sched_yield();
}

/** Give way: yield. */
void sched_give_way(void)
{
    delayed_progress();
    sched_yield();
}

/** The interrupt flag: on except in the restricted victim row. */
int kirq_enabled(void)
{
    return !irqs_off;
}

/* The takes of the big lock, which no row may make. */
static int big_takes;

/** The big lock, which no caller holds. */
int bkl_held(void)
{
    return 0;
}

/** A take of the big lock: counted, for the rows to fail. */
void bkl_enter(void)
{
    __atomic_add_fetch(&big_takes, 1, __ATOMIC_SEQ_CST);
}

/** Give back a take of the big lock. */
void bkl_exit(void)
{}

/* The failed writes the cache records, which no row makes. */
static int device_failures;

/** Count a failed write the cache records. */
void vfs_device_failed(block_device_t *dev, uint64_t lba, int rc)
{
    (void)dev;
    (void)lba;
    (void)rc;
    __atomic_add_fetch(&device_failures, 1, __ATOMIC_SEQ_CST);
}

/** No caller holds a mount's lock. */
int vfs_ns_held_by_caller(void)
{
    return 0;
}

/** One device: no device is under another. */
int block_is_below(block_device_t *ancestor, block_device_t *device)
{
    (void)ancestor;
    (void)device;
    return 0;
}

/** The caches' target in frames, which `bcache_set_limit` overrides. */
size_t mem_cache_target(void)
{
    return 128;
}

/** The kernel heap's pages: room for any cache the rows set. */
uint32_t kheap_region_pages(void)
{
    return 65536;
}

/** A zeroed allocation, from the C library. */
void *kzalloc(size_t n)
{
    return calloc(1, n);
}

/** An allocation, from the C library. */
void *kmalloc(size_t n)
{
    return malloc(n);
}

/** Free what `kzalloc` or `kmalloc` gave. */
void kfree(void *p)
{
    free(p);
}

/** A buffer's page. */
void *kalloc_cache_page(void)
{
    return calloc(1, PAGE_SIZE);
}

/** `count` zeroed pages, or NULL. */
void *kalloc_pages_try(uint32_t count, uintptr_t *phys)
{
    if (phys) {
        *phys = 0;
    }
    return calloc(count, PAGE_SIZE);
}

/** Free pages from `kalloc_pages_try` or `kalloc_cache_page`. */
void kfree_pages(void *p, uint32_t count)
{
    (void)count;
    free(p);
}

/* The device: BLOCKS blocks in memory, taking eight a request, and the
 * version of each block its last write left, which the rows read. */
#define BLOCKS 256u
static unsigned char image[BLOCKS * BCACHE_BLOCK_BYTES];
static uint64_t on_disk[BLOCKS];
/* Every version's number is unique and taken by the block's holder, so a
 * block's versions rise: the next is taken from here. */
static uint64_t next_version;
/* The version each block's last holder stored, and the highest version of
 * it let go, which a sync that began after must leave on the device. */
static uint64_t stored[BLOCKS];
static uint64_t released[BLOCKS];
/* Writes of a block whose bytes were not one version. */
static int torn_writes;

/** The version the block at `p` holds, or ~0 when its bytes are not one
 * version: its first and last words differ, or a byte between is not the
 * low byte of the first. */
static uint64_t version_of(const unsigned char *p)
{
    uint64_t first, last;
    uint32_t i;

    __builtin_memcpy(&first, p, 8);
    __builtin_memcpy(&last, p + BCACHE_BLOCK_BYTES - 8, 8);
    if (first != last) {
        return ~(uint64_t)0;
    }
    for (i = 8; i < BCACHE_BLOCK_BYTES - 8; i++) {
        if (p[i] != (unsigned char)first) {
            return ~(uint64_t)0;
        }
    }
    return first;
}

/** Make the block at `p` version `v`, in two halves with a yield between,
 * so that a write of it meanwhile would carry half of each. */
static void version_put(unsigned char *p, uint64_t v)
{
    __builtin_memset(p + 8, (int)(unsigned char)v, BCACHE_BLOCK_BYTES / 2 - 8);
    __builtin_memcpy(p, &v, 8);
    sched_yield();
    __builtin_memset(p + BCACHE_BLOCK_BYTES / 2, (int)(unsigned char)v, BCACHE_BLOCK_BYTES / 2 - 8);
    __builtin_memcpy(p + BCACHE_BLOCK_BYTES - 8, &v, 8);
}

/** Read `count` sectors at `lba`, after a yield, as a disk's sleep. */
static int disk_read(block_device_t *d, uint64_t lba, uint32_t count, void *buf)
{
    (void)d;
    sched_yield();
    __builtin_memcpy(buf, image + lba * 512u, (size_t)count * 512u);
    return 0;
}

/** Write `count` sectors at `lba`, which are whole blocks, after a yield:
 * count a block that is not one version, and keep each block's version. */
static int disk_write(block_device_t *d, uint64_t lba, uint32_t count, const void *buf)
{
    const unsigned char *p = buf;
    uint32_t i;

    (void)d;
    sched_yield();
    for (i = 0; i < count / BCACHE_BLOCK_SECTORS; i++) {
        uint64_t v = version_of(p + (size_t)i * BCACHE_BLOCK_BYTES);
        uint64_t blk = lba / BCACHE_BLOCK_SECTORS + i;

        if (v == ~(uint64_t)0) {
            __atomic_add_fetch(&torn_writes, 1, __ATOMIC_SEQ_CST);
            continue;
        }
        __atomic_store_n(&on_disk[blk], v, __ATOMIC_SEQ_CST);
    }
    __builtin_memcpy(image + lba * 512u, buf, (size_t)count * 512u);
    return 0;
}

/* The blocks a thread's walks of the dirty lists have passed in its step,
 * and the most one step may pass: a few thousand walks of at most the
 * cache's 48 buffers. A count past it is a walk that goes round a list
 * whose links another thread broke, which would never end. */
static __thread unsigned long walked;
#define WALKED_MOST 10000000ul

/**
 * The device's test of a block written last (`bcache_sync_last`), which
 * names none but yields the processor, so that each step of a walk of the
 * dirty lists, which calls it for the blocks it passes, lets the other
 * threads run in the middle of the walk.
 */
static int yield_last(void *arg, uint64_t blk)
{
    (void)arg;
    (void)blk;
    if (++walked > WALKED_MOST) {
        k_panic("a walk of the dirty lists goes round a list for ever");
    }
    sched_yield();
    return 0;
}

static block_device_t disk = {
    512, BLOCKS *BCACHE_BLOCK_SECTORS, BLOCK_FAULT_SAFE, 0, 0, 64, disk_read, disk_write,
};

/** The block layer's request, done by the device's own read or write. */
int block_request(block_device_t *device, int write, uint64_t lba, uint32_t count, void *data)
{
    return write ? device->write(device, lba, count, data) : device->read(device, lba, count, data);
}

/** A request with flags: the fake device keeps no cache of its own. */
int block_request_flags(block_device_t *device, int write, uint64_t lba, uint32_t count, void *data,
                        uint32_t flags)
{
    (void)flags;
    return block_request(device, write, lba, count, data);
}

/** The device's flush, which has nothing to do. */
int block_flush(block_device_t *device)
{
    (void)device;
    return 0;
}

/** The device's flags. */
uint32_t block_flags(block_device_t *device)
{
    return device->flags;
}

/** The device that takes the requests: a host device has no partition view
 * over it. */
block_device_t *block_submitter(block_device_t *dev)
{
    return dev;
}

/** The most sectors a request moves. */
uint32_t block_max_sectors(block_device_t *device)
{
    return device->max_sectors;
}

/* One thread of a row: its process, its role, its blocks, how many steps it
 * takes, and the seed of its choices. */
struct worker {
    process_t proc;
    int role;
    uint32_t first, count; /* its blocks: [first, first + count) */
    int steps;
    uint32_t seed;
};
#define ROLE_WRITER  1
#define ROLE_SYNCER  2
#define ROLE_RESERVE 3
#define ROLE_SHRINK  4

/** The next of `w`'s choices, below `n`. */
static uint32_t choose(struct worker *w, uint32_t n)
{
    w->seed = w->seed * 1103515245u + 12345u;
    return (w->seed >> 8) % n;
}

/** Raise `*at` to `v` when it is lower. */
static void raise_to(uint64_t *at, uint64_t v)
{
    uint64_t was = __atomic_load_n(at, __ATOMIC_SEQ_CST);

    while (was < v &&
           !__atomic_compare_exchange_n(at, &was, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
    }
}

/**
 * One step of a writer: take a block of its own, check that it holds the
 * version its last holder stored, store a new one and mark it, in one of
 * the six ways a file system marks a block, and let it go.
 */
static void write_step(struct worker *w)
{
    uint64_t blk = w->first + choose(w, w->count);
    struct bbuf *b = bcache_get(&disk, blk);
    int how = (int)choose(w, 6);
    uint64_t v;

    CHECK(b != 0, "a writer's get finds a buffer");
    if (!b) {
        return;
    }
    v = __atomic_add_fetch(&next_version, 1, __ATOMIC_SEQ_CST);
    CHECK(version_of(b->data) == __atomic_load_n(&stored[blk], __ATOMIC_SEQ_CST),
          "a block holds the version its last holder stored, through its writes and evictions");
    version_put(b->data, v);
    __atomic_store_n(&stored[blk], v, __ATOMIC_SEQ_CST);
    switch (how) {
    case 0:
        bcache_dirty(b);
        break;
    case 1:
        bcache_dirty_ordered(b, (blk & 1) ? BCACHE_ORDER_AFTER : BCACHE_ORDER_FIRST);
        break;
    case 2:
        bcache_dirty_owned(b, 1 + blk % 4);
        break;
    case 3:
        bcache_dirty_pointer(b, 1 + blk % 4, 1 + (int)(blk % 2));
        break;
    case 4:
        CHECK(bcache_write(b) == 0, "a write through succeeds");
        break;
    default:
        CHECK(bcache_write_unflushed(b) == 0, "an unflushed write through succeeds");
        break;
    }
    bcache_release(b);
    raise_to(&released[blk], v);
}

/**
 * One step of a syncer: note the versions let go so far, sync the device,
 * or one owner's blocks or one block, and check that the device has each
 * of them or a later one.
 */
static void sync_step(struct worker *w)
{
    static __thread uint64_t before[BLOCKS];
    uint32_t i, way = choose(w, 4);
    uint64_t blk = choose(w, BLOCKS);
    int rc;

    for (i = 0; i < BLOCKS; i++) {
        before[i] = __atomic_load_n(&released[i], __ATOMIC_SEQ_CST);
    }
    if (way <= 1) {
        rc = way ? bcache_sync_unflushed(&disk) : bcache_sync(&disk);
        CHECK(rc == 0, "a sync succeeds");
        for (i = 0; i < BLOCKS; i++) {
            if (__atomic_load_n(&on_disk[i], __ATOMIC_SEQ_CST) < before[i]) {
                CHECK(0, "a sync leaves on the device every version let go before it began");
                break;
            }
        }
    } else if (way == 2) {
        CHECK(bcache_sync_owned(&disk, 0) == 0, "a sync of every owner succeeds");
        for (i = 0; i < BLOCKS; i++) {
            if (__atomic_load_n(&on_disk[i], __ATOMIC_SEQ_CST) < before[i]) {
                CHECK(0, "a sync of every owner leaves every version let go before it began");
                break;
            }
        }
    } else {
        CHECK(bcache_sync_block(&disk, blk) == 0, "a sync of one block succeeds");
        CHECK(__atomic_load_n(&on_disk[blk], __ATOMIC_SEQ_CST) >= before[blk],
              "a sync of one block leaves the version let go before it began");
    }
}

/** One step of a reserve: ask that up to sixteen misses find a buffer
 * without a write. */
static void reserve_step(struct worker *w)
{
    uint32_t want = 1 + choose(w, 16);

    CHECK(bcache_reserve(&disk, want) <= want, "a reserve answers at most what it was asked");
}

/** One step of a shrinker: reclaim clean buffers or write dirty victims. */
static void shrink_step(struct worker *w)
{
    uint32_t written = 0, want = 1 + choose(w, 8);

    CHECK(bcache_shrink(want, (int)choose(w, 2), &written) <= want,
          "a shrink frees no more buffers than requested");
}

/** A thread of a row: be its process and take its steps. */
static void *worker_run(void *arg)
{
    struct worker *w = arg;
    int i;

    thread_proc = &w->proc;
    for (i = 0; i < w->steps; i++) {
        walked = 0;
        if (w->role == ROLE_WRITER) {
            write_step(w);
        } else if (w->role == ROLE_SYNCER) {
            sync_step(w);
        } else if (w->role == ROLE_SHRINK) {
            shrink_step(w);
        } else {
            reserve_step(w);
        }
    }
    return 0;
}

/**
 * Check the dirty lists whole: each linked both ways, of its class, latest
 * dirtied first, of dirty buffers alone, and as many as `ndirty` and as the
 * buffers marked dirty on the list of all buffers. One thread runs.
 */
static void lists_whole(const char *row)
{
    const struct dirty_list *lists[2];
    const struct bbuf *b, *prev;
    uint32_t n = 0, marked = 0;
    int l;

    lists[0] = &firsts;
    lists[1] = &dirties;
    for (l = 0; l < 2; l++) {
        prev = 0;
        for (b = lists[l]->head; b && n <= nbufs; b = b->dirty_next) {
            CHECK(b->dirty && b->dirty_prev == prev, row);
            CHECK((b->order == BCACHE_ORDER_FIRST) == (l == 0), row);
            CHECK(!prev || prev->dirtied > b->dirtied, row);
            prev = b;
            n++;
        }
        CHECK(lists[l]->tail == prev, row);
    }
    for (b = bufs; b; b = b->next) {
        marked += b->dirty != 0;
    }
    CHECK(n == ndirty && marked == ndirty, row);
}

/**
 * Run a row of `n` threads, each of `roles[i]`, `steps` steps, writers on
 * the blocks [first, first + count). Then, with one thread, sync once more
 * and check that every block's last version is on the device, the lists are
 * whole and empty, and no write was torn and none took the big lock.
 */
static void run_row(const char *row, int n, const int *roles, uint32_t first, uint32_t count,
                    int steps)
{
    static struct worker workers[8];
    void *args[8];
    uint32_t i;
    int k;

    for (k = 0; k < n; k++) {
        __builtin_memset(&workers[k], 0, sizeof(workers[k]));
        workers[k].proc.pid = 100 + k;
        workers[k].role = roles[k];
        workers[k].first = first;
        workers[k].count = count;
        workers[k].steps = steps;
        workers[k].seed = 7919u * (uint32_t)(k + 1);
        args[k] = &workers[k];
    }
    CHECK(host_threads_run(n, worker_run, args) == 0,
          "every thread of the row starts and is joined");
    thread_proc = &workers[0].proc;
    CHECK(bcache_sync(&disk) == 0, row);
    for (i = 0; i < BLOCKS; i++) {
        if (on_disk[i] != stored[i]) {
            CHECK(0, "the last sync leaves every block's last version on the device");
            break;
        }
    }
    lists_whole(row);
    CHECK(ndirty == 0, "and the last sync leaves nothing dirty");
    CHECK(torn_writes == 0, "no write carries a block being changed");
    CHECK(big_takes == 0 && device_failures == 0, "no step takes the big lock or fails a write");
    printf("bcache_lock_hosttest: %s: %d threads, %d steps each\n", row, n, steps);
}

/**
 * Force clean reclaim at each last release: a sync's multi-block run must
 * save its last block before releasing it, and a writing shrink must keep
 * its buffer until its final decision. The old paths read freed storage;
 * AddressSanitizer reports either without depending on thread timing.
 */
static void release_reclaim_rows(void)
{
    struct bbuf *b;
    uint32_t written = 0;
    int i;

    CHECK(bcache_invalidate(&disk) == 0, "the release rows start with no disk buffers");
    (void)bcache_shrink(100000, 0, &written);
    bcache_sync_last(&disk, 0, 0, 0);
    for (i = 0; i < 2; i++) {
        b = bcache_get_blank(&disk, (uint64_t)i);
        CHECK(b != 0, "a release row gets its block");
        if (!b) {
            return;
        }
        bcache_dirty(b);
        bcache_release(b);
    }
    reclaim_target = b;
    CHECK(bcache_sync(&disk) == 0, "a run survives reclaim at its last release");
    CHECK(release_hooks == 1 && release_reclaims == 2,
          "the run's last buffer was freed at its release");
    CHECK(bcache_invalidate(&disk) == 0, "the run leaves nothing dirty");
    (void)bcache_shrink(100000, 0, &written);
    b = bcache_get_blank(&disk, 3);
    CHECK(b != 0, "the writing shrink gets its block");
    if (!b) {
        return;
    }
    bcache_dirty(b);
    bcache_release(b);
    reclaim_target = b;
    written = 0;
    CHECK(bcache_shrink(1, 1, &written) == 1 && written == 1,
          "a writing shrink keeps its buffer through the last look");
    CHECK(release_hooks == 2 && release_reclaims == 2,
          "clean reclaim cannot free the buffer kept by a writing shrink");
    reclaim_target = 0;
}

/**
 * Keep an ordered first held across more idle turns than the victim
 * search's retry limit, with the rest of the full cache after it. A miss
 * must wait for the release and write a victim, without reporting I/O
 * failure while forty-seven buffers can still be reclaimed. With
 * `restricted` 1 the caller holds an after above the first; with 2 its
 * interrupts are off. Neither may wait for the first.
 */
static void delayed_first_row(int restricted)
{
    struct bbuf *b, *own = 0;
    process_t *was = thread_proc;
    uint32_t written = 0;
    unsigned i;

    delayed_turns = 0;
    CHECK(bcache_invalidate(&disk) == 0, "the delayed first starts with no disk buffers");
    (void)bcache_shrink(100000, 0, &written);
    bcache_set_limit(48);
    delayed_proc.pid = 50;
    thread_proc = &delayed_proc;
    b = bcache_get_blank(&disk, 0);
    CHECK(b != 0, "the delayed first gets a buffer");
    if (!b) {
        thread_proc = was;
        return;
    }
    bcache_dirty_ordered(b, BCACHE_ORDER_FIRST);
    delayed_first = b;
    thread_proc = was;
    for (i = 1; i < 48; i++) {
        b = bcache_get_blank(&disk, i);
        CHECK(b != 0, "the delayed first's after gets a buffer");
        if (!b) {
            break;
        }
        bcache_dirty_ordered(b, BCACHE_ORDER_AFTER);
        if (restricted == 1 && i == 1) {
            own = b;
        } else {
            bcache_release(b);
        }
    }
    irqs_off = restricted == 2;
    b = bcache_get_blank(&disk, 100);
    irqs_off = 0;
    if (!restricted) {
        CHECK(b != 0 && delayed_turns == 64,
              "a miss waits for an ordered first past sixteen idle turns");
    } else {
        CHECK(b == 0 && delayed_turns <= 16,
              "a miss cannot wait below a held buffer or with interrupts off");
    }
    printf("bcache_lock_hosttest: delayed first: restricted %d, %d pauses, buffer %d\n", restricted,
           delayed_turns, b != 0);
    if (b) {
        bcache_release(b);
    }
    if (own) {
        bcache_release(own);
    }
    if (delayed_first) {
        thread_proc = &delayed_proc;
        bcache_release(delayed_first);
        delayed_first = 0;
        thread_proc = was;
    }
    CHECK(bcache_sync(&disk) == 0, "the delayed first's buffers sync");
    CHECK(bcache_invalidate(&disk) == 0, "the delayed first leaves no disk buffers");
}

/** Write and release the observed buffer, then reclaim it for block 5 as
 * another process, before the sleeper tests its condition again. */
static void reuse_progress(void)
{
    process_t *was = thread_proc;
    struct bbuf *b = reuse_target;

    CHECK(b && b->waiters == 1, "the wait pins its buffer before the release");
    if (!b) {
        return;
    }
    reuse_target = 0;
    thread_proc = &reuse_owner;
    CHECK(bcache_write(b) == 0, "the original holder writes its buffer");
    bcache_release(b);
    thread_proc = &reuse_taker;
    reuse_held = bcache_get_blank(&disk, 5);
    CHECK(reuse_held == b && b->blk == 5 && b->waiters == 1,
          "a waiter pin allows the clean buffer to be rekeyed and held");
    thread_proc = was;
}

/** Force reuse after release in a direct wait, an exhausted victim search
 * and a write's deferred wait. Block 5's new holder could wait for
 * the caller's block 10, so the original wait must end while 5 stays held.
 * The cache fits the clean scan, so the taker finds the released buffer
 * before the full search can write and reuse another dirty buffer.
 * The wait model records a false condition and releases 5 on old code,
 * making the regression fail without hanging the host process. */
static void reuse_wait_row(int mode)
{
    process_t *was = thread_proc;
    struct bbuf *own, *b, *top = 0;
    struct victim_search search = {0, 0, 0, 0, 0};
    uint32_t flags, written = 0;
    unsigned i;

    CHECK(bcache_invalidate(&disk) == 0, "the reuse row starts with no disk buffers");
    (void)bcache_shrink(100000, 0, &written);
    bcache_set_limit(BCACHE_MIN_BUFFERS);
    CHECK(bcache_capacity() <= CLEAN_LOOK, "the clean scan covers the reuse row's cache");
    reuse_owner.pid = 50;
    reuse_taker.pid = 51;
    own = bcache_get_blank(&disk, 10);
    CHECK(own != 0, "the waiter gets its lower block");
    if (!own) {
        return;
    }
    if (mode == 2) {
        bcache_dirty_owned(own, 2);
    } else {
        bcache_dirty_ordered(own, BCACHE_ORDER_FIRST);
    }
    thread_proc = &reuse_owner;
    reuse_target = bcache_get_blank(&disk, 20);
    CHECK(reuse_target != 0, "the original holder gets its block");
    if (!reuse_target) {
        thread_proc = was;
        bcache_release(own);
        return;
    }
    if (mode == 2) {
        bcache_dirty_owned(reuse_target, 1);
    } else {
        bcache_dirty_ordered(reuse_target, BCACHE_ORDER_FIRST);
    }
    thread_proc = was;
    for (i = 0; i < bcache_capacity() - 2; i++) {
        b = bcache_get_blank(&disk, 100 + i);
        CHECK(b != 0, "the reuse row fills the cache");
        if (!b) {
            break;
        }
        if (mode == 2) {
            if (i < 2) {
                bcache_dirty_pointer(b, 1, (int)i + 1);
                top = b;
            } else {
                bcache_dirty_owned(b, 2);
            }
        } else {
            bcache_dirty_ordered(b, BCACHE_ORDER_AFTER);
        }
        bcache_release(b);
    }
    reuse_check = 1;
    wb_take();
    if (mode == 2) {
        CHECK(top && write_one(top, 0) == 1, "a write returns to look again after reuse");
    } else {
        flags = cache_lock();
        if (mode == 1) {
            int retry = victim_retry(&disk, &search, &flags);

            CHECK(retry == 1, "a victim search retries after the original release");
            if (!retry) {
                cache_unlock(flags);
            }
        } else {
            wait_unheld(reuse_target, flags);
        }
    }
    CHECK(mutex_held(&wb_lock), "the wait restores the caller's write lock");
    wb_give();
    CHECK(!reuse_check && reuse_held && reuse_held->holder == reuse_taker.pid &&
              reuse_held->waiters == 0,
          "the original wait drops its pin while the new holder keeps block 5");
    reuse_check = 0;
    if (reuse_target) {
        thread_proc = &reuse_owner;
        bcache_release(reuse_target);
        reuse_target = 0;
    }
    if (reuse_held) {
        thread_proc = &reuse_taker;
        bcache_release(reuse_held);
        reuse_held = 0;
    }
    thread_proc = was;
    bcache_release(own);
    CHECK(bcache_sync(&disk) == 0, "the reuse row's buffers sync");
    CHECK(bcache_invalidate(&disk) == 0, "the reuse row leaves no disk buffers");
    printf("bcache_lock_hosttest: reuse before recheck: mode %d\n", mode);
}

int main(void)
{
    static const int dirty_and_sync[6] = {ROLE_WRITER, ROLE_WRITER, ROLE_WRITER,
                                          ROLE_WRITER, ROLE_SYNCER, ROLE_SYNCER};
    static const int one_buffer[4] = {ROLE_WRITER, ROLE_SYNCER, ROLE_SYNCER, ROLE_SYNCER};
    static const int reserves[6] = {ROLE_WRITER,  ROLE_WRITER,  ROLE_WRITER,
                                    ROLE_RESERVE, ROLE_RESERVE, ROLE_SYNCER};
    static const int shrinkers[6] = {ROLE_WRITER, ROLE_WRITER, ROLE_SYNCER,
                                     ROLE_SYNCER, ROLE_SHRINK, ROLE_SHRINK};
    static process_t main_proc;

    no_wb = getenv("RACH2_BCACHE_NO_WB") != 0;
    main_proc.pid = 1;
    thread_proc = &main_proc;
    release_reclaim_rows();
    delayed_first_row(0);
    delayed_first_row(1);
    delayed_first_row(2);
    reuse_wait_row(0);
    reuse_wait_row(1);
    reuse_wait_row(2);
    bcache_set_limit(48);
    __builtin_memset(image, 0, sizeof(image));
    bcache_sync_last(&disk, yield_last, 0, 0);

    /* Several threads dirty, write and sync the buffers of one device, more
     * blocks than the cache holds, so that misses write dirty victims. */
    run_row("dirty and sync", 6, dirty_and_sync, 0, BLOCKS, 20000);
    /* A sync races the dirtying of one buffer, which its writer changes in
     * two halves. */
    run_row("one buffer", 4, one_buffer, 5, 1, 20000);
    /* Reserves run beside writers and a sync. */
    run_row("reserve", 6, reserves, 0, BLOCKS, 20000);

    run_row("shrink", 6, shrinkers, 0, BLOCKS, 20000);

    if (fails) {
        printf("bcache_lock_hosttest: %d failures\n", fails);
        return 1;
    }
    printf("bcache_lock_hosttest: ok\n");
    return 0;
}
