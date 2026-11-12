/*
 * paging.c: the frame allocator, the shared code of the page tables and the
 * page fault handler of paging.h.
 */

#include "../arch/arch.h"
#include "../../kernel.h"
#include "../cmdline.h"
#include "../fpu.h"
#include "../klog.h"
#include "paging.h"
#include "kheap.h"
#include "pagecache.h"
#include "frame.h"
#include "usercopy.h"
#include "../serial.h"
#include "../lib/ksignal.h"
#include "../lib/kerrno.h"
#include "../lib/kirq.h"
#include "../proc/process.h"
#include "../proc/ptrace.h"
#include "../proc/sched.h"
#include "../proc/smp.h"
#include "../lib/spinlock.h"
#include "mm.h"
#include "notify.h"
#include "pin.h"
#include "pressure.h"

/* SIGBUS's code for an address with no object behind it (Linux's). */
#define BUS_ADRERR 2

page_dir_t *_kernel_dir;

uint32_t *_frames;
size_t _frames_number;
/* One bit per frame: a private anonymous page, kept across fork and COW.
 * Reserved beside the reference table and changed under frame_lock. */
static uint8_t *_frame_anon;
/* The frames of the window, below HARD_MEMORY_LIMIT, which
 * `alloc_kernel_frame` gives out: frames whose physical address a device can
 * be given. */
static size_t _window_frames;
/* The frames the allocator manages, counted at boot (`frame_total_count`). */
static size_t _managed_frames;
static size_t _free_frames; /* words of the table that are 0, kept by every writer */

static void frame_take(uintptr_t frame);
static void frame_make_anon(uintptr_t phys);
static int frame_is_anon(uintptr_t phys);
/* Where the next scan of the window for a free frame starts: no frame of
 * the window from `_user_frames_start` up to it is free. A scan from the
 * bottom of a million-frame table for every page would cost the table's
 * length squared over a large mapping. */
static uintptr_t _next_free_frame;
/* The hint for the frames above the window, which are taken first: the
 * window's frames are the only ones a device can be given the address of,
 * so the last WINDOW_KEEP of them are left to `alloc_kernel_frame`, which
 * takes from the window's top for the block requests and the page cache. */
static uintptr_t _next_free_upper;
static size_t _free_window_frames; /* free frames below the window */
#define WINDOW_KEEP 64u
/* rach2.framehigh, a test switch: the scan for a frame starts at 4 GiB, so
 * the heap's pages and the buffers a device is given of them lie above it
 * on a machine that has memory there, and the frames between the window and
 * 4 GiB are taken once those above are gone. Set while they are left. */
static int _framehigh_low;
#define FRAME_HIGH_FIRST (0x100000000ULL / PAGE_SIZE)
/* The index of the free frames, as Linux's free lists: `_frame_group_free[g]`
 * counts the free frames of group g, the FRAME_GROUP frames from
 * g * FRAME_GROUP, and bit g of `_frame_group_bits` is set while that count
 * is not 0, so that a search passes 32 groups with no free frame in one word
 * and never reads the used frames one at a time. Reserved beside the
 * reference table and kept under frame_lock by `frame_take` and
 * `frame_freed` once `_frame_index_ready`; the boot's searches before then
 * read the table itself. */
#define FRAME_GROUP      64u
static uint32_t *_frame_group_bits;
static uint8_t *_frame_group_free;
static int _frame_index_ready;
/* rach2.framefail: the allocation that fails on purpose, counted down. */
static uint32_t _framefail_left;
static int _framefail_on = -1; /* -1 until the boot line is read */

/** Whether this allocation is the one armed to fail: one made in a system
 * call that brings the armed count to 0. */
static int framefail_now(void)
{
    if (_framefail_left == 0 || !current_proc || !current_proc->in_syscall) {
        return 0;
    }
    if (--_framefail_left != 0) {
        return 0;
    }
    klog(KLOG_WARN, "vm", "frame allocation failed on purpose (rach2.framefail)");
    return 1;
}

int paging_framefail_arm(uint32_t count)
{
    if (_framefail_on < 0) {
        _framefail_on = kcmdline_has("rach2.framefail");
    }
    if (!_framefail_on) {
        return -1;
    }
    _framefail_left = count;
    return 0;
}

#ifndef ARCH_DIRECT_MAP_ALL
/* The temporary mappings: one bit per slot of the region set while a caller
 * holds it, and one per slot given back but still mapped, which no caller is
 * given until the next sweep of its pool unmaps it. A pool's lock covers its
 * bits. */
static uint32_t _temp_slots[KVA_TEMP_SLOTS / 32];
static uint32_t _temp_stale[KVA_TEMP_SLOTS / 32];
/* The most pools: each has whole words of the two maps, so that no two
 * pools' locks cover one word. */
#define TEMP_POOLS_MAX (KVA_TEMP_SLOTS / 32u)
/*
 * The slots split into a pool for each processor, as Linux gives each its
 * own slots of the kmap_local fixmap (`kmap_local_calc_idx`,
 * mm/highmem.c:476-480), so that processors mapping at once take their own
 * locks and two never take one slot. A processor borrows from another's pool
 * only when its own has nothing free even after a sweep.
 */
struct temp_pool {
    struct spinlock lock; /* over `next` and the pool's bits of the maps */
    uint32_t first, end;  /* the slots [first, end), multiples of 32 */
    uint32_t next;        /* the slot the next search starts at */
};
static struct temp_pool _temp_pools[TEMP_POOLS_MAX] = {
    {SPINLOCK_INIT_NAMED("kva_temp_pool"), 0, KVA_TEMP_SLOTS, 0}};
/* The pools in use and the slots of each but the last, which also has the
 * rest; one pool of every slot until `paging_temp_split`, which sets them
 * once, before a second processor runs. */
static uint32_t _temp_pool_count = 1;
static uint32_t _temp_pool_size = KVA_TEMP_SLOTS;
#endif
/* The pages of the MMIO region, a bit each, set while a window or a
 * mapping of `map_mmio` has the page. `mmio_lock` covers them, the large
 * spans' records and the region's entries. */
#define MMIO_PAGES (KVA_MMIO_SIZE / PAGE_SIZE)
static uint32_t _mmio_used[MMIO_PAGES / 32u];
static struct spinlock mmio_lock = SPINLOCK_INIT_NAMED("mmio_lock");
/* The first physical address past what an entry can name: 4 GiB on i686,
 * 2^52 on x86-64. */
#define MMIO_PHYS_TOP ((uint64_t)PTE_ADDR(~(pte_t)0) + PAGE_SIZE)
#if KVA_MMIO_LARGE
/* The region's spans of a leaf table that a large page maps, a bit each,
 * and for each the directory entry it replaced (`mmu_kernel_large`). */
#define MMIO_SPANS (KVA_MMIO_SIZE / MMU_TABLE_SPAN)
static uint32_t _mmio_large[(MMIO_SPANS + 31u) / 32u];
static pte_t _mmio_tables[MMIO_SPANS];
#endif

/**
 * The first frame above the boot's reservations and the floor of every scan
 * of the window for a free frame. Set once at init time to
 * `mem_reserved_ptr / PAGE_SIZE`, so a frame freed below a break that a later
 * `mem_reserve` raised is still found.
 */
static uintptr_t _user_frames_start = 0;

/* Frames `alloc_kernel_frame` has taken from the top of the window, the page
 * cache's among them, for the record a failed user allocation writes. */
static uint32_t _kernel_frames_reserved;

static void _page_fault_handler(isr_params_t *params);

/** The words of `_frame_group_bits`, one bit for each group of the table. */
static size_t frame_index_words(void)
{
    size_t groups = (_frames_number + FRAME_GROUP - 1u) / FRAME_GROUP;

    return (groups + 31u) / 32u;
}

/** Where the index begins in the frame table's reservation: after the
 * reference words and the anonymous-frame bits, at a word's boundary. */
static size_t frame_index_offset(void)
{
    size_t end = _frames_number * sizeof(uint32_t) + (_frames_number + 7u) / 8u;

    return (end + sizeof(uint32_t) - 1u) & ~(sizeof(uint32_t) - 1u);
}

/** The index's bytes: its group bits and a count for each group. */
static size_t frame_index_bytes(void)
{
    return frame_index_words() * sizeof(uint32_t) +
           (_frames_number + FRAME_GROUP - 1u) / FRAME_GROUP;
}

/** Count the free frames of each group from the table, once the boot has
 * marked what it holds, and from then on keep the index with each change. */
static void frame_index_build(void)
{
    uintptr_t frame;

    k_memset(_frame_group_bits, 0, frame_index_bytes());
    for (frame = 0; frame < _frames_number; frame++) {
        if (_frames[frame] == 0) {
            uintptr_t g = frame / FRAME_GROUP;

            _frame_group_free[g]++;
            _frame_group_bits[g / 32u] |= 1u << (g % 32u);
        }
    }
    _frame_index_ready = 1;
}

/** `frame`, which was free, is taken. `frame_lock` is held. */
static void frame_index_take(uintptr_t frame)
{
    uintptr_t g = frame / FRAME_GROUP;

    if (_frame_index_ready && --_frame_group_free[g] == 0) {
        _frame_group_bits[g / 32u] &= ~(1u << (g % 32u));
    }
}

/** `frame` is free again. `frame_lock` is held. */
static void frame_index_put(uintptr_t frame)
{
    uintptr_t g = frame / FRAME_GROUP;

    if (_frame_index_ready && _frame_group_free[g]++ == 0) {
        _frame_group_bits[g / 32u] |= 1u << (g % 32u);
    }
}

/** The lowest free frame in [from, to), or `to` when there is none. A group
 * the index has no free frame in is passed whole, and 32 such groups with
 * one word. `frame_lock` is held. */
static uintptr_t frame_next_free(uintptr_t from, uintptr_t to)
{
    uintptr_t g, end;

    if (!_frame_index_ready) {
        for (; from < to; from++) {
            if (_frames[from] == 0) {
                return from;
            }
        }
        return to;
    }
    while (from < to) {
        g = from / FRAME_GROUP;
        if (_frame_group_free[g] != 0) {
            end = (g + 1u) * FRAME_GROUP < to ? (g + 1u) * FRAME_GROUP : to;
            for (; from < end; from++) {
                if (_frames[from] == 0) {
                    return from;
                }
            }
            continue;
        }
        g++;
        while (g * FRAME_GROUP < to && !(_frame_group_bits[g / 32u] >> (g % 32u))) {
            g = (g / 32u + 1u) * 32u;
        }
        while (g * FRAME_GROUP < to && _frame_group_free[g] == 0) {
            g++;
        }
        from = g * FRAME_GROUP;
    }
    return to;
}

/** The highest free frame in [floor, from), or `floor` - 1 when there is
 * none, passing groups as `frame_next_free` does. `floor` is above 0.
 * `frame_lock` is held. */
static uintptr_t frame_prev_free(uintptr_t floor, uintptr_t from)
{
    uintptr_t g, start;

    if (!_frame_index_ready) {
        while (from-- > floor) {
            if (_frames[from] == 0) {
                return from;
            }
        }
        return floor - 1u;
    }
    while (from > floor) {
        g = (from - 1u) / FRAME_GROUP;
        if (_frame_group_free[g] != 0) {
            start = g * FRAME_GROUP > floor ? g * FRAME_GROUP : floor;
            while (from-- > start) {
                if (_frames[from] == 0) {
                    return from;
                }
            }
            from = start;
            continue;
        }
        while (g > 0 && g * FRAME_GROUP > floor &&
               !(_frame_group_bits[(g - 1u) / 32u] << (31u - (g - 1u) % 32u))) {
            g = (g - 1u) / 32u * 32u;
        }
        while (g > 0 && g * FRAME_GROUP > floor && _frame_group_free[g - 1u] == 0) {
            g--;
        }
        from = g * FRAME_GROUP > floor ? g * FRAME_GROUP : floor;
    }
    return floor - 1u;
}

/**
 * The architecture places the frame table (`mmu_frame_table`): below the
 * window with the other boot tables on i686, at the top of memory on
 * x86-64. A map with nothing usable gets the window, so a machine whose
 * firmware gives no map still boots. A table carved at the boot allocator's
 * break, as on i686, is placed before the map is consulted, so a table that
 * lies over memory the map does not offer would overwrite firmware's
 * memory, and the machine is refused by name in the memory report and stops
 * rather than run on it: on a machine too small for the kernel, its modules
 * and the table, this is where the table is placed. A frame the map does not
 * offer is marked `FRAME_ABSENT`.
 *
 * `mmu_init` marks the frames it carves, and every frame below
 * `mem_reserved_ptr` and of the table is marked used once more, for the
 * tables carved at frames the architecture had already passed and for those
 * carved after it. The map has the same ranges reserved, and what the
 * allocator manages and has free is counted from the table and not from the
 * map's arithmetic, so the boot's report compares the two. frame.h gives
 * HARD_MEMORY_LIMIT and PAGE_SIZE again, so that frame.c compiles on the
 * host, and the boot stops if the two copies differ.
 */
void init_paging(struct frame_map *map)
{
    uintptr_t frame, first, last;
    size_t table_bytes;
    uint32_t i;

    if (FRAME_DIRECT_MAP_LIMIT != HARD_MEMORY_LIMIT || FRAME_PAGE_SIZE != PAGE_SIZE) {
        k_panic("frame.h constants drifted from the architecture's");
    }

    _frames_number = map->top_frame ? (size_t)map->top_frame : (HARD_MEMORY_LIMIT / PAGE_SIZE);
    _window_frames = _frames_number < HARD_MEMORY_LIMIT / PAGE_SIZE ? _frames_number
                                                                    : HARD_MEMORY_LIMIT / PAGE_SIZE;
    table_bytes = frame_index_offset() + frame_index_bytes();
    _frames = mmu_frame_table(map, table_bytes);
    if (_frames == NULL) {
        k_panic("init_paging: no reserved memory for the frame table");
    }
    first = virt_to_phys(_frames) / PAGE_SIZE;
    last = (virt_to_phys(_frames) + table_bytes - 1) / PAGE_SIZE;
    if (map->top_frame) {
        for (frame = first; frame <= last; frame++) {
            if (!frame_map_usable(map, frame)) {
                klog(KLOG_ERR, "memory",
                     "refused: the frame table needs memory past what the map offers after the "
                     "kernel's reservations");
                k_panic("memory: too little for this kernel");
            }
        }
        if (frame_map_reserve(map, (uint64_t)first * PAGE_SIZE, (uint64_t)(last + 1) * PAGE_SIZE) !=
            0) {
            k_panic("init_paging: the frame table splits more ranges than the map keeps");
        }
    }
    _frame_anon = (uint8_t *)(_frames + _frames_number);
    k_memset(_frame_anon, 0, (_frames_number + 7u) / 8u);
    _frame_group_bits = _frames + frame_index_offset() / sizeof(uint32_t);
    _frame_group_free = (uint8_t *)(_frame_group_bits + frame_index_words());
    if (map->top_frame == 0) {
        k_memset(_frames, 0, table_bytes);
    } else {
        for (frame = 0; frame < _frames_number; frame++) {
            _frames[frame] = FRAME_ABSENT;
        }
        for (i = 0; i < map->count; i++) {
            uintptr_t lo = (uintptr_t)((map->usable[i].start + PAGE_SIZE - 1) / PAGE_SIZE);
            uintptr_t hi = (uintptr_t)(map->usable[i].end / PAGE_SIZE);

            for (frame = lo; frame < hi; frame++) {
                _frames[frame] = 0;
            }
        }
    }

    mmu_init();

    {
        uintptr_t final_frame = mem_reserved_ptr / PAGE_SIZE;
        uintptr_t f;
        for (f = 0; f < final_frame && f < _frames_number; f++) {
            _frames[f] = 1;
        }
    }
    for (frame = first; frame <= last; frame++) {
        _frames[frame] = 1;
    }

    _user_frames_start = mem_reserved_ptr / PAGE_SIZE;
    _next_free_frame = _user_frames_start;
    _next_free_upper = _window_frames;
    if (_frames_number > FRAME_HIGH_FIRST && kcmdline_has("rach2.framehigh")) {
        _next_free_upper = (uintptr_t)FRAME_HIGH_FIRST;
        _framehigh_low = 1;
        klog(KLOG_INFO, "memory", "rach2.framehigh: frames are taken from 4 GiB up first");
    }
    if (map->top_frame) {
        frame_map_reserve(map, 0, mem_reserved_ptr);
    }
    _managed_frames = 0;
    for (frame = _user_frames_start; frame < _frames_number; frame++) {
        if (_frames[frame] == 0) {
            _managed_frames++;
        }
    }
    _free_frames = 0;
    _free_window_frames = 0;
    for (frame = 0; frame < _frames_number; frame++) {
        if (_frames[frame] == 0) {
            _free_frames++;
            if (frame < _window_frames) {
                _free_window_frames++;
            }
        }
    }
    frame_index_build();

    register_isr_handler(PAGE_FAULT_VECTOR, &_page_fault_handler, "page fault");
    exception_unlock(PAGE_FAULT_VECTOR);
    mmu_start();

    mmu_heap_ready();
    kheap_init(kheap_size_for((uint64_t)_managed_frames * PAGE_SIZE, KVA_HEAP_SIZE));
}

void frame_take_boot(uintptr_t frame)
{
    if (_frames != NULL && frame < _frames_number) {
        frame_take(frame);
    }
}

/* Whether the entry `e` maps a page a read brought in zeroed that nothing
 * has stored into since (PTE_ZEROFILL): Linux's zero page. */
