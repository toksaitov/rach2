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
#include "pressure.h"

page_dir_t *_kernel_dir;

uint32_t *_frames;
size_t _frames_number;
/* The frames below the identity-mapped window, which `alloc_kernel_frame`
 * hands out: a frame the kernel reaches through its physical address. */
static size_t _window_frames;
static size_t _managed_frames;
static size_t _free_frames; /* words of the table that are 0, kept by every writer */

static void frame_take(uintptr_t frame);
/* Where the next scan for a free frame starts: no frame below it is free.
 * A scan from the bottom of a million-frame table for every page would be
 * the table's length squared over a large mapping. */
static uintptr_t _next_free_frame;
/* The hint for the frames above the identity-mapped window, which are
 * taken first: the window's frames are the only ones a device can be given
 * the address of, so the last WINDOW_KEEP of them are left to the requests
 * and the page cache, which take from the window's top. */
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
static int _framefail_on = -1;

/* Whether this allocation is the one armed to fail. */
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
/* The next free page of the MMIO region; a region is mapped for good. */
static uintptr_t _mmio_next = KVA_MMIO;

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
 * Build the frame table and the kernel directory, turn paging on, and start
 * the heap.
 *
 * The frame table has one word per frame below the map's top, and the
 * architecture places it (`mmu_frame_table`): below the window with the
 * other boot tables on i686, at the top of memory on x86-64. A map with
 * nothing usable gets the window, so a machine whose firmware gives no map
 * still boots. On i686 the boot allocator placed the table before the map
 * could be consulted, so a table that lies over memory the map does not
 * offer would overwrite firmware's memory, and the machine is refused by
 * name in the memory report and stops rather than run on it: on a machine
 * too small for the kernel, its modules and the table, this is where the
 * table lands. The table's frames are reserved in the map, and a frame the
 * map does not offer is marked `FRAME_ABSENT`.
 *
 * The architecture then builds the kernel's directory (`mmu_init`), marking
 * the frames below the break it passes, and every frame below
 * `mem_reserved_ptr` and of the table is marked used once more, for the
 * tables carved at frames the architecture had already passed and for those
 * carved after it. The map has the same ranges reserved, and what the
 * allocator manages and has free is counted from the table and not from the
 * map's arithmetic, so the boot's report compares the two. The heap is an
 * eighth of what the allocator manages (`kheap_size_for`).
 */
void init_paging(struct frame_map *map)
{
    uintptr_t frame, first, last;
    size_t table_bytes;
    uint32_t i;

    /* frame.h mirrors these two values so it can stay host-linkable; fail
     * loudly if the mirror ever drifts from the real definitions. */
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

    /* From here the frame allocator is counted, and every page table made
     * later comes from it or from the heap, the heap's own among them. */
    mmu_heap_ready();
    kheap_init(kheap_size_for((uint64_t)_managed_frames * PAGE_SIZE, KVA_HEAP_SIZE));
}

void frame_take_boot(uintptr_t frame)
{
    if (_frames != NULL && frame < _frames_number) {
        frame_take(frame);
    }
}

static int cow_fault(page_dir_t *dir, uintptr_t addr);

/* The entry of `dir`'s own table for the user address `addr`, or NULL where
 * there is no table. */
static pte_t *user_pte(page_dir_t *dir, uintptr_t addr)
{
    pte_t *table = mmu_user_table(dir, addr, 0);

    return table ? &table[MMU_INDEX(addr)] : NULL;
}

/* The kernel directory's entry for the kernel address `addr`, or NULL. */
static pte_t *kernel_pte(uintptr_t addr)
{
    pte_t *table = mmu_kernel_table(addr);

    return table ? &table[MMU_INDEX(addr)] : NULL;
}

/**
 * Is the page containing `addr` writable in `dir`? Present and write-enabled;
 * a present copy-on-write page answers 0, which is what tells the resolver to
 * make it private before promising a caller it may write there.
 */
static int pte_writable(page_dir_t *dir, uintptr_t addr)
{
    pte_t *e = user_pte(dir, addr);

    return e && (*e & (PTE_P | PTE_W)) == (PTE_P | PTE_W);
}

/**
 * Is the page containing `addr` already present in `dir`? Keeps the
 * demand-paging resolver idempotent: an eager-loaded code/data page, or one
 * the pre-fault path already backed, must not be allocated a second time
 * (which would leak the old frame and wipe the page's contents).
 */
static int pte_present(page_dir_t *dir, uintptr_t addr)
{
    pte_t *e = user_pte(dir, addr);

    return e && (*e & PTE_P) != 0;
}

