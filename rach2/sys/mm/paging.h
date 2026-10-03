#ifndef _PAGING_H_
#define _PAGING_H_

#include "frame.h"
#include "heap.h"

#include "../panic.h"
#include "../irq.h"

#include "../lib/kdef.h"
#include "../lib/kint.h"
#include "../lib/kstd.h"

#include "../lib/kstring.h"
#include "../proc/percpu.h"

/*
 * A page directory, the root of an address space's page tables. Its layout
 * is the architecture's (arch/i686/mmu.h); the shared code holds it by
 * pointer and reaches its entries through the leaf tables below.
 */
typedef struct page_dir page_dir_t;

/* The bytes of address space a leaf table covers, and an address's entry in
 * the leaf table that covers it. */
#define MMU_TABLE_SPAN ((uintptr_t)MMU_TABLE_ENTRIES * PAGE_SIZE)
#define MMU_INDEX(va)  (((uintptr_t)(va) / PAGE_SIZE) % MMU_TABLE_ENTRIES)

/*
 * The leaf table of `dir` that covers the user address `va`, an array of
 * MMU_TABLE_ENTRIES entries read and written with cpu.h's PTE_ bits, or
 * NULL where `dir` has no table of its own there. With `create` a missing
 * table is made, and one the kernel's directory shares is copied, so that
 * the entries written are this address space's alone; NULL then when no
 * memory could be had. The architecture defines it (arch/i686/mmu.c).
 */
pte_t *mmu_user_table(page_dir_t *dir, uintptr_t va, int create);

/* The kernel directory's leaf table that covers the kernel address `va`,
 * which every directory shares, or NULL where there is none. */
pte_t *mmu_kernel_table(uintptr_t va);

/*
 * The steps of `init_paging` the architecture takes (arch/i686/mmu.c):
 * `mmu_frame_table` places the frame table; `mmu_init` builds the kernel's
 * directory with the identity window and the tables of the kernel's layout,
 * marking the frames it carves in the frame table (`frame_take_boot`);
 * `mmu_start` turns translation on through it; `mmu_heap_ready` says the
 * frame allocator is counted and the heap is next, and every page table
 * made from then on comes from them, the heap's own among them.
 */
void mmu_init(void);
void mmu_start(void);
void mmu_heap_ready(void);

/*
 * The first physical address the kernel cannot reach, which bounds the
 * memory the frame allocator takes: 4 GiB on i686, whose physical addresses
 * without PAE are 32 bits, and the direct map's end on x86-64.
 */
uint64_t mmu_phys_reach(void);

/*
 * The kernel's address of `bytes` for the frame table, page-aligned, or
 * NULL when no memory holds it. `map` is the firmware's memory with the
 * kernel and the modules reserved; the caller reserves the table in it. On
 * i686 the table is carved at the boot allocator's break, in the identity
 * window. On x86-64 the direct map is built and loaded first, so that the
 * table may lie anywhere, and it goes at the top of the highest usable
 * range above the window that holds it.
 */
void *mmu_frame_table(struct frame_map *map, size_t bytes);

/** Mark the boot's frame `frame` used, for the architecture's table carving
 * before the heap exists. */
void frame_take_boot(uintptr_t frame);

/** Copy the user pages of `from`, the address space in use, into the
 * directory `child`, copy-on-write: 0, or -1 with no memory for a table. */
struct mm;
int paging_fork_copy(struct mm *from, page_dir_t *child);

/*
 * Initialize paging. `map` is the physical memory the firmware's map
 * establishes, with the kernel image and the modules already reserved; the
 * frame table is sized from it, one word per frame below its top, and the
 * table and the tables this carves are reserved in it before it returns.
 * The heap is then sized from the memory the frame allocator manages.
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

/*
 * Allow or forbid instruction fetches from an already-present page of 'dir',
 * by PTE_NOEXEC, which is 0 where the processor has no such bit. The ELF
 * loader and demand paging use it to keep a page of a region without
 * PROT_EXEC from running. No-op if the page is absent.
 */
void paging_set_exec(page_dir_t *dir, uintptr_t addr, bool exec);

