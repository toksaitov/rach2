/**
 * pagecache_ring_hosttest.c: a lookup on a KRFFS disk root and the page
 * cache's slots.
 *
 * host-deps:
 *
 * A lookup on a KRFFS volume reads the header of every node in the ring
 * (`krffs_find_child`), and on a disk root each of those reads goes through
 * the page cache. A compile on such a root needs the ring's header pages and
 * its own pages resident together; when they do not fit, every lookup
 * refills the cache from the disk. That is what the manual's pages did to
 * the self-build suite at 1024 slots, until `PC_SLOTS` was raised to 2048
 * (entry 110 of milestone 50).
 *
 * This is a model of that on a count. The os-real image staged 817 nodes
 * when the row was written, their headers on 492 pages, 66 of them the
 * manual's. The model's compile has 560 pages of its own: with that many,
 * the ring without the manual, 426 pages, fits 1024 slots beside them and
 * the ring with the manual does not, which is what the suite measured. A
 * round is the compile's pages and then two lookups, each reading every
 * header page in ring order; the first round fills the cache, and the rows
 * count the pages the next three read from the backing. At `PC_SLOTS` they
 * must read none. Two control rows run the same rounds at 1024 slots, the
 * old size: without the manual they read none, and with it every round reads
 * the ring again, so the model does show the fault.
 *
 * Built with PAGECACHE_HOSTTEST and the slots pagecache.h sets, which
 * includes pagecache.c directly; the backing is a function that stamps each
 * page with its number. `pagecache_ring_mutate.sh` sets the slots back to
 * 1024 and requires the row at `PC_SLOTS` to fail.
 */
#define PAGECACHE_HOSTTEST

/* The hash rebuild panics when its table has no empty entry, which the
 * sizes make impossible; the stub below traps, which fails the test. */
void k_panic(const char *s);

#include "mm/pagecache.c"

extern int printf(const char *fmt, ...);

void k_panic(const char *s)
{
    (void)s;
    __builtin_trap();
}

/* The ring's header pages with the manual staged, the manual's share of
 * them, the compile's own pages, and the slots the cache had before. */
#define RING_PAGES   492u
#define MANUAL_PAGES 66u
#define HOT_PAGES    560u
#define OLD_SLOTS    1024u
/* A round to fill the cache, then the rounds that are counted. */
#define ROUNDS       4u

static int fails;
static uint32_t unserved;

static void check(int ok, const char *what)
{
    if (!ok) {
        fails++;
        printf("pagecache_ring_hosttest: FAIL: %s\n", what);
    }
}

static int backing_read(uint32_t disk_off, void *page)
{
    ((uint8_t *)page)[0] = (uint8_t)(disk_off / PC_PAGE_SIZE);
    return 0;
}

static int backing_write(uint32_t disk_off, const void *page)
{
    (void)disk_off;
    (void)page;
    return 0;
}

/* Stand in for a read through the window: fault page `p` in. */
static void touch(uint32_t p)
{
    if (pc_fault(PCWIN_BASE + p * PC_PAGE_SIZE, 0) != 1) {
        unserved++;
    }
}

/* One round of a compile over a ring of `ring` header pages: the compile's
 * own pages, which lie after the ring, and two lookups. */
static void compile_round(uint32_t ring)
{
    uint32_t p, walk;

    for (p = ring; p < ring + HOT_PAGES; p++) {
        touch(p);
    }
    for (walk = 0; walk < 2; walk++) {
        for (p = 0; p < ring; p++) {
            touch(p);
        }
    }
}

/* The pages the rounds after the first read from the backing, on a cache of
 * `slots` slots, or of PC_SLOTS when that is fewer. */
static uint32_t fills_once_warm(uint32_t ring, uint32_t slots)
{
    uint32_t round, before;

    init_pagecache();
    pc_set_backing(backing_read, backing_write, (ring + HOT_PAGES) * PC_PAGE_SIZE);
    if (slots < PC_SLOTS) {
        pc_set_capacity(slots);
    }
    compile_round(ring);
    before = pc_stat_reads();
    for (round = 1; round < ROUNDS; round++) {
        compile_round(ring);
    }
    return pc_stat_reads() - before;
}

int main(void)
{
    uint32_t now = fills_once_warm(RING_PAGES, PC_SLOTS);
    uint32_t old_without = fills_once_warm(RING_PAGES - MANUAL_PAGES, OLD_SLOTS);
    uint32_t old_with = fills_once_warm(RING_PAGES, OLD_SLOTS);

    check(unserved == 0, "every page of the model is served");
    check(old_without == 0, "at 1024 slots the ring without the manual fits beside a compile");
    check(old_with >= (ROUNDS - 1u) * RING_PAGES,
          "and with the manual every round reads the ring from the disk again");
    check(now == 0,
          "at PC_SLOTS a compile's lookups over the ring with the manual read nothing from the "
          "disk once warm");
    printf("pagecache_ring_hosttest: fills once warm: %u at %u slots, %u and %u at 1024 slots "
           "without and with the manual\n",
           now, (unsigned)PC_SLOTS, old_without, old_with);
    if (fails == 0) {
        printf("pagecache_ring_hosttest: result: pass: the ring's header pages and a compile's "
               "fit the page cache\n");
    }
    return fails != 0;
}
