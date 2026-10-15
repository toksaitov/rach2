/* pagecache.c: the page cache of pagecache.h, and its build for the host
 * tests. */

#include "pagecache.h"

#ifdef PAGECACHE_HOSTTEST

/*
 * The host tests' build has no frames, page tables or device: each slot
 * points at a static page, mapping does nothing, and the test supplies the
 * backing. The bookkeeping (hash, clock, dirty tracking, eviction order) is
 * what runs. The system stdlib is avoided so it does not redefine kint.h's
 * types.
 */
#define PC_PAGE_SIZE 4096u

static uint8_t pc_host_pages[PC_SLOTS][PC_PAGE_SIZE];

#define KLOG_ERR 3

/** The record a failed writeback leaves has no reader here: the test counts
 * the write it sees fail. */
static void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

/** Map nothing: the host tests' pages are not mapped. */
static void pc_platform_map(uintptr_t va, uintptr_t phys, int writable)
{
    (void)va;
    (void)phys;
    (void)writable;
}

/** Unmap nothing. */
static void pc_platform_unmap(uintptr_t va)
{
    (void)va;
}

#else

#include "paging.h"
#include "pressure.h"
#include "../klog.h"
#include "../panic.h"
#include "../pci.h"

#define PC_PAGE_SIZE  PAGE_SIZE

/* The window's size when no device memory BAR lies inside it: 512 MiB, the
 * window's whole region in the kernel's layout (cpu.h), and the ceiling on a
 * mounted disk file system, sized for the machine that boots from its own
 * disk, not for a test image. */
#define PC_WINDOW_CAP 0x20000000u

/** Map the frame at `phys` at the window address `va`, writable when
 * `writable` says (`map_kernel_frame`). */
static void pc_platform_map(uintptr_t va, uintptr_t phys, int writable)
{
    map_kernel_frame(va, phys, writable);
}

/** Unmap the window address `va` (`unmap_kernel_frame`). */
static void pc_platform_unmap(uintptr_t va)
{
    unmap_kernel_frame(va);
}

#endif

/* The hash maps a page number to its slot for O(1) lookup; it is twice the
 * slot count, so PC_SLOTS must be a power of two. An entry is a slot's
 * index, PC_HASH_EMPTY, or PC_HASH_DELETED for a removed one. */
#define PC_HASH_SIZE    (PC_SLOTS * 2u)
#define PC_HASH_EMPTY   (-1)
#define PC_HASH_DELETED (-2)

/* One slot of the cache: its backing offset, its frame, and the clock and
 * dirty state. `page` is where the backing reads and writes the bytes (the
 * kernel's address of the frame, `phys_to_virt` of it, or a static page on
 * the host); `phys` is the frame to map at the window address, 0 while the
 * slot has none. */
struct pc_entry {
    uint32_t disk_off;
    uintptr_t phys;
    void *page;
    uint8_t valid;    /* the slot holds the page at `disk_off` */
    uint8_t dirty;    /* written since its last writeback */
    uint8_t accessed; /* touched since the clock hand last passed */
    uint8_t pinned;   /* a fill or a writeback is in flight */
};

static struct pc_entry pc_slots[PC_SLOTS];
static int32_t pc_hash[PC_HASH_SIZE];
static uint32_t pc_tombs; /* PC_HASH_DELETED entries in the hash */

static uint32_t pc_capacity = PC_SLOTS; /* live slots; the test may shrink this */
static uint32_t pc_clock;               /* clock hand over [0, pc_capacity) */
static uint32_t pc_window_span;         /* pre-mapped window bytes */
static uint32_t pc_fs_size;             /* backed size; 0 means no backing */

static pc_read_fn pc_read;
static pc_write_fn pc_write;

static uint32_t pc_reads;
static uint32_t pc_evicts;
static uint32_t pc_writebacks;
static uint32_t pc_resident_frames; /* slots holding a frame */

/* Whether a slot has a frame: on the host every slot has its buffer from
 * the start and `phys` is never set. */
#ifdef PAGECACHE_HOSTTEST
#define PC_HAS_STORAGE(e) ((e)->page != 0)
#else
#define PC_HAS_STORAGE(e) ((e)->phys != 0)
#endif