/** The PTE_NOEXEC of a user page of a region with the protection `prot`
 * (VM_PROT_*): the bit when the region has no VM_PROT_EXEC, else 0. */
pte_t paging_noexec(uint32_t prot);

/** Drop the translation of `addr` in the running processor's directory
 * wherever a processor has it (`paging_flush_dir`). */
void paging_flush_tlb(uintptr_t addr);

/** Drop the translation of `addr` in `dir` on every processor that has it;
 * see paging.c. */
void paging_flush_dir(const page_dir_t *dir, uintptr_t addr);

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

/**
 * Translate through the kernel's own page directory, for an interrupt
 * handler that touches kernel data an arbitrary process's directory may not
 * map, and return what `paging_leave_kernel` needs to put the interrupted
 * one back. The processor's register is read and written directly, since
 * the record `switch_page_directory` keeps may be out of step here.
 */
uintptr_t paging_enter_kernel(void);

/** Put back the translation `paging_enter_kernel` replaced, if it did. */
void paging_leave_kernel(uintptr_t saved);

/*
 * Allocate a user page in a specific page directory.
 * virt_addr: virtual address to map (will be page-aligned)
 * dir: page directory to map in
 * Returns physical address of allocated frame, or 0 on failure.
 */
uintptr_t alloc_user_page(page_dir_t *dir, uintptr_t virt_addr);

/*
 * A frame for a user page, taken as alloc_user_page takes one, never from
 * the reserve, and not mapped: for a page that is filled before it is
 * mapped. Returns its physical address, or 0 when there is none.
 */
uintptr_t alloc_user_frame(void);

/*
 * Map an already-owned physical frame 'phys' at user virtual address 'virt'
 * in 'dir' with the given PTE 'flags' (present/user/write/PTE_COW/PTE_NOEXEC,
 * etc., any of the bits PTE_FLAGS covers), creating a private page table for
 * the slot if needed. Unlike alloc_user_page it neither allocates a frame nor
 * changes any reference count: the caller owns the frame lifecycle. Used by
 * copy-on-write fork to share a frame read-only. Returns 0 on success, -1 on
 * failure.
 */
int map_user_frame(page_dir_t *dir, uintptr_t virt, uintptr_t phys, pte_t flags);

/*
 * Pre-fault [addr, addr+len) in the current process's address space before the
 * kernel touches it (fault-aware usercopy). Each page in a valid VMA is backed
 * on demand (growing the stack if needed) exactly as a user-mode fault would;
 * 'write' requests write permission. Returns 1 if the whole range is now
 * accessible (an empty range vacuously so), or 0 if any page is outside every
 * VMA or lacks the permission (so the caller can return -EFAULT instead of
 * faulting in kernel mode), or 0 if there is no current user address space.
 * It may sleep; a 1 holds until the caller sleeps, since the range is checked
 * again after the last page is in and brought in again when a page went.
 */
int vm_prefault_range(uintptr_t addr, uintptr_t len, int write);

/** Free all user pages in 'dir' (used by exec to replace the process image). */
void free_user_pages(page_dir_t *dir);

/*
 * Unmap the user pages in [start, end) from 'dir': for each present, non-shared
 * page, return its frame to the pool, clear the PTE, and flush the TLB if 'dir'
 * is active. Never-faulted lazy pages in the range are skipped. Used by munmap
 * and by brk when the heap shrinks. start/end should be page-aligned.
 */
void unmap_user_range(page_dir_t *dir, uintptr_t start, uintptr_t end);

/*
 * Move the entries of [from, from + len) in `dir` to [to, to + len), for
 * mremap: each frame moves with its flags and keeps its references, and a
 * page never backed stays so. The two ranges must not overlap. Returns 0,
 * or -1 when a page table for the new range cannot be had, with nothing
 * moved.
 */
int paging_move_range(page_dir_t *dir, uintptr_t from, uintptr_t to, uintptr_t len);

/* Bring the resident pages of [start, end) in `dir` to the VMA protection
 * `prot` (VM_PROT_*): no access makes them PTE_NONE, read-only clears the
 * write bit, and write sets it only on a page this process alone holds; a
 * shared page becomes copy-on-write instead, unless `prot` has
 * VM_PROT_SHARED for a shared region, whose pages are written in place. */
