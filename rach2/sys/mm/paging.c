#include "../cmdline.h"
#include "../klog.h"
#include "paging.h"
#include "kheap.h"
#include "pagecache.h"
#include "frame.h"
#include "usercopy.h"
#include "../serial.h"
#include "../lib/ksignal.h"
#include "../proc/process.h"

/**
 * Defined in kernel.c: tear the current user process down with a signal
 * death (`signo`; SIGSEGV here): free its pages, wake a waiting parent,
 * reparent its children to init, and schedule the parent to collect the
 * zombie. A real init's death panics instead (a rescue/no-init debug shell
 * regains control). Kills a process that faulted illegally without taking
 * the machine down. Mirrors the CTRL+C kill path.
 */
extern void terminate_current_process(int32_t signo);

page_dir_t *_kernel_dir;
page_dir_t *_curr_dir;

uint32_t *_frames;
size_t    _frames_number;
/* The frames below the identity-mapped window, which `alloc_kernel_frame`
 * hands out: a frame the kernel reaches through its physical address. */
static size_t _window_frames;
static uint32_t _managed_frames;
/* Where the next scan for a free frame starts: no frame below it is free.
 * A scan from the bottom of a million-frame table for every page would be
 * the table's length squared over a large mapping. */
static uintptr_t _next_free_frame;
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

/* The temporary mappings: one bit per slot of the region, set while a
 * caller holds it. */
static uint32_t _temp_slots[KVA_TEMP_SLOTS / 32];
/* The next free page of the MMIO region; a region is mapped for good. */
static uintptr_t _mmio_next = KVA_MMIO;

/* Set once the heap exists: from then on page tables and directories come
 * from it and go back to it, and their physical addresses, which the
 * processor reads, come from the kernel directory's own mapping. Before
 * that the boot carves them below the window, where the two are equal. */
static int _heap_ready;

/* The physical address of a page table or directory the kernel holds by
 * its virtual address. */
static uintptr_t table_phys(const void *table)
{
    return _heap_ready ? paging_virt_to_phys((uintptr_t)table) : (uintptr_t)table;
}

/**
 * Starting frame index for user page allocation. Set once at init time to
 * `mem_reserved_ptr / PAGE_SIZE`, so freed user pages can be reused even after
 * `mem_reserved_ptr` advances due to later kernel allocations.
 */
static uintptr_t _user_frames_start = 0;

/* Frames `alloc_kernel_frame` has taken from the top of the pool, the page
 * cache's among them, for the record a failed user allocation writes. */
static uint32_t _kernel_frames_reserved;

/**
 * Allocate a page table from the free list, or fresh via `mem_reserve`.
 */
static page_table_t *alloc_page_table(void)
{
    page_table_t *table;

    if (_heap_ready) {
        table = kalloc_pages(1, NULL);
        if (table) {
            k_memset(table, 0, sizeof(page_table_t));
        }
        return table;
    }

    table = (page_table_t *)mem_reserve(sizeof(page_table_t), true);
    if (table) {
        /* Mark the frame as used in the frame table. mem_reserve allocates
         * memory after init_paging without updating the table; without this,
         * alloc_user_page could hand the same frame to a user page, overwriting
         * the page table. */
        uintptr_t frame = (uintptr_t)table / PAGE_SIZE;
        if (frame < _frames_number) {
            _frames[frame] = 1;
        }
        k_memset(table, 0, sizeof(page_table_t));
    }
    return table;
}

/**
 * Add a page table to the free list.
 */
static void free_page_table(page_table_t *table)
{
    if (!table) return;
    kfree_pages(table, 1);
}

/**
 * Allocate a page directory from the free list, or fresh via `mem_reserve`.
 */
static page_dir_t *alloc_page_dir(void)
{
    page_dir_t *dir;

    if (_heap_ready) {
        dir = kalloc_pages(sizeof(page_dir_t) / PAGE_SIZE, NULL);
        if (dir) {
            k_memset(dir, 0, sizeof(page_dir_t));
        }
        return dir;
    }

    dir = (page_dir_t *)mem_reserve(sizeof(page_dir_t), true);
    if (dir) {
        /* Mark the frames as used in the table. page_dir_t is 8KB (2 pages),
         * so mark both frames. */
        uintptr_t frame = (uintptr_t)dir / PAGE_SIZE;
        uintptr_t num_frames = (sizeof(page_dir_t) + PAGE_SIZE - 1) / PAGE_SIZE;
        uintptr_t i;
        for (i = 0; i < num_frames && (frame + i) < _frames_number; i++) {
            _frames[frame + i] = 1;
        }
        k_memset(dir, 0, sizeof(page_dir_t));
    }
    return dir;
}

static void _page_fault_handler(isr_params_t *params);

static void _load_page_dir(page_dir_t *dir);

