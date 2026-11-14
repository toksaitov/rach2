/**
 * pagecache_run_hosttest.c: the runs of dirty pages that
 * rach2/sys/mm/pagecache.c writes back in one write of its backing.
 *
 * host-deps:
 *
 * The ATA disk flushes its cache after each write, and under an emulator
 * that flush is a synchronous write of the disk's image file. A writeback
 * puts the dirty pages beside its page on the backing in the same write, so
 * that the walk of a lookup that evicts a page at a time behind a large copy
 * flushes once for each run and not once for each page. The rows check that
 * every dirty page is written once with its own bytes at its own place; that
 * a run takes the dirty pages before its page and after it, and stops at a
 * clean page, a page not resident, a pinned page and PC_RUN_PAGES pages; that
 * a run whose write fails leaves each of its pages dirty, tells the failure
 * hook once and is tried once in a sync's pass, though each of its pages
 * has a slot of its own; that a writeback while another run's write sleeps
 * writes its page alone, from the page's own frame; that a fault evicts a
 * clean page before a dirty one, and a dirty one, with its run, only when
 * every page is dirty; and that a page going dirty at the background
 * threshold wakes the flusher, whose writeback leaves the count under it.
 *
 * Built with PAGECACHE_HOSTTEST and 128 slots, it includes pagecache.c
 * whole, over a model disk of 128 pages that logs each write.
 */
#define PAGECACHE_HOSTTEST
#define PC_SLOTS 128u

/* The hash rebuild panics when its table has no empty entry, which the
 * sizes make impossible; the stub below traps, which fails the test. */
void k_panic(const char *s);

#include "mm/pagecache.c"

extern int printf(const char *fmt, ...);

/** The kernel's panic: stop the test with a trap. */
void k_panic(const char *s)
{
    (void)s;
    __builtin_trap();
}

static int fails;

/** Count a failed check and print what it checked. */
static void check(int ok, const char *what)
{
    if (!ok) {
        fails++;
        printf("pagecache_run_hosttest: FAIL: %s\n", what);
    }
}

#define DISK_PAGES 128u
#define LOG_MAX    64u

static uint8_t disk[DISK_PAGES][PC_PAGE_SIZE];
static uint32_t page_writes[DISK_PAGES];
/* Each write the backing took: its first page and its count. */
static uint32_t log_first[LOG_MAX], log_count[LOG_MAX], log_n;
static const void *log_from[LOG_MAX];
static int fail_next;              /* fail the next write */
static void (*during_write)(void); /* run once inside the next write */
static int hook_calls;
static int kicks; /* wakes of the flusher */

/** The backing's read: the disk's page. Returns 0, or -1 past the disk. */
static int disk_read(uint32_t disk_off, void *page)
{
    uint32_t p = disk_off / PC_PAGE_SIZE, i;

    if (p >= DISK_PAGES) {
        return -1;
    }
    for (i = 0; i < PC_PAGE_SIZE; ++i) {
        ((uint8_t *)page)[i] = disk[p][i];
    }
    return 0;
}

/**
 * The backing's write: log it, store its pages unless `fail_next` fails it,
 * and run `during_write` once, as another caller running while a write
 * sleeps. Returns 0, or -1 for a failed write and one past the disk.
 */
static int disk_write(uint32_t disk_off, const void *pages, uint32_t count)
{
    uint32_t first = disk_off / PC_PAGE_SIZE, k, i;
    void (*during)(void) = during_write;
    int failed = fail_next;

    if (count == 0 || count > PC_RUN_PAGES || first >= DISK_PAGES || count > DISK_PAGES - first ||
        log_n == LOG_MAX) {
        check(0, "a write within the disk, of 1 to PC_RUN_PAGES pages");
        return -1;
    }
    log_first[log_n] = first;
    log_count[log_n] = count;
    log_from[log_n] = pages;
    log_n++;
    fail_next = 0;
    if (!failed) {
        for (k = 0; k < count; ++k) {
            for (i = 0; i < PC_PAGE_SIZE; ++i) {
                disk[first + k][i] = ((const uint8_t *)pages)[k * PC_PAGE_SIZE + i];
            }
            page_writes[first + k]++;
        }
    }
    during_write = 0;
    if (during) {
        during();
    }
    return failed ? -1 : 0;
}

/** The file system's failure hook: count the call. */
static void hook(int rc)
{
    (void)rc;
    hook_calls++;
}

/** The flusher's wake: count it. */
static void kick(void)
{
    kicks++;
}

/** Start each row on an empty cache over a disk whose pages hold their
 *  numbers, with the logs and counts cleared. */
