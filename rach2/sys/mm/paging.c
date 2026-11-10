/*
 * paging.c: the frame allocator, the shared code of the page tables and the
 * page fault handler of paging.h.
 */

#include "../arch/arch.h"
#include "../cmdline.h"
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
/* The frames of the window, below HARD_MEMORY_LIMIT, which
 * `alloc_kernel_frame` gives out: frames whose physical address a device can
 * be given. */
static size_t _window_frames;
/* The frames the allocator manages, counted at boot (`frame_total_count`). */
static size_t _managed_frames;
static size_t _free_frames; /* words of the table that are 0, kept by every writer */

static void frame_take(uintptr_t frame);
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
 * given until the next sweep unmaps it. */
static uint32_t _temp_slots[KVA_TEMP_SLOTS / 32];
static uint32_t _temp_stale[KVA_TEMP_SLOTS / 32];
/* The slot the next search for a free one starts at. */
static uint32_t _temp_next;
#endif
/* The pages of the MMIO region, a bit each, set while a window or a
 * mapping of `map_mmio` has the page. */
#define MMIO_PAGES (KVA_MMIO_SIZE / PAGE_SIZE)
static uint32_t _mmio_used[MMIO_PAGES / 32u];
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
    table_bytes = _frames_number * sizeof(uint32_t);
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

    register_isr_handler(PAGE_FAULT_VECTOR, &_page_fault_handler, "page fault");

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
 * Back a page of a region that maps a file: a frame filled from the file and
 * mapped once it is full, writable when the region is. The mapping is
 * private: a store changes the frame, never the file. The fault counts as
 * major when the fill read the page through the file system.
 *
 * The fill may sleep in the disk. The file is held from before the fill, so
 * a thread that unmaps the region meanwhile cannot free it under the read,
 * and the region is looked up again after it: a page another path backed
 * meanwhile, or a region that changed in any way, its protection included,
 * leaves the frame unused, and the resolution runs again against what is
 * there now, so a page is never mapped with rights its region no longer
 * gives. 1 with the page mapped, 0 when no frame could be had or the read
 * failed, -1 to resolve again.
 */
static int file_fault(page_dir_t *dir, vm_space_t *vm, const vma_t *vma, uintptr_t addr)
{
    struct vm_file *f = vma->file;
    uintptr_t page = addr & ~(PAGE_SIZE - 1);
    uint32_t pgoff = vma->pgoff + (uint32_t)((page - vma->start) / PAGE_SIZE);
    uint32_t prot = vma->prot;
    uintptr_t phys = alloc_user_frame();
    int filled;
    int rc;

    if (phys == 0) {
        return 0;
    }
    vm_file_get(f);
    filled = f->fill(f, pgoff, phys);
    rc = filled >= 0 ? 1 : 0;
    vma = vm_find(vm, page);
    if (!vma || vma->file != f || vma->prot != prot ||
        vma->pgoff + (page - vma->start) / PAGE_SIZE != pgoff || pte_present(dir, page)) {
        rc = -1;
    }
    vm_file_put(f);
    if (rc == 1 && map_user_frame(dir, page, phys,
                                  PTE_P | PTE_U | ((prot & VM_PROT_WRITE) ? PTE_W : 0) |
                                      paging_noexec(prot)) == 0) {
        fault_count(filled > 0);
        return 1;
    }
    free_frame(phys);
    return rc < 0 ? -1 : 0;
}

/**
 * Borrow a resident file or shared-memory page. A shared region maps it
 * writable when allowed; a private region maps it copy-on-write and copies
 * immediately when resolving a store. ENXIO raises SIGBUS past EOF.
 *
 * The share of a disk file's pages reads a page from the file the first
 * time, which may sleep, so the file is held across it and the region is
 * looked up again after, as `file_fault` does: a region that changed, or a
 * page backed meanwhile, has the resolution run again. 1 with the page
 * mapped, 0 when no frame could be had, -1 to resolve again.
 */