/**
 * Give slot `e` a frame if it has none: 1 when it has one, 0 when none can
 * be had or the free frames are the reserve's.
 *
 * A slot's frame comes when the slot is first filled and goes when reclaim
 * takes it, so the cache has what it uses and no more. The frame is of the
 * DMA pool, since a disk's backing may give its address to the device. On
 * the host every slot has its buffer from the start.
 */
static int pc_slot_storage(struct pc_entry *e)
{
#ifdef PAGECACHE_HOSTTEST
    return e->page != 0;
#else
    void *frame;

    if (e->phys) {
        return 1;
    }
    if (!mem_may_take(0)) {
        return 0;
    }
    frame = alloc_kernel_frame();
    if (!frame) {
        return 0;
    }
    e->phys = virt_to_phys(frame);
    e->page = frame;
    ++pc_resident_frames;
    return 1;
#endif
}

/** Give slot `e`'s frame back, if it has one; on the host, nothing. */
static void pc_slot_release(struct pc_entry *e)
{
#ifdef PAGECACHE_HOSTTEST
    (void)e;
#else
    if (e->phys) {
        free_kernel_frame(e->page);
        e->phys = 0;
        e->page = 0;
        --pc_resident_frames;
    }
#endif
}

/** The page number of the backing offset `disk_off`, the hash's key. */
static uint32_t pc_page_of(uint32_t disk_off)
{
    return disk_off / PC_PAGE_SIZE;
}

/**
 * Rebuild the lookup hash from the valid slots, clearing every tombstone.
 * The table has more entries than there are slots, so an empty one is
 * found within the table's length.
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
            uint32_t probes = 0;

            while (pc_hash[h] != PC_HASH_EMPTY) {
                h = (h + 1u) & (PC_HASH_SIZE - 1u);
                if (++probes == PC_HASH_SIZE) {
                    k_panic("pagecache: the hash table has no empty entry");
                }
            }
            pc_hash[h] = (int32_t)i;
        }
    }
}

/**
 * The slot that caches `disk_off`'s page, or -1 on a miss.
 */
static int32_t pc_hash_find(uint32_t disk_off)
{
    uint32_t page = pc_page_of(disk_off);
    uint32_t h = page & (PC_HASH_SIZE - 1u);
    uint32_t probes = 0;

    while (pc_hash[h] != PC_HASH_EMPTY && probes < PC_HASH_SIZE) {
        int32_t s = pc_hash[h];
        if (s >= 0 && pc_slots[s].valid && pc_page_of(pc_slots[s].disk_off) == page) {
            return s;
        }
        h = (h + 1u) & (PC_HASH_SIZE - 1u);
        ++probes;
    }

    return -1;
}

/**
 * Insert `slot` under `disk_off`'s page. The key must be absent (callers
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
    pc_hash[h] = (int32_t)slot;
}

/**
 * Remove `disk_off`'s page from the hash, leaving a tombstone. The table is
 * rebuilt once tombstones pass half the slot count, so probe chains stay
 * short and an insert always finds a bucket.
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
 * Write the resident page of `e` back if it is dirty, with the slot pinned
 * meanwhile: 0 when it is clean or written, -1 when the slot is pinned or
 * the write fails, which leaves the page dirty.
 *
 * The pin is tested before the dirty mark, since a write in flight clears
 * the mark for its length. A system call switches processes only when it
 * yields or sleeps, and a page fault runs with interrupts off, so the pin
 * is taken before another caller can enter. The backing's write may sleep
 * with the pin held.
 *
 * The page is mapped read-only and its mark cleared before the write, so a
 * store meanwhile faults and marks the page dirty again, and a successful
 * write leaves that mark set.
 */
static int pc_writeback(struct pc_entry *e)
{
    int rc;

    if (e->pinned) {
        return -1;
    }
    if (!e->dirty) {
        return 0;
    }
    e->pinned = 1;
    pc_platform_map(PCWIN_BASE + e->disk_off, e->phys, 0);
    e->dirty = 0;
    rc = pc_write(e->disk_off, e->page);
    if (rc != 0) {
        klog(KLOG_ERR, "pagecache", "writeback failed at disk offset 0x%X; the page stays dirty",
             e->disk_off);
        e->dirty = 1;
    } else {
        ++pc_writebacks;
    }
    e->pinned = 0;
    return rc != 0 ? -1 : 0;
}

