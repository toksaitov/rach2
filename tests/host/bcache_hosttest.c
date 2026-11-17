/*
 * bcache_hosttest.c: the buffer cache of rach2/sys/fs/bcache.c, against fake
 * block devices in memory.
 *
 * host-deps: rach2/sys/fs/bcache.c rach2/sys/block/chain.c
 * host-deps: rach2/sys/lib/kstring.c
 * host-deps: rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/kheap_host.c tests/host/compat/blockmax_host.c
 * host-deps: tests/host/compat/pressure_host.c
 * host-deps: tests/host/compat/spinlock_host.c
 *
 * The fake devices are polled and fault-safe, and each is backed by a memory
 * image. `block_request` is stubbed to call the device's own read or write,
 * so no request pool is built. The devices' calls run hooks that stand for
 * other callers acting while a transfer sleeps: a device that goes, a block
 * written or keyed by another process, a torn or dropped write. The rows are
 * listed with `main`. The busy-buffer wait needs a scheduler; the stubbed
 * wait fails the test if any row reaches it. The interrupt flag's restore,
 * which follows every give of `cache_spin`, runs a hook a row sets, which
 * stands for another processor that takes the lock the moment it is free.
 * The interrupt flag is modeled, so a victim search with interrupts off
 * must not reach a scheduler service point; a dirty release behind a search
 * must keep its retries going until the next turn writes and takes it.
 *
 * No hosted headers: the kernel headers define the fixed-width types, and
 * the libc calls used here are declared by hand. Each panic row runs the
 * violating call in a forked child; the child's exit status says whether the
 * panic came and whether its text named the rule.
 */

#include "../../rach2/sys/fs/bcache.h"
#include "../../rach2/sys/lib/kerrno.h"
#include "../../rach2/sys/lib/kirq.h"
#include "../../rach2/sys/proc/mutex.h"
#include "../../rach2/sys/proc/process.h"

extern int printf(const char *fmt, ...);
extern int fork(void);
extern int waitpid(int pid, int *status, int options);
extern void _exit(int status);
extern void exit(int status) __attribute__((noreturn));

process_t proc_a, proc_b;
struct percpu cpu_table[NR_CPUS] = {{.current = &proc_a}};

/* Set before a violating call in a forked child; k_panic exits 42 when its
 * text contains this, 43 when it does not. */
static const char *expect_panic;

/** Return 1 when `needle` occurs in `hay`, and 0 otherwise. */
static int contains(const char *hay, const char *needle)
{
    const char *h;

    for (h = hay; *h; h++) {
        const char *a = h;
        const char *b = needle;

        while (*a && *b && *a == *b) {
            a++;
            b++;
        }
        if (!*b) {
            return 1;
        }
    }
    return 0;
}

/** The kernel's panic: exit 42 or 43 in a panic row's child, as
 *  `expect_panic` says, and otherwise print the text and exit 44. */
void k_panic(const char *s)
{
    if (expect_panic) {
        _exit(contains(s, expect_panic) ? 42 : 43);
    }
    printf("bcache_hosttest: unexpected panic: %s\n", s);
    _exit(44);
}

/* Whether the caller holds a mount's lock, which the cache asks before it
   writes back a buffer of a device that stands on a file; a row sets it. */
static int ns_held;

/** The mount table's answer: `ns_held`. */
int vfs_ns_held_by_caller(void)
{
    return ns_held;
}

/* The takes of the big lock the calls hold, 1 for a caller that holds it,
   as every row but `reader_rows` calls; 0 for a reader without it, which
   the cache takes only to record a failed write. `big_takes` counts the
   takes, and `reads_unlocked` the device reads made without it. */
static int big_held = 1;
static int big_takes;
static int reads_unlocked;

/** The big lock's answer: whether the caller holds it. */
int bkl_held(void)
{
    return big_held > 0;
}

/** Take the big lock, counted in `big_takes`. */
void bkl_enter(void)
{
    big_takes++;
    big_held++;
}

/** Give back a take of the big lock. */
void bkl_exit(void)
{
    big_held--;
}

/* The takes of the cache's write lock, `wb_lock`, and how many are held. A
   restore hook stands for another processor, which in the kernel waits for
   the lock; here it takes the lock as it is, so the count only nests. With
   `take_dev` set, `take_keyed` says whether block `take_blk` of it was in
   the cache at the first take since a row set it to -1: a miss that chose
   a clean victim without the lock keys the block and takes none, and one
   that found only dirty buffers takes the lock before it chooses. */
static int wb_depth;
static int wb_takes;
static block_device_t *take_dev;
static uint64_t take_blk;
static int take_keyed;

/** Take the write lock, counted in `wb_takes`, and note in `take_keyed`
 * whether the block a row watches was keyed at the first take. */
void mutex_lock(struct mutex *m)
{
    (void)m;
    wb_takes++;
    wb_depth++;
    if (take_dev && take_keyed == -1) {
        take_keyed = bcache_cached(take_dev, take_blk);
    }
}

/** Give back a take of the write lock; a give of a free one exits 45. */
void mutex_unlock(struct mutex *m)
{
    (void)m;
    if (wb_depth == 0) {
        printf("bcache_hosttest: the write lock is given back unheld\n");
        _exit(45);
    }
    wb_depth--;
}

/** Whether the write lock is held. */
int mutex_held(const struct mutex *m)
{
    (void)m;
    return wb_depth > 0;
}

/** The kernel log; the records are dropped. */
void klog(int level, const char *sys, const char *fmt, ...)
{
    (void)level;
    (void)sys;
    (void)fmt;
}

/* Set when a row reaches the sleeping wait. */
static int waited_on_host;

/** Clear a wait queue. */
void wait_queue_init(wait_queue_t *wq)
{
    __builtin_memset(wq, 0, sizeof(*wq));
}
/** Wake nobody: no process sleeps here. */
void wq_wake_all(wait_queue_t *wq)
{
    (void)wq;
}

/* The wakes of buffers' queues, the last queue woken, and the processors
   the last kick of the woken interrupted. */
static int wakes_made;
static wait_queue_t *last_woken;
static uint64_t last_kicked;
/* The processor a wake reports it would interrupt, one bit. */
#define WAKE_CPU 4u

/** Count a wake made under a spinlock, which has nobody to wake, and
 *  report `WAKE_CPU` to be interrupted. */
void wq_wake_all_deferred(wait_queue_t *wq, uint64_t *cpus)
{
    wakes_made++;
    last_woken = wq;
    *cpus |= WAKE_CPU;
}
/** Record the processors a waker kicks once its lock is let go. */
void sched_kick_cpus(uint64_t cpus)
{
    last_kicked = cpus;
}

/* Run once at the next restore of the interrupt flag, then cleared. */
static void (*restore_hook)(void);

/* The interrupt flag, on outside a protected region. A restore hook acts
 * as another processor, whose own flag starts on. */
static int irqs_on = 1;

/** Save the model's interrupt flag and clear it. */
uint32_t kirq_save(void)
{
    uint32_t flags = (uint32_t)irqs_on;

    irqs_on = 0;
    return flags;
}

/** Whether the model's caller has interrupts on. */
int kirq_enabled(void)
{
    return irqs_on;
}
/** Run and clear `restore_hook`, if a row set one. */
void kirq_restore(uint32_t flags)
{
    void (*fn)(void) = restore_hook;

    irqs_on = (int)flags;
    if (fn) {
        restore_hook = 0;
        irqs_on = 1;
        fn();
        irqs_on = (int)flags;
    }
}
/** The sleeping wait, which no row may reach: one process runs here, so a
 *  wait would never end. Record the fault, print it and exit 1. */
int wq_wait_event(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    (void)cond;
    (void)arg;
    waited_on_host = 1;
    printf("bcache_hosttest: FAIL: a row reached the sleeping wait\n");
    exit(1);
}
/** The uninterruptible wait, as `wq_wait_event`. */
int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    return wq_wait_event(wq, cond, arg);
}

#define FAKE_SECTORS   100u /* not a multiple of 8: block 12 is a 4-sector tail */
/* The most buffers the cache makes in this test, set on the cache below,
   so the rows that fill it know its size. */
#define BCACHE_BUFFERS 64u
#define BIG_SECTORS    (BCACHE_BUFFERS * 8u + 8u) /* enough blocks to fill it */

static unsigned char image_small[FAKE_SECTORS * 512];
#define WIDE_BLOCKS  320u /* more than one sync batch of 128 */
#define WIDE_SECTORS (WIDE_BLOCKS * 8u)
static unsigned char image_wide[WIDE_SECTORS * 512];
static unsigned char image_big[BIG_SECTORS * 512];
static int reads_made, fail_reads, fail_writes;
/* The device is pulled in the middle of the next read: the cache is told it
   has gone while the buffer being filled is held, and the read then fails. */
static int gone_during_read;
/* The device is pulled in the middle of the next write, that of a dirty
   victim held for it. */
static int gone_during_write;
static unsigned last_count;
static uint64_t writes_lba[8];
static unsigned last_write_count; /* sectors of the last write */
static int writes_made;
/* A block another caller keys while this write is inside the device, which
   is what a writeback that sleeps allows. Keyed with a marker byte, so the
   row can tell that buffer from one the cache filled itself. */
static uint64_t fill_during_write;
/* The fault injector: report success and write nothing at write number
   `drop_at`; write only `tear_bytes` at write number `tear_at`. */
static int drop_at = -1, tear_at = -1, tear_bytes;
/* The one sector whose write fails, or 0, unlike `fail_writes`' every write. */
static uint64_t fail_lba;
/* Another process's write of the device fails while the next write sleeps. */
static int failed_during_write;
/* Run once at the end of the next write's hooks, then cleared. */
static void (*write_hook)(void);
/* Two interleavings. `write_during_fill` is a block written through the
   cache, synced and dropped from it while a run read that covers it is
   inside the device, as another process's write lands during the fill's
   sleep: the read returns the old bytes, and the fill must not put them over
   the newer ones. `first_during_sync` is a block that `hook_proc` dirties as
   a `first` while a sync's write of an `after` is inside the device; the
   sync must write it before its next `after`. */
static uint64_t write_during_fill;
static uint64_t first_during_sync;
static struct process *hook_proc;
/* What `hook_proc` does in the same write as `first_during_sync`, after it:
   mark that block an `after` again, with 0xA1; and, as a drain storing an
   inode does, dirty `drain_data` for owner 7 and then mark the block
   `drain_table`. `steady_writes` writes go on, each dirtying the next block
   from `steady_next`, as a writer that never stops. */
static uint64_t after_during_sync;
static uint64_t drain_data, drain_table;
static int steady_writes;
static uint64_t steady_next;
/* The block whose write's request flags `watch_flags` receives, from the
   block layer's stub, which also counts the flushes and answers them. */
static uint64_t watch_blk;
static uint32_t watch_flags;
extern uint32_t blockmax_last_flags;
extern int blockmax_flushes, blockmax_flush_rc;
/* What the cache recorded against the device. */
static block_device_t *failed_dev;
static uint64_t failed_lba;
static int failed_rc;

/** Record the failed write the cache reports in `failed_dev`, `failed_lba`
 *  and `failed_rc`. */
void vfs_device_failed(block_device_t *dev, uint64_t lba, int rc)
{
    failed_dev = dev;
    failed_lba = lba;
    failed_rc = rc;
}

/**
 * The fake device's read: copy the sectors from the image at `drvdata`,
 * counting the read in `reads_made` and its sectors in `last_count`.
 *
 * Fails with -5 (EIO) when `fail_reads` is set. With `gone_during_read` set
 * it marks the device gone and fails, uncounted. When a read of more than
 * one block covers `write_during_fill`, it then writes that block through
 * the cache after the copy, with 0xEE in its first byte, syncs the device
 * and invalidates it.
 */
static int fake_read(block_device_t *d, uint64_t lba, uint32_t count, void *buf)
{
    if (gone_during_read) {
        gone_during_read = 0;
        bcache_device_gone(d);
        return -5;
    }
    reads_made++;
    reads_unlocked += big_held == 0;
    last_count = count;
    if (fail_reads) {
        return -5;
    }
    __builtin_memcpy(buf, (unsigned char *)d->drvdata + lba * 512, count * 512);
    if (write_during_fill && count > 8 && lba <= write_during_fill * 8 &&
        write_during_fill * 8 < lba + count) {
        uint64_t blk = write_during_fill;
        struct bbuf *w;

        write_during_fill = 0;
        w = bcache_get_blank(d, blk);
        w->data[0] = 0xEE;
        bcache_dirty(w);
        bcache_release(w);
        bcache_sync(d);
        bcache_invalidate(d);
    }
    return 0;
}

/** Mark block `blk` of `d` dirty as `hook_proc`, with the byte `mark`, the
 * place `order` and the owner `owner`. */
static void hook_mark(block_device_t *d, uint64_t blk, int order, uint64_t owner)
{
    struct process *was = current_proc;
    struct bbuf *b;

    current_proc = hook_proc;
    b = bcache_get(d, blk);
    b->data[0] = 0xA1;
    bcache_dirty_ordered(b, order);
    bcache_dirty_owned(b, owner);
    bcache_release(b);
    current_proc = was;
}

/** Run the marks another process makes while a write is inside the device:
 * `after_during_sync`, `drain_data` and `drain_table`, each once, and one of
 * the `steady_writes`. */
static void hook_marks(block_device_t *d)
{
    if (after_during_sync) {
        uint64_t blk = after_during_sync;

        after_during_sync = 0;
        hook_mark(d, blk, BCACHE_ORDER_AFTER, 0);
    }
    if (drain_table) {
        uint64_t data = drain_data, table = drain_table;

        drain_data = drain_table = 0;
        hook_mark(d, data, 0, 7);
        hook_mark(d, table, 0, 0);
    }
    if (steady_writes > 0) {
        steady_writes--;
        hook_mark(d, steady_next++, 0, 0);
    }
}

