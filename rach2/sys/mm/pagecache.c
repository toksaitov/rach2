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

/** Nothing to exclude: a host test has one thread. */
static uint32_t pc_enter(int *bkl)
{
    *bkl = 0;
    return 0;
}

/** Undo nothing. */
static void pc_leave(uint32_t flags, int bkl)
{
    (void)flags;
    (void)bkl;
}

#ifndef PAGECACHE_HOST_WAIT
/** Wake nothing: a host test without PAGECACHE_HOST_WAIT has no queue for
 * `pc_wait_release` to sleep on. */
static void pc_platform_released(void)
{}
#endif

/* Linux's defaults for the background threshold and the dirty limit, which
 * the kernel keeps in the buffer cache (bcache.c). */
static int dirty_background_ratio = 10;
static unsigned long dirty_background_bytes;
static int vm_dirty_ratio = 20;
static unsigned long vm_dirty_bytes;

#else

#include "paging.h"
#include "pressure.h"
#include "../fs/bcache.h"
#include "../klog.h"
#include "../lib/kirq.h"
#include "../panic.h"
#include "../pci.h"
#include "../proc/smp.h"

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

/** Exclude every other caller as a window fault does: interrupts off, then
 * the big lock unless the thread holds it, which `*bkl` says. The interrupt
 * flags before, for `pc_leave`. */
static uint32_t pc_enter(int *bkl)
{
    uint32_t flags = kirq_save();

    *bkl = !bkl_held();
    if (*bkl) {
        bkl_enter();
    }
    return flags;
}

/** Give back what `pc_enter` took. */
static void pc_leave(uint32_t flags, int bkl)
{
    if (bkl) {
        bkl_exit();
    }
    kirq_restore(flags);
}

#endif

/* The kernel sleeps on the release of a pin, and so does a host test built
 * with PAGECACHE_HOST_WAIT over proc/wait_queue.c and a model scheduler. */
#if !defined(PAGECACHE_HOSTTEST) || defined(PAGECACHE_HOST_WAIT)
#include "../proc/wait_queue.h"

/* The syncs that sleep until a pin they passed is let go (`pc_wait_release`).
 * A zeroed queue needs no init. */
static wait_queue_t pc_release_wq;

