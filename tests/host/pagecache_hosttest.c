/**
 * Dual-target seam test for the page-cache bookkeeping (mm/pagecache.c).
 *
 * host-deps:
 *
 * Built with PAGECACHE_HOSTTEST and PC_SLOTS=4, which includes pagecache.c
 * directly (so the static hash, clock, and eviction logic are reachable) and
 * swaps the frame/mapping layer for static pages. A fake array stands in for
 * the disk: a fault must fill the right page once, a re-touch within residency
 * must hit (not re-read), an eviction must write back iff dirty, and a dirty page
 * thrashed out and re-read must come back with its bytes intact. Returns the
 * failed-check count (0 == pass).
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
    for (i = 0; i < PC_PAGE_SIZE; ++i) {
        fake_disk[p][i] = ((const uint8_t *)page)[i];
    }
    ++fake_writes[p];
    return 0;
}

/* Stand in for the CPU touching window page `p`: fault it (read or write), and on
   a write scribble `marker` into the cached bytes, as a store through the window
   would. */
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

    /* A read fault fills its page once; a re-touch within residency is a hit. */
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

    /* The eviction probe: write a distinct marker to pages 0..7, thrash the cache
       by reading pages 8..31 (a working set far past the 4-frame capacity), then
       re-read 0..7. Each dirty page must have been written back and must read
       back with its marker; the clean pages must never be written. */
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

    /* pc_sync writes back a dirty page and re-protects it, so a second sync with
       no new write does nothing. */
    touch(40, 1, 0xBBu);
    pc_sync();
    CHECK(fake_writes[40] == 1);
    CHECK(fake_disk[40][0] == 0xBBu);
    {
        uint32_t wb = pc_stat_writebacks();
        pc_sync();
        CHECK(pc_stat_writebacks() == wb);
    }

    return (fails > 100) ? 100 : fails;
}