void init_paging(struct frame_map *map)
{
    unsigned int frame;

    uintptr_t tmp, phys_address,
                   table_index,
                   page_index;

    page_table_t *table;

    /* frame.h mirrors these two values so it can stay host-linkable; fail
     * loudly if the mirror ever drifts from the real definitions. */
    if (FRAME_DIRECT_MAP_LIMIT != HARD_MEMORY_LIMIT || FRAME_PAGE_SIZE != PAGE_SIZE) {
        k_panic("frame.h constants drifted from paging.h");
    }

    /* The table covers every frame below the map's top, one word each, and
     * lives below the window with the other boot tables; a map with nothing
     * usable gets the window, so a machine whose firmware says nothing still
     * boots as it did. */
    _frames_number = map->top_frame ? map->top_frame : (HARD_MEMORY_LIMIT / PAGE_SIZE);
    _window_frames = _frames_number < HARD_MEMORY_LIMIT / PAGE_SIZE
                         ? _frames_number
                         : HARD_MEMORY_LIMIT / PAGE_SIZE;
    _frames = (uint32_t *) mem_reserve(_frames_number * sizeof(uint32_t), true);
    if (_frames == NULL) {
        k_panic("init_paging: no reserved memory for the frame table");
    }
    _managed_frames = 0;
    for (frame = 0; frame < _frames_number; frame++) {
        if (map->top_frame == 0 || frame_map_usable(map, (uint32_t)frame)) {
            _frames[frame] = 0;
            _managed_frames++;
        } else {
            _frames[frame] = FRAME_ABSENT;
        }
    }

    _kernel_dir = (page_dir_t *) mem_reserve(sizeof(page_dir_t), true);
    if (_kernel_dir == NULL) {
        k_panic("init_paging: no reserved memory for the kernel page directory");
    }
    k_memset(_kernel_dir, 0, sizeof(page_dir_t));
    _curr_dir = _kernel_dir;

    /*
     * Identity map all physical memory up to HARD_MEMORY_LIMIT.
     * This allows kernel code to access any physical address directly.
     * Only frames below mem_reserved_ptr are marked as used in the frame table;
     * frames above remain free for user page allocation.
     */
    phys_address = frame = 0;
    while (phys_address < HARD_MEMORY_LIMIT) {
        tmp = phys_address / PAGE_SIZE;

        table_index = tmp / 1024;
        page_index  = tmp % 1024;

        if (!(table = _curr_dir->logical_page_tables[table_index])) {
            tmp = mem_reserve(sizeof(page_table_t), true);
            if (tmp == 0) {
                k_panic("init_paging: no reserved memory for an identity-map "
                        "page table");
            }
            table = (page_table_t *) tmp;

            k_memset(table, 0, sizeof(page_table_t));

            /* This page table is writable only by a supervisor.
             * Page table address | 'present' and 'rw' flags */
            _curr_dir->phys_page_tables[table_index] = tmp | 0x3;

            _curr_dir->logical_page_tables[table_index] = table;
        }

        /* Mark frames below mem_reserved_ptr as used (kernel, heap, modules).
         * The `frame < _frames_number` bound matches the final pass and the
         * other table writes; it only matters when real RAM is smaller than
         * the direct-mapped window, but the guard keeps every write in range.
         */
        if (phys_address < mem_reserved_ptr && frame < _frames_number) {
            _frames[frame] = 1;
        }
        frame++;

        /*
         * This page is writable only by a supervisor.
         * Page frame address | 'present' and 'rw' flags
         */
        table->pages[page_index] = phys_address | 0x3;

        phys_address += PAGE_SIZE;
    }

    /*
     * The kernel's own gigabyte: every page table for it exists now and is
     * shared by every directory cloned later, so a mapping made in it after
     * the first fork is seen by every process. 256 tables, a megabyte.
     */
    for (table_index = KVA_BASE / PAGE_SIZE / 1024; table_index < 1024; table_index++) {
        if (_curr_dir->logical_page_tables[table_index]) {
            continue;
        }
        tmp = mem_reserve(sizeof(page_table_t), true);
        if (tmp == 0) {
            k_panic("init_paging: no reserved memory for the kernel's page tables");
        }
        k_memset((void *)tmp, 0, sizeof(page_table_t));
        _curr_dir->phys_page_tables[table_index] = tmp | 0x3;
        _curr_dir->logical_page_tables[table_index] = (page_table_t *)tmp;
    }

    /*
     * Final pass: mark all frames up to mem_reserved_ptr as used.
     * This is necessary because page tables allocated during the loop above
     * may reside at physical addresses that were already scanned before
     * those allocations occurred. Without this final pass, those frames
     * would incorrectly appear free and could be allocated for user pages,
     * overwriting kernel page tables.
     */
    {
        uintptr_t final_frame = mem_reserved_ptr / PAGE_SIZE;
        uintptr_t f;
        for (f = 0; f < final_frame && f < _frames_number; f++) {
            _frames[f] = 1;
        }
    }

    /*
     * Save the initial user frames starting point.
     * User page allocations will search for free frames starting here.
     */
    _user_frames_start = mem_reserved_ptr / PAGE_SIZE;
    _next_free_frame = _user_frames_start;
    /* Everything the boot carved below the break is reserved, the tables
     * above included. What the allocator manages is counted from the table
     * and not from the map's arithmetic, so the boot's report compares the
     * two: every whole frame the map offers above the break, reachable or
     * not yet. */
    if (map->top_frame) {
        frame_map_reserve(map, 0, mem_reserved_ptr);
    }
    _managed_frames = 0;
    for (frame = _user_frames_start; frame < _frames_number; frame++) {
        if (_frames[frame] == 0) {
            _managed_frames++;
        }
    }

    register_isr_handler(14, &_page_fault_handler, "page fault");

    _load_page_dir(_curr_dir);

    /* From here the heap exists, and every page table and directory made
     * later comes from it. */
    kheap_init();
    _heap_ready = 1;
}

static void _load_page_dir(page_dir_t *dir)
{
    uint32_t cr0;

    __asm__ __volatile__ ("mov %0, %%cr3" :: "r"(&dir->phys_page_tables));
    __asm__ __volatile__ ("mov %%cr0, %0" : "=r"(cr0));

    /* Enable paging (PG, bit 31) and supervisor write-protect (WP, bit 16). WP
     * makes a ring-0 write to a read-only page fault instead of silently
     * succeeding; the page cache's dirty tracking depends on it, mapping a
     * clean frame read-only so the next write re-faults and re-marks it dirty.
     * A kernel write to a user text page uses the physical alias or pre-faults
     * first, so it never traps. A kernel write to a present copy-on-write page
     * does trap: the WP fault routes to cow_fault, which makes the page private
     * and writable, and the instruction retries. WP affects nothing else. */
    cr0 |= 0x80000000u | 0x00010000u;

    __asm__ __volatile__ ("mov %0, %%cr0" :: "r"(cr0));
}