/* Take back the page `alloc_user_page` mapped at `addr` in `dir`: the
 * entry cleared and the frame given back. For a page that could not be
 * made ready. */
static void unmap_user_page_frame(page_dir_t *dir, uintptr_t addr)
{
    pte_t *e = user_pte(dir, addr);
    pte_t pte;

    if (!e) {
        return;
    }
    pte = *e;
    if (!(pte & PTE_P)) {
        return;
    }
    *e = 0;
    paging_flush_dir(dir, addr);
    frame_unref(PTE_ADDR(pte));
}

/**
 * Back a page of a region that maps a file: a frame filled from the file and
 * mapped once it is full, writable when the region is. The mapping is
 * private: a store changes the frame, never the file.
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
    int rc;

    if (phys == 0) {
        return 0;
    }
    vm_file_get(f);
    rc = f->fill(f, pgoff, phys) == 0 ? 1 : 0;
    vma = vm_find(vm, page);
    if (!vma || vma->file != f || vma->prot != prot ||
        vma->pgoff + (page - vma->start) / PAGE_SIZE != pgoff || pte_present(dir, page)) {
        rc = -1;
    }
    vm_file_put(f);
    if (rc == 1 && map_user_frame(dir, page, phys,
                                  PTE_P | PTE_U | ((prot & VM_PROT_WRITE) ? PTE_W : 0) |
                                      paging_noexec(prot)) == 0) {
        return 1;
    }
    free_frame(phys);
    return rc < 0 ? -1 : 0;
}

/**
 * Back a page of a shared region: map the frame the region's file or shared
 * memory has for it, writable when the region is. The frame is the same one
 * every mapping of that page maps, so a store is seen by all of them.
 *
 * The share of a disk file's pages reads a page from the file the first
 * time, which may sleep, so the file is held across it and the region is
 * looked up again after, as `file_fault` does: a region that changed, or a
 * page backed meanwhile, has the resolution run again. 1 with the page
 * mapped, 0 when no frame could be had, -1 to resolve again.
 */
static int shared_fault(page_dir_t *dir, vm_space_t *vm, const vma_t *vma, uintptr_t addr)
{
    struct vm_file *f = vma->file;
    uintptr_t page = addr & ~(PAGE_SIZE - 1);
    uint32_t pgoff = vma->pgoff + (uint32_t)((page - vma->start) / PAGE_SIZE);
    uint32_t prot = vma->prot;
    uintptr_t phys;
    int rc = 1;

    vm_file_get(f);
    phys = f->share(f, pgoff);
    vma = vm_find(vm, page);
    if (!vma || vma->file != f || vma->prot != prot ||
        vma->pgoff + (page - vma->start) / PAGE_SIZE != pgoff || pte_present(dir, page)) {
        rc = -1;
    }
    vm_file_put(f);
    if (phys == 0) {
        return rc < 0 ? -1 : 0;
    }
    if (rc == 1 && map_user_frame(dir, page, phys,
                                  PTE_P | PTE_U | ((prot & VM_PROT_WRITE) ? PTE_W : 0) |
                                      paging_noexec(prot)) == 0) {
        return 1;
    }
    free_frame(phys);
    return rc < 0 ? -1 : 0;
}

/* Whether the region `vma` allows the access: a write needs write, and a
 * read needs read, which write and execute imply. */
static int region_allows(const vma_t *vma, int write)
{
    return (vma->prot & (write ? VM_PROT_WRITE : VM_PROT_READ)) != 0;
}

/**
 * One try at `vm_resolve_in`: 1 when the page is present and allows the
 * access, 0 when the access is illegal or no frame could be had, and -1
 * when a file's page was read while the regions changed.
 *
 * A page that is there and allows the access needs nothing. Any other asks
 * for a frame, or a copy-on-write copy, so the caches give frames back first
 * when memory is short (`mem_balance`), which inside a system call may sleep
 * in a writeback; the region is looked up after that, and nothing from the
 * lookup on sleeps but a file's fill, which checks what it found again.
 */
