#ifndef _X86_64_MMU_H_
#define _X86_64_MMU_H_

#include "../../mm/paging.h"

#include "../../lib/kint.h"

/*
 * The x86-64's page tables: four levels of 512 entries, a PML4, page
 * directory pointer tables, page directories and page tables, which
 * together map 256 TiB in 4 KiB, 2 MiB and 1 GiB pages.
 *
 * Every table is a page reached through the direct map, at KVA_DIRECT plus
 * its physical address, so an entry's address is all the kernel needs to
 * reach the table below it, and a directory is its PML4 alone.
 */

struct page_dir {
    pte_t entries[MMU_TABLE_ENTRIES];
};

/** The kernel's address of the physical address `p`, in the direct map. */
#define MMU_VIRT(p) ((void *)(KVA_DIRECT + (uintptr_t)(p)))

#endif /* _X86_64_MMU_H_ */
