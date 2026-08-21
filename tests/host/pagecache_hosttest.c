/**
 * Dual-target host test for the page-cache bookkeeping (mm/pagecache.c).
 *
 * host-deps:
 *
 * Built with PAGECACHE_HOSTTEST and PC_SLOTS=4, which includes pagecache.c
 * directly (so the static hash, clock, and eviction logic are reachable) and
 * swaps the frame/mapping layer for static pages. A fake array stands in for
 * the disk: a fault must fill the right page once, a re-touch within
 * residency must hit (not re-read), an eviction must write back iff dirty,
 * and a dirty page thrashed out and re-read must come back with its bytes
 * intact. Returns the failed-check count (0 == pass).
 */
#define PAGECACHE_HOSTTEST
#define PC_SLOTS 4u

#include "mm/pagecache.c"

static int fails;

#define CHECK(cond)    \
    do {               \
        if (!(cond)) { \
            fails++;   \
        }              \
    } while (0)

/* A fake disk of 64 pages, with per-page read/write counters so the test can
   assert the access pattern, not just the final bytes. */
#define FAKE_PAGES 64u
static uint8_t fake_disk[FAKE_PAGES][PC_PAGE_SIZE];
static uint32_t fake_reads[FAKE_PAGES];
static uint32_t fake_writes[FAKE_PAGES];

/* Fault injection for the writeback paths: fail one page's writes, or all of
   them, so the retain-on-failure machinery can be exercised. */
#define FAKE_NO_FAIL 0xFFFFFFFFu
static uint32_t fake_write_fail = FAKE_NO_FAIL;
static int fake_write_fail_all;

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

static int fake_write(uint32_t disk_off, const void *page)
{
    uint32_t p = disk_off / PC_PAGE_SIZE;
    uint32_t i;
    if (p >= FAKE_PAGES) {
        return -1;
    }
    if (fake_write_fail_all || p == fake_write_fail) {
        return -1;
    }
    for (i = 0; i < PC_PAGE_SIZE; ++i) {
        fake_disk[p][i] = ((const uint8_t *)page)[i];
    }
    ++fake_writes[p];
    return 0;
}

/* Stand in for the CPU touching window page `p`: fault it (read or write), and
   on a write scribble `marker` into the cached bytes, as a store through the
   window would. */
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

    /* A read fault fills its page once; a re-touch within residency is a
     * hit. */
    touch(0, 0, 0);
    CHECK(fake_reads[0] == 1);
    CHECK(pc_stat_reads() == 1);
    touch(0, 0, 0);
    CHECK(fake_reads[0] == 1);
    CHECK(pc_stat_reads() == 1);

    /* The cached bytes match the backing. */
    {
        uint8_t *pg = (uint8_t *)pc_test_page(0);
        CHECK(pg != 0 && pg[0] == 0);
    }

    /* A fault past offset 0..fs_size is refused (a wild pointer). */
    CHECK(pc_fault(PCWIN_BASE + FAKE_PAGES * PC_PAGE_SIZE, 0) == 0);

    /* Filling four distinct pages fits the cache; the fifth evicts a clean page
       with no writeback. */
    touch(1, 0, 0);
    touch(2, 0, 0);
    touch(3, 0, 0);
    CHECK(pc_stat_evictions() == 0);
    touch(4, 0, 0);
    CHECK(pc_stat_evictions() == 1);
    CHECK(pc_stat_writebacks() == 0);

    /* The eviction probe: write a distinct marker to pages 0..7, thrash the
       cache by reading pages 8..31 (a working set far past the 4-frame
       capacity), then re-read 0..7. Each dirty page must have been written back
       and must read back with its marker; the clean pages must never be
       written. */
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

    /* pc_sync writes back a dirty page and re-protects it, so a second sync
     * with no new write does nothing. */
    touch(40, 1, 0xBBu);
    pc_sync();
    CHECK(fake_writes[40] == 1);
    CHECK(fake_disk[40][0] == 0xBBu);
    {
        uint32_t wb = pc_stat_writebacks();
        pc_sync();
        CHECK(pc_stat_writebacks() == wb);
    }

    /* Writeback-failure paths: a failed disk write must never drop a dirty
       page. A failed writeback on sync is reported and the page stays dirty;
       once the disk recovers, a later sync flushes it with its bytes intact. */
    init_pagecache();
    pc_set_backing(fake_read, fake_write, FAKE_PAGES * PC_PAGE_SIZE);
    touch(20, 1, 0xC0u);
    fake_write_fail = 20;
    CHECK(pc_sync() == -1);
    CHECK(fake_disk[20][0] != 0xC0u);
    fake_write_fail = FAKE_NO_FAIL;
    CHECK(pc_sync() == 0);
    CHECK(fake_disk[20][0] == 0xC0u);

    /* A shrink whose writebacks all fail is reported by pc_set_capacity, which
       must keep the capacity covering the stuck dirty frames so the RECLAIM
       path (not just sync, which scans every slot) can still reach them. Four
       dirty pages fill the four-frame cache with no eviction. */
    init_pagecache();
    pc_set_backing(fake_read, fake_write, FAKE_PAGES * PC_PAGE_SIZE);
    touch(21, 1, 0xC1u);
    touch(22, 1, 0xC2u);
    touch(23, 1, 0xC3u);
    touch(24, 1, 0xC4u);
    fake_write_fail_all = 1;
    CHECK(pc_set_capacity(1) == -1);
    /* the failed writes did not land */
    CHECK(fake_writes[21] == 0 && fake_writes[24] == 0);
    fake_write_fail_all = 0;
    /* Drive a working set far past one frame: only if the four stuck frames
       stayed inside the capacity can eviction reach and flush them. Had
       capacity collapsed to 1, slots 1..3 would be unreachable here and never
       written. No pc_sync: this proves the retain invariant through the reclaim
       path alone. */
    for (p = 30; p < 40; ++p) {
        touch(p, 0, 0);
    }
    for (p = 21; p <= 24; ++p) {
        CHECK(fake_writes[p] == 1);
        CHECK(fake_disk[p][0] == (uint8_t)(0xC0u + (p - 20u)));
    }

    return (fails > 100) ? 100 : fails;
}
