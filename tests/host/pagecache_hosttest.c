/**
 * pagecache_hosttest.c: the bookkeeping of the page cache,
 * rach2/sys/mm/pagecache.c, over a model disk of 64 pages.
 *
 * host-deps:
 *
 * Built with PAGECACHE_HOSTTEST and PC_SLOTS=4, it includes pagecache.c
 * whole, so the static hash, clock and eviction code are reachable, and the
 * frame and mapping layer is replaced by static pages. An array stands in
 * for the disk: a fault must fill the right page once, a second touch while
 * the page is resident must hit without a read, an eviction must write back
 * only a dirty page, and a dirty page evicted and read again must come back
 * with its bytes. The writer can sleep while another cache operation runs.
 * A failed write before the failure hook is installed is handed to the hook
 * once at its installation, and dropped by a new backing.
 *
 * Compiled with PAGECACHE_SYNC_HOSTTEST as well, as mk/host-fixtures.mk
 * compiles it, the test also runs `pc_sync_wait` and `sys_sync`, which that
 * file's rule extracts from rach2/sys/syscall_fs.c into syscall_sync.inc,
 * with the sleep on a pin's release replaced, for the rows of the wait and
 * its failures.
 */
#define PAGECACHE_HOSTTEST
#define PC_SLOTS 4u

/* The hash rebuild panics when its table has no empty entry, which the
   sizes make impossible; the stub below traps, which fails the test. */
void k_panic(const char *s);

#include "mm/pagecache.c"

#ifdef _WIN32
extern int printf(const char *format, ...);
extern int _write(int fd, const void *bytes, unsigned size);
#else
extern int dprintf(int fd, const char *format, ...);
#endif

/** The kernel's panic: stop the test with a trap. */
void k_panic(const char *s)
{
    (void)s;
    __builtin_trap();
}

static int fails;

/** Write a failure without including host headers over the kernel types. */
static void failed_check(const char *condition)
{
#ifdef _WIN32
    static const char prefix[] = "pagecache-hosttest: failed: ";
    unsigned size = 0;
    while (condition[size])
        size++;
    _write(2, prefix, sizeof(prefix) - 1);
    _write(2, condition, size);
    _write(2, "\n", 1);
#else
    dprintf(2, "pagecache-hosttest: failed: %s\n", condition);
#endif
}

