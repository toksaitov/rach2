#include "pagecache.h"

#ifdef PAGECACHE_HOSTTEST

/*
 * Host test build: no real frames, page tables, or device. Each slot
 * points at a static page, mapping is a no-op, and the backing is a fake the
 * test supplies. The bookkeeping (hash, clock, dirty tracking, eviction
 * order) is what runs. The system stdlib is avoided so it does not redefine
 * kint.h's types.
 */
#define PC_PAGE_SIZE 4096u

static uint8_t pc_host_pages[PC_SLOTS][PC_PAGE_SIZE];

/* The record a failed write-back raises has no reader here: the test
   counts the write it sees fail. */
#define KLOG_ERR 3
static void klog(int level, const char *subsystem, const char *format, ...)
{
    (void) level;
    (void) subsystem;
    (void) format;
}

static void pc_platform_map(uint32_t va, uintptr_t phys, int writable)
{
    (void) va;
    (void) phys;
    (void) writable;
}

static void pc_platform_unmap(uint32_t va)
{
    (void) va;
}

#else

#include "paging.h"
#include "pressure.h"
#include "../klog.h"
#include "../panic.h"
#include "../pci.h"

#define PC_PAGE_SIZE PAGE_SIZE

/* The default cap on the window when no device MMIO BAR sits below it:
   512 MiB, which is the ceiling on a mounted disk file system, sized for the
   machine that boots from its own disk, not for a test image, while
   bounding the pre-mapped page tables to 128 (512 KiB). */
#define PC_WINDOW_CAP 0x20000000u

static void pc_platform_map(uint32_t va, uintptr_t phys, int writable)
{
    map_kernel_frame(va, phys, writable);
}

static void pc_platform_unmap(uint32_t va)
{
    unmap_kernel_frame(va);
}

#endif

/* The hash maps a page number to its slot for O(1) lookup; it is twice the slot
   count, so PC_SLOTS must be a power of two. */
#define PC_HASH_SIZE    (PC_SLOTS * 2u)
#define PC_HASH_EMPTY   (-1)
#define PC_HASH_DELETED (-2)

/* One resident page: its backing offset, its frame, and the clock/dirty state.
   `page` is where the backing reads and writes the bytes (the frame's identity
   address in the kernel, a malloc'd buffer on the host); `phys` is the frame to
   map at the window virtual address. */
struct pc_entry {
    uint32_t disk_off;
    uintptr_t phys;
    void *page;
    uint8_t valid;
    uint8_t dirty;
    uint8_t accessed;
    uint8_t pinned;
};

static struct pc_entry pc_slots[PC_SLOTS];
static int32_t pc_hash[PC_HASH_SIZE];
static uint32_t pc_tombs;

static uint32_t pc_capacity = PC_SLOTS;  /* live slots; the test may shrink this */
static uint32_t pc_clock;                /* clock hand over [0, pc_capacity) */
static uint32_t pc_window_span;          /* pre-mapped window bytes */
static uint32_t pc_fs_size;              /* backed size; 0 means no backing */

static pc_read_fn pc_read;
static pc_write_fn pc_write;

static uint32_t pc_reads;
static uint32_t pc_evicts;
static uint32_t pc_writebacks;
static uint32_t pc_resident_frames; /* slots holding a frame */

/*
 * A slot's frame comes when the slot is first filled and goes when reclaim
 * takes it, so the cache holds what it uses and no more. The frame is below
 * the identity-mapped window, since the backing reads and writes it by its
 * physical address. On the host every slot has its buffer from the start.
 */
/* Whether a slot has a frame: on the host every slot has its buffer from
   the start and `phys` is never set. */
#ifdef PAGECACHE_HOSTTEST
#define PC_HAS_STORAGE(e) ((e)->page != 0)
#else
#define PC_HAS_STORAGE(e) ((e)->phys != 0)
#endif

static int pc_slot_storage(struct pc_entry *e)
{
#ifdef PAGECACHE_HOSTTEST
    return e->page != 0;
#else
    if (e->phys) {
        return 1;
    }
    if (!mem_may_take(0)) {
        return 0; /* the reserve's: a resident page gives way instead */
    }
    e->phys = (uintptr_t)alloc_kernel_frame();
    if (!e->phys) {
        return 0;
    }
    e->page = (void *)e->phys;
    ++pc_resident_frames;
    return 1;
#endif
}