void paging_protect_range(page_dir_t *dir, uintptr_t start, uintptr_t end, uint32_t prot);

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
 * that zero, copy or fill a user frame use these, because on i686 a user
 * frame may lie above the identity-mapped window. Where the direct map
 * reaches every frame (`ARCH_DIRECT_MAP_ALL`, x86-64), the address is the
 * frame's there and nothing is mapped.
 */
void *kmap_temp(uintptr_t phys);

/** Whether the frame of the physical address `phys` is not the allocator's:
 * past the top of memory, or one the memory map does not offer, which
 * firmware and devices have. /dev/mem reaches these and the first
 * megabyte, as Linux's STRICT_DEVMEM allows. */
int paging_frame_is_device(uintptr_t phys);

/**
 * Release a mapping `kmap_temp` returned. The caller must not use the
 * address again. On i686 the slot stays mapped until the slots are next
 * swept: when none is free, every released slot is unmapped at once and
 * every processor drops its translations, once for up to 1024 releases, as
 * Linux's `kmap` does with its pool.
 */
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
 * directory, or 0 when nothing is mapped there. The architecture walks its
 * tables (arch/i686/mmu.c), large pages included where it has them. */
uintptr_t paging_virt_to_phys(uintptr_t va);

/** The physical address `dir` maps the user address `va` to, or 0 when the
 * page is not present. */
uintptr_t paging_user_phys(page_dir_t *dir, uintptr_t va);

/** Whether `dir` has the user page at `va`: 0 when it is not present, 1 when
 * it is and has not been written since it was mapped, 2 when it has. */
int paging_user_state(page_dir_t *dir, uintptr_t va);

struct mm;

/**
 * Read or write `len` bytes at `addr` of the address space `mm`, which need
 * not be the one in use, for ptrace and /proc/PID/mem: missing pages are
 * brought in as a fault would bring them, and a write reaches a read-only
 * private page, a breakpoint in the program's text, through a copy of its
 * own, as Linux's FOLL_FORCE does. A page of no region, of a PROT_NONE one,
 * or of a read-only shared one for a write, is -EIO; 0, or -EIO, or -ENOMEM
 * when no temporary mapping was free. It may sleep, bringing a page in or
 * releasing a file a region let go of meanwhile, and looks each page up
 * again after a sleep.
 */
int paging_access(struct mm *mm, uintptr_t addr, void *buf, uint32_t len, int write);

/**
 * Test switch: with `rach2.framefail` on the boot line, `/dev/framefail`
 * takes a count, and the count-th frame allocation made inside a system
 * call from then on fails as a full machine's would. Allocations for a
 * user page fault are not counted, so the process arming it is not the
 * one that fails. Returns -1 when the switch is off.
 */
int paging_framefail_arm(uint32_t count);

/**
 * Reserve one free physical frame for kernel use (never user), and return
 * the kernel's address of it; NULL if none. The frame is of the DMA pool,
 * below the window's top, HARD_MEMORY_LIMIT, so `virt_to_phys` gives a
 * device its address, which fits 32 bits.
 */
void *alloc_kernel_frame(void);

/** Return a frame `alloc_kernel_frame` gave. */
void free_kernel_frame(void *frame);

/** Mark `frame` used by a boot reservation, with the counts kept; for
 * `mem_reserve`, which takes memory after the table exists. */
void paging_mark_reserved(uintptr_t frame);

/** Total physical frames the pool manages. */
size_t frame_total_count(void);

/** The least memory, in KiB, the kernel boots with once its own image, the
 * modules and the frame table are reserved. Below it the boot faults in
 * whatever first takes a frame, so the memory manager refuses such a machine
 * by name instead; the recovery image on 10 MiB has 1408 KiB and boots. */
#define FRAME_MIN_MANAGED_KIB 1024u

/** Physical frames currently free, kept as a count by the table's writers. */
size_t frame_free_count(void);

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
 * The page directory the running processor translates through, in its
 * record (proc/percpu.h).
 */
#define _curr_dir (this_cpu()->space)

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