#define PTE_IS_ZERO(e) (((e) & (pte_t)(PTE_ZEROFILL | PTE_DIRTY)) == (pte_t)PTE_ZEROFILL)

static int cow_fault(struct mm *mm, uintptr_t addr);
static int page_private(struct mm *mm, uintptr_t addr, int writable);
static uint32_t frame_maps(uintptr_t frame);

/** The entry of `dir`'s own table for the user address `addr`, or NULL where
 * there is no table. */
static pte_t *user_pte(page_dir_t *dir, uintptr_t addr)
{
    pte_t *table = mmu_user_table(dir, addr, 0);

    return table ? &table[MMU_INDEX(addr)] : NULL;
}

/** The kernel directory's entry for the kernel address `addr`, or NULL. */
static pte_t *kernel_pte(uintptr_t addr)
{
    pte_t *table = mmu_kernel_table(addr);

    return table ? &table[MMU_INDEX(addr)] : NULL;
}

/**
 * Whether the page at `addr` is present and writable in `dir`. A
 * copy-on-write page is not, so the resolver makes it private before it
 * returns 1 for a write there.
 */
static int pte_writable(page_dir_t *dir, uintptr_t addr)
{
    pte_t *e = user_pte(dir, addr);

    return e && (*e & (PTE_P | PTE_W)) == (PTE_P | PTE_W);
}

/**
 * Whether the page at `addr` is present in `dir`. The resolver backs only a
 * page that is not: a second frame for a page exec loaded or a pre-fault
 * backed would leak the first and lose the page's contents.
 */
static int pte_present(page_dir_t *dir, uintptr_t addr)
{
    pte_t *e = user_pte(dir, addr);

    return e && (*e & PTE_P) != 0;
}

/** Count a page fault the current thread took, a major one when `major`
 * says its page was read through a file system (`process.h`). */
static void fault_count(int major)
{
    if (current_proc) {
        if (major) {
            current_proc->maj_flt++;
        } else {
            current_proc->min_flt++;
        }
    }
}

/**
 * Install `phys` at the user page `page` of `mm` with the entry bits
 * `flags`, the second phase of a fault, whose frame was had, filled and
 * zeroed outside every lock: under the page-table lock the entry is read
 * again and set only while it is still empty, so that of two faults on one
 * page the first to get there maps it and the other finds it taken, as
 * Linux's do_anonymous_page checks pte_none under the lock
 * (mm/memory.c:4805-4810). 0 once installed; 1 when the entry is not empty,
 * after which the caller gives its frame back and resolves again; -1 with
 * no memory for a table. x86 caches no translation of an entry that is not
 * present, so nothing is flushed.
 */
static int install_new(struct mm *mm, uintptr_t page, uintptr_t phys, pte_t flags)
{
    uint32_t lock = mm_ptl_lock(mm);
    pte_t *table = mmu_user_table(mm->dir, page, 1);
    int rc = -1;

    if (table) {
        rc = table[MMU_INDEX(page)] != 0;
        if (rc == 0) {
            table[MMU_INDEX(page)] = PTE_ADDR((pte_t)phys) | (flags & (pte_t)PTE_FLAGS);
            mm_resident_add(mm->dir, 1);
        }
    }
    mm_ptl_unlock(mm, lock);
    return rc;
}

/**
 * Let go of `mm`'s regions' lock before an operation of the file `f`, a
 * fill, a share or a device object's page, which may sleep: a fill may wait
 * for a file's lock, which comes before the regions' lock, so the lock goes
 * across the operation, as Linux's fault lets it go across a read
 * (`lock_folio_maybe_drop_mmap`, mm/filemap.c:3094-3119). The big lock is
 * taken too unless the file's operations run without it (`unlocked`), as
 * an ext2 file's do, whose fault Linux makes under the folio's lock and the
 * mapping's invalidate lock alone (mm/filemap.c:3326-3370); a driver's or
 * another file system's still runs under it. `*bkl` says whether the big
 * lock was taken here. 1 when the lock was let go; 0 when the caller holds
 * it more than once or to write (`mmap_may_drop`), and the operation is not
 * made, since it could then wait for a file whose holder waits for the
 * regions' lock.
 */
static int callback_begin(struct mm *mm, const struct vm_file *f, int *bkl)
{
    if (!mmap_may_drop(mm)) {
        return 0;
    }
    mmap_unlock(mm);
    *bkl = !f->unlocked && !bkl_held();
    if (*bkl) {
        bkl_enter();
    }
    return 1;
}

/** End what `callback_begin` began: the big lock back if it took it, and
 * the regions' lock taken again, to read. */
static void callback_end(struct mm *mm, int bkl)
{
    if (bkl) {
        bkl_exit();
    }
    mmap_read_lock(mm);
}

/**
 * Whether the region of `mm` at `page` is the one a fault found before its
 * file's operation let the regions' lock go: one that maps the file `f`
 * with the protection `prot`, its page `pgoff` at `page`, and no unmap of
 * a device object's or a truncated file's pages since (`zaps` as `seq`
 * read it), which may have taken back the page the operation gave.
 */
static int region_same(struct mm *mm, uintptr_t page, const struct vm_file *f, uint32_t prot,
                       uint32_t pgoff, uint32_t seq)
{
    const vma_t *vma = vm_find(&mm->vm, page);

    return vma && vma->file == f && vma->prot == prot &&
           vma->pgoff + (page - vma->start) / PAGE_SIZE == pgoff && mm->zaps == seq;
}

/**
 * Back a page of a region that maps a file: a frame filled from the file and
 * mapped once it is full. A read maps it copy-on-write; a store marks it
 * anonymous and maps it writable. The mapping is private: a store changes
 * the frame, never the file. The fault counts as
 * major when the fill read the page through the file system.
 *
 * The fill may sleep in the disk and runs with the regions' lock let go, and
 * with the big lock held unless the file's reads run without it
 * (`callback_begin`). The file is held from before the fill, so a thread
 * that unmaps the region meanwhile cannot free it under the read, and the
 * region is looked up again after it: a region that
 * changed in any way, its protection included, leaves the frame unused, and
 * the resolution runs again against what is there now, so a page is never
 * mapped with rights its region no longer gives; so does a page another
 * fault mapped meanwhile (`install_new`). 1 with the page mapped, 0 when no
 * frame could be had, the read failed or the lock could not be let go, -1
 * to resolve again.
 */
static int file_fault(struct mm *mm, const vma_t *vma, uintptr_t addr, int write)
{
    struct vm_file *f = vma->file;
    uintptr_t page = addr & ~(PAGE_SIZE - 1);
    uint32_t pgoff = vma->pgoff + (uint32_t)((page - vma->start) / PAGE_SIZE);
    uint32_t prot = vma->prot;
    uint32_t seq = mm->zaps;
    uintptr_t phys = alloc_user_frame();
    int filled, bkl, rc;

    if (phys == 0) {
        return 0;
    }
    vm_file_get(f);
    if (!callback_begin(mm, f, &bkl)) {
        vm_file_put(f);
        free_frame(phys);
        return 0;
    }
    filled = f->fill(f, pgoff, phys);
    callback_end(mm, bkl);
    rc = filled >= 0 ? 1 : 0;
    if (!region_same(mm, page, f, prot, pgoff, seq)) {
        rc = -1;
    }
    vm_file_put(f);
    if (rc == 1 && write) {
        frame_make_anon(phys);
    }
    if (rc == 1) {
        rc = install_new(mm, page, phys,
                         PTE_P | PTE_U | (write ? PTE_W : PTE_COW) | paging_prot_bits(prot));
        if (rc == 0) {
            fault_count(filled > 0);
            return 1;
        }
        rc = rc > 0 ? -1 : 0;
    }
    free_frame(phys);
    return rc;
}

/* The pages a read fault of a file maps at most, its own among them:
 * Linux's fault_around_bytes, 64 KiB (mm/memory.c:5194-5195). */
#define FAULT_AROUND 16u

/**
 * Map the pages of the file `f` around `page`, which a read fault of its
 * region has just mapped with the entry bits `flags`, that the file has in
 * memory, with the same bits, as Linux's do_fault_around maps the pages a
 * file's map_pages finds in the page cache (mm/memory.c:5254-5284 and
 * 5300-5330): a program that reads its text and its libraries a page at a
 * time then takes one fault in sixteen pages, not one a page. The window is
 * FAULT_AROUND pages from the multiple of FAULT_AROUND pages at or below
 * `page`, or from the region's start when that is higher, cut at the
 * region's end and at the end of the page table, so that one table holds
 * it. The file is asked with the regions' lock let go, as for its share,
 * and its pages are mapped only while the region still maps the whole
 * window of the file as it did and no unmap of a truncated file's pages has
 * come since; a page another fault mapped meanwhile is left as it is.
 * Called, as `shared_fault` returns, with the regions' lock held to read.
 */
static void share_around(struct mm *mm, uintptr_t page, struct vm_file *f, pte_t flags)
{
    const vma_t *vma = vm_find(&mm->vm, page);
    uintptr_t phys[FAULT_AROUND];
    uintptr_t from, to, end;
    uint32_t prot, pgoff, seq, n, i, lock;
    pte_t *table;
    int bkl;

    if (!vma || vma->file != f || !f->around) {
        return;
    }
    from = page & ~(FAULT_AROUND * PAGE_SIZE - 1);
    if (from < vma->start) {
        from = vma->start;
    }
    end = (page & ~(MMU_TABLE_SPAN - 1)) + MMU_TABLE_SPAN;
    to = from + FAULT_AROUND * PAGE_SIZE;
    if (end != 0 && to > end) {
        to = end;
    }
    if (to > vma->end) {
        to = vma->end;
    }
    n = (uint32_t)((to - from) / PAGE_SIZE);
    if (n <= 1) {
        return;
    }
    prot = vma->prot;
    pgoff = vma->pgoff + (uint32_t)((from - vma->start) / PAGE_SIZE);
    seq = mm->zaps;
    vm_file_get(f);
    if (!callback_begin(mm, f, &bkl)) {
        vm_file_put(f);
        return;
    }
    i = f->around(f, pgoff, n, phys);
    callback_end(mm, bkl);
    vma = vm_find(&mm->vm, from);
    if (i != 0 && region_same(mm, from, f, prot, pgoff, seq) && vma->end >= to) {
        lock = mm_ptl_lock(mm);
        table = mmu_user_table(mm->dir, from, 0);
        for (i = 0; table && i < n; i++) {
            if (phys[i] && table[MMU_INDEX(from + i * PAGE_SIZE)] == 0) {
                table[MMU_INDEX(from + i * PAGE_SIZE)] =
                    PTE_ADDR((pte_t)phys[i]) | (flags & (pte_t)PTE_FLAGS);
                mm_resident_add(mm->dir, 1);
                phys[i] = 0;
            }
        }
        mm_ptl_unlock(mm, lock);
    }
    for (i = 0; i < n; i++) {
        if (phys[i]) {
            free_frame(phys[i]);
        }
    }
    vm_file_put(f);
}

/**
 * Borrow a resident file or shared-memory page. A shared region maps it
 * writable when allowed; a private region maps it copy-on-write and copies
 * immediately when resolving a store. ENXIO raises SIGBUS past EOF.
 *
 * The share of a disk file's pages reads a page from the file the first
 * time, which may sleep, so it runs as `file_fault`'s fill does, with the
 * file held across it and the region looked up again after: a region that
 * changed, or a page backed meanwhile, has the resolution run again. A read
 * then maps the file's pages in memory around the page (`share_around`). 1
 * with the page mapped, 0 when no frame could be had or the lock could not
 * be let go, -1 to resolve again.
 */
static int shared_fault(struct mm *mm, const vma_t *vma, uintptr_t addr, int write)
{
    struct vm_file *f = vma->file;
    int private = vma->type != VM_SHARED;
    uintptr_t page = addr & ~(PAGE_SIZE - 1);
    uint32_t pgoff = vma->pgoff + (uint32_t)((page - vma->start) / PAGE_SIZE);
    uint32_t prot = vma->prot;
    uint32_t seq = mm->zaps;
    uintptr_t phys = 0;
    pte_t flags;
    int got, bkl, rc = 1;

    vm_file_get(f);
    if (!callback_begin(mm, f, &bkl)) {
        vm_file_put(f);
        return 0;
    }
    got = f->share(f, pgoff, &phys);
    callback_end(mm, bkl);
    if (!region_same(mm, page, f, prot, pgoff, seq)) {
        rc = -1;
    }
    vm_file_put(f);
    if (rc == 1 && got == -ENXIO && current_proc) {
        current_proc->bus_fault = 1;
    }
    if (got < 0) {
        return rc < 0 ? -1 : 0;
    }
    if (rc == 1) {
        flags = PTE_P | PTE_U | (private ? PTE_COW : ((prot & VM_PROT_WRITE) ? PTE_W : 0)) |
                paging_prot_bits(prot);
        rc = install_new(mm, page, phys, flags);
        if (rc == 0) {
            fault_count(got > 0);
            if (write) {
                return private ? cow_fault(mm, page) : 1;
            }
            share_around(mm, page, f, flags);
            return 1;
        }
        rc = rc > 0 ? -1 : 0;
    }
    free_frame(phys);
    return rc;
}

/** The cache bits of a page table entry for a device region of protection
 * `prot`: write-combining for VM_PROT_WC (`arch_pte_wc`), PTE_NOCACHE for
 * VM_PROT_UC, and none for a cached region. */
static pte_t device_cache(uint32_t prot)
{
    if (prot & VM_PROT_WC) {
        return arch_pte_wc();
    }
    return (prot & VM_PROT_UC) ? PTE_NOCACHE : 0;
}

/* The pages of a device region whose frames one look of its object gives,
 * when a fault maps the whole region (`device_map_rest`), on the stack. */
#define DEVICE_BATCH 64u

/**
 * Map every page of the device region around `page` that is not yet
 * present, after a fault has mapped `page` itself, for an object whose
 * pages a mapping may take at once (`vm_device_whole`), as Linux's
 * remap_pfn_range does at mmap for virtio-gpu's blobs of host memory
 * (virtgpu_vram.c:32-67) and i915's fault does for a whole object
 * (i915_gem_mman.c:253-295): a program that touches a blob of 16,384
 * pages then takes one fault, not 16,384. The object is asked for a batch
 * of pages at a time with the regions' lock let go, as `device_fault` asks
 * for one, and the batch is mapped only while the region and the count of
 * zaps are as they were, so that no page the object took back meanwhile is
 * mapped. The first page the object has not got, or a table that cannot be
 * had, ends the walk and leaves the rest to their faults. A region of the
 * one page the fault mapped has nothing left, and the walk does not let the
 * locks go for it. The caller holds the regions' lock to read, as on entry
 * to `device_fault`.
 */
static void device_map_rest(struct mm *mm, uintptr_t page, struct vm_file *f, uint32_t prot)
{
    const vma_t *vma = vm_find(&mm->vm, page);
    pte_t flags = PTE_P | PTE_U | ((prot & VM_PROT_WRITE) ? PTE_W : 0) | paging_prot_bits(prot) |
                  device_cache(prot);
    uintptr_t start, end, at, stop, phys[DEVICE_BATCH];
    uint32_t base, seq, n, i;
    int bkl, rc = 0;

    if (!vma || vma->file != f || vma->end - vma->start <= PAGE_SIZE) {
        return;
    }
    start = vma->start;
    end = vma->end;
    base = vma->pgoff;
    for (at = start; rc >= 0 && at < end; at = stop) {
        stop = end - at > DEVICE_BATCH * PAGE_SIZE ? at + DEVICE_BATCH * PAGE_SIZE : end;
        seq = mm->zaps;
        vm_file_get(f);
        if (!callback_begin(mm, f, &bkl)) {
            vm_file_put(f);
            break;
        }
        for (n = 0; at + n * PAGE_SIZE < stop; n++) {
            if (f->device(f, base + (uint32_t)((at - start) / PAGE_SIZE) + n, &phys[n]) != 0) {
                rc = -1;
                break;
            }
        }
        callback_end(mm, bkl);
        vma = region_same(mm, at, f, prot, base + (uint32_t)((at - start) / PAGE_SIZE), seq)
                  ? vm_find(&mm->vm, at)
                  : NULL;
        vm_file_put(f);
        if (!vma) {
            break;
        }
        if ((vma->end - at) / PAGE_SIZE < n) {
            n = (uint32_t)((vma->end - at) / PAGE_SIZE);
            rc = -1;
        }
        for (i = 0; i < n; i++) {
            int got = install_new(mm, at + i * PAGE_SIZE, phys[i], flags);

            if (got < 0) {
                rc = -1;
                break;
            }
            if (got == 0) {
                frame_ref(phys[i]);
            }
        }
    }
}

/**
 * Back a page of a device region (`vm_device_map`): map the physical
 * address the object gives for it, writable when the region is, with the
 * region's cache type, and take a reference for the mapping, which counts
 * only a frame the allocator counts and which unmapping drops. The object's
 * `page` may sleep, so it runs as `file_fault`'s fill does, and the region
 * is looked up again after, with the count of unmaps of device pages, so
 * that a page the object took back meanwhile is not mapped. 1 with the page
 * mapped; 0 when the object has no page there, with `bus_fault` set for the
 * fault handler's SIGBUS, when it cannot give the page yet, with
 * `fault_retry` set for the handler's return to user mode, when no page
 * table could be had, or when the lock could not be let go; -1 to resolve
 * again.
 */