/* Page-fault error-code bits (Intel SDM Vol. 3A, 4.7). */
#define PF_ERR_PRESENT 0x1u /* 0 = not-present page, 1 = protection violation */
#define PF_ERR_WRITE   0x2u /* 0 = read access, 1 = write access */
#define PF_ERR_USER    0x4u /* 0 = supervisor mode, 1 = user mode */


static int cow_fault(uint32_t addr);

/**
 * Is the page containing `addr` writable in `dir`? Present and write-enabled;
 * a present copy-on-write page answers 0, which is what tells the resolver to
 * make it private before promising a caller it may write there.
 */
static int pte_writable(page_dir_t *dir, uint32_t addr)
{
    uint32_t page_num = addr / PAGE_SIZE;
    uint32_t table_index = page_num / 1024;
    uint32_t page_index = page_num % 1024;
    page_table_t *table;

    if (table_index >= 1024) {
        return 0;
    }
    table = dir->logical_page_tables[table_index];
    if (!table) {
        return 0;
    }
    return (table->pages[page_index] & 0x3) == 0x3;
}

/**
 * Is the page containing `addr` already present in `dir`? Keeps the
 * demand-paging resolver idempotent: an eager-loaded code/data page, or one
 * the pre-fault path already backed, must not be allocated a second time
 * (which would leak the old frame and wipe the page's contents).
 */
static int pte_present(page_dir_t *dir, uint32_t addr)
{
    uint32_t page_num = addr / PAGE_SIZE;
    uint32_t table_index = page_num / 1024;
    uint32_t page_index = page_num % 1024;
    page_table_t *table;

    if (table_index >= 1024) {
        return 0;
    }
    table = dir->logical_page_tables[table_index];
    if (!table) {
        return 0;
    }
    return (table->pages[page_index] & 0x1) != 0;
}

/**
 * Demand-paging resolver. Decide whether an access at the (not necessarily
 * aligned) `addr` is legal for the current process and, if so, make sure the
 * page is backed. `write` is 1 for a write. Returns 1 if the page is now
 * mapped, 0 if the access is illegal (no VMA covers it and it is not a valid
 * stack growth, or it writes a read-only region); so the caller faults or
 * segfaults.
 *
 * The current process always has a user address space here (the handler and
 * the pre-fault path both gate on `current_proc->cr3 != 0`).
 */
static int vm_resolve(uint32_t addr, int write)
{
    vm_space_t *vm = &current_proc->vm;
    vma_t *vma = vm_find(vm, addr);
    uintptr_t phys;

    if (!vma) {
        /* The one legal access outside every region is growing the stack
         * down. */
        if (!vm_grow_stack(vm, addr & ~(PAGE_SIZE - 1))) {
            return 0;
        }
        /* A grown stack page is read+write; fall through to back it. */
    } else {
        /* The access must be permitted by the region's protection. A write to a
         * read-only region and any access to a PROT_NONE region are protection
         * violations the caller turns into a segfault (or -EFAULT for a kernel
         * prefault); never silently backed. */
        if (write) {
            if (!(vma->prot & VM_PROT_WRITE)) {
                return 0;
            }
        } else if (!(vma->prot & VM_PROT_READ)) {
            return 0;
        }
        if (pte_present(_curr_dir, addr)) {
            /* Present is not enough for a write. A page shared by a fork is
             * present, read-only and marked copy-on-write, and the region it
             * lies in is writable, so the protection test above passed: a
             * pre-fault that answered "backed" here would promise the kernel a
             * page it cannot write, and the raw store that followed would fault
             * with the copy already half made. Making it private now is the
             * same work the fault handler would do, done before the promise.
             *
             * A present page that is not writable and not copy-on-write is out
             * of frames or a mapping this kernel does not make; reporting that
             * as unresolved segfaults the process, where reporting it as backed
             * would fault on the same instruction forever. */
            if (write && !pte_writable(_curr_dir, addr)) {
                return cow_fault(addr);
            }
            return 1; /* already backed; nothing to do */
        }
    }

    /* Lazily back the page. alloc_user_page maps it present + user + writable;
     * code/data pages are eager-loaded at exec, so only heap/stack/anon faults
     * reach here. */
    phys = alloc_user_page(_curr_dir, addr);
    if (phys == 0) {
        return 0; /* out of frames */
    }

    /* Zero the fresh page: brk/mmap/stack memory must read as zero, and the
     * frame may carry a previous process's data. The frame is inside the
     * identity-mapped window, so its physical address is directly writable. */
    paging_zero_frame(phys);

    /* Honor a read-only region's protection in the PTE, so a later write faults
     * as a protection violation, which the handler segfaults, and which
     * copy-on-write uses to tell a write fault from a demand fault. A grown
     * stack page (vma == NULL here) is left writable. */
    if (vma && !(vma->prot & VM_PROT_WRITE)) {
        paging_set_writable(_curr_dir, addr, false);
    }
    return 1;
}

int vm_prefault_range(uint32_t addr, uint32_t len, int write)
{
    uint32_t page, end;

    if (!current_proc || current_proc->cr3 == 0) {
        return 0; /* no user address space to fault against */
    }
    if (len == 0) {
        return 1; /* an empty range touches nothing */
    }

    /* Callers range-check with user_range_ok first, but self-defend anyway: an
     * addr + len that wraps the 32-bit address space would make `page < end`
     * terminate early and leave the tail unfaulted (and the caller copying it
     * would then fault unhandled). A wrap is rejected outright. */
    end = addr + len;
    if (end < addr) {
        return 0;
    }
    for (page = addr & ~(PAGE_SIZE - 1); page < end; page += PAGE_SIZE) {
        if (!vm_resolve(page, write)) {
            return 0;
        }
    }
    return 1;
}