static int vm_resolve_once(struct mm *mm, uintptr_t addr, int write, uintptr_t as_limit)
{
    page_dir_t *dir = mm->dir;
    vm_space_t *vm = &mm->vm;
    vma_t *vma = vm_find(vm, addr);
    uintptr_t phys;

    if (vma && region_allows(vma, write) && pte_present(dir, addr) &&
        (!write || pte_writable(dir, addr))) {
        return 1;
    }
    mem_balance();
    vma = vm_find(vm, addr);
    if (!vma) {
        if (!vm_grow_stack(vm, addr & ~(PAGE_SIZE - 1), as_limit)) {
            return 0;
        }
        /* A grown stack page is read+write; fall through to back it. */
    } else {
        if (!region_allows(vma, write)) {
            return 0;
        }
        if (pte_present(dir, addr)) {
            return !write || pte_writable(dir, addr) ? 1 : cow_fault(dir, addr);
        }
        if (vma->prot & VM_PROT_IO) {
            return 0;
        }
        if (vma->file) {
            return vma->type == VM_SHARED ? shared_fault(dir, vm, vma, addr)
                                          : file_fault(dir, vm, vma, addr);
        }
    }

    phys = alloc_user_page(dir, addr);
    if (phys == 0) {
        return 0; /* out of frames */
    }

    if (paging_zero_frame(phys) != 0) {
        unmap_user_page_frame(dir, addr);
        return 0;
    }

    if (vma && !(vma->prot & VM_PROT_WRITE)) {
        paging_set_writable(dir, addr, false);
    }
    /* A grown stack page is the stack's, which has no VM_PROT_EXEC. */
    if (!vma || !(vma->prot & VM_PROT_EXEC)) {
        paging_set_exec(dir, addr, false);
    }
    return 1;
}

/**
 * Demand-paging resolver. Decide whether an access at the (not necessarily
 * aligned) `addr` is legal in the address space `mm` and, if so, make sure
 * the page is backed. `write` is 1 for a write, and `as_limit` is the
 * RLIMIT_AS a grown stack is held to.
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
 * A missing page is heap, stack or an anonymous mapping, since exec loads
 * code and data whole, or a page of a file. An anonymous one gets a zeroed
 * frame, read-only in the PTE when its region is, so a later write faults; a
 * grown stack page stays writable. A file's page is read into its frame,
 * which may sleep, and when the regions changed meanwhile the resolution
 * runs again (`vm_resolve_once`), so success always means a page that is
 * present and allows the access.
 *
 * Returns 1 if the page is now mapped, 0 if the access is illegal or no
 * frame could be had or zeroed; the caller then segfaults the process, or a
 * pre-fault answers -EFAULT. `mm` is the current process's for a fault and
 * a pre-fault, and a stopped tracee's for ptrace.
 */
static int vm_resolve_in(struct mm *mm, uintptr_t addr, int write, uintptr_t as_limit)
{
    int rc;

    do {
        rc = vm_resolve_once(mm, addr, write, as_limit);
    } while (rc < 0);
    return rc;
}

/** `vm_resolve_in` for the current process's address space. */
static int vm_resolve(uintptr_t addr, int write)
{
    return vm_resolve_in(current_proc->mm, addr, write, current_proc->rlim[RLIM_AS].cur);
}

/* Whether every page of [start, end) is present in `mm` and allows the
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
 * Bring in every page of [addr, addr + len) of the current process for the
 * access, as `paging.h` describes. The resolution of one page may sleep,
 * reading a file or writing a cache back, and another thread may meanwhile
 * unmap or protect a page already brought in, which the kernel would then
 * touch and fault on with no way to recover. So once every page is in, the
 * range is checked again with nothing that sleeps, and brought in again
 * when a page went; the answer holds until the caller sleeps.
 */
int vm_prefault_range(uintptr_t addr, uintptr_t len, int write)
{
    uintptr_t page, end;

    if (!current_proc || current_proc->mm == NULL) {
        return 0; /* no user address space to fault against */
    }
    if (len == 0) {
        return 1; /* an empty range touches nothing */
    }

    /* A wrapping range would end the loop early and leave its tail unbacked. */
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
 * Copy-on-write resolver for a write fault on a present, PTE_COW page in the
 * address space of `dir`. If this process is the frame's last owner (refcount
 * 1) the page is just made writable in place; otherwise a private writable
 * copy is allocated, the shared bytes are copied in, and the shared frame's
 * reference is dropped. The caller balanced memory before it looked at the
 * page, so nothing here sleeps and the entry read is the one replaced.
 * Returns 1 if it handled a COW page, 0 if the page is not COW or no frame
 * could be had or copied for it (the caller continues to demand paging /
 * segfault).
 */