static void pc_slot_release(struct pc_entry *e)
{
#ifdef PAGECACHE_HOSTTEST
    (void)e;
#else
    if (e->phys) {
        free_kernel_frame((void *)e->phys);
        e->phys = 0;
        e->page = 0;
        --pc_resident_frames;
    }
#endif
}

static uint32_t pc_page_of(uint32_t disk_off)
{
    return disk_off / PC_PAGE_SIZE;
}

/**
 * Rebuilds the lookup hash from the valid slots, clearing every tombstone.
 */
static void pc_hash_rebuild(void)
{
    uint32_t i;

    for (i = 0; i < PC_HASH_SIZE; ++i) {
        pc_hash[i] = PC_HASH_EMPTY;
    }
    pc_tombs = 0;

    for (i = 0; i < PC_SLOTS; ++i) {
        if (pc_slots[i].valid) {
            uint32_t h = pc_page_of(pc_slots[i].disk_off) & (PC_HASH_SIZE - 1u);
            while (pc_hash[h] != PC_HASH_EMPTY) {
                h = (h + 1u) & (PC_HASH_SIZE - 1u);
            }
            pc_hash[h] = (int32_t) i;
        }
    }
}

/**
 * Returns the slot caching `disk_off`'s page, or -1 on a miss.
 */
static int32_t pc_hash_find(uint32_t disk_off)
{
    uint32_t page = pc_page_of(disk_off);
    uint32_t h = page & (PC_HASH_SIZE - 1u);
    uint32_t probes = 0;

    while (pc_hash[h] != PC_HASH_EMPTY && probes < PC_HASH_SIZE) {
        int32_t s = pc_hash[h];
        if (s >= 0 && pc_slots[s].valid &&
            pc_page_of(pc_slots[s].disk_off) == page) {
            return s;
        }
        h = (h + 1u) & (PC_HASH_SIZE - 1u);
        ++probes;
    }

    return -1;
}

/**
 * Inserts `slot` under `disk_off`'s page. The key must be absent (callers
 * insert only after a miss), so the first empty or tombstoned bucket takes
 * it.
 */
static void pc_hash_insert(uint32_t disk_off, uint32_t slot)
{
    uint32_t h = pc_page_of(disk_off) & (PC_HASH_SIZE - 1u);

    while (pc_hash[h] >= 0) {
        h = (h + 1u) & (PC_HASH_SIZE - 1u);
    }
    if (pc_hash[h] == PC_HASH_DELETED && pc_tombs > 0) {
        --pc_tombs;
    }
    pc_hash[h] = (int32_t) slot;
}

/**
 * Removes `disk_off`'s page from the hash, leaving a tombstone. Rebuilds the
 * table once tombstones reach half the slot count, so probe chains stay short
 * and an insert always finds a bucket.
 */
static void pc_hash_remove(uint32_t disk_off)
{
    uint32_t page = pc_page_of(disk_off);
    uint32_t h = page & (PC_HASH_SIZE - 1u);
    uint32_t probes = 0;

    while (pc_hash[h] != PC_HASH_EMPTY && probes < PC_HASH_SIZE) {
        int32_t s = pc_hash[h];
        if (s >= 0 && pc_page_of(pc_slots[s].disk_off) == page) {
            pc_hash[h] = PC_HASH_DELETED;
            ++pc_tombs;
            if (pc_tombs >= PC_SLOTS / 2u + 1u) {
                pc_hash_rebuild();
            }
            return;
        }
        h = (h + 1u) & (PC_HASH_SIZE - 1u);
        ++probes;
    }
}

/**
 * Writes back a dirty slot, unmaps it, and frees it for reuse. Returns 0, or -1
 * if the writeback failed (then the slot is left intact and dirty). Writeback
 * precedes the unmap and reuse, so no dirty page is ever dropped.
 */
