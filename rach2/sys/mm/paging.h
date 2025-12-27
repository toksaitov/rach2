#ifndef _PAGING_H_
#define _PAGING_H_

#include "heap.h"

#include "../panic.h"
#include "../isr.h"

#include "../lib/kdef.h"
#include "../lib/kint.h"
#include "../lib/kstd.h"

#include "../lib/kstring.h"

/* Increased from 16MB to 32MB to support Doom (needs ~5MB for BSS+heap) */
#define HARD_MEMORY_LIMIT 0x2000000

#define PAGE_SIZE 0x1000

struct pde {
    uint32_t present       : 1;
    uint32_t rw            : 1;
    uint32_t us            : 1;
    uint32_t pwt           : 1;
    uint32_t pcd           : 1;
    uint32_t accessed      : 1;
    uint32_t reserved      : 6;
    uint32_t table_address : 20;
} __attribute__((packed));

typedef struct pde pde_t;

struct pte {
    uint32_t present       : 1;
    uint32_t rw            : 1;
    uint32_t us            : 1;
    uint32_t pwt           : 1;
    uint32_t pcd           : 1;
    uint32_t accessed      : 1;
    uint32_t dirty         : 1;
    uint32_t pat           : 1;
    uint32_t global        : 1;
    uint32_t reserved      : 3;
    uint32_t frame_address : 20;
} __attribute__((packed));

typedef struct pte pte_t;

struct page_table {
    uint32_t pages[1024];
} __attribute__((packed));

typedef struct page_table page_table_t;

struct page_dir {
    page_table_t *logical_page_tables[1024];
    uint32_t phys_page_tables[1024];
} __attribute__((packed));

typedef struct page_dir page_dir_t;

void init_paging(void);

/*
 * Mark a page as user-accessible or supervisor-only.
 * addr: virtual address within the page
 * user: true for user-accessible, false for supervisor-only
 */
void paging_set_user(uintptr_t addr, bool user);

/*
 * Flush TLB entry for a specific address
 */
void paging_flush_tlb(uintptr_t addr);

/*
 * Clone the kernel page directory for a new process.
 * Copies kernel page table references (shared), ready for per-process user pages.
 * Returns pointer to new page directory, or NULL on failure.
 */
page_dir_t *clone_page_directory(void);

/*
 * Get physical address of page directory for CR3.
 * Returns the address suitable for loading into CR3.
 */
uintptr_t get_page_dir_phys(page_dir_t *dir);

/*
 * Switch to a different page directory (loads CR3).
 */
void switch_page_directory(page_dir_t *dir);

/*
 * Allocate a user page in a specific page directory.
 * virt_addr: virtual address to map (will be page-aligned)
 * dir: page directory to map in
 * Returns physical address of allocated frame, or 0 on failure.
 */
uintptr_t alloc_user_page(page_dir_t *dir, uintptr_t virt_addr);

/*
 * Free all user pages in a page directory.
 * Frees physical frames and clears page table entries for user-accessible
 * pages above the kernel region. Used by exec to replace process image.
 */
void free_user_pages(page_dir_t *dir);

/*
 * Free a page directory and its private page tables.
 * Adds structures to free lists for reuse. Call after free_user_pages().
 * Must not be called on kernel directory or currently active directory.
 */
void free_page_directory(page_dir_t *dir);

/*
 * Kernel page directory (shared by all processes for kernel mappings)
 */
extern page_dir_t *_kernel_dir;

/*
 * Current page directory
 */
extern page_dir_t *_curr_dir;

/*
 * Frame bitmap and count (for tracking physical frame allocation)
 */
extern uint8_t *_frames;
extern size_t _frames_number;

#endif
