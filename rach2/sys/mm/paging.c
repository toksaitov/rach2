#include "paging.h"
#include "frame.h"
#include "usercopy.h"
#include "../serial.h"
#include "../lib/ksignal.h"
#include "../proc/process.h"

/**
 * Defined in kernel.c: tear the current user process down with a signal death
 * (`signo`; SIGSEGV here): free its pages, wake a waiting parent, and either
 * resume the kernel debug shell (if it was init) or schedule the parent (if it
 * was a child). Kills a process that faulted illegally without taking the machine
 * down. Mirrors the CTRL+C kill path.
 */
extern void terminate_current_process(int32_t signo);

page_dir_t *_kernel_dir;
page_dir_t *_curr_dir;

uint8_t *_frames;
size_t   _frames_number;

/**
 * Free lists for page directories and page tables. When a process exits, its
 * structures go onto these lists for reuse. The first pointer in the freed
 * structure points to the next free entry.
 */
static page_dir_t   *_free_page_dirs = NULL;
static page_table_t *_free_page_tables = NULL;

/**
 * Starting frame index for user page allocation. Set once at init time to
 * `mem_reserved_ptr / PAGE_SIZE`, so freed user pages can be reused even after
 * `mem_reserved_ptr` advances due to later kernel allocations.
 */
static uintptr_t _user_frames_start = 0;

/**
 * Allocate a page table from the free list, or fresh via `mem_reserve`.
 */