static int device_fault(struct mm *mm, const vma_t *vma, uintptr_t addr)
{
    struct vm_file *f = vma->file;
    uintptr_t page = addr & ~(PAGE_SIZE - 1);
    uint32_t pgoff = vma->pgoff + (uint32_t)((page - vma->start) / PAGE_SIZE);
    uint32_t prot = vma->prot;
    uint32_t seq = mm->zaps;
    uintptr_t phys = 0;
    int got, bkl, rc;

    vm_file_get(f);
    if (!callback_begin(mm, f, &bkl)) {
        vm_file_put(f);
        return 0;
    }
    got = f->device(f, pgoff, &phys);
    callback_end(mm, bkl);
    if (!region_same(mm, page, f, prot, pgoff, seq)) {
        vm_file_put(f);
        return -1;
    }
    vm_file_put(f);
    if (got != 0) {
        if (current_proc) {
            if (got > 0) {
                current_proc->fault_retry = 1;
            } else {
                current_proc->bus_fault = 1;
            }
        }
        return 0;
    }
    rc = install_new(mm, page, phys,
                     PTE_P | PTE_U | ((prot & VM_PROT_WRITE) ? PTE_W : 0) | paging_prot_bits(prot) |
                         device_cache(prot));
    if (rc != 0) {
        return rc > 0 ? -1 : 0;
    }
    frame_ref(phys);
    fault_count(0);
    if (vm_device_whole(f)) {
        device_map_rest(mm, page, f, prot);
    }
    return 1;
}

/** Whether the region `vma` allows the access: a write needs write, and a
 * read needs read, which mmap and the ELF loader give every region with
 * write or execute; a guard allows none. */
static int region_allows(const vma_t *vma, int write)
{
    return (vma->prot & (write ? VM_PROT_WRITE : VM_PROT_READ)) != 0 &&
           !(vma->prot & VM_PROT_GUARD);
}

/**
 * Whether the running thread's PKRU lets it make a data access, a `write`
 * or a read, to the pages of the region `vma` of the address space `mm`,
 * as Linux's arch_vma_access_permitted asks: the keys count for the
 * thread's own accesses, its faults and the copies and pins the kernel
 * makes for it, and not for another address space, which ptrace,
 * /proc/PID/mem and process_vm_readv reach through `paging_access` and
 * which is always allowed. Always 1 with the keys off.
 */
static int pkey_allows(const struct mm *mm, const vma_t *vma, int write)
{
    uint32_t key, pkru;

    if (!fpu_pkeys() || !current_proc || mm != current_proc->mm || !vma) {
        return 1;
    }
    key = VM_PROT_KEY(vma->prot);
    pkru = fpu_pkru();
    return !(pkru & PKRU_AD(key)) && !(write && (pkru & PKRU_WD(key)));
}

/**
 * Grow the stack down to `page` for a fault outside every region, as
 * `vm_grow_stack` does under RLIMIT_AS `as_limit`; a locked stack's growth
 * also counts against RLIMIT_MEMLOCK, as Linux's acct_stack_growth checks
 * it. 1 if grown, 0 if not. The caller holds `vm`'s lock to write, and the
 * big lock is taken across the change, as every change of the regions
 * holds it (mm.h).
 */
static int grow_stack(vm_space_t *vm, uintptr_t page, uintptr_t as_limit)
{
    const vma_t *above;
    int grown = 0;

    bkl_enter();
    above = vm_lower(vm, page);
    if (!(above && above->type == VM_STACK && (above->prot & VM_PROT_LOCKED) &&
          page < above->start &&
          !mem_lock_room(vm_locked(vm) / PAGE_SIZE, (above->start - page) / PAGE_SIZE))) {
        grown = vm_grow_stack(vm, page, as_limit);
    }
    bkl_exit();
    return grown;
}

/**
 * Grow the stack of `mm` down to `page` for a fault outside every region
 * (`grow_stack`). The growth changes the regions, so it needs the lock to
 * write: a caller that holds it to read once lets it go and takes it to
 * write for the growth, as Linux's lock_mm_and_find_vma takes it to write
 * to expand a stack (mm/memory.c:6197-6248), and takes it to read again
 * after. -1 once the stack grew, for the resolution to run again and back
 * the page in the stack's region; 0 when it did not grow, or when the
 * caller holds the lock to read more than once and cannot let it go.
 */
static int grow_fault(struct mm *mm, uintptr_t page, uintptr_t as_limit)
{
    int grown;

    if (mmap_held(mm, 1)) {
        return grow_stack(&mm->vm, page, as_limit) ? -1 : 0;
    }
    if (!mmap_may_drop(mm)) {
        return 0;
    }
    mmap_unlock(mm);
    mmap_write_lock(mm);
    grown = grow_stack(&mm->vm, page, as_limit);
    mmap_unlock(mm);
    mmap_read_lock(mm);
    return grown ? -1 : 0;
}

/** Let `mm`'s regions' lock go, which the caller holds once to read
 * (`mmap_may_drop`), have the caches give frames back under the big lock,
 * which they need (`mem_balance`), and take the regions' lock again. */
static void balance_unlocked(struct mm *mm)
{
    mmap_unlock(mm);
    bkl_enter();
    mem_balance();
    bkl_exit();
    mmap_read_lock(mm);
}

/**
 * Back the missing page `page` of `mm` in an anonymous region of protection
 * `prot` with a zeroed frame of its own, read-only and marked PTE_ZEROFILL
 * for a read, and count the fault. 1 when the page is mapped, -1 when
 * another fault mapped it first and this frame is given back, and 0 when no
 * frame could be had or zeroed.
 */
static int anon_fault(struct mm *mm, uintptr_t page, uint32_t prot, int write)
{
    uintptr_t phys = alloc_user_frame();
    int rc;

    if (phys == 0) {
        return 0;
    }
    frame_make_anon(phys);
    if (paging_zero_frame(phys) != 0) {
        free_frame(phys);
        return 0;
    }
    rc = install_new(mm, page, phys,
                     PTE_P | PTE_U | ((prot & VM_PROT_WRITE) ? PTE_W : 0) |
                         (write ? 0 : PTE_ZEROFILL) | paging_prot_bits(prot));
    if (rc != 0) {
        free_frame(phys);
        return rc > 0 ? -1 : 0;
    }
    fault_count(0);
    return 1;
}

/**
 * One try at `vm_resolve_in`: 1 when the page is present and allows the
 * access, 0 when the access is illegal or no frame could be had, and -1
 * when the regions or the entry changed while a lock was let go.
 *
 * A page that is there and allows the access needs nothing. Any other needs
 * a frame, or a copy-on-write copy, so the caches give frames back first
 * when memory is short (`mem_balance`), once for the resolution, as
 * `*balanced` records: reclaim may write a file back under the file's lock,
 * which comes before the regions' lock, so a caller that may let the lock
 * go balances with it let go and resolves again, and one that cannot does
 * not balance. Each page backed or copied counts as a fault of the current
 * thread (`fault_count`).
 */
static int vm_resolve_once(struct mm *mm, uintptr_t addr, int write, uintptr_t as_limit,
                           int *balanced)
{
    page_dir_t *dir = mm->dir;
    vm_space_t *vm = &mm->vm;
    vma_t *vma = vm_find(vm, addr);
    uintptr_t page = addr & ~(PAGE_SIZE - 1);
    int rc;

    if (vma && region_allows(vma, write) && pte_present(dir, addr) &&
        (!write || pte_writable(dir, addr))) {
        return 1;
    }
    if (!*balanced) {
        *balanced = 1;
        if (mem_short() && mmap_may_drop(mm)) {
            balance_unlocked(mm);
            return -1;
        }
    }
    if (!vma) {
        return grow_fault(mm, page, as_limit);
    }
    if (!region_allows(vma, write)) {
        return 0;
    }
    if (pte_present(dir, addr)) {
        if (!write || pte_writable(dir, addr)) {
            return 1;
        }
        rc = cow_fault(mm, addr);
        if (rc > 0) {
            fault_count(0);
        }
        return rc;
    }
    if (vma->prot & VM_PROT_IO) {
        return vma->file && vma->file->device ? device_fault(mm, vma, addr) : 0;
    }
    if (vma->file) {
        return vma->file->share ? shared_fault(mm, vma, addr, write)
                                : file_fault(mm, vma, addr, write);
    }
    return anon_fault(mm, page, vma->prot, write);
}

/**
 * Decide whether an access at `addr`, which need not be aligned, is legal in
 * the address space `mm` and, if so, make sure the page is backed. `write`
 * is 1 for a write, and `as_limit` is the RLIMIT_AS a grown stack is held
 * to. The caller holds `mm`'s regions' lock, to read or to write; a caller
 * that holds it to read once may find it let go and taken again on return,
 * so it keeps no region it found before.
 *
 * Outside every region the one legal access grows the stack down
 * (`vm_grow_stack`, under RLIMIT_AS). Inside one, the region's protection
 * must allow the access: a write to a read-only region and any access to a
 * PROT_NONE region are never backed. A present page is left as it is, except
 * that a write to one that is not writable goes to `cow_fault`: a page shared
 * by a fork is present, read-only and copy-on-write in a writable region, and
 * a pre-fault that answered "backed" would promise the kernel a page it
 * cannot write. A present page that is neither writable nor copy-on-write
 * then reports unresolved, since reporting it backed would fault on the same
 * instruction forever.
 *
 * A missing page is anonymous memory (heap, stack, an anonymous mapping or
 * a segment's pages past its file bytes), a page of a file, a segment's
 * or a mapping's, or a page of a device object, which the object gives
 * (`device_fault`) or, gone, answers with SIGBUS. An
 * anonymous one gets a zeroed frame, read-only in the PTE when its region
 * is, so a later write faults. One a
 * read brings in is marked PTE_ZEROFILL, and stands for Linux's base zero page
 * until a store sets its dirty bit or the kernel stores into its frame
 * (`page_private`, `cow_fault`, a pin). A file's
 * page is read into its frame, which may sleep, and when the regions
 * changed meanwhile the resolution runs again (`vm_resolve_once`), so
 * success always means a page that is present and allows the access.
 *
 * Returns 1 if the page is now mapped, 0 if the access is illegal or no
 * frame could be had or zeroed; the fault handler then signals the process,
 * and a pre-fault answers -EFAULT. `mm` is the current process's for a fault
 * and a pre-fault, and a stopped tracee's for ptrace.
 */
static int vm_resolve_in(struct mm *mm, uintptr_t addr, int write, uintptr_t as_limit)
{
    int rc, balanced = 0;

    do {
        rc = vm_resolve_once(mm, addr, write, as_limit, &balanced);
    } while (rc < 0);
    return rc;
}

/** RLIMIT_MEMLOCK is the caller's, as Linux reads the current task's; a
 * privileged caller has CAP_IPC_LOCK and no bound. */
int mem_lock_room(unsigned long have, unsigned long pages)
{
    unsigned long limit;

    if (!current_proc || proc_privileged(current_proc)) {
        return 1;
    }
    limit = current_proc->rlim[RLIM_MEMLOCK].cur;
    if (limit == RLIM_INFINITY) {
        return 1;
    }
    limit /= PAGE_SIZE;
    return have <= limit && pages <= limit - have;
}

/** `vm_resolve_in` for the current process's address space, for a data
 * access the thread's protection keys must allow (`pkey_allows`): a copy,
 * a pre-fault or a populate. */
static int vm_resolve(uintptr_t addr, int write)
{
    struct mm *mm = current_proc->mm;

    if (!pkey_allows(mm, vm_find(&mm->vm, addr), write)) {
        return 0;
    }
    return vm_resolve_in(mm, addr, write, current_proc->rlim[RLIM_AS].cur);
}

/** Whether every page of [start, end) is present in `mm` and allows the
 * access, found with nothing that sleeps. */
static int range_ready(struct mm *mm, uintptr_t start, uintptr_t end, int write)
{
    uintptr_t page;

    for (page = start & ~(PAGE_SIZE - 1); page < end; page += PAGE_SIZE) {
        const vma_t *vma = vm_find(&mm->vm, page);

        if (!vma || !region_allows(vma, write) || !pkey_allows(mm, vma, write) ||
            !pte_present(mm->dir, page) || (write && !pte_writable(mm->dir, page))) {
            return 0;
        }
    }
    return 1;
}

/**
 * A kernel address, below USER_ADDR_MIN or at USER_ADDR_MAX and above, is
 * the kernel's own memory, which a file system's caller passes for a read
 * into a page or a program's image, and is always present; a user pointer
 * there never gets past `user_range_ok`. A range that starts in user space
 * and ends past it is not resident. The regions are walked under their
 * lock, taken only when that needs no wait (`mmap_read_trylock`), since the
 * caller may hold a file's lock, which comes first, or run with interrupts
 * off; a lock it cannot have now answers 0, and the caller brings the range
 * in with its own locks let go. The caller holds the big lock, which keeps
 * a present page present until it sleeps: every change that takes a page
 * away or takes a right from it, an unmap, a protection change, a move or a
 * device's or a truncation's revocation, is made under it.
 */
int vm_resident(uintptr_t addr, uintptr_t len, int write)
{
    uintptr_t end = addr + len;
    struct mm *mm;
    int ready;

    if (len == 0 || (current_proc && proc_window_covers(current_proc, addr, len))) {
        return 1;
    }
    if (addr < USER_ADDR_MIN || addr >= USER_ADDR_MAX) {
        return 1;
    }
    if (end < addr || end > USER_ADDR_MAX || !current_proc || !current_proc->mm) {
        return 0;
    }
    bkl_assert_held();
    mm = current_proc->mm;
    if (!mmap_read_trylock(mm)) {
        return 0;
    }
    ready = range_ready(mm, addr, end, write);
    mmap_unlock(mm);
    return ready;
}

int vm_ready_held(uintptr_t addr, uintptr_t len, int write)
{
    uintptr_t end = addr + len;

    if (len == 0 || (current_proc && proc_window_covers(current_proc, addr, len))) {
        return 1;
    }
    if (addr < USER_ADDR_MIN || end < addr || end > USER_ADDR_MAX || !current_proc ||
        !current_proc->mm) {
        return 0;
    }
    return range_ready(current_proc->mm, addr, end, write);
}

/** `vm_prefault_range`'s resolution of [addr, end) of `mm`, the current
 * process's, whose regions' lock the caller holds to read. A range whose
 * pages are all present and allow the access, as a copy's almost always
 * are, is answered by one check, since resolving a present page changes
 * nothing; Linux's copy_user_generic touches the range with no check at
 * all and resolves only a page that faults (arch/x86/include/asm/
 * uaccess_64.h:119-131). */
static int prefault_locked(struct mm *mm, uintptr_t addr, uintptr_t end, int write)
{
    uintptr_t page;
    int ok = 1;

    if (range_ready(mm, addr, end, write)) {
        return 1;
    }
    do {
        for (page = addr & ~(PAGE_SIZE - 1); ok && page < end; page += PAGE_SIZE) {
            ok = vm_resolve(page, write);
        }
    } while (ok && !range_ready(mm, addr, end, write));
    return ok;
}

/**
 * The resolution of one page may sleep, reading a file or writing a cache
 * back, with the regions' lock let go, and another thread may meanwhile
 * unmap or protect a page already brought in, which the kernel would then
 * touch and fault on with no way to recover. So once every page is in, the
 * range is checked again with the lock held and nothing that sleeps, and
 * brought in again when a page went. A range that wraps is refused, since
 * the loop would end early and leave its tail unbacked. A kernel buffer the
 * thread opened to the checks (`user_window_open`) is present already. The
 * lock is given back on return; a caller that touches the range after holds
 * it across both (`vm_user_begin`), or holds the big lock, which keeps the
 * pages present until it sleeps (`vm_resident`).
 */
int vm_prefault_range(uintptr_t addr, uintptr_t len, int write)
{
    int rc = vm_user_begin(addr, len, write);

    if (rc > 0) {
        vm_user_end();
    }
    return rc >= 0;
}

int vm_user_begin(uintptr_t addr, uintptr_t len, int write)
{
    struct mm *mm;
    uintptr_t end;

    if (!current_proc || current_proc->mm == NULL) {
        return -1;
    }
    if (len == 0 || proc_window_covers(current_proc, addr, len)) {
        return 0;
    }
    end = addr + len;
    if (end < addr) {
        return -1;
    }
    mm = current_proc->mm;
    mmap_read_lock(mm);
    if (!prefault_locked(mm, addr, end, write)) {
        mmap_unlock(mm);
        return -1;
    }
    return 1;
}

void vm_user_end(void)
{
    mmap_unlock(current_proc->mm);
}

int vm_prefault_held(uintptr_t addr, uintptr_t len, int write)
{
    uintptr_t end = addr + len;

    if (!current_proc || current_proc->mm == NULL || end < addr) {
        return 0;
    }
    if (len == 0 || proc_window_covers(current_proc, addr, len)) {
        return 1;
    }
    return prefault_locked(current_proc->mm, addr, end, write);
}

/**
 * A region that is private and writable has its pages brought in for a write,
 * which gives each its own frame now, as Linux's populate breaks the sharing
 * of copy-on-write pages; any other for a read, so that a shared page is not
 * made dirty for nothing. A region with no access, a guard, device memory
 * or memfd_secret's is passed over, as Linux's populate passes over a VM_IO
 * region and one its get_user_pages refuses. Each
 * page brought in counts as a fault (`fault_count`), as Linux counts its
 * populate's. The regions' lock is taken to read, as Linux's __mm_populate
 * takes it after the change that asked for the pages has let it go
 * (mm/gup.c), and each page's region is found again, since the lock may
 * go around a page's fill.
 */
