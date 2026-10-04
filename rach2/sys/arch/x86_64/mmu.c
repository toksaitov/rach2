/*
 * mmu.c: the x86-64's page tables, under the shared paging of
 * ../../mm/paging.c.
 *
 * The kernel's PML4 has the upper half: the direct map of all physical
 * memory, the kernel's windows and the image (cpu.h). Every upper-half
 * PML4 entry the kernel will use is made here, at boot, and a process's
 * PML4 is a copy of those 256 entries, so the tables below them are shared
 * and a mapping made in a window later is seen by every process. A
 * process's own tables are the lower half's. What a page entry holds is the
 * shared code's business; which table holds it is this file's.
 */

#include "mmu.h"
#include "percpu.h"

#include "../arch.h"
#include "../x86/kcpu.h"

#include "../../klog.h"
#include "../../panic.h"
#include "../../mm/heap.h"
#include "../../mm/paging.h"
#include "../../lib/kstring.h"

pte_t mmu_nx;

/* Set once the frame allocator is counted: from then on a table is a frame
 * from anywhere the map offers, reached through the direct map and given
 * back when the table goes. Before, the boot carves one below the window,
 * and it stays for good. */
static int _heap_ready;

/* The firmware's memory while the boot carves, which a carved page must lie
 * in: on a machine too small for the kernel and its modules the break is
 * past the memory, and a table there would read back as garbage. */
static const struct frame_map *_boot_map;

#define GIB 0x40000000UL
#define MIB 0x100000UL

/* The entry an address takes in the table of `level`: 4 for the PML4, 3
 * for a page directory pointer table, 2 for a page directory, 1 for a page
 * table. */
#define LEVEL_INDEX(va, level) (((uintptr_t)(va) >> (3 + 9 * (level))) & 511u)

/* The first PML4 entry of the upper half, and the kernel's windows: the
 * entries from KVA_HEAP's for 4 TiB, which exist from the boot whether a
 * window uses them yet or not, so that premap_kernel_window can place one
 * there later. */
#define PML4_UPPER        256u
#define PML4_WINDOWS      LEVEL_INDEX(KVA_HEAP, 4)
#define PML4_WINDOW_COUNT 8u

/* The image's bounds (link.ld): text from _text to _etext, read-only data
 * from _rodata, data and .bss from _data to _end, which is physical. */
extern char _text[], _etext[], _rodata[], _data[];

/**
 * A zeroed page for a table, by physical address; 0 when there is none.
 * A page the boot carves past the memory the map offers refuses the machine
 * by name, as `init_paging` refuses one too small for the frame table.
 */
static uintptr_t table_alloc(void)
{
    uintptr_t phys;

    if (_heap_ready) {
        phys = alloc_frame();
    } else {
        phys = mem_reserve(PAGE_SIZE, true);
        if (phys && _boot_map && _boot_map->top_frame &&
            !frame_map_usable(_boot_map, phys / PAGE_SIZE)) {
            klog(KLOG_ERR, "memory",
                 "refused: the kernel's page tables need memory past what the map offers after "
                 "the kernel's reservations");
            k_panic("memory: too little for this kernel");
        }
        if (phys) {
            frame_take_boot(phys / PAGE_SIZE);
        }
    }
    if (phys) {
        k_memset(MMU_VIRT(phys), 0, PAGE_SIZE);
    }
    return phys;
}

/**
 * The table of `level` under `pml4` that holds the entry of `va`, or NULL.
 *
 * With `create`, a missing table on the way is made, and the entry naming it
 * gets `flags` with the present bit; NULL then when there was no memory. A
 * large page on the way has no table below it, and gives NULL too.
 */
static pte_t *table_for(pte_t *pml4, uintptr_t va, int level, int create, pte_t flags)
{
    pte_t *table = pml4;
    int l;

    for (l = 4; l > level; l--) {
        pte_t *e = &table[LEVEL_INDEX(va, l)];

        if (!(*e & PTE_P)) {
            uintptr_t phys;

            if (!create) {
                return NULL;
            }
            phys = table_alloc();
            if (!phys) {
                return NULL;
            }
            *e = phys | PTE_P | flags;
        } else if (*e & PTE_PS) {
            return NULL;
        }
        table = MMU_VIRT(PTE_ADDR(*e));
    }
    return table;
}

