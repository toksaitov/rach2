/*
 * bcache_hosttest.c: the buffer cache against a fake block device.
 *
 * host-deps: rach2/sys/fs/bcache.c rach2/sys/block/chain.c
 * host-deps: rach2/sys/lib/kstring.c
 * host-deps: rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/kheap_host.c tests/host/compat/blockmax_host.c
 * host-deps: tests/host/compat/pressure_host.c
 *
 * The fake device is polled and fault-safe, backed by a memory image whose
 * every sector holds its own number. `block_request` is stubbed to call the
 * device directly: the request pool is i386 code and the guest's row. The
 * rows are the cache's contract: hits and misses, the least-recently-
 * released victim named exactly, a failed read returning no buffer,
 * invalidation returning EBUSY while a buffer is held or dirty and dropping
 * every buffer of the device after, the hold rules and the ownership of a
 * release ending in a panic, a short last block, a block past the device and
 * a block number that would wrap the sector arithmetic both returning
 * nothing, sync writing dirty buffers in ascending order and keeping a
 * buffer dirty across a failed write until a later call writes it, a
 * write-through reaching the device before it returns and in call order, a
 * dirty victim written before its buffer is reused, a failed write recorded
 * against the device and its sector with the buffer kept dirty and not
 * reused, and a torn or dropped write the device reports as success, which
 * the cache cannot see and the volume walk must judge. The busy-buffer wait
 * needs a scheduler and is the guest's row, not this one's.
 *
 * No hosted headers: the kernel headers define the fixed-width types, and
 * the libc calls used here are declared by hand the way the other host
 * tests declare them. Each panic row runs the violating call in a forked
 * child; the child's exit status says whether the panic came and whether
 * its text named the rule.
 */

#include "../../rach2/sys/fs/bcache.h"
#include "../../rach2/sys/lib/kerrno.h"
#include "../../rach2/sys/proc/process.h"

extern int printf(const char *fmt, ...);
extern int fork(void);
extern int waitpid(int pid, int *status, int options);
extern void _exit(int status);
extern void exit(int status);

/* ---- stubs for what the cache reaches outside itself ---- */

process_t proc_a, proc_b;
process_t *current_proc = &proc_a;

/* Set before a violating call in a forked child; k_panic exits 42 when its
 * text contains this, 43 when it does not. */
static const char *expect_panic;

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

void k_panic(const char *s)
{
    if (expect_panic) {
        _exit(contains(s, expect_panic) ? 42 : 43);
    }
    printf("bcache_hosttest: unexpected panic: %s\n", s);
    _exit(44);
}

/* The cache asks the mount table whether this caller holds a lock before it
   writes back a buffer of a device that stands on a file; a row sets this. */
static int ns_held;

int vfs_ns_held_by_caller(void)
{
    return ns_held;
}

void klog(int level, const char *sys, const char *fmt, ...)
{
    (void)level;
    (void)sys;
    (void)fmt;
}

static int waited_on_host;

void wait_queue_init(wait_queue_t *wq)
{
    __builtin_memset(wq, 0, sizeof(*wq));
}
void wq_wake_all(wait_queue_t *wq)
{
    (void)wq;
}
int wq_wait_event(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    (void)cond;
    (void)arg;
    /* No row may sleep: one process runs here, so a wait would never end. */
    waited_on_host = 1;
    printf("bcache_hosttest: FAIL: a row reached the sleeping wait\n");
    exit(1);
}
int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    return wq_wait_event(wq, cond, arg);
}

/* ---- the fake device ---- */

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
/* The device is pulled in the middle of this read: the cache is told it has
   gone while the buffer being filled is held, and the read then fails. */
static int gone_during_read;
/* And in the middle of this write of a dirty victim, which is held for it. */
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
/* The two interleavings. `write_during_fill` is a block written through
   the cache, synced and dropped from it while a run read that covers it is
   inside the device, as another process's write lands during the fill's
   sleep: the read answers the old bytes, and the fill must not put them
   over the newer ones. `first_during_sync` is a block another process
   dirties as a `first` while a sync's write of an `after` is inside the
   device: the sync must write it before its next `after`. */
static uint64_t write_during_fill;
static uint64_t first_during_sync;
static struct process *hook_proc;
/* What the cache recorded against the device. */
static block_device_t *failed_dev;
static uint64_t failed_lba;
static int failed_rc;