int vm_populate(uintptr_t start, uintptr_t end)
{
    struct mm *mm;
    uintptr_t page;
    int rc = 0;

    if (!current_proc || !current_proc->mm) {
        return -1;
    }
    mm = current_proc->mm;
    mmap_read_lock(mm);
    for (page = start & ~(PAGE_SIZE - 1); rc == 0 && page < end; page += PAGE_SIZE) {
        const vma_t *vma = vm_find(&mm->vm, page);
        int write;

        if (!vma || !(vma->prot & VM_PROT_READ) ||
            (vma->prot & (VM_PROT_IO | VM_PROT_GUARD | VM_PROT_SECRET))) {
            continue;
        }
        write = (vma->prot & VM_PROT_WRITE) && vma->type != VM_SHARED;
        if (!vm_resolve(page, write)) {
            rc = -1;
        }
    }
    mmap_unlock(mm);
    return rc;
}

/** Whether a read pin may take the page at `page` of the region `vma` of
 * `mm`: always in a region mapped with PROT_READ, and in one without it
 * (VM_PROT_NOREAD) only a page present and not the zero page, as Linux's
 * get_user_pages_fast takes such a page from its entry, and never with
 * `slow`, as the slow path and a remote pin refuse a region without
 * VM_READ. */
static int pin_read_allows(struct mm *mm, const vma_t *vma, uintptr_t page, int slow)
{
    pte_t *e;

    if (VM_READABLE(vma->prot)) {
        return 1;
    }
    if (slow) {
        return 0;
    }
    e = user_pte(mm->dir, page);
    return e && (*e & PTE_P) && !PTE_IS_ZERO(*e);
}

/** Whether the region allows the access of a pin with `flags`, PIN_WRITE
 * for a write, and with PIN_IO device memory; a region of memfd_secret and
 * the vDSO's data page never do, as Linux's get_user_pages refuses both,
 * and a read of a region mapped without PROT_READ needs its page there
 * already, and is refused with PIN_SLOW (`pin_read_allows`). */
static int pin_allows(struct mm *mm, uintptr_t page, uint32_t flags)
{
    const vma_t *vma = vm_find(&mm->vm, page);
    int write = (flags & PIN_WRITE) != 0;

    return vma && ((flags & PIN_IO) || !(vma->prot & VM_PROT_IO)) &&
           !(vma->prot & (VM_PROT_SECRET | VM_PROT_VVAR)) && region_allows(vma, write) &&
           (write || pin_read_allows(mm, vma, page, (flags & PIN_SLOW) != 0)) &&
           pkey_allows(mm, vma, write);
}

int paging_pin_allows(struct mm *mm, uintptr_t addr, int write)
{
    return pin_allows(mm, addr & ~(uintptr_t)(PAGE_SIZE - 1), write ? PIN_WRITE : 0u);
}

/** Resolve a pin's page, preserving the caller's pending SIGBUS detail. */
static int pin_resolve(struct mm *mm, uintptr_t page, int write, uintptr_t as_limit)
{
    int saved = current_proc ? current_proc->bus_fault : 0;
    int resolved, bus;

    if (current_proc) {
        current_proc->bus_fault = 0;
    }
    resolved = vm_resolve_in(mm, page, write, as_limit);
    bus = current_proc ? current_proc->bus_fault : 0;
    if (current_proc) {
        current_proc->bus_fault = saved;
    }
    return resolved ? 0 : (bus ? -EFAULT : -ENOMEM);
}

/**
 * Each page is brought in as a fault would, which may sleep; once every
 * page is in, the range is checked again with nothing that sleeps, as
 * `vm_prefault_range` does, and brought in again when a page went. From
 * then on nothing sleeps: a private page shared with another address space,
 * by a fork or as a file's page, is given a frame of its own with its
 * rights (`page_private`), so that a later write by the process reaches the
 * frame the device has, as Linux unshares a page it pins, and the frame is
 * pinned. With PIN_IO in `flags`, device pages outside the allocator are
 * kept without pins; `ram_pages` counts only the RAM frames that were pinned.
 * The caller holds `mm`'s regions' lock to read, once, so that a page's
 * fill may let it go; no fault can change a pinned page afterwards, since
 * each is writable or of its own frame.
 */
static int pin_range(struct mm *mm, uintptr_t start, uintptr_t end, uint32_t flags,
                     uintptr_t *frames, uint32_t *ram_pages)
{
    uintptr_t as_limit = current_proc ? current_proc->rlim[RLIM_AS].cur : RLIM_INFINITY;
    uintptr_t page;
    uint32_t i = 0, ram = 0;
    int write = (flags & PIN_WRITE) != 0;
    int rc = -ENOMEM;

    do {
        for (page = start; page < end; page += PAGE_SIZE) {
            if (!pin_allows(mm, page, flags)) {
                return -EFAULT;
            }
            rc = pin_resolve(mm, page, write, as_limit);
            if (rc != 0) {
                return pin_allows(mm, page, flags) ? rc : -EFAULT;
            }
        }
    } while (!range_ready(mm, start, end, write));
    rc = -ENOMEM;
    for (page = start; page < end; page += PAGE_SIZE, i++) {
        const vma_t *vma = vm_find(&mm->vm, page);
        pte_t *e = user_pte(mm->dir, page);

        if (!pin_allows(mm, page, flags)) {
            break;
        }
        if (vma->prot & VM_PROT_IO) {
            uintptr_t phys = PTE_ADDR(*e), frame = phys / PAGE_SIZE;

            if (frame < _frames_number && _frames[frame] < FRAME_REF_MAX) {
                rc = -EFAULT;
                break;
            }
            frames[i] = phys;
            continue;
        }
        paging_entry_clear(mm, page, PTE_ZEROFILL);
        if (vma->type != VM_SHARED &&
            ((*e & PTE_COW) || frame_maps(PTE_ADDR(*e) / PAGE_SIZE) > 1) &&
            page_private(mm, page, (vma->prot & VM_PROT_WRITE) != 0) != 0) {
            break;
        }
        frames[i] = PTE_ADDR(*user_pte(mm->dir, page));
        if (frame_pin(frames[i]) != 0) {
            break;
        }
        ram++;
    }
    if (page >= end) {
        *ram_pages = ram;
        return 0;
    }
    while (i--) {
        frame_unpin(frames[i]);
    }
    return rc;
}

int paging_pin_range(struct mm *mm, uintptr_t start, uintptr_t end, uint32_t flags,
                     uintptr_t *frames)
{
    uint32_t ram;

    return pin_range(mm, start, end, flags & ~PIN_IO, frames, &ram);
}

int paging_dma_range(struct mm *mm, uintptr_t start, uintptr_t end, uint32_t flags,
                     uintptr_t *frames, uint32_t *ram_pages)
{
    return pin_range(mm, start, end, PIN_IO | flags, frames, ram_pages);
}

/*
 * The hold of a change of a present user entry made by a holder of the
 * regions' lock to read, a fault or a pin, whom no other such holder is
 * kept from (`page_change_begin`).
 */
struct page_change {
    int bkl;        /* whether the change took the big lock */
    int counted;    /* whether it counts itself under way (`mmn_change_begin`) */
    uint32_t flags; /* the page-table lock's saved flags */
};

/**
 * Begin the second phase of a change of the present entry of `mm` at
 * `page`, by a holder of the regions' lock to read: the ranges registered
 * on the space are told of `event` first, unless it is 0, and then the
 * page-table lock is taken. When the space has a range the big lock is
 * held from the notice to the change (notify.h), so that a driver that
 * reads entries under it sees each page before the change or hears of it
 * after, as Linux's wp_page_copy calls mmu_notifier_invalidate_range_start
 * before it takes the page-table lock (mm/memory.c:3384-3390). A change
 * that gives the page a new frame (`event` set) counts itself under way
 * from before the notice to the end (`mmn_change_begin`), for a driver
 * that reads entries without the big lock. 0; or -1, with nothing held,
 * when a range was registered after the space was seen without one, and
 * the change is made again.
 */
static int page_change_begin(struct mm *mm, struct page_change *c, int event, uintptr_t page)
{
    int ranges = mm->notify != NULL;

    c->bkl = ranges && !bkl_held();
    c->counted = ranges && event;
    if (c->bkl) {
        bkl_enter();
    }
    if (c->counted) {
        mmn_change_begin(mm);
        mmn_notify(mm, event, page, page + PAGE_SIZE);
    }
    c->flags = mm_ptl_lock(mm);
    if (!ranges && mm->notify) {
        mm_ptl_unlock(mm, c->flags);
        return -1;
    }
    return 0;
}

/** End what `page_change_begin` began. */
static void page_change_end(struct mm *mm, const struct page_change *c)
{
    mm_ptl_unlock(mm, c->flags);
    if (c->counted) {
        mmn_change_end(mm);
    }
    if (c->bkl) {
        bkl_exit();
    }
}

/**
 * Resolve a write to the present copy-on-write (PTE_COW) page at `addr` of
 * `mm`. When this address space holds the frame's last mapping, the page is
 * made writable in place and loses its mark, whatever pins the frame has;
 * otherwise a writable copy of its own takes its place, the ranges
 * registered on the page are told (MMN_COW), and the shared frame's
 * reference is dropped. The copy is made first, with no lock but the
 * regions', and the entry is read again under the page-table lock and
 * replaced only while it is the one copied (`page_change_begin`). The
 * caller balanced memory before it looked at the page, so nothing here
 * sleeps. Every caller found the page present. Returns 1 if it resolved
 * the page; -1 when the entry changed meanwhile and the resolution runs
 * again: another thread's fault on the page made it writable after this one
 * found it read-only, which the faults of one address space may do at once,
 * each under its regions' lock held to read, or a zap made it absent, which
 * `paging_zap_where` may do while its caller holds that lock to read, as
 * Linux's handle_pte_fault leaves an entry that is no longer the one it read
 * and calls do_wp_page only for one present and not writable
 * (mm/memory.c:5799-5805); and 0 if the page is not copy-on-write or no
 * frame could be had or copied for it.
 */
static int cow_fault(struct mm *mm, uintptr_t addr)
{
    page_dir_t *dir = mm->dir;
    pte_t *e = user_pte(dir, addr);
    struct page_change c;
    pte_t pte;
    uintptr_t old_phys, frame;
    uintptr_t new_phys = 0;
    int rc = 1;

    addr &= ~(uintptr_t)(PAGE_SIZE - 1);
    pte = e ? *e : 0;
    if (!(pte & PTE_P) || (pte & PTE_W)) {
        return -1;
    }
    if (!(pte & PTE_COW)) {
        return 0;
    }
    old_phys = PTE_ADDR(pte);
    frame = old_phys / PAGE_SIZE;
    if (frame < _frames_number && frame_maps(frame) > 1) {
        new_phys = alloc_user_frame();
        if (new_phys == 0) {
            return 0;
        }
        if (paging_copy_frame(new_phys, old_phys) != 0) {
            free_frame(new_phys);
            return 0;
        }
        frame_make_anon(new_phys);
    }
    if (page_change_begin(mm, &c, new_phys ? MMN_COW : 0, addr) != 0) {
        rc = -1;
    } else {
        if (*(volatile pte_t *)e != pte) { /* another fault may have changed it */
            rc = -1;
        } else if (!new_phys) {
            frame_make_anon(old_phys);
            *e = (pte & ~(pte_t)(PTE_COW | PTE_ZEROFILL)) | PTE_W;
            paging_flush_dir(dir, addr);
        } else {
            *e = (pte_t)new_phys | PTE_P | PTE_W | PTE_U | (pte & (PTE_NOEXEC | PTE_PKEY));
            paging_flush_dir(dir, addr);
        }
        page_change_end(mm, &c);
    }
    if (rc < 0) {
        if (new_phys) {
            free_frame(new_phys);
        }
        return -1;
    }
    if (new_phys) {
        frame_unref(old_phys);
    }
    return 1;
}

/**
 * Give the present page at `addr` of `mm` a frame of its own, as a
 * copy-on-write fault would, but keeping its rights: a store the kernel then
 * makes through its own mapping of the frame changes this address space
 * alone. A copy-on-write page of a region that is `writable` takes the
 * copy-on-write path, which also makes it writable, as its region is. Any
 * other page another address space shares, the text a fork left in both or
 * a copy-on-write page of a region mprotect made read-only since, gets a
 * copy with the same rights, read-only still, and one this space alone
 * holds is used as it is; either way the frame is this space's alone after,
 * and the page loses its copy-on-write mark. The entry is replaced in two
 * phases, as `cow_fault` replaces it, and the change is made again when
 * the entry changed meanwhile. The caller balanced memory first, so nothing
 * here sleeps. 0, or -1 when the page is not present or no frame could be
 * had or copied.
 */
static int page_private(struct mm *mm, uintptr_t addr, int writable)
{
    page_dir_t *dir = mm->dir;
    struct page_change c;
    uintptr_t old_phys, new_phys;
    pte_t *e;
    pte_t pte;
    int rc;

    addr &= ~(uintptr_t)(PAGE_SIZE - 1);
    for (;;) {
        e = user_pte(dir, addr);
        if (!e || !((pte = *e) & PTE_P)) {
            return -1;
        }
        if ((pte & PTE_COW) && writable) {
            rc = cow_fault(mm, addr);
            if (rc < 0) {
                continue;
            }
            return rc ? 0 : -1;
        }
        old_phys = PTE_ADDR(pte);
        new_phys = old_phys;
        if (frame_maps(old_phys / PAGE_SIZE) > 1) {
            new_phys = alloc_user_frame();
            if (new_phys == 0) {
                return -1;
            }
            if (paging_copy_frame(new_phys, old_phys) != 0) {
                free_frame(new_phys);
                return -1;
            }
        }
        if (page_change_begin(mm, &c, new_phys != old_phys ? MMN_COW : 0, addr) == 0) {
            if (*e == pte) {
                frame_make_anon(new_phys);
                *e = (pte_t)new_phys | (pte & PTE_FLAGS & ~(pte_t)(PTE_COW | PTE_ZEROFILL));
                paging_flush_dir(dir, addr);
                page_change_end(mm, &c);
                if (new_phys != old_phys) {
                    frame_unref(old_phys);
                }
                return 0;
            }
            page_change_end(mm, &c);
        }
        if (new_phys != old_phys) {
            free_frame(new_phys);
        }
    }
}

void paging_entry_clear(struct mm *mm, uintptr_t addr, pte_t bits)
{
    uint32_t flags = mm_ptl_lock(mm);
    pte_t *e = user_pte(mm->dir, addr);

    if (e && (*e & PTE_P)) {
        *e &= ~bits;
    }
    mm_ptl_unlock(mm, flags);
}

/** `access_page` for the device region `vma`, whose page tables may not
 * reach its bytes: through its object's `access`, as Linux's
 * __access_remote_vm calls the region's, with the file held across it,
 * since it may sleep. 0, -ENOMEM, or -EIO for any other refusal. */
static int access_device(const vma_t *vma, uintptr_t addr, uint8_t *p, uint32_t len, int write)
{
    struct vm_file *f = vma->file;
    int rc;

    vm_file_get(f);
    rc = vm_device_access(f, ((uint64_t)vma->pgoff << PAGE_SHIFT) + (uint64_t)(addr - vma->start),
                          p, len, write);
    vm_file_put(f);
    return rc == 0 ? 0 : (rc == -ENOMEM ? -ENOMEM : -EIO);
}

/** One page's part of `paging_access`: `len` bytes at `addr`, within one
 * page, to or from `p`; 0, -EIO or -ENOMEM as `paging_access` says. A
 * device region's bytes go through `access_device`, and a region of
 * memfd_secret and the vDSO's data page are -EIO, as Linux's
 * get_user_pages refuses both. The resolution and, for a write, the
 * balance of memory before a copy may sleep with `mm`'s regions' lock let
 * go, so the region is looked up again after them, and the page must still
 * be present in a region that allows some access. */
static int access_page(struct mm *mm, uintptr_t addr, uint8_t *p, uint32_t len, int write)
{
    const vma_t *vma = vm_find(&mm->vm, addr);
    uint8_t *map;

    if (addr < USER_ADDR_MIN || addr >= USER_ADDR_MAX || vma == NULL ||
        !(vma->prot & (VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXEC)) ||
        (vma->prot & (VM_PROT_GUARD | VM_PROT_SECRET | VM_PROT_VVAR))) {
        return -EIO;
    }
    if (vm_device_object(vma->file)) {
        return access_device(vma, addr, p, len, write);
    }
    if (vma->prot & VM_PROT_IO) {
        return -EIO;
    }
    if (!pte_present(mm->dir, addr) && !vm_resolve_in(mm, addr, 0, (uintptr_t)RLIM_INFINITY)) {
        return -EIO;
    }
    if (write && mem_short() && mmap_may_drop(mm)) {
        balance_unlocked(mm);
    }
    vma = vm_find(&mm->vm, addr);
    if (vma == NULL || !(vma->prot & (VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXEC)) ||
        (vma->prot & (VM_PROT_IO | VM_PROT_GUARD | VM_PROT_SECRET | VM_PROT_VVAR)) ||
        !pte_present(mm->dir, addr)) {
        return -EIO;
    }
    if (write &&
        (vma->type == VM_SHARED ? !(vma->prot & VM_PROT_WRITE)
                                : page_private(mm, addr, (vma->prot & VM_PROT_WRITE) != 0) != 0)) {
        return -EIO;
    }
    map = kmap_temp(paging_user_phys(mm->dir, addr) & ~(uintptr_t)(PAGE_SIZE - 1));
    if (!map) {
        return -ENOMEM;
    }
    if (write) {
        k_memcpy(map + (addr & (PAGE_SIZE - 1)), p, len);
    } else {
        k_memcpy(p, map + (addr & (PAGE_SIZE - 1)), len);
    }
    kunmap_temp(map);
    return 0;
}