/**
 * The fake device's write: copy the sectors into the image at `drvdata`.
 *
 * Fails with -5 (EIO) when `fail_writes` is set. Before the copy it runs the
 * hooks that are set, each once: `gone_during_write` marks the device gone,
 * `failed_during_write` marks it as having failed a write,
 * `first_during_sync` dirties that block as a `first` with 0xF1 as
 * `hook_proc`, then the marks of `hook_marks`, `fill_during_write` keys
 * that block blank with 0xAB, and `write_hook` runs. It
 * records the request flags of `watch_blk`'s write, the LBAs of the first
 * eight writes and the count of the last. Write number `drop_at` reports
 * success and copies nothing; write number `tear_at` copies only
 * `tear_bytes`.
 */
static int fake_write(block_device_t *d, uint64_t lba, uint32_t count, const void *buf)
{
    if (fail_writes || (fail_lba && lba == fail_lba)) {
        return -5;
    }
    if (gone_during_write) {
        gone_during_write = 0;
        bcache_device_gone(d);
    }
    if (failed_during_write) {
        failed_during_write = 0;
        d->flags |= BLOCK_WRITE_FAILED;
    }
    if (first_during_sync) {
        uint64_t blk = first_during_sync;
        struct process *was = current_proc;
        struct bbuf *f;

        first_during_sync = 0;
        current_proc = hook_proc;
        f = bcache_get_blank(d, blk);
        f->data[0] = 0xF1;
        bcache_dirty_ordered(f, BCACHE_ORDER_FIRST);
        bcache_release(f);
        current_proc = was;
    }
    hook_marks(d);
    if (fill_during_write) {
        uint64_t blk = fill_during_write;
        struct bbuf *keyed;

        fill_during_write = 0;
        keyed = bcache_get_blank(d, blk);
        if (keyed) {
            keyed->data[0] = 0xAB;
            bcache_release(keyed);
        }
    }
    if (write_hook) {
        void (*fn)(void) = write_hook;

        write_hook = 0;
        fn();
    }
    if (watch_blk != 0 && lba == watch_blk * 8) {
        watch_flags = blockmax_last_flags;
    }
    if (writes_made < 8) {
        writes_lba[writes_made] = lba;
    }
    last_write_count = count;
    writes_made++;
    if (writes_made == drop_at) {
        return 0;
    }
    if (writes_made == tear_at) {
        __builtin_memcpy((unsigned char *)d->drvdata + lba * 512, buf, (size_t)tear_bytes);
        return 0;
    }
    __builtin_memcpy((unsigned char *)d->drvdata + lba * 512, buf, count * 512);
    return 0;
}

/* The members in order: the sector size, the count, the flags, the deadline's
   period and its periods, the most sectors a request moves (0 for a page),
   the read and write calls, `submit` and `flush`, the device this one's
   bytes live on, and the driver's own pointer. */
static block_device_t fake = {
    512, FAKE_SECTORS, BLOCK_FAULT_SAFE, 0, 0, 0, fake_read, fake_write, 0, 0, 0, image_small};
static block_device_t other = {
    512, BIG_SECTORS, BLOCK_FAULT_SAFE, 0, 0, 0, fake_read, fake_write, 0, 0, 0, image_big};
/* A device whose bytes live on `fake` once a row sets its `backing`: what a
   loop device is to the volume holding its file. Its own reads and writes
   go through `fake`, so taking one of its blocks and then one of `fake`'s is
   the order the rules must allow, and the other way round is the one they
   must refuse. */
static block_device_t nested = {
    512, FAKE_SECTORS, BLOCK_FAULT_SAFE, 0, 0, 0, fake_read, fake_write, 0, 0, 0, image_small};
/* A device that takes 64 sectors a request, as the ATA disk does, for the
   run fill and the runs of a sync. */
static block_device_t wide = {
    512, WIDE_SECTORS, BLOCK_FAULT_SAFE, 0, 0, 64, fake_read, fake_write, 0, 0, 0, image_wide};
/* A device that takes 2048 sectors a request, as the AHCI disk does: a
   fill's run is the cache's most, 256 blocks. */
static unsigned char image_vast[WIDE_SECTORS * 512];
static block_device_t vast = {
    512, WIDE_SECTORS, BLOCK_FAULT_SAFE, 0, 0, 2048, fake_read, fake_write, 0, 0, 0, image_vast};

/** The block layer's request, done by the device's own read or write. */
int block_request(block_device_t *device, int write, uint64_t lba, uint32_t count, void *data)
{
    if (write) {
        return device->write(device, lba, count, data);
    }
    return device->read(device, lba, count, data);
}

static int fails;

/** Count a failed check and print `what`. */
static void check(int ok, const char *what)
{
    if (!ok) {
        fails++;
        printf("bcache_hosttest: FAIL: %s\n", what);
    }
}

/**
 * Run `fn(arg)` in a forked child that expects a panic whose text contains
 * `text`, and check the row `what` on the child's exit status.
 *
 * The row passes only when the child exits 42, which `k_panic` gives for a
 * panic that names the rule. The child exits 44 when `fn` returns with no
 * panic.
 */
static void panic_row(void (*fn)(void *), void *arg, const char *text, const char *what)
{
    int pid = fork();
    int status = 0;

    if (pid == 0) {
        expect_panic = text;
        fn(arg);
        _exit(44);
    }
    check(pid > 0 && waitpid(pid, &status, 0) == pid, "the panic child ran");
    check((status & 0x7f) == 0 && ((status >> 8) & 0xff) == 42, what);
}

/** A panic row's call: get the block of `fake` that `arg` points to. */
static void get_fake(void *arg)
{
    bcache_get(&fake, *(uint64_t *)arg);
}
/** A panic row's call: write the buffer `arg` through. */
static void write_it(void *arg)
{
    bcache_write(arg);
}
/** A panic row's call: get the block of `nested` that `arg` points to. */
static void get_nested(void *arg)
{
    bcache_get(&nested, *(uint64_t *)arg);
}

/** A panic row's call: get the block of `other` that `arg` points to. */
static void get_other(void *arg)
{
    bcache_get(&other, *(uint64_t *)arg);
}
/** A panic row's call: sync the device `arg`. */
static void sync_it(void *arg)
{
    bcache_sync((block_device_t *)arg);
}
/** A panic row's call: release the buffer `arg`. */
static void release_it(void *arg)
{
    bcache_release(arg);
}
/** A panic row's call: mark the buffer `arg` dirty. */
static void dirty_it(void *arg)
{
    bcache_dirty(arg);
}

/**
 * Check a sync on `wide` made a write-through disk (`BLOCK_WRITE_THROUGH`).
 *
 * The sync writes without the disk's flush, flushes before the `after` at
 * block 140, so that the `first` it follows is on the medium, and once at
 * its end, and writes the `first`, block 135, with the flush. A failure of
 * the flush at the sync's end is recorded against the device as a failed
 * write is.
 */
static void write_through_rows(void)
{
    struct bbuf *b;
    int i;

    bcache_set_limit(300);
    wide.flags |= BLOCK_WRITE_THROUGH;
    for (i = 0; i < 130; i++) {
        b = bcache_get_blank(&wide, (uint64_t)(i < 128 ? i + 1 : (i == 128 ? 135 : 140)));
        b->data[0] = 0xB0;
        bcache_dirty_ordered(b, i < 128 ? 0 : (i == 128 ? BCACHE_ORDER_FIRST : BCACHE_ORDER_AFTER));
        bcache_release(b);
    }
    watch_blk = 135;
    watch_flags = BLOCK_REQ_NOFLUSH;
    blockmax_flushes = 0;
    check(bcache_sync(&wide) == 0 && blockmax_flushes == 2,
          "a sync on a write-through disk flushes before an `after` and once at its end");
    check(watch_flags == 0, "and writes a `first` with the flush");
    b = bcache_get(&wide, 7);
    b->data[0] = 0xB1;
    bcache_dirty(b);
    bcache_release(b);
    blockmax_flush_rc = -5;
    failed_dev = 0;
    check(bcache_sync(&wide) == -5 && failed_dev == &wide && (wide.flags & BLOCK_WRITE_FAILED),
          "a failed flush at a sync's end is recorded against the device");
    blockmax_flush_rc = 0;
    watch_blk = 0;
    wide.flags &= ~(BLOCK_WRITE_THROUGH | BLOCK_WRITE_FAILED);
    bcache_invalidate(&wide);
}

/** Dirty block `blk` of `dev` for `owner`, with the place `order`. */
static void dirty_for(block_device_t *dev, uint64_t blk, uint64_t owner, int order)
{
    struct bbuf *b = bcache_get_blank(dev, blk);

    b->data[0] = 0xC0;
    bcache_dirty_ordered(b, order);
    bcache_dirty_owned(b, owner);
    bcache_release(b);
}

/** Whether block `blk` of `dev`, which is in the cache, is dirty. */
static int dirty_now(block_device_t *dev, uint64_t blk)
{
    struct bbuf *b = bcache_get(dev, blk);
    int dirty = b && b->dirty;

    bcache_release(b);
    return dirty;
}

/** A panic row's call: hold block 4 of `fake`, dirty, and write it alone. */
static void sync_block_it(void *arg)
{
    struct bbuf *b = bcache_get_blank(&fake, 4);

    (void)arg;
    bcache_dirty(b);
    bcache_sync_block(&fake, 4);
}

/** Check the owner tag of `owned_rows` under the plain marks: a plain mark
 * keeps it, the last tag given stands, a tag of 0 makes the block shared,
 * and a write clears it. */
static void owned_mark_rows(void)
{
    struct bbuf *b;

    b = bcache_get(&vast, 60);
    bcache_dirty_owned(b, 7);
    bcache_dirty(b);
    bcache_release(b);
    b = bcache_get(&vast, 61);
    bcache_dirty_owned(b, 7);
    bcache_dirty_owned(b, 9);
    bcache_release(b);
    writes_made = 0;
    check(bcache_sync_owned(&vast, 7) == 0 && writes_made == 1 && writes_lba[0] == 60 * 8 &&
              last_write_count == 8 && dirty_now(&vast, 61),
          "a plain dirty mark keeps the owner, and the last owner given stands");
    check(bcache_sync_owned(&vast, 9) == 0 && writes_made == 2, "so its blocks go with that one");
    b = bcache_get(&vast, 61);
    bcache_dirty_owned(b, 9);
    bcache_dirty_owned(b, 0);
    bcache_release(b);
    writes_made = 0;
    check(bcache_sync_owned(&vast, 9) == 0 && writes_made == 0 && dirty_now(&vast, 61),
          "a tag of 0 makes the block shared");
    b = bcache_get(&vast, 60);
    bcache_dirty(b);
    bcache_release(b);
    writes_made = 0;
    check(bcache_sync_owned(&vast, 7) == 0 && writes_made == 0 && dirty_now(&vast, 60),
          "a write clears the owner");
    bcache_sync(&vast);
}

/**
 * Check `bcache_sync_owned` beside a large dirty neighbour, on `vast` made a
 * write-through disk.
 *
 * Owner 7 has blocks 50 to 52 and 54, and owner 9 every other block up to
 * 250, more than a sync's stack batch. The owned sync makes two requests,
 * which hold only 7's blocks though 53 lies between and a run could cross
 * it, no flush, and leaves 9's blocks dirty. A plain mark keeps a tag, the
 * last tag stands, and a write clears it. An `after` of the owner's goes
 * after the firsts of other owners marked before it, which keep their flush,
 * and not after one marked later.
 */
static void owned_rows(void)
{
    uint64_t i;

    bcache_invalidate(&vast);
    bcache_set_limit(300);
    vast.flags |= BLOCK_WRITE_THROUGH;
    for (i = 1; i <= 250; i++) {
        dirty_for(&vast, i, (i >= 50 && i <= 52) || i == 54 ? 7 : 9, 0);
    }
    writes_made = 0;
    blockmax_flushes = 0;
    check(bcache_sync_owned(&vast, 7) == 0 && writes_made == 2 && writes_lba[0] == 50 * 8 &&
              writes_lba[1] == 54 * 8 && last_write_count == 8,
          "an owned sync writes that owner's blocks in two requests");
    check(blockmax_flushes == 0 && blockmax_last_flags == BLOCK_REQ_NOFLUSH,
          "without the disk's flush");
    check(dirty_now(&vast, 49) && dirty_now(&vast, 53) && dirty_now(&vast, 55),
          "and leaves the neighbours dirty");
    check(!dirty_now(&vast, 50) && !dirty_now(&vast, 54), "with its own blocks clean");
    writes_made = 0;
    check(bcache_sync_owned(&vast, 7) == 0 && writes_made == 0,
          "an owned sync with nothing dirty writes nothing");
    check(bcache_sync_owned(&vast, 8) == 0 && writes_made == 0, "nor does one of another owner");
    check(bcache_sync(&vast) == 0 && writes_made == 4 && !dirty_now(&vast, 250),
          "and a sync of the device writes the rest, in runs of up to half the cache");
    owned_mark_rows();

    dirty_for(&vast, 80, 9, BCACHE_ORDER_FIRST);
    dirty_for(&vast, 90, 0, BCACHE_ORDER_FIRST);
    dirty_for(&vast, 70, 7, BCACHE_ORDER_AFTER);
    dirty_for(&vast, 91, 9, 0);
    dirty_for(&vast, 92, 9, BCACHE_ORDER_FIRST);
    writes_made = 0;
    watch_blk = 80;
    watch_flags = BLOCK_REQ_NOFLUSH;
    check(bcache_sync_owned(&vast, 7) == 0 && writes_made == 3 && writes_lba[2] == 70 * 8,
          "an owned `after` follows every `first` of the device marked before it, whoever owns it");
    check(watch_flags == 0 && dirty_now(&vast, 91), "with its flush, and leaves the rest");
    check(dirty_now(&vast, 92), "and a `first` marked after it");
    watch_blk = 0;
    bcache_sync(&vast);
    vast.flags &= ~BLOCK_WRITE_THROUGH;
    bcache_invalidate(&vast);
}

/**
 * Check `bcache_sync_block`, the write of one shared block, on `vast` made a
 * write-through disk. An `after` goes after the device's firsts marked
 * before it, with the disk flushed between, and the neighbours stay dirty; a
 * clean block and one not in the cache write nothing; a `first` keeps the
 * disk's flush and a plain block has none; a failed write answers its errno
 * and stays dirty, and a block the caller holds panics.
 */
