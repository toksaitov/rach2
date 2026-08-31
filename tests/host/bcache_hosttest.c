/*
 * bcache_hosttest.c: the buffer cache against a fake block device.
 *
 * host-deps: rach2/sys/fs/bcache.c
 *
 * The fake device is polled and fault-safe, backed by a memory image whose
 * every sector holds its own number. `block_request` is stubbed to call the
 * device directly: the request pool is i386 code and the guest's row. The
 * rows are the read half's contract: hits and misses, the least-recently-
 * released victim named exactly, a failed read answered with no buffer,
 * invalidation refused while held, the two hold rules refused by panic, a
 * short last block, and a block past the device refused. The busy-buffer
 * wait needs a scheduler and is the guest's row, not this one's.
 *
 * No hosted headers: the kernel headers define the fixed-width types, and
 * the libc calls used here are declared by hand the way the other host
 * tests declare them. Each panic row runs the violating call in a forked
 * child; the child's exit status says whether the panic came and whether
 * its text named the rule.
 */

#include "../../rach2/sys/fs/bcache.h"
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
    waited_on_host = 1;
    return -1;
}

/* ---- the fake device ---- */

#define FAKE_SECTORS 100u /* not a multiple of 8: block 12 is a 4-sector tail */
#define BIG_SECTORS  (BCACHE_BUFFERS * 8u + 8u) /* enough blocks to fill it */

static unsigned char image_small[FAKE_SECTORS * 512];
static unsigned char image_big[BIG_SECTORS * 512];
static int reads_made, fail_reads;
static unsigned last_count;

static int fake_read(block_device_t *d, uint64_t lba, uint32_t count, void *buf)
{
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
    __builtin_memcpy((unsigned char *)d->drvdata + lba * 512, buf, count * 512);
    return 0;
}

static block_device_t fake = {512, FAKE_SECTORS, BLOCK_FAULT_SAFE, 0, fake_read, fake_write,
                              0,   image_small};
static block_device_t other = {512,        BIG_SECTORS, BLOCK_FAULT_SAFE, 0, fake_read,
                               fake_write, 0,           image_big};

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
        printf("FAIL: %s\n", what);
    }
}

/* Run `blk` through bcache_get in a child that expects a panic whose text
 * contains `text`; the parent turns the exit status into a row. */
static void panic_row(uint64_t blk, const char *text, const char *what)
{
    int pid = fork();
    int status = 0;

    if (pid == 0) {
        expect_panic = text;
        bcache_get(&fake, blk);
        _exit(44); /* no panic came */
    }
    check(pid > 0 && waitpid(pid, &status, 0) == pid, "the panic child ran");
    check((status & 0x7f) == 0 && ((status >> 8) & 0xff) == 42, what);
}

int main(void)
{
    struct bbuf *b, *c;
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

    b = bcache_get(&fake, 12);
    check(b != 0 && b->bytes == 4 * 512, "the short last block is its tail");
    check(last_count == 4, "read as the sectors that exist");
    bcache_release(b);

    check(bcache_get(&fake, 13) == 0, "a block past the device is refused");

    fail_reads = 1;
    check(bcache_get(&other, 3) == 0, "a failed read returns no buffer");
    fail_reads = 0;

    b = bcache_get(&fake, 5);
    check(bcache_invalidate(&fake) == -16, "invalidation refuses while held");
    bcache_release(b);
    check(bcache_invalidate(&fake) == 0, "and drops the device's buffers after");
    reads_made = 0;
    b = bcache_get(&fake, 5);
    check(reads_made == 1, "so the next get reads the device again");
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
    check(b != 0 && reads_made == 1, "a full cache still answers a miss");
    bcache_release(b);
    b = bcache_get(&other, 0);
    check(b != 0 && reads_made == 1, "the refreshed block survived");
    bcache_release(b);
    b = bcache_get(&other, 1);
    check(b != 0 && reads_made == 2, "and the oldest release was the victim");
    bcache_release(b);

    /* The hold rules end in a panic, observed from a forked child. */
    b = bcache_get(&fake, 3);
    panic_row(2, "lower or equal", "a lower block while holding a higher one panics");
    bcache_release(b);

    {
        struct bbuf *held[BCACHE_HOLD_MAX];

        for (i = 0; i < BCACHE_HOLD_MAX; i++) {
            held[i] = bcache_get(&fake, i);
            check(held[i] != 0, "a buffer within the hold limit");
        }
        panic_row(9, "limit", "a fifth held buffer panics");
        for (i = 0; i < BCACHE_HOLD_MAX; i++) {
            bcache_release(held[i]);
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

    check(waited_on_host == 0, "no row reached the sleeping wait");

    if (fails == 0) {
        printf("bcache_hosttest: OK\n");
    }
    return fails;
}
