#include "paging.h"
#include "../serial.h"

page_dir_t *_kernel_dir;
page_dir_t *_curr_dir;

uint8_t *_frames;
size_t   _frames_number;

/*
 * Free lists for page directories and page tables.
 * When a process exits, we add its structures to these lists for reuse.
 * The first pointer in the freed structure points to the next free entry.
 */
static page_dir_t   *_free_page_dirs = NULL;
static page_table_t *_free_page_tables = NULL;

/*
 * Starting frame index for user page allocation.
 * Set once at init time to mem_reserved_ptr / PAGE_SIZE.
 * This ensures freed user pages can be reused even if
 * mem_reserved_ptr advances due to kernel allocations.
 */
static uintptr_t _user_frames_start = 0;

/*
 * Allocate a page table from free list or mem_reserve.
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
        /*
         * Mark the frame as used in the bitmap.
         * This is critical: mem_reserve allocates memory after init_paging,
         * but doesn't update the frame bitmap. If we don't mark this frame
         * as used, alloc_user_page could allocate it for a user page,
         * overwriting our page table structure!
         */
        uintptr_t frame = (uintptr_t)table / PAGE_SIZE;
        if (frame < _frames_number) {
            _frames[frame] = 1;
        }
        k_memset(table, 0, sizeof(page_table_t));
    }
    return table;
}

/*
 * Add a page table to the free list.
 */
static void free_page_table(page_table_t *table)
{
    if (!table) return;
    *(page_table_t **)table = _free_page_tables;
    _free_page_tables = table;
}

/*
 * Allocate a page directory from free list or mem_reserve.
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
        /*
         * Mark the frames as used in the bitmap.
         * page_dir_t is 8KB (2 pages), so mark both frames.
         */
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

void init_paging(void)
{
    unsigned int frame;

    uintptr_t tmp, phys_address,
                   table_index,
                   page_index;

    page_table_t *table;

    _frames_number = HARD_MEMORY_LIMIT / PAGE_SIZE; /* ToDo: use real limit. */
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

static void _page_fault_handler(isr_params_t *params)
{
    uint32_t fault_address;
    __asm__ __volatile__("mov %%cr2, %0" : "=r"(fault_address));

    k_printf("Kernel message: page fault at 0x%X with error code 0x%X\n", fault_address, params->error_code);

    k_panic("Unrecoverable error: page fault while in kernel mode.");
}

/*
 * Map a physical MMIO region to the same virtual address (identity map).
 * Creates page table entries for accessing device registers above HARD_MEMORY_LIMIT.
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

    if (user) {
        /* Set U/S bit (bit 2) in both page table entry and directory entry */
        table->pages[page_index] |= 0x4;
        _curr_dir->phys_page_tables[table_index] |= 0x4;
    } else {
        /* Clear U/S bit */
        table->pages[page_index] &= ~0x4;
    }

    paging_flush_tlb(addr);
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

    /*
     * Copy kernel page table references.
     * Kernel page tables are shared between all processes - we only copy
     * the pointers, not the actual tables. This ensures kernel memory
     * is always accessible from any process.
     */
    for (i = 0; i < 1024; i++) {
        new_dir->phys_page_tables[i] = _kernel_dir->phys_page_tables[i];
        new_dir->logical_page_tables[i] = _kernel_dir->logical_page_tables[i];
    }

    return new_dir;
}

uintptr_t get_page_dir_phys(page_dir_t *dir)
{
    /*
     * The phys_page_tables array is at the start of the page_dir structure
     * and is what CR3 should point to.
     */
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
     * CRITICAL: Allocate or copy page table FIRST, before finding a free frame.
     * This ensures mem_reserve() advances mem_reserved_ptr before we scan for
     * free frames, preventing the same physical address from being used for
     * both the page table and the user page.
     */
    table = dir->logical_page_tables[table_index];

    /*
     * If the page table exists but is shared with the kernel directory
     * (inherited from clone_page_directory), we need to create a private
     * copy for this process to ensure memory isolation.
     */
    if (table && dir != _kernel_dir &&
        table == _kernel_dir->logical_page_tables[table_index]) {
        /* Create a copy of the page table for this directory */
        page_table_t *new_table = alloc_page_table();
        if (!new_table) {
            serial_printf("alloc_user_page: failed to copy kernel page table\n");
            return 0;
        }

        /* Copy existing entries */
        {
            uint32_t i;
            for (i = 0; i < 1024; i++) {
                new_table->pages[i] = table->pages[i];
            }
        }

        /* Update directory to use the new table */
        dir->phys_page_tables[table_index] = (uintptr_t)new_table | 0x7;  /* P + RW + U/S */
        dir->logical_page_tables[table_index] = new_table;
        table = new_table;
    } else if (!table) {
        /* Allocate new page table */
        table = alloc_page_table();
        if (!table) {
            serial_printf("alloc_user_page: failed to alloc new page table for idx %d\n", (int)table_index);
            return 0;
        }

        /* Add to directory with present, rw, and user bits set */
        dir->phys_page_tables[table_index] = (uintptr_t)table | 0x7;  /* P + RW + U/S */
        dir->logical_page_tables[table_index] = table;
    } else if (!(dir->phys_page_tables[table_index] & 0x4)) {
        /* Page table exists but not user-accessible, set U/S bit */
        dir->phys_page_tables[table_index] |= 0x4;
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

    /* Mark frame as used */
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

/*
 * Free all user pages in a page directory.
 *
 * Iterates through all page directory entries and frees user-accessible
 * pages that are above the kernel region (HARD_MEMORY_LIMIT).
 * Also clears private page table entries.
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

        /* This is a private page table - check each page */
        for (page_idx = 0; page_idx < 1024; page_idx++) {
            uint32_t pte = table->pages[page_idx];
            uint32_t virt_addr;
            uint32_t phys_addr;
            uint32_t frame;

            /* Check if page is present and user-accessible */
            if (!(pte & 0x1) || !(pte & 0x4)) {
                continue;
            }

            virt_addr = (dir_idx << 22) | (page_idx << 12);
            phys_addr = pte & ~0xFFF;

            /* Skip identity-mapped kernel region (first 32MB) */
            if (virt_addr < HARD_MEMORY_LIMIT) {
                continue;
            }

            /*
             * Sanity check: physical address must be valid.
             * Skip entries that are clearly garbage:
             * - phys_addr == 0 (NULL pointer)
             * - phys_addr points to the page table itself (corruption)
             */
            if (phys_addr == 0 || phys_addr == (uint32_t)table) {
                /* Clear the garbage entry but don't try to free frame */
                table->pages[page_idx] = 0;
                continue;
            }

            /* Mark frame as free */
            frame = phys_addr / PAGE_SIZE;
            if (frame < _frames_number) {
                _frames[frame] = 0;
            }

            /* Clear page table entry */
            table->pages[page_idx] = 0;

            /* Flush TLB if this is the current directory */
            if (dir == _curr_dir) {
                paging_flush_tlb(virt_addr);
            }
        }
    }
}

/*
 * Free a page directory and all its private page tables.
 *
 * This should be called after free_user_pages() to release the page
 * directory structures themselves. The directory and its private
 * page tables are added to free lists for reuse.
 *
 * Note: This must NOT be called on the kernel directory or the
 * currently active directory.
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

        /* This is a private page table - add to free list */
        free_page_table(table);
        dir->logical_page_tables[dir_idx] = NULL;
        dir->phys_page_tables[dir_idx] = 0;
    }

    /* Add the page directory itself to the free list */
    *(page_dir_t **)dir = _free_page_dirs;
    _free_page_dirs = dir;
}