static void shared_rows(void)
{
    struct bbuf *b;

    vast.flags |= BLOCK_WRITE_THROUGH;
    dirty_for(&vast, 30, 0, BCACHE_ORDER_FIRST);
    dirty_for(&vast, 31, 0, BCACHE_ORDER_FIRST);
    dirty_for(&vast, 20, 0, BCACHE_ORDER_AFTER);
    dirty_for(&vast, 21, 0, 0);
    writes_made = 0;
    blockmax_flushes = 0;
    check(bcache_sync_block(&vast, 20) == 0 && writes_made == 2 && writes_lba[1] == 20 * 8,
          "a shared `after` is written after the device's firsts, in a run");
    check(last_write_count == 8 && !dirty_now(&vast, 20) && dirty_now(&vast, 21),
          "alone, with its neighbour left dirty");
    check(blockmax_flushes == 1, "and the disk flushed before it");
    writes_made = 0;
    blockmax_flushes = 0;
    check(bcache_sync_block(&vast, 20) == 0 && bcache_sync_block(&vast, 300) == 0 &&
              writes_made == 0,
          "a clean block and one not in the cache write nothing");
    check(bcache_sync_block(&vast, 21) == 0 && writes_made == 1 &&
              blockmax_last_flags == BLOCK_REQ_NOFLUSH && blockmax_flushes == 0,
          "a plain shared block is written without the flush");
    dirty_for(&vast, 32, 0, BCACHE_ORDER_FIRST);
    check(bcache_sync_block(&vast, 32) == 0 && blockmax_last_flags == 0, "and a `first` with it");
    b = bcache_get(&vast, 21);
    bcache_dirty(b);
    bcache_release(b);
    fail_writes = 1;
    check(bcache_sync_block(&vast, 21) == -5 && dirty_now(&vast, 21),
          "a failed write answers its errno and stays dirty");
    fail_writes = 0;
    vast.flags &= ~(BLOCK_WRITE_THROUGH | BLOCK_WRITE_FAILED);
    check(bcache_sync_block(&vast, 21) == 0 && !dirty_now(&vast, 21),
          "and the next write takes it");
    dirty_for(&vast, 30, 0, BCACHE_ORDER_FIRST);
    dirty_for(&vast, 20, 0, BCACHE_ORDER_AFTER);
    dirty_for(&vast, 32, 0, BCACHE_ORDER_FIRST);
    writes_made = 0;
    check(bcache_sync_block_ready(&vast, 30) == 0 && writes_made == 0,
          "a `first` is ready for its write at once");
    check(bcache_sync_block_ready(&vast, 20) == 1 && writes_made == 1 && dirty_now(&vast, 20),
          "an `after` has the way cleared, which sleeps, and is not written");
    check(bcache_sync_block_ready(&vast, 20) == 0 && writes_made == 1, "and then it is ready");
    check(bcache_sync_block(&vast, 20) == 0 && bcache_sync_block_ready(&vast, 20) == 0 &&
              bcache_sync_block_ready(&vast, 300) == 0 && writes_made == 2,
          "as a clean block and one not in the cache are");
    check(dirty_now(&vast, 32), "and a `first` marked after the `after` is left for later");
    bcache_sync(&vast);
    dirty_for(&vast, 33, 0, 0);
    dirty_for(&vast, 20, 0, BCACHE_ORDER_AFTER);
    dirty_for(&vast, 30, 0, BCACHE_ORDER_FIRST);
    b = bcache_get(&vast, 33);
    bcache_dirty_ordered(b, BCACHE_ORDER_FIRST);
    bcache_release(b);
    writes_made = 0;
    check(bcache_sync_block(&vast, 20) == 0 && writes_made == 2 && writes_lba[0] == 33 * 8,
          "a block that joins the firsts keeps its place, dirty since before the `after`");
    check(dirty_now(&vast, 30), "ahead of a `first` marked after the `after`");
    bcache_sync(&vast);
    bcache_invalidate(&vast);
    panic_row(sync_block_it, 0, "a sync while holding", "a write of a block the caller holds");
}

/** Name blocks 40 and 41 of `last_rows` as those a sync writes last. */
static int last_block(void *arg, uint64_t blk)
{
    (void)arg;
    return blk == 40 || blk == 41;
}

/**
 * Check `bcache_sync_last` on `vast`: a sync writes blocks 40 and 41 after
 * the rest, in a request of their own though the blocks beside them are
 * dirty. A block that fails keeps them dirty and an eviction from them, until
 * it heals. An `after` waits with them while they hold a `first`, and goes
 * right after the rest when they do not.
 */
static void last_rows(void)
{
    uint32_t written = 0;

    bcache_invalidate(&vast);
    bcache_sync_last(&vast, last_block, 0, 0);
    dirty_for(&vast, 39, 0, 0);
    dirty_for(&vast, 40, 0, 0);
    dirty_for(&vast, 41, 0, 0);
    dirty_for(&vast, 42, 0, 0);
    writes_made = 0;
    check(bcache_sync(&vast) == 0 && writes_made == 3 && writes_lba[0] == 39 * 8 &&
              writes_lba[1] == 42 * 8 && writes_lba[2] == 40 * 8,
          "a sync writes the last blocks after the rest, in their own request");
    dirty_for(&vast, 39, 0, 0);
    dirty_for(&vast, 40, 0, 0);
    dirty_for(&vast, 42, 0, 0);
    fail_lba = 42 * 8;
    check(bcache_sync(&vast) == -5 && !dirty_now(&vast, 39) && dirty_now(&vast, 40),
          "a block that fails keeps them dirty");
    check(bcache_sync(&vast) == -5 && dirty_now(&vast, 40), "and so does a retry that fails");
    bcache_shrink(BCACHE_BUFFERS, 1, &written);
    check(written == 0 && bcache_cached(&vast, 40) && dirty_now(&vast, 40),
          "and no shrink writes them after the failure");
    fail_lba = 0;
    writes_made = 0;
    check(bcache_sync(&vast) == 0 && writes_made == 2 && writes_lba[0] == 42 * 8 &&
              writes_lba[1] == 40 * 8,
          "once it heals they go after it");
    dirty_for(&vast, 38, 0, 0);
    dirty_for(&vast, 39, 0, BCACHE_ORDER_AFTER);
    dirty_for(&vast, 40, 0, BCACHE_ORDER_FIRST);
    fail_lba = 38 * 8;
    check(bcache_sync(&vast) == -5 && dirty_now(&vast, 39) && dirty_now(&vast, 40),
          "an `after` stays dirty with a `first` among them");
    fail_lba = 0;
    writes_made = 0;
    check(bcache_sync(&vast) == 0 && writes_made == 3 && writes_lba[0] == 38 * 8 &&
              writes_lba[1] == 40 * 8 && writes_lba[2] == 39 * 8,
          "and goes after that `first`");
    dirty_for(&vast, 39, 0, BCACHE_ORDER_AFTER);
    dirty_for(&vast, 40, 0, 0);
    writes_made = 0;
    check(bcache_sync(&vast) == 0 && writes_made == 2 && writes_lba[0] == 39 * 8 &&
              writes_lba[1] == 40 * 8,
          "with no `first` among them it goes before them");
    bcache_sync_last(&vast, 0, 0, 0);
    vast.flags &= ~BLOCK_WRITE_FAILED;
    bcache_invalidate(&vast);
}

/**
 * Check `bcache_sync_block_ordered` on `vast` made a write-through disk. A
 * table block (`bcache_sync_last`) goes after the shared blocks dirtied
 * before it, with no flush before, between or with any write, and leaves
 * one dirtied after it; an `after` goes after its firsts with the flush
 * between, as the cache's reclaim keeps it; a plain block and a `first`
 * are written without the flush.
 */
static void ordered_rows(void)
{
    vast.flags |= BLOCK_WRITE_THROUGH;
    bcache_sync_last(&vast, last_block, 0, 0);
    dirty_for(&vast, 30, 0, BCACHE_ORDER_FIRST);
    dirty_for(&vast, 31, 0, BCACHE_ORDER_FIRST);
    dirty_for(&vast, 21, 0, 0);
    dirty_for(&vast, 40, 0, BCACHE_ORDER_FIRST);
    dirty_for(&vast, 24, 0, 0);
    writes_made = 0;
    blockmax_flushes = 0;
    watch_blk = 30;
    watch_flags = 0;
    check(bcache_sync_block_ordered(&vast, 40) == 0 && writes_made >= 2 && writes_made <= 8 &&
              writes_lba[0] == 30 * 8 && writes_lba[writes_made - 1] == 40 * 8 &&
              !dirty_now(&vast, 31) && !dirty_now(&vast, 21) && dirty_now(&vast, 24),
          "an ordered table block is written after the blocks dirtied before it");
    check(blockmax_flushes == 0 && watch_flags == BLOCK_REQ_NOFLUSH &&
              blockmax_last_flags == BLOCK_REQ_NOFLUSH,
          "with no flush before, between or with its writes");
    bcache_sync_last(&vast, 0, 0, 0);
    dirty_for(&vast, 32, 0, BCACHE_ORDER_FIRST);
    dirty_for(&vast, 20, 0, BCACHE_ORDER_AFTER);
    writes_made = 0;
    blockmax_flushes = 0;
    watch_blk = 32;
    watch_flags = 0;
    check(bcache_sync_block_ordered(&vast, 20) == 0 && writes_made == 2 &&
              writes_lba[0] == 32 * 8 && writes_lba[1] == 20 * 8,
          "an ordered `after` is written after its firsts");
    check(blockmax_flushes == 1 && watch_flags == BLOCK_REQ_NOFLUSH &&
              blockmax_last_flags == BLOCK_REQ_NOFLUSH,
          "with the flush between them");
    blockmax_flushes = 0;
    check(bcache_sync_block_ordered(&vast, 24) == 0 && blockmax_last_flags == BLOCK_REQ_NOFLUSH &&
              blockmax_flushes == 0,
          "a plain block is written without the flush");
    dirty_for(&vast, 33, 0, BCACHE_ORDER_FIRST);
    check(bcache_sync_block_ordered(&vast, 33) == 0 && blockmax_last_flags == BLOCK_REQ_NOFLUSH &&
              blockmax_flushes == 0,
          "and so is a `first`");
    watch_blk = 0;
    vast.flags &= ~BLOCK_WRITE_THROUGH;
    bcache_sync(&vast);
    bcache_invalidate(&vast);
}

/** Name blocks 40 and 42 as those `follow_rows` writes last. */
static int table_block(void *arg, uint64_t blk)
{
    (void)arg;
    return blk == 40 || blk == 42;
}

/** Say that blocks 40 and 42 name owner 7's blocks and no other owner's. */
static int table_names_7(void *arg, uint64_t blk, uint64_t owner)
{
    (void)arg;
    return (blk == 40 || blk == 42) && owner == 7;
}

/**
 * Check the order of a block written last against what it names, on `vast`,
 * whose blocks 40 and 42 are such blocks and name owner 7's blocks.
 *
 * A data block of 7's whose write fails keeps the table block 40, marked
 * `first` by a free after it, and the free's `after` bitmap off the device,
 * when a file's sync writes the bitmap: neither is written, and no shrink
 * writes them, until the data block heals. A drain that dirties a data block
 * and stores its inode in table block 42 while the sync's last pass writes
 * table block 40 has the data block written before 42. A table block's
 * write takes the shared blocks and 7's dirtied before its mark and leaves
 * 9's and those dirtied after. A writer that dirties a block at every write
 * of a sync does not keep it: the sync writes what was dirty when it began.
 */
static void follow_rows(void)
{
    uint32_t written = 0;

    bcache_invalidate(&vast);
    bcache_sync_last(&vast, table_block, table_names_7, 0);
    dirty_for(&vast, 50, 7, 0);
    dirty_for(&vast, 40, 0, BCACHE_ORDER_FIRST);
    dirty_for(&vast, 60, 0, BCACHE_ORDER_AFTER);
    fail_lba = 50 * 8;
    writes_made = 0;
    check(bcache_sync_block(&vast, 60) == -5 && writes_made == 0 && dirty_now(&vast, 40) &&
              dirty_now(&vast, 60),
          "a failed data write keeps its table block and the bitmap behind it off the device");
    bcache_shrink(BCACHE_BUFFERS, 1, &written);
    check(written == 0 && writes_made == 0 && dirty_now(&vast, 40) && dirty_now(&vast, 60),
          "and no shrink writes them");
    fail_lba = 0;
    check(bcache_sync(&vast) == 0 && writes_made == 3 && writes_lba[0] == 50 * 8 &&
              writes_lba[1] == 40 * 8 && writes_lba[2] == 60 * 8,
          "once it heals they go after it, in order");
    vast.flags &= ~BLOCK_WRITE_FAILED;

    dirty_for(&vast, 40, 0, 0);
    dirty_for(&vast, 42, 0, 0);
    hook_proc = &proc_b;
    drain_data = 51;
    drain_table = 42;
    writes_made = 0;
    check(bcache_sync(&vast) == 0 && drain_table == 0 && writes_made == 3 &&
              writes_lba[0] == 40 * 8 && writes_lba[1] == 51 * 8 && writes_lba[2] == 42 * 8,
          "a drain into a table block during the last pass has its data written first");

    dirty_for(&vast, 52, 9, 0);
    dirty_for(&vast, 53, 7, 0);
    dirty_for(&vast, 54, 0, 0);
    dirty_for(&vast, 42, 0, 0);
    dirty_for(&vast, 57, 0, 0);
    writes_made = 0;
    check(bcache_sync_block(&vast, 42) == 0 && writes_made == 2 && writes_lba[0] == 53 * 8 &&
              writes_lba[1] == 42 * 8 && !dirty_now(&vast, 54),
          "a table block's write takes the blocks it may name, dirtied before it, in a run");
    check(dirty_now(&vast, 52) && dirty_now(&vast, 57),
          "and leaves another owner's and those dirtied after it");
    bcache_sync(&vast);

    dirty_for(&vast, 100, 0, 0);
    steady_writes = 50;
    steady_next = 200;
    writes_made = 0;
    check(bcache_sync(&vast) == 0 && writes_made == 1 && dirty_now(&vast, 200),
          "a writer dirtying a block at every write does not keep a sync");
    steady_writes = 0;
    bcache_sync(&vast);
    bcache_sync_last(&vast, 0, 0, 0);
    bcache_invalidate(&vast);
}