/* Make every page table of [va, va + size) in the kernel's PML4, or panic:
 * for the boot's windows. */
static void boot_tables(uintptr_t va, uintptr_t size)
{
    uintptr_t at;

    for (at = va; at - va < size; at += MMU_TABLE_SPAN) {
        if (!table_for(_kernel_dir->entries, at, 1, 1, PTE_W)) {
            k_panic("init_paging: no reserved memory for the kernel's page tables");
        }
    }
}

/* Map the image's pages [from, to), virtual, to their physical addresses
 * with `flags`, at boot. */
static void map_image(uintptr_t from, uintptr_t to, pte_t flags)
{
    uintptr_t va;

    for (va = from & ~(uintptr_t)(PAGE_SIZE - 1); va < to; va += PAGE_SIZE) {
        pte_t *table = table_for(_kernel_dir->entries, va, 1, 1, PTE_W);

        if (!table) {
            k_panic("init_paging: no reserved memory for the image's page tables");
        }
        table[LEVEL_INDEX(va, 1)] = (va - KVA_IMAGE) | PTE_P | flags;
    }
}

/**
 * Build the kernel's PML4 with the direct map and the image.
 *
 * The direct map covers physical memory to the top of the frame table and
 * at least the first 4 GiB, where the firmware's tables and the devices'
 * registers are, in 1 GiB pages where CPUID says the processor has them
 * (leaf 0x80000001, EDX bit 26) and in 2 MiB pages where it does not. Its
 * pages are writable and never executable. The image is mapped in 4 KiB
 * pages: its text read-only, its read-only data read-only and not
 * executable, its data and .bss writable and not executable.
 *
 * This PML4 has nothing in the lower half: the loader's tables, which map
 * the first 4 GiB at 0 too, are left once it is loaded.
 */
static void map_direct_and_image(void)
{
    uint32_t max_ext = 0, edx = 0;
    uintptr_t top, at, i;
    int gib_pages;

    if (msr_read(MSR_EFER) & EFER_NXE) {
        mmu_nx = PTE_NX;
    }
    kcpu_id(0x80000000u, &max_ext, NULL, NULL, NULL);
    if (max_ext >= 0x80000001u) {
        kcpu_id(0x80000001u, NULL, NULL, NULL, &edx);
    }
    gib_pages = (edx & (1u << 26)) != 0;

    at = table_alloc();
    if (at == 0) {
        k_panic("init_paging: no reserved memory for the kernel page directory");
    }
    _kernel_dir = MMU_VIRT(at);
    _curr_dir = _kernel_dir;

    top = (uintptr_t)_frames_number * PAGE_SIZE;
    if (top < 4 * GIB) {
        top = 4 * GIB;
    }
    top = (top + GIB - 1) & ~(GIB - 1);
    if (top > KVA_DIRECT_SIZE) {
        top = KVA_DIRECT_SIZE;
    }
    for (at = 0; at < top; at += GIB) {
        pte_t *pdpt = table_for(_kernel_dir->entries, KVA_DIRECT + at, 3, 1, PTE_W);
        pte_t *pd;
        uintptr_t pd_phys;

        if (!pdpt) {
            k_panic("init_paging: no reserved memory for the direct map");
        }
        if (gib_pages) {
            pdpt[LEVEL_INDEX(KVA_DIRECT + at, 3)] = at | PTE_P | PTE_W | PTE_PS | mmu_nx;
            continue;
        }
        pd_phys = table_alloc();
        if (pd_phys == 0) {
            k_panic("init_paging: no reserved memory for the direct map");
        }
        pd = MMU_VIRT(pd_phys);
        for (i = 0; i < MMU_TABLE_ENTRIES; i++) {
            pd[i] = (at + i * 2 * MIB) | PTE_P | PTE_W | PTE_PS | mmu_nx;
        }
        pdpt[LEVEL_INDEX(KVA_DIRECT + at, 3)] = pd_phys | PTE_P | PTE_W;
    }

    map_image((uintptr_t)_text, (uintptr_t)_etext, 0);
    map_image((uintptr_t)_rodata, (uintptr_t)_data, mmu_nx);
    map_image((uintptr_t)_data, KVA_IMAGE + (uintptr_t)&_end, PTE_W | mmu_nx);
}

