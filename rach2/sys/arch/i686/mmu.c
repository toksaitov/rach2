/*
 * mmu.c: the i686's page tables, under the shared paging of ../../mm/paging.c.
 *
 * A directory has 1024 entries, each naming a table of 1024 page entries.
 * The kernel's directory identity-maps the window below HARD_MEMORY_LIMIT
 * and has a table for every 4 MiB of the kernel's gigabyte, which every
 * directory cloned from it shares; a process's directory adds tables of its
 * own for user space. What a page entry holds is the shared code's business;
 * which table holds it is this file's.
 */

#include "mmu.h"
#include "tss.h"

#include "../arch.h"

#include "../../klog.h"
#include "../../mm/kheap.h"
#include "../../mm/paging.h"

/* Set once the heap exists: from then on page tables and directories come
 * from it and go back to it, and their physical addresses, which the
 * processor reads, come from the kernel directory's own mapping. Before
 * that the boot carves them below the window, where the two are equal. */
static int _heap_ready;

/* The physical address of a page table or directory the kernel reaches by
 * its virtual address. */
static uintptr_t table_phys(const void *table)
{
    return _heap_ready ? paging_virt_to_phys((uintptr_t)table) : (uintptr_t)table;
}

/**
 * A page table: from the heap once it exists, from the boot allocator
 * before.
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

    table = mem_reserve_ptr(sizeof(page_table_t), true);
    if (table) {
        /* mem_reserve marks the frame too once the table exists. */
        frame_take_boot((uintptr_t)table / PAGE_SIZE);
        k_memset(table, 0, sizeof(page_table_t));
    }
    return table;
}

/**
 * Give a page table back to the heap.
 */
static void free_page_table(page_table_t *table)
{
    if (!table)
        return;
    kfree_pages(table, 1);
}

/**
 * A page directory: from the heap once it exists, from the boot allocator
 * before.
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

    dir = mem_reserve_ptr(sizeof(page_dir_t), true);
    if (dir) {
        /* Mark the frames as used in the table. page_dir_t is 8KB (2 pages),
         * so mark both frames. */
        uintptr_t frame = (uintptr_t)dir / PAGE_SIZE;
        uintptr_t num_frames = (sizeof(page_dir_t) + PAGE_SIZE - 1) / PAGE_SIZE;
        uintptr_t i;

        for (i = 0; i < num_frames; i++) {
            frame_take_boot(frame + i);
        }
        k_memset(dir, 0, sizeof(page_dir_t));
    }
    return dir;
}

/** A physical address without PAE is 32 bits: the kernel reaches the first
 * 4 GiB and nothing above. */
uint64_t mmu_phys_reach(void)
{
    return FRAME_REACH_32;
}

/**
 * The frame table at the boot allocator's break, in the identity window,
 * where the kernel reaches it before paging is on.
 */
void *mmu_frame_table(struct frame_map *map, size_t bytes)
{
    (void)map;
    return mem_reserve_ptr(bytes, true);
}

/**
 * Build the kernel directory: the window identity-mapped, supervisor-only,
 * so the kernel reaches any physical address below `HARD_MEMORY_LIMIT`
 * directly, and a page table for every 4 MiB of the kernel's gigabyte (256
 * tables, a megabyte), shared by every directory cloned later, so a mapping
 * made there after the first fork is seen by every process. The identity
 * loop marks the frames below the break as it passes them; `init_paging`
 * marks them all once more after, for the tables carved at frames the loop
 * had already passed and for those carved after it.
 */
void mmu_init(void)
{
    uintptr_t tmp, phys_address, table_index, page_index, frame;
    page_table_t *table;

    _kernel_dir = mem_reserve_ptr(sizeof(page_dir_t), true);
    if (_kernel_dir == NULL) {
        k_panic("init_paging: no reserved memory for the kernel page directory");
    }
    k_memset(_kernel_dir, 0, sizeof(page_dir_t));
    _curr_dir = _kernel_dir;

    phys_address = frame = 0;
    while (phys_address < HARD_MEMORY_LIMIT) {
        tmp = phys_address / PAGE_SIZE;

        table_index = tmp / 1024;
        page_index = tmp % 1024;

        if (!(table = _curr_dir->logical_page_tables[table_index])) {
            tmp = mem_reserve(sizeof(page_table_t), true);
            if (tmp == 0) {
                k_panic("init_paging: no reserved memory for an identity-map "
                        "page table");
            }
            table = (page_table_t *)tmp;

            k_memset(table, 0, sizeof(page_table_t));

            /* This page table is writable only by a supervisor. */
            _curr_dir->phys_page_tables[table_index] = (uint32_t)tmp | PTE_P | PTE_W;

            _curr_dir->logical_page_tables[table_index] = table;
        }

        if (phys_address < mem_reserved_ptr) {
            frame_take_boot(frame);
        }
        frame++;

        /* This page is writable only by a supervisor. */
        table->pages[page_index] = (pte_t)phys_address | PTE_P | PTE_W;

        phys_address += PAGE_SIZE;
    }

    for (table_index = KVA_BASE / PAGE_SIZE / 1024; table_index < 1024; table_index++) {
        if (_curr_dir->logical_page_tables[table_index]) {
            continue;
        }
        tmp = mem_reserve(sizeof(page_table_t), true);
        if (tmp == 0) {
            k_panic("init_paging: no reserved memory for the kernel's page tables");
        }
        k_memset((void *)tmp, 0, sizeof(page_table_t));
        _curr_dir->phys_page_tables[table_index] = (uint32_t)tmp | PTE_P | PTE_W;
        _curr_dir->logical_page_tables[table_index] = (page_table_t *)tmp;
    }
}

