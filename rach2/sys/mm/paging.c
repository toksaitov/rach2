#include "paging.h"

page_dir_t *_kernel_dir;
page_dir_t *_curr_dir;

uint8_t *_frames;
size_t   _frames_number;

static void _page_fault_handler(isr_params_t params);

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

    phys_address = frame = 0;
    while (phys_address < mem_reserved_ptr + 0x100000) { /* Extra 1MB for post-init allocations */
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

        if (frame == _frames_number) {
            k_panic("Failed to allocate a page frame. No more free frames.");
        } else {
            _frames[frame++] = 1;
        }

        /*
         * This page is writable only by a supervisor.
         * Page frame address | 'present' and 'rw' flags
         */
        table->pages[page_index] = phys_address | 0x3;

        phys_address += PAGE_SIZE;
    }

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

static void _page_fault_handler(isr_params_t params)
{
    uint32_t fault_address;
    __asm__ __volatile__("mov %%cr2, %0" : "=r"(fault_address));

    k_printf("Kernel message: page fault at 0x%X with error code 0x%X\n", fault_address, params.error_code);

    k_panic("Unrecoverable error: page fault while in kernel mode.");
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

    /* Allocate a new page directory */
    new_dir = (page_dir_t *) mem_reserve(sizeof(page_dir_t), true);
    if (!new_dir) {
        return NULL;
    }

    k_memset(new_dir, 0, sizeof(page_dir_t));

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
        return 0;
    }

    /* Find a free physical frame */
    for (frame = 0; frame < _frames_number; frame++) {
        if (_frames[frame] == 0) {
            break;
        }
    }

    if (frame >= _frames_number) {
        return 0;  /* No free frames */
    }

    /* Mark frame as used */
    _frames[frame] = 1;
    phys_addr = frame * PAGE_SIZE;

    /* Get or create page table */
    table = dir->logical_page_tables[table_index];

    /*
     * If the page table exists but is shared with the kernel directory
     * (inherited from clone_page_directory), we need to create a private
     * copy for this process to ensure memory isolation.
     */
    if (table && dir != _kernel_dir &&
        table == _kernel_dir->logical_page_tables[table_index]) {
        /* Create a copy of the page table for this directory */
        uintptr_t table_addr = mem_reserve(sizeof(page_table_t), true);
        page_table_t *new_table = (page_table_t *)table_addr;

        /* Copy existing entries */
        {
            uint32_t i;
            for (i = 0; i < 1024; i++) {
                new_table->pages[i] = table->pages[i];
            }
        }

        /* Update directory to use the new table */
        dir->phys_page_tables[table_index] = table_addr | 0x7;  /* P + RW + U/S */
        dir->logical_page_tables[table_index] = new_table;
        table = new_table;
    } else if (!table) {
        /* Allocate new page table */
        uintptr_t table_addr = mem_reserve(sizeof(page_table_t), true);
        table = (page_table_t *)table_addr;
        k_memset(table, 0, sizeof(page_table_t));

        /* Add to directory with present, rw, and user bits set */
        dir->phys_page_tables[table_index] = table_addr | 0x7;  /* P + RW + U/S */
        dir->logical_page_tables[table_index] = table;
    } else if (!(dir->phys_page_tables[table_index] & 0x4)) {
        /* Page table exists but not user-accessible, set U/S bit */
        dir->phys_page_tables[table_index] |= 0x4;
    }

    /* Map the page with present, rw, and user bits */
    table->pages[page_index] = phys_addr | 0x7;  /* P + RW + U/S */

    /* Flush TLB for this address if we're in the current directory */
    if (dir == _curr_dir) {
        paging_flush_tlb(virt_addr);
    }

    return phys_addr;
}