/** The direct map's end: every physical address below it has a kernel
 * address, and none above it does. */
uint64_t mmu_phys_reach(void)
{
    return KVA_DIRECT_SIZE;
}

/**
 * Build the kernel's PML4 with the direct map and the image and load it, so
 * that all of memory is reached from here, and place the frame table at the
 * top of the highest usable range above the window that holds it, out of
 * the window the boot's tables and the DMA pool share. A machine with no
 * room for it above the window has it carved at the break, as on i686.
 */
void *mmu_frame_table(struct frame_map *map, size_t bytes)
{
    uint64_t at;

    _boot_map = map;
    map_direct_and_image();
    arch_space_load(get_page_dir_phys(_kernel_dir));
    if (frame_map_place(map, bytes, HARD_MEMORY_LIMIT, &at) == 0) {
        return phys_to_virt(at);
    }
    return mem_reserve_ptr(bytes, true);
}

/**
 * Give the kernel's windows their PML4 entries and every page table of the
 * regions cpu.h gives them, as on i686, so that map_mmio and the temporary
 * mappings write entries that exist. The heap's tables are made for the
 * size the heap is given, by `kheap_init` (mm/kheap.c), since the region's
 * PML4 entry has room for far more than one machine's heap.
 */
void mmu_init(void)
{
    uintptr_t i;

    for (i = 0; i < PML4_WINDOW_COUNT; i++) {
        uintptr_t pdpt = table_alloc();

        if (pdpt == 0) {
            k_panic("init_paging: no reserved memory for the kernel's page tables");
        }
        _kernel_dir->entries[PML4_WINDOWS + i] = pdpt | PTE_P | PTE_W;
    }
    boot_tables(KVA_MMIO, KVA_MMIO_SIZE);
    boot_tables(KVA_MODULES, KVA_MODULES_SIZE);
}

/**
 * Translate through the kernel's PML4, which `mmu_frame_table` loaded
 * already, so this loads it once more. The loader already turned paging,
 * PAE, long mode and write protection on (loader.s); CR0.WP makes a ring-0
 * write to a read-only page fault, which the page cache's dirty tracking
 * and copy-on-write depend on, as on i686.
 */
void mmu_start(void)
{
    arch_space_load(get_page_dir_phys(_kernel_dir));
}

void mmu_heap_ready(void)
{
    _boot_map = NULL;
    _heap_ready = 1;
}

/**
 * Make every page table spanning [`va_start`, `va_start` + `size`) in the
 * kernel's PML4, leaving their entries not present, so that
 * `map_kernel_frame` can write one there.
 *
 * Only a PML4 entry the boot made can take new tables (`mmu_init`): one made
 * after the first process would be missing from its copy. Returns 0, or -1
 * for a range outside those entries, one that runs past the end of the
 * address space, or a table that could not be had.
 */
int premap_kernel_window(uintptr_t va_start, size_t size)
{
    uintptr_t at, last;

    if (size == 0) {
        return 0;
    }
    if (va_start < KVA_BASE || size - 1 > ~va_start) {
        return -1;
    }
    last = (va_start + (size - 1)) & ~(MMU_TABLE_SPAN - 1);
    for (at = va_start & ~(MMU_TABLE_SPAN - 1);; at += MMU_TABLE_SPAN) {
        if (!(_kernel_dir->entries[LEVEL_INDEX(at, 4)] & PTE_P) ||
            !table_for(_kernel_dir->entries, at, 1, 1, PTE_W)) {
            return -1;
        }
        if (at == last) {
            return 0;
        }
    }
}