static void fresh(void)
{
    uint32_t p, i;

    for (p = 0; p < DISK_PAGES; ++p) {
        for (i = 0; i < PC_PAGE_SIZE; ++i) {
            disk[p][i] = (uint8_t)p;
        }
        page_writes[p] = 0;
    }
    log_n = 0;
    fail_next = 0;
    during_write = 0;
    hook_calls = 0;
    kicks = 0;
    init_pagecache();
    pc_set_backing(disk_read, disk_write, DISK_PAGES * PC_PAGE_SIZE);
    pc_on_failure(hook);
}

/** Read page `p` through the window, or write `marker` over its first and
 *  last bytes, as stores through the window would. */
static void touch(uint32_t p, int write, uint8_t marker)
{
    uint8_t *page;

    check(pc_fault(PCWIN_BASE + p * PC_PAGE_SIZE, write) == 1, "a fault inside the disk is served");
    if (write) {
        page = (uint8_t *)pc_test_page(p * PC_PAGE_SIZE);
        check(page != 0, "a written page is resident");
        if (page) {
            page[0] = marker;
            page[PC_PAGE_SIZE - 1u] = marker;
        }
    }
}

/** The slot that holds page `p`, which must be resident. */
static struct pc_entry *slot_of(uint32_t p)
{
    int32_t s = pc_hash_find(p * PC_PAGE_SIZE);

    check(s >= 0, "the page is resident");
    return s >= 0 ? &pc_slots[s] : &pc_slots[0];
}

/** Whether the disk's page `p` holds `marker` at both ends. */
static int on_disk(uint32_t p, uint8_t marker)
{
    return disk[p][0] == marker && disk[p][PC_PAGE_SIZE - 1u] == marker;
}

/**
 * Forty dirty pages in a row and one apart: a sync writes the forty in two
 * runs, of PC_RUN_PAGES and of the rest, and the one alone, so three writes
 * in all, each page once with its own bytes; a second sync writes nothing.
 */
static void test_sync_runs(void)
{
    uint32_t p, total = 0, k;

    fresh();
    for (p = 10; p < 50; ++p) {
        touch(p, 1, (uint8_t)(0x80u + p));
    }
    touch(60, 1, 0xF0u);
    check(pc_sync() == 0, "the sync writes every dirty page");
    check(log_n == 3, "forty pages in a row and one apart take three writes");
    for (k = 0; k < log_n; ++k) {
        check(log_count[k] <= PC_RUN_PAGES, "no run passes PC_RUN_PAGES");
        total += log_count[k];
    }
    check(total == 41, "the writes hold the 41 dirty pages");
    check(log_n == 3 && log_first[0] == 10 && log_count[0] == PC_RUN_PAGES,
          "the first run is the first PC_RUN_PAGES pages from page 10");
    check(log_n == 3 && log_first[1] == 10 + PC_RUN_PAGES && log_count[1] == 40 - PC_RUN_PAGES,
          "the second run is the rest of the forty");
    check(log_n == 3 && log_first[2] == 60 && log_count[2] == 1, "the page apart goes alone");
    for (p = 10; p < 50; ++p) {
        check(page_writes[p] == 1 && on_disk(p, (uint8_t)(0x80u + p)),
              "each page of the forty is written once with its own bytes");
        check(!slot_of(p)->dirty, "each page of the forty is clean after the sync");
    }
    check(page_writes[60] == 1 && on_disk(60, 0xF0u), "the page apart is written");
    check(page_writes[9] == 0 && page_writes[50] == 0, "no clean page is written");
    check(pc_stat_writebacks() == 41, "the writebacks count the pages written");
    log_n = 0;
    check(pc_sync() == 0 && log_n == 0, "a second sync writes nothing");
}

/**
 * An eviction of a page in the middle of dirty pages 20 to 29 writes the
 * ten in one write from page 20, drops only the page evicted, and leaves
 * the other nine resident and clean.
 */
static void test_evict_middle(void)
{
    uint32_t p;

    fresh();
    for (p = 20; p < 30; ++p) {
        touch(p, 1, (uint8_t)(0x40u + p));
    }
    check(pc_evict((uint32_t)(slot_of(25) - pc_slots)) == 0, "the eviction succeeds");
    check(log_n == 1 && log_first[0] == 20 && log_count[0] == 10,
          "the eviction writes the run from its first dirty page");
    check(pc_test_page(25 * PC_PAGE_SIZE) == 0, "the page evicted is gone");
    for (p = 20; p < 30; ++p) {
        check(on_disk(p, (uint8_t)(0x40u + p)), "each page of the run is on the disk");
        if (p != 25) {
            check(pc_test_page(p * PC_PAGE_SIZE) != 0 && !slot_of(p)->dirty,
                  "each other page stays resident and clean");
        }
    }
}