int paging_access(struct mm *mm, uintptr_t addr, void *buf, uint32_t len, int write)
{
    uint8_t *p = buf;
    int rc = 0;

    mmap_read_lock(mm);
    while (len > 0 && rc == 0) {
        uint32_t chunk = PAGE_SIZE - (addr & (PAGE_SIZE - 1));

        if (chunk > len) {
            chunk = len;
        }
        rc = access_page(mm, addr, p, chunk, write);
        addr += chunk;
        p += chunk;
        len -= chunk;
    }
    mmap_unlock(mm);
    mm_reap();
    return rc;
}

/** Whether the fault `p` at the user address `addr` is an instruction fetch
 * outside every region that may be executed, which no resolution makes
 * legal: the page may be present and readable, and would then be fetched
 * from again for ever. */
static int fetch_refused(uintptr_t addr, const isr_params_t *p)
{
    const vma_t *vma;

    if (!FAULT_EXEC(p)) {
        return 0;
    }
    vma = vm_find(&current_proc->mm->vm, addr);
    return !vma || !(vma->prot & VM_PROT_EXEC);
}

/**
 * The protection key that refuses the current thread's user fault `params`
 * at `addr`, a `write` or a read, or -1 when none does: the key of the
 * region that holds the address, when the error code says a key refused the
 * access or the thread's PKRU refuses it to the region, as Linux's
 * access_error asks before the region's own protection. An instruction
 * fetch is never refused by a key.
 */
static int fault_key(const isr_params_t *params, uintptr_t addr, int write)
{
    const vma_t *vma;

    if (FAULT_EXEC(params) || !fpu_pkeys()) {
        return -1;
    }
    vma = vm_find(&current_proc->mm->vm, addr);
    if (!vma || (!FAULT_PK(params) && pkey_allows(current_proc->mm, vma, write))) {
        return -1;
    }
    return (int)VM_PROT_KEY(vma->prot);
}

/**
 * Signal the current process for its user fault `params` at `addr` that no
 * resolution made legal, a `write` or a read: with `bus`, SIGBUS with
 * BUS_ADRERR, for a device region whose object had no page there; with a
 * `key` of 0 or more, which refused it, SIGSEGV with SEGV_PKUERR and the key
 * in `si_pkey`; else SIGSEGV, SEGV_ACCERR when `mapped`, a region that is no
 * guard holding the address, and SEGV_MAPERR when none does or a guard does,
 * as Linux answers a guard's fault. Each is forced as Linux forces it, and
 * the death is logged only when no handler and no tracer will see a
 * SIGSEGV. The caller holds the big lock, which posting a signal needs.
 */
static void fault_signal(const isr_params_t *params, uintptr_t addr, int write, int bus, int key,
                         int mapped)
{
    struct k_siginfo info;
    int signo = bus ? SIGBUS : SIGSEGV;

    k_memset(&info, 0, sizeof(info));
    if (bus) {
        info.code = BUS_ADRERR;
    } else if (key >= 0) {
        info.code = SEGV_PKUERR;
        info.pkey = (uint32_t)key;
    } else {
        info.code = mapped ? SEGV_ACCERR : SEGV_MAPERR;
    }
    info.addr = addr;
    current_proc->trap_nr = PAGE_FAULT_VECTOR;
    current_proc->trap_err = (uint32_t)params->error_code;
    current_proc->trap_addr = addr;
    signal_force(current_proc, signo, &info);
    if (!bus && current_proc->sigacts[SIGSEGV].handler == KSIG_DFL &&
        !(current_proc->ptrace & PT_TRACED)) {
        klog(KLOG_WARN, "vm", "fault: killing pid %d: bad %s at 0x%lx (err 0x%x)",
             current_proc->pid, write ? "write" : "read", (unsigned long)addr,
             (uint32_t)params->error_code);
    }
}

#ifdef FRAME_FP
/* How many callers a kernel fault's record names. */
#define FAULT_CALLERS 16

/**
 * Log the return addresses of the kernel frames that led to a fault in
 * kernel mode, by the frame-pointer chain from `fp`, so that a fault in a
 * shared helper such as k_strlen names its callers. Only links that climb
 * the running thread's kernel stack are followed, so a broken chain ends
 * the record and never faults again.
 */
static void fault_callers(uintptr_t fp)
{
    uintptr_t top, bottom;
    int depth;

    if (!current_proc || current_proc->kernel_stack < KERNEL_STACK_SIZE) {
        return;
    }
    top = current_proc->kernel_stack;
    bottom = top - KERNEL_STACK_SIZE;
    for (depth = 0; depth < FAULT_CALLERS; depth++) {
        const uintptr_t *frame = (const uintptr_t *)fp;

        if (fp < bottom || fp > top - 2 * sizeof(uintptr_t) || fp % sizeof(uintptr_t) != 0) {
            return;
        }
        klog(KLOG_CRIT, "vm", "fault: called from 0x%lX", (unsigned long)frame[1]);
        if (frame[0] <= fp) {
            return;
        }
        fp = frame[0];
    }
}
#endif

/**
 * Write the fault's note for the panic's report (`k_panic_note_fault`).
 * Called only just before a `k_panic`: the note is never cleared, so one
 * written on a path that returns would be reported under a later panic
 * that has nothing to do with it. Linux likewise prints a page fault only
 * on the path that dies (arch/x86/mm/fault.c:641-718, `show_fault_oops`
 * at 705).
 */
static void note_fault(const isr_params_t *params, uintptr_t fault_address)
{
    k_panic_note_fault("page fault", (uintptr_t)FRAME_PC(params), (long)params->error_code);
    k_panic_note_address(fault_address);
}

/** End a user process whose fault at `address` found no frame, on a write
 * when `write` is nonzero, through SIGKILL or directly for the system's
 * init, which drops SIGKILL. Called on the faulting thread's stack with the
 * big lock held; only the signal-posting path returns. */
static void fault_oom(uintptr_t address, int write)
{
    struct k_siginfo info;

    k_memset(&info, 0, sizeof(info));
    info.code = SI_KERNEL;
    klog(KLOG_WARN, "vm", "fault: out of memory: killing pid %d: no frame for a %s at 0x%lx",
         current_proc->pid, write ? "write" : "read", (unsigned long)address);
    if (current_proc->tgid == init_pid && !debug_shell_is_home) {
        terminate_current_process(SIGKILL);
    } else {
        signal_force(current_proc, SIGKILL, &info);
    }
}

/**
 * Resolve the fault at the user address `addr` of the current process under
 * its regions' lock, held to read, with interrupts on when the faulting code
 * had them on, as Linux's do_user_addr_fault enables them before it takes
 * mmap_lock (arch/x86/mm/fault.c:1359). Sets `*key` to the protection key
 * that refused the access, or -1, and `*mapped` to whether a region other
 * than a guard holds the address, both for the signal. Returns 1 when the
 * page is backed and allows the access.
 */
static int fault_resolve(isr_params_t *params, uintptr_t addr, int write, int *key, int *mapped)
{
    struct mm *mm = current_proc->mm;
    uint32_t was_in_call = current_proc->in_syscall;
    uint32_t irq = kirq_save();
    const vma_t *vma;
    int resolved;

    current_proc->frame_refused = 0;
    current_proc->bus_fault = 0;
    current_proc->fault_retry = 0;
    if (FRAME_IRQ_ON(params)) {
        current_proc->in_syscall = 1;
        arch_irq_enable(); /* the address is read; a nested fault cannot lose it */
    }
    mmap_read_lock(mm);
    *key = fault_key(params, addr, write);
    resolved = *key < 0 && !fetch_refused(addr, params) &&
               vm_resolve_in(mm, addr, write, current_proc->rlim[RLIM_AS].cur);
    vma = vm_find(&mm->vm, addr);
    *mapped = vma && !(vma->prot & VM_PROT_GUARD);
    mmap_unlock(mm);
    mm_reap();
    kirq_restore(irq);
    current_proc->in_syscall = was_in_call;
    return resolved;
}

/**
 * Resolve a page fault, or end what caused it.
 *
 * Tried in order. A fault while the panic reads the running process
 * never gets here: the router panics for it before it takes the big lock
 * (`panic_if_reading` in isr.c). Any fault at a user-space address of
 * a live user process goes to `vm_resolve_in`, but an instruction fetch
 * from a region without
 * execute (`fetch_refused`) and a data access that the thread's protection
 * keys refuse (`fault_key`), which no resolution makes legal and which is
 * SIGSEGV with SEGV_PKUERR and the key; the test is of the address and not
 * of the processor's mode, so that a kernel touch of a user page not yet
 * backed resolves too. A write to a page shared by a fork reaches `cow_fault`
 * through it, and only once the region's protection allows the write: a
 * region mprotect made read-only keeps its copy-on-write pages read-only. A
 * supervisor fault inside the page-cache window is filled by `pc_fault`,
 * whose polled read is safe with interrupts off; a fault past the backed
 * size, or one the fill cannot serve, panics. The window is a kernel-only
 * mapping, so a user-mode fault there, which would fault again on the
 * supervisor-only page the fill maps, is not filled and falls through to
 * the segfault.
 *
 * A user fault in a device region whose object has no page there now
 * (`device_fault`) is SIGBUS with BUS_ADRERR and the address, forced as
 * the SIGSEGV below is, as Linux answers a driver's VM_FAULT_SIGBUS.
 * A user fault of a legal access that no frame could be had for ends the
 * process with SIGKILL, which no handler sees. The system's init is ended
 * directly, since Linux 6.12.115's sig_task_ignored drops its SIGKILL
 * (kernel/signal.c:88-90). Linux's OOM killer excludes init instead
 * (mm/oom_kill.c:164-171); this kernel has no other-victim selection.
 * A SIGSEGV handler that returned would fault again. Any other user fault
 * is SIGSEGV with the address, SEGV_ACCERR when a region holds it and
 * SEGV_MAPERR when none does, forced as Linux forces it: a handler runs and
 * the faulting instruction runs again after it returns, and a tracer takes
 * the stop; the death is logged only when no handler and no tracer will see
 * the signal.
 *
 * Every panic here, the page-cache window's among them, follows the fault's
 * note for the report (`note_fault`), written just before it, so that no
 * path that returns leaves a note behind. What is left panics: the
 * usercopy helpers return -EFAULT before the
 * kernel touches a bad user pointer, so a kernel-mode fault here, or one
 * with no user context, is a kernel bug. One in the guard page below the
 * running thread's kernel stack is that stack running out, and the panic
 * says so. Most overruns do not reach this handler: the fault's own frame
 * would be pushed into the same guard, and the processor raises a double
 * fault instead (`isr_double_fault`).
 */
static void page_fault(isr_params_t *params, uintptr_t fault_address)
{
    int write, user_mode, have_user_proc, in_user_range, pid, key = -1;
    int resolved, mapped = 0, bkl;

    write = FAULT_WRITE(params);
    user_mode = FAULT_USER(params);
    have_user_proc = (current_proc != NULL && current_proc->mm != NULL);
    in_user_range = (fault_address >= USER_ADDR_MIN && fault_address < USER_ADDR_MAX);

    if (have_user_proc && in_user_range) {
        resolved = fault_resolve(params, fault_address, write, &key, &mapped);
        if (resolved) {
            return; /* page mapped; iret retries the faulting instruction */
        }
        if (user_mode && current_proc->fault_retry) {
            return; /* the signals are handled, then the instruction runs again */
        }
    }

    if (!user_mode && fault_address >= PCWIN_BASE &&
        fault_address < PCWIN_BASE + pc_window_size()) {
        if (!current_proc) {
            note_fault(params, fault_address);
            k_panic("pagecache: window fault with no current process");
        }
        bkl = !bkl_held();
        if (bkl) {
            bkl_enter();
        }
        resolved = pc_fault(fault_address, write);
        if (bkl) {
            bkl_exit();
        }
        if (resolved) {
            return;
        }
        note_fault(params, fault_address);
        k_panic("pagecache: unrecoverable disk fault");
    }

    if (user_mode && have_user_proc) {
        bkl = !bkl_held();
        if (bkl) {
            bkl_enter();
        }
        if (in_user_range && current_proc->frame_refused) {
            fault_oom(fault_address, write);
        } else {
            fault_signal(params, fault_address, write, in_user_range && current_proc->bus_fault,
                         in_user_range ? key : -1, mapped);
        }
        if (bkl) {
            bkl_exit();
        }
        return;
    }

    note_fault(params, fault_address);
    pid = kstack_overrun_pid(fault_address);
    if (pid >= 0) {
        klog(KLOG_CRIT, "vm",
             "fault: in kernel mode at 0x%lX (err 0x%X), pc 0x%lX: "
             "pid %d ran past the end of its kernel stack",
             (unsigned long)fault_address, (uint32_t)params->error_code,
             (unsigned long)FRAME_PC(params), pid);
        k_panic("page fault: a kernel stack ran out");
    }

    klog(KLOG_CRIT, "vm", "fault: in kernel mode at 0x%lX (err 0x%X), pc 0x%lX",
         (unsigned long)fault_address, (uint32_t)params->error_code,
         (unsigned long)FRAME_PC(params));
#ifdef FRAME_FP
    fault_callers((uintptr_t)FRAME_FP(params));
#endif

    k_panic("Unrecoverable error: page fault while in kernel mode.");
}

/**
 * Read the fault's address and handle the fault (`page_fault`), with
 * interrupts on for a fault from user mode, as Linux's `do_user_addr_fault`
 * turns them on (arch/x86/mm/fault.c), since its fill may sleep on a file's
 * locks. The address is read first, before an interrupt's own fault could
 * change it. Meanwhile the tick leaves its switch for the fault's end
 * (`in_fault`), as it does in a system call, since the fault changes the
 * address space under the big lock; the end takes it, as the dispatcher
 * does at a call's.
 */
static void _page_fault_handler(isr_params_t *params)
{
    uintptr_t fault_address = arch_fault_address();

    if (!FAULT_USER(params) || !current_proc) {
        page_fault(params, fault_address);
        return;
    }
    current_proc->in_fault = 1;
    arch_irq_enable();
    page_fault(params, fault_address);
    arch_irq_disable();
    current_proc->in_fault = 0;
    sched_take_preempt();
}

uintptr_t map_mmio(uintptr_t phys_addr, size_t size)
{
    return map_mmio_typed(phys_addr, size, PTE_NOCACHE);
}

/** The bit of page `i` of the MMIO region in `_mmio_used`. `mmio_lock` is
 * held. */
static int mmio_bit(uintptr_t i)
{
    return (_mmio_used[i / 32u] >> (i % 32u)) & 1u;
}

/** Mark the `count` pages of the MMIO region from page `i` used, or free
 * when `used` is 0. `mmio_lock` is held. */
static void mmio_mark(uintptr_t i, uintptr_t count, int used)
{
    for (; count > 0; i++, count--) {
        if (used) {
            _mmio_used[i / 32u] |= 1u << (i % 32u);
        } else {
            _mmio_used[i / 32u] &= ~(1u << (i % 32u));
        }
    }
}

/**
 * The first page of the lowest free run of `count` pages of the MMIO region
 * whose index is `phase` modulo `align`, or MMIO_PAGES when there is none.
 * The region has 32768 pages, so a first fit over the bit map is cheap.
 * `mmio_lock` is held.
 */
static uintptr_t mmio_find(uintptr_t count, uintptr_t align, uintptr_t phase)
{
    uintptr_t i, n;

    for (i = phase; i + count <= MMIO_PAGES; i += align) {
        for (n = 0; n < count && !mmio_bit(i + n); n++) {
        }
        if (n == count) {
            return i;
        }
    }
    return MMIO_PAGES;
}

#if KVA_MMIO_LARGE
/** Whether the span of MMU_TABLE_SPAN at `va`, a window's kernel address,
 * and `phys` behind it can be one large page within [lo, hi). */
static int mmio_span_large(uintptr_t va, uint64_t phys, uintptr_t lo, uintptr_t hi)
{
    return (va & (MMU_TABLE_SPAN - 1)) == 0 && (phys & (MMU_TABLE_SPAN - 1)) == 0 && va >= lo &&
           hi - va >= MMU_TABLE_SPAN;
}
#endif

/** Map the window of `pages` pages at the region's page `first` to the
 * device memory at `phys`, page-aligned, with the entry flags `flags`:
 * each aligned span with a large page where the architecture has them, the
 * rest a page at a time. `mmio_lock` is held. */
static void mmio_fill(uintptr_t first, uintptr_t pages, uint64_t phys, pte_t flags)
{
    uintptr_t lo = KVA_MMIO + first * PAGE_SIZE;
    uintptr_t hi = lo + pages * PAGE_SIZE;
    uintptr_t va = lo;

    while (va < hi) {
#if KVA_MMIO_LARGE
        uintptr_t slot = (va - KVA_MMIO) / MMU_TABLE_SPAN;

        if (mmio_span_large(va, phys, lo, hi) &&
            mmu_kernel_large(va, (phys_addr_t)phys, flags, &_mmio_tables[slot]) == 0) {
            _mmio_large[slot / 32u] |= 1u << (slot % 32u);
            arch_tlb_flush_page(va);
            va += MMU_TABLE_SPAN;
            phys += MMU_TABLE_SPAN;
            continue;
        }
#endif
        *kernel_pte(va) = (pte_t)phys | flags;
        va += PAGE_SIZE;
        phys += PAGE_SIZE;
    }
}