static int pc_evict(uint32_t slot)
{
    struct pc_entry *e = &pc_slots[slot];

    if (!e->valid) {
        return 0;
    }
    if (e->dirty) {
        if (pc_write(e->disk_off, e->page) != 0) {
            /* Said here, since every caller goes on: a page kept dirty
               is right, and a disk that refused a write is news. */
            klog(KLOG_ERR, "pagecache", "writeback failed at disk offset 0x%X; the page stays dirty",
                 e->disk_off);
            return -1;
        }
        e->dirty = 0;
        ++pc_writebacks;
    }
    pc_platform_unmap(PCWIN_BASE + e->disk_off);
    /* Clear valid before removing from the hash: pc_hash_remove may rebuild the
     * table, which scans the valid slots, and must not re-insert this victim.
     */
    e->valid = 0;
    pc_hash_remove(e->disk_off);
    ++pc_evicts;

    return 0;
}

/**
 * Returns a slot to fill: a free one within the live capacity, else the clock
 * victim (written back first). A victim whose writeback fails is left dirty and
 * skipped, so a clean victim is still found; returns -1 only if no victim could
 * be freed. Never returns a pinned slot.
 */
static int32_t pc_get_slot(void)
{
    uint32_t i;
    uint32_t scanned;

    /* An empty slot with a frame first, then one a frame can be had for;
       when no frame can be had, a resident page gives way below. */
    for (i = 0; i < pc_capacity; ++i) {
        if (!pc_slots[i].valid && !pc_slots[i].pinned && PC_HAS_STORAGE(&pc_slots[i])) {
            return (int32_t) i;
        }
    }
    for (i = 0; i < pc_capacity; ++i) {
        if (!pc_slots[i].valid && !pc_slots[i].pinned && pc_slot_storage(&pc_slots[i])) {
            return (int32_t) i;
        }
    }

    for (scanned = 0; scanned < pc_capacity * 3u; ++scanned) {
        uint32_t v = pc_clock;
        pc_clock = (pc_clock + 1u) % pc_capacity;
        if (pc_slots[v].pinned || !PC_HAS_STORAGE(&pc_slots[v])) {
            continue; /* held, or a slot reclaim emptied of its frame */
        }
        if (pc_slots[v].accessed) {
            pc_slots[v].accessed = 0;
            continue;
        }
        if (pc_evict(v) == 0) {
            return (int32_t) v;
        }
        /* Writeback failed: leave the page dirty and try another victim. */
    }

    return -1;
}

int pc_fault(uint32_t fault_address, int write)
{
    uint32_t page_va = fault_address & ~(PC_PAGE_SIZE - 1u);
    uint32_t disk_off;
    int32_t slot;
    struct pc_entry *e;

    if (!pc_read || page_va < PCWIN_BASE) {
        return 0;
    }
    disk_off = page_va - PCWIN_BASE;
    if (disk_off >= pc_fs_size) {
        return 0;  /* past the backed size: a wild kernel pointer */
    }

    slot = pc_hash_find(disk_off);
    if (slot >= 0) {
        /* Hit: the only fault on a present window page is a write to one mapped
           read-only, so upgrade it to writable and record the write. */
        e = &pc_slots[slot];
        e->accessed = 1;
        if (write) {
            e->dirty = 1;
            pc_platform_map(page_va, e->phys, 1);
        }
        return 1;
    }

    slot = pc_get_slot();
    if (slot < 0) {
        return 0;  /* a dirty victim could not be written back */
    }
    e = &pc_slots[slot];
    e->pinned = 1;
    e->disk_off = disk_off;
    if (pc_read(disk_off, e->page) != 0) {
        e->pinned = 0;
        return 0;  /* the fill read failed */
    }
    e->valid = 1;
    e->dirty = (uint8_t) (write ? 1 : 0);
    e->accessed = 1;
    pc_hash_insert(disk_off, (uint32_t) slot);
    pc_platform_map(page_va, e->phys, write ? 1 : 0);
    e->pinned = 0;
    ++pc_reads;

    return 1;
}