/** Dirty block `blk` of `dev` as a pointer block of `owner` at `level`. */
static void dirty_pointer_for(block_device_t *dev, uint64_t blk, uint64_t owner, int level)
{
    struct bbuf *b = bcache_get_blank(dev, blk);

    b->data[0] = 0xC1;
    bcache_dirty_pointer(b, owner, level);
    bcache_release(b);
}

/** Give the held-and-released block `blk` of `dev` the place `order`. */
static void order_again(block_device_t *dev, uint64_t blk, int order)
{
    struct bbuf *b = bcache_get(dev, blk);

    bcache_dirty_ordered(b, order);
    bcache_release(b);
}

/**
 * Check the order of pointer blocks (`bcache_dirty_pointer`) on `vast`.
 *
 * A sync writes owner 7's data block before the indirect block that names
 * it and that before the double indirect block above, though their numbers
 * run the other way, and another owner's block in block order. A pointer
 * block does not wait for data dirtied after its last mark. The shared
 * tree, owner 0, as a directory's, waits for the shared blocks and not a
 * file's; a file's tree waits for a shared block dirtied before it, such as
 * the bitmap that took its block, and not for another file's.
 */
static void pointer_order_rows(void)
{
    bcache_invalidate(&vast);
    dirty_for(&vast, 72, 7, 0);
    dirty_pointer_for(&vast, 61, 7, 1);
    dirty_pointer_for(&vast, 50, 7, 2);
    dirty_for(&vast, 55, 9, 0);
    writes_made = 0;
    check(bcache_sync(&vast) == 0 && writes_made == 4 && writes_lba[0] == 72 * 8 &&
              writes_lba[1] == 61 * 8 && writes_lba[2] == 50 * 8 && writes_lba[3] == 55 * 8,
          "data goes before its indirect block and that before the double indirect, by any "
          "numbers");

    dirty_pointer_for(&vast, 61, 7, 1);
    dirty_for(&vast, 72, 7, 0);
    writes_made = 0;
    check(bcache_sync_block(&vast, 61) == 0 && writes_made == 1 && dirty_now(&vast, 72),
          "a pointer block does not wait for data dirtied after its last mark");
    bcache_sync(&vast);

    dirty_for(&vast, 90, 7, 0);
    dirty_for(&vast, 73, 0, 0);
    dirty_pointer_for(&vast, 62, 0, 1);
    writes_made = 0;
    check(bcache_sync_block(&vast, 62) == 0 && writes_made == 2 && writes_lba[0] == 73 * 8 &&
              writes_lba[1] == 62 * 8 && dirty_now(&vast, 90),
          "a shared pointer block waits for the shared blocks and not a file's");
    bcache_sync(&vast);

    dirty_for(&vast, 92, 9, 0);
    dirty_for(&vast, 74, 0, 0);
    dirty_pointer_for(&vast, 67, 7, 1);
    dirty_for(&vast, 79, 0, 0);
    writes_made = 0;
    check(bcache_sync_block(&vast, 67) == 0 && writes_made == 2 && writes_lba[0] == 74 * 8 &&
              writes_lba[1] == 67 * 8 && dirty_now(&vast, 92) && dirty_now(&vast, 79),
          "a file's pointer block waits for a shared block dirtied before it, as the bitmap "
          "that took its block, and not for another file's or a later one");
    bcache_sync(&vast);
}

/**
 * Check how a pointer block waits on `vast`. A failed data write keeps its
 * pointer block off the device, by a sync and by a shrink, until it heals.
 * A shrink passes over a pointer block that waits and writes what it names
 * first. An `after` waits for a pointer block marked `first`, and that for
 * its data. An owned mark makes a pointer block a leaf again.
 */
static void pointer_wait_rows(void)
{
    uint32_t written = 0;
    struct bbuf *b;

    dirty_for(&vast, 75, 7, 0);
    dirty_pointer_for(&vast, 63, 7, 1);
    fail_lba = 75 * 8;
    check(bcache_sync(&vast) == -5 && dirty_now(&vast, 63) && dirty_now(&vast, 75),
          "a failed data write keeps its pointer block off the device");
    bcache_shrink(BCACHE_BUFFERS, 1, &written);
    check(written == 0 && dirty_now(&vast, 63), "and no shrink writes it");
    fail_lba = 0;
    writes_made = 0;
    check(bcache_sync(&vast) == 0 && writes_made == 2 && writes_lba[0] == 75 * 8 &&
              writes_lba[1] == 63 * 8,
          "once it heals they go in order");
    vast.flags &= ~BLOCK_WRITE_FAILED;

    dirty_for(&vast, 76, 7, 0);
    dirty_pointer_for(&vast, 64, 7, 1);
    (void)dirty_now(&vast, 76);
    writes_made = 0;
    written = 0;
    bcache_shrink(BCACHE_BUFFERS, 1, &written);
    check(written == 2 && writes_made == 2 && writes_lba[0] == 76 * 8 && writes_lba[1] == 64 * 8,
          "a shrink passes over a waiting pointer block and writes what it names first");

    dirty_for(&vast, 78, 7, 0);
    dirty_pointer_for(&vast, 66, 7, 1);
    order_again(&vast, 66, BCACHE_ORDER_FIRST);
    dirty_for(&vast, 41, 0, BCACHE_ORDER_AFTER);
    writes_made = 0;
    check(bcache_sync_block(&vast, 41) == 0 && writes_made == 3 && writes_lba[0] == 78 * 8 &&
              writes_lba[1] == 66 * 8 && writes_lba[2] == 41 * 8,
          "an `after` waits for a `first` pointer block, and that for its data");

    dirty_for(&vast, 77, 7, 0);
    dirty_pointer_for(&vast, 65, 7, 1);
    b = bcache_get(&vast, 65);
    bcache_dirty_owned(b, 7);
    bcache_release(b);
    writes_made = 0;
    check(bcache_sync_block(&vast, 65) == 0 && writes_made == 1 && dirty_now(&vast, 77),
          "an owned mark makes a pointer block a leaf again");
    bcache_sync(&vast);
    bcache_invalidate(&vast);
}

/**
 * Check `bcache_reserve` on a cache of 16 buffers, emptied first.
 *
 * With room nothing is written. A cache of dirty blocks is written from the
 * oldest, in runs, until the misses asked for find a clean buffer, and those
 * misses then write nothing; a request past the limit is cut to it. A dirty
 * buffer in front of clean ones is written, since a miss takes the oldest.
 * An `after` waits for its device's firsts. A failed write ends the call.
 */
static void reserve_rows(void)
{
    uint32_t written = 0;
    struct bbuf *b;
    uint64_t i;

    bcache_invalidate(&wide);
    bcache_invalidate(&vast);
    bcache_invalidate(&fake);
    bcache_invalidate(&other);
    bcache_set_limit(16);
    bcache_shrink(4096, 0, &written);
    check(bcache_resident() == 0, "the reserve rows start from an empty cache");
    writes_made = 0;
    check(bcache_reserve(&wide, 4) == 4 && writes_made == 0, "the room to grow is ready");

    for (i = 0; i < 16; i++) {
        dirty_for(&wide, i, 0, 0);
    }
    check(bcache_reserve(&wide, 0) == 0 && writes_made == 0, "a reserve of none writes nothing");
    check(bcache_reserve(&wide, 4) == 4 && writes_made == 1 && last_write_count == 8 * 8,
          "a reserve of four writes one run of dirty victims");
    for (i = 0; i < 4; i++) {
        b = bcache_get_blank(&wide, 100 + i);
        bcache_release(b);
    }
    check(writes_made == 1, "and four misses then write nothing");
    check(dirty_now(&wide, 8) && dirty_now(&wide, 15), "with the later blocks left dirty");
    check(bcache_reserve(&wide, 1000) == 16 && !dirty_now(&wide, 15),
          "a reserve past the limit is cut to the limit");

    for (i = 40; i < 56; i++) {
        dirty_for(&wide, i, 0, 0);
    }
    bcache_sync(&wide);
    dirty_for(&wide, 40, 0, BCACHE_ORDER_AFTER);
    dirty_for(&wide, 50, 0, BCACHE_ORDER_FIRST);
    writes_made = 0;
    check(bcache_reserve(&wide, 15) == 15 && writes_made == 2 && writes_lba[0] == 50 * 8 &&
              writes_lba[1] == 40 * 8,
          "a reserve writes an `after` that is in the way after its device's firsts");

    dirty_for(&wide, 41, 0, 0);
    fail_writes = 1;
    failed_dev = 0;
    check(bcache_reserve(&wide, 16) == 15 && failed_dev == &wide,
          "a failed write ends a reserve with the count it reached");
    fail_writes = 0;
    wide.flags &= ~BLOCK_WRITE_FAILED;
    bcache_sync(&wide);
    bcache_invalidate(&wide);
}

/** Leave `wide` as a sync stopped by a failed write of block 38 leaves it:
 * 39 dirty and `order`, and 40, a block written last, dirty and `last`. */
static void failed_at_38(int order, int last)
{
    uint32_t written = 0;

    dirty_for(&wide, 38, 0, 0);
    dirty_for(&wide, 39, 0, order);
    dirty_for(&wide, 40, 0, last);
    fail_lba = 38 * 8;
    check(bcache_sync(&wide) == -5, "the sync stops at the block that fails");
    bcache_shrink(4096, 0, &written);
    writes_made = 0;
}

/**
 * Check what a reclaim may write on `wide` once a write has failed, on the
 * cache of 16 buffers `reserve_rows` left. A block written last, barred
 * then, stays dirty: a shrink, a reserve and an eviction each leave the
 * `first` an `after` would take with it, and a block no check bars carries
 * it into no run. One barred while an earlier `first` is being written stays
 * too. A sync still retries it.
 */
static void barred_rows(void)
{
    uint32_t written = 0;
    struct bbuf *b;
    uint64_t i;

    bcache_invalidate(&wide);
    bcache_sync_last(&wide, last_block, 0, 0);

    failed_at_38(BCACHE_ORDER_AFTER, BCACHE_ORDER_FIRST);
    bcache_shrink(BCACHE_BUFFERS, 1, &written);
    check(writes_made == 0 && dirty_now(&wide, 39) && dirty_now(&wide, 40),
          "a shrink writes no `after` ahead of a barred `first`");

    failed_at_38(BCACHE_ORDER_AFTER, BCACHE_ORDER_FIRST);
    check(bcache_reserve(&wide, 16) == 13 && writes_made == 0 && dirty_now(&wide, 39) &&
              dirty_now(&wide, 40),
          "and neither does a reserve");

    fail_lba = 0;
    check(bcache_sync(&wide) == 0 && writes_made == 3 && writes_lba[0] == 38 * 8 &&
              writes_lba[1] == 40 * 8 && writes_lba[2] == 39 * 8,
          "a sync still retries it, in order");

    failed_at_38(0, 0);
    check(bcache_reserve(&wide, 16) == 14 && writes_made == 1 && writes_lba[0] == 39 * 8 &&
              last_write_count == 8 && dirty_now(&wide, 40),
          "a reserve's run leaves a barred block beside it");
    fail_lba = 0;
    check(bcache_sync(&wide) == 0, "and a sync writes it");

    failed_at_38(BCACHE_ORDER_AFTER, BCACHE_ORDER_FIRST);
    for (i = 50; i < 63; i++) {
        dirty_for(&wide, i, 0, BCACHE_ORDER_AFTER);
    }
    writes_made = 0;
    b = bcache_get_blank(&wide, 100);
    check(!b && writes_made == 0 && dirty_now(&wide, 39) && dirty_now(&wide, 40),
          "a miss on a cache of such blocks evicts none");
    if (b) {
        bcache_release(b);
    }
    fail_lba = 0;
    check(bcache_sync(&wide) == 0, "until a sync has written them");

    wide.flags &= ~BLOCK_WRITE_FAILED;
    dirty_for(&wide, 39, 0, BCACHE_ORDER_AFTER);
    dirty_for(&wide, 40, 0, BCACHE_ORDER_FIRST);
    dirty_for(&wide, 30, 0, BCACHE_ORDER_FIRST);
    failed_during_write = 1;
    writes_made = 0;
    bcache_shrink(BCACHE_BUFFERS, 1, &written);
    check(writes_made == 1 && writes_lba[0] == 30 * 8 && dirty_now(&wide, 39) &&
              dirty_now(&wide, 40),
          "a `first` barred while an earlier one is written stays dirty");
    check(bcache_sync(&wide) == 0, "and a sync writes it");

    bcache_sync_last(&wide, 0, 0, 0);
    wide.flags &= ~BLOCK_WRITE_FAILED;
    bcache_invalidate(&wide);
}

/* What another process does at a service point, while a row sets it, and how
 * many service points have passed. */
static void (*service_hook)(void);
static int service_points;
static int give_way_skips;

/** Count the point and run `service_hook`, which stands for what runs when
 * the write lock is let go there. */
void sched_service_point(void)
{
    check(kirq_enabled(), "a victim search never reaches a service point with interrupts off");
    service_points++;
    if (service_hook) {
        service_hook();
    }
}

/** Give way with interrupts on, as the kernel does; count the skipped
 * points with interrupts off so a row proves its search reaches them. */
void sched_give_way(void)
{
    if (kirq_enabled()) {
        sched_service_point();
    } else {
        give_way_skips++;
    }
}

/* The device of the sort rows: more blocks than a sort passes between two
 * service points, 4096, and a batch of them, so that a sort reaches one. It
 * keeps no bytes, only the order of its writes. */
#define SORT_BLOCKS 4200u
#define SORT_AFTER  2000u /* an `after`, inside the batch */
#define SORT_LAST   4300u /* a block written last, past the batch's plain ones */
#define SORT_FIRST  4400u /* the `first` a service point dirties, of that class too */
#define SORT_NEXT   4500u /* a plain block the service point writes and drops */
#define SORT_DEVICE 4600u
static uint32_t sort_at[SORT_DEVICE];
static uint32_t sort_writes;

/** Record that the write numbered next in `sort_writes` carries each block
 * of its range. */