/**
 * Write back the page of `slot` if it is dirty, as `pc_writeback` does, and
 * then unmap it and empty the slot, which keeps its frame. Returns 0 when
 * the slot is emptied or was empty, and -1, with the page kept, when the
 * slot is pinned, the writeback fails, or a store dirtied the page again
 * during the write.
 *
 * The slot is emptied before its hash entry is removed, since the removal
 * may rebuild the hash from the valid slots.
 */
static int pc_evict(uint32_t slot)
{
    struct pc_entry *e = &pc_slots[slot];

    if (!e->valid) {
        return 0;
    }
    if (pc_writeback(e) != 0 || e->dirty) {
        return -1;
    }
    pc_platform_unmap(PCWIN_BASE + e->disk_off);
    e->valid = 0;
    pc_hash_remove(e->disk_off);
    ++pc_evicts;

    return 0;
}

/**
 * A slot to fill, never a pinned one: an empty slot within the live
 * capacity that has a frame, else one a frame can be had for, else the
 * clock's victim, written back first. When no frame can be had, a resident
 * page gives way. The clock passes a slot reclaim emptied of its frame, and
 * a victim whose writeback fails is left dirty and passed, so a clean
 * victim is still found; -1 only when no victim could be emptied.
 */
static int32_t pc_get_slot(void)
{
    uint32_t i;
    uint32_t scanned;

    for (i = 0; i < pc_capacity; ++i) {
        if (!pc_slots[i].valid && !pc_slots[i].pinned && PC_HAS_STORAGE(&pc_slots[i])) {
            return (int32_t)i;
        }
    }
    for (i = 0; i < pc_capacity; ++i) {
        if (!pc_slots[i].valid && !pc_slots[i].pinned && pc_slot_storage(&pc_slots[i])) {
            return (int32_t)i;
        }
    }

    for (scanned = 0; scanned < pc_capacity * 3u; ++scanned) {
        uint32_t v = pc_clock;
        pc_clock = (pc_clock + 1u) % pc_capacity;
        if (pc_slots[v].pinned || !PC_HAS_STORAGE(&pc_slots[v])) {
            continue;
        }
        if (pc_slots[v].accessed) {
            pc_slots[v].accessed = 0;
            continue;
        }
        if (pc_evict(v) == 0) {
            return (int32_t)v;
        }
    }

    return -1;
}

/** A hit is a write to a page mapped read-only, the only fault a resident
 * page of the window takes: the page is made writable and marked dirty. */
int pc_fault(uintptr_t fault_address, int write)
{
    uintptr_t page_va = fault_address & ~(uintptr_t)(PC_PAGE_SIZE - 1u);
    uint32_t disk_off;
    int32_t slot;
    struct pc_entry *e;

    if (!pc_read || page_va < PCWIN_BASE) {
        return 0;
    }
    if (page_va - PCWIN_BASE >= pc_fs_size) {
        return 0;
    }
    disk_off = (uint32_t)(page_va - PCWIN_BASE);

    slot = pc_hash_find(disk_off);
    if (slot >= 0) {
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
        return 0;
    }
    e = &pc_slots[slot];
    e->pinned = 1;
    e->disk_off = disk_off;
    if (pc_read(disk_off, e->page) != 0) {
        e->pinned = 0;
        return 0;
    }
    e->valid = 1;
    e->dirty = (uint8_t)(write ? 1 : 0);
    e->accessed = 1;
    pc_hash_insert(disk_off, (uint32_t)slot);
    pc_platform_map(page_va, e->phys, write ? 1 : 0);
    e->pinned = 0;
    ++pc_reads;

    return 1;
}

