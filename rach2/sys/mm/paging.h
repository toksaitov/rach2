#ifndef _PAGING_H_
#define _PAGING_H_

#include "heap.h"

#include "../panic.h"
#include "../isr.h"

#include "../lib/kdef.h"
#include "../lib/kint.h"
#include "../lib/kstd.h"

#include "../lib/kstring.h"

/* Top of the direct-mapped physical window (32MB). */
#define HARD_MEMORY_LIMIT 0x2000000

#define PAGE_SIZE 0x1000

/*
 * Software-available PTE bit (bit 9; the CPU ignores bits 9-11) used to mark a
 * page as copy-on-write: present + user + read-only + PTE_COW means "shared, copy
 * on the next write". The fault handler distinguishes a COW write (copy/re-grant)
 * from a genuine write to a read-only region (segfault) by this bit.
 */
#define PTE_COW 0x200u

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

/*
 * Initialize paging. 'usable_frames' is the number of PAGE_SIZE physical frames
 * the kernel should manage (from frame_usable_count(), sized off the multiboot
 * memory map and capped to the direct-mapped window); 0 falls back to the full
 * window.
 */
void init_paging(size_t usable_frames);

/*
 * Mark a page as user-accessible or supervisor-only.
 * addr: virtual address within the page
 * user: true for user-accessible, false for supervisor-only
 */
void paging_set_user(uintptr_t addr, bool user);

/*
 * Set or clear the write permission (PTE bit 1) of an already-present page in
 * 'dir'. The ELF loader uses this to make read-only segments (text) read-only
 * after copying their bytes in via the physical alias, so a user write to text
 * faults as a protection violation; copy-on-write uses the same distinction
 * between a write fault and a protection fault. No-op if the page is absent.
 */
void paging_set_writable(page_dir_t *dir, uintptr_t addr, bool writable);

/** Flush the TLB entry for 'addr'. */
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
 * Map an already-owned physical frame 'phys' at user virtual address 'virt' in
 * 'dir' with the given low-12-bit PTE 'flags' (present/user/write/PTE_COW, etc.),
 * creating a private page table for the slot if needed. Unlike alloc_user_page it
 * neither allocates a frame nor changes any reference count: the caller owns the
 * frame lifecycle. Used by copy-on-write fork to share a frame read-only.
 * Returns 0 on success, -1 on failure.
 */
int map_user_frame(page_dir_t *dir, uintptr_t virt, uintptr_t phys, uint32_t flags);

/*
 * Pre-fault [addr, addr+len) in the current process's address space before the
 * kernel touches it (fault-aware usercopy). Each page in a valid VMA is backed
 * on demand (growing the stack if needed) exactly as a user-mode fault would;
 * 'write' requests write permission. Returns 1 if the whole range is now
 * accessible (an empty range vacuously so), or 0 if any page is outside every
 * VMA or lacks the permission (so the caller can return -EFAULT instead of
 * faulting in kernel mode), or 0 if there is no current user address space.
 */
int vm_prefault_range(uint32_t addr, uint32_t len, int write);

/** Free all user pages in 'dir' (used by exec to replace the process image). */
void free_user_pages(page_dir_t *dir);

/*
 * Unmap the user pages in [start, end) from 'dir': for each present, non-shared
 * page, return its frame to the pool, clear the PTE, and flush the TLB if 'dir'
 * is active. Never-faulted lazy pages in the range are skipped. Used by munmap
 * and by brk when the heap shrinks. start/end should be page-aligned.
 */
void unmap_user_range(page_dir_t *dir, uint32_t start, uint32_t end);

/** Free a page directory and its private page tables; call after
 * free_user_pages(). */
void free_page_directory(page_dir_t *dir);

/** Identity-map an MMIO region; returns its virtual address (== physical),
 * or 0. */
uintptr_t map_mmio(uintptr_t phys_addr, size_t size);

/** Reserve one free physical frame for kernel use (never user); 0 if none. */
uintptr_t alloc_kernel_frame(void);

/** Total physical frames the pool manages. */
uint32_t frame_total_count(void);

/** Physical frames currently free (a linear bitmap scan). */
uint32_t frame_free_count(void);

/*
 * Pre-allocate the kernel page tables spanning [va_start, va_start+size) with
 * not-present PTEs, before the first fork copies the kernel PDEs. va_start must
 * be 4 MiB-aligned. Returns 0, or -1 on allocation failure or range overflow.
 */
int premap_kernel_window(uintptr_t va_start, size_t size);

/*
 * Map physical frame 'phys' at page-aligned kernel VA 'va' (supervisor-only,
 * present, writable iff 'writable'); the page table must already exist.
 */
void map_kernel_frame(uintptr_t va, uintptr_t phys, int writable);

/** Clear the leaf PTE for kernel VA 'va' and flush its TLB entry. */
void unmap_kernel_frame(uintptr_t va);

/*
 * Kernel page directory (shared by all processes for kernel mappings)
 */
extern page_dir_t *_kernel_dir;

/*
 * Current page directory
 */
extern page_dir_t *_curr_dir;

/*
 * Physical frame reference counts (one byte per frame) and the frame count.
 * 0 means the frame is free; N means N mappings share it. A fresh allocation
 * sets the count to 1; copy-on-write fork bumps it; freeing a mapping drops it,
 * and the frame returns to the pool only when the last reference is gone.
 */
extern uint8_t *_frames;
extern size_t _frames_number;

/*
 * Take an extra reference on the frame backing physical address 'phys' (used
 * when a page is shared, e.g. a COW fork). No-op for a free or out-of-range
 * frame, or one already at the max count.
 */
void frame_ref(uintptr_t phys);

/*
 * Drop one reference on the frame backing 'phys'; it returns to the pool only
 * when the count reaches zero. Returns the new reference count (0 == freed).
 */
uint32_t frame_unref(uintptr_t phys);

#endif