static int cow_fault(page_dir_t *dir, uintptr_t addr)
{
    pte_t *e = user_pte(dir, addr);
    pte_t pte;
    uintptr_t old_phys, frame;
    uintptr_t new_phys;

    if (!e) {
        return 0;
    }
    pte = *e;
    if (!(pte & PTE_P) || !(pte & PTE_COW)) {
        return 0; /* not a copy-on-write page */
    }

    old_phys = PTE_ADDR(pte);
    frame = old_phys / PAGE_SIZE;

    /* Sole owner: re-grant write and drop the COW mark in place, no copy. */
    if (frame >= _frames_number || _frames[frame] <= 1) {
        *e = (pte & ~(pte_t)PTE_COW) | PTE_W;
        paging_flush_dir(dir, addr);
        return 1;
    }

    new_phys = alloc_user_page(dir, addr);
    if (new_phys == 0) {
        return 0; /* out of frames -> caller segfaults the process */
    }
    if (pte & PTE_NOEXEC) {
        paging_set_exec(dir, addr, false);
    }
    if (paging_copy_frame(new_phys, old_phys) != 0) {
        /* Nothing was copied: the shared page is put back as it was, the
         * fresh frame goes back, and the fault is unresolved. */
        *user_pte(dir, addr) = pte;
        paging_flush_dir(dir, addr);
        frame_unref(new_phys);
        return 0;
    }
    frame_unref(old_phys);
    return 1;
}

/**
 * Give the present page at `addr` of `dir` a frame of its own, as a
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
static int page_private(page_dir_t *dir, uintptr_t addr, int writable)
{
    pte_t *e = user_pte(dir, addr);
    pte_t pte;
    uintptr_t old_phys;
    uintptr_t new_phys;

    if (!e || !((pte = *e) & PTE_P)) {
        return -1;
    }
    if ((pte & PTE_COW) && writable) {
        return cow_fault(dir, addr) ? 0 : -1;
    }
    old_phys = PTE_ADDR(pte);
    new_phys = old_phys;
    if (old_phys / PAGE_SIZE < _frames_number && _frames[old_phys / PAGE_SIZE] > 1) {
        new_phys = alloc_user_frame();
        if (new_phys == 0) {
            return -1;
        }
        if (paging_copy_frame(new_phys, old_phys) != 0) {
            free_frame(new_phys);
            return -1;
        }
    }
    *e = (pte_t)new_phys | (pte & PTE_FLAGS & ~(pte_t)PTE_COW);
    paging_flush_dir(dir, addr);
    if (new_phys != old_phys) {
        frame_unref(old_phys);
    }
    return 0;
}

/* One page's part of `paging_access`: `len` bytes at `addr`, within one
 * page, to or from `p`. The resolution and, for a write, the balance of
 * memory before a copy may sleep, so the region is looked up again after
 * them, and the page must still be present in a region that allows some
 * access. */