/**
 * The MMIO region's page tables exist from the boot (`mmu_init`) and every
 * directory shares them, so a mapping made here at any time is seen by
 * every process. A bit for each page of the region says it is taken, so a
 * window `mmio_unmap` frees is used again. `mmio_lock` covers the search,
 * the marks and the entries, so two processors never take one run.
 *
 * The page count is rounded up by division and not by adding a page less
 * one: a region reaching the top of the address space would wrap that sum to
 * a few bytes, count no pages, map nothing and still return an address.
 */
uintptr_t mmio_map(uint64_t phys, size_t size, pte_t cache)
{
    uint64_t start_page = phys & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t end_addr = phys + size;
    uint64_t span;
    uintptr_t pages, first, align = 1, phase = 0;
    uint32_t flags;

    if (size == 0 || end_addr < phys || end_addr > MMIO_PHYS_TOP) {
        klog(KLOG_ERR, "vm", "map_mmio: bad region 0x%x%08x, %zu bytes", (uint32_t)(phys >> 32),
             (uint32_t)phys, size);
        return 0;
    }
    span = end_addr - start_page;
    if (span / PAGE_SIZE + ((span % PAGE_SIZE) ? 1u : 0u) > MMIO_PAGES) {
        klog(KLOG_ERR, "vm", "map_mmio: 0x%x%08x, %zu bytes, is larger than the MMIO region",
             (uint32_t)(phys >> 32), (uint32_t)phys, size);
        return 0;
    }
    pages = (uintptr_t)(span / PAGE_SIZE + ((span % PAGE_SIZE) ? 1u : 0u));
#if KVA_MMIO_LARGE
    if (span >= MMU_TABLE_SPAN) {
        align = MMU_TABLE_SPAN / PAGE_SIZE;
        phase = (uintptr_t)((start_page / PAGE_SIZE) % align);
    }
#endif
    flags = spin_lock_irqsave(&mmio_lock);
    first = mmio_find(pages, align, phase);
#if KVA_MMIO_LARGE
    if (first >= MMIO_PAGES && align > 1) {
        first = mmio_find(pages, 1, 0); /* any run, in 4 KiB pages */
    }
#endif
    if (first >= MMIO_PAGES) {
        spin_unlock_irqrestore(&mmio_lock, flags);
        klog(KLOG_ERR, "vm", "map_mmio: no run of %lu pages free in the MMIO region",
             (unsigned long)pages);
        return 0;
    }
    mmio_mark(first, pages, 1);
    mmio_fill(first, pages, start_page, PTE_P | PTE_W | PTE_NOEXEC | cache);

    /* Every translation the processor cached goes. */
    arch_space_load(arch_space_root());
    smp_kernel_mapping_changed();
    spin_unlock_irqrestore(&mmio_lock, flags);

    return KVA_MMIO + first * PAGE_SIZE + (uintptr_t)(phys - start_page);
}

uintptr_t mmio_map_frames(const uintptr_t *frames, uintptr_t count, pte_t cache)
{
    uintptr_t first, i, va;
    uint32_t flags;

    if (count == 0 || count > MMIO_PAGES) {
        klog(KLOG_ERR, "vm", "map_mmio: bad list of %lu frames", (unsigned long)count);
        return 0;
    }
    flags = spin_lock_irqsave(&mmio_lock);
    first = mmio_find(count, 1, 0);
    if (first >= MMIO_PAGES) {
        spin_unlock_irqrestore(&mmio_lock, flags);
        klog(KLOG_ERR, "vm", "map_mmio: no run of %lu pages free in the MMIO region",
             (unsigned long)count);
        return 0;
    }
    mmio_mark(first, count, 1);
    va = KVA_MMIO + first * PAGE_SIZE;
    for (i = 0; i < count; i++) {
        *kernel_pte(va + i * PAGE_SIZE) =
            (pte_t)(frames[i] & ~(uintptr_t)(PAGE_SIZE - 1)) | PTE_P | PTE_W | PTE_NOEXEC | cache;
    }
    arch_space_load(arch_space_root());
    smp_kernel_mapping_changed();
    spin_unlock_irqrestore(&mmio_lock, flags);
    return va;
}

void mmio_unmap(uintptr_t va, size_t size)
{
    uintptr_t lo = va & ~(uintptr_t)(PAGE_SIZE - 1);
    uintptr_t hi = va + size;
    uint32_t flags;

    if (size == 0 || lo < KVA_MMIO || hi < va || hi > KVA_MMIO + KVA_MMIO_SIZE) {
        return;
    }
    hi = (hi + PAGE_SIZE - 1) & ~(uintptr_t)(PAGE_SIZE - 1); /* the region's end is aligned */
    flags = spin_lock_irqsave(&mmio_lock);
    mmio_mark((lo - KVA_MMIO) / PAGE_SIZE, (hi - lo) / PAGE_SIZE, 0);
    for (va = lo; va < hi;) {
#if KVA_MMIO_LARGE
        uintptr_t slot = (va - KVA_MMIO) / MMU_TABLE_SPAN;

        if (_mmio_large[slot / 32u] & (1u << (slot % 32u))) {
            _mmio_large[slot / 32u] &= ~(1u << (slot % 32u));
            mmu_kernel_unlarge(va, _mmio_tables[slot]);
            arch_tlb_flush_page(va);
            va += MMU_TABLE_SPAN;
            continue;
        }
#endif
        *kernel_pte(va) = 0;
        arch_tlb_flush_page(va);
        va += PAGE_SIZE;
    }
    smp_kernel_mapping_changed();
    spin_unlock_irqrestore(&mmio_lock, flags);
}

size_t mmio_page_size(uintptr_t va)
{
    size_t size;
    uint32_t flags;
    pte_t *e;

    if (va < KVA_MMIO || va - KVA_MMIO >= KVA_MMIO_SIZE) {
        return 0;
    }
    flags = spin_lock_irqsave(&mmio_lock);
#if KVA_MMIO_LARGE
    {
        uintptr_t slot = (va - KVA_MMIO) / MMU_TABLE_SPAN;

        if (_mmio_large[slot / 32u] & (1u << (slot % 32u))) {
            spin_unlock_irqrestore(&mmio_lock, flags);
            return MMU_TABLE_SPAN;
        }
    }
#endif
    e = kernel_pte(va);
    size = e && (*e & PTE_P) ? PAGE_SIZE : 0;
    spin_unlock_irqrestore(&mmio_lock, flags);
    return size;
}

void mmio_usage(size_t *used, size_t *largest)
{
    uintptr_t i, run = 0, best = 0, taken = 0;
    uint32_t flags = spin_lock_irqsave(&mmio_lock);

    for (i = 0; i < MMIO_PAGES; i++) {
        if (mmio_bit(i)) {
            taken++;
            run = 0;
        } else if (++run > best) {
            best = run;
        }
    }
    spin_unlock_irqrestore(&mmio_lock, flags);
    *used = taken * PAGE_SIZE;
    *largest = best * PAGE_SIZE;
}

uintptr_t map_mmio_typed(uintptr_t phys_addr, size_t size, pte_t cache)
{
    return mmio_map((uint64_t)phys_addr, size, cache);
}

#ifdef ARCH_DIRECT_MAP_ALL
/** The direct map reaches every frame: a temporary mapping is the frame's
 * address there, which changes no entry, so no processor has a translation
 * to drop. */
void *kmap_temp(uintptr_t phys)
{
    return phys_to_virt(phys);
}

void kunmap_temp(void *va)
{
    (void)va;
}

void paging_temp_split(unsigned int cpus)
{
    (void)cpus;
}
#else
void paging_temp_split(unsigned int cpus)
{
    uint32_t flags = kirq_save();
    uint32_t p;

    _temp_pool_count = cpus < 1u ? 1u : cpus > TEMP_POOLS_MAX ? TEMP_POOLS_MAX : cpus;
    _temp_pool_size = (KVA_TEMP_SLOTS / _temp_pool_count) & ~31u;
    for (p = 0; p < _temp_pool_count; p++) {
        struct temp_pool *pool = &_temp_pools[p];

        pool->first = p * _temp_pool_size;
        pool->end = p + 1u == _temp_pool_count ? KVA_TEMP_SLOTS : pool->first + _temp_pool_size;
        pool->next = pool->first;
    }
    kirq_restore(flags);
}

/** The pool that has `slot`. */
static struct temp_pool *temp_pool_of(uint32_t slot)
{
    uint32_t p = slot / _temp_pool_size;

    return &_temp_pools[p < _temp_pool_count ? p : _temp_pool_count - 1u];
}

/**
 * The first slot of `pool` from its `next` that is neither held nor stale,
 * or `KVA_TEMP_SLOTS` when there is none. The pool's lock is held.
 */
static uint32_t temp_find(struct temp_pool *pool)
{
    uint32_t n = pool->end - pool->first;
    uint32_t i;

    for (i = 0; i < n; i++) {
        uint32_t slot = pool->first + (pool->next - pool->first + i) % n;
        uint32_t bit = 1u << (slot % 32);

        if (!((_temp_slots[slot / 32] | _temp_stale[slot / 32]) & bit)) {
            pool->next = slot + 1u < pool->end ? slot + 1u : pool->first;
            return slot;
        }
    }
    return KVA_TEMP_SLOTS;
}

/**
 * Unmap every stale slot of `pool` and drop every processor's translations
 * before returning (`smp_kernel_mapping_changed`): one change of kernel
 * mappings for all the slots of the pool given back since its last sweep,
 * where unmapping each as it was given back would be one change for each.
 * The pool's lock is held; 0 when no slot was stale, and then nothing
 * changed.
 */
static int temp_sweep(struct temp_pool *pool)
{
    uint32_t w, slot, swept = 0;

    for (w = pool->first / 32; w < pool->end / 32; w++) {
        for (slot = w * 32; _temp_stale[w] != 0 && slot < w * 32 + 32; slot++) {
            if (_temp_stale[w] & (1u << (slot % 32))) {
                *kernel_pte(KVA_TEMP + slot * PAGE_SIZE) = 0;
                _temp_stale[w] &= ~(1u << (slot % 32));
                swept++;
            }
        }
    }
    if (swept == 0) {
        return 0;
    }
    arch_space_load(arch_space_root());
    smp_kernel_mapping_changed();
    return 1;
}

/**
 * A slot is taken from this processor's pool, sweeping it when nothing in it
 * is free, and then from each other pool in turn the same way, so that one
 * processor may hold every slot of the region. The new entry's translation
 * is dropped on this processor, since a sweep by another processor, or of
 * another's pool, left this one's translations of the slot in place. The
 * others need nothing: a sweep drops every processor's translations before
 * it returns, so none has one of a slot taken after it.
 */
void *kmap_temp(uintptr_t phys)
{
    uint32_t home = arch_cpu_id() % _temp_pool_count;
    uint32_t slot = KVA_TEMP_SLOTS;
    uint32_t i, flags;
    uintptr_t va;

    for (i = 0; i < _temp_pool_count && slot == KVA_TEMP_SLOTS; i++) {
        struct temp_pool *pool = &_temp_pools[(home + i) % _temp_pool_count];

        flags = spin_lock_irqsave(&pool->lock);
        slot = temp_find(pool);
        if (slot == KVA_TEMP_SLOTS && temp_sweep(pool)) {
            slot = temp_find(pool);
        }
        if (slot != KVA_TEMP_SLOTS) {
            _temp_slots[slot / 32] |= 1u << (slot % 32);
            *kernel_pte(KVA_TEMP + slot * PAGE_SIZE) =
                (pte_t)(phys & ~(PAGE_SIZE - 1)) | PTE_P | PTE_W;
        }
        spin_unlock_irqrestore(&pool->lock, flags);
    }
    if (slot == KVA_TEMP_SLOTS) {
        klog(KLOG_ERR, "vm", "temporary mappings: every slot is held");
        return 0;
    }
    va = KVA_TEMP + slot * PAGE_SIZE;
    arch_tlb_flush_page(va);
    return (void *)(va + (phys & (PAGE_SIZE - 1)));
}

void kunmap_temp(void *va)
{
    uintptr_t addr = (uintptr_t)va & ~(PAGE_SIZE - 1);
    uint32_t slot = (uint32_t)((addr - KVA_TEMP) / PAGE_SIZE);
    struct temp_pool *pool;
    uint32_t flags;

    if (addr < KVA_TEMP || slot >= KVA_TEMP_SLOTS) {
        return;
    }
    pool = temp_pool_of(slot);
    flags = spin_lock_irqsave(&pool->lock);
    _temp_slots[slot / 32] &= ~(1u << (slot % 32));
    _temp_stale[slot / 32] |= 1u << (slot % 32);
    spin_unlock_irqrestore(&pool->lock, flags);
}
#endif

int paging_zero_frame(uintptr_t phys)
{
    void *va = kmap_temp(phys);

    if (!va) {
        return -1;
    }
    k_memset(va, 0, PAGE_SIZE);
    kunmap_temp(va);
    return 0;
}

int paging_copy_frame(uintptr_t dst, uintptr_t src)
{
    void *d = kmap_temp(dst);
    void *s = kmap_temp(src);
    int rc = d && s ? 0 : -1;

    if (d && s) {
        k_memcpy(d, s, PAGE_SIZE);
    }
    if (s) {
        kunmap_temp(s);
    }
    if (d) {
        kunmap_temp(d);
    }
    return rc;
}

/*
 * The frame allocator's lock: the frame table, the free counts and the
 * hints the scans start from, against an allocation or a release on any
 * processor and in any interrupt. It is taken with interrupts off, after
 * the big lock and the heap's lock, and nothing under it takes another.
 */
static struct spinlock frame_lock = SPINLOCK_INIT_NAMED("frame_lock");

/** Mark `frame` used with one reference, whatever it held, and keep the
 * free counts. `frame_lock` is held, or the boot runs alone. */
static void frame_take(uintptr_t frame)
{
    if (_frames[frame] == 0) {
        _free_frames--;
        if (frame < _window_frames) {
            _free_window_frames--;
        }
        frame_index_take(frame);
    }
    _frames[frame] = 1;
    _frame_anon[frame / 8u] &= (uint8_t)~(1u << (frame % 8u));
}

/** Mark an allocated frame as anonymous before publishing a private copy.
 * The caller owns a reference and holds the big lock. */
static void frame_make_anon(uintptr_t phys)
{
    uintptr_t frame = phys / PAGE_SIZE;
    uint32_t flags = spin_lock_irqsave(&frame_lock);

    if (frame < _frames_number && _frames[frame] && _frames[frame] < FRAME_REF_MAX) {
        _frame_anon[frame / 8u] |= (uint8_t)(1u << (frame % 8u));
    }
    spin_unlock_irqrestore(&frame_lock, flags);
}

/** Whether a mapped frame is anonymous, under the big lock. Its mark lasts
 * until reuse, independent of each mapping's hardware dirty bit. */
static int frame_is_anon(uintptr_t phys)
{
    uintptr_t frame = phys / PAGE_SIZE;

    return frame < _frames_number && (_frame_anon[frame / 8u] & (1u << (frame % 8u))) != 0;
}

/**
 * A free frame for a user page or a kernel object: from above the window
 * first, then from below it while more than WINDOW_KEEP are free there;
 * `_frames_number` when there is none. The two hints say no frame below
 * each is free in its region, and the search from each passes the groups
 * the index has no free frame in (`frame_next_free`). `frame_lock` is held.
 */
static uintptr_t find_free_frame(void)
{
    uintptr_t frame;

    frame = frame_next_free(_next_free_upper, _frames_number);
    if (frame < _frames_number) {
        _next_free_upper = frame + 1;
        return frame;
    }
    if (_framehigh_low) {
        _framehigh_low = 0;
        _next_free_upper = _window_frames;
        return find_free_frame();
    }
    _next_free_upper = _frames_number;
    if (_free_window_frames <= WINDOW_KEEP) {
        return _frames_number;
    }
    frame = frame_next_free(_next_free_frame, _window_frames);
    if (frame < _window_frames) {
        _next_free_frame = frame + 1;
        return frame;
    }
    return _frames_number;
}

/** The selection and the claim are one step under `frame_lock`: an
 * allocation between them would take the frame this one had chosen. */
uintptr_t alloc_frame(void)
{
    uintptr_t frame;
    uint32_t flags;

    if (framefail_now() || !mem_may_take(0)) {
        return 0;
    }
    flags = spin_lock_irqsave(&frame_lock);
    frame = find_free_frame();
    if (frame >= _frames_number) {
        spin_unlock_irqrestore(&frame_lock, flags);
        return 0;
    }
    frame_take(frame);
    spin_unlock_irqrestore(&frame_lock, flags);
    return frame * PAGE_SIZE;
}

/** The first frame of the lowest run of `count` free frames in [from, to),
 * or `to` when there is none: each candidate is the next free frame the
 * index gives, and a used frame inside a run restarts the search past it.
 * `frame_lock` is held. */
