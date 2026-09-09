#ifndef _PAGING_H_
#define _PAGING_H_

#include "frame.h"
#include "heap.h"

#include "../panic.h"
#include "../isr.h"

#include "../lib/kdef.h"
#include "../lib/kint.h"
#include "../lib/kstd.h"

#include "../lib/kstring.h"

/* Top of the identity-mapped physical window (32 MiB): the kernel image,
 * the boot data, the page tables the boot carves and the DMA buffers that a
 * device is given a physical address of live below it, where the virtual
 * address equals the physical one. Nothing above it is reached through its
 * physical address; a frame there is mapped into the kernel's layout first,
 * and the modules and the page tables made later are reached that way. */
#define HARD_MEMORY_LIMIT 0x2000000

#define PAGE_SIZE 0x1000

/*
 * The kernel's virtual layout above user space, in the gigabyte from
 * `USER_ADDR_MAX`. Every page table for it exists at boot and is shared by
 * every page directory, so a mapping made in it after the first fork is
 * seen by every process. The regions:
 *
 *   0xC0000000  256 MiB  the heap (mm/kheap.c), grown a frame at a time
 *   0xD0000000  512 MiB  the page-cache window (mm/pagecache.h)
 *   0xF0000000  128 MiB  MMIO, a controller's BAR mapped wherever it is
 *   0xF8000000    4 MiB  temporary mappings, one page each, for the paths
 *                        that touch a user frame by its physical address
 *   0xF8400000   64 MiB  the boot modules, mapped wherever the loader put
 *                        them
 *   0xFC400000   ...     unused
 */
#define KVA_BASE      0xC0000000u
#define KVA_HEAP      0xC0000000u
#define KVA_HEAP_SIZE 0x10000000u
#define KVA_MMIO      0xF0000000u
#define KVA_MMIO_SIZE 0x08000000u
#define KVA_TEMP      0xF8000000u
/* Policy: the temporary mappings held at once, one page each, the whole
 * of the region's 4 MiB; a caller that finds none free is told so. */
#define KVA_TEMP_SLOTS 1024u
#define KVA_MODULES   0xF8400000u
#define KVA_MODULES_SIZE 0x04000000u

/*
 * Software-available PTE bit (bit 9; the CPU ignores bits 9-11) used to mark
 * a page as copy-on-write: present + user + read-only + PTE_COW means
 * "shared, copy on the next write". The fault handler distinguishes a COW
 * write (copy/re-grant) from a genuine write to a read-only region
 * (segfault) by this bit.
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
 * Initialize paging. `map` is the physical memory the firmware's map
 * establishes, with the kernel image and the modules already reserved; the
 * frame table is sized from it, one word per frame below its top, and the
 * tables this carves are reserved in it before it returns.
 */
void init_paging(struct frame_map *map);

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
 * Clone the kernel page directory for a new process. Copies kernel page
 * table references (shared), ready for per-process user pages. Returns
 * pointer to new page directory, or NULL on failure.
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
 * Map an already-owned physical frame 'phys' at user virtual address 'virt'
 * in 'dir' with the given low-12-bit PTE 'flags'
 * (present/user/write/PTE_COW, etc.), creating a private page table for the
 * slot if needed. Unlike alloc_user_page it neither allocates a frame nor
 * changes any reference count: the caller owns the frame lifecycle. Used by
 * copy-on-write fork to share a frame read-only. Returns 0 on success, -1 on
 * failure.
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

/**
 * Map an MMIO region into the kernel's MMIO region, uncached, and return
 * its virtual address, or 0. The address is not the physical one: a driver
 * reaches its registers through what this returns. A region is mapped once
 * for the life of the kernel.
 */
uintptr_t map_mmio(uintptr_t phys_addr, size_t size);

/**
 * Map the frame at physical `phys` at a temporary kernel address and return
 * it, or 0 when every slot is taken. The mapping lasts until `kunmap_temp`;
 * a caller may sleep while it holds one, since a slot is its own. The paths
 * that zero, copy or fill a user frame use these, because a user frame may
 * lie above the identity-mapped window.
 */
void *kmap_temp(uintptr_t phys);

/** Release a mapping `kmap_temp` returned. */
void kunmap_temp(void *va);

/** Zero the frame at physical `phys`; -1 when no temporary mapping was
 * free, and then the frame is untouched. */
int paging_zero_frame(uintptr_t phys);

/** Copy the frame at physical `src` into the one at physical `dst`; -1
 * when no temporary mapping was free, and then nothing was copied. */
int paging_copy_frame(uintptr_t dst, uintptr_t src);

/**
 * A free frame from anywhere the map offers, marked used with one
 * reference; 0 when none is free. The caller reaches it through a mapping,
 * never through its physical address.
 */
uintptr_t alloc_frame(void);

/** Return a frame `alloc_frame` gave to the pool. */
void free_frame(uintptr_t phys);

/** The physical address behind kernel virtual address `va` in the kernel
 * directory, or 0 when nothing is mapped there. */
uintptr_t paging_virt_to_phys(uintptr_t va);

/**
 * Test switch: with `rach2.framefail` on the boot line, `/dev/framefail`
 * takes a count, and the count-th frame allocation made inside a system
 * call from then on fails as a full machine's would. Allocations for a
 * user page fault are not counted, so the process arming it is not the
 * one that fails. Returns -1 when the switch is off.
 */
int paging_framefail_arm(uint32_t count);

/**
 * Reserve one free physical frame for kernel use (never user); 0 if none.
 * The frame is below the identity-mapped window, so its physical address
 * is also its virtual one, which is what a device given the address for
 * DMA needs.
 */
void *alloc_kernel_frame(void);

/** Return a frame `alloc_kernel_frame` gave. */
void free_kernel_frame(void *frame);

/** Mark `frame` used by a boot reservation, with the counts kept; for
 * `mem_reserve`, which takes memory after the table exists. */
void paging_mark_reserved(uintptr_t frame);

/** Total physical frames the pool manages. */
uint32_t frame_total_count(void);

/** The least memory, in KiB, the kernel boots with once its own image, the
 * modules and the frame table are reserved. Below it the boot faults in
 * whatever first takes a frame, so the memory manager refuses such a machine
 * by name instead; the recovery image on 10 MiB has 1408 KiB and boots. */
#define FRAME_MIN_MANAGED_KIB 1024u

/** Physical frames currently free, kept as a count by the table's writers. */
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
 * The frame table: one word per physical frame below the map's top, named
 * by frame number. 0 means the frame is free; N below `FRAME_REF_MAX` means
 * N mappings share it. A fresh allocation sets the count to 1; copy-on-write
 * fork raises it; freeing a mapping lowers it, and the frame returns to the
 * pool only when the last reference is gone. The words at and above
 * `FRAME_REF_MAX` are states and not counts: a frame the map does not offer.
 */
#define FRAME_REF_MAX 0xFFFFFF00u
#define FRAME_ABSENT  0xFFFFFFFFu
extern uint32_t *_frames;
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