/**
 * Load `dir` into CR3 and turn on paging and supervisor write protection.
 *
 * CR0.WP makes a ring-0 write to a read-only page fault instead of silently
 * succeeding; the page cache's dirty tracking depends on it, mapping a clean
 * frame read-only so the next write faults again and marks it dirty. A
 * kernel write to a user text page goes through a temporary mapping of its
 * frame, or is refused by the pre-fault, so it never faults. A kernel write
 * to a present copy-on-write page does fault: `cow_fault` makes the page
 * private and writable, and the instruction retries. WP affects nothing
 * else.
 */
static void _load_page_dir(page_dir_t *dir)
{
#ifndef RACH2_HOSTTEST
    uint32_t cr0;

    arch_space_load((uintptr_t)&dir->phys_page_tables);
    __asm__ __volatile__("mov %%cr0, %0" : "=r"(cr0));

    /* PG is bit 31 and WP bit 16. */
    cr0 |= 0x80000000u | 0x00010000u;

    __asm__ __volatile__("mov %0, %%cr0" ::"r"(cr0));
#else
    (void)dir;
#endif
}

/**
 * Turn translation on through the kernel directory.
 *
 * The double fault's task runs with the page directory its TSS names, which
 * the processor loads in place of the one in use, so the TSS is given the
 * kernel directory before paging is turned on (tss.c).
 */
void mmu_start(void)
{
    tss_set_double_fault_dir((uintptr_t)&_curr_dir->phys_page_tables);
    _load_page_dir(_curr_dir);
}