uint32_t pc_shrink(uint32_t want, int can_write, uint32_t *written)
{
    uint32_t got = 0;
    int pass;

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

/**
 * Busy slots and failed writes are reported apart, so that a competing sync
 * can wait for the writes in flight without trying a failed write again for
 * ever. A failure does not stop the walk over the other slots.
 */
int pc_sync(void)
{
    uint32_t i;
    int result = 0;

    for (i = 0; i < PC_SLOTS; ++i) {
        struct pc_entry *e = &pc_slots[i];
        if (!e->valid) {
            continue;
        }
        if (e->pinned) {
            result |= PC_SYNC_BUSY;
        } else if (pc_writeback(e) != 0) {
            result |= PC_SYNC_FAILED;
        }
    }

    return result;
}

/** The size is clamped to the window before it is rounded up to a page, so
 * that the rounding cannot overflow; the window's span is page-aligned, so
 * the rounded size never passes it. The window maps whole pages, so the
 * file system's last partial page is a full page there. */
void pc_set_backing(pc_read_fn read, pc_write_fn write, uint32_t fs_size)
{
    uint32_t i;

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
    if (fs_size > pc_window_span) {
        fs_size = pc_window_span;
    }
    pc_fs_size = (fs_size + (PC_PAGE_SIZE - 1u)) & ~(PC_PAGE_SIZE - 1u);
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

/** The host tests' window is 256 MiB, and each slot has its static page. */
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
 * Sizes the window from PCWIN_BASE to the lowest device memory BAR whose
 * physical address lies in it, capped at PC_WINDOW_CAP.
 *
 * Walks every bus the driver discovery walks (`pci_find_device_skip` scans
 * buses 0 to 255), not bus 0 alone, so a BAR behind a bridge counts too. A
 * BAR's physical address and the window's virtual ones do not meet:
 * `map_mmio` maps a BAR into the kernel's MMIO region, whatever its physical
 * address. The comparison is made in 64 bits, a 64-bit BAR's high dword
 * included, so it means the same on either architecture; on x86-64 the
 * window lies above any physical address and is always the cap.
 *
 * The limit is rounded down to a page: a BAR's base need only be 16-byte
 * aligned, but the window's span must be page-aligned, so that a file
 * system's size rounded up to a page never passes it.
 */
static uint32_t pc_probe_window_size(void)
{
    uint64_t limit = (uint64_t)PCWIN_BASE + PC_WINDOW_CAP;
    uint16_t bus;

    for (bus = 0; bus < 256; ++bus) {
        uint8_t dev;
        for (dev = 0; dev < 32; ++dev) {
            uint8_t fn;
            for (fn = 0; fn < 8; ++fn) {
                uint8_t bari;
                if (pci_read_config16((uint8_t)bus, dev, fn, 0) == 0xFFFF) {
                    if (fn == 0) {
                        break;
                    }
                    continue;
                }
                for (bari = 0; bari < 6; ++bari) {
                    uint32_t bar = pci_read_config32((uint8_t)bus, dev, fn, 0x10 + bari * 4u);
                    uint64_t base;
                    if (!pci_bar_is_memory(bar)) {
                        continue;
                    }
                    base = pci_bar_address(bar);
                    if (pci_bar_is_64bit(bar)) {
                        /* The high dword is the next BAR slot. */
                        base |= (uint64_t)pci_read_config32((uint8_t)bus, dev, fn,
                                                            0x10 + (bari + 1u) * 4u)
                                << 32;
                        ++bari;
                    }
                    if (base >= PCWIN_BASE && base < limit) {
                        limit = base;
                    }
                }
            }
        }
    }

    limit &= ~(uint64_t)(PC_PAGE_SIZE - 1u);

    return (uint32_t)(limit - PCWIN_BASE);
}

/**
 * The window's page tables must exist before the first fork copies the
 * kernel's entries: on i686 `mmu_init` made every table of the kernel's
 * gigabyte and `premap_kernel_window` finds them, and on x86-64 it makes
 * them. No frame is taken: a slot gets its frame when it is first filled
 * and gives it back under memory pressure, so the cache is a target the
 * machine's free memory sets and not a reservation. The capacity is half of
 * what the caches together may hold under pressure, and never more slots
 * than there are.
 */
void init_pagecache(void)
{
    uint32_t i;

    pc_window_span = pc_probe_window_size();

    if (premap_kernel_window(PCWIN_BASE, pc_window_span) != 0) {
        k_panic("pagecache: window pre-map failed");
    }

    for (i = 0; i < PC_SLOTS; ++i) {
        pc_slots[i].phys = 0;
        pc_slots[i].page = 0;
        pc_slots[i].valid = 0;
        pc_slots[i].dirty = 0;
        pc_slots[i].accessed = 0;
        pc_slots[i].pinned = 0;
    }
    pc_capacity =
        mem_cache_target() / 2u > PC_SLOTS ? PC_SLOTS : (uint32_t)(mem_cache_target() / 2u);
    if (pc_capacity < 1u) {
        pc_capacity = 1u;
    }
    pc_resident_frames = 0;
    pc_clock = 0;
    pc_reads = pc_evicts = pc_writebacks = 0;
    pc_hash_rebuild();
}

#endif