uint32_t pc_shrink(uint32_t want, int can_write, uint32_t *written)
{
    uint32_t got = 0;
    int pass;

    /* Clean pages go first, whatever their age, then, with `can_write`,
       dirty ones written back; a page whose writeback fails stays. */
    for (pass = 0; pass < (can_write ? 2 : 1) && got < want; ++pass) {
        uint32_t i;

        for (i = 0; i < PC_SLOTS && got < want; ++i) {
            struct pc_entry *e = &pc_slots[i];

            if (!PC_HAS_STORAGE(e) || e->pinned) {
                continue;
            }
            if (e->valid && (pass == 0 ? e->dirty : 0)) {
                continue;
            }
            if (e->valid && e->dirty) {
                ++(*written);
            }
            if (e->valid && pc_evict(i) != 0) {
                continue;
            }
            pc_slot_release(e);
            ++got;
        }
    }
    return got;
}

uint32_t pc_resident(void)
{
    return pc_resident_frames;
}

int pc_sync(void)
{
    uint32_t i;
    int result = 0;

    for (i = 0; i < PC_SLOTS; ++i) {
        struct pc_entry *e = &pc_slots[i];
        if (e->valid && e->dirty) {
            int wrote = (pc_write(e->disk_off, e->page) == 0);
            if (wrote) {
                e->dirty = 0;
                ++pc_writebacks;
            } else {
                result = -1;  /* leave it dirty for a later retry */
            }
            /* Re-protect read-only either way, so a future write faults and
               re-dirties (a still-dirty page just re-dirties redundantly). */
            pc_platform_map(PCWIN_BASE + e->disk_off, e->phys, 0);
        }
    }

    return result;
}

void pc_set_backing(pc_read_fn read, pc_write_fn write, uint32_t fs_size)
{
    uint32_t i;

    /* A backing switch is a remount, not a sync: drop every resident frame
       without writeback, since the new backing has nothing to do with it. */
    for (i = 0; i < PC_SLOTS; ++i) {
        if (pc_slots[i].valid) {
            pc_platform_unmap(PCWIN_BASE + pc_slots[i].disk_off);
            pc_slots[i].valid = 0;
            pc_slots[i].dirty = 0;
            pc_slots[i].accessed = 0;
        }
    }
    pc_hash_rebuild();
    pc_clock = 0;

    pc_read = read;
    pc_write = write;
    /* Clamp to the window first (so the round-up cannot overflow), then round
       up to whole pages: the window maps page-grained, so the file system's
       last partial page is a full mappable page. The window span is page-aligned, so
       the rounded size never exceeds it. */
    if (fs_size > pc_window_span) {
        fs_size = pc_window_span;
    }
    pc_fs_size = (fs_size + (PC_PAGE_SIZE - 1u)) & ~(PC_PAGE_SIZE - 1u);
}

int pc_has_backing(void)
{
    return pc_read != 0;
}

int pc_set_capacity(uint32_t slots)
{
    uint32_t i;
    uint32_t stuck = 0;
    int rc = 0;

    if (slots < 1u) {
        slots = 1u;
    }
    if (slots > PC_SLOTS) {
        slots = PC_SLOTS;
    }
    for (i = slots; i < PC_SLOTS; ++i) {
        if (pc_slots[i].valid && pc_evict(i) != 0) {
            /* The writeback failed, so this slot is still dirty. Note it: the
             * capacity must keep covering it, or the reclaim scan would never
             * revisit it and its dirty page would be stranded above capacity.
             */
            stuck = i + 1;
            rc = -1;
        }
    }
    if (stuck > slots) {
        slots = stuck;
    }
    pc_capacity = slots;
    pc_clock = 0;
    return rc;
}

uint32_t pc_window_size(void)
{
    return pc_window_span;
}

uint32_t pc_stat_reads(void)
{
    return pc_reads;
}

uint32_t pc_stat_evictions(void)
{
    return pc_evicts;
}

uint32_t pc_stat_writebacks(void)
{
    return pc_writebacks;
}

#ifdef PAGECACHE_HOSTTEST

void init_pagecache(void)
{
    uint32_t i;

    pc_window_span = 0x10000000u;
    for (i = 0; i < PC_SLOTS; ++i) {
        pc_slots[i].phys = 0;
        pc_slots[i].page = pc_host_pages[i];
        pc_slots[i].valid = 0;
        pc_slots[i].dirty = 0;
        pc_slots[i].accessed = 0;
        pc_slots[i].pinned = 0;
    }
    pc_capacity = PC_SLOTS;
    pc_clock = 0;
    pc_reads = pc_evicts = pc_writebacks = 0;
    pc_hash_rebuild();
}