void mmu_heap_ready(void)
{
    _heap_ready = 1;
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
 *
 * The tables are counted rather than walked to an end address: `va_start +
 * size` at the top of the address space wraps below the start, and a loop
 * bounded by that sum would allocate nothing and report success. The refusal
 * is written as a subtraction from zero, which is the space above `va_start`
 * except when `va_start` is zero, where the whole space is above it.
 */
int premap_kernel_window(uintptr_t va_start, size_t size)
{
    uintptr_t addr;
    uintptr_t tables, i;

    if (va_start != 0 && size > (uintptr_t)0 - va_start) {
        return -1;
    }
    tables = size / (PAGE_SIZE * 1024) + ((size % (PAGE_SIZE * 1024)) ? 1u : 0u);

    for (i = 0; i < tables; i++) {
        uintptr_t table_index;
        page_table_t *table;

        addr = va_start + i * (PAGE_SIZE * 1024);
        table_index = (addr / PAGE_SIZE) / 1024;

        if (table_index >= 1024) {
            return -1;
        }
        if (!_kernel_dir->logical_page_tables[table_index]) {
            table = alloc_page_table();
            if (!table) {
                return -1;
            }
            _kernel_dir->logical_page_tables[table_index] = table;
            _kernel_dir->phys_page_tables[table_index] =
                (uint32_t)table_phys(table) | PTE_P | PTE_W;
        }
    }

    return 0;
}

void paging_set_user(uintptr_t addr, bool user)
{
    uintptr_t page_num = addr / PAGE_SIZE;
    uintptr_t table_index = page_num / 1024;
    uintptr_t page_index = page_num % 1024;
    page_table_t *table;

    if (table_index >= 1024)
        return;

    table = _curr_dir->logical_page_tables[table_index];
    if (!table)
        return;

    /* The user bit is set in both the page table and directory entry. */
    if (user) {
        table->pages[page_index] |= PTE_U;
        _curr_dir->phys_page_tables[table_index] |= PTE_U;
    } else {
        table->pages[page_index] &= ~PTE_U;
    }

    paging_flush_tlb(addr);
}

/**
 * A new page directory from the heap with every entry of the kernel
 * directory copied, or NULL when the heap has no room.
 *
 * Only the table pointers are copied, so the kernel's page tables are shared
 * and kernel memory is reachable from any process. A kernel PDE added after
 * the copy would not reach the clone, so `map_mmio` must not add one.
 */
page_dir_t *clone_page_directory(void)
{
    page_dir_t *new_dir;
    uint32_t i;

    new_dir = alloc_page_dir();
    if (!new_dir) {
        return NULL;
    }

    for (i = 0; i < 1024; i++) {
        new_dir->phys_page_tables[i] = _kernel_dir->phys_page_tables[i];
        new_dir->logical_page_tables[i] = _kernel_dir->logical_page_tables[i];
    }

    return new_dir;
}

/**
 * The address CR3 takes for `dir`: the physical address of its entries, the
 * directory's second page. For the kernel's own directory, carved below the
 * window, that is the address the kernel has; every other lives in the
 * heap, where the two differ.
 */
uintptr_t get_page_dir_phys(page_dir_t *dir)
{
    return table_phys(&dir->phys_page_tables);
}

void switch_page_directory(page_dir_t *dir)
{
    if (dir == _curr_dir) {
        return;
    }

    _curr_dir = dir;
#ifndef RACH2_HOSTTEST
    arch_space_load(get_page_dir_phys(dir));
#endif
}

/**
 * The first user address at or after `va` whose page table `dir` may have
 * of its own, as ../../mm/paging.h describes: `va`, or the next 4 MiB
 * boundary where the directory has no table of the process's own.
 */
uintptr_t mmu_user_skip(page_dir_t *dir, uintptr_t va)
{
    if (mmu_user_table(dir, va, 0)) {
        return va;
    }
    return (va & ~(MMU_TABLE_SPAN - 1)) + MMU_TABLE_SPAN;
}

/**
 * The leaf table of `dir` for the user address `va`, as ../../mm/paging.h
 * describes. A table `clone_page_directory` shares with the kernel
 * directory is never one of a process's own: with `create` it is copied,
 * so per-process mappings stay isolated; a missing table is allocated; an
 * existing private table is made user-accessible in the directory.
 */
pte_t *mmu_user_table(page_dir_t *dir, uintptr_t va, int create)
{
    uintptr_t table_index = va / PAGE_SIZE / 1024;
    page_table_t *table = dir->logical_page_tables[table_index];
    page_table_t *shared = _kernel_dir->logical_page_tables[table_index];

    if (!create) {
        return table && table != shared ? table->pages : NULL;
    }
    if (table && dir != _kernel_dir && table == shared) {
        page_table_t *new_table = alloc_page_table();
        uint32_t i;

        if (!new_table) {
            klog(KLOG_ERR, "vm", "user table: the kernel page table did not copy");
            return NULL;
        }
        for (i = 0; i < 1024; i++) {
            new_table->pages[i] = table->pages[i];
        }
        dir->phys_page_tables[table_index] =
            (uint32_t)table_phys(new_table) | PTE_P | PTE_W | PTE_U;
        dir->logical_page_tables[table_index] = new_table;
        table = new_table;
    } else if (!table) {
        table = alloc_page_table();
        if (!table) {
            klog(KLOG_ERR, "vm", "user table: no table could be allocated for index %d",
                 (int)table_index);
            return NULL;
        }
        dir->phys_page_tables[table_index] = (uint32_t)table_phys(table) | PTE_P | PTE_W | PTE_U;
        dir->logical_page_tables[table_index] = table;
    } else if (!(dir->phys_page_tables[table_index] & PTE_U)) {
        dir->phys_page_tables[table_index] |= PTE_U; /* make table user-accessible */
    }
    return table->pages;
}

pte_t *mmu_kernel_table(uintptr_t va)
{
    page_table_t *table = _kernel_dir->logical_page_tables[va / PAGE_SIZE / 1024];

    return table ? table->pages : NULL;
}

uintptr_t paging_virt_to_phys(uintptr_t va)
{
    pte_t *table = mmu_kernel_table(va);
    pte_t *e = table ? &table[MMU_INDEX(va)] : NULL;

    if (!e || !(*e & PTE_P)) {
        return 0;
    }
    return PTE_ADDR(*e) | (va & (PAGE_SIZE - 1));
}

/**
 * Free all user pages in a page directory. Walks every private page table
 * and drops the frame reference of each present user page above the
 * identity-mapped window, where user space starts, clearing its entry; a
 * frame goes back to the pool with its last reference. An entry naming frame
 * 0 or the page table itself is corrupt and is cleared without touching a
 * frame. The page tables stay, for `free_page_directory`.
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
            pte_t pte = table->pages[page_idx];
            uintptr_t virt_addr;
            uintptr_t phys_addr;

            /* A PROT_NONE page is a user page without the user bit. */
            if (!(pte & PTE_P) || !(pte & (PTE_U | PTE_NONE))) {
                continue;
            }

            virt_addr = ((uintptr_t)dir_idx << 22) | ((uintptr_t)page_idx << 12);
            phys_addr = PTE_ADDR(pte);

            /* Skip identity-mapped kernel region (first 32MB) */
            if (virt_addr < HARD_MEMORY_LIMIT) {
                continue;
            }

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

            paging_flush_dir(dir, virt_addr);
        }
    }
}

/**
 * Free a page directory and all its private page tables. Call this after
 * free_user_pages() to release the directory structures themselves; the
 * directory and its private page tables go back to the heap.
 *
 * The kernel directory and the active one are refused and so left in place:
 * a caller switches to another directory first.
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

        /* A private page table: back to the heap. */
        free_page_table(table);
        dir->logical_page_tables[dir_idx] = NULL;
        dir->phys_page_tables[dir_idx] = 0;
    }

    kfree_pages(dir, sizeof(page_dir_t) / PAGE_SIZE);
}