static int access_page(struct mm *mm, uintptr_t addr, uint8_t *p, uint32_t len, int write)
{
    const vma_t *vma = vm_find(&mm->vm, addr);
    uint8_t *map;

    if (addr < USER_ADDR_MIN || addr >= USER_ADDR_MAX || vma == NULL ||
        !(vma->prot & (VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXEC))) {
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
        (vma->prot & VM_PROT_IO) || !pte_present(mm->dir, addr)) {
        return -EIO;
    }
    if (write && (vma->type == VM_SHARED
                      ? !(vma->prot & VM_PROT_WRITE)
                      : page_private(mm->dir, addr, (vma->prot & VM_PROT_WRITE) != 0) != 0)) {
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

/* Whether the fault `p` at the user address `addr` is an instruction fetch
 * from a page of no region that may be executed, which no resolution makes
 * legal: the page is present and readable, and would be fetched from again
 * for ever. */
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
 * Resolve a page fault, or end what caused it.
 *
 * Tried in order. Any fault at a user-space address of a live user process
 * goes to `vm_resolve`, but an instruction fetch from a region without
 * execute (`fetch_refused`), keyed on the address and not on the
 * processor's mode,
 * so a kernel touch of a lazy user page resolves too. A write to a page shared
 * by a fork reaches `cow_fault` through it, and only once the region's
 * protection allows the write: a region mprotect made read-only keeps its
 * copy-on-write pages read-only. A supervisor fault inside
 * the page-cache window is filled by `pc_fault`, whose polled read is safe
 * with interrupts off; a fault past the backed size, or one the fill cannot
 * serve, panics. The window is a kernel-only mapping, so a user-mode fault
 * there, which would fault again on the supervisor-only page the fill maps,
 * is not filled and falls through to the segfault.
 *
 * What is left kills the process with SIGSEGV when the fault came from user
 * mode, and otherwise panics: the usercopy helpers return -EFAULT before the
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

    if (have_user_proc && in_user_range && !fetch_refused(fault_address, params)) {
        current_proc->frame_refused = 0;
        if (vm_resolve(fault_address, write)) {
            return; /* page mapped; iret retries the faulting instruction */
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

        /* Out of memory: the access was legal and no frame could be had for
           it. Linux's OOM killer ends a process with SIGKILL, which no
           handler sees; a SIGSEGV handler that returned would fault again. */
        k_memset(&info, 0, sizeof(info));
        info.code = SI_KERNEL;
        klog(KLOG_WARN, "vm", "fault: out of memory: killing pid %d: no frame for a %s at 0x%x",
             current_proc->pid, write ? "write" : "read", fault_address);
        signal_force(current_proc, SIGKILL, &info);
        return;
    }

    if (user_mode && have_user_proc) {
        struct k_siginfo info;

        /* SIGSEGV with the address, SEGV_ACCERR when a region holds it and
           SEGV_MAPERR when none does, forced as Linux forces it: a handler
           runs, and the faulting instruction runs again after it returns,
           and a tracer takes the stop; the death is logged only when no
           handler and no tracer will see the signal. */
        k_memset(&info, 0, sizeof(info));
        info.code = vm_find(&current_proc->mm->vm, fault_address) ? SEGV_ACCERR : SEGV_MAPERR;
        info.addr = fault_address;
        current_proc->trap_nr = PAGE_FAULT_VECTOR;
        current_proc->trap_err = (uint32_t)params->error_code;
        current_proc->trap_addr = fault_address;
        signal_force(current_proc, SIGSEGV, &info);
        if (current_proc->sigacts[SIGSEGV].handler == KSIG_DFL &&
            !(current_proc->ptrace & PT_TRACED)) {
            klog(KLOG_WARN, "vm", "fault: killing pid %d: bad %s at 0x%x (err 0x%x)",
                 current_proc->pid, write ? "write" : "read", fault_address, params->error_code);
        }
        return;
    }

    pid = kstack_overrun_pid(fault_address);
    if (pid >= 0) {
        klog(KLOG_CRIT, "vm",
             "fault: in kernel mode at 0x%X (err 0x%X), eip 0x%X: "
             "pid %d ran past the end of its kernel stack",
             fault_address, params->error_code, FRAME_PC(params), pid);
        k_panic("page fault: a kernel stack ran out");
    }

    klog(KLOG_CRIT, "vm", "fault: in kernel mode at 0x%X (err 0x%X), eip 0x%X", fault_address,
         params->error_code, FRAME_PC(params));

    k_panic("Unrecoverable error: page fault while in kernel mode.");
}

/**
 * Map a physical MMIO region into the kernel's MMIO region and return the
 * virtual address it was given, or 0 when the region has no room or the
 * range is empty or wraps. The entries are cache-disabled (PCD|PWT) so a
 * device-register read is never stale, whatever the region's address. The
 * page tables of the kernel's gigabyte exist at boot and are shared by
 * every directory, so a mapping made here at any time is seen by every
 * process.
 *
 * The page count is rounded up by division and not by adding a page less
 * one: a region reaching the top of the address space would wrap that sum to
 * a few bytes, ask for no pages, map nothing and still return an address.
 */
uintptr_t map_mmio(uintptr_t phys_addr, size_t size)
{
    uintptr_t start_page = phys_addr & ~(PAGE_SIZE - 1);
    uintptr_t end_addr = phys_addr + size;
    uintptr_t pages, span, i, va;

    if (size == 0 || end_addr < phys_addr) {
        klog(KLOG_ERR, "vm", "map_mmio: bad region 0x%x, %d bytes", phys_addr, size);
        return 0;
    }
    span = end_addr - start_page;
    pages = span / PAGE_SIZE + ((span % PAGE_SIZE) ? 1u : 0u);
    if (pages > (KVA_MMIO + KVA_MMIO_SIZE - _mmio_next) / PAGE_SIZE) {
        klog(KLOG_ERR, "vm", "map_mmio: the MMIO region is full at 0x%x", _mmio_next);
        return 0;
    }
    va = _mmio_next;
    for (i = 0; i < pages; i++) {
        /* Present, writable, cache-disabled: a register is not memory. */
        *kernel_pte(va + i * PAGE_SIZE) =
            (pte_t)(start_page + i * PAGE_SIZE) | PTE_P | PTE_W | PTE_NOCACHE;
    }
    _mmio_next += pages * PAGE_SIZE;

    /* Every translation the processor cached goes. */
    arch_space_load(arch_space_root());

    return va + (phys_addr - start_page);
}

#ifdef ARCH_DIRECT_MAP_ALL
/* The direct map reaches every frame: a temporary mapping is the frame's
 * address there, which changes no entry and so no processor drops. */
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

/* Mark `frame` used with one reference, whatever it held. */
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
 * each is free in its region.
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

uintptr_t alloc_frame(void)
{
    uintptr_t frame;
    uint32_t flags;

    if (framefail_now() || !mem_may_take(0)) {
        return 0;
    }
    /* Selection and claim as one step: an allocation between them would
     * take the frame this one had chosen. */
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

void paging_user_count(page_dir_t *dir, uintptr_t start, uintptr_t end, unsigned long *present,
                       unsigned long *dirty)
{
    uintptr_t va = start;

    *present = *dirty = 0;
    while (va < end) {
        pte_t *table = mmu_user_table(dir, va, 0);
        uintptr_t next =
            table ? (va & ~(MMU_TABLE_SPAN - 1)) + MMU_TABLE_SPAN : mmu_user_skip(dir, va);

        if (next > end || next <= va) {
            next = end;
        }
        for (; table && va < next; va += PAGE_SIZE) {
            pte_t e = table[MMU_INDEX(va)];

            if (e & PTE_P) {
                (*present)++;
                *dirty += (e & PTE_DIRTY) != 0;
            }
        }
        va = next;
    }
}

/**
 * Reserve one free physical frame for kernel-internal use and return the
 * kernel's address of it, or NULL if none is free.
 *
 * The frame is marked used at reference count 1 and has no user mapping, so
 * `alloc_user_page` and `free_user_pages` skip it; the caller owns it until
 * `free_kernel_frame`. The scan runs down from the top of the window below
 * HARD_MEMORY_LIMIT, while user pages come from above the window first and
 * then from the window's bottom up, so a kernel reservation and user pages
 * rarely contend. The window's frames are the DMA pool: they lie below
 * 4 GiB on every architecture, so a device takes the frame's physical
 * address, `virt_to_phys` of what this returns, in a 32-bit register.
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

/**
 * Return the number of physical frames the pool manages.
 */
size_t frame_total_count(void)
{
    return _managed_frames;
}

/**
 * Return the number of physical frames currently free.
 *
 * Every writer of the frame table keeps the count, so this reads one word
 * and scans nothing. A reader gets a point-in-time count, which is what
 * sysinfo, the leak probe and reclaim need, so it takes no lock.
 */
size_t frame_free_count(void)
{
    return _free_frames;
}

/**
 * Map physical frame `phys` at the page-aligned kernel virtual address `va`
 * with a supervisor-only leaf PTE: present, no user bit, writable iff
 * `writable`.
 *
 * The page table for `va` must already exist (see `premap_kernel_window`).
 * The page cache maps a frame read-only on a read fault so the first write
 * faults again and can be recorded dirty, and read-write on a write fault.
 * No-op if the table is somehow absent.
 */
void map_kernel_frame(uintptr_t va, uintptr_t phys, int writable)
{
    pte_t *e = kernel_pte(va);

    if (!e) {
        return;
    }
    *e = (pte_t)(phys & ~(PAGE_SIZE - 1)) | PTE_P | (writable ? PTE_W : 0);
    paging_flush_tlb(va);
}

/**
 * Clear the leaf PTE for the page-aligned kernel virtual address `va` and
 * flush its TLB entry. No-op if the page table is absent. Used by page-cache
 * eviction.
 */
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
        return; /* not present */
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
        return; /* not present */
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

/**
 * Drop the translation of `addr` in `dir` wherever a processor has it.
 *
 * A user address is dropped on this processor when it translates through
 * `dir`, and on every other that does (`smp_tlb_shootdown`), before this
 * returns: the caller may free the page's frame next. A kernel address is
 * in every directory: this processor drops it now, and every other drops
 * all its translations before it next takes the big lock, which it holds
 * whenever it reads kernel memory that was not mapped when it last held it
 * (`smp_kernel_mapping_changed`).
 */
void paging_flush_dir(const page_dir_t *dir, uintptr_t addr)
{
    if (addr < USER_ADDR_MIN || addr >= USER_ADDR_MAX) {
        arch_tlb_flush_page(addr);
        smp_kernel_mapping_changed();
        return;
    }
    if (dir == _curr_dir) {
        arch_tlb_flush_page(addr);
    }
    smp_tlb_shootdown(dir, addr);
}

void frame_ref(uintptr_t phys)
{
    uintptr_t frame = phys / PAGE_SIZE;
    uint32_t flags = spin_lock_irqsave(&frame_lock);

    /* Only a live frame can gain a sharer, and the words at and above
     * `FRAME_REF_MAX` are states, not counts. */
    if (frame < _frames_number && _frames[frame] != 0 && _frames[frame] < FRAME_REF_MAX - 1) {
        _frames[frame]++;
    }
    spin_unlock_irqrestore(&frame_lock, flags);
}

/* `frame_unref` with the frame allocator's lock held. */
static uint32_t frame_unref_locked(uintptr_t phys)
{
    uintptr_t frame = phys / PAGE_SIZE;

    if (frame >= _frames_number || _frames[frame] == 0) {
        return 0;
    }
    /* A state word is not a count: a frame the map does not offer or the
     * kernel cannot reach has no mapping to drop. */
    if (_frames[frame] >= FRAME_REF_MAX) {
        return _frames[frame];
    }
    _frames[frame]--;
    if (_frames[frame] == 0) {
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
    return _frames[frame];
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

    virt &= ~(PAGE_SIZE - 1);
    table = mmu_user_table(dir, virt, 1);
    if (!table) {
        return -1;
    }

    /* The caller owns the frame and its reference count; we only write the
     * PTE. */
    table[MMU_INDEX(virt)] = PTE_ADDR((pte_t)phys) | (flags & (pte_t)PTE_FLAGS);
    paging_flush_dir(dir, virt);
    return 0;
}

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
    /* Selection and claim as one step, as in `alloc_frame`; the refusals
     * come first, so a refused selection does not move the hint past a
     * free frame. */
    flags = spin_lock_irqsave(&frame_lock);
    frame = framefail_now() ? _frames_number : find_free_frame();
    if (frame < _frames_number) {
        frame_take(frame);
    }
    spin_unlock_irqrestore(&frame_lock, flags);

    if (frame >= _frames_number) {
        /* Who holds the pool, so that a machine that runs out says what
         * filled it: the boot's reservations below the user frames, the
         * kernel's frames from the top, and the user frames between, with
         * the shared ones counted apart. */
        size_t user_used = 0, user_shared = 0;
        uintptr_t f;

        for (f = _user_frames_start; f < _frames_number; f++) {
            if (_frames[f] != 0 && _frames[f] < FRAME_REF_MAX) {
                user_used++;
            }
            if (_frames[f] > 1 && _frames[f] < FRAME_REF_MAX) {
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
        return 0; /* No free frames */
    }
    return frame * PAGE_SIZE;
}

uintptr_t alloc_user_page(page_dir_t *dir, uintptr_t virt_addr)
{
    uintptr_t phys_addr;
    pte_t *table;

    virt_addr &= ~(PAGE_SIZE - 1);

    /* The table first: one that cannot be had leaves no frame to give back. */
    table = mmu_user_table(dir, virt_addr, 1);
    if (!table) {
        return 0;
    }
    phys_addr = alloc_user_frame();
    if (phys_addr == 0) {
        return 0;
    }

    table[MMU_INDEX(virt_addr)] = (pte_t)phys_addr | PTE_P | PTE_W | PTE_U;

    /* Flush TLB for this address if we're in the current directory */
    paging_flush_dir(dir, virt_addr);

    return phys_addr;
}

void unmap_user_range(page_dir_t *dir, uintptr_t start, uintptr_t end)
{
    uintptr_t addr;

    if (!dir) {
        return;
    }

    for (addr = start; addr < end; addr += PAGE_SIZE) {
        /* A table shared with the kernel directory is never touched. */
        pte_t *e = user_pte(dir, addr);
        pte_t pte;

        if (!e) {
            continue;
        }
        pte = *e;
        if (!(pte & PTE_P)) {
            continue; /* not present: a never-faulted lazy page, nothing to free */
        }

        /* Drop the mapping's reference (frees the frame on the last
         * reference). */
        frame_unref(PTE_ADDR(pte));
        *e = 0;
        paging_flush_dir(dir, addr);
    }
}

int paging_move_range(page_dir_t *dir, uintptr_t from, uintptr_t to, uintptr_t len)
{
    uintptr_t off;

    /* The tables first, so the move cannot stop half way. */
    for (off = 0; off < len; off += PAGE_SIZE) {
        pte_t *e = user_pte(dir, from + off);

        if (e && *e && !mmu_user_table(dir, to + off, 1)) {
            return -1;
        }
    }
    for (off = 0; off < len; off += PAGE_SIZE) {
        pte_t *e = user_pte(dir, from + off);

        if (!e || !*e) {
            continue;
        }
        *user_pte(dir, to + off) = *e;
        *e = 0;
        paging_flush_dir(dir, from + off);
        paging_flush_dir(dir, to + off);
    }
    return 0;
}

void paging_protect_range(page_dir_t *dir, uintptr_t start, uintptr_t end, uint32_t prot)
{
    uintptr_t addr;

    for (addr = start; addr < end; addr += PAGE_SIZE) {
        pte_t *e = user_pte(dir, addr);
        pte_t pte;
        uintptr_t frame;

        if (!e) {
            continue;
        }
        pte = *e;
        if (!(pte & PTE_P)) {
            continue; /* not resident: the fault reads the region's protection */
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
            } else if (frame < _frames_number && _frames[frame] > 1) {
                pte = (pte & ~(pte_t)PTE_W) | PTE_COW;
            } else {
                pte |= PTE_W;
            }
        }
        *e = pte;
        paging_flush_dir(dir, addr);
    }
}

/**
 * Give the child's directory `child_dir` the page of `parent_dir` whose
 * entry is `*e`, at `va` in a region of type `type`, as `paging_fork_copy`
 * describes: 0, or -1 with no memory for a table.
 */
static int fork_copy_page(page_dir_t *child_dir, pte_t *e, uintptr_t va, vm_type_t type)
{
    pte_t pte = *e;
    uintptr_t parent_phys = PTE_ADDR(pte);

    if (!(pte & PTE_P) || !(pte & (PTE_U | PTE_NONE)) || parent_phys == 0) {
        return 0; /* present, and user or PROT_NONE, and a real frame */
    }
    if (type == VM_SHARED) {
        if (map_user_frame(child_dir, va, parent_phys, pte & PTE_FLAGS) != 0) {
            return -1;
        }
        frame_ref(parent_phys);
        return 0;
    }

    /*
     * Copy-on-write: share the frame instead of copying it. The child maps
     * the same physical frame, and the frame's reference count is bumped so
     * it survives until both sides drop it. A WRITABLE parent page is
     * write-protected and marked PTE_COW in BOTH directories, so the first
     * write on either side faults into cow_fault (which makes a private copy
     * and re-grants write). A READ-ONLY page (ELF text, PROT_READ, or an
     * already-COW page from an earlier fork) is shared with its exact flags,
     * no copy; a write to genuine read-only text still segfaults because it
     * has no PTE_COW.
     */
    if (pte & PTE_W) {
        if (map_user_frame(child_dir, va, parent_phys,
                           PTE_P | PTE_U | PTE_COW | (pte & PTE_NOEXEC)) != 0) {
            return -1;
        }
        *e = (pte & ~(pte_t)PTE_W) | PTE_COW;
        paging_flush_tlb(va); /* the parent is the current dir */
    } else if (map_user_frame(child_dir, va, parent_phys, pte & PTE_FLAGS) != 0) {
        return -1;
    }
    frame_ref(parent_phys);
    return 0;
}

/**
 * Duplicate the parent's user address space `from`, the one in use, into
 * the child's directory, copy-on-write.
 *
 * Walks the parent's regions, and in each the leaf tables the parent has
 * there, skipping a table's span where it has none; every user page lies in
 * a region, and a walk of the whole of user space would take 64 million
 * steps on x86-64. For each present user page it maps the SAME physical
 * frame into the child (bumping the frame refcount) rather than copying it;
 * writable pages are write-protected and PTE_COW-marked in both directories
 * so the first write faults into cow_fault for a private copy. A page of a
 * shared region is the same frame in the child, with the same rights: a
 * store on either side is seen by both. Returns 0 on success, -1 on
 * failure.
 */
int paging_fork_copy(struct mm *from, page_dir_t *child_dir)
{
    page_dir_t *parent_dir = from->dir;
    uint32_t r;

    for (r = 0; r < from->vm.count; r++) {
        const vma_t *region = &from->vm.vmas[r];
        uintptr_t va = region->start;

        while (va < region->end) {
            pte_t *table = mmu_user_table(parent_dir, va, 0);
            uintptr_t next = table ? (va & ~(MMU_TABLE_SPAN - 1)) + MMU_TABLE_SPAN
                                   : mmu_user_skip(parent_dir, va);

            if (next > region->end || next <= va) {
                next = region->end;
            }
            for (; table && va < next; va += PAGE_SIZE) {
                if (fork_copy_page(child_dir, &table[MMU_INDEX(va)], va, region->type) != 0) {
                    return -1;
                }
            }
            va = next;
        }
    }
    return 0;
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