/**
 * Runs stop where the pages stop being dirty, resident and free: dirty
 * pages 0 to 3, page 4 clean, 5 and 6 dirty, page 7 not resident, 8 and 9
 * dirty, 10 dirty and pinned, 11 dirty. The sync writes 0-3, 5-6, 8-9 and
 * 11 apart, and passes page 10, which stays dirty.
 */
static void test_run_bounds(void)
{
    static const uint32_t first[4] = {0, 5, 8, 11};
    static const uint32_t count[4] = {4, 2, 2, 1};
    uint32_t p, k;
    struct pc_entry *pinned;

    fresh();
    for (p = 0; p < 12; ++p) {
        if (p == 4) {
            touch(p, 0, 0);
        } else if (p != 7) {
            touch(p, 1, (uint8_t)(0x20u + p));
        }
    }
    pinned = slot_of(10);
    pinned->pinned = 1;
    check(pc_sync() == PC_SYNC_BUSY, "the sync reports the pinned page");
    check(log_n == 4, "four runs");
    for (k = 0; k < 4 && k < log_n; ++k) {
        check(log_first[k] == first[k] && log_count[k] == count[k],
              "each run stops at a clean, absent or pinned page");
    }
    check(page_writes[4] == 0 && page_writes[7] == 0 && page_writes[10] == 0,
          "the clean, absent and pinned pages are not written");
    check(pinned->dirty, "the pinned page stays dirty");
    pc_unpin(pinned);
    pinned->waited = 0;
    log_n = 0;
    check(pc_sync() == 0 && log_n == 1 && log_first[0] == 10 && log_count[0] == 1,
          "once let go, the page is written alone");
}

/**
 * A run whose write fails stays dirty, every page of it, the hook is told
 * once, and the pass that failed tries it no more; the next sync writes the
 * same run.
 */
static void test_failed_run(void)
{
    uint32_t p;

    fresh();
    for (p = 30; p < 36; ++p) {
        touch(p, 1, (uint8_t)(0x60u + p));
    }
    fail_next = 1;
    check(pc_sync() == PC_SYNC_FAILED, "the sync reports the failure");
    check(log_n == 1 && log_count[0] == 6, "the failed write was the whole run, tried once");
    check(hook_calls == 1, "the hook is told once for the run");
    for (p = 30; p < 36; ++p) {
        check(slot_of(p)->dirty && !slot_of(p)->pinned, "each page of the failed run stays dirty");
        check(page_writes[p] == 0, "no page of the failed run is stored");
    }
    log_n = 0;
    check(pc_sync() == 0 && log_n == 1 && log_first[0] == 30 && log_count[0] == 6,
          "the next sync writes the run");
    for (p = 30; p < 36; ++p) {
        check(on_disk(p, (uint8_t)(0x60u + p)) && !slot_of(p)->dirty,
              "each page of the run is then on the disk and clean");
    }
}

/* The page the writeback inside a sleeping run's write takes. */
#define LONE_PAGE 100u

/** Inside the write of run 40-47: write back page 100, apart from it, and
 *  dirty page 41 again, as a store during the write would. */
static void writeback_during_run(void)
{
    uint8_t *page;

    check(pc_run_busy, "the run's buffer is taken during its write");
    check(pc_writeback(slot_of(LONE_PAGE)) == 0, "a writeback during the run's write succeeds");
    check(log_n == 2 && log_first[1] == LONE_PAGE && log_count[1] == 1, "it writes its page alone");
    check(log_n == 2 && log_from[1] == pc_test_page(LONE_PAGE * PC_PAGE_SIZE),
          "from the page's own frame");
    check(slot_of(41)->pinned && !slot_of(41)->dirty, "a page of the run is pinned and clean");
    touch(41, 1, 0xEEu);
    page = (uint8_t *)pc_test_page(41 * PC_PAGE_SIZE);
    check(page != 0 && page[0] == 0xEEu, "a store during the write reaches the page");
}

/**
 * A writeback that runs while a run's write sleeps finds the buffer taken
 * and writes its page alone; a store to a page of the run during the write
 * leaves that page dirty with its new bytes, and the run's bytes are those
 * copied before the write.
 */