/**
 * Mark the page of `addr` in the directory in use as reachable from user
 * mode, or not: the leaf entry, and with `user` every entry above it too,
 * since the processor asks for the user bit at each level. The debug
 * shell's ring-3 exercise uses it on the kernel's own pages.
 */
void paging_set_user(uintptr_t addr, bool user)
{
    pte_t *table = _curr_dir->entries;
    int l;

    for (l = 4; l > 1; l--) {
        pte_t *e = &table[LEVEL_INDEX(addr, l)];

        if (!(*e & PTE_P) || (*e & PTE_PS)) {
            return;
        }
        if (user) {
            *e |= PTE_U;
        }
        table = MMU_VIRT(PTE_ADDR(*e));
    }
    if (user) {
        table[LEVEL_INDEX(addr, 1)] |= PTE_U;
    } else {
        table[LEVEL_INDEX(addr, 1)] &= ~(pte_t)PTE_U;
    }
    paging_flush_tlb(addr);
}

/**
 * A new directory with the kernel's upper half and an empty lower half, or
 * NULL when no frame was free.
 */
page_dir_t *clone_page_directory(void)
{
    uintptr_t phys = table_alloc();
    page_dir_t *dir;
    uint32_t i;

    if (phys == 0) {
        return NULL;
    }
    dir = MMU_VIRT(phys);
    for (i = PML4_UPPER; i < MMU_TABLE_ENTRIES; i++) {
        dir->entries[i] = _kernel_dir->entries[i];
    }
    return dir;
}

/** The address CR3 takes for `dir`: its PML4's, in the direct map. */
uintptr_t get_page_dir_phys(page_dir_t *dir)
{
    return (uintptr_t)dir - KVA_DIRECT;
}

void switch_page_directory(page_dir_t *dir)
{
    if (dir == _curr_dir) {
        return;
    }
    _curr_dir = dir;
    arch_space_load(get_page_dir_phys(dir));
}

/**
 * The page table of `dir` for the user address `va`, as ../../mm/paging.h
 * describes. A table made here has the user bit and the write bit in the
 * entries above it, so the leaf entry alone decides an access.
 */
pte_t *mmu_user_table(page_dir_t *dir, uintptr_t va, int create)
{
    if (va >= USER_ADDR_MAX) {
        return NULL;
    }
    return table_for(dir->entries, va, 1, create, PTE_W | PTE_U);
}

/**
 * The first user address at or after `va` whose page table `dir` may have,
 * as ../../mm/paging.h describes: the end of the span an absent entry of
 * the PML4, a page directory pointer table or a page directory covers,
 * 512 GiB, 1 GiB or 2 MiB, or `va` when the walk reaches a page table.
 */
uintptr_t mmu_user_skip(page_dir_t *dir, uintptr_t va)
{
    pte_t *table = dir->entries;
    int l;

    if (va >= USER_ADDR_MAX) {
        return va;
    }
    for (l = 4; l > 1; l--) {
        pte_t e = table[LEVEL_INDEX(va, l)];

        if (!(e & PTE_P) || (e & PTE_PS)) {
            uintptr_t span = (uintptr_t)1 << (3 + 9 * l);

            return (va & ~(span - 1)) + span;
        }
        table = MMU_VIRT(PTE_ADDR(e));
    }
    return va;
}

pte_t *mmu_kernel_table(uintptr_t va)
{
    if (va < KVA_BASE) {
        return NULL;
    }
    return table_for(_kernel_dir->entries, va, 1, 0, 0);
}

/**
 * The physical address the kernel's PML4 maps `va` to, through a 1 GiB or
 * 2 MiB page of the direct map as through a 4 KiB one; 0 when nothing is
 * mapped there.
 */