static int sort_write(block_device_t *d, uint64_t lba, uint32_t count, const void *buf)
{
    uint64_t blk;

    (void)d;
    (void)buf;
    sort_writes++;
    for (blk = lba / 8; blk < (lba + count) / 8 && blk < SORT_DEVICE; blk++) {
        sort_at[blk] = sort_writes;
    }
    return 0;
}

/** The read of the sort device, which no row makes. */
static int sort_read(block_device_t *d, uint64_t lba, uint32_t count, void *buf)
{
    (void)d;
    (void)lba;
    (void)count;
    (void)buf;
    check(0, "the sort rows read nothing");
    return -5;
}

static block_device_t sorted = {
    512, SORT_DEVICE * 8u, BLOCK_FAULT_SAFE, 0, 0, 2048, sort_read, sort_write, 0, 0, 0, 0};

/** Say that no block written last names the plain blocks' owner, 9, so that
 * only the sync's hold, and no block's wait, keeps them ahead. */
static int sort_names(void *arg, uint64_t blk, uint64_t owner)
{
    (void)arg;
    (void)blk;
    return owner != 9;
}

/** Name the blocks `SORT_LAST` and `SORT_FIRST` as those a sync writes last. */
static int sort_last(void *arg, uint64_t blk)
{
    (void)arg;
    return blk == SORT_LAST || blk == SORT_FIRST;
}

/**
 * What a punch of another process does at the service point of the first
 * sort, once: a pointer cleared in a block written last, dirtied `first`,
 * and the bitmap's `after` marked after it, the older dirty block
 * `SORT_AFTER`.
 */
static void sort_first_hook(void)
{
    struct process *was = current_proc;

    service_hook = 0;
    current_proc = hook_proc;
    dirty_for(&sorted, SORT_FIRST, 0, BCACHE_ORDER_FIRST);
    dirty_for(&sorted, SORT_AFTER, 0, BCACHE_ORDER_AFTER);
    current_proc = was;
}

/** What a sync of one block does at the service point of a sort, once: it
 * writes `SORT_NEXT`, which the sort's batch holds, and the cache lets the
 * buffer go. */
static void sort_taken_hook(void)
{
    struct process *was = current_proc;

    service_hook = 0;
    current_proc = hook_proc;
    check(bcache_sync_block(&sorted, SORT_NEXT) == 0 && sort_at[SORT_NEXT] == 1,
          "the service point writes a block of the batch");
    current_proc = was;
}

/** Empty the sort device, mark `SORT_BLOCKS` plain blocks of owner 9 dirty
 * from block 0, and `SORT_NEXT`, and set the cache to hold them with room to
 * spare. */
static void sort_setup(void)
{
    uint32_t i;

    bcache_set_limit(SORT_DEVICE + 64);
    __builtin_memset(sort_at, 0, sizeof(sort_at));
    sort_writes = 0;
    for (i = 0; i < SORT_BLOCKS; i++) {
        dirty_for(&sorted, i, 9, 0);
    }
    dirty_for(&sorted, SORT_NEXT, 9, 0);
}

/** The last write of any of the plain blocks the sort rows dirty. */
static uint32_t sort_plain_end(void)
{
    uint32_t i, end = 0;

    for (i = 0; i < SORT_BLOCKS; i++) {
        if (i != SORT_AFTER && sort_at[i] > end) {
            end = sort_at[i];
        }
    }
    return end;
}

/**
 * The sort of a sync's batch lets the write lock go at its service points
 * (`gather_dirty`), past the sync's look at the dirty `first` blocks. A
 * `first` dirtied there, of the class written last, with an `after` marked
 * behind it, is one the sync that holds the last blocks must see: it sorts
 * again without that `after`, writes every plain block, and only then takes
 * the `first`, then the `after` and the last block, which go in that order.
 * The plain blocks are of an owner no block written last names, so that only
 * the hold keeps them ahead: without the second sort the `after` in the batch
 * would take the `first` to the device among them.
 *
 * A block a service point writes while it is in the batch is not written
 * again.
 */
static void sort_rows(void)
{
    uint32_t plain_end;

    hook_proc = &proc_b;
    bcache_sync_last(&sorted, sort_last, sort_names, 0);
    sort_setup();
    dirty_for(&sorted, SORT_AFTER, 0, BCACHE_ORDER_AFTER);
    dirty_for(&sorted, SORT_LAST, 0, 0);
    service_points = 0;
    service_hook = sort_first_hook;
    check(bcache_sync(&sorted) == 0, "a sync over a sort that a `first` is dirtied in ends");
    plain_end = sort_plain_end();
    check(service_hook == 0 && service_points == 2,
          "the batch is gathered and sorted again past the `first`, two service points in all");
    check(plain_end != 0 && sort_at[SORT_AFTER] > plain_end,
          "the `after` is left out of the second batch and follows every plain block");
    check(plain_end != 0 && sort_at[SORT_FIRST] > plain_end,
          "the `first`, of the class written last, follows every plain block");
    check(sort_at[SORT_AFTER] > sort_at[SORT_FIRST], "the `after` follows the `first`");
    check(sort_at[SORT_LAST] > sort_at[SORT_FIRST], "and so does the block written last");
    check(sort_at[SORT_LAST] != sort_at[SORT_AFTER], "in a request of its own");

    sort_setup();
    service_points = 0;
    service_hook = sort_taken_hook;
    check(bcache_sync(&sorted) == 0, "a sync over a sort that takes a block of its batch ends");
    check(service_hook == 0 && service_points == 1 && sort_at[SORT_NEXT] == 1,
          "and leaves that block as the service point wrote it, with no second write");
    check(sort_plain_end() != 0 && bcache_sync(&sorted) == 0 && bcache_invalidate(&sorted) == 0,
          "the sort rows leave nothing dirty");
    bcache_sync_last(&sorted, 0, 0, 0);
    bcache_set_limit(BCACHE_BUFFERS);
}

/** The rows of a file's own writeback, of the blocks written last and what
 * they name, of pointer blocks, of the reserve, of a reclaim after a failed
 * write and of a sort's service point. */
static void writeback_rows(void)
{
    owned_rows();
    shared_rows();
    ordered_rows();
    last_rows();
    follow_rows();
    pointer_order_rows();
    pointer_wait_rows();
    reserve_rows();
    barred_rows();
    sort_rows();
}

/**
 * Run the rows in order, and return the count of failed checks, at most 255;
 * with the argument `reader`, run `reader_rows` alone.
 *
 * A get reads a block by one device read and holds it, and a second get is
 * a counted hit. A blank get of an uncached block makes no read and returns
 * zeros, and of a cached block returns its bytes. The short last block is
 * read as the 4 sectors that exist; a block past the device, a block number
 * that would wrap the sector arithmetic, and a failed read return nothing.
 * Invalidation is EBUSY while a buffer is held or dirty, and then drops
 * every buffer of the device.
 *
 * A sync writes dirty buffers in ascending block order and keeps a buffer
 * dirty across a failed write. A write-through (`bcache_write`) reaches the
 * device before it returns, in call order, and leaves the buffer clean. A
 * dirty victim is written before its buffer is reused.
 *
 * The order a file system asks for: at an eviction a `first` reaches the
 * device before an `after` of the same device, though the `after` was
 * released earlier and is the older. An `after` is not evicted while another
 * process holds a `first` of its device; that row makes twice the cache's
 * worth of misses, so every unheld buffer the rule does not protect is
 * replaced at least once. A sync writes the firsts, then the rest in block
 * order, and clears the marks.
 *
 * A failed write is recorded against the device and its sector through
 * `vfs_device_failed`. The buffer stays dirty and is not reused until a
 * later sync writes it, and the device takes no write-through (EROFS) until
 * it is invalidated. A torn write and a dropped write that the device
 * reports as success leave the prefix new or the whole block old, which the
 * cache cannot see; judging the disk afterwards is the volume walk's work.
 *
 * Replacement is least recently released: with every buffer filled from
 * `other` and its block 0 got again, the next miss evicts its block 1.
 *
 * The hold rules panic, each in a forked child: a lower block, or the held
 * block again, while holding block 3; a block of an unrelated device; a
 * block of the device above while holding one of the device under it (the
 * other way round is allowed, since that is what the upper device's read
 * does); a release, a dirty mark or a write-through by a process that does
 * not hold the buffer; a fifth held buffer. A sync panics when the caller
 * holds a dirty block of the device it syncs, or holds a block of an
 * unrelated device while it syncs one that stands on another: the first
 * would wait for itself, and the second would write through a file system
 * the caller is already inside. While the caller holds a mount's lock, an
 * eviction passes over the dirty block of a device that stands on another,
 * since writing it takes the lock of the mount holding its file and two
 * callers could then wait for each other. A block keyed by another caller
 * while a victim's writeback is inside the device is the buffer the get
 * returns. Another process's holds do not count against this one's rules.
 *
 * A device that has gone: an unheld clean buffer is dropped, a dirty one
 * records the device failed and is dropped, since nothing will write it,
 * and a held one is marked and dropped at its release. A buffer whose
 * device goes during its fill, or during its writeback as a dirty victim,
 * carries no discard mark into its next use on a sound device, where the
 * mark would throw away a dirty block at its release.
 *
 * The runs, on `wide`: the miss at the block after a first miss reads a run
 * of eight blocks in one request, and the seven after it are hits. A sync
 * writes adjacent dirty blocks in requests of eight, and writes more dirty
 * blocks than one batch gathers, all of them, in order. A write that lands
 * while a run read is inside the device is not overwritten by the fill's
 * older copy. A `first` dirtied while a sync writes an `after` is written
 * before an `after` marked after it, whatever their block numbers, and is
 * otherwise the next sync's, as is any block dirtied during a sync. On
 * `vast`, with a
 * limit of 600 so that half of it holds a run of 256, the run after a first
 * miss is 32 blocks in one request, Linux's readahead window at its most,
 * though the device takes 256 at once, a reader in order goes on in runs of
 * 32, and the block past the last starts a run of the device's remainder;
 * 300 dirty blocks are written in a run of 256 and one of 44. On a cache cut
 * to 40 buffers with 21 held by six other processes, four or fewer each, a
 * read run is bounded to half the cache, and its last blocks evict its own
 * first blocks, not the caller's block, though that is the least recently
 * released. A write run of 40 blank dirty blocks goes out in two runs of
 * 20, not one that holds every buffer while the request sleeps. The rows of
 * a file's own writeback, of the blocks written last and what they name, of
 * the reserve, of a reclaim after a failed write and of a sort follow
 * (`writeback_rows`), and the rows of what other callers do while a walk
 * has let `cache_spin` go end the run (`pause_rows`).
 */
/** Fill the cache of forty with blocks 1 to 40 of `vast`, the first `clean`
 * of them clean and the rest dirty, as a caller with the big lock. */
static void fill_forty(unsigned clean)
{
    uint32_t written = 0;
    unsigned i;

    (void)bcache_sync(&vast);
    (void)bcache_invalidate(&vast);
    (void)bcache_shrink(100000, 1, &written);
    bcache_set_limit(40);
    for (i = 1; i <= 40; i++) {
        struct bbuf *b = bcache_get_blank(&vast, (uint64_t)i);

        if (i > clean) {
            bcache_dirty(b);
        }
        bcache_release(b);
    }
}

/**
 * The calls of a reader without the big lock: a hit takes no lock, a miss
 * that finds a clean buffer near the head of the release order chooses and
 * keys it and reads the device with neither the big lock nor the write lock,
 * and writes nothing, and a miss that finds only dirty buffers takes the
 * write lock before it chooses, to write one, and gives it back; none takes
 * the big lock. Whether the victim was keyed at the first take of the write
 * lock tells the two misses apart (`take_keyed`).
 */
static void reader_rows(void)
{
    struct bbuf *b;

    fill_forty(10);
    image_vast[300 * 4096] = 0x5A;
    big_held = 0;
    big_takes = 0;
    wb_takes = 0;
    writes_made = 0;
    reads_made = 0;
    reads_unlocked = 0;
    b = bcache_get(&vast, 300);
    check(b != 0 && big_takes == 0 && wb_takes == 0 && writes_made == 0 && reads_made == 1 &&
              reads_unlocked == 1 && b->data[0] == 0x5A,
          "a reader's miss takes a clean buffer with no write and reads with no lock");
    bcache_release(b);
    b = bcache_get(&vast, 300);
    check(b != 0 && big_takes == 0 && wb_takes == 0 && reads_made == 1,
          "and its hit takes no lock");
    bcache_release(b);
    big_held = 1;

    fill_forty(0);
    big_held = 0;
    big_takes = 0;
    wb_takes = 0;
    writes_made = 0;
    take_dev = &vast;
    take_blk = 300;
    take_keyed = -1;
    b = bcache_get(&vast, 300);
    check(b != 0 && wb_takes > 0 && wb_depth == 0 && writes_made == 1 && big_takes == 0 &&
              big_held == 0,
          "a reader's miss among dirty buffers takes the write lock to write one, and gives it "
          "back");
    check(take_keyed == 0, "and takes the write lock before it chooses its victim");
    take_dev = 0;
    bcache_release(b);
    big_held = 1;
    (void)bcache_sync(&vast);
    (void)bcache_invalidate(&vast);
}

/** Fill the small and big images with their sector numbers and name the two
 * processes; 1 when the arguments ask for `reader_rows` alone. */
static int setup(int argc, char **argv)
{
    unsigned i;

    bcache_set_limit(BCACHE_BUFFERS);
    for (i = 0; i < sizeof(image_small); i++) {
        image_small[i] = (unsigned char)(i / 512u);
    }
    for (i = 0; i < sizeof(image_big); i++) {
        image_big[i] = (unsigned char)(i / 512u);
    }
    proc_a.pid = 1;
    proc_b.pid = 2;
    return argc > 1 && argv[1][0] == 'r';
}

/* The buffer `reclaim_after_release` stands for a waiter of, the wakes made
   when it ran, and the buffers its shrink gave back. */
static struct bbuf *reclaim_buffer;
static int wakes_at_reclaim;
static uint32_t reclaimed;

/** The release's give of `cache_spin` seen by another processor: the
 *  waiter `reclaim_buffer` counted finds it unheld and leaves, and a
 *  shrink under the big lock takes `cache_spin` at once and frees every clean
 *  buffer nobody holds or waits for, as a reclaim would. */