static void test_busy_buffer(void)
{
    uint32_t p;

    fresh();
    for (p = 40; p < 48; ++p) {
        touch(p, 1, (uint8_t)(0xA0u + p));
    }
    touch(LONE_PAGE, 1, 0x77u);
    during_write = writeback_during_run;
    check(pc_writeback(slot_of(40)) == 0, "the run's writeback succeeds");
    check(!pc_run_busy, "the buffer is free after the run");
    check(log_n == 2 && log_first[0] == 40 && log_count[0] == 8, "the run was pages 40 to 47");
    check(on_disk(LONE_PAGE, 0x77u), "the lone page is on the disk");
    check(on_disk(41, (uint8_t)(0xA0u + 41)), "the run wrote page 41 as it was copied");
    check(slot_of(41)->dirty, "page 41, stored to during the write, stays dirty");
    for (p = 40; p < 48; ++p) {
        check(!slot_of(p)->pinned, "every pin of the run is let go");
        if (p != 41) {
            check(!slot_of(p)->dirty, "every other page of the run is clean");
        }
    }
    log_n = 0;
    check(pc_sync() == 0 && log_n == 1 && log_first[0] == 41 && log_count[0] == 1,
          "the next sync writes page 41 alone");
    check(disk[41][0] == 0xEEu, "with the bytes the store left");
}

/**
 * On four slots, pages 0 and 1 dirty and 2 and 3 clean: a fault of page 10
 * takes a clean page and writes nothing, and so does one of page 11; with
 * the four slots then dirty, a fault of page 12 writes the run of 0 and 1.
 */
static void test_clean_first(void)
{
    fresh();
    check(pc_set_capacity(4) == 0, "the capacity falls to four");
    touch(0, 1, 0x10u);
    touch(1, 1, 0x11u);
    touch(2, 0, 0);
    touch(3, 0, 0);
    touch(10, 0, 0);
    check(log_n == 0, "a fault with clean pages to take writes nothing");
    check(pc_test_page(0) != 0 && pc_test_page(PC_PAGE_SIZE) != 0, "the dirty pages stay");
    touch(11, 0, 0);
    check(log_n == 0, "a second fault takes the other clean page");
    check(pc_dirty_count() == 2, "two pages are dirty");
    touch(10, 1, 0x1Au);
    touch(11, 1, 0x1Bu);
    touch(12, 0, 0);
    check(log_n == 1 && log_count[0] == 2 && (log_first[0] == 0 || log_first[0] == 10),
          "with every page dirty, a fault writes a run of two");
    check(pc_dirty_count() == 2, "the run's two pages are clean, the others dirty");
}

/**
 * On 100 slots and a background ratio of 10 per cent, nine dirty pages
 * wake nothing and the tenth wakes the flusher, as each one after it does;
 * the level is then 1. The background writeback writes runs until the
 * count is under ten, here the whole run of pages 50 to 69 in one write.
 */
static void test_background(void)
{
    uint32_t p, written;

    fresh();
    check(pc_set_capacity(100) == 0, "the capacity falls to 100");
    pc_set_flusher(kick);
    for (p = 50; p < 59; ++p) {
        touch(p, 1, (uint8_t)p);
    }
    check(kicks == 0 && pc_dirty_level() == 0, "nine dirty pages are under the threshold");
    touch(59, 1, 59);
    check(kicks == 1 && pc_dirty_level() == 1, "the tenth wakes the flusher");
    for (p = 60; p < 70; ++p) {
        touch(p, 1, (uint8_t)p);
    }
    check(kicks == 11, "each page dirtied past the threshold wakes it");
    touch(60, 1, 0x60u);
    check(kicks == 11, "a store to a page already dirty wakes nothing");
    written = pc_write_background();
    check(written == 20 && log_n == 1 && log_first[0] == 50 && log_count[0] == 20,
          "the background writeback writes the run of twenty in one write");
    check(pc_dirty_count() == 0 && pc_dirty_level() == 0, "the count is under the threshold");
    for (p = 50; p < 70; ++p) {
        check(page_writes[p] == 1, "each page is written once");
    }
    touch(80, 1, 0x80u);
    log_n = 0;
    check(pc_write_background() == 0 && log_n == 0, "under the threshold it writes nothing");
    pc_set_flusher(0);
}

/** Run the rows. Returns 0 when every check passed and 1 otherwise. */
int main(void)
{
    test_sync_runs();
    test_evict_middle();
    test_run_bounds();
    test_failed_run();
    test_busy_buffer();
    test_clean_first();
    test_background();
    if (fails == 0) {
        printf("pagecache_run_hosttest: ok\n");
    }
    return fails != 0;
}