uintptr_t paging_virt_to_phys(uintptr_t va)
{
    pte_t *table = _kernel_dir->entries;
    int l;

    if (va < KVA_BASE) {
        return 0;
    }
    for (l = 4; l >= 1; l--) {
        pte_t e = table[LEVEL_INDEX(va, l)];
        uintptr_t span = (uintptr_t)PAGE_SIZE << (9 * (l - 1));

        if (!(e & PTE_P)) {
            return 0;
        }
        if (l == 1 || (l <= 3 && (e & PTE_PS))) {
            return (PTE_ADDR(e) & ~(span - 1)) | (va & (span - 1));
        }
        table = MMU_VIRT(PTE_ADDR(e));
    }
    return 0;
}

phys_addr_t mmu_virt_to_phys(const volatile void *v)
{
    uintptr_t va = (uintptr_t)v;

    return va >= KVA_IMAGE ? va - KVA_IMAGE : va - KVA_DIRECT;
}

/* The table an entry names. */
static pte_t *entry_table(pte_t e)
{
    return MMU_VIRT(PTE_ADDR(e));
}

/**
 * Free all user pages in a directory: drop the frame reference of each
 * present user page of the lower half and clear its entry, flushing it when
 * `dir` is the one in use. A PROT_NONE page is a user page without the
 * user bit. An entry naming frame 0 is corrupt and is cleared without
 * touching a frame. The tables stay, for `free_page_directory`.
 */
void free_user_pages(page_dir_t *dir)
{
    uint32_t i4, i3, i2, i1;

    if (!dir) {
        return;
    }
    for (i4 = 0; i4 < PML4_UPPER; i4++) {
        pte_t *pdpt;

        if (!(dir->entries[i4] & PTE_P)) {
            continue;
        }
        pdpt = entry_table(dir->entries[i4]);
        for (i3 = 0; i3 < MMU_TABLE_ENTRIES; i3++) {
            pte_t *pd;

            if (!(pdpt[i3] & PTE_P)) {
                continue;
            }
            pd = entry_table(pdpt[i3]);
            for (i2 = 0; i2 < MMU_TABLE_ENTRIES; i2++) {
                pte_t *pt;

                if (!(pd[i2] & PTE_P)) {
                    continue;
                }
                pt = entry_table(pd[i2]);
                for (i1 = 0; i1 < MMU_TABLE_ENTRIES; i1++) {
                    pte_t pte = pt[i1];
                    uintptr_t va = ((uintptr_t)i4 << 39) | ((uintptr_t)i3 << 30) |
                                   ((uintptr_t)i2 << 21) | ((uintptr_t)i1 << 12);

                    if (!(pte & PTE_P) || !(pte & (PTE_U | PTE_NONE))) {
                        continue;
                    }
                    if (PTE_ADDR(pte) != 0) {
                        frame_unref(PTE_ADDR(pte));
                    }
                    pt[i1] = 0;
                    paging_flush_dir(dir, va);
                }
            }
        }
    }
}

/**
 * Free a directory and its lower half's tables. Call it after
 * `free_user_pages`; the frames go back to the allocator. The kernel's
 * directory and the one in use are refused and left in place: a caller
 * switches to another first.
 */
void free_page_directory(page_dir_t *dir)
{
    uint32_t i4, i3, i2;

    if (!dir || dir == _kernel_dir || dir == _curr_dir) {
        return;
    }
    for (i4 = 0; i4 < PML4_UPPER; i4++) {
        pte_t *pdpt;

        if (!(dir->entries[i4] & PTE_P)) {
            continue;
        }
        pdpt = entry_table(dir->entries[i4]);
        for (i3 = 0; i3 < MMU_TABLE_ENTRIES; i3++) {
            pte_t *pd;

            if (!(pdpt[i3] & PTE_P)) {
                continue;
            }
            pd = entry_table(pdpt[i3]);
            for (i2 = 0; i2 < MMU_TABLE_ENTRIES; i2++) {
                if (pd[i2] & PTE_P) {
                    frame_unref(PTE_ADDR(pd[i2]));
                }
            }
            frame_unref(PTE_ADDR(pdpt[i3]));
        }
        frame_unref(PTE_ADDR(dir->entries[i4]));
        dir->entries[i4] = 0;
    }
    frame_unref(get_page_dir_phys(dir));
}