static uintptr_t frame_run_free(uintptr_t from, uintptr_t to, uint32_t count)
{
    uintptr_t first, i;

    while (from < to && to - from >= count) {
        first = frame_next_free(from, to);
        if (first >= to || to - first < count) {
            return to;
        }
        i = 1;
        while (i < count && _frames[first + i] == 0) {
            i++;
        }
        if (i == count) {
            return first;
        }
        from = first + i + 1u;
    }
    return to;
}

/** The runs are sought where `find_free_frame` takes single frames: above
 * the window, from its hint and then below it, where `rach2.framehigh`
 * leaves frames free; then in the window while WINDOW_KEEP frames would be
 * left to `alloc_kernel_frame`. */
uintptr_t alloc_frames(uint32_t count)
{
    uintptr_t frame, i;
    uint32_t flags;

    if (count == 0 || framefail_now() || !mem_may_take(0)) {
        return 0;
    }
    flags = spin_lock_irqsave(&frame_lock);
    frame = frame_run_free(_next_free_upper, _frames_number, count);
    if (frame >= _frames_number) {
        frame = frame_run_free(_window_frames, _next_free_upper, count);
        if (frame >= _next_free_upper) {
            frame = _frames_number;
        }
    }
    if (frame >= _frames_number && _free_window_frames > WINDOW_KEEP + count) {
        frame = frame_run_free(_user_frames_start, _window_frames, count);
        if (frame >= _window_frames) {
            frame = _frames_number;
        }
    }
    if (frame >= _frames_number) {
        spin_unlock_irqrestore(&frame_lock, flags);
        return 0;
    }
    for (i = 0; i < count; i++) {
        frame_take(frame + i);
    }
    spin_unlock_irqrestore(&frame_lock, flags);
    return frame * PAGE_SIZE;
}

void free_frame(uintptr_t phys)
{
    frame_unref(phys);
}

int paging_frame_is_device(uintptr_t phys)
{
    uintptr_t frame = phys / PAGE_SIZE;

    return frame >= _frames_number || _frames[frame] >= FRAME_REF_MAX;
}

static pte_t *user_span(page_dir_t *dir, uintptr_t va, uintptr_t end, uintptr_t *next);

/** The region is found once for each it crosses and the leaf table once
 * for each table's span, so that a run of n pages costs n reads of entries
 * and not n walks from the top. */
uint64_t paging_user_ram_frames(struct mm *mm, uintptr_t va, uint64_t n, uintptr_t *frames)
{
    const vma_t *vma = NULL;
    pte_t *table = NULL;
    uintptr_t addr, end, next = 0, phys;
    uint64_t got = 0;
    pte_t e;

    if (!mm->dir || n > (~(uintptr_t)0 - va) / PAGE_SIZE) {
        return 0;
    }
    end = va + (uintptr_t)n * PAGE_SIZE;
    for (addr = va; addr < end; addr += PAGE_SIZE, got++) {
        if (!vma || addr >= vma->end) {
            vma = vm_find(&mm->vm, addr);
            if (!vma || !region_allows(vma, 1) ||
                (vma->prot & (VM_PROT_IO | VM_PROT_GUARD | VM_PROT_SECRET))) {
                break;
            }
        }
        if (!table || addr >= next) {
            table = user_span(mm->dir, addr, end, &next);
            if (!table) {
                break;
            }
        }
        e = table[MMU_INDEX(addr)];
        if ((e & (PTE_P | PTE_W)) != (PTE_P | PTE_W) || (e & PTE_COW)) {
            break;
        }
        phys = PTE_ADDR(e);
        if (paging_frame_is_device(phys) ||
            (vma->type != VM_SHARED && frame_maps(phys / PAGE_SIZE) > 1)) {
            break;
        }
        frames[got] = phys;
    }
    return got;
}

uintptr_t paging_user_ram_frame(struct mm *mm, uintptr_t va)
{
    uintptr_t phys;

    return paging_user_ram_frames(mm, va, 1, &phys) == 1 ? phys : 0;
}

uintptr_t paging_user_phys(page_dir_t *dir, uintptr_t va)
{
    pte_t *e = user_pte(dir, va);

    if (!e || !(*e & PTE_P)) {
        return 0;
    }
    return PTE_ADDR(*e) | (va & (PAGE_SIZE - 1));
}

/**
 * The leaf table of `dir` for the user address `va`, with the end of the span
 * it covers in `*next`; or NULL, with the end of the span around `va` whose
 * tables `dir` lacks (`mmu_user_skip`). `*next` is clipped to `end`. A walk
 * over a user range steps from span to span, and visits only the tables the
 * range has, so that a region of terabytes with few pages costs few steps.
 */
static pte_t *user_span(page_dir_t *dir, uintptr_t va, uintptr_t end, uintptr_t *next)
{
    pte_t *table = mmu_user_table(dir, va, 0);

    *next = table ? (va & ~(MMU_TABLE_SPAN - 1)) + MMU_TABLE_SPAN : mmu_user_skip(dir, va);
    if (*next > end || *next <= va) {
        *next = end;
    }
    return table;
}

int paging_user_page(page_dir_t *dir, uintptr_t va)
{
    pte_t *e = user_pte(dir, va);

    if (!e || !(*e & PTE_P)) {
        return PAGING_PAGE_NONE;
    }
    if (PTE_IS_ZERO(*e)) {
        return PAGING_PAGE_ZERO;
    }
    if (paging_frame_is_device(PTE_ADDR(*e))) {
        return PAGING_PAGE_DEVICE;
    }
    return PAGING_PAGE_FRAME;
}

void paging_user_frames(page_dir_t *dir, uintptr_t start, uintptr_t end,
                        struct paging_frame_counts *counts)
{
    uintptr_t va, next;

    k_memset(counts, 0, sizeof(*counts));
    for (va = start; va < end; va = next) {
        pte_t *table = user_span(dir, va, end, &next);

        for (; table && va < next; va += PAGE_SIZE) {
            pte_t e = table[MMU_INDEX(va)];

            if (!(e & PTE_P) || PTE_IS_ZERO(e) || paging_frame_is_device(PTE_ADDR(e))) {
                continue;
            }
            counts->frames++;
            counts->dirty += (e & PTE_DIRTY) != 0;
            counts->pinned += frame_pinned(PTE_ADDR(e)) != 0;
            counts->anon += frame_is_anon(PTE_ADDR(e));
        }
    }
}

void paging_user_count(page_dir_t *dir, uintptr_t start, uintptr_t end, unsigned long *present,
                       unsigned long *dirty, unsigned long *anon)
{
    uintptr_t va, next;

    *present = *dirty = *anon = 0;
    for (va = start; va < end; va = next) {
        pte_t *table = user_span(dir, va, end, &next);

        for (; table && va < next; va += PAGE_SIZE) {
            pte_t e = table[MMU_INDEX(va)];

            if (e & PTE_P) {
                (*present)++;
                *dirty += (e & PTE_DIRTY) != 0;
                *anon += frame_is_anon(PTE_ADDR(e));
            }
        }
    }
}

/**
 * The scan runs down from the top of the window, while user pages come from
 * above the window first and then from the window's bottom up, so a kernel
 * frame and user pages rarely contend.
 */
void *alloc_kernel_frame(void)
{
    uint32_t flags = spin_lock_irqsave(&frame_lock);
    uintptr_t frame = frame_prev_free(_user_frames_start, _window_frames);

    if (frame >= _user_frames_start && frame < _window_frames) {
        frame_take(frame);
        _kernel_frames_reserved++;
        spin_unlock_irqrestore(&frame_lock, flags);
        return phys_to_virt(frame * PAGE_SIZE);
    }
    spin_unlock_irqrestore(&frame_lock, flags);
    return 0;
}

void paging_mark_reserved(uintptr_t frame)
{
    uint32_t flags = spin_lock_irqsave(&frame_lock);

    if (frame < _frames_number) {
        frame_take(frame);
    }
    spin_unlock_irqrestore(&frame_lock, flags);
}

void free_kernel_frame(void *frame)
{
    uint32_t flags = spin_lock_irqsave(&frame_lock);

    _kernel_frames_reserved--;
    spin_unlock_irqrestore(&frame_lock, flags);
    frame_unref(virt_to_phys(frame));
}

size_t frame_total_count(void)
{
    return _managed_frames;
}

/** One word is read and nothing scanned. A count at one moment is what
 * sysinfo, the leak probe and reclaim need, so no lock is taken. */
size_t frame_free_count(void)
{
    return _free_frames;
}

/**
 * An entry that was not present is in no processor's TLB, since x86 caches
 * no translation of a page that is not present, so a new mapping is
 * flushed on this processor alone; a changed one everywhere.
 */
void map_kernel_frame(uintptr_t va, uintptr_t phys, int writable)
{
    pte_t *e = kernel_pte(va);
    pte_t old;

    if (!e) {
        return;
    }
    old = *e;
    *e = (pte_t)(phys & ~(PAGE_SIZE - 1)) | PTE_P | (writable ? PTE_W : 0);
    if (old & PTE_P) {
        paging_flush_tlb(va);
    } else {
        arch_tlb_flush_page(va);
    }
}

void unmap_kernel_frame(uintptr_t va)
{
    unmap_kernel_frames(va, 1);
}

void unmap_kernel_frames(uintptr_t va, uint32_t count)
{
    uint32_t i;
    int present = 0;

    for (i = 0; i < count; i++) {
        pte_t *e = kernel_pte(va + (uintptr_t)i * PAGE_SIZE);

        if (e && (*e & PTE_P)) {
            *e = 0;
            arch_tlb_flush_page(va + (uintptr_t)i * PAGE_SIZE);
            present = 1;
        }
    }
    if (present) {
        smp_kernel_flush(va, va + (uintptr_t)count * PAGE_SIZE);
    }
}

void paging_set_writable(page_dir_t *dir, uintptr_t addr, bool writable)
{
    pte_t *e = user_pte(dir, addr);

    if (!e || !(*e & PTE_P)) {
        return;
    }
    if (writable) {
        *e |= PTE_W;
    } else {
        *e &= ~(pte_t)PTE_W; /* read-only for both user and kernel */
    }
    paging_flush_dir(dir, addr);
}

void paging_set_exec(page_dir_t *dir, uintptr_t addr, bool exec)
{
    pte_t *e = user_pte(dir, addr);

    if (!e || !(*e & PTE_P)) {
        return;
    }
    *e &= ~(pte_t)PTE_NOEXEC;
    if (!exec) {
        *e |= PTE_NOEXEC;
    }
    paging_flush_dir(dir, addr);
}

pte_t paging_prot_bits(uint32_t prot)
{
    pte_t bits = PTE_NOEXEC;

    if (prot & VM_PROT_EXEC) {
        bits = 0;
    }
#if PTE_PKEY_SHIFT
    bits |= ((pte_t)VM_PROT_KEY(prot) << PTE_PKEY_SHIFT) & (pte_t)PTE_PKEY;
#endif
    return bits;
}

void paging_flush_tlb(uintptr_t addr)
{
    paging_flush_dir(_curr_dir, addr);
}

void paging_flush_dir(const page_dir_t *dir, uintptr_t addr)
{
    if (addr < USER_ADDR_MIN || addr >= USER_ADDR_MAX) {
        addr &= ~(uintptr_t)(PAGE_SIZE - 1);
        arch_tlb_flush_page(addr);
        smp_kernel_flush(addr, addr + PAGE_SIZE);
        return;
    }
    addr &= ~(uintptr_t)(PAGE_SIZE - 1);
    paging_flush_range(dir, addr, addr + PAGE_SIZE);
}

void paging_flush_range(const page_dir_t *dir, uintptr_t start, uintptr_t end)
{
    uintptr_t va;

    if (start >= end) {
        return;
    }
    if ((end - start) / PAGE_SIZE > PAGING_FLUSH_CEILING) {
        if (dir == _curr_dir) {
            arch_space_load(arch_space_root());
        }
        smp_tlb_shootdown(dir, CPU_TLB_ALL, 0);
        return;
    }
    if (dir == _curr_dir) {
        for (va = start; va < end; va += PAGE_SIZE) {
            arch_tlb_flush_page(va);
        }
    }
    smp_tlb_shootdown(dir, start, end);
}

/*
 * The span of user pages of `dir` whose entries a walk changed, which one
 * flush drops at the walk's end (`batch_flush`) instead of a flush for each
 * page; `lo` equal to `hi` is an empty span.
 */
struct flush_batch {
    const page_dir_t *dir;
    uintptr_t lo, hi;
};

/** Start an empty batch for `dir`. */
static void batch_start(struct flush_batch *b, const page_dir_t *dir)
{
    b->dir = dir;
    b->lo = b->hi = 0;
}

/** Add the page `va` to the span of `b`. */
static void batch_add(struct flush_batch *b, uintptr_t va)
{
    va &= ~(uintptr_t)(PAGE_SIZE - 1);
    if (b->lo == b->hi) {
        b->lo = va;
        b->hi = va + PAGE_SIZE;
        return;
    }
    if (va < b->lo) {
        b->lo = va;
    }
    if (va + PAGE_SIZE > b->hi) {
        b->hi = va + PAGE_SIZE;
    }
}

/** Drop the translations of the span of `b` and empty it. */
static void batch_flush(struct flush_batch *b)
{
    paging_flush_range(b->dir, b->lo, b->hi);
    b->lo = b->hi = 0;
}

void frame_ref(uintptr_t phys)
{
    uintptr_t frame = phys / PAGE_SIZE;
    uint32_t flags = spin_lock_irqsave(&frame_lock);

    if (frame < _frames_number && _frames[frame] != 0 && _frames[frame] < FRAME_REF_MAX - 1) {
        _frames[frame]++;
    }
    spin_unlock_irqrestore(&frame_lock, flags);
}

/** Count `frame`, whose word just reached 0, among the free frames, with
 * `frame_lock` held. A frame freed below a hint moves the hint down to it. */
static void frame_freed(uintptr_t frame)
{
    _free_frames++;
    frame_index_put(frame);
    if (frame < _window_frames) {
        _free_window_frames++;
        if (frame >= _user_frames_start && frame < _next_free_frame) {
            _next_free_frame = frame;
        }
    } else if (frame < _next_free_upper) {
        _next_free_upper = frame;
    }
}

/** `frame_unref` with `frame_lock` held. A state word is not a count: a
 * frame the map does not offer has no mapping to drop. */
static uint32_t frame_unref_locked(uintptr_t phys)
{
    uintptr_t frame = phys / PAGE_SIZE;

    if (frame >= _frames_number || _frames[frame] == 0) {
        return 0;
    }
    if (_frames[frame] >= FRAME_REF_MAX) {
        return _frames[frame];
    }
    _frames[frame]--;
    if (_frames[frame] == 0) {
        frame_freed(frame);
    }
    return _frames[frame];
}

/** A frame at FRAME_REF_MAX - FRAME_PIN_BIAS or above has no room for one
 * more pin below the state words. */
int frame_pin(uintptr_t phys)
{
    uintptr_t frame = phys / PAGE_SIZE;
    uint32_t flags = spin_lock_irqsave(&frame_lock);
    int rc = -1;

    if (frame < _frames_number && _frames[frame] != 0 &&
        _frames[frame] < FRAME_REF_MAX - FRAME_PIN_BIAS) {
        _frames[frame] += FRAME_PIN_BIAS;
        rc = 0;
    }
    spin_unlock_irqrestore(&frame_lock, flags);
    return rc;
}

void frame_unpin(uintptr_t phys)
{
    uintptr_t frame = phys / PAGE_SIZE;
    uint32_t flags = spin_lock_irqsave(&frame_lock);

    if (frame < _frames_number && _frames[frame] >= FRAME_PIN_BIAS &&
        _frames[frame] < FRAME_REF_MAX) {
        _frames[frame] -= FRAME_PIN_BIAS;
        if (_frames[frame] == 0) {
            frame_freed(frame);
        }
    }
    spin_unlock_irqrestore(&frame_lock, flags);
}

/** The references of `frame` that are not pins: its mappings, and the
 * table of a file's pages that keeps one. 0 for one past the table or a
 * state word. */
static uint32_t frame_maps(uintptr_t frame)
{
    if (frame >= _frames_number || _frames[frame] >= FRAME_REF_MAX) {
        return 0;
    }
    return _frames[frame] % FRAME_PIN_BIAS;
}

int frame_pinned(uintptr_t phys)
{
    uintptr_t frame = phys / PAGE_SIZE;

    return frame < _frames_number && _frames[frame] >= FRAME_PIN_BIAS &&
           _frames[frame] < FRAME_REF_MAX;
}

uint32_t frame_unref(uintptr_t phys)
{
    uint32_t flags = spin_lock_irqsave(&frame_lock);
    uint32_t left = frame_unref_locked(phys);

    spin_unlock_irqrestore(&frame_lock, flags);
    return left;
}

int map_user_frame(page_dir_t *dir, uintptr_t virt, uintptr_t phys, pte_t flags)
{
    pte_t *table;
    pte_t old;

    virt &= ~(PAGE_SIZE - 1);
    table = mmu_user_table(dir, virt, 1);
    if (!table) {
        return -1;
    }

    old = table[MMU_INDEX(virt)];
    table[MMU_INDEX(virt)] = PTE_ADDR((pte_t)phys) | (flags & (pte_t)PTE_FLAGS);
    mm_resident_add(dir, ((flags & PTE_P) != 0) - ((old & PTE_P) != 0));
    /* x86 caches no translation of an entry that is not present. */
    if (old & PTE_P) {
        paging_flush_dir(dir, virt);
    }
    return 0;
}

