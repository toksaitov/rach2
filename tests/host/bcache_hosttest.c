/*
 * bcache_hosttest.c: the buffer cache against a fake block device.
 *
 * host-deps: rach2/sys/fs/bcache.c rach2/sys/lib/kstring.c
 * host-deps: rach2/sys/lib/kctypes.c
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

#define FAKE_SECTORS 100u /* not a multiple of 8: block 12 is a 4-sector tail */
#define BIG_SECTORS  (BCACHE_BUFFERS * 8u + 8u) /* enough blocks to fill it */

static unsigned char image_small[FAKE_SECTORS * 512];
static unsigned char image_big[BIG_SECTORS * 512];
static int reads_made, fail_reads, fail_writes;
/* The device is pulled in the middle of this read: the cache is told it has
   gone while the buffer being filled is held, and the read then fails. */
static int gone_during_read;
/* And in the middle of this write of a dirty victim, which is held for it. */
static int gone_during_write;
static unsigned last_count;
static uint64_t writes_lba[8];
static int writes_made;
/* The fault injector: report success and write nothing at write number
   `drop_at`; write only `tear_bytes` at write number `tear_at`. */
static int drop_at = -1, tear_at = -1, tear_bytes;
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
    if (writes_made < 8) {
        writes_lba[writes_made] = lba;
    }
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

/* The members in order: the sector size, the count, the flags, the deadline,
   the two polled calls, the two a sleeping device has, and the driver's own
   pointer. Written out rather than left to position, so that a member added
   to the structure is a compile error here and not a silent shift. */
static block_device_t fake = {512, FAKE_SECTORS, BLOCK_FAULT_SAFE, 0, fake_read, fake_write, 0,
                              0,   image_small};
static block_device_t other = {512, BIG_SECTORS, BLOCK_FAULT_SAFE, 0, fake_read, fake_write,
                               0,   0,           image_big};

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
static void get_other(void *arg)
{
    bcache_get(&other, *(uint64_t *)arg);
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
        panic_row(get_other, &two, "second device",
                  "a block on another device while holding one panics");
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

    check(waited_on_host == 0, "no row reached the sleeping wait");

    if (fails == 0) {
        printf("bcache_hosttest: OK\n");
    }
    return fails;
}