static void reclaim_after_release(void)
{
    uint32_t written = 0;

    wakes_at_reclaim = wakes_made;
    reclaim_buffer->waiters--;
    bkl_enter();
    reclaimed = bcache_shrink(BCACHE_BUFFERS, 0, &written);
    bkl_exit();
}

/**
 * The rows of a release's wake: a release wakes the queue of a buffer with
 * a waiter before it lets `cache_spin` go, since a shrink that takes the
 * lock after may free the buffer once its waiter has left, and kicks the
 * processors the wake reported only after; a release of a buffer nobody
 * waits for wakes nothing.
 */
static void release_wake_rows(void)
{
    struct bbuf *b;
    wait_queue_t *queue;

    bcache_invalidate(&fake);
    wakes_made = 0;
    b = bcache_get(&fake, 3);
    bcache_release(b);
    check(b != 0 && wakes_made == 0, "a release of a buffer nobody waits for wakes nothing");

    b = bcache_get(&fake, 3);
    if (!b) {
        check(0, "the buffer of the wake rows is read");
        return;
    }
    queue = &b->wq;
    b->waiters++; /* a sleeper, counted as `wait_unheld` counts it */
    reclaim_buffer = b;
    reclaimed = 0;
    last_kicked = 0;
    wakes_at_reclaim = -1;
    restore_hook = reclaim_after_release;
    bcache_release(b);
    check(restore_hook == 0, "the release gives `cache_spin` back");
    check(reclaimed > 0 && !bcache_cached(&fake, 3),
          "a shrink as the lock goes frees the buffer whose waiter left");
    check(wakes_made == 1 && last_woken == queue, "the release wakes the buffer's queue once");
    check(wakes_at_reclaim == 1, "before the lock goes and the shrink can free the buffer");
    check(last_kicked == WAKE_CPU, "and kicks the processor the wake reported after it");
}

/* The rows of the pauses. Each stands for another processor that takes
 * `cache_spin` the moment a walk lets it go, through `restore_hook`, or for
 * a fill that comes as a write lets its buffer go, through `write_hook`. */

/** A fill that takes the buffer the reclaim has just written and let go,
 *  and empties it again, as `fill_run` does after a write it overtook:
 *  here the invalidate of the buffer's device, which empties it too. */
static void empty_after_let_go(void)
{
    (void)bcache_invalidate(&fake);
}

/** The count of the write as it gives `cache_spin` back; the next give is
 *  the write's release, where the fill comes. */
static void after_wrote(void)
{
    restore_hook = empty_after_let_go;
}

/** The end of the reclaim's write inside the device. */
static void arm_after_write(void)
{
    restore_hook = after_wrote;
}

/**
 * A reclaim that writes a dirty buffer, which a fill takes and empties again
 * as the write lets it go, leaves the buffer on the empties and does not
 * free it, and the next miss takes it from there.
 */
static void reclaim_empty_row(void)
{
    uint32_t written = 0;
    struct bbuf *b, *c;

    (void)bcache_shrink(100000, 1, &written);
    b = bcache_get_blank(&fake, 4);
    bcache_dirty(b);
    bcache_release(b);
    check(bcache_resident() == 1, "the reclaim row starts with one dirty buffer");
    written = 0;
    write_hook = arm_after_write;
    (void)bcache_shrink(BCACHE_BUFFERS, 1, &written);
    check(write_hook == 0 && restore_hook == 0, "the fill comes as the reclaim's write lets go");
    check(written == 0 && bcache_resident() == 1 && !bcache_cached(&fake, 4),
          "a reclaim leaves a buffer a fill emptied after its write, on the empties");
    if (bcache_resident() == 1) {
        c = bcache_get_blank(&fake, 5);
        check(c == b, "and the next miss takes it from there");
        bcache_release(c);
    }
    restore_hook = 0;
    (void)bcache_invalidate(&fake);
}

/* The block of `vast` the victim rows hit and let go at a search's pauses,
 * the processes that hold the rest of a cache of forty, the buffers they
 * hold and how many, and the pauses at which the block is hit still to
 * come. */
#define PAUSE_X 36u
static process_t pause_holders[10];
static struct bbuf *pause_held[40];
static unsigned pause_count;
static int pause_hits;
/* Dirty `PAUSE_X` at every hit, with a write that is not barred. */
static int pause_dirty;

/** Another process hits block `PAUSE_X` of `vast` and lets it go, which
 *  moves it to the tail of the release order; again at the next give of
 *  `cache_spin` while `pause_hits` lasts and the block is cached. */
static void hit_x(void)
{
    struct process *was = current_proc;
    struct bbuf *x;

    if (!bcache_cached(&vast, PAUSE_X)) {
        pause_hits = 0;
        return;
    }
    current_proc = &proc_b;
    x = bcache_get_blank(&vast, PAUSE_X);
    if (x) {
        if (pause_dirty) {
            x->data[0] = 0xa7;
            bcache_dirty(x);
        }
        bcache_release(x);
    }
    current_proc = was;
    if (--pause_hits > 0) {
        restore_hook = hit_x;
    }
}

/** Fill the cache of forty with blocks 1 to 40 of `vast`, each held by one
 *  of ten other processes, four at most each; with `with_x`, block
 *  `PAUSE_X` is let go instead, and the 35 blocks before it come ahead of
 *  it in the release order. */
static void pause_fill(int with_x)
{
    uint32_t written = 0;
    unsigned i;

    (void)bcache_sync(&vast);
    (void)bcache_invalidate(&vast);
    (void)bcache_shrink(100000, 1, &written);
    bcache_set_limit(40);
    pause_count = 0;
    for (i = 1; i <= 40; i++) {
        if (with_x && i == PAUSE_X) {
            bcache_release(bcache_get_blank(&vast, i));
            continue;
        }
        pause_holders[pause_count / 4].pid = 70 + (int)(pause_count / 4);
        current_proc = &pause_holders[pause_count / 4];
        pause_held[pause_count++] = bcache_get_blank(&vast, i);
    }
    current_proc = &proc_a;
    check(bcache_resident() == 40, "the victim rows fill the cache of forty");
}

/** Let go of the buffers `pause_fill` gave the other processes. */
static void pause_release(void)
{
    unsigned i;

    for (i = 0; i < pause_count; i++) {
        current_proc = &pause_holders[i / 4];
        bcache_release(pause_held[i]);
    }
    current_proc = &proc_a;
    pause_count = 0;
}

/**
 * A miss under the big lock on a cache whose every buffer but one is held,
 * and that one dirty with its write failed: hit and let go at every pause of
 * the search, it stays unfit, so its releases are no progress, and the miss
 * fails after its sixteen retries while the hits go on. Each retry follows
 * a service point, where the write lock may go to a writer; the seventeenth
 * turn, which fails, gives way to nobody.
 */
static void victim_unfit_rows(void)
{
    struct bbuf *b;
    int points;

    pause_fill(1);
    b = bcache_get(&vast, PAUSE_X);
    b->data[0] = 0x5a;
    bcache_dirty(b);
    bcache_release(b);
    fail_writes = 1;
    (void)bcache_sync(&vast);
    b = bcache_get(&vast, PAUSE_X);
    check(b->dirty && b->write_failed,
          "the unfit row's one free buffer is dirty, its write failed");
    bcache_release(b);
    points = service_points;
    pause_hits = 1000;
    restore_hook = hit_x;
    b = bcache_get_blank(&vast, 100);
    check(b == 0 && pause_hits > 0,
          "a miss fails while an unfit buffer is hit and let go at each pause of its search");
    check(service_points - points == 16, "and each of its sixteen retries follows a service point");
    restore_hook = 0;
    pause_hits = 0;
    if (b) {
        bcache_release(b);
    }
    pause_release();
    fail_writes = 0;
    (void)bcache_sync(&vast);
    (void)bcache_invalidate(&vast);
}

/**
 * Dirty releases behind a search reset its no-progress count, as Linux's
 * should_reclaim_retry counts dirty reclaimable pages (mm/page_alloc.c:
 * 4179-4185, 4206-4219 in 6.12.115). Sixty pauses move the unbarred dirty
 * buffer behind the search, beyond sixteen idle retries; the next turn
 * writes and takes it. With all buffers held and interrupts off, a miss
 * instead fails without a service point, as Linux's __cond_resched
 * (kernel/sched/core.c:7287-7292).
 */
static void victim_dirty_and_irq_rows(void)
{
    struct bbuf *b;
    uint32_t flags;
    int points, skips;

    pause_fill(1);
    pause_hits = 60;
    pause_dirty = 1;
    writes_made = 0;
    restore_hook = hit_x;
    b = bcache_get_blank(&vast, 100);
    check(b != 0 && pause_hits == 0 && writes_made > 0 &&
              image_vast[PAUSE_X * BCACHE_BLOCK_BYTES] == 0xa7,
          "a dirty release at each of sixty pauses keeps the search going until it writes and "
          "takes it");
    restore_hook = 0;
    pause_dirty = pause_hits = 0;
    if (b) {
        bcache_release(b);
    }
    pause_release();
    (void)bcache_sync(&vast);
    (void)bcache_invalidate(&vast);

    pause_fill(0);
    points = service_points;
    skips = give_way_skips;
    flags = kirq_save();
    b = bcache_get_blank(&vast, 100);
    check(!kirq_enabled() && b == 0 && service_points == points && give_way_skips > skips,
          "an interrupts-off search exhausts its retries without a service point");
    kirq_restore(flags);
    if (b) {
        bcache_release(b);
    }
    pause_release();
    (void)bcache_invalidate(&vast);
}

/**
 * A miss under the big lock on a cache whose every buffer but one is held:
 * the one fit buffer, hit and let go at the search's first pause, goes
 * behind the buffers the turn passed over, and the next turn takes it. Hit
 * and let go at each of sixty pauses, more than the sixteen retries of a
 * turn with nothing let go, it stays behind every turn, and the miss still
 * takes it once the hits stop, since a turn with a release is progress. A
 * miss with every buffer held fails once its turns let nothing go.
 */
static void victim_pause_rows(void)
{
    struct bbuf *b;

    pause_fill(1);
    pause_hits = 1;
    restore_hook = hit_x;
    b = bcache_get_blank(&vast, 100);
    check(b != 0 && pause_hits == 0,
          "a miss takes the one fit buffer, hit and let go at its search's first pause");
    restore_hook = 0;
    if (b) {
        bcache_release(b);
    }
    pause_release();

    pause_fill(1);
    pause_hits = 60;
    restore_hook = hit_x;
    b = bcache_get_blank(&vast, 100);
    check(b != 0 && pause_hits == 0,
          "and one hit and let go at each of sixty pauses, once the hits stop");
    restore_hook = 0;
    pause_hits = 0;
    if (b) {
        bcache_release(b);
    }
    pause_release();

    pause_fill(0);
    b = bcache_get_blank(&vast, 100);
    check(b == 0, "a miss with every buffer held fails once its turns let nothing go");
    if (b) {
        bcache_release(b);
    }
    pause_release();
    (void)bcache_invalidate(&vast);
    victim_unfit_rows();
    victim_dirty_and_irq_rows();
}

/* A device of more blocks than the index has buckets, which the index row
 * keys blank and never reads or writes. */
#define ROOMY_BLOCKS 20000u

/** The read of `roomy`, which no row makes. */
static int roomy_read(block_device_t *d, uint64_t lba, uint32_t count, void *buf)
{
    (void)d;
    (void)lba;
    (void)count;
    (void)buf;
    check(0, "the index row reads nothing");
    return -5;
}

/** The write of `roomy`, which no row makes. */
static int roomy_write(block_device_t *d, uint64_t lba, uint32_t count, const void *buf)
{
    (void)d;
    (void)lba;
    (void)count;
    (void)buf;
    check(0, "the index row writes nothing");
    return -5;
}

static block_device_t roomy = {
    512, ROOMY_BLOCKS * 8u, BLOCK_FAULT_SAFE, 0, 0, 0, roomy_read, roomy_write, 0, 0, 0, 0};

/* The index's buckets before the move, the blocks keyed before the one
 * whose keying starts it, and how many of those and that one a lookup
 * during the move found, or -1 before it. */
static uint32_t index_old;
static uint32_t index_keys;
static int index_found;

/** The first give of `cache_spin` once the index has its new table, which
 *  is a pause of the move (`index_fit`): another process looks up every
 *  keyed block, takes one of them, and keys a new one, `index_keys + 1`.
 *  Until then it waits for the next give. */
static void index_mid_move(void)
{
    struct process *was = current_proc;
    uint32_t i;
    int found = 0;

    if (bcache_buckets() == index_old) {
        restore_hook = index_mid_move;
        return;
    }
    for (i = 0; i <= index_keys; i++) {
        found += bcache_cached(&roomy, i);
    }
    current_proc = &proc_b;
    bcache_release(bcache_get_blank(&roomy, 2));
    bcache_release(bcache_get_blank(&roomy, index_keys + 1));
    current_proc = was;
    index_found = found;
}

/**
 * A lookup and a keying while the index moves to a larger table, a bucket
 * of the old one at a time: a lookup finds every keyed block, in whichever
 * table its bucket is, a hit takes one, and a block keyed then is found
 * once the move ends, with every other.
 */
static void index_move_row(void)
{
    uint32_t written = 0, i;
    struct bbuf *b;
    int all = 1;

    (void)bcache_shrink(100000, 1, &written);
    index_old = bcache_buckets();
    if (bcache_resident() != 0 || index_old + 2 > ROOMY_BLOCKS) {
        check(0, "the index row starts with an empty cache and a device past the buckets");
        return;
    }
    bcache_set_limit(index_old + 64);
    index_keys = index_old;
    for (i = 0; i < index_keys; i++) {
        bcache_release(bcache_get_blank(&roomy, i));
    }
    index_found = -1;
    restore_hook = index_mid_move;
    b = bcache_get_blank(&roomy, index_keys);
    restore_hook = 0;
    check(bcache_buckets() > index_old && index_found == (int)index_keys + 1,
          "a lookup while the index moves finds every keyed block");
    for (i = 0; i <= index_keys + 1; i++) {
        all &= bcache_cached(&roomy, i);
    }
    check(all, "and the block keyed during the move is found after it, with every other");
    if (b) {
        bcache_release(b);
    }
    (void)bcache_invalidate(&roomy);
    (void)bcache_shrink(100000, 1, &written);
}

