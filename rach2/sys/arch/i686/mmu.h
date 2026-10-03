#ifndef _I686_MMU_H_
#define _I686_MMU_H_

#include "../../mm/paging.h"

#include "../../lib/kint.h"

/*
 * The i686's page tables: a directory of 1024 entries, each naming a table
 * of 1024 page entries, which together map 4 GiB in 4 KiB pages.
 */

struct page_table {
    pte_t pages[MMU_TABLE_ENTRIES];
};

typedef struct page_table page_table_t;

/*
 * A directory: the tables by the kernel's addresses, then the processor's
 * entries, which CR3 names. The processor reads the second half alone; the
 * first is the kernel's, so that a table is reached without mapping it.
 */
struct page_dir {
    page_table_t *logical_page_tables[1024];
    uint32_t phys_page_tables[1024];
};

#endif /* _I686_MMU_H_ */