void vfs_device_failed(block_device_t *dev, uint64_t lba, int rc)
{
    failed_dev = dev;
    failed_lba = lba;
    failed_rc = rc;
}

static int fake_read(block_device_t *d, uint64_t lba, uint32_t count, void *buf)
{
    if (gone_during_read) {
        gone_during_read = 0;
        bcache_device_gone(d);
        return -5;
    }
    reads_made++;
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

static int fake_write(block_device_t *d, uint64_t lba, uint32_t count, const void *buf)
{
    if (fail_writes) {
        return -5;
    }
    if (gone_during_write) {
        gone_during_write = 0;
        bcache_device_gone(d);
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
    if (writes_made < 8) {
        writes_lba[writes_made] = lba;
    }
    last_write_count = count;
    writes_made++;
    if (writes_made == drop_at) {
        return 0; /* the device says yes and keeps nothing */
    }
    if (writes_made == tear_at) {
        __builtin_memcpy((unsigned char *)d->drvdata + lba * 512, buf, (size_t)tear_bytes);
        return 0;
    }
    __builtin_memcpy((unsigned char *)d->drvdata + lba * 512, buf, count * 512);
    return 0;
}

/* The members in order: the sector size, the count, the flags, the deadline's
   period and its periods, the most sectors a request moves, the two polled
   calls, the two a sleeping device has, the device this one's bytes live on,
   and the driver's own pointer. Written out rather than left to position, so
   that a member added to the structure is a compile error here and not a
   silent shift. */
static block_device_t fake = {
    512, FAKE_SECTORS, BLOCK_FAULT_SAFE, 0, 0, 0, fake_read, fake_write, 0, 0, 0, image_small};
static block_device_t other = {
    512, BIG_SECTORS, BLOCK_FAULT_SAFE, 0, 0, 0, fake_read, fake_write, 0, 0, 0, image_big};
/* A device whose bytes live on `fake`: what a loop device is to the volume
   holding its file. Its own reads and writes go through `fake`, so taking one
   of its blocks and then one of `fake`'s is the order the rules must allow,
   and the other way round is the one they must refuse. */
static block_device_t nested = {
    512, FAKE_SECTORS, BLOCK_FAULT_SAFE, 0, 0, 0, fake_read, fake_write, 0, 0, 0, image_small};
/* A device that takes 64 sectors a request, as the ATA disk does: what
   the run fill and the runs of a sync are proved on. */
static block_device_t wide = {
    512, WIDE_SECTORS, BLOCK_FAULT_SAFE, 0, 0, 64, fake_read, fake_write, 0, 0, 0, image_wide};
/* A device that takes 2048 sectors a request, as the AHCI disk does: a
   fill's run is the cache's most, 256 blocks. */
static unsigned char image_vast[WIDE_SECTORS * 512];
static block_device_t vast = {
    512, WIDE_SECTORS, BLOCK_FAULT_SAFE, 0, 0, 2048, fake_read, fake_write, 0, 0, 0, image_vast};

int block_request(block_device_t *device, int write, uint64_t lba, uint32_t count, void *data)
{
    if (write) {
        return device->write(device, lba, count, data);
    }
    return device->read(device, lba, count, data);
}

/* ---- the rows ---- */

static int fails;

static void check(int ok, const char *what)
{
    if (!ok) {
        fails++;
        printf("bcache_hosttest: FAIL: %s\n", what);
    }
}

/* Run `fn(arg)` in a child that expects a panic whose text contains `text`;
 * the parent turns the exit status into a row. */
static void panic_row(void (*fn)(void *), void *arg, const char *text, const char *what)
{
    int pid = fork();
    int status = 0;

    if (pid == 0) {
        expect_panic = text;
        fn(arg);
        _exit(44); /* no panic came */
    }
    check(pid > 0 && waitpid(pid, &status, 0) == pid, "the panic child ran");
    check((status & 0x7f) == 0 && ((status >> 8) & 0xff) == 42, what);
}

static void get_fake(void *arg)
{
    bcache_get(&fake, *(uint64_t *)arg);
}
static void write_it(void *arg)
{
    bcache_write(arg);
}
static void get_nested(void *arg)
{
    bcache_get(&nested, *(uint64_t *)arg);
}

static void get_other(void *arg)
{
    bcache_get(&other, *(uint64_t *)arg);
}
static void sync_it(void *arg)
{
    bcache_sync((block_device_t *)arg);
}
static void release_it(void *arg)
{
    bcache_release(arg);
}
static void dirty_it(void *arg)
{
    bcache_dirty(arg);
}

int main(void)
{
    struct bbuf *b, *c, *d;
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

    b = bcache_get(&fake, 0);
    check(b != 0, "a block is read and held");
    check(b && b->data[0] == 0 && b->data[513] == 1, "with the device's bytes");
    check(reads_made == 1, "by one device read");
    bcache_release(b);
    b = bcache_get(&fake, 0);
    check(b != 0 && reads_made == 1, "a second get is a hit, not a read");
    check(bcache_stat_hits() == 1, "and the hit is counted");
    bcache_release(b);

    /* The blank get: a miss costs no read and reads as zeros; a cached block
       is returned as it is, bytes and all. */
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

    /* Writeback: a dirty buffer is written at sync in ascending block order,
       is kept dirty across a failed write, and blocks invalidation until it
       is written. */
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

    /* Write-through: bcache_write reaches the device before it returns,
       in call order, and leaves the buffer clean. */
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

    /* A dirty victim is written before its buffer is reused: one dirty
       buffer, then every buffer taken by the big device. */
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

    /* The order a file system asks for: a `first` buffer reaches the device
       before an `after` one of the same device, at an eviction, at a sync,
       and not while a first is held by another process. The after is
       released before the first, so it is the older and the eviction's
       choice. */
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
    /* Twice the cache's worth of misses, so every unheld buffer the rule
       does not protect is replaced at least once on the way. */
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

    /* A failed write is recorded against the device: the mounts on it go
       read-only through vfs_device_failed, and the buffer stays dirty until
       a later sync writes it. */
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

    /* The edge of the guarantee: a torn write and a dropped write are
       reported as success by the device, so the cache cannot know; what
       the disk holds afterwards is for the walk to judge. */
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

    /* Replacement is least recently released: fill every buffer from `other`,
       refresh block 0, and the next miss must evict other:1 exactly. */
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

    /* The hold rules end in a panic, observed from a forked child. */
    {
        uint64_t two = 2, three = 3, nine = 9;

        b = bcache_get(&fake, 3);
        panic_row(get_fake, &two, "not above", "a lower block while holding a higher one panics");
        panic_row(get_fake, &three, "not above", "the held block taken again panics");
        panic_row(get_other, &two, "not under",
                  "a block on another device while holding one panics");
        /* A chain of devices: down is allowed, up is not. The buffer held
           above is let go first, since these rows take blocks of their own. */
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
        /* A dirty buffer of a device that stands on another is not written
           back on the way to an unrelated device while this caller holds a
           mount's lock: that write takes the lock of the mount holding its
           file, and two callers could then wait for each other. */
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
        /* A writeback sleeps, whatever kind of device it goes to, and
           another caller can key the very block this call is about to key.
           The device's write keys it here, with a byte the cache would not
           have put there, and the call must come back with that buffer
           rather than a second one of its own. */
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
                    bcache_dirty(b2); /* the oldest buffer, and dirty */
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
        /* A sync waits for a dirty buffer somebody else holds. A caller
           holding one of the device it syncs would be waiting for itself,
           and one holding a block of another device would write through a
           file system it is already inside. Both are named, not waited on. */
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

    /* Another process's holds do not count against this one's rules. */
    b = bcache_get(&fake, 7);
    current_proc = &proc_b;
    c = bcache_get(&fake, 2);
    check(c != 0, "a second process takes a lower block freely");
    bcache_release(c);
    current_proc = &proc_a;
    bcache_release(b);

    /* A device that has gone. A clean buffer nobody holds is dropped at
       once; a dirty one records the device failed and is dropped, since
       nothing will ever write it; one a process holds is kept, marked, and
       dropped at its release, because a buffer cannot be reused under its
       holder. */
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

    /* A device that goes in the middle of a fill: the buffer being filled is
       held, so it is marked to be dropped; the fill then fails and drops it
       without a release. The mark must go with it, or the next user of that
       buffer, on a device that never went anywhere, has its dirty block
       thrown away at its release. */
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

    /* The other way a held buffer changes hands without a release: a dirty
       victim is written back inside a fill for another device, held for the
       write, and the device goes during it. The victim is then given to the
       other device with the mark still on it, unless the reuse clears it;
       every dirty block of the other device must reach it at a sync. */
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

    /* The runs, on the device that takes 64 sectors: a miss at the block
       after the last fill reads eight blocks in one request, and the seven
       after it are hits; a sync writes adjacent dirty blocks in requests of
       eight; and a sync of more dirty blocks than one batch gathers leaves
       nothing dirty, in ascending order. */
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

    /* A write that lands while a run read is inside the device: the read
       answered the block's old bytes, another process then wrote the block
       through the cache, synced it and let the buffer go, and the fill's
       staged copy is older than the device. The fill must not put it in
       the cache; the block read afterwards is the device's newer one. */
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

    /* A `first` dirtied by another process while a sync's write of an
       `after` is inside the device: the sync writes it before its next
       `after`, whatever their block numbers. */
    for (i = 0; i < 3; i++) {
        b = bcache_get_blank(&wide, (uint64_t)(210 + 10 * i));
        b->data[0] = 0xA0;
        bcache_dirty_ordered(b, BCACHE_ORDER_AFTER);
        bcache_release(b);
    }
    hook_proc = &proc_b;
    first_during_sync = 250;
    writes_made = 0;
    check(bcache_sync(&wide) == 0 && first_during_sync == 0 && writes_made == 4,
          "a sync during which a first was dirtied writes all four blocks");
    check(writes_lba[0] == 1680 && writes_lba[1] == 2000 && writes_lba[2] == 1760 &&
              writes_lba[3] == 1840,
          "and the late first goes to the device before the next after");
    bcache_invalidate(&wide);
    bcache_set_limit(BCACHE_BUFFERS);

    check(waited_on_host == 0, "no row reached the sleeping wait");

    if (fails == 0) {
        printf("bcache_hosttest: OK\n");
    }

    /* The device that takes 2048 sectors: the run after a first miss is
       the largest run, 256 blocks in one request, and the blocks of it are
       hits after, each with its own bytes. */
    for (i = 0; i < (int)sizeof(image_vast); i++) {
        image_vast[i] = (unsigned char)(i / 4096u);
    }
    bcache_set_limit(600); /* a run is at most half the limit: room for 256 */
    reads_made = 0;
    b = bcache_get(&vast, 1);
    bcache_release(b);
    b = bcache_get(&vast, 2);
    check(reads_made == 2 && last_count == 2048 && b->data[0] == 2,
          "the miss after a first one on the vast device reads a run of 256 blocks in one request");
    bcache_release(b);
    {
        int own = 0;

        for (i = 3; i < 258; i++) {
            b = bcache_get(&vast, (uint64_t)i);
            own += b->data[0] == (unsigned char)i && b->data[4095] == (unsigned char)i;
            bcache_release(b);
        }
        check(reads_made == 2 && own == 255,
              "and the 255 blocks after it are hits, each with its own bytes");
    }
    b = bcache_get(&vast, 258);
    check(reads_made == 3 && last_count == (WIDE_BLOCKS - 258) * 8,
          "the block past the run starts the next run, the device's remainder");
    bcache_release(b);
    bcache_invalidate(&vast);

    /* Written back the same way: 300 dirty blocks in a row go to the device
       in a run of 256 blocks, 2048 sectors, and a run of the 44 left. */
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

    /* A cache of forty buffers, made so by freeing every buffer the rows
       above made and lowering the limit, with twenty-one of them held by
       six other processes, four each within the hold rule: the run is
       bounded to half the cache, and when its last blocks find no free
       buffer the block the caller asked for is not the victim, though it
       is the least recently released: the run's own earlier blocks are. */
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

    /* A write run is bounded the same way: forty dirty blocks, keyed blank
       so that no read run takes buffers, fill the cache of forty and go to
       the device in two runs of twenty, not one of forty that would hold
       every buffer while the request sleeps. */
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
    bcache_invalidate(&vast);

    return fails > 255 ? 255 : fails;
}