/** Host-test accessor: the bytes cached for `disk_off`, or NULL on a miss. */
void *pc_test_page(uint32_t disk_off)
{
    int32_t s = pc_hash_find(disk_off & ~(PC_PAGE_SIZE - 1u));
    return (s >= 0) ? pc_slots[s].page : 0;
}

#else

/**
 * Sizes the window from PCWIN_BASE to the lowest device memory BAR that lands
 * in it, capped at PC_WINDOW_CAP, so the window never overlaps device MMIO.
 *
 * Walks every bus the driver discovery walks (`pci_find_device_skip` scans
 * buses 0 to 255), not bus 0 alone: a BAR behind a bridge that landed in the
 * window would otherwise share virtual addresses with the cache, and a later
 * `map_mmio` of it would trade PTEs with the window's fills.
 */
static uint32_t pc_probe_window_size(void)
{
    uint32_t limit = PCWIN_BASE + PC_WINDOW_CAP;
    uint16_t bus;

    for (bus = 0; bus < 256; ++bus) {
        uint8_t dev;
        for (dev = 0; dev < 32; ++dev) {
            uint8_t fn;
            for (fn = 0; fn < 8; ++fn) {
                uint8_t bari;
                if (pci_read_config16((uint8_t) bus, dev, fn, 0) == 0xFFFF) {
                    if (fn == 0) {
                        break;
                    }
                    continue;
                }
                for (bari = 0; bari < 6; ++bari) {
                    uint32_t bar = pci_read_config32((uint8_t) bus, dev, fn,
                                                     0x10 + bari * 4u);
                    uint32_t base;
                    int above_4g = 0;
                    if (!pci_bar_is_memory(bar)) {
                        continue;
                    }
                    base = pci_bar_address(bar);
                    if (pci_bar_is_64bit(bar)) {
                        /* The high dword is the next BAR slot; a non-zero high
                           puts the BAR above 4 GiB, past the 32-bit window. */
                        uint32_t high = pci_read_config32(
                            (uint8_t) bus, dev, fn, 0x10 + (bari + 1u) * 4u);
                        above_4g = (high != 0);
                        ++bari;
                    }
                    if (!above_4g && base >= PCWIN_BASE && base < limit) {
                        limit = base;
                    }
                }
            }
        }
    }

    /* Floor to a page: a clamping BAR base need only be 16-byte aligned, but
       the window span must be page-aligned so a page-rounded file-system size never
       exceeds it. */
    limit &= ~(uint32_t) (PC_PAGE_SIZE - 1u);

    return limit - PCWIN_BASE;
}

void init_pagecache(void)
{
    uint32_t i;

    pc_window_span = pc_probe_window_size();

    /* Install the window's page tables before the first fork copies the kernel
       PDEs, so every process shares them. */
    if (premap_kernel_window(PCWIN_BASE, pc_window_span) != 0) {
        k_panic("pagecache: window pre-map failed");
    }

    /* No frame is taken here: a slot gets its frame when it is first filled
       and gives it back under memory pressure, so the cache is a target the
       machine's free memory sets and not a reservation. */
    for (i = 0; i < PC_SLOTS; ++i) {
        pc_slots[i].phys = 0;
        pc_slots[i].page = 0;
        pc_slots[i].valid = 0;
        pc_slots[i].dirty = 0;
        pc_slots[i].accessed = 0;
        pc_slots[i].pinned = 0;
    }
    /* Half of what the caches together may hold under pressure, and never
       more slots than there are. */
    pc_capacity = mem_cache_target() / 2u;
    if (pc_capacity > PC_SLOTS) {
        pc_capacity = PC_SLOTS;
    }
    if (pc_capacity < 1u) {
        pc_capacity = 1u;
    }
    pc_resident_frames = 0;
    pc_clock = 0;
    pc_reads = pc_evicts = pc_writebacks = 0;
    pc_hash_rebuild();
}

#endif
