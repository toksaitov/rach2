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
 *
 * Compiled with PAGECACHE_SYNC_HOSTTEST as well, as pagecache_mutate.sh
 * compiles it, the test also runs `sys_sync`, which the script extracts
 * from rach2/sys/syscall_fs.c into syscall_sync.inc, with the clock tick
 * and the scheduler replaced, for the rows of its bounded wait and its
 * failures.
 */
#define PAGECACHE_HOSTTEST
#define PC_SLOTS 4u

/* The hash rebuild panics when its table has no empty entry, which the
   sizes make impossible; the stub below traps, which fails the test. */
void k_panic(const char *s);

#include "mm/pagecache.c"

extern int dprintf(int fd, const char *format, ...);

/** The kernel's panic: stop the test with a trap. */
void k_panic(const char *s)
{
    (void)s;
    __builtin_trap();
}

static int fails;

#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            dprintf(2, "pagecache-hosttest: failed: %s\n", #cond); \
            fails++;                                               \
        }                                                          \
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
 * The backing's write: store `page` at `disk_off` unless a fault fails it,
 * then call `fake_write_pause` before returning the status. Returns 0, or -1
 * for a failed write and past the disk.
 *
 * The pause stands for another process running during a sleeping write. It
 * is cleared before it is called, so a writeback it starts does not pause
 * again and the calls cannot recurse without end.
 */
static int fake_write(uint32_t disk_off, const void *page)
{
    uint32_t p = disk_off / PC_PAGE_SIZE;
    uint32_t i;
    int failed;
    void (*pause)(uint32_t, const void *);

    if (p >= FAKE_PAGES) {
        return -1;
    }
    ++fake_attempts[p];
    failed = fake_write_fail_all || p == fake_write_fail;
    if (!failed) {
        for (i = 0; i < PC_PAGE_SIZE; ++i) {
            fake_disk[p][i] = ((const uint8_t *)page)[i];
        }
        ++fake_writes[p];
    }
    pause = fake_write_pause;
    fake_write_pause = 0;
    if (pause) {
        pause(disk_off, page);
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
            CHECK(pc_evict(0) == -1);
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
                rc = evict ? pc_evict(0) : pc_sync();
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
                CHECK(pc_evict(0) == 0);
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

static volatile uint32_t kernel_ticks;
static uint32_t sync_waits;
static uint32_t sync_yields;
static uint32_t sync_mounts;
static uint32_t sync_release; /* the wait that ends the competing write */

/** No file is mapped shared here, so the mapped pages have nothing to give
 *  the cache. */
static void filemap_sync_all(void)
{}

/** Count the sync of the mounts. */
static void vfs_sync_all(void)
{
    ++sync_mounts;
}

/** Count a yield to the scheduler. */
void schedule(void)
{
    ++sync_yields;
}

/** Wait a tick; at wait number `sync_release` complete the competing write,
 *  redirtying page 0 and unpinning its slot, so it ends only after the
 *  caller has waited. */
void idle_wait_irq_on(void)
{
    ++kernel_ticks;
    ++sync_waits;
    if (sync_waits == sync_release) {
        touch(0, 1, 0xE2u);
        pc_slots[0].pinned = 0;
    }
}

#include "syscall_sync.inc"

/**
 * Run the kernel's `sys_sync` with a busy page, a failed write, or both,
 * and with a busy page that is never released.
 *
 * A busy page starts clean, as it does during writeback, then becomes dirty
 * before the writer releases it, and the sync must write it. A stuck slot
 * must stop the wait at its bound of 3000 ticks, the tick count wrapping on
 * the way. A failure alone must return without waiting, and a later sync
 * writes the failed page.
 */
static void test_sync_wait(void)
{
    int row;

    for (row = 0; row < 4; ++row) {
        int busy = row != 1;
        int failed = row == 1 || row == 2;

        init_pagecache();
        pc_set_backing(fake_read, fake_write, FAKE_PAGES * PC_PAGE_SIZE);
        fake_disk[0][0] = 0;
        fake_attempts[0] = 0;
        fake_attempts[1] = 0;
        fake_writes[0] = 0;
        sync_waits = sync_yields = sync_mounts = 0;
        sync_release = row == 3 ? 0u : 3u;
        kernel_ticks = 0xFFFFFFFEu;
        touch(0, 0, 0);
        pc_slots[0].pinned = (uint8_t)busy;
        if (failed) {
            touch(1, 1, 0xE1u);
            fake_write_fail = 1;
        }
        CHECK(pc_sync() == ((busy ? PC_SYNC_BUSY : 0) | (failed ? PC_SYNC_FAILED : 0)));
        fake_attempts[1] = 0;
        CHECK(sys_sync(0) == 0);
        CHECK(sync_mounts == 1);
        CHECK(sync_yields == sync_waits);
        if (busy && row != 3) {
            CHECK(sync_waits == 3);
            CHECK(fake_disk[0][0] == 0xE2u);
            CHECK(fake_writes[0] == 1);
            CHECK(!pc_slots[0].dirty && !pc_slots[0].pinned);
        } else if (row == 3) {
            CHECK(sync_waits == 3000);
            CHECK(pc_slots[0].pinned);
            CHECK(fake_attempts[0] == 0);
        } else {
            CHECK(sync_waits == 0);
            CHECK(fake_attempts[1] == 1);
        }
        if (failed) {
            CHECK(pc_slots[1].dirty);
            fake_write_fail = FAKE_NO_FAIL;
            CHECK(sys_sync(0) == 0);
            CHECK(!pc_slots[1].dirty);
            CHECK(fake_disk[1][0] == 0xE1u);
        }
        pc_slots[0].pinned = 0;
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
 * - `test_sleeping_writeback`, and with PAGECACHE_SYNC_HOSTTEST
 *   `test_sync_wait`.
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
#ifdef PAGECACHE_SYNC_HOSTTEST
    test_sync_wait();
#endif
    dprintf(1, "pagecache-hosttest: result: %s: %d failures\n", fails ? "fail" : "pass", fails);
    return fails ? 1 : 0;
}