/** Wake the syncs that wait for a pin to go (`pc_release_wq`). */
static void pc_platform_released(void)
{
    wq_wake_all(&pc_release_wq);
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
    uint8_t waited;   /* a sync passed it pinned and waits for its release */
    uint8_t held;     /* a store (`pc_copy_in`) brought it in and copies next */
    /* The pass whose write of the page failed, 0 for none: that pass's
     * other runs leave it out, and a sync's pass tries it once. */
    uint32_t failed_pass;
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
static void (*pc_failed)(int rc); /* told of each failed write, or NULL */
/* The last failed write's answer while `pc_failed` is NULL, 0 for none,
 * given to the hook when it is installed: the boot writes the root before
 * the file system that records it exists (`pc_on_failure`). */
static int pc_unclaimed;

static uint32_t pc_reads;
static uint32_t pc_evicts;
static uint32_t pc_writebacks;  /* pages that writebacks which succeeded wrote */
static uint32_t pc_dirty_pages; /* resident pages marked dirty */
/* Who wakes the flusher when a page goes dirty at or past the background
 * threshold (`pc_set_flusher`), or NULL. */
static void (*pc_kick)(void);
static uint32_t pc_pass; /* the last pass given out (`pc_new_pass`) */

/* The pages of a run of more than one, copied one after another for the
 * backing's one write (`pc_writeback`), and whether a writeback has them:
 * a write that sleeps keeps them while another caller writes back, and that
 * one writes its page alone. In the kernel's image, so its bytes are
 * physically contiguous, as a device that is given their addresses needs. */
static uint8_t pc_run_buf[PC_RUN_PAGES * PC_PAGE_SIZE] __attribute__((aligned(4096)));
static int pc_run_busy;
static uint32_t pc_resident_frames; /* slots holding a frame */
/* The releases of pins a sync passed (`waited`), which `pc_sync_wait` waits
 * on; it wraps. */
static volatile uint32_t pc_waited_releases;

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

/** `bytes` in pages, rounded up, when it is not 0, else `ratio` per cent of
 * the live capacity; at most the capacity and at least one page. The
 * rounding divides first, since the sum would wrap a 32-bit long. */
static uint32_t pc_share(unsigned long bytes, int ratio)
{
    unsigned long want;

    if (bytes) {
        want = bytes / PC_PAGE_SIZE + (bytes % PC_PAGE_SIZE != 0);
    } else {
        want = (unsigned long)(((uint64_t)pc_capacity * (uint32_t)ratio) / 100u);
    }
    if (want > pc_capacity) {
        want = pc_capacity;
    }
    return want ? (uint32_t)want : 1u;
}

/** The dirty limit, past which a writer writes back itself
 * (`pc_balance_dirty`): `vm_dirty_bytes` or `vm_dirty_ratio`, as
 * `pc_share` takes them. */
static uint32_t pc_limit(void)
{
    return pc_share(vm_dirty_bytes, vm_dirty_ratio);
}

/**
 * The dirty pages past which the flusher writes back:
 * `dirty_background_bytes` or `dirty_background_ratio`, as `pc_share` takes
 * them, and half the dirty limit when that is not above it, as in Linux's
 * `domain_dirty_limits`.
 */
static uint32_t pc_background(void)
{
    uint32_t bg = pc_share(dirty_background_bytes, dirty_background_ratio);
    uint32_t limit = pc_limit();

    if (bg >= limit) {
        bg = limit / 2u ? limit / 2u : 1u;
    }
    return bg;
}

/** A pass's number for the marks of its failed writes, never 0. When the
 * count wraps, every mark is cleared, so no old mark matches a new pass. */
static uint32_t pc_new_pass(void)
{
    uint32_t i;

    if (pc_pass == 0xFFFFFFFFu) {
        for (i = 0; i < PC_SLOTS; ++i) {
            pc_slots[i].failed_pass = 0;
        }
        pc_pass = 0;
    }
    return ++pc_pass;
}

/** Mark the page of `e` dirty and count it; a page that goes dirty at or
 * past the background threshold wakes the flusher. Interrupts may be off. */
static void pc_mark_dirty(struct pc_entry *e)
{
    if (e->dirty) {
        return;
    }
    e->dirty = 1;
    ++pc_dirty_pages;
    if (pc_kick && pc_dirty_pages >= pc_background()) {
        pc_kick();
    }
}

/** Clear the dirty mark of `e`, and its count. */
static void pc_mark_clean(struct pc_entry *e)
{
    if (e->dirty) {
        e->dirty = 0;
        --pc_dirty_pages;
    }
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
 * Let go of the pin of `e`. When a sync passed the slot while it was pinned,
 * the release is counted and the syncs that wait for it are woken
 * (`pc_wait_release`); other releases wake nobody.
 */
static void pc_unpin(struct pc_entry *e)
{
    e->pinned = 0;
    if (e->waited) {
        e->waited = 0;
        ++pc_waited_releases;
        pc_platform_released();
    }
}

/**
 * Gather the run `pc_writeback` writes for the dirty page of `e`: the page
 * and the resident pages next to it on the backing, back and forth, while
 * each is dirty, not pinned and not one whose write failed in `pass`,
 * PC_RUN_PAGES at most. `run` gets them in the backing's order; returns how
 * many.
 */
static uint32_t pc_gather(struct pc_entry *e, struct pc_entry **run, uint32_t pass)
{
    struct pc_entry *before[PC_RUN_PAGES];
    uint32_t back = 0, n = 0, off;
    int32_t s;

    for (off = e->disk_off; off >= PC_PAGE_SIZE && back + 1u < PC_RUN_PAGES;) {
        off -= PC_PAGE_SIZE;
        s = pc_hash_find(off);
        if (s < 0 || !pc_slots[s].dirty || pc_slots[s].pinned || pc_slots[s].failed_pass == pass) {
            break;
        }
        before[back++] = &pc_slots[s];
    }
    while (back > 0) {
        run[n++] = before[--back];
    }
    run[n++] = e;
    for (off = e->disk_off + PC_PAGE_SIZE; off < pc_fs_size && n < PC_RUN_PAGES;
         off += PC_PAGE_SIZE) {
        s = pc_hash_find(off);
        if (s < 0 || !pc_slots[s].dirty || pc_slots[s].pinned || pc_slots[s].failed_pass == pass) {
            break;
        }
        run[n++] = &pc_slots[s];
    }
    return n;
}

/**
 * Write the resident page of `e` back if it is dirty, with the slot pinned
 * meanwhile: 0 when it is clean or written, -1 when the slot is pinned or
 * the write fails, which leaves the page dirty.
 *
 * The dirty pages beside it on the backing go in the same write, up to
 * PC_RUN_PAGES (`pc_gather`), as Linux's writeback puts a file's contiguous
 * dirty pages in one bio (mm/page-writeback.c, `write_cache_pages`): the
 * ATA disk flushes its cache after each write, a synchronous write of the
 * disk's image file under an emulator, and a copy of a 6 MB file whose
 * walks of the ring evicted dirty pages one at a time, in faults with
 * interrupts off, flushed the disk 2610 times. A run of more than one page
 * is copied into `pc_run_buf` while no other writeback has it, and a page
 * alone is written from its frame.
 *
 * The pins are tested before the dirty marks, since a write in flight
 * clears the mark for its length. A system call switches processes only
 * when it yields or sleeps, and a page fault runs with interrupts off, so
 * the pins are taken before another caller can enter. The backing's write
 * may sleep with the pins held.
 *
 * Each page is mapped read-only and its mark cleared before it is copied
 * and written, so a store meanwhile faults and marks the page dirty again,
 * and a successful write leaves that mark set. A write that fails leaves
 * every page of the run dirty and marked as failed in `pass`, the caller's
 * from `pc_new_pass`.
 */
static int pc_writeback(struct pc_entry *e, uint32_t pass)
{
    struct pc_entry *run[PC_RUN_PAGES];
    const void *from;
    uint32_t n = 1, i;
    int rc, copied = 0;

    if (e->pinned) {
        return -1;
    }
    if (!e->dirty) {
        return 0;
    }
    if (pc_run_busy) {
        run[0] = e;
    } else {
        n = pc_gather(e, run, pass);
    }
    for (i = 0; i < n; ++i) {
        run[i]->pinned = 1;
        pc_platform_map(PCWIN_BASE + run[i]->disk_off, run[i]->phys, 0);
        pc_mark_clean(run[i]);
    }
    from = e->page;
    if (n > 1) {
        pc_run_busy = 1;
        copied = 1;
        for (i = 0; i < n; ++i) {
            const uint8_t *src = (const uint8_t *)run[i]->page;
            uint8_t *dst = pc_run_buf + i * PC_PAGE_SIZE;
            uint32_t j;

            for (j = 0; j < PC_PAGE_SIZE; ++j) {
                dst[j] = src[j];
            }
        }
        from = pc_run_buf;
    }
    rc = pc_write(run[0]->disk_off, from, n);
    if (copied) {
        pc_run_busy = 0;
    }
    if (rc != 0) {
        klog(KLOG_ERR, "pagecache",
             "writeback failed at disk offset 0x%X, %u page(s); the pages stay dirty",
             run[0]->disk_off, n);
        for (i = 0; i < n; ++i) {
            if (!run[i]->dirty) {
                run[i]->dirty = 1;
                ++pc_dirty_pages;
            }
            run[i]->failed_pass = pass;
        }
        if (pc_failed) {
            pc_failed(rc);
        } else {
            pc_unclaimed = rc;
        }
    } else {
        pc_writebacks += n;
        for (i = 0; i < n; ++i) {
            run[i]->failed_pass = 0;
        }
    }
    for (i = 0; i < n; ++i) {
        pc_unpin(run[i]);
    }
    return rc != 0 ? -1 : 0;
}

/**
 * Write back the page of `slot` if it is dirty, as `pc_writeback` does in
 * `pass`, and then unmap it and empty the slot, which keeps its frame.
 * Returns 0 when the slot is emptied or was empty, and -1, with the page
 * kept, when the slot is pinned or held for a store (`pc_copy_in`), the
 * writeback fails, or a store dirtied the page again during the write.
 *
 * The slot is emptied before its hash entry is removed, since the removal
 * may rebuild the hash from the valid slots.
 */
static int pc_evict(uint32_t slot, uint32_t pass)
{
    struct pc_entry *e = &pc_slots[slot];

    if (!e->valid) {
        return 0;
    }
    if (e->held || pc_writeback(e, pass) != 0 || e->dirty) {
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
 *
 * The clock's first two sweeps pass dirty pages, the first clearing the
 * marks of use, so that a clean page not used since is taken before any
 * dirty one, and a dirty one only when the sweeps found none: a fault runs
 * with interrupts off, and a writeback there holds the processor for the
 * disk's write and flush, as Linux's direct reclaim leaves dirty file pages
 * to the flusher (mm/vmscan.c, `shrink_folio_list`). The flusher keeps the
 * dirty pages near the background threshold (`pc_write_background`), and a
 * writer keeps them under the dirty limit (`pc_balance_dirty`).
 */
static int32_t pc_get_slot(void)
{
    uint32_t i;
    uint32_t scanned, pass = 0;

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

    for (scanned = 0; scanned < pc_capacity * 4u; ++scanned) {
        uint32_t v = pc_clock;
        pc_clock = (pc_clock + 1u) % pc_capacity;
        if (pc_slots[v].pinned || !PC_HAS_STORAGE(&pc_slots[v])) {
            continue;
        }
        if (pc_slots[v].accessed) {
            pc_slots[v].accessed = 0;
            continue;
        }
        if (pc_slots[v].dirty && scanned < pc_capacity * 2u) {
            continue;
        }
        if (pc_slots[v].dirty && pass == 0) {
            pass = pc_new_pass();
        }
        if (pc_evict(v, pass) == 0) {
            return (int32_t)v;
        }
    }

    return -1;
}

/**
 * Bring the page at `disk_off`, page-aligned and inside the backed size, into
 * a slot: the slot's index, or -1 when no slot can be had or the read fails,
 * which leaves nothing resident. The page is mapped read-only for a read, or
 * writable and marked dirty for a `write`; a hit that is a `write` marks it
 * dirty and maps it writable, a fault's only work on a resident page, since
 * a resident page is mapped read-only until written. Interrupts are off and
 * the caller holds the big lock, so no other caller enters until it returns.
 */
static int32_t pc_bring(uint32_t disk_off, int write)
{
    uintptr_t page_va = PCWIN_BASE + disk_off;
    int32_t slot;
    struct pc_entry *e;

    slot = pc_hash_find(disk_off);
    if (slot >= 0) {
        e = &pc_slots[slot];
        e->accessed = 1;
        if (write) {
            pc_mark_dirty(e);
            pc_platform_map(page_va, e->phys, 1);
        }
        return slot;
    }

    slot = pc_get_slot();
    if (slot < 0) {
        return -1;
    }
    e = &pc_slots[slot];
    e->pinned = 1;
    e->disk_off = disk_off;
    if (pc_read(disk_off, e->page) != 0) {
        pc_unpin(e);
        return -1;
    }
    e->valid = 1;
    e->dirty = 0;
    e->accessed = 1;
    e->failed_pass = 0;
    if (write) {
        pc_mark_dirty(e);
    }
    pc_hash_insert(disk_off, (uint32_t)slot);
    pc_platform_map(page_va, e->phys, write ? 1 : 0);
    pc_unpin(e);
    ++pc_reads;

    return slot;
}

int pc_fault(uintptr_t fault_address, int write)
{
    uintptr_t page_va = fault_address & ~(uintptr_t)(PC_PAGE_SIZE - 1u);

    if (!pc_read || page_va < PCWIN_BASE) {
        return 0;
    }
    if (page_va - PCWIN_BASE >= pc_fs_size) {
        return 0;
    }
    return pc_bring((uint32_t)(page_va - PCWIN_BASE), write) >= 0;
}

/** Pages a store holds at once (`pc_copy_in`): a field that crosses an edge. */
#define PC_COPY_HOLD 2u

/**
 * Copy `size` bytes at byte `off` of the backing, in pieces, through the
 * slots that hold them (`pc_bring`): into `to` for a read, or out of `from`
 * into the slots' frames, marked dirty, for a write; the other is NULL. Each
 * piece is brought in and copied with interrupts off and the big lock held,
 * as a window fault is served. A read goes a page at a time, so that it
 * needs one slot, as a fault does. A write's piece is at most PC_COPY_HOLD
 * pages, every one brought in, held so that no slot search takes one for
 * the next, before it copies a byte, so that a page that cannot be read or
 * given a slot leaves the piece as it was. Returns 0,
 * or -1 with no backing, for a range past the backed size, or when a page
 * cannot be read or given a slot; the pieces before it are copied.
 */
static int pc_copy(uint32_t off, uint8_t *to, const uint8_t *from, uint32_t size)
{
    if (!pc_read || off > pc_fs_size || size > pc_fs_size - off) {
        return -1;
    }
    while (size != 0) {
        uint32_t first = off & ~(PC_PAGE_SIZE - 1u);
        uint32_t span = (from ? PC_COPY_HOLD : 1u) * PC_PAGE_SIZE - (off - first);
        uint32_t n = span < size ? span : size;
        uint32_t pages = (off - first + n + PC_PAGE_SIZE - 1u) / PC_PAGE_SIZE;
        int32_t slots[PC_COPY_HOLD];
        uint32_t i, k, at;
        int bkl, failed = 0;
        uint32_t flags = pc_enter(&bkl);

        for (k = 0; k < pages; ++k) {
            slots[k] = pc_bring(first + k * PC_PAGE_SIZE, 0);
            if (slots[k] < 0) {
                failed = 1;
                break;
            }
            pc_slots[slots[k]].held = 1;
        }
        for (i = 0; i < k; ++i) {
            pc_slots[slots[i]].held = 0;
        }
        if (failed) {
            pc_leave(flags, bkl);
            return -1;
        }
        at = off - first;
        for (k = 0; k < pages; ++k) {
            uint8_t *frame;
            uint32_t m = PC_PAGE_SIZE - at < n ? PC_PAGE_SIZE - at : n;

            if (from) {
                (void)pc_bring(first + k * PC_PAGE_SIZE, 1); /* a hit: dirty, writable */
            }
            frame = (uint8_t *)pc_slots[slots[k]].page + at;
            for (i = 0; i < m; ++i) {
                if (from) {
                    frame[i] = from[i];
                } else {
                    to[i] = frame[i];
                }
            }
            if (from) {
                from += m;
            } else {
                to += m;
            }
            off += m;
            size -= m;
            n -= m;
            at = 0;
        }
        pc_leave(flags, bkl);
    }
    return 0;
}

int pc_copy_out(uint32_t off, void *buf, uint32_t size)
{
    return pc_copy(off, (uint8_t *)buf, 0, size);
}

int pc_copy_in(uint32_t off, const void *buf, uint32_t size)
{
    return pc_copy(off, 0, (const uint8_t *)buf, size);
}

uint32_t pc_shrink(uint32_t want, int can_write, uint32_t *written)
{
    uint32_t got = 0, id = pc_new_pass();
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
            if (e->valid && pc_evict(i, id) != 0) {
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

uint32_t pc_releases(void)
{
    return pc_waited_releases;
}

#if !defined(PAGECACHE_HOSTTEST) || defined(PAGECACHE_HOST_WAIT)
/** The condition of `pc_wait_release`: a release has been counted since the
 * count `*arg` was read. */
static int pc_released_since(void *arg)
{
    return pc_waited_releases != *(const uint32_t *)arg;
}

/** A release is counted before its wake, and the waiter is on the queue
 * and blocked before it reads the count, so no wake is lost. */
void pc_wait_release(uint32_t seen)
{
    (void)wq_wait_event_nointr(&pc_release_wq, pc_released_since, &seen);
}
#endif

int pc_dirty_level(void)
{
    if (pc_dirty_pages >= pc_limit()) {
        return 2;
    }
    return pc_dirty_pages >= pc_background() ? 1 : 0;
}

uint32_t pc_dirty_count(void)
{
    return pc_dirty_pages;
}

void pc_set_flusher(void (*kick)(void))
{
    pc_kick = kick;
}

/**
 * Write runs back until the dirty pages are fewer than `target`: each dirty
 * page that is not pinned and whose run has not failed in this pass starts
 * a run (`pc_writeback`), which may sleep; a page that goes dirty behind
 * the walk waits for the next pass. Returns the pages written.
 */
static uint32_t pc_write_down(uint32_t target)
{
    uint32_t i, written = 0, before, pass = pc_new_pass();

    for (i = 0; i < PC_SLOTS && pc_dirty_pages >= target; ++i) {
        struct pc_entry *e = &pc_slots[i];

        if (!e->valid || !e->dirty || e->pinned || e->failed_pass == pass) {
            continue;
        }
        before = pc_writebacks;
        (void)pc_writeback(e, pass);
        written += pc_writebacks - before;
    }
    return written;
}

uint32_t pc_write_background(void)
{
    return pc_write_down(pc_background());
}

/** Linux's writer waits past the limit until the flusher brings the dirty
 * pages under the midpoint of the threshold and the limit
 * (`dirty_freerun_ceiling`); here the writer writes the runs itself, since
 * the flusher may not run while it holds the processor. */
uint32_t pc_balance_dirty(void)
{
    uint32_t limit = pc_limit();

    if (pc_dirty_pages < limit) {
        return 0;
    }
    return pc_write_down((pc_background() + limit) / 2u);
}

/**
 * Busy slots and failed writes are reported apart, so that a competing sync
 * can wait for the writes in flight without trying a failed write again for
 * ever. A failure does not stop the walk over the other slots. A busy slot
 * is marked `waited`, so that its release wakes the caller's wait.
 *
 * A pass tries each dirty page once: a page whose run failed in this pass
 * is passed when its own slot comes, since a run's pages lie in any slots,
 * and a mark another pass left, which may run meanwhile, is not this
 * pass's, so the page is written again and a failure is this pass's too.
 */
int pc_sync(void)
{
    uint32_t i, pass = pc_new_pass();
    int result = 0;

    for (i = 0; i < PC_SLOTS; ++i) {
        struct pc_entry *e = &pc_slots[i];
        if (!e->valid || e->failed_pass == pass) {
            continue;
        }
        if (e->pinned) {
            e->waited = 1;
            result |= PC_SYNC_BUSY;
        } else if (pc_writeback(e, pass) != 0) {
            result |= PC_SYNC_FAILED;
        }
    }

    return result;
}

void pc_on_failure(void (*failed)(int rc))
{
    pc_failed = failed;
    if (failed && pc_unclaimed != 0) {
        failed(pc_unclaimed);
        pc_unclaimed = 0;
    }
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
            pc_slots[i].failed_pass = 0;
        }
    }
    pc_hash_rebuild();
    pc_clock = 0;
    pc_dirty_pages = 0;

    pc_read = read;
    pc_write = write;
    pc_unclaimed = 0;
    if (fs_size > pc_window_span) {
        fs_size = pc_window_span;
    }
    pc_fs_size = (fs_size + (PC_PAGE_SIZE - 1u)) & ~(PC_PAGE_SIZE - 1u);
}

int pc_set_capacity(uint32_t slots)
{
    uint32_t i, pass = pc_new_pass();
    uint32_t stuck = 0;
    int rc = 0;

    if (slots < 1u) {
        slots = 1u;
    }
    if (slots > PC_SLOTS) {
        slots = PC_SLOTS;
    }
    for (i = slots; i < PC_SLOTS; ++i) {
        if (pc_slots[i].valid && pc_evict(i, pass) != 0) {
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
        pc_slots[i].waited = 0;
        pc_slots[i].held = 0;
        pc_slots[i].failed_pass = 0;
    }
    pc_capacity = PC_SLOTS;
    pc_clock = 0;
    pc_reads = pc_evicts = pc_writebacks = 0;
    pc_dirty_pages = 0;
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
 * Reads every function the drivers' searches find (`pci_nth`, bus 0 and
 * each bus behind a bridge), so a BAR behind a bridge counts too. A
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
    pci_device_t d;
    int i;

    for (i = 0; pci_nth(i, &d); ++i) {
        uint8_t bari;
        for (bari = 0; bari < 6; ++bari) {
            uint32_t bar = d.bar[bari];
            uint64_t base;
            if (!pci_bar_is_memory(bar)) {
                continue;
            }
            base = pci_bar_address(bar);
            if (pci_bar_is_64bit(bar)) {
                /* The high dword is the next BAR slot; the last has none. */
                base |= bari < 5 ? (uint64_t)d.bar[bari + 1] << 32 : 0;
                ++bari;
            }
            if (base >= PCWIN_BASE && base < limit) {
                limit = base;
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
        pc_slots[i].waited = 0;
        pc_slots[i].held = 0;
        pc_slots[i].failed_pass = 0;
    }
    pc_capacity =
        mem_cache_target() / 2u > PC_SLOTS ? PC_SLOTS : (uint32_t)(mem_cache_target() / 2u);
    if (pc_capacity < 1u) {
        pc_capacity = 1u;
    }
    pc_resident_frames = 0;
    pc_clock = 0;
    pc_reads = pc_evicts = pc_writebacks = 0;
    pc_dirty_pages = 0;
    pc_hash_rebuild();
}

#endif