#define CHECK(cond)              \
    do {                         \
        if (!(cond)) {           \
            failed_check(#cond); \
            fails++;             \
        }                        \
    } while (0)

/* A model disk of 64 pages, with read and write counts for each page, so the
   rows check the pattern of access as well as the final bytes. */
#define FAKE_PAGES 64u
static uint8_t fake_disk[FAKE_PAGES][PC_PAGE_SIZE];
static uint32_t fake_reads[FAKE_PAGES];
static uint32_t fake_writes[FAKE_PAGES];   /* writes that stored their page */
static uint32_t fake_attempts[FAKE_PAGES]; /* writes, failed ones included */
static void (*fake_write_pause)(uint32_t disk_off, const void *page);

/* The faults of the writeback paths: fail one page's writes, or all of them,
   so the rows reach the code that keeps a page whose write failed. */
#define FAKE_NO_FAIL 0xFFFFFFFFu
static uint32_t fake_write_fail = FAKE_NO_FAIL;
static int fake_write_fail_all;

/** The backing's read: copy the page at `disk_off` into `page`, counting
 *  the read. Returns 0, or -1 past the disk. */
static int fake_read(uint32_t disk_off, void *page)
{
    uint32_t p = disk_off / PC_PAGE_SIZE;
    uint32_t i;
    if (p >= FAKE_PAGES) {
        return -1;
    }
    for (i = 0; i < PC_PAGE_SIZE; ++i) {
        ((uint8_t *)page)[i] = fake_disk[p][i];
    }
    ++fake_reads[p];
    return 0;
}

/**
 * The backing's write: store the `count` pages at `pages` from `disk_off`
 * unless a fault fails one of them, which fails the whole write and stores
 * none, then call `fake_write_pause` with the first page before returning
 * the status. Each page counts an attempt. Returns 0, or -1 for a failed
 * write and past the disk.
 *
 * The pause stands for another process running during a sleeping write. It
 * is cleared before it is called, so a writeback it starts does not pause
 * again and the calls cannot recurse without end.
 */
static int fake_write(uint32_t disk_off, const void *pages, uint32_t count)
{
    uint32_t first = disk_off / PC_PAGE_SIZE;
    uint32_t k, i;
    int failed = fake_write_fail_all;
    void (*pause)(uint32_t, const void *);

    if (count == 0 || first >= FAKE_PAGES || count > FAKE_PAGES - first) {
        return -1;
    }
    for (k = 0; k < count; ++k) {
        ++fake_attempts[first + k];
        if (first + k == fake_write_fail) {
            failed = 1;
        }
    }
    if (!failed) {
        for (k = 0; k < count; ++k) {
            for (i = 0; i < PC_PAGE_SIZE; ++i) {
                fake_disk[first + k][i] = ((const uint8_t *)pages)[k * PC_PAGE_SIZE + i];
            }
            ++fake_writes[first + k];
        }
    }
    pause = fake_write_pause;
    fake_write_pause = 0;
    if (pause) {
        pause(disk_off, pages);
    }
    return failed ? -1 : 0;
}

/** Stand in for the CPU touching window page `p`: fault it, for a read or
 *  a write, and on a write store `marker` in the cached page's first byte,
 *  as a store through the window would. */
static void touch(uint32_t p, int write, uint8_t marker)
{
    uint32_t va = PCWIN_BASE + p * PC_PAGE_SIZE;
    CHECK(pc_fault(va, write) == 1);
    if (write) {
        uint8_t *page = (uint8_t *)pc_test_page(p * PC_PAGE_SIZE);
        CHECK(page != 0);
        if (page) {
            page[0] = marker;
        }
    }
}

/* What the failure hook was told: its calls and the last answer. */
static int hook_calls, hook_rc;

/** The file system's failure hook: count the call. */
static void failure_hook(int rc)
{
    hook_calls++;
    hook_rc = rc;
}

/**
 * A failed write before a hook is installed is held and handed to the hook
 * once, at its installation, as the boot's write of the root before the
 * root is mounted is; with a hook, each failed write calls it, and a write
 * that succeeds does not; a new backing drops a held failure.
 */
static void test_failure_hook(void)
{
    pc_on_failure(0);
    init_pagecache();
    pc_set_backing(fake_read, fake_write, FAKE_PAGES * PC_PAGE_SIZE);
    hook_calls = 0;
    touch(25, 1, 0xC5u);
    fake_write_fail = 25;
    CHECK(pc_sync() == PC_SYNC_FAILED);
    CHECK(hook_calls == 0);
    pc_on_failure(failure_hook);
    CHECK(hook_calls == 1 && hook_rc == -1);
    pc_on_failure(failure_hook);
    CHECK(hook_calls == 1);
    CHECK(pc_sync() == PC_SYNC_FAILED);
    CHECK(hook_calls == 2 && hook_rc == -1);
    fake_write_fail = FAKE_NO_FAIL;
    CHECK(pc_sync() == 0);
    CHECK(hook_calls == 2 && fake_disk[25][0] == 0xC5u);

    pc_on_failure(0);
    touch(26, 1, 0xC6u);
    fake_write_fail = 26;
    CHECK(pc_sync() == PC_SYNC_FAILED);
    fake_write_fail = FAKE_NO_FAIL;
    pc_set_backing(fake_read, fake_write, FAKE_PAGES * PC_PAGE_SIZE);
    pc_on_failure(failure_hook);
    CHECK(hook_calls == 2);
    pc_on_failure(0);
}

/* What runs while a writeback of page 0 sleeps: a reclaim, a miss that
   could reuse the slot, a sync of the clean or the redirtied page, and an
   eviction of the redirtied page. */
enum write_race {
    RACE_RECLAIM,
    RACE_REUSE,
    RACE_SYNC_CLEAN,
    RACE_SYNC_DIRTY,
    RACE_EVICT_DIRTY,
    RACE_COUNT
};

static enum write_race race;
static uint32_t race_calls;

/**
 * Run a competing cache operation while the backing writer is suspended.
 *
 * The writer has sampled the old bytes but has not returned its status.
 * A store at this point must remain dirty even if that write succeeds.
 */
static void race_during_write(uint32_t disk_off, const void *page)
{
    uint32_t written = 0;

    ++race_calls;
    CHECK(disk_off == 0);
    CHECK(pc_test_page(0) == page);
    CHECK(!pc_slots[0].dirty);
    if (race == RACE_RECLAIM) {
        CHECK(pc_shrink(PC_SLOTS, 0, &written) == PC_SLOTS - 1u);
        CHECK(written == 0);
    } else if (race == RACE_REUSE) {
        CHECK(pc_fault(PCWIN_BASE + PC_PAGE_SIZE, 0) == 0);
        CHECK(pc_test_page(PC_PAGE_SIZE) == 0);
    } else {
        if (race == RACE_SYNC_DIRTY || race == RACE_EVICT_DIRTY) {
            touch(0, 1, 0xD2u);
        }
        if (race == RACE_EVICT_DIRTY) {
            CHECK(pc_evict(0, pc_new_pass()) == -1);
        } else {
            CHECK(pc_sync() == PC_SYNC_BUSY);
        }
    }
    CHECK(pc_test_page(0) == page);
    CHECK(pc_slots[0].page == page);
    CHECK(pc_slots[0].disk_off == 0);
    CHECK(fake_attempts[0] == 1);
}

/**
 * Check sync and eviction across successful and failed sleeping writes.
 *
 * Each case uses one live slot. Reclaim must leave it resident, a miss must
 * not reuse it, and another writeback must report it busy without writing.
 * Failed or redirtied pages must retain their bytes for a successful retry.
 */
static void test_sleeping_writeback(void)
{
    int evict, fail;

    for (evict = 0; evict < 2; ++evict) {
        for (fail = 0; fail < 2; ++fail) {
            for (race = RACE_RECLAIM; race < RACE_COUNT; ++race) {
                int dirty = race == RACE_SYNC_DIRTY || race == RACE_EVICT_DIRTY;
                int retained = !evict || fail || dirty;
                int rc;
                uint8_t *page;
                uint8_t marker = dirty ? 0xD2u : 0xD1u;

                init_pagecache();
                pc_set_backing(fake_read, fake_write, FAKE_PAGES * PC_PAGE_SIZE);
                CHECK(pc_set_capacity(1) == 0);
                fake_disk[0][0] = 0;
                fake_attempts[0] = 0;
                touch(0, 1, 0xD1u);
                fake_write_fail_all = fail;
                race_calls = 0;
                fake_write_pause = race_during_write;
                rc = evict ? pc_evict(0, pc_new_pass()) : pc_sync();
                CHECK(rc == (evict ? ((fail || dirty) ? -1 : 0) : (fail ? PC_SYNC_FAILED : 0)));
                CHECK(race_calls == 1);
                CHECK(!pc_slots[0].pinned);
                CHECK(fake_disk[0][0] == (fail ? 0 : 0xD1u));
                CHECK(pc_stat_writebacks() == (fail ? 0u : 1u));
                page = pc_test_page(0);
                CHECK((page != 0) == retained);
                if (page) {
                    CHECK(page[0] == marker);
                    CHECK(pc_slots[0].dirty == (fail || dirty));
                }
                fake_write_fail_all = 0;
                CHECK(pc_sync() == 0);
                CHECK(fake_disk[0][0] == marker);
                CHECK(pc_evict(0, pc_new_pass()) == 0);
                CHECK(pc_test_page(0) == 0);
                touch(0, 0, 0);
                page = pc_test_page(0);
                CHECK(page != 0 && page[0] == marker);
            }
        }
    }
}

#ifdef PAGECACHE_SYNC_HOSTTEST
#include "isr.h"

static uint32_t sync_mounts;
static uint32_t sync_blocks; /* waits that slept until the pin was let go */
static uint32_t sync_quick;  /* waits a release during the pass ended */
static int sync_heal;        /* a blocked wait also lets page 2's writes pass */

/** No file is mapped shared here, so the mapped pages have nothing to give
 *  the cache. */
static void filemap_sync_all(void)
{}

/** Count the sync of the mounts. */
static void vfs_sync_all(void)
{
    ++sync_mounts;
}

/**
 * The kernel's wait for the release of a pin a sync passed. When the count
 * has moved since `seen` it returns at once, as the kernel's condition is
 * then true before the sleep. Otherwise it stands for the sleep: only then
 * does the competing write of page 0 end, redirtying the page and letting
 * its pin go, as on one processor a flusher runs only once the caller
 * blocks. A release that the count does not see would leave the kernel's
 * caller asleep for ever, so it stops the test.
 */
void pc_wait_release(uint32_t seen)
{
    if (pc_releases() != seen) {
        ++sync_quick;
        return;
    }
    ++sync_blocks;
    CHECK(pc_slots[0].pinned && pc_slots[0].waited);
    touch(0, 1, 0xE2u);
    pc_unpin(&pc_slots[0]);
    CHECK(!pc_slots[0].waited);
    if (sync_heal) {
        fake_write_fail = FAKE_NO_FAIL;
    }
    if (pc_releases() == seen) {
        failed_check("pc_releases() != seen");
        __builtin_trap();
    }
}

/** End the competing write of page 0 while the sync's write of page 2
 *  sleeps, after the sync has passed page 0's pin. */
static void release_during_pass(uint32_t disk_off, const void *page)
{
    (void)page;
    CHECK(disk_off == 2u * PC_PAGE_SIZE);
    CHECK(pc_slots[0].pinned && pc_slots[0].waited);
    touch(0, 1, 0xE2u);
    pc_unpin(&pc_slots[0]);
}

#include "syscall_sync.inc"

/**
 * Run the kernel's `pc_sync_wait` and `sys_sync` with a page pinned by a
 * competing write, a failed write, or both.
 *
 * Row 0, a busy page alone: the wait sleeps once, and the sync then writes
 * the page the competing write redirtied. Row 1, a failure alone: no wait,
 * and the failure is answered. Row 2, a busy page beside a write that fails
 * in every pass: the failing write marks no pin of its own, so the wait
 * still sleeps once instead of passing again and again while the competing
 * write cannot run, and the failure is answered. Row 3, a write that fails
 * in the first pass and succeeds in the second: the failure is kept across
 * the passes. Row 4, a pin let go while the pass sleeps in a later write:
 * the wait ends at once, with no sleep, since the release came after the
 * count was read. `sys_sync` answers 0 whatever failed, and a later sync
 * writes a page whose writes failed.
 */
static void test_sync_wait(void)
{
    int row;

    for (row = 0; row < 5; ++row) {
        int busy = row != 1;
        int failed = row == 1 || row == 2 || row == 3;

        init_pagecache();
        pc_set_backing(fake_read, fake_write, FAKE_PAGES * PC_PAGE_SIZE);
        fake_disk[0][0] = 0;
        fake_disk[2][0] = 2;
        fake_attempts[0] = fake_attempts[2] = 0;
        fake_writes[0] = fake_writes[2] = 0;
        sync_blocks = sync_quick = sync_mounts = 0;
        sync_heal = row == 3;
        touch(0, 0, 0);
        pc_slots[0].pinned = (uint8_t)busy;
        if (failed || row == 4) {
            touch(2, 1, 0xE1u);
        }
        if (failed) {
            fake_write_fail = 2;
        }
        if (row == 4) {
            fake_write_pause = release_during_pass;
        }
        CHECK(pc_sync_wait() == (failed ? PC_SYNC_FAILED : 0));
        CHECK(!pc_slots[0].pinned && !pc_slots[0].waited);
        CHECK(sync_blocks == (row == 0 || row == 2 || row == 3 ? 1u : 0u));
        CHECK(sync_quick == (row == 4 ? 1u : 0u));
        if (busy) {
            CHECK(fake_disk[0][0] == 0xE2u);
            CHECK(fake_writes[0] == 1);
            CHECK(!pc_slots[0].dirty);
        } else {
            CHECK(fake_attempts[0] == 0);
        }
        CHECK(fake_attempts[2] == (row == 1                 ? 1u
                                   : (row == 2 || row == 3) ? 2u
                                   : row == 4               ? 1u
                                                            : 0u));
        CHECK(pc_slots[1].dirty == (row == 1 || row == 2)); /* page 2's slot */
        CHECK(sys_sync(0) == 0);
        CHECK(sync_mounts == 1);
        if (failed) {
            fake_write_fail = FAKE_NO_FAIL;
            CHECK(sys_sync(0) == 0);
            CHECK(!pc_slots[1].dirty);
            CHECK(fake_disk[2][0] == 0xE1u);
        }
    }
}
#endif

/**
 * Run the rows, on a cache of four slots over the model disk, each page of
 * which has its number in its first byte. Returns 0 when every check passed
 * and 1 otherwise.
 *
 * - A read fault fills its page once and a second touch is a hit; the
 *   cached bytes are the disk's; a fault past the file system's size is
 *   refused, as for a wild pointer.
 * - Four pages fit the cache, and a fifth evicts a clean page with no
 *   writeback.
 * - Pages 0 to 7 are written, pages 8 to 31 read, far past the cache's four
 *   slots, and pages 0 to 7 read again: each written page was written back
 *   once and reads back its marker, and no page only read is written.
 * - `pc_sync` writes back a dirty page and protects it again, so a second
 *   sync with no store between writes nothing.
 * - A failed writeback never drops a dirty page: a sync reports it and the
 *   page stays dirty, and a later sync writes its bytes.
 * - A shrink whose writebacks all fail is reported by `pc_set_capacity`,
 *   which keeps the capacity over the four dirty slots, so a reclaim, and
 *   not only a sync, which scans every slot, can still reach them. Reads far
 *   past one slot then write each of the four through the reclaim alone; had
 *   the capacity fallen to one, slots 1 to 3 would be out of its reach and
 *   never written.
 * - `test_sleeping_writeback`, `test_failure_hook`, and with
 *   PAGECACHE_SYNC_HOSTTEST `test_sync_wait`.
 */
int main(void)
{
    uint32_t p;
    uint32_t i;

    for (p = 0; p < FAKE_PAGES; ++p) {
        for (i = 0; i < PC_PAGE_SIZE; ++i) {
            fake_disk[p][i] = 0;
        }
        fake_disk[p][0] = (uint8_t)p;
    }

    init_pagecache();
    pc_set_backing(fake_read, fake_write, FAKE_PAGES * PC_PAGE_SIZE);

    touch(0, 0, 0);
    CHECK(fake_reads[0] == 1);
    CHECK(pc_stat_reads() == 1);
    touch(0, 0, 0);
    CHECK(fake_reads[0] == 1);
    CHECK(pc_stat_reads() == 1);

    {
        uint8_t *pg = (uint8_t *)pc_test_page(0);
        CHECK(pg != 0 && pg[0] == 0);
    }

    CHECK(pc_fault(PCWIN_BASE + FAKE_PAGES * PC_PAGE_SIZE, 0) == 0);

    touch(1, 0, 0);
    touch(2, 0, 0);
    touch(3, 0, 0);
    CHECK(pc_stat_evictions() == 0);
    touch(4, 0, 0);
    CHECK(pc_stat_evictions() == 1);
    CHECK(pc_stat_writebacks() == 0);

    for (p = 0; p < 8; ++p) {
        touch(p, 1, (uint8_t)(0x80u + p));
    }
    for (p = 8; p < 32; ++p) {
        touch(p, 0, 0);
    }
    for (p = 0; p < 8; ++p) {
        uint8_t *pg;
        touch(p, 0, 0);
        pg = (uint8_t *)pc_test_page(p * PC_PAGE_SIZE);
        CHECK(pg != 0 && pg[0] == (uint8_t)(0x80u + p));
        CHECK(fake_disk[p][0] == (uint8_t)(0x80u + p));
        CHECK(fake_writes[p] == 1);
    }
    for (p = 8; p < 32; ++p) {
        CHECK(fake_writes[p] == 0);
    }

    touch(40, 1, 0xBBu);
    pc_sync();
    CHECK(fake_writes[40] == 1);
    CHECK(fake_disk[40][0] == 0xBBu);
    {
        uint32_t wb = pc_stat_writebacks();
        pc_sync();
        CHECK(pc_stat_writebacks() == wb);
    }

    init_pagecache();
    pc_set_backing(fake_read, fake_write, FAKE_PAGES * PC_PAGE_SIZE);
    touch(20, 1, 0xC0u);
    fake_write_fail = 20;
    CHECK(pc_sync() == PC_SYNC_FAILED);
    CHECK(fake_disk[20][0] != 0xC0u);
    fake_write_fail = FAKE_NO_FAIL;
    CHECK(pc_sync() == 0);
    CHECK(fake_disk[20][0] == 0xC0u);

    init_pagecache();
    pc_set_backing(fake_read, fake_write, FAKE_PAGES * PC_PAGE_SIZE);
    touch(21, 1, 0xC1u);
    touch(22, 1, 0xC2u);
    touch(23, 1, 0xC3u);
    touch(24, 1, 0xC4u);
    fake_write_fail_all = 1;
    CHECK(pc_set_capacity(1) == -1);
    CHECK(fake_writes[21] == 0 && fake_writes[24] == 0);
    fake_write_fail_all = 0;
    for (p = 30; p < 40; ++p) {
        touch(p, 0, 0);
    }
    for (p = 21; p <= 24; ++p) {
        CHECK(fake_writes[p] == 1);
        CHECK(fake_disk[p][0] == (uint8_t)(0xC0u + (p - 20u)));
    }

    test_sleeping_writeback();
    test_failure_hook();
#ifdef PAGECACHE_SYNC_HOSTTEST
    test_sync_wait();
#endif
#ifdef _WIN32
    printf("pagecache-hosttest: result: %s: %d failures\n", fails ? "fail" : "pass", fails);
#else
    dprintf(1, "pagecache-hosttest: result: %s: %d failures\n", fails ? "fail" : "pass", fails);
#endif
    return fails ? 1 : 0;
}