/**
 * The selection and the claim are one step under `frame_lock`, as in
 * `alloc_frame`; the refusals come first, so that a refused selection does
 * not move the hint past a free frame. When no frame is free, the record
 * says who holds the pool, so that a machine that runs out shows what
 * filled it: the boot's reservations below the user frames, the kernel's
 * frames from the top, and the user frames between, with the shared ones
 * counted apart.
 */
uintptr_t alloc_user_frame(void)
{
    uintptr_t frame;
    uint32_t flags;

    if (!mem_may_take(1)) {
        klog(KLOG_WARN, "vm", "user page: refused: the free frames are the reserve's, pid %d",
             current_proc ? (int)current_proc->pid : 0);
        if (current_proc) {
            current_proc->frame_refused = 1;
        }
        return 0;
    }
    flags = spin_lock_irqsave(&frame_lock);
    frame = framefail_now() ? _frames_number : find_free_frame();
    if (frame < _frames_number) {
        frame_take(frame);
    }
    spin_unlock_irqrestore(&frame_lock, flags);

    if (frame >= _frames_number) {
        size_t user_used = 0, user_shared = 0;
        uintptr_t f;

        for (f = _user_frames_start; f < _frames_number; f++) {
            if (_frames[f] != 0 && _frames[f] < FRAME_REF_MAX) {
                user_used++;
            }
            if (frame_maps(f) > 1) {
                user_shared++;
            }
        }
        user_used -= user_used < _kernel_frames_reserved ? user_used : _kernel_frames_reserved;
        klog(KLOG_ERR, "vm",
             "user page: no free frames in %ld: %ld boot, %d kernel, %ld user of which %ld shared, "
             "pid %d",
             (long)_frames_number, (long)_user_frames_start, (int)_kernel_frames_reserved,
             (long)user_used, (long)user_shared, current_proc ? current_proc->pid : 0);
        if (current_proc) {
            current_proc->frame_refused = 1;
        }
        return 0;
    }
    return frame * PAGE_SIZE;
}

/** The table is made first: one that cannot be had then leaves no frame
 * to give back. */
uintptr_t alloc_user_page(page_dir_t *dir, uintptr_t virt_addr)
{
    uintptr_t phys_addr;
    pte_t *table;
    pte_t old;

    virt_addr &= ~(PAGE_SIZE - 1);

    table = mmu_user_table(dir, virt_addr, 1);
    if (!table) {
        return 0;
    }
    phys_addr = alloc_user_frame();
    if (phys_addr == 0) {
        return 0;
    }

    frame_make_anon(phys_addr);
    old = table[MMU_INDEX(virt_addr)];
    table[MMU_INDEX(virt_addr)] = (pte_t)phys_addr | PTE_P | PTE_W | PTE_U;
    mm_resident_add(dir, 1 - ((old & PTE_P) != 0));
    if (old & PTE_P) {
        paging_flush_dir(dir, virt_addr);
    }

    return phys_addr;
}

/**
 * Two walks: the first takes the present bit from each present entry and
 * leaves its frame in it, marked PTE_NONE, an entry no other code makes,
 * and the second, after the one flush, clears those entries and drops their
 * frames. So no processor can reach a frame after it is freed, and a large
 * unmap costs one shootdown, not one a page. The caller holds the space's
 * lock to write, or the space is no thread's, so no fault installs an entry
 * meanwhile and nothing else sees the marked entries. The big lock is taken
 * throughout, since code that reads user memory under it with no fault
 * expects a page it found to stay until it sleeps.
 */
void unmap_user_range(page_dir_t *dir, uintptr_t start, uintptr_t end)
{
    struct flush_batch b;
    uintptr_t addr, next, lo, hi;
    long gone = 0;

    if (!dir) {
        return;
    }
    bkl_enter();
    batch_start(&b, dir);
    for (addr = start; addr < end; addr = next) {
        pte_t *table = user_span(dir, addr, end, &next);

        for (; table && addr < next; addr += PAGE_SIZE) {
            pte_t *e = &table[MMU_INDEX(addr)];

            if (*e & PTE_P) {
                *e = PTE_ADDR(*e) | PTE_NONE;
                batch_add(&b, addr);
                gone++;
            }
        }
    }
    mm_resident_add(dir, -gone);
    lo = b.lo;
    hi = b.hi;
    batch_flush(&b);
    for (addr = lo; addr < hi; addr = next) {
        pte_t *table = user_span(dir, addr, hi, &next);

        for (; table && addr < next; addr += PAGE_SIZE) {
            pte_t *e = &table[MMU_INDEX(addr)];

            if ((*e & (PTE_P | PTE_NONE)) == PTE_NONE) {
                uintptr_t phys = PTE_ADDR(*e);

                *e = 0;
                if (phys) {
                    frame_unref(phys);
                }
            }
        }
    }
    bkl_exit();
}

/**
 * Write-lock `mm` for `paging_zap_device`, under the big lock that the walk
 * of the process list holds: 1 when it is held to write now, by this call
 * or already by the caller; 0 when the caller holds it only to read, which
 * a zap cannot wait for, and the space is zapped as it stands; -1 when the
 * lock had to be waited for, which lets the big lock go, so that the walk
 * starts again. `*taken` says whether this call took a hold to give back.
 */
static int zap_lock(struct mm *mm, int *taken)
{
    *taken = 0;
    if (mmap_held(mm, 1)) {
        return 1;
    }
    if (mmap_held(mm, 0)) {
        return 0;
    }
    if (mmap_write_trylock(mm)) {
        *taken = 1;
        return 1;
    }
    mm_get(mm);
    mmap_write_lock(mm);
    mmap_unlock(mm);
    mm_put(mm);
    return -1;
}

/* The number of the last walk of `paging_zap_where`, under the big lock. */
static uint32_t zap_walks;

/**
 * Each thread of a process has its own record on the process list and the
 * address space of its process; a walk changes each space once, the first
 * time it meets it, and passes it for the threads after, as Linux's
 * unmap_mapping_range visits each region that maps the object once
 * (mm/memory.c:3755-3774): QEMU's space, which a VM's threads share, would
 * otherwise be walked region by region for each of its dozen threads at
 * each revoke of a blob. When the count of walks wraps, every space's mark
 * is cleared first, so that no stale mark passes a space. A walk that
 * waited for a lock starts again from the first record with a new number
 * and zaps every space again, since the big lock was free during the wait
 * and a thread of a space already changed may have faulted the object back
 * in; only the last pass, which waits for nothing, runs whole under the big
 * lock, as Linux's unmap_mapping_pages holds i_mmap_rwsem across its whole
 * walk (mm/memory.c:3822-3838). Each space is changed under its regions' lock
 * held to write (`zap_lock`), and its count of zaps moves, so that a fault
 * that let the lock go across a file's or a device's operation maps no
 * page taken back meanwhile.
 */
void paging_zap_where(int (*match)(const struct vma *r, void *arg, uintptr_t *lo, uintptr_t *hi),
                      void *arg)
{
    process_t *p;
    const vma_t *r;
    uintptr_t lo, hi;
    uint32_t walk;
    int held, taken;

    bkl_assert_held();
again:
    if (zap_walks == 0xFFFFFFFFUL) {
        for (p = proc_list; p; p = p->next) {
            if (p->mm) {
                p->mm->zap_walk = 0;
            }
        }
        zap_walks = 0;
    }
    walk = ++zap_walks;
    for (p = proc_list; p; p = p->next) {
        struct mm *mm = p->mm;

        if (!mm || !mm->dir || mm->zap_walk == walk) {
            continue;
        }
        held = zap_lock(mm, &taken);
        if (held < 0) {
            goto again;
        }
        mm->zaps++;
        mm->zap_walk = walk;
        for (r = vm_first(&mm->vm); r; r = vm_next(r)) {
            if (match(r, arg, &lo, &hi)) {
                /* Under way, for a space this thread holds only to read. */
                mmn_change_begin(mm);
                mmn_notify(mm, MMN_TRUNCATE, lo, hi);
                unmap_user_range(mm->dir, lo, hi);
                mmn_change_end(mm);
            }
        }
        if (taken) {
            mmap_unlock(mm);
        }
    }
}

/** What `paging_zap_device` unmaps: its object, and pages `first` to `end`
 * of it. */
struct zap_device {
    const void *obj;
    uint32_t first, end;
};

/** The `paging_zap_where` match of `paging_zap_device`: the part of the
 * region `r` that maps the pages of `arg`, a `struct zap_device`. */
static int zap_device_match(const vma_t *r, void *arg, uintptr_t *lo, uintptr_t *hi)
{
    const struct zap_device *z = arg;
    uint32_t last = r->pgoff + (uint32_t)((r->end - r->start) / PAGE_SIZE);
    uint32_t from = r->pgoff > z->first ? r->pgoff : z->first;
    uint32_t to = last < z->end ? last : z->end;

    if (from >= to || vm_device_object(r->file) != z->obj) {
        return 0;
    }
    *lo = r->start + (uintptr_t)(from - r->pgoff) * PAGE_SIZE;
    *hi = r->start + (uintptr_t)(to - r->pgoff) * PAGE_SIZE;
    return 1;
}

void paging_zap_device(const void *obj, uint32_t pgoff, uint32_t count)
{
    struct zap_device z;

    if (!obj) {
        return;
    }
    z.obj = obj;
    z.first = pgoff;
    z.end = count > 0xFFFFFFFFu - pgoff ? 0xFFFFFFFFu : pgoff + count;
    paging_zap_where(zap_device_match, &z);
}

/** The tables of the new range are made first, so that the move cannot
 * stop half way. The caller holds the space's lock to write; the big lock
 * is taken, as `unmap_user_range` takes it. */
int paging_move_range(page_dir_t *dir, uintptr_t from, uintptr_t to, uintptr_t len)
{
    uintptr_t addr, next, end = from + len;
    struct flush_batch moved, replaced;
    pte_t *table;
    long gone = 0;

    for (addr = from; addr < end; addr = next) {
        for (table = user_span(dir, addr, end, &next); table && addr < next; addr += PAGE_SIZE) {
            if (table[MMU_INDEX(addr)] && !mmu_user_table(dir, to + (addr - from), 1)) {
                return -1;
            }
        }
    }
    bkl_enter();
    batch_start(&moved, dir);
    batch_start(&replaced, dir);
    for (addr = from; addr < end; addr = next) {
        for (table = user_span(dir, addr, end, &next); table && addr < next; addr += PAGE_SIZE) {
            pte_t *e = &table[MMU_INDEX(addr)];

            if (*e) {
                pte_t *dst = user_pte(dir, to + (addr - from));

                if (*dst & PTE_P) {
                    batch_add(&replaced, to + (addr - from));
                    gone++;
                }
                *dst = *e;
                if (*e & PTE_P) {
                    batch_add(&moved, addr);
                }
                *e = 0;
            }
        }
    }
    batch_flush(&moved);
    batch_flush(&replaced);
    mm_resident_add(dir, -gone);
    bkl_exit();
    return 0;
}

/** Give the present user page `addr`, whose entry is `e`, the protection
 * `prot`, for `paging_protect_range`, and add it to the batch `b` to flush;
 * a page not present is left alone, since its fault reads the region's
 * protection. */
static void protect_page(struct flush_batch *b, pte_t *e, uintptr_t addr, uint32_t prot)
{
    pte_t pte = *e;
    uintptr_t frame;

    if (!(pte & PTE_P)) {
        return;
    }
    if (!(prot & (VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXEC))) {
        pte = (pte & ~(pte_t)(PTE_W | PTE_U)) | PTE_NONE;
    } else {
        pte = (pte & ~(pte_t)(PTE_NONE | PTE_NOEXEC | PTE_PKEY)) | PTE_U | paging_prot_bits(prot);
        frame = PTE_ADDR(pte) / PAGE_SIZE;
        if (!(prot & VM_PROT_WRITE)) {
            pte &= ~(pte_t)PTE_W;
        } else if (prot & VM_PROT_SHARED) {
            pte |= PTE_W;
        } else if (pte & PTE_COW) {
            /* Still shared: the first write copies or takes it. */
        } else if (frame_maps(frame) > 1) {
            pte = (pte & ~(pte_t)PTE_W) | PTE_COW;
        } else {
            pte |= PTE_W;
        }
    }
    *e = pte;
    batch_add(b, addr);
}

/** The caller holds the space's lock to write; the big lock is taken, as
 * `unmap_user_range` takes it, since a page made read-only is one a write
 * under the big lock would otherwise fault on. */
void paging_protect_range(page_dir_t *dir, uintptr_t start, uintptr_t end, uint32_t prot)
{
    struct flush_batch b;
    uintptr_t addr, next;
    pte_t *table;

    bkl_enter();
    batch_start(&b, dir);
    for (addr = start; addr < end; addr = next) {
        for (table = user_span(dir, addr, end, &next); table && addr < next; addr += PAGE_SIZE) {
            protect_page(&b, &table[MMU_INDEX(addr)], addr, prot);
        }
    }
    batch_flush(&b);
    bkl_exit();
}

/**
 * Give the child's directory `child_dir` the page of the parent, the
 * directory in use, whose entry is `*e`, at `va` in a region of type `type`,
 * as `paging_fork_copy` describes: 0, or -1 with no memory for a table or a
 * copy. A page is given only when it is present, user or PROT_NONE, and
 * names a frame other than 0. A writable private page is made read-only and
 * copy-on-write in the parent too, so that the first write on either side
 * faults into `cow_fault`; its dirty bit is kept in the child. A private
 * page pinned for a device (`frame_pin`)
 * stays the parent's as it is, and the child gets a copy with the same
 * rights, as Linux copies a page that may be pinned at fork: a write by the
 * parent would otherwise move the parent to a copy and leave the device
 * with the frame the child has. A parent's page made read-only is added to
 * the batch `b`, which the caller flushes.
 */
static int fork_copy_page(struct flush_batch *b, page_dir_t *child_dir, pte_t *e, uintptr_t va,
                          vm_type_t type)
{
    pte_t pte = *e;
    uintptr_t parent_phys = PTE_ADDR(pte);
    uintptr_t copy;

    if (!(pte & PTE_P) || !(pte & (PTE_U | PTE_NONE)) || parent_phys == 0) {
        return 0;
    }
    if (type != VM_SHARED && frame_pinned(parent_phys)) {
        copy = alloc_user_frame();
        if (copy == 0) {
            return -1;
        }
        if (frame_is_anon(parent_phys)) {
            frame_make_anon(copy);
        }
        if (paging_copy_frame(copy, parent_phys) != 0 ||
            map_user_frame(child_dir, va, copy, pte & PTE_FLAGS) != 0) {
            free_frame(copy);
            return -1;
        }
        return 0;
    }
    if (type == VM_SHARED) {
        if (map_user_frame(child_dir, va, parent_phys, pte & PTE_FLAGS) != 0) {
            return -1;
        }
        frame_ref(parent_phys);
        return 0;
    }
    if (pte & PTE_W) {
        if (map_user_frame(child_dir, va, parent_phys,
                           PTE_P | PTE_U | PTE_COW | (pte & (PTE_NOEXEC | PTE_PKEY | PTE_DIRTY)) |
                               (PTE_IS_ZERO(pte) ? PTE_ZEROFILL : 0)) != 0) {
            return -1;
        }
        *e = (pte & ~(pte_t)PTE_W) | PTE_COW;
        batch_add(b, va);
    } else if (map_user_frame(child_dir, va, parent_phys, pte & PTE_FLAGS) != 0) {
        return -1;
    }
    frame_ref(parent_phys);
    return 0;
}

/**
 * The walk is over the parent's regions, and in each over the leaf tables
 * the parent has there, passing a table's span where it has none: every
 * user page lies in a region, and a walk of the whole of user space would
 * take 64 million steps on x86-64. The ranges registered on a writable
 * private region are told first (MMN_PROTECT), since its pages become
 * read-only in the parent. The parent's pages made read-only are flushed
 * once, at the end, as Linux's fork flushes the parent's whole space once:
 * a write another thread makes meanwhile through the old translation lands
 * in the frame both share, as if it came before the fork.
 */
int paging_fork_copy(struct mm *from, page_dir_t *child_dir)
{
    page_dir_t *parent_dir = from->dir;
    const vma_t *region;
    struct flush_batch b;
    int rc = 0;

    batch_start(&b, parent_dir);
    for (region = vm_first(&from->vm); region && rc == 0; region = vm_next(region)) {
        uintptr_t va, next;

        if (region->prot & (VM_PROT_DONTFORK | VM_PROT_WIPEONFORK)) {
            continue; /* a wiped region's child faults in zero pages */
        }
        if (region->type != VM_SHARED && (region->prot & VM_PROT_WRITE)) {
            mmn_notify(from, MMN_PROTECT, region->start, region->end);
        }
        for (va = region->start; va < region->end; va = next) {
            pte_t *table = user_span(parent_dir, va, region->end, &next);

            for (; table && va < next && rc == 0; va += PAGE_SIZE) {
                rc = fork_copy_page(&b, child_dir, &table[MMU_INDEX(va)], va, region->type);
            }
            if (rc != 0) {
                break;
            }
        }
    }
    batch_flush(&b);
    return rc;
}

uintptr_t paging_enter_kernel(void)
{
    uintptr_t saved = arch_space_root();
    uintptr_t kernel = get_page_dir_phys(_kernel_dir);

    if (saved == kernel) {
        return 0;
    }
    arch_space_load(kernel);
    return saved;
}

void paging_leave_kernel(uintptr_t saved)
{
    if (saved != 0) {
        arch_space_load(saved);
    }
}