static page_table_t *alloc_page_table(void)
{
    page_table_t *table;

    if (_free_page_tables) {
        table = _free_page_tables;
        _free_page_tables = *(page_table_t **)table;
        k_memset(table, 0, sizeof(page_table_t));
        return table;
    }

    table = (page_table_t *)mem_reserve(sizeof(page_table_t), true);
    if (table) {
        /* Mark the frame as used in the bitmap. mem_reserve allocates memory
         * after init_paging but does not update the frame bitmap; without this,
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
    *(page_table_t **)table = _free_page_tables;
    _free_page_tables = table;
}

/**
 * Allocate a page directory from the free list, or fresh via `mem_reserve`.
 */
static page_dir_t *alloc_page_dir(void)
{
    page_dir_t *dir;

    if (_free_page_dirs) {
        dir = _free_page_dirs;
        _free_page_dirs = *(page_dir_t **)dir;
        k_memset(dir, 0, sizeof(page_dir_t));
        return dir;
    }

    dir = (page_dir_t *)mem_reserve(sizeof(page_dir_t), true);
    if (dir) {
        /* Mark the frames as used in the bitmap. page_dir_t is 8KB (2 pages),
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

void init_paging(size_t usable_frames)
{
    unsigned int frame;

    uintptr_t tmp, phys_address,
                   table_index,
                   page_index;

    page_table_t *table;

    /* frame.h mirrors these two values so it can stay host-linkable; fail loudly
     * if the mirror ever drifts from the real definitions. */
    if (FRAME_DIRECT_MAP_LIMIT != HARD_MEMORY_LIMIT || FRAME_PAGE_SIZE != PAGE_SIZE) {
        k_panic("frame.h constants drifted from paging.h");
    }

    /* Size the frame pool from the multiboot memory map, capped to the
     * direct-mapped window (computed by frame_usable_count in k_main). Falls
     * back to the full window if the bootloader gave us nothing usable. */
    _frames_number = usable_frames ? usable_frames : (HARD_MEMORY_LIMIT / PAGE_SIZE);
    _frames = (uint8_t *) mem_reserve(_frames_number, true);
    k_memset(_frames, 0, _frames_number);

    _kernel_dir = (page_dir_t *) mem_reserve(sizeof(page_dir_t), true);
    k_memset(_kernel_dir, 0, sizeof(page_dir_t));
    _curr_dir = _kernel_dir;

    /*
     * Identity map all physical memory up to HARD_MEMORY_LIMIT.
     * This allows kernel code to access any physical address directly.
     * Only frames below mem_reserved_ptr are marked as "in use" in the bitmap;
     * frames above remain free for user page allocation.
     */
    phys_address = frame = 0;
    while (phys_address < HARD_MEMORY_LIMIT) {
        tmp = phys_address / PAGE_SIZE;

        table_index = tmp / 1024;
        page_index  = tmp % 1024;

        if (!(table = _curr_dir->logical_page_tables[table_index])) {
            tmp = mem_reserve(sizeof(page_table_t), true);
            table = (page_table_t *) tmp;

            k_memset(table, 0, sizeof(page_table_t));

            /* This page table is writable only by a supervisor.
             * Page table address | 'present' and 'rw' flags */
            _curr_dir->phys_page_tables[table_index] = tmp | 0x3;

            _curr_dir->logical_page_tables[table_index] = table;
        }

        /* Mark frames below mem_reserved_ptr as used (kernel, heap, modules) */
        if (phys_address < mem_reserved_ptr) {
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

    register_isr_handler(14, &_page_fault_handler);

    _load_page_dir(_curr_dir);
}

static void _load_page_dir(page_dir_t *dir)
{
    uint32_t cr0;

    __asm__ __volatile__ ("mov %0, %%cr3" :: "r"(&dir->phys_page_tables));
    __asm__ __volatile__ ("mov %%cr0, %0" : "=r"(cr0));

    cr0 |= 0x80000000;

    __asm__ __volatile__ ("mov %0, %%cr0" :: "r"(cr0));
}

/* Page-fault error-code bits (Intel SDM Vol. 3A, 4.7). */
#define PF_ERR_PRESENT 0x1u /* 0 = not-present page, 1 = protection violation */
#define PF_ERR_WRITE   0x2u /* 0 = read access, 1 = write access */
#define PF_ERR_USER    0x4u /* 0 = supervisor mode, 1 = user mode */


/**
 * Is the page containing `addr` already present in `dir`? Keeps the demand-paging
 * resolver idempotent: an eager-loaded code/data page, or one the pre-fault path
 * already backed, must not be allocated a second time (which would leak the old
 * frame and wipe the page's contents).
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
 * page is backed. `write` is 1 for a write. Returns 1 if the page is now mapped,
 * 0 if the access is illegal (no VMA covers it and it is not a valid stack
 * growth, or it writes a read-only region); so the caller faults or segfaults.
 *
 * The current process always has a user address space here (the handler and the
 * pre-fault path both gate on `current_proc->cr3 != 0`).
 */
static int vm_resolve(uint32_t addr, int write)
{
    vm_space_t *vm = &current_proc->vm;
    vma_t *vma = vm_find(vm, addr);
    uintptr_t phys;

    if (!vma) {
        /* The one legal access outside every region is growing the stack down. */
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
    k_memset((void *)phys, 0, PAGE_SIZE);

    /* Honor a read-only region's protection in the PTE, so a later write faults
     * as a protection violation, which the handler segfaults, and which
     * copy-on-write uses to tell a write fault from a demand fault. A grown stack
     * page (vma == NULL here) is left writable. */
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

    /* Callers range-check with user_range_ok first, so addr + len does not
     * overflow past USER_ADDR_MAX. */
    end = addr + len;
    for (page = addr & ~(PAGE_SIZE - 1); page < end; page += PAGE_SIZE) {
        if (!vm_resolve(page, write)) {
            return 0;
        }
    }
    return 1;
}

/**
 * Copy-on-write resolver for a write fault on a present, PTE_COW page in the
 * current address space. If this process is the frame's last owner (refcount 1)
 * the page is just made writable in place; otherwise a private writable copy is
 * allocated, the shared bytes are copied in, and the shared frame's reference is
 * dropped. Returns 1 if it handled a COW page, 0 if the page is not COW (the
 * caller continues to demand paging / segfault).
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
    k_memcpy((void *)new_phys, (void *)old_phys, PAGE_SIZE);
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

    /* Demand paging: a fault on a user-space address owned by a live user process
     * is resolved by lazily backing the page (heap/stack/anon) or growing the
     * stack. Keyed on the faulting *address* being in user space, not on the CPU
     * mode, so a kernel touch of a lazy user page would resolve here too. */
    if (have_user_proc && in_user_range) {
        if (vm_resolve(fault_address, write)) {
            return; /* page mapped; iret retries the faulting instruction */
        }
    }

    /* Unrecoverable. A fault taken in user mode is the process's own bug: kill
     * just that process and survive; terminate_current_process returns to the
     * shell if it was init, or schedules the parent to collect the zombie. */
    if (user_mode && have_user_proc) {
        serial_printf("Page fault: killing pid %d: bad %s at 0x%x (err 0x%x)\n",
                      current_proc->pid, write ? "write" : "read", fault_address,
                      params->error_code);
        terminate_current_process(SIGSEGV);
        return; /* child case: the parent was scheduled; nothing to retry */
    }

    /* A fault in kernel mode (or with no user context) is a kernel bug. The
     * fault-aware usercopy path returns -EFAULT before the kernel ever touches a
     * bad user pointer, so reaching here means the kernel itself faulted. */
    k_printf("Kernel message: page fault at 0x%X with error code 0x%X\n",
             fault_address, params->error_code);

    k_panic("Unrecoverable error: page fault while in kernel mode.");
}

/**
 * Map a physical MMIO region to the same virtual address (identity map). Creates
 * the page table entries for accessing device registers above
 * `HARD_MEMORY_LIMIT`.
 */
uintptr_t map_mmio(uintptr_t phys_addr, size_t size)
{
    uintptr_t start_page = phys_addr & ~(PAGE_SIZE - 1);
    uintptr_t end_addr = phys_addr + size;
    uintptr_t addr;

    for (addr = start_page; addr < end_addr; addr += PAGE_SIZE) {
        uintptr_t page_num = addr / PAGE_SIZE;
        uintptr_t table_index = page_num / 1024;
        uintptr_t page_index = page_num % 1024;
        page_table_t *table;

        if (table_index >= 1024) {
            serial_printf("map_mmio: address 0x%x out of range\n", addr);
            return 0;
        }

        /* Get or create page table */
        table = _kernel_dir->logical_page_tables[table_index];
        if (!table) {
            table = alloc_page_table();
            if (!table) {
                serial_printf("map_mmio: failed to allocate page table\n");
                return 0;
            }
            _kernel_dir->logical_page_tables[table_index] = table;
            _kernel_dir->phys_page_tables[table_index] = (uintptr_t)table | 0x3;
        }

        /* Map page: present, writable, cache-disabled for MMIO */
        table->pages[page_index] = addr | 0x1B;  /* P=1, R/W=1, PWT=1, PCD=1 */
    }

    /* Flush TLB */
    __asm__ __volatile__(
        "movl %%cr3, %%eax\n\t"
        "movl %%eax, %%cr3"
        ::: "eax", "memory"
    );

    return phys_addr;
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
     * keeps kernel memory reachable from any process. */
    for (i = 0; i < 1024; i++) {
        new_dir->phys_page_tables[i] = _kernel_dir->phys_page_tables[i];
        new_dir->logical_page_tables[i] = _kernel_dir->logical_page_tables[i];
    }

    return new_dir;
}

uintptr_t get_page_dir_phys(page_dir_t *dir)
{
    /* The phys_page_tables array is at the start of the page_dir structure and
     * is what CR3 should point to. */
    return (uintptr_t)&dir->phys_page_tables;
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

    /* Only a live frame can gain a sharer; cap at the byte's range so a runaway
     * count cannot wrap to 0 (which would read as "free"). */
    if (frame < _frames_number && _frames[frame] != 0 && _frames[frame] < 0xFF) {
        _frames[frame]++;
    }
}

uint32_t frame_unref(uintptr_t phys)
{
    uintptr_t frame = phys / PAGE_SIZE;

    if (frame >= _frames_number || _frames[frame] == 0) {
        return 0;
    }
    /* A frame whose count ever saturated (frame_ref caps at 0xFF) has lost its
     * exact share count, so decrementing it could drop below the real number of
     * mappings and free a still-live frame. Pin it instead; leaking a frame is
     * safe, a use-after-free is not. (Unreachable while MAX_PROCS <= 255 bounds
     * sharing far below the cap; this is a guard, not a hot path.) */
    if (_frames[frame] == 0xFF) {
        return 0xFF;
    }
    _frames[frame]--;
    return _frames[frame];
}

/**
 * Ensure `dir` has a PRIVATE (not kernel-shared) page table for `table_index` and
 * return it, or NULL on allocation failure. A table inherited from the kernel
 * directory by clone_page_directory is copied so per-process mappings stay
 * isolated; a missing table is allocated; an existing private table is made
 * user-accessible. Factored out of alloc_user_page so map_user_frame (and thus
 * copy-on-write) shares exactly the same table-management logic.
 */
static page_table_t *ensure_user_table(page_dir_t *dir, uintptr_t table_index)
{
    page_table_t *table = dir->logical_page_tables[table_index];

    if (table && dir != _kernel_dir &&
        table == _kernel_dir->logical_page_tables[table_index]) {
        page_table_t *new_table = alloc_page_table();
        uint32_t i;

        if (!new_table) {
            serial_printf("ensure_user_table: failed to copy kernel page table\n");
            return NULL;
        }
        for (i = 0; i < 1024; i++) {
            new_table->pages[i] = table->pages[i];
        }
        dir->phys_page_tables[table_index] = (uintptr_t)new_table | 0x7; /* P+RW+U */
        dir->logical_page_tables[table_index] = new_table;
        table = new_table;
    } else if (!table) {
        table = alloc_page_table();
        if (!table) {
            serial_printf("ensure_user_table: failed to alloc table for idx %d\n",
                          (int)table_index);
            return NULL;
        }
        dir->phys_page_tables[table_index] = (uintptr_t)table | 0x7; /* P+RW+U */
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

    /* The caller owns the frame and its reference count; we only write the PTE. */
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
        serial_printf("alloc_user_page: table_index %d >= 1024\n", (int)table_index);
        return 0;
    }

    /*
     * CRITICAL: ensure the page table FIRST, before finding a free frame. This
     * advances mem_reserve()'s pointer (if a new table is allocated) before we
     * scan for free frames, preventing the same physical address from being used
     * for both the page table and the user page.
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
    for (frame = _user_frames_start; frame < _frames_number; frame++) {
        if (_frames[frame] == 0) {
            break;
        }
    }

    if (frame >= _frames_number) {
        serial_printf("alloc_user_page: no free frames! checked %d frames\n", (int)_frames_number);
        return 0;  /* No free frames */
    }

    /* Mark frame as used (one reference) */
    _frames[frame] = 1;
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
 * Free all user pages in a page directory. Walks every page directory entry and
 * frees the user-accessible pages above the kernel region (`HARD_MEMORY_LIMIT`),
 * clearing the private page table entries as it goes.
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
            if (phys_addr == 0 || phys_addr == (uint32_t)table) {
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

        /* Drop the mapping's reference (frees the frame on the last reference). */
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
 * free_user_pages() to release the directory structures themselves; the directory
 * and its private page tables go onto free lists for reuse.
 *
 * Must NOT be called on the kernel directory or the currently active directory.
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

    /* Add the page directory itself to the free list */
    *(page_dir_t **)dir = _free_page_dirs;
    _free_page_dirs = dir;
}