/* The gives of `cache_spin` before `take_behind` takes its block, and the
 * buffer it took. */
static int invalidate_gives;
static struct bbuf *taken_behind;

/** Another process takes block 1 of `vast` at the `invalidate_gives`-th
 *  give of `cache_spin`. */
static void take_behind(void)
{
    struct process *was = current_proc;

    if (--invalidate_gives > 0) {
        restore_hook = take_behind;
        return;
    }
    current_proc = &proc_b;
    taken_behind = bcache_get_blank(&vast, 1);
    current_proc = was;
}

/**
 * A buffer taken while the second walk of an invalidate has let the lock
 * go, ahead of that walk, is marked when the walk reaches it, the call
 * still answers 0, and the buffer is dropped at its release with the rest.
 * With forty buffers, the oldest block 1's at the end of the list of all
 * buffers, the first walk lets the lock go once, after its 32nd buffer, and
 * the second, whose count goes on from the first's 39 steps, after its
 * 25th.
 */
static void invalidate_walk_row(void)
{
    int rc, any = 0;
    unsigned i;

    fill_forty(40);
    check(bcache_resident() == 40, "the invalidate row fills the cache of forty");
    taken_behind = 0;
    invalidate_gives = 2;
    restore_hook = take_behind;
    rc = bcache_invalidate(&vast);
    restore_hook = 0;
    check(rc == 0 && invalidate_gives == 0 && taken_behind != 0,
          "an invalidate whose second walk lets go before a block taken then answers 0");
    if (taken_behind) {
        current_proc = &proc_b;
        bcache_release(taken_behind);
        current_proc = &proc_a;
    }
    for (i = 1; i <= 40; i++) {
        any |= bcache_cached(&vast, i);
    }
    check(!any, "and the block taken is dropped at its release, with the rest");
}

/**
 * The unmount's drop (`bcache_drop`) takes what an invalidate refuses: a
 * dirty buffer is dropped unwritten and counted, a held one stays until its
 * release, which drops it, and the device's failed-write mark is cleared.
 */
static void drop_rows(void)
{
    struct bbuf *held, *b;
    int dropped;

    b = bcache_get_blank(&fake, 3);
    b->data[0] = 0xD0;
    bcache_dirty(b);
    bcache_release(b);
    held = bcache_get(&fake, 5);
    fake.flags |= BLOCK_WRITE_FAILED;
    check(held && bcache_invalidate(&fake) == -16,
          "an invalidate refuses a device with a dirty and a held buffer");
    writes_made = 0;
    dropped = bcache_drop(&fake);
    check(dropped == 1 && writes_made == 0 && !bcache_cached(&fake, 3) &&
              !(fake.flags & BLOCK_WRITE_FAILED),
          "the unmount's drop gives up the dirty buffer unwritten and clears the failed mark");
    check(bcache_cached(&fake, 5), "and keeps the held buffer until its release");
    if (held) {
        bcache_release(held);
    }
    check(!bcache_cached(&fake, 5) && bcache_invalidate(&fake) == 0,
          "which drops it, leaving the device nothing");
}

/** Run the rows of the pauses. */
static void pause_rows(void)
{
    reclaim_empty_row();
    victim_pause_rows();
    index_move_row();
    invalidate_walk_row();
    bcache_set_limit(BCACHE_BUFFERS);
}