/**
 * Copy-on-write resolver for a write fault on a present, PTE_COW page in the
 * current address space. If this process is the frame's last owner (refcount
 * 1) the page is just made writable in place; otherwise a private writable
 * copy is allocated, the shared bytes are copied in, and the shared frame's
 * reference is dropped. Returns 1 if it handled a COW page, 0 if the page is
 * not COW (the caller continues to demand paging / segfault).
 */
static int cow_fault(uint32_t addr)
{
    uint32_t page_num = addr / PAGE_SIZE;
    uint32_t table_index = page_num / 1024;
    uint32_t page_index = page_num % 1024;
    page_table_t *table;
    uint32_t pte, old_phys, frame;
    uintptr_t new_phys;

    if (table_index >= 1024) {
        return 0;
    }
    table = _curr_dir->logical_page_tables[table_index];
    if (!table) {
        return 0;
    }
    pte = table->pages[page_index];
    if (!(pte & 0x1) || !(pte & PTE_COW)) {
        return 0; /* not a copy-on-write page */
    }

    old_phys = pte & ~0xFFFu;
    frame = old_phys / PAGE_SIZE;

    /* Sole owner: re-grant write and drop the COW mark in place, no copy. */
    if (frame >= _frames_number || _frames[frame] <= 1) {
        table->pages[page_index] = (pte & ~PTE_COW) | 0x2u; /* +write, -COW */
        paging_flush_tlb(addr);
        return 1;
    }

    /* Shared: alloc_user_page installs a fresh writable frame (present+rw+user,
     * no COW) at addr; copy the shared page into it, then drop our reference to
     * the old shared frame. */
    new_phys = alloc_user_page(_curr_dir, addr);
    if (new_phys == 0) {
        return 0; /* out of frames -> caller segfaults the process */
    }
    paging_copy_frame(new_phys, old_phys);
    frame_unref(old_phys);
    return 1;
}

static void _page_fault_handler(isr_params_t *params)
{
    uint32_t fault_address;
    int write, user_mode, have_user_proc, in_user_range;

    __asm__ __volatile__("mov %%cr2, %0" : "=r"(fault_address));

    write = (params->error_code & PF_ERR_WRITE) != 0;
    user_mode = (params->error_code & PF_ERR_USER) != 0;
    have_user_proc = (current_proc != NULL && current_proc->cr3 != 0);
    in_user_range = (fault_address >= USER_ADDR_MIN && fault_address < USER_ADDR_MAX);

    /* Copy-on-write: a write that faulted on a *present* page (protection
     * violation) may be a COW page shared by a fork. Resolve it before demand
     * paging, since vm_resolve would see the page already present and loop. A
     * non-COW present-page write falls through (a genuine read-only region then
     * segfaults below). */
    if (have_user_proc && write && in_user_range &&
        (params->error_code & PF_ERR_PRESENT)) {
        if (cow_fault(fault_address)) {
            return; /* page is now private + writable; retry the instruction */
        }
    }

    /* Demand paging: a fault on a user-space address owned by a live user
     * process is resolved by lazily backing the page (heap/stack/anon) or
     * growing the stack. Keyed on the faulting *address* being in user space,
     * not on the CPU mode, so a kernel touch of a lazy user page would resolve
     * here too. */
    if (have_user_proc && in_user_range) {
        if (vm_resolve(fault_address, write)) {
            return; /* page mapped; iret retries the faulting instruction */
        }
    }

    /* Page cache: a fault inside the demand-paged disk window is serviced by
     * polling the backing sectors into a cache frame and mapping it. The window
     * is a kernel-only mapping the file system touches in supervisor mode, so
     * this serves supervisor faults only: a user-mode fault at a window address
     * is a wild ring-3 access that falls through to the segfault below. Without
     * the `!user_mode` gate a ring-3 touch would loop on the supervisor-only
     * page the fill maps, or, with no disk backing, panic the kernel. The
     * poll-only fill is safe with IF=0. A fault past the backed size, or one
     * the fill cannot serve, is a real bug. */
    if (!user_mode &&
        fault_address >= PCWIN_BASE &&
        fault_address < PCWIN_BASE + pc_window_size()) {
        if (!current_proc) {
            k_panic("pagecache: window fault with no current process");
        }
        if (pc_fault(fault_address, write)) {
            return;
        }
        k_panic("pagecache: unrecoverable disk fault");
    }

    /* Unrecoverable. A fault taken in user mode is the process's own bug: kill
     * just that process and survive; terminate_current_process schedules the
     * parent to collect the zombie, or panics if the dier was a real init. */
    if (user_mode && have_user_proc) {
        klog(KLOG_WARN, "vm", "fault: killing pid %d: bad %s at 0x%x (err 0x%x)",
             current_proc->pid, write ? "write" : "read", fault_address,
             params->error_code);
        terminate_current_process(SIGSEGV);
        return; /* child case: the parent was scheduled; nothing to retry */
    }

    /* A fault in kernel mode (or with no user context) is a kernel bug. The
     * fault-aware usercopy path returns -EFAULT before the kernel ever touches
     * a bad user pointer, so reaching here means the kernel itself faulted. */
    klog(KLOG_CRIT, "vm", "fault: in kernel mode at 0x%X (err 0x%X)",
         fault_address, params->error_code);

    k_panic("Unrecoverable error: page fault while in kernel mode.");
}