static int shared_fault(struct mm *mm, const vma_t *vma, uintptr_t addr, int write)
{
    page_dir_t *dir = mm->dir;
    vm_space_t *vm = &mm->vm;
    struct vm_file *f = vma->file;
    int private = vma->type != VM_SHARED;
    int got;
    uintptr_t page = addr & ~(PAGE_SIZE - 1);
    uint32_t pgoff = vma->pgoff + (uint32_t)((page - vma->start) / PAGE_SIZE);
    uint32_t prot = vma->prot;
    uintptr_t phys = 0;
    int rc = 1;

    vm_file_get(f);
    got = f->share(f, pgoff, &phys);
    vma = vm_find(vm, page);
    if (!vma || vma->file != f || vma->prot != prot ||
        vma->pgoff + (page - vma->start) / PAGE_SIZE != pgoff || pte_present(dir, page)) {
        rc = -1;
    }
    vm_file_put(f);
    if (rc == 1 && got == -ENXIO && current_proc) {
        current_proc->bus_fault = 1;
    }
    if (got < 0) {
        return rc < 0 ? -1 : 0;
    }
    if (rc == 1 &&
        map_user_frame(dir, page, phys,
                       PTE_P | PTE_U | (private ? PTE_COW : ((prot & VM_PROT_WRITE) ? PTE_W : 0)) |
                           paging_noexec(prot)) == 0) {
        fault_count(got > 0);
        return private && write ? cow_fault(mm, page) : 1;
    }
    free_frame(phys);
    return rc < 0 ? -1 : 0;
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

/**
 * Back a page of a device region (`vm_device_map`): map the physical
 * address the object gives for it, writable when the region is, with the
 * region's cache type, and take a reference for the mapping, which counts
 * only a frame the allocator counts and which unmapping drops. The object's
 * `page` may sleep, so the file is held across it and the region is looked
 * up again after, as `shared_fault` does. 1 with the page mapped; 0 when the
 * object has no page there, with `bus_fault` set for the fault handler's
 * SIGBUS, when it cannot give the page yet, with `fault_retry` set for the
 * handler's return to user mode, or when no page table could be had; -1 to
 * resolve again.
 */
static int device_fault(page_dir_t *dir, vm_space_t *vm, const vma_t *vma, uintptr_t addr)
{
    struct vm_file *f = vma->file;
    uintptr_t page = addr & ~(PAGE_SIZE - 1);
    uint32_t pgoff = vma->pgoff + (uint32_t)((page - vma->start) / PAGE_SIZE);
    uint32_t prot = vma->prot;
    uintptr_t phys = 0;
    int got;

    vm_file_get(f);
    got = f->device(f, pgoff, &phys);
    vma = vm_find(vm, page);
    if (!vma || vma->file != f || vma->prot != prot ||
        vma->pgoff + (page - vma->start) / PAGE_SIZE != pgoff || pte_present(dir, page)) {
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
    if (map_user_frame(dir, page, phys,
                       PTE_P | PTE_U | ((prot & VM_PROT_WRITE) ? PTE_W : 0) | paging_noexec(prot) |
                           device_cache(prot)) != 0) {
        return 0;
    }
    frame_ref(phys);
    fault_count(0);
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
 * Grow the stack down to `page` for a fault outside every region, as
 * `vm_grow_stack` does under RLIMIT_AS `as_limit`; a locked stack's growth
 * also counts against RLIMIT_MEMLOCK, as Linux's acct_stack_growth checks
 * it. 1 if grown, 0 if not.
 */
static int grow_stack(vm_space_t *vm, uintptr_t page, uintptr_t as_limit)
{
    const vma_t *above = vm_lower(vm, page);

    if (above && above->type == VM_STACK && (above->prot & VM_PROT_LOCKED) && page < above->start &&
        !mem_lock_room(vm_locked(vm) / PAGE_SIZE, (above->start - page) / PAGE_SIZE)) {
        return 0;
    }
    return vm_grow_stack(vm, page, as_limit);
}

/**
 * One try at `vm_resolve_in`: 1 when the page is present and allows the
 * access, 0 when the access is illegal or no frame could be had, and -1
 * when a file's page was read while the regions changed.
 *
 * A page that is there and allows the access needs nothing. Any other needs
 * a frame, or a copy-on-write copy, so the caches give frames back first
 * when memory is short (`mem_balance`), which inside a system call may sleep
 * in a writeback; the region is looked up after that, and nothing from the
 * lookup on sleeps but a file's fill, which checks what it found again. A
 * page of a stack grown here is the stack's: writable and never executable.
 * Each page backed or copied counts as a fault of the current thread
 * (`fault_count`).
 */
static int vm_resolve_once(struct mm *mm, uintptr_t addr, int write, uintptr_t as_limit)
{
    page_dir_t *dir = mm->dir;
    vm_space_t *vm = &mm->vm;
    vma_t *vma = vm_find(vm, addr);
    uintptr_t phys;
    uint32_t prot;

    if (vma && region_allows(vma, write) && pte_present(dir, addr) &&
        (!write || pte_writable(dir, addr))) {
        return 1;
    }
    mem_balance();
    vma = vm_find(vm, addr);
    if (!vma) {
        if (!grow_stack(vm, addr & ~(PAGE_SIZE - 1), as_limit)) {
            return 0;
        }
    } else {
        if (!region_allows(vma, write)) {
            return 0;
        }
        if (pte_present(dir, addr)) {
            if (!write || pte_writable(dir, addr)) {
                return 1;
            }
            if (!cow_fault(mm, addr)) {
                return 0;
            }
            fault_count(0);
            return 1;
        }
        if (vma->prot & VM_PROT_IO) {
            return vma->file && vma->file->device ? device_fault(dir, vm, vma, addr) : 0;
        }
        if (vma->file) {
            return vma->file->share ? shared_fault(mm, vma, addr, write)
                                    : file_fault(dir, vm, vma, addr);
        }
    }

    prot = vma ? vma->prot : VM_PROT_READ | VM_PROT_WRITE;
    phys = alloc_user_frame();
    if (phys == 0) {
        return 0;
    }
    if (paging_zero_frame(phys) != 0 ||
        map_user_frame(dir, addr, phys,
                       PTE_P | PTE_U | ((prot & VM_PROT_WRITE) ? PTE_W : 0) |
                           paging_noexec(prot)) != 0) {
        free_frame(phys);
        return 0;
    }
    fault_count(0);
    return 1;
}

/**
 * Decide whether an access at `addr`, which need not be aligned, is legal in
 * the address space `mm` and, if so, make sure the page is backed. `write`
 * is 1 for a write, and `as_limit` is the RLIMIT_AS a grown stack is held
 * to.
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
 * is, so a later write faults; a grown stack page stays writable. A file's
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
    int rc;

    do {
        rc = vm_resolve_once(mm, addr, write, as_limit);
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

/** `vm_resolve_in` for the current process's address space. */
static int vm_resolve(uintptr_t addr, int write)
{
    return vm_resolve_in(current_proc->mm, addr, write, current_proc->rlim[RLIM_AS].cur);
}

/** Whether every page of [start, end) is present in `mm` and allows the
 * access, found with nothing that sleeps. */
static int range_ready(struct mm *mm, uintptr_t start, uintptr_t end, int write)
{
    uintptr_t page;

    for (page = start & ~(PAGE_SIZE - 1); page < end; page += PAGE_SIZE) {
        const vma_t *vma = vm_find(&mm->vm, page);

        if (!vma || !region_allows(vma, write) || !pte_present(mm->dir, page) ||
            (write && !pte_writable(mm->dir, page))) {
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
 * and ends past it is not resident.
 */
int vm_resident(uintptr_t addr, uintptr_t len, int write)
{
    uintptr_t end = addr + len;

    if (len == 0 || (current_proc && proc_window_covers(current_proc, addr, len))) {
        return 1;
    }
    if (addr < USER_ADDR_MIN || addr >= USER_ADDR_MAX) {
        return 1;
    }
    if (end < addr || end > USER_ADDR_MAX || !current_proc || !current_proc->mm) {
        return 0;
    }
    return range_ready(current_proc->mm, addr, end, write);
}

/**
 * The resolution of one page may sleep, reading a file or writing a cache
 * back, and another thread may meanwhile unmap or protect a page already
 * brought in, which the kernel would then touch and fault on with no way to
 * recover. So once every page is in, the range is checked again with
 * nothing that sleeps, and brought in again when a page went. A range that
 * wraps is refused, since the loop would end early and leave its tail
 * unbacked. A kernel buffer the thread opened to the checks
 * (`user_window_open`) is present already.
 */
int vm_prefault_range(uintptr_t addr, uintptr_t len, int write)
{
    uintptr_t page, end;

    if (!current_proc || current_proc->mm == NULL) {
        return 0;
    }
    if (len == 0 || proc_window_covers(current_proc, addr, len)) {
        return 1;
    }

    end = addr + len;
    if (end < addr) {
        return 0;
    }
    do {
        for (page = addr & ~(PAGE_SIZE - 1); page < end; page += PAGE_SIZE) {
            if (!vm_resolve(page, write)) {
                return 0;
            }
        }
    } while (!range_ready(current_proc->mm, addr, end, write));
    return 1;
}

/**
 * A region that is private and writable has its pages brought in for a write,
 * which gives each its own frame now, as Linux's populate breaks the sharing
 * of copy-on-write pages; any other for a read, so that a shared page is not
 * made dirty for nothing. A region with no access, a guard or device memory
 * is passed over, as Linux's populate passes a VM_IO region over. Each
 * page brought in counts as a fault (`fault_count`), as Linux counts its
 * populate's.
 */
int vm_populate(uintptr_t start, uintptr_t end)
{
    uintptr_t page;

    if (!current_proc || !current_proc->mm) {
        return -1;
    }
    for (page = start & ~(PAGE_SIZE - 1); page < end; page += PAGE_SIZE) {
        const vma_t *vma = vm_find(&current_proc->mm->vm, page);
        int write;

        if (!vma || !(vma->prot & VM_PROT_READ) || (vma->prot & (VM_PROT_IO | VM_PROT_GUARD))) {
            continue;
        }
        write = (vma->prot & VM_PROT_WRITE) && vma->type != VM_SHARED;
        if (!vm_resolve(page, write)) {
            return -1;
        }
    }
    return 0;
}

/** Whether the region allows the access and, with `io`, device memory. */
static int pin_allows(struct mm *mm, uintptr_t page, int write, int io)
{
    const vma_t *vma = vm_find(&mm->vm, page);

    return vma && (io || !(vma->prot & VM_PROT_IO)) && region_allows(vma, write);
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
 * pinned. With `io`, device pages outside the allocator are kept without
 * pins; `ram_pages` counts only the RAM frames that were pinned.
 */
static int pin_range(struct mm *mm, uintptr_t start, uintptr_t end, uint32_t flags,
                     uintptr_t *frames, uint32_t *ram_pages)
{
    uintptr_t as_limit = current_proc ? current_proc->rlim[RLIM_AS].cur : RLIM_INFINITY;
    uintptr_t page;
    uint32_t i = 0, ram = 0;
    int write = (flags & PIN_WRITE) != 0, io = (flags & PIN_IO) != 0;
    int rc = -ENOMEM;

    do {
        for (page = start; page < end; page += PAGE_SIZE) {
            if (!pin_allows(mm, page, write, io)) {
                return -EFAULT;
            }
            rc = pin_resolve(mm, page, write, as_limit);
            if (rc != 0) {
                return pin_allows(mm, page, write, io) ? rc : -EFAULT;
            }
        }
    } while (!range_ready(mm, start, end, write));
    rc = -ENOMEM;
    for (page = start; page < end; page += PAGE_SIZE, i++) {
        const vma_t *vma = vm_find(&mm->vm, page);
        pte_t *e = user_pte(mm->dir, page);

        if (!pin_allows(mm, page, write, io)) {
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

int paging_pin_range(struct mm *mm, uintptr_t start, uintptr_t end, int write, uintptr_t *frames)
{
    uint32_t ram;

    return pin_range(mm, start, end, write ? PIN_WRITE : 0u, frames, &ram);
}

int paging_dma_range(struct mm *mm, uintptr_t start, uintptr_t end, int write, uintptr_t *frames,
                     uint32_t *ram_pages)
{
    return pin_range(mm, start, end, PIN_IO | (write ? PIN_WRITE : 0u), frames, ram_pages);
}

/**
 * Resolve a write to the present copy-on-write (PTE_COW) page at `addr` of
 * `mm`. When this address space holds the frame's last mapping, the page is
 * made writable in place and loses its mark, whatever pins the frame has;
 * otherwise a writable copy of its own takes its place, the ranges
 * registered on the page are told (MMN_COW), and the shared frame's
 * reference is dropped. The caller balanced memory before it looked at the
 * page, so nothing here sleeps and the entry read is the one replaced.
 * Returns 1 if it resolved the page, 0 if the page is not present or not
 * copy-on-write or no frame could be had or copied for it.
 */
static int cow_fault(struct mm *mm, uintptr_t addr)
{
    page_dir_t *dir = mm->dir;
    pte_t *e = user_pte(dir, addr);
    pte_t pte;
    uintptr_t old_phys, frame;
    uintptr_t new_phys;

    if (!e) {
        return 0;
    }
    pte = *e;
    if (!(pte & PTE_P) || !(pte & PTE_COW)) {
        return 0;
    }

    old_phys = PTE_ADDR(pte);
    frame = old_phys / PAGE_SIZE;

    if (frame >= _frames_number || frame_maps(frame) <= 1) {
        *e = (pte & ~(pte_t)PTE_COW) | PTE_W;
        paging_flush_dir(dir, addr);
        return 1;
    }

    new_phys = alloc_user_frame();
    if (new_phys == 0) {
        return 0;
    }
    if (paging_copy_frame(new_phys, old_phys) != 0) {
        free_frame(new_phys);
        return 0;
    }
    addr &= ~(uintptr_t)(PAGE_SIZE - 1);
    mmn_notify(mm, MMN_COW, addr, addr + PAGE_SIZE);
    *e = (pte_t)new_phys | PTE_P | PTE_W | PTE_U | (pte & PTE_NOEXEC);
    paging_flush_dir(dir, addr);
    frame_unref(old_phys);
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
 * and the page loses its copy-on-write mark. The caller balanced memory
 * first, so nothing here sleeps. 0, or -1 when no frame could be had or
 * copied.
 */
static int page_private(struct mm *mm, uintptr_t addr, int writable)
{
    page_dir_t *dir = mm->dir;
    pte_t *e = user_pte(dir, addr);
    pte_t pte;
    uintptr_t old_phys;
    uintptr_t new_phys;

    if (!e || !((pte = *e) & PTE_P)) {
        return -1;
    }
    if ((pte & PTE_COW) && writable) {
        return cow_fault(mm, addr) ? 0 : -1;
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
        addr &= ~(uintptr_t)(PAGE_SIZE - 1);
        mmn_notify(mm, MMN_COW, addr, addr + PAGE_SIZE);
    }
    *e = (pte_t)new_phys | (pte & PTE_FLAGS & ~(pte_t)PTE_COW);
    paging_flush_dir(dir, addr);
    if (new_phys != old_phys) {
        frame_unref(old_phys);
    }
    return 0;
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
 * device region's bytes go through `access_device`. The resolution and,
 * for a write, the balance of memory before a copy may sleep, so the
 * region is looked up again after them, and the page must still be present
 * in a region that allows some access. */
static int access_page(struct mm *mm, uintptr_t addr, uint8_t *p, uint32_t len, int write)
{
    const vma_t *vma = vm_find(&mm->vm, addr);
    uint8_t *map;

    if (addr < USER_ADDR_MIN || addr >= USER_ADDR_MAX || vma == NULL ||
        !(vma->prot & (VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXEC)) ||
        (vma->prot & VM_PROT_GUARD)) {
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
    if (write) {
        mem_balance();
    }
    vma = vm_find(&mm->vm, addr);
    if (vma == NULL || !(vma->prot & (VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXEC)) ||
        (vma->prot & (VM_PROT_IO | VM_PROT_GUARD)) || !pte_present(mm->dir, addr)) {
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
    vm_file_reap();
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
 * Signal the current process for its user fault `params` at `addr` that no
 * resolution made legal, a `write` or a read: with `bus`, SIGBUS with
 * BUS_ADRERR, for a device region whose object had no page there; else SIGSEGV,
 * SEGV_ACCERR when a region holds the address and SEGV_MAPERR when none does or
 * a guard does, as Linux answers a guard's fault. Either is forced as Linux
 * forces it, and the death is logged only when no handler and no tracer will
 * see a SIGSEGV.
 */
static void fault_signal(const isr_params_t *params, uintptr_t addr, int write, int bus)
{
    struct k_siginfo info;
    int signo = bus ? SIGBUS : SIGSEGV;

    k_memset(&info, 0, sizeof(info));
    if (bus) {
        info.code = BUS_ADRERR;
    } else {
        const vma_t *vma = vm_find(&current_proc->mm->vm, addr);

        info.code = vma && !(vma->prot & VM_PROT_GUARD) ? SEGV_ACCERR : SEGV_MAPERR;
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

/**
 * Resolve a page fault, or end what caused it.
 *
 * Tried in order. Any fault at a user-space address of a live user process
 * goes to `vm_resolve`, but an instruction fetch from a region without
 * execute (`fetch_refused`); the test is of the address and not of the
 * processor's mode, so that a kernel touch of a user page not yet backed
 * resolves too. A write to a page shared by a fork reaches `cow_fault`
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
 * process with SIGKILL, which no handler sees, as Linux's OOM killer does;
 * a SIGSEGV handler that returned would fault again. Any other user fault
 * is SIGSEGV with the address, SEGV_ACCERR when a region holds it and
 * SEGV_MAPERR when none does, forced as Linux forces it: a handler runs and
 * the faulting instruction runs again after it returns, and a tracer takes
 * the stop; the death is logged only when no handler and no tracer will see
 * the signal.
 *
 * What is left panics: the usercopy helpers return -EFAULT before the
 * kernel touches a bad user pointer, so a kernel-mode fault here, or one
 * with no user context, is a kernel bug. One in the guard page below the
 * running thread's kernel stack is that stack running out, and the panic
 * says so. Most overruns do not reach this handler: the fault's own frame
 * would be pushed into the same guard, and the processor raises a double
 * fault instead (`isr_double_fault`).
 */
static void _page_fault_handler(isr_params_t *params)
{
    uintptr_t fault_address;
    int write, user_mode, have_user_proc, in_user_range, pid;

    fault_address = arch_fault_address();

    write = FAULT_WRITE(params);
    user_mode = FAULT_USER(params);
    have_user_proc = (current_proc != NULL && current_proc->mm != NULL);
    in_user_range = (fault_address >= USER_ADDR_MIN && fault_address < USER_ADDR_MAX);

    if (have_user_proc && in_user_range) {
        current_proc->frame_refused = 0;
        current_proc->bus_fault = 0;
        current_proc->fault_retry = 0;
        if (!fetch_refused(fault_address, params) && vm_resolve(fault_address, write)) {
            return; /* page mapped; iret retries the faulting instruction */
        }
        if (user_mode && current_proc->fault_retry) {
            return; /* the signals are handled, then the instruction runs again */
        }
    }

    if (!user_mode && fault_address >= PCWIN_BASE &&
        fault_address < PCWIN_BASE + pc_window_size()) {
        if (!current_proc) {
            k_panic("pagecache: window fault with no current process");
        }
        if (pc_fault(fault_address, write)) {
            return;
        }
        k_panic("pagecache: unrecoverable disk fault");
    }

    if (user_mode && have_user_proc && in_user_range && current_proc->frame_refused) {
        struct k_siginfo info;

        k_memset(&info, 0, sizeof(info));
        info.code = SI_KERNEL;
        klog(KLOG_WARN, "vm", "fault: out of memory: killing pid %d: no frame for a %s at 0x%lx",
             current_proc->pid, write ? "write" : "read", (unsigned long)fault_address);
        signal_force(current_proc, SIGKILL, &info);
        return;
    }

    if (user_mode && have_user_proc) {
        fault_signal(params, fault_address, write, in_user_range && current_proc->bus_fault);
        return;
    }

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

    k_panic("Unrecoverable error: page fault while in kernel mode.");
}

uintptr_t map_mmio(uintptr_t phys_addr, size_t size)
{
    return map_mmio_typed(phys_addr, size, PTE_NOCACHE);
}

/** The bit of page `i` of the MMIO region in `_mmio_used`. */
static int mmio_bit(uintptr_t i)
{
    return (_mmio_used[i / 32u] >> (i % 32u)) & 1u;
}

/** Mark the `count` pages of the MMIO region from page `i` used, or free
 * when `used` is 0. */
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
 * rest a page at a time. */
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
 * window `mmio_unmap` frees is used again.
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
    first = mmio_find(pages, align, phase);
#if KVA_MMIO_LARGE
    if (first >= MMIO_PAGES && align > 1) {
        first = mmio_find(pages, 1, 0); /* any run, in 4 KiB pages */
    }
#endif
    if (first >= MMIO_PAGES) {
        klog(KLOG_ERR, "vm", "map_mmio: no run of %lu pages free in the MMIO region",
             (unsigned long)pages);
        return 0;
    }
    mmio_mark(first, pages, 1);
    mmio_fill(first, pages, start_page, PTE_P | PTE_W | PTE_NOEXEC | cache);

    /* Every translation the processor cached goes. */
    arch_space_load(arch_space_root());
    smp_kernel_mapping_changed();

    return KVA_MMIO + first * PAGE_SIZE + (uintptr_t)(phys - start_page);
}

uintptr_t mmio_map_frames(const uintptr_t *frames, uintptr_t count, pte_t cache)
{
    uintptr_t first, i, va;

    if (count == 0 || count > MMIO_PAGES) {
        klog(KLOG_ERR, "vm", "map_mmio: bad list of %lu frames", (unsigned long)count);
        return 0;
    }
    first = mmio_find(count, 1, 0);
    if (first >= MMIO_PAGES) {
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
    return va;
}

void mmio_unmap(uintptr_t va, size_t size)
{
    uintptr_t lo = va & ~(uintptr_t)(PAGE_SIZE - 1);
    uintptr_t hi = va + size;

    if (size == 0 || lo < KVA_MMIO || hi < va || hi > KVA_MMIO + KVA_MMIO_SIZE) {
        return;
    }
    hi = (hi + PAGE_SIZE - 1) & ~(uintptr_t)(PAGE_SIZE - 1); /* the region's end is aligned */
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
}

size_t mmio_page_size(uintptr_t va)
{
    pte_t *e;

    if (va < KVA_MMIO || va - KVA_MMIO >= KVA_MMIO_SIZE) {
        return 0;
    }
#if KVA_MMIO_LARGE
    {
        uintptr_t slot = (va - KVA_MMIO) / MMU_TABLE_SPAN;

        if (_mmio_large[slot / 32u] & (1u << (slot % 32u))) {
            return MMU_TABLE_SPAN;
        }
    }
#endif
    e = kernel_pte(va);
    return e && (*e & PTE_P) ? PAGE_SIZE : 0;
}

void mmio_usage(size_t *used, size_t *largest)
{
    uintptr_t i, run = 0, best = 0, taken = 0;

    for (i = 0; i < MMIO_PAGES; i++) {
        if (mmio_bit(i)) {
            taken++;
            run = 0;
        } else if (++run > best) {
            best = run;
        }
    }
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
#else
/**
 * The first slot from `_temp_next` that is neither held nor stale, or
 * `KVA_TEMP_SLOTS` when there is none. Interrupts are off.
 *
 * Such a slot has been unmapped since every processor that holds the big
 * lock last dropped its translations, so none that runs the caller has one
 * for it and its new entry needs no flush.
 */
static uint32_t temp_find(void)
{
    uint32_t i;

    for (i = 0; i < KVA_TEMP_SLOTS; i++) {
        uint32_t slot = (_temp_next + i) % KVA_TEMP_SLOTS;
        uint32_t bit = 1u << (slot % 32);

        if (!((_temp_slots[slot / 32] | _temp_stale[slot / 32]) & bit)) {
            _temp_next = (slot + 1) % KVA_TEMP_SLOTS;
            return slot;
        }
    }
    return KVA_TEMP_SLOTS;
}

/**
 * Unmap every stale slot and drop this processor's translations, and have
 * every other drop its own before it next takes the big lock: one change of
 * kernel mappings for all the slots given back since the last sweep, where
 * unmapping each as it was given back would be one change for each.
 * Interrupts are off.
 */
static void temp_sweep(void)
{
    uint32_t slot;

    for (slot = 0; slot < KVA_TEMP_SLOTS; slot++) {
        if (_temp_stale[slot / 32] & (1u << (slot % 32))) {
            *kernel_pte(KVA_TEMP + slot * PAGE_SIZE) = 0;
        }
    }
    k_memset(_temp_stale, 0, sizeof _temp_stale);
    arch_space_load(arch_space_root());
    smp_kernel_mapping_changed();
}

void *kmap_temp(uintptr_t phys)
{
    uint32_t flags = kirq_save();
    uint32_t slot = temp_find();
    uintptr_t va;

    if (slot == KVA_TEMP_SLOTS) {
        temp_sweep();
        slot = temp_find();
    }
    if (slot == KVA_TEMP_SLOTS) {
        kirq_restore(flags);
        klog(KLOG_ERR, "vm", "temporary mappings: every slot is held");
        return 0;
    }
    va = KVA_TEMP + slot * PAGE_SIZE;
    _temp_slots[slot / 32] |= 1u << (slot % 32);
    *kernel_pte(va) = (pte_t)(phys & ~(PAGE_SIZE - 1)) | PTE_P | PTE_W;
    kirq_restore(flags);
    return (void *)(va + (phys & (PAGE_SIZE - 1)));
}

void kunmap_temp(void *va)
{
    uintptr_t addr = (uintptr_t)va & ~(PAGE_SIZE - 1);
    uint32_t slot = (uint32_t)((addr - KVA_TEMP) / PAGE_SIZE);
    uint32_t flags;

    if (addr < KVA_TEMP || slot >= KVA_TEMP_SLOTS) {
        return;
    }
    flags = kirq_save();
    _temp_slots[slot / 32] &= ~(1u << (slot % 32));
    _temp_stale[slot / 32] |= 1u << (slot % 32);
    kirq_restore(flags);
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
static struct spinlock frame_lock = SPINLOCK_INIT;

/** Mark `frame` used with one reference, whatever it held, and keep the
 * free counts. `frame_lock` is held, or the boot runs alone. */
static void frame_take(uintptr_t frame)
{
    if (_frames[frame] == 0) {
        _free_frames--;
        if (frame < _window_frames) {
            _free_window_frames--;
        }
    }
    _frames[frame] = 1;
}

/**
 * A free frame for a user page or a kernel object: from above the window
 * first, then from below it while more than WINDOW_KEEP are free there;
 * `_frames_number` when there is none. The two hints say no frame below
 * each is free in its region. `frame_lock` is held.
 */
static uintptr_t find_free_frame(void)
{
    uintptr_t frame;

    for (frame = _next_free_upper; frame < _frames_number; frame++) {
        if (_frames[frame] == 0) {
            _next_free_upper = frame + 1;
            return frame;
        }
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
    for (frame = _next_free_frame; frame < _window_frames; frame++) {
        if (_frames[frame] == 0) {
            _next_free_frame = frame + 1;
            return frame;
        }
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

void free_frame(uintptr_t phys)
{
    frame_unref(phys);
}

int paging_frame_is_device(uintptr_t phys)
{
    uintptr_t frame = phys / PAGE_SIZE;

    return frame >= _frames_number || _frames[frame] >= FRAME_REF_MAX;
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

void paging_user_count(page_dir_t *dir, uintptr_t start, uintptr_t end, unsigned long *present,
                       unsigned long *dirty)
{
    uintptr_t va, next;

    *present = *dirty = 0;
    for (va = start; va < end; va = next) {
        pte_t *table = user_span(dir, va, end, &next);

        for (; table && va < next; va += PAGE_SIZE) {
            pte_t e = table[MMU_INDEX(va)];

            if (e & PTE_P) {
                (*present)++;
                *dirty += (e & PTE_DIRTY) != 0;
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
    uintptr_t frame = _window_frames;

    while (frame-- > _user_frames_start) {
        if (_frames[frame] == 0) {
            frame_take(frame);
            _kernel_frames_reserved++;
            spin_unlock_irqrestore(&frame_lock, flags);
            return phys_to_virt(frame * PAGE_SIZE);
        }
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

void map_kernel_frame(uintptr_t va, uintptr_t phys, int writable)
{
    pte_t *e = kernel_pte(va);

    if (!e) {
        return;
    }
    *e = (pte_t)(phys & ~(PAGE_SIZE - 1)) | PTE_P | (writable ? PTE_W : 0);
    paging_flush_tlb(va);
}

void unmap_kernel_frame(uintptr_t va)
{
    pte_t *e = kernel_pte(va);

    if (!e) {
        return;
    }
    *e = 0;
    paging_flush_tlb(va);
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

pte_t paging_noexec(uint32_t prot)
{
    pte_t bit = PTE_NOEXEC;

    if (prot & VM_PROT_EXEC) {
        bit = 0;
    }
    return bit;
}

void paging_flush_tlb(uintptr_t addr)
{
    paging_flush_dir(_curr_dir, addr);
}

void paging_flush_dir(const page_dir_t *dir, uintptr_t addr)
{
    if (addr < USER_ADDR_MIN || addr >= USER_ADDR_MAX) {
        arch_tlb_flush_page(addr);
        smp_kernel_mapping_changed();
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

    old = table[MMU_INDEX(virt_addr)];
    table[MMU_INDEX(virt_addr)] = (pte_t)phys_addr | PTE_P | PTE_W | PTE_U;
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
 * unmap costs one shootdown, not one a page. The big lock is held
 * throughout, and no processor writes an entry that is not present, so
 * nothing else sees the marked entries.
 */
void unmap_user_range(page_dir_t *dir, uintptr_t start, uintptr_t end)
{
    struct flush_batch b;
    uintptr_t addr, next, lo, hi;

    if (!dir) {
        return;
    }
    batch_start(&b, dir);
    for (addr = start; addr < end; addr = next) {
        pte_t *table = user_span(dir, addr, end, &next);

        for (; table && addr < next; addr += PAGE_SIZE) {
            pte_t *e = &table[MMU_INDEX(addr)];

            if (*e & PTE_P) {
                *e = PTE_ADDR(*e) | PTE_NONE;
                batch_add(&b, addr);
            }
        }
    }
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
}

/**
 * Each thread of a process has its own record on the process list and the
 * address space of its process, so a space shared by threads is walked
 * once for each of them, the later walks finding nothing to unmap.
 */
void paging_zap_device(const void *obj, uint32_t pgoff, uint32_t count)
{
    process_t *p;
    const vma_t *r;
    uint32_t end = count > 0xFFFFFFFFu - pgoff ? 0xFFFFFFFFu : pgoff + count;

    if (!obj) {
        return;
    }
    for (p = proc_list; p; p = p->next) {
        const vm_space_t *vm = p->mm ? &p->mm->vm : NULL;

        for (r = vm ? vm_first(vm) : NULL; r; r = vm_next(r)) {
            uint32_t last = r->pgoff + (uint32_t)((r->end - r->start) / PAGE_SIZE);
            uint32_t from = r->pgoff > pgoff ? r->pgoff : pgoff;
            uint32_t to = last < end ? last : end;

            if (from < to && vm_device_object(r->file) == obj) {
                uintptr_t lo = r->start + (uintptr_t)(from - r->pgoff) * PAGE_SIZE;
                uintptr_t hi = r->start + (uintptr_t)(to - r->pgoff) * PAGE_SIZE;

                mmn_notify(p->mm, MMN_TRUNCATE, lo, hi);
                unmap_user_range(p->mm->dir, lo, hi);
            }
        }
    }
}

/** The tables of the new range are made first, so that the move cannot
 * stop half way. */
int paging_move_range(page_dir_t *dir, uintptr_t from, uintptr_t to, uintptr_t len)
{
    uintptr_t addr, next, end = from + len;
    struct flush_batch moved, replaced;
    pte_t *table;

    for (addr = from; addr < end; addr = next) {
        for (table = user_span(dir, addr, end, &next); table && addr < next; addr += PAGE_SIZE) {
            if (table[MMU_INDEX(addr)] && !mmu_user_table(dir, to + (addr - from), 1)) {
                return -1;
            }
        }
    }
    batch_start(&moved, dir);
    batch_start(&replaced, dir);
    for (addr = from; addr < end; addr = next) {
        for (table = user_span(dir, addr, end, &next); table && addr < next; addr += PAGE_SIZE) {
            pte_t *e = &table[MMU_INDEX(addr)];

            if (*e) {
                pte_t *dst = user_pte(dir, to + (addr - from));

                if (*dst & PTE_P) {
                    batch_add(&replaced, to + (addr - from));
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
        pte = (pte & ~(pte_t)(PTE_NONE | PTE_NOEXEC)) | PTE_U | paging_noexec(prot);
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

void paging_protect_range(page_dir_t *dir, uintptr_t start, uintptr_t end, uint32_t prot)
{
    struct flush_batch b;
    uintptr_t addr, next;
    pte_t *table;

    batch_start(&b, dir);
    for (addr = start; addr < end; addr = next) {
        for (table = user_span(dir, addr, end, &next); table && addr < next; addr += PAGE_SIZE) {
            protect_page(&b, &table[MMU_INDEX(addr)], addr, prot);
        }
    }
    batch_flush(&b);
}

/**
 * Give the child's directory `child_dir` the page of the parent, the
 * directory in use, whose entry is `*e`, at `va` in a region of type `type`,
 * as `paging_fork_copy` describes: 0, or -1 with no memory for a table or a
 * copy. A page is given only when it is present, user or PROT_NONE, and
 * names a frame other than 0. A writable private page is made read-only and
 * copy-on-write in the parent too, so that the first write on either side
 * faults into `cow_fault`. A private page pinned for a device (`frame_pin`)
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
                           PTE_P | PTE_U | PTE_COW | (pte & PTE_NOEXEC)) != 0) {
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

        if (region->prot & VM_PROT_DONTFORK) {
            continue;
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