int main(int argc, char **argv)
{
    struct bbuf *b, *c, *d;
    unsigned i;

    if (setup(argc, argv)) {
        reader_rows();
        return fails > 255 ? 255 : fails;
    }

    b = bcache_get(&fake, 0);
    check(b != 0, "a block is read and held");
    check(b && b->data[0] == 0 && b->data[513] == 1, "with the device's bytes");
    check(reads_made == 1, "by one device read");
    bcache_release(b);
    b = bcache_get(&fake, 0);
    check(b != 0 && reads_made == 1, "a second get is a hit, not a read");
    check(bcache_stat_hits() == 1, "and the hit is counted");
    bcache_release(b);

    reads_made = 0;
    b = bcache_get_blank(&fake, 5);
    check(b != 0 && reads_made == 0, "a blank get of an uncached block makes no read");
    check(b && b->data[0] == 0 && b->data[4095] == 0, "and the buffer is zeros");
    check(b && b->bytes == 4096, "with the block's whole length");
    bcache_release(b);
    b = bcache_get_blank(&fake, 0);
    check(b != 0 && reads_made == 0 && b->data[513] == 1,
          "a blank get of a cached block returns its bytes");
    bcache_release(b);
    bcache_invalidate(&fake);
    reads_made = 0;

    b = bcache_get(&fake, 12);
    check(b != 0 && b->bytes == 4 * 512, "the short last block is its tail");
    check(last_count == 4, "read as the sectors that exist");
    bcache_release(b);

    check(bcache_get(&fake, 13) == 0, "a block past the device returns nothing");
    check(bcache_get(&fake, (uint64_t)1 << 61) == 0,
          "a block number that would wrap the sector arithmetic returns nothing");

    fail_reads = 1;
    check(bcache_get(&other, 3) == 0, "a failed read returns no buffer");
    fail_reads = 0;

    b = bcache_get(&fake, 5);
    check(bcache_invalidate(&fake) == -16, "invalidation is EBUSY while a buffer is held");
    bcache_release(b);
    check(bcache_invalidate(&fake) == 0, "and drops the device's buffers after");
    reads_made = 0;
    b = bcache_get(&fake, 5);
    bcache_release(b);
    b = bcache_get(&fake, 0);
    bcache_release(b);
    b = bcache_get(&fake, 12);
    bcache_release(b);
    check(reads_made == 3, "so every block of the device is read again");

    b = bcache_get(&fake, 9);
    bcache_dirty(b);
    bcache_release(b);
    b = bcache_get(&fake, 2);
    b->data[0] = 0xAB;
    bcache_dirty(b);
    bcache_release(b);
    check(bcache_invalidate(&fake) == -16, "invalidation is EBUSY while a buffer is dirty");
    fail_writes = 1;
    check(bcache_sync(&fake) == -5, "a failed write returns the device's errno");
    fail_writes = 0;
    writes_made = 0;
    check(bcache_sync(&fake) == 0, "the next sync writes");
    check(writes_made == 2 && writes_lba[0] == 16 && writes_lba[1] == 72,
          "both dirty buffers, lowest block first");
    check(image_small[2 * 4096] == 0xAB, "with the buffer's bytes");
    check(bcache_sync(&fake) == 0 && writes_made == 2, "and nothing is dirty after");
    check(bcache_invalidate(&fake) == 0, "so invalidation succeeds");

    writes_made = 0;
    b = bcache_get(&fake, 6);
    b->data[7] = 0x5A;
    check(bcache_write(b) == 0 && writes_made == 1 && writes_lba[0] == 48 && !b->dirty,
          "a write-through reaches the device before it returns");
    check(image_small[6 * 4096 + 7] == 0x5A, "with the buffer's bytes");
    bcache_release(b);
    c = bcache_get(&fake, 7);
    check(bcache_write(c) == 0 && writes_made == 2 && writes_lba[1] == 56,
          "and a second one follows it in call order");
    current_proc = &proc_b;
    panic_row(write_it, c, "does not hold",
              "a write-through by a process that does not hold the buffer panics");
    current_proc = &proc_a;
    bcache_release(c);

    b = bcache_get(&fake, 8);
    b->data[0] = 0x77;
    bcache_dirty(b);
    bcache_release(b);
    writes_made = 0;
    for (i = 0; i < BCACHE_BUFFERS; i++) {
        c = bcache_get(&other, i);
        bcache_release(c);
    }
    check(writes_made == 1 && writes_lba[0] == 64 && image_small[8 * 4096] == 0x77,
          "a dirty victim is written before its buffer is reused");

    b = bcache_get(&fake, 11);
    b->data[0] = 0x11;
    bcache_dirty_ordered(b, BCACHE_ORDER_AFTER);
    bcache_release(b);
    b = bcache_get(&fake, 10);
    b->data[0] = 0x10;
    bcache_dirty_ordered(b, BCACHE_ORDER_FIRST);
    bcache_release(b);
    writes_made = 0;
    for (i = 0; i < BCACHE_BUFFERS; i++) {
        c = bcache_get(&other, i);
        bcache_release(c);
    }
    check(writes_made == 2 && writes_lba[0] == 80 && writes_lba[1] == 88,
          "an evicted after buffer is written after the device's first, the older though it is");
    check(image_small[10 * 4096] == 0x10 && image_small[11 * 4096] == 0x11,
          "with both buffers' bytes");
    b = bcache_get(&fake, 11);
    b->data[0] = 0x22;
    bcache_dirty_ordered(b, BCACHE_ORDER_AFTER);
    bcache_release(b);
    current_proc = &proc_b;
    d = bcache_get(&fake, 10);
    d->data[0] = 0x33;
    bcache_dirty_ordered(d, BCACHE_ORDER_FIRST);
    current_proc = &proc_a;
    writes_made = 0;
    for (i = 0; i < 2 * BCACHE_BUFFERS; i++) {
        c = bcache_get(&other, i % (BIG_SECTORS / 8));
        bcache_release(c);
    }
    b = bcache_get(&fake, 11);
    check(writes_made == 0 && b->dirty && b->data[0] == 0x22,
          "an after buffer is not evicted while another process holds a first of its device");
    bcache_release(b);
    current_proc = &proc_b;
    bcache_release(d);
    current_proc = &proc_a;
    b = bcache_get(&fake, 4);
    b->data[0] = 0x44;
    bcache_dirty_ordered(b, BCACHE_ORDER_AFTER);
    bcache_release(b);
    writes_made = 0;
    check(bcache_sync(&fake) == 0 && writes_made == 3 && writes_lba[0] == 80 &&
              writes_lba[1] == 32 && writes_lba[2] == 88,
          "a sync writes the device's firsts before the rest, and the rest in block order");
    check(image_small[10 * 4096] == 0x33 && image_small[11 * 4096] == 0x22 &&
              image_small[4 * 4096] == 0x44,
          "with every buffer's bytes");
    b = bcache_get(&fake, 4);
    check(!b->dirty && b->order == 0, "and the marks clear with the write");
    bcache_release(b);

    b = bcache_get(&fake, 9);
    b->data[0] = 0x66;
    bcache_dirty(b);
    bcache_release(b);
    fail_writes = 1;
    failed_dev = 0;
    check(bcache_sync(&fake) == -5 && failed_dev == &fake && failed_lba == 72 && failed_rc == -5,
          "a failed writeback is recorded against the device and its sector");
    writes_made = 0;
    for (i = 0; i < BCACHE_BUFFERS; i++) {
        c = bcache_get(&other, i);
        bcache_release(c);
    }
    b = bcache_get(&fake, 9);
    check(b->dirty && b->data[0] == 0x66, "a dirty buffer whose write fails is not reused");
    bcache_release(b);
    fail_writes = 0;
    check(bcache_sync(&fake) == 0 && image_small[9 * 4096] == 0x66, "and a later sync writes it");
    b = bcache_get(&fake, 3);
    check(bcache_write(b) == -30,
          "a device that failed a write takes no metadata write until it is dropped");
    bcache_release(b);
    check(bcache_invalidate(&fake) == 0, "the device is dropped clean");
    b = bcache_get(&fake, 3);
    check(bcache_write(b) == 0, "and the next mount's first metadata write goes through");
    bcache_release(b);

    b = bcache_get(&fake, 10);
    __builtin_memset(b->data, 0x99, 4096);
    tear_at = writes_made + 1;
    tear_bytes = 512;
    check(bcache_write(b) == 0, "a torn write is reported as success");
    check(image_small[10 * 4096] == 0x99 && image_small[10 * 4096 + 512] == 81,
          "and leaves the prefix new and the rest old on the device");
    tear_at = -1;
    drop_at = writes_made + 1;
    __builtin_memset(b->data, 0x88, 4096);
    check(bcache_write(b) == 0 && image_small[10 * 4096] == 0x99,
          "a dropped write leaves the device as it was");
    drop_at = -1;
    bcache_release(b);

    for (i = 0; i < BCACHE_BUFFERS; i++) {
        b = bcache_get(&other, i);
        check(b != 0, "a fill row within the big device");
        bcache_release(b);
    }
    b = bcache_get(&other, 0);
    bcache_release(b);
    reads_made = 0;
    b = bcache_get(&fake, 5);
    check(b != 0 && reads_made == 1, "a full cache still serves a miss");
    bcache_release(b);
    b = bcache_get(&other, 0);
    check(b != 0 && reads_made == 1, "the refreshed block survived");
    bcache_release(b);
    b = bcache_get(&other, 1);
    check(b != 0 && reads_made == 2, "and the oldest release was the victim");
    bcache_release(b);

    {
        uint64_t two = 2, three = 3, nine = 9;

        b = bcache_get(&fake, 3);
        panic_row(get_fake, &two, "not above", "a lower block while holding a higher one panics");
        panic_row(get_fake, &three, "not above", "the held block taken again panics");
        panic_row(get_other, &two, "not under",
                  "a block on another device while holding one panics");
        bcache_release(b);
        nested.backing = &fake;
        check(block_depth(&nested) == BLOCK_MAX_DEPTH && block_depth(&fake) == 0,
              "a device says how many are under it");
        check(block_is_below(&fake, &nested) && !block_is_below(&nested, &fake),
              "and which way round they are");
        {
            struct bbuf *n = bcache_get(&nested, 5);
            struct bbuf *under;

            check(n != 0, "a block of the device above is taken");
            under = bcache_get(&fake, 1);
            check(under != 0, "and a block of the one under it, which is what its read does");
            bcache_release(under);
            bcache_release(n);
        }
        {
            uint64_t five = 5;

            b = bcache_get(&fake, 1);
            panic_row(get_nested, &five, "not under",
                      "a block of the device above while holding one under it panics");
            bcache_release(b);
        }
        {
            struct bbuf *dirty;
            int i2;

            bcache_invalidate(&nested);
            bcache_invalidate(&other);
            dirty = bcache_get(&nested, 7);
            check(dirty != 0, "a block of the device above is taken");
            bcache_dirty(dirty);
            bcache_release(dirty);
            ns_held = 1;
            writes_made = 0;
            for (i2 = 0; i2 < BCACHE_BUFFERS; i2++) {
                struct bbuf *c2 = bcache_get(&other, (uint64_t)i2);

                if (c2) {
                    bcache_release(c2);
                }
            }
            check(writes_made == 0,
                  "and its dirty block is passed over while a mount's lock is held");
            ns_held = 0;
            bcache_sync(&nested);
            bcache_invalidate(&nested);
            bcache_invalidate(&other);
        }
        {
            uint64_t last = BCACHE_BUFFERS;
            struct bbuf *b2;
            int i2;

            bcache_invalidate(&fake);
            bcache_invalidate(&other);
            for (i2 = 0; i2 < BCACHE_BUFFERS; i2++) {
                b2 = bcache_get(&other, (uint64_t)i2);
                check(b2 != 0, "a buffer is taken to fill the cache");
                if (i2 == 0) {
                    bcache_dirty(b2);
                }
                if (b2) {
                    bcache_release(b2);
                }
            }
            fill_during_write = last;
            b2 = bcache_get(&other, last);
            check(b2 != 0 && b2->data[0] == 0xAB,
                  "a block keyed while the victim's write ran is the one given back");
            if (b2) {
                bcache_release(b2);
            }
            bcache_invalidate(&other);
        }
        {
            struct bbuf *held;

            bcache_invalidate(&fake);
            held = bcache_get(&fake, 2);
            check(held != 0, "a block of a device is taken and marked dirty");
            bcache_dirty(held);
            panic_row(sync_it, &fake, "a sync while holding a block of the device",
                      "a sync of the device whose dirty block the caller holds panics");
            bcache_release(held);
            check(bcache_sync(&fake) == 0, "and the same sync writes it once it is let go");

            held = bcache_get(&nested, 9);
            check(held != 0, "a dirty block of a device that stands on another");
            bcache_dirty(held);
            bcache_release(held);
            held = bcache_get(&other, 1);
            check(held != 0, "with a block of an unrelated device held");
            panic_row(sync_it, &nested, "a sync while holding a block of another device",
                      "a sync that would write through a file system already entered panics");
            bcache_release(held);
            check(bcache_sync(&nested) == 0, "and that sync writes once the other is let go");
            bcache_invalidate(&nested);
            bcache_invalidate(&other);
        }

        bcache_invalidate(&nested);
        nested.backing = 0;
        b = bcache_get(&fake, 3);

        current_proc = &proc_b;
        panic_row(release_it, b, "does not hold",
                  "a release by a process that does not hold the buffer panics");
        panic_row(dirty_it, b, "does not hold",
                  "a dirty mark by a process that does not hold the buffer panics");
        current_proc = &proc_a;
        bcache_release(b);

        {
            struct bbuf *held[BCACHE_HOLD_MAX];

            for (i = 0; i < BCACHE_HOLD_MAX; i++) {
                held[i] = bcache_get(&fake, i);
                check(held[i] != 0, "a buffer within the hold limit");
            }
            panic_row(get_fake, &nine, "fifth", "a fifth held buffer panics");
            for (i = 0; i < BCACHE_HOLD_MAX; i++) {
                bcache_release(held[i]);
            }
        }
    }

    b = bcache_get(&fake, 7);
    current_proc = &proc_b;
    c = bcache_get(&fake, 2);
    check(c != 0, "a second process takes a lower block freely");
    bcache_release(c);
    current_proc = &proc_a;
    bcache_release(b);

    bcache_invalidate(&fake);
    failed_dev = 0;
    b = bcache_get(&fake, 1);
    bcache_release(b);
    c = bcache_get(&fake, 2);
    bcache_dirty(c);
    bcache_release(c);
    d = bcache_get(&fake, 3);
    check(bcache_device_holds(&fake) == 1, "one buffer of the device is held");
    bcache_device_gone(&fake);
    check(fake.flags & BLOCK_GONE, "the device is marked gone");
    check(failed_dev == &fake, "a dirty buffer recorded the device as failed");
    check(bcache_device_holds(&fake) == 1, "the held buffer is still held");
    check(d->dev == &fake && d->discard, "and is marked to be dropped at its release");
    check(bcache_invalidate(&fake) == -EBUSY, "the device cannot be dropped while it is held");
    bcache_release(d);
    check(d->dev == 0 && !d->discard, "the release drops the held buffer");
    check(bcache_device_holds(&fake) == 0, "and nothing of the device is held");
    check(bcache_invalidate(&fake) == 0, "and the device can be dropped: nothing dirty is left");
    fake.flags &= ~BLOCK_GONE;

    bcache_invalidate(&fake);
    bcache_invalidate(&other);
    gone_during_read = 1;
    check(bcache_get(&fake, 4) == 0, "a fill on a device that goes in the middle of it fails");
    fake.flags &= ~BLOCK_GONE;
    b = bcache_get(&other, 3);
    check(b != 0, "the next fill on a sound device takes a buffer");
    bcache_dirty(b);
    bcache_release(b);
    check(b->dev == &other && b->dirty, "and its dirty block is kept at the release");
    bcache_sync(&other);
    bcache_invalidate(&other);

    bcache_invalidate(&fake);
    b = bcache_get(&fake, 8);
    bcache_dirty(b);
    bcache_release(b);
    gone_during_write = 1;
    for (i = 0; i < BCACHE_BUFFERS; i++) {
        c = bcache_get(&other, i);
        bcache_dirty(c);
        bcache_release(c);
    }
    fake.flags &= ~BLOCK_GONE;
    writes_made = 0;
    bcache_sync(&other);
    check(writes_made == BCACHE_BUFFERS,
          "a buffer reused after its device went during a writeback keeps its next dirty block");
    bcache_invalidate(&other);

    for (i = 0; i < (int)sizeof(image_wide); i++) {
        image_wide[i] = (unsigned char)(i / 4096u);
    }
    bcache_set_limit(WIDE_BLOCKS + 8);
    reads_made = 0;
    b = bcache_get(&wide, 100);
    bcache_release(b);
    check(reads_made == 1 && last_count == 8, "a first miss on the wide device reads one block");
    b = bcache_get(&wide, 101);
    check(reads_made == 2 && last_count == 64 && b->data[0] == 101,
          "the miss at the next block reads a run of eight in one request, the block among them");
    bcache_release(b);
    for (i = 102; i < 109; i++) {
        b = bcache_get(&wide, (uint64_t)i);
        check(b->data[0] == (unsigned char)i, "and the blocks after it are in the cache");
        bcache_release(b);
    }
    check(reads_made == 2, "with no further read");
    for (i = 200; i < 216; i++) {
        b = bcache_get_blank(&wide, (uint64_t)i);
        b->data[0] = 0x5A;
        bcache_dirty(b);
        bcache_release(b);
    }
    writes_made = 0;
    check(bcache_sync(&wide) == 0 && writes_made == 2 && writes_lba[0] == 1600 &&
              writes_lba[1] == 1664 && last_count == 64,
          "a sync writes sixteen adjacent dirty blocks as two requests of eight");
    check(image_wide[200 * 4096] == 0x5A && image_wide[215 * 4096] == 0x5A, "with their bytes");
    for (i = 0; i < (int)WIDE_BLOCKS; i++) {
        b = bcache_get_blank(&wide, (uint64_t)(WIDE_BLOCKS - 1 - i));
        b->data[1] = 0x3C;
        bcache_dirty(b);
        bcache_release(b);
    }
    writes_made = 0;
    check(bcache_sync(&wide) == 0 && writes_made == WIDE_BLOCKS / 8 && writes_lba[0] == 0 &&
              writes_lba[1] == 64,
          "a sync of more dirty blocks than one batch gathers writes them all, in order");
    {
        int dirty_left = 0;

        for (i = 0; i < (int)WIDE_BLOCKS; i++) {
            b = bcache_get(&wide, (uint64_t)i);
            dirty_left += b->dirty;
            bcache_release(b);
        }
        check(dirty_left == 0 && image_wide[(WIDE_BLOCKS - 1) * 4096 + 1] == 0x3C,
              "and leaves nothing dirty");
    }
    bcache_invalidate(&wide);

    b = bcache_get(&wide, 300);
    bcache_release(b);
    write_during_fill = 305;
    b = bcache_get(&wide, 301);
    bcache_release(b);
    check(write_during_fill == 0, "the write landed during the run read");
    b = bcache_get(&wide, 305);
    check(b->data[0] == 0xEE && image_wide[305 * 4096] == 0xEE,
          "a block written during a run read is not overwritten by the fill's older copy");
    bcache_release(b);
    bcache_invalidate(&wide);

    for (i = 0; i < 3; i++) {
        b = bcache_get_blank(&wide, (uint64_t)(210 + 10 * i));
        b->data[0] = 0xA0;
        bcache_dirty_ordered(b, BCACHE_ORDER_AFTER);
        bcache_release(b);
    }
    hook_proc = &proc_b;
    first_during_sync = 250;
    after_during_sync = 230;
    writes_made = 0;
    check(bcache_sync(&wide) == 0 && first_during_sync == 0 && writes_made == 4,
          "a sync during which a first was dirtied writes all four blocks");
    check(writes_lba[0] == 1680 && writes_lba[1] == 1760 && writes_lba[2] == 2000 &&
              writes_lba[3] == 1840,
          "and the late first goes to the device before the `after` marked after it");
    check(image_wide[230 * 4096] == 0xA1, "with that mark's bytes");
    for (i = 0; i < 2; i++) {
        b = bcache_get_blank(&wide, (uint64_t)(210 + 10 * i));
        b->data[0] = 0xA0;
        bcache_dirty_ordered(b, BCACHE_ORDER_AFTER);
        bcache_release(b);
    }
    first_during_sync = 250;
    writes_made = 0;
    check(bcache_sync(&wide) == 0 && writes_made == 2 && dirty_now(&wide, 250),
          "a first dirtied during a sync is the next one's, and an earlier `after` waits not");
    check(bcache_sync(&wide) == 0 && writes_made == 3 && !dirty_now(&wide, 250), "which writes it");
    bcache_invalidate(&wide);

    write_through_rows();
    bcache_set_limit(BCACHE_BUFFERS);

    check(waited_on_host == 0, "no row reached the sleeping wait");

    for (i = 0; i < (int)sizeof(image_vast); i++) {
        image_vast[i] = (unsigned char)(i / 4096u);
    }
    bcache_set_limit(600);
    reads_made = 0;
    b = bcache_get(&vast, 1);
    bcache_release(b);
    b = bcache_get(&vast, 2);
    check(reads_made == 2 && last_count == 32 * 8 && b->data[0] == 2,
          "the miss after a first one on the vast device reads a run of 32 blocks in one request");
    bcache_release(b);
    {
        int own = 0;

        for (i = 3; i < 290; i++) {
            b = bcache_get(&vast, (uint64_t)i);
            own += b->data[0] == (unsigned char)i && b->data[4095] == (unsigned char)i;
            bcache_release(b);
        }
        check(reads_made == 10 && own == 287,
              "and a reader in order goes on in runs of 32, each block with its own bytes");
    }
    b = bcache_get(&vast, 290);
    check(reads_made == 11 && last_count == (WIDE_BLOCKS - 290) * 8,
          "the block past the last run starts the next run, the device's remainder");
    bcache_release(b);
    bcache_invalidate(&vast);

    writes_made = 0;
    for (i = 1; i <= 300; i++) {
        b = bcache_get(&vast, (uint64_t)i);
        b->data[0] = (unsigned char)(i + 1);
        bcache_dirty(b);
        bcache_release(b);
    }
    check(bcache_sync(&vast) == 0 && writes_made == 2 && writes_lba[0] == 8 &&
              writes_lba[1] == 8 + 256 * 8 && last_write_count == 44 * 8 &&
              image_vast[300 * 4096] == (unsigned char)(301) && image_vast[256 * 4096] == 1,
          "300 dirty blocks are written in a run of 256 and a run of 44");
    bcache_invalidate(&vast);
    for (i = 0; i < (int)sizeof(image_vast); i++) {
        image_vast[i] = (unsigned char)(i / 4096u);
    }

    {
        uint32_t written = 0;
        static process_t holders[6];
        struct bbuf *held[21];

        check(bcache_shrink(100000, 1, &written) > 0, "the buffers of the rows above are freed");
        bcache_set_limit(40);
        for (i = 0; i < 21; i++) {
            holders[i / 4].pid = 50 + i / 4;
            current_proc = &holders[i / 4];
            held[i] = bcache_get(&vast, (uint64_t)(100 + i));
        }
        current_proc = &proc_a;
        reads_made = 0;
        b = bcache_get(&vast, 1);
        bcache_release(b);
        b = bcache_get(&vast, 2);
        check(reads_made == 2 && last_count == 20 * 8 && b->data[0] == 2,
              "a small cache bounds the run to half its buffers and keeps the caller's block");
        bcache_release(b);
        b = bcache_get(&vast, 21);
        bcache_release(b);
        check(reads_made == 2,
              "and the run's last block is in, keyed on the run's own first blocks");
        for (i = 0; i < 21; i++) {
            current_proc = &holders[i / 4];
            bcache_release(held[i]);
        }
        current_proc = &proc_a;
    }

    bcache_invalidate(&vast);
    writes_made = 0;
    for (i = 1; i <= 40; i++) {
        b = bcache_get_blank(&vast, (uint64_t)i);
        bcache_dirty(b);
        bcache_release(b);
    }
    check(writes_made == 0, "forty blank blocks fit the cache of forty without a write");
    check(bcache_sync(&vast) == 0 && writes_made == 2 && writes_lba[0] == 8 &&
              writes_lba[1] == 8 + 20 * 8 && last_write_count == 20 * 8,
          "and are written in two runs of twenty");
    writeback_rows();
    reader_rows();
    release_wake_rows();
    pause_rows();
    drop_rows();
    check(wb_depth == 0, "every call gives the write lock back");

    if (fails == 0) {
        printf("bcache_hosttest: OK\n");
    }
    return fails > 255 ? 255 : fails;
}