/**
 * Map a physical MMIO region to the same virtual address (identity map),
 * creating or rewriting its page table entries cache-disabled (PCD|PWT) so a
 * device-register read is never stale, whatever the region's address.
 *
 * This is a boot-time operation. Device MMIO above the identity-mapped window
 * needs a fresh kernel page table, and a kernel PDE installed after the first
 * fork would not reach the address spaces already cloned from `_kernel_dir`, so
 * a call that would add a new PDE after that point is refused (returns 0).
 * Rewriting a leaf PTE in a table every directory already shares stays legal.
 */
uintptr_t map_mmio(uintptr_t phys_addr, size_t size)
{
    uintptr_t start_page = phys_addr & ~(PAGE_SIZE - 1);
    uintptr_t end_addr = phys_addr + size;
    uintptr_t pages, i, va;

    /* Reject an empty or wrapping region: a `phys_addr + size` that overflowed
     * would leave end_addr below start_page, map nothing, and falsely succeed.
     */
    if (size == 0 || end_addr < phys_addr) {
        klog(KLOG_ERR, "vm", "map_mmio: bad region 0x%x, %d bytes", phys_addr,
             size);
        return 0;
    }
    pages = (end_addr - start_page + PAGE_SIZE - 1) / PAGE_SIZE;
    if (pages > (KVA_MMIO + KVA_MMIO_SIZE - _mmio_next) / PAGE_SIZE) {
        klog(KLOG_ERR, "vm", "map_mmio: the MMIO region is full at 0x%x", _mmio_next);
        return 0;
    }
    va = _mmio_next;
    for (i = 0; i < pages; i++) {
        uintptr_t page_num = (va + i * PAGE_SIZE) / PAGE_SIZE;
        page_table_t *table = _kernel_dir->logical_page_tables[page_num / 1024];

        /* Present, writable, cache-disabled: a register is not memory. */
        table->pages[page_num % 1024] = (start_page + i * PAGE_SIZE) | 0x1B;
    }
    _mmio_next += pages * PAGE_SIZE;

    /* Flush TLB */
    __asm__ __volatile__(
        "movl %%cr3, %%eax\n\t"
        "movl %%eax, %%cr3"
        ::: "eax", "memory"
    );

    return va + (phys_addr - start_page);
}

static uint32_t paging_irq_save(void)
{
    uint32_t flags;
    __asm__ __volatile__("pushfl; popl %0; cli" : "=r"(flags)::"memory");
    return flags;
}

static void paging_irq_restore(uint32_t flags)
{
    if (flags & 0x200) {
        __asm__ __volatile__("sti");
    }
}

void *kmap_temp(uintptr_t phys)
{
    uint32_t flags = paging_irq_save();
    uint32_t slot;

    for (slot = 0; slot < KVA_TEMP_SLOTS; slot++) {
        if (!(_temp_slots[slot / 32] & (1u << (slot % 32)))) {
            uintptr_t va = KVA_TEMP + slot * PAGE_SIZE;
            page_table_t *table = _kernel_dir->logical_page_tables[va / PAGE_SIZE / 1024];

            _temp_slots[slot / 32] |= 1u << (slot % 32);
            table->pages[(va / PAGE_SIZE) % 1024] = (phys & ~(PAGE_SIZE - 1)) | 0x3;
            paging_flush_tlb(va);
            paging_irq_restore(flags);
            return (void *)(va + (phys & (PAGE_SIZE - 1)));
        }
    }
    paging_irq_restore(flags);
    klog(KLOG_ERR, "vm", "temporary mappings: every slot is held");
    return 0;
}

void kunmap_temp(void *va)
{
    uintptr_t addr = (uintptr_t)va & ~(PAGE_SIZE - 1);
    uint32_t slot = (addr - KVA_TEMP) / PAGE_SIZE;
    uint32_t flags;
    page_table_t *table;

    if (addr < KVA_TEMP || slot >= KVA_TEMP_SLOTS) {
        return;
    }
    flags = paging_irq_save();
    table = _kernel_dir->logical_page_tables[addr / PAGE_SIZE / 1024];
    table->pages[(addr / PAGE_SIZE) % 1024] = 0;
    paging_flush_tlb(addr);
    _temp_slots[slot / 32] &= ~(1u << (slot % 32));
    paging_irq_restore(flags);
}

void paging_zero_frame(uintptr_t phys)
{
    void *va = kmap_temp(phys);

    if (va) {
        k_memset(va, 0, PAGE_SIZE);
        kunmap_temp(va);
    }
}

void paging_copy_frame(uintptr_t dst, uintptr_t src)
{
    void *d = kmap_temp(dst);
    void *s = kmap_temp(src);

    if (d && s) {
        k_memcpy(d, s, PAGE_SIZE);
    }
    if (s) {
        kunmap_temp(s);
    }
    if (d) {
        kunmap_temp(d);
    }
}

uintptr_t alloc_frame(void)
{
    uintptr_t frame;

    if (framefail_now()) {
        return 0;
    }
    for (frame = _next_free_frame; frame < _frames_number; frame++) {
        if (_frames[frame] == 0) {
            _frames[frame] = 1;
            _next_free_frame = frame + 1;
            return frame * PAGE_SIZE;
        }
    }
    return 0;
}

void free_frame(uintptr_t phys)
{
    frame_unref(phys);
}

uintptr_t paging_virt_to_phys(uintptr_t va)
{
    page_table_t *table = _kernel_dir->logical_page_tables[va / PAGE_SIZE / 1024];
    uint32_t pte;

    if (!table) {
        return 0;
    }
    pte = table->pages[(va / PAGE_SIZE) % 1024];
    if (!(pte & 0x1)) {
        return 0;
    }
    return (pte & ~(PAGE_SIZE - 1)) | (va & (PAGE_SIZE - 1));
}

/**
 * Reserve one free physical frame for kernel-internal use and return its
 * physical address, or 0 if none is free.
 *
 * The frame is marked used at reference count 1 and never has the user
 * bit, so `alloc_user_page` and `free_user_pages` skip it; the caller owns
 * it for the life of the kernel. The scan runs from the top of the pool
 * down, away from the bottom-up user-page scan, so a kernel reservation and
 * user pages rarely contend. The returned address is also the frame's
 * identity-mapped virtual address, since the kernel identity-maps the whole
 * pool.
 */
uintptr_t alloc_kernel_frame(void)
{
    uintptr_t frame = _window_frames;

    while (frame-- > _user_frames_start) {
        if (_frames[frame] == 0) {
            _frames[frame] = 1;
            _kernel_frames_reserved++;
            return frame * PAGE_SIZE;
        }
    }

    return 0;
}

/**
 * Return the number of physical frames the pool manages.
 */
uint32_t frame_total_count(void)
{
    return _managed_frames;
}

/**
 * Return the number of physical frames currently free.
 *
 * A linear scan of the frame table below the window. The caller (sysinfo,
 * the leak probe) wants a point-in-time count, so no locking is needed
 * under the IF discipline.
 */
uint32_t frame_free_count(void)
{
    uint32_t i, free_count = 0;

    for (i = 0; i < (uint32_t)_frames_number; i++) {
        if (_frames[i] == 0) {
            free_count++;
        }
    }
    return free_count;
}

/**
 * Pre-allocate every page table spanning [`va_start`, `va_start` + `size`) in
 * the kernel directory, leaving all of their PTEs not-present.
 *
 * `clone_page_directory` copies the kernel PDEs by value into each new
 * process, so a window page table first installed after a fork would be
 * invisible to that process. Installing them all up front, before the first
 * fork, lets `map_kernel_frame` later write a leaf PTE into a table every
 * directory already shares. `va_start` must be 4 MiB-aligned. Returns 0, or
 * -1 if a page table could not be allocated or the range runs past the
 * address space.
 */
int premap_kernel_window(uintptr_t va_start, size_t size)
{
    uintptr_t addr;
    uintptr_t end = va_start + size;

    for (addr = va_start; addr < end; addr += PAGE_SIZE * 1024) {
        uintptr_t table_index = (addr / PAGE_SIZE) / 1024;
        page_table_t *table;

        if (table_index >= 1024) {
            return -1;
        }
        if (!_kernel_dir->logical_page_tables[table_index]) {
            table = alloc_page_table();
            if (!table) {
                return -1;
            }
            _kernel_dir->logical_page_tables[table_index] = table;
            _kernel_dir->phys_page_tables[table_index] = table_phys(table) | 0x3;
        }
    }

    return 0;
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
    uintptr_t page_num = va / PAGE_SIZE;
    uintptr_t table_index = page_num / 1024;
    uintptr_t page_index = page_num % 1024;
    page_table_t *table;

    if (table_index >= 1024) {
        return;
    }
    table = _kernel_dir->logical_page_tables[table_index];
    if (!table) {
        return;
    }
    table->pages[page_index] =
        (phys & ~(PAGE_SIZE - 1)) | (writable ? 0x3u : 0x1u);
    paging_flush_tlb(va);
}

/**
 * Clear the leaf PTE for the page-aligned kernel virtual address `va` and
 * flush its TLB entry. No-op if the page table is absent. Used by page-cache
 * eviction.
 */
void unmap_kernel_frame(uintptr_t va)
{
    uintptr_t page_num = va / PAGE_SIZE;
    uintptr_t table_index = page_num / 1024;
    uintptr_t page_index = page_num % 1024;
    page_table_t *table;

    if (table_index >= 1024) {
        return;
    }
    table = _kernel_dir->logical_page_tables[table_index];
    if (!table) {
        return;
    }
    table->pages[page_index] = 0;
    paging_flush_tlb(va);
}

void paging_set_user(uintptr_t addr, bool user)
{
    uintptr_t page_num = addr / PAGE_SIZE;
    uintptr_t table_index = page_num / 1024;
    uintptr_t page_index = page_num % 1024;
    page_table_t *table;

    if (table_index >= 1024) return;

    table = _curr_dir->logical_page_tables[table_index];
    if (!table) return;

    /* U/S is bit 2 (0x4): set in both the page table and directory entry. */
    if (user) {
        table->pages[page_index] |= 0x4;
        _curr_dir->phys_page_tables[table_index] |= 0x4;
    } else {
        table->pages[page_index] &= ~0x4;
    }

    paging_flush_tlb(addr);
}

void paging_set_writable(page_dir_t *dir, uintptr_t addr, bool writable)
{
    uintptr_t page_num = addr / PAGE_SIZE;
    uintptr_t table_index = page_num / 1024;
    uintptr_t page_index = page_num % 1024;
    page_table_t *table;

    if (table_index >= 1024) {
        return;
    }
    table = dir->logical_page_tables[table_index];
    if (!table) {
        return;
    }
    if (!(table->pages[page_index] & 0x1)) {
        return; /* not present */
    }

    if (writable) {
        table->pages[page_index] |= 0x2;
    } else {
        table->pages[page_index] &= ~0x2; /* read-only for both ring 3 and 0 */
    }

    if (dir == _curr_dir) {
        paging_flush_tlb(addr);
    }
}

void paging_flush_tlb(uintptr_t addr)
{
    __asm__ __volatile__("invlpg (%0)" :: "r"(addr) : "memory");
}

page_dir_t *clone_page_directory(void)
{
    page_dir_t *new_dir;
    uint32_t i;

    /* Allocate a new page directory (from free list or fresh) */
    new_dir = alloc_page_dir();
    if (!new_dir) {
        return NULL;
    }

    /* Copy kernel page table references. Kernel page tables are shared across
     * all processes, so copy only the pointers, not the tables themselves; this
     * keeps kernel memory reachable from any process. After this snapshot a new
     * kernel PDE would not reach this clone, so map_mmio must not add one. */
    for (i = 0; i < 1024; i++) {
        new_dir->phys_page_tables[i] = _kernel_dir->phys_page_tables[i];
        new_dir->logical_page_tables[i] = _kernel_dir->logical_page_tables[i];
    }

    return new_dir;
}

uintptr_t get_page_dir_phys(page_dir_t *dir)
{
    /* CR3 takes the physical address of the entries, the directory's second
     * page; the kernel's own directory was carved below the window, where
     * that is the address the kernel holds, and every other lives in the
     * heap. */
    return table_phys(&dir->phys_page_tables);
}

void switch_page_directory(page_dir_t *dir)
{
    if (dir == _curr_dir) {
        return;
    }

    _curr_dir = dir;
    __asm__ __volatile__("mov %0, %%cr3" :: "r"(get_page_dir_phys(dir)) : "memory");
}

void frame_ref(uintptr_t phys)
{
    uintptr_t frame = phys / PAGE_SIZE;

    /* Only a live frame can gain a sharer, and the words at and above
     * `FRAME_REF_MAX` are states, not counts. */
    if (frame < _frames_number && _frames[frame] != 0 && _frames[frame] < FRAME_REF_MAX - 1) {
        _frames[frame]++;
    }
}

uint32_t frame_unref(uintptr_t phys)
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
    if (frame >= _user_frames_start && frame < _next_free_frame && _frames[frame] == 0) {
        _next_free_frame = frame;
    }
    return _frames[frame];
}

/**
 * Ensure `dir` has a PRIVATE (not kernel-shared) page table for
 * `table_index` and return it, or NULL on allocation failure. A table
 * inherited from the kernel directory by clone_page_directory is copied so
 * per-process mappings stay isolated; a missing table is allocated; an
 * existing private table is made user-accessible. Factored out of
 * alloc_user_page so map_user_frame (and thus copy-on-write) shares exactly
 * the same table-management logic.
 */
static page_table_t *ensure_user_table(page_dir_t *dir, uintptr_t table_index)
{
    page_table_t *table = dir->logical_page_tables[table_index];

    if (table && dir != _kernel_dir &&
        table == _kernel_dir->logical_page_tables[table_index]) {
        page_table_t *new_table = alloc_page_table();
        uint32_t i;

        if (!new_table) {
            klog(KLOG_ERR, "vm", "user table: the kernel page table did not copy");
            return NULL;
        }
        for (i = 0; i < 1024; i++) {
            new_table->pages[i] = table->pages[i];
        }
        dir->phys_page_tables[table_index] = table_phys(new_table) | 0x7; /* P+RW+U */
        dir->logical_page_tables[table_index] = new_table;
        table = new_table;
    } else if (!table) {
        table = alloc_page_table();
        if (!table) {
            klog(KLOG_ERR, "vm", "user table: no table could be allocated for index %d",
                          (int)table_index);
            return NULL;
        }
        dir->phys_page_tables[table_index] = table_phys(table) | 0x7; /* P+RW+U */
        dir->logical_page_tables[table_index] = table;
    } else if (!(dir->phys_page_tables[table_index] & 0x4)) {
        dir->phys_page_tables[table_index] |= 0x4; /* make table user-accessible */
    }
    return table;
}

int map_user_frame(page_dir_t *dir, uintptr_t virt, uintptr_t phys, uint32_t flags)
{
    uintptr_t page_num, table_index, page_index;
    page_table_t *table;

    virt &= ~(PAGE_SIZE - 1);
    page_num = virt / PAGE_SIZE;
    table_index = page_num / 1024;
    page_index = page_num % 1024;
    if (table_index >= 1024) {
        return -1;
    }

    table = ensure_user_table(dir, table_index);
    if (!table) {
        return -1;
    }

    /* The caller owns the frame and its reference count; we only write the
     * PTE. */
    table->pages[page_index] = (phys & ~0xFFFu) | (flags & 0xFFFu);
    if (dir == _curr_dir) {
        paging_flush_tlb(virt);
    }
    return 0;
}

uintptr_t alloc_user_page(page_dir_t *dir, uintptr_t virt_addr)
{
    uintptr_t phys_addr;
    uintptr_t page_num;
    uintptr_t table_index;
    uintptr_t page_index;
    uintptr_t frame;
    page_table_t *table;

    /* Align to page boundary */
    virt_addr &= ~(PAGE_SIZE - 1);

    /* Calculate table and page indices */
    page_num = virt_addr / PAGE_SIZE;
    table_index = page_num / 1024;
    page_index = page_num % 1024;

    if (table_index >= 1024) {
        klog(KLOG_ERR, "vm", "user page: table index %d is past the directory",
             (int)table_index);
        return 0;
    }

    /*
     * CRITICAL: ensure the page table FIRST, before finding a free frame.
     * This advances mem_reserve()'s pointer (if a new table is allocated)
     * before we scan for free frames, preventing the same physical address
     * from being used for both the page table and the user page.
     */
    table = ensure_user_table(dir, table_index);
    if (!table) {
        return 0;
    }

    /*
     * NOW find a free physical frame.
     * Start scanning from _user_frames_start (set at init time) to avoid
     * conflicts with kernel structures. This fixed starting point ensures
     * freed user pages can be reused, even if mem_reserved_ptr advances
     * due to later kernel allocations (like new page tables).
     */
    for (frame = _next_free_frame; frame < _frames_number; frame++) {
        if (_frames[frame] == 0) {
            break;
        }
    }
    if (framefail_now()) {
        frame = _frames_number;
    }

    if (frame >= _frames_number) {
        /* Who holds the pool, so that a machine that runs out says what
         * filled it: the boot's reservations below the user frames, the
         * kernel's frames from the top, and the user frames between, with
         * the shared ones counted apart. */
        uint32_t user_used = 0, user_shared = 0;
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
             "user page: no free frames in %d: %d boot, %d kernel, %d user of which %d shared, pid %d",
             (int)_frames_number, (int)_user_frames_start, (int)_kernel_frames_reserved,
             (int)user_used, (int)user_shared, current_proc ? current_proc->pid : 0);
        return 0;  /* No free frames */
    }

    /* Mark frame as used (one reference) */
    _frames[frame] = 1;
    _next_free_frame = frame + 1;
    phys_addr = frame * PAGE_SIZE;

    /* Map the page with present, rw, and user bits */
    table->pages[page_index] = phys_addr | 0x7;  /* P + RW + U/S */

    /* Flush TLB for this address if we're in the current directory */
    if (dir == _curr_dir) {
        paging_flush_tlb(virt_addr);
    }

    return phys_addr;
}

/**
 * Free all user pages in a page directory. Walks every page directory entry
 * and frees the user-accessible pages above the kernel region
 * (`USER_ADDR_MIN`), clearing the private page table entries as it goes.
 */
void free_user_pages(page_dir_t *dir)
{
    uint32_t dir_idx, page_idx;
    page_table_t *table;

    if (!dir) {
        return;
    }

    for (dir_idx = 0; dir_idx < 1024; dir_idx++) {
        table = dir->logical_page_tables[dir_idx];
        if (!table) {
            continue;
        }

        /* Skip shared kernel page tables */
        if (table == _kernel_dir->logical_page_tables[dir_idx]) {
            continue;
        }

        /* A private page table: check each page. */
        for (page_idx = 0; page_idx < 1024; page_idx++) {
            uint32_t pte = table->pages[page_idx];
            uint32_t virt_addr;
            uint32_t phys_addr;

            /* Present is bit 0 (0x1), U/S is bit 2 (0x4). */
            if (!(pte & 0x1) || !(pte & 0x4)) {
                continue;
            }

            virt_addr = (dir_idx << 22) | (page_idx << 12);
            phys_addr = pte & ~0xFFF;

            /* Skip identity-mapped kernel region (first 32MB) */
            if (virt_addr < HARD_MEMORY_LIMIT) {
                continue;
            }

            /* Sanity check: the physical address must be valid. Skip entries
             * that are clearly garbage:
             *   * phys_addr == 0 (NULL pointer)
             *   * phys_addr points to the page table itself (corruption) */
            if (phys_addr == 0 || phys_addr == table_phys(table)) {
                /* Clear the garbage entry but don't try to free frame */
                table->pages[page_idx] = 0;
                continue;
            }

            /* Drop this mapping's reference; the frame returns to the pool only
             * when its last reference (e.g. a COW-sharing sibling) is gone. */
            frame_unref(phys_addr);

            /* Clear page table entry */
            table->pages[page_idx] = 0;

            /* Flush TLB if this is the current directory */
            if (dir == _curr_dir) {
                paging_flush_tlb(virt_addr);
            }
        }
    }
}

void unmap_user_range(page_dir_t *dir, uint32_t start, uint32_t end)
{
    uint32_t addr;

    if (!dir) {
        return;
    }

    for (addr = start; addr < end; addr += PAGE_SIZE) {
        uint32_t page_num = addr / PAGE_SIZE;
        uint32_t table_index = page_num / 1024;
        uint32_t page_index = page_num % 1024;
        page_table_t *table;
        uint32_t pte, phys;

        if (table_index >= 1024) {
            continue;
        }
        table = dir->logical_page_tables[table_index];
        if (!table) {
            continue;
        }
        /* Never touch a page table shared with the kernel directory. */
        if (table == _kernel_dir->logical_page_tables[table_index]) {
            continue;
        }

        pte = table->pages[page_index];
        if (!(pte & 0x1)) {
            continue; /* not present: a never-faulted lazy page, nothing to free */
        }

        /* Drop the mapping's reference (frees the frame on the last
         * reference). */
        phys = pte & ~0xFFF;
        frame_unref(phys);
        table->pages[page_index] = 0;
        if (dir == _curr_dir) {
            paging_flush_tlb(addr);
        }
    }
}

/**
 * Free a page directory and all its private page tables. Call this after
 * free_user_pages() to release the directory structures themselves; the
 * directory and its private page tables go onto free lists for reuse.
 *
 * Must NOT be called on the kernel directory or the currently active
 * directory.
 */
void free_page_directory(page_dir_t *dir)
{
    uint32_t dir_idx;
    page_table_t *table;

    if (!dir || dir == _kernel_dir || dir == _curr_dir) {
        return;
    }

    /* Free all private page tables (not shared with kernel) */
    for (dir_idx = 0; dir_idx < 1024; dir_idx++) {
        table = dir->logical_page_tables[dir_idx];
        if (!table) {
            continue;
        }

        /* Skip shared kernel page tables */
        if (table == _kernel_dir->logical_page_tables[dir_idx]) {
            continue;
        }

        /* A private page table: add to the free list. */
        free_page_table(table);
        dir->logical_page_tables[dir_idx] = NULL;
        dir->phys_page_tables[dir_idx] = 0;
    }

    kfree_pages(dir, sizeof(page_dir_t) / PAGE_SIZE);
}
