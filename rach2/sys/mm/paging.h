/*
 * paging: the frame allocator, the page tables of the kernel and of each
 * address space, and the page fault handler. Each architecture's mmu.c builds
 * and walks its own tables; the code here decides what goes in an entry.
 */
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
 * is the architecture's (its mmu.h); the shared code holds it by pointer and
 * reaches its entries through the leaf tables below.
 */
typedef struct page_dir page_dir_t;

/* The bytes of address space a leaf table covers, and an address's entry in
 * the leaf table that covers it. */
#define MMU_TABLE_SPAN ((uintptr_t)MMU_TABLE_ENTRIES * PAGE_SIZE)
#define MMU_INDEX(va)  (((uintptr_t)(va) / PAGE_SIZE) % MMU_TABLE_ENTRIES)

/**
 * The leaf table of `dir` that covers the user address `va`, an array of
 * MMU_TABLE_ENTRIES entries read and written with cpu.h's PTE_ bits, or
 * NULL where `dir` has no table of its own there. With `create` a missing
 * table is made, and one the kernel's directory shares is copied, so that
 * the entries written are this address space's alone; NULL then when no
 * memory could be had. Each architecture's mmu.c defines it.
 */
pte_t *mmu_user_table(page_dir_t *dir, uintptr_t va, int create);

/**
 * The first user address at or after `va` whose leaf table `dir` may have
 * of its own: `va` when it has one there, and otherwise the end of the
 * largest span around `va` whose tables it lacks, so that a walk over a
 * sparse region passes an absent upper level in one step. Each
 * architecture's mmu.c defines it.
 */
uintptr_t mmu_user_skip(page_dir_t *dir, uintptr_t va);

/** The kernel directory's leaf table that covers the kernel address `va`,
 * which every directory shares, or NULL where there is none. */
pte_t *mmu_kernel_table(uintptr_t va);

/**
 * Make the kernel's page tables that every directory shares, a step of
 * `init_paging`, marking each frame carved for them used
 * (`frame_take_boot`). On i686 that is the kernel's directory, with the
 * window identity-mapped and a table for every 4 MiB of the kernel's
 * gigabyte; on x86-64, whose directory and direct map `mmu_frame_table`
 * made, it is the windows' PML4 entries and the tables of the MMIO and
 * module regions. Each architecture's mmu.c defines it.
 */
void mmu_init(void);

/** Translate through the kernel's directory from here on, a step of
 * `init_paging`: on i686 this turns paging on; on x86-64 the loader did,
 * and the directory is loaded again. */
void mmu_start(void);

/** Note that the frame allocator is counted and the heap is next, the last
 * step of `init_paging` before it: every page table made from then on comes
 * from the heap on i686 and from the frame allocator on x86-64. */
void mmu_heap_ready(void);

/**
 * The first physical address the kernel cannot reach, which bounds the
 * memory the frame allocator takes: 4 GiB on i686, whose physical addresses
 * without PAE are 32 bits, and the direct map's end on x86-64.
 */
uint64_t mmu_phys_reach(void);

/**
 * The kernel's address of `bytes` for the frame table, page-aligned, or
 * NULL when no memory holds it. `map` is the firmware's memory with the
 * kernel and the modules reserved; the caller reserves the table in it. On
 * i686 the table is carved at the boot allocator's break, in the identity
 * window. On x86-64 the direct map is built and loaded first, so that the
 * table may lie anywhere, and it goes at the top of the highest usable
 * range above the window that holds it, or at the break when none does.
 */
void *mmu_frame_table(struct frame_map *map, size_t bytes);

/** Mark the boot's frame `frame` used, for the architecture's table carving
 * before the heap exists; nothing before the frame table exists. */
void frame_take_boot(uintptr_t frame);

struct mm;

/**
 * Give the directory `child` the user pages of `from`, the address space in
 * use, as fork does: each present user page of `from`'s regions is the same
 * frame in the child, with a reference taken, and nothing is copied. A writable
 * page of a private region becomes read-only and copy-on-write (PTE_COW) in
 * both, so that the first write on either side takes a copy of its own; a
 * read-only page is shared as it is, and a write to it still faults; a page
 * of a shared region keeps its rights in both, so that a store is seen by
 * both. Returns 0, or -1 when a page table of the child cannot be had.
 */
int paging_fork_copy(struct mm *from, page_dir_t *child);

/**
 * Build the frame table and the kernel's directory, turn translation on and
 * start the heap; the boot calls it once. `map` is the physical memory the
 * firmware's map establishes, with the kernel image and the modules already
 * reserved; the frame table is sized from it, one word per frame below its
 * top, and the table and the boot allocator's memory are reserved in it
 * before this returns. The heap is sized from what the frame allocator then
 * manages (`kheap_size_for`). A frame table that would need memory the map
 * does not offer stops the boot with a panic.
 */
void init_paging(struct frame_map *map);

/**
 * Give the page at `addr` in the directory in use the user bit, or take it
 * away. Setting it sets it in the entries above the leaf too, since the
 * processor tests it at each level; clearing it leaves them. A page with no
 * table is left alone. The debug shell's ring-3 exercise uses it on the
 * kernel's own pages. Each architecture's mmu.c defines it.
 */
void paging_set_user(uintptr_t addr, bool user);

/**
 * Set or clear the write bit of the present page at `addr` in `dir`; nothing
 * when the page is not present. The ELF loader makes a page of a segment
 * without write read-only with it, after it has copied the page in through
 * a temporary mapping, so that a user store there faults.
 */
void paging_set_writable(page_dir_t *dir, uintptr_t addr, bool writable);

/**
 * Allow or forbid instruction fetches from the present page at `addr` in
 * `dir`, by PTE_NOEXEC, which is 0 where the processor has no such bit;
 * nothing when the page is not present. The ELF loader uses it to keep a
 * page of a segment without execute from running.
 */
void paging_set_exec(page_dir_t *dir, uintptr_t addr, bool exec);

/** The PTE_NOEXEC of a user page of a region with the protection `prot`
 * (VM_PROT_*): the bit when the region has no VM_PROT_EXEC, else 0. */
pte_t paging_noexec(uint32_t prot);

/** Drop the translation of `addr` in the running processor's directory
 * wherever a processor has it (`paging_flush_dir`). */
void paging_flush_tlb(uintptr_t addr);

/**
 * Drop the translation of `addr` in `dir` wherever a processor has it.
 *
 * A user address is dropped on this processor when it translates through
 * `dir`, and on every other that does (`smp_tlb_shootdown`), before this
 * returns, so that the caller may free the page's frame next. A kernel
 * address is in every directory: this processor drops it now, and every
 * other drops all its translations before it next takes the big lock,
 * which it holds whenever it reads kernel memory that was not mapped when
 * it last held it (`smp_kernel_mapping_changed`).
 */
void paging_flush_dir(const page_dir_t *dir, uintptr_t addr);

/**
 * A new directory for an address space, or NULL when no memory can be had.
 * The kernel's entries are copied from the kernel's directory, so that the
 * kernel's tables are shared and a mapping made in them later is seen
 * through this one too; no user page is mapped. The kernel's tables all
 * exist from the boot, since one made after the copy would be missing
 * here. Each architecture's mmu.c defines it.
 */
page_dir_t *clone_page_directory(void);

/** The physical address of `dir`'s top table, which the processor's page
 * table register (CR3) takes. */
uintptr_t get_page_dir_phys(page_dir_t *dir);

/** Translate through `dir` on this processor, loading CR3, and record it
 * as the one in use (`_curr_dir`); nothing when it is already. */
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

/**
 * Back the user address `virt_addr`, rounded down to its page, in `dir` with
 * a frame from `alloc_user_frame`, present, writable and user, and return
 * the frame's physical address; 0 when no page table or no frame can be
 * had. The frame is not zeroed.
 */
uintptr_t alloc_user_page(page_dir_t *dir, uintptr_t virt_addr);

/**
 * A free frame for a user page, with one reference, not mapped and not
 * zeroed, for a page that is filled before it is mapped. Returns its
 * physical address, or 0 when the free frames are the reserve's and the
 * caller may not take them (`mem_may_take`), when none is free, or when
 * `rach2.framefail` fails it. A refusal is logged and sets the current
 * process's `frame_refused`, by which the page fault handler ends the
 * process with SIGKILL.
 */
uintptr_t alloc_user_frame(void);

/**
 * Map the frame at `phys` at the user address `virt`, rounded down to its
 * page, in `dir`, with the entry bits `flags` (any of those PTE_FLAGS
 * covers), making `dir`'s own table for it when needed, and drop the old
 * translation wherever a processor has it. No frame is allocated and no
 * count changes: the caller owns the frame's references. Returns 0, or -1
 * when no table can be had.
 */
int map_user_frame(page_dir_t *dir, uintptr_t virt, uintptr_t phys, pte_t flags);

/**
 * Bring in every page of [addr, addr + len) of the current process for a
 * read, or for a write when `write` says so, as a user fault would, the
 * stack's growth included, so that the kernel can touch the range without
 * faulting. Returns 1 when every page is present and allows the access, and
 * for an empty range; 0 when a page lies in no region or its region does
 * not allow the access, when no frame can be had, when the range wraps, or
 * when there is no current address space, and the caller then answers
 * -EFAULT. It may sleep, and a 1 holds until the caller sleeps.
 */
int vm_prefault_range(uintptr_t addr, uintptr_t len, int write);

/**
 * Drop the reference of every present user page in `dir`'s own tables and
 * clear its entry; a frame goes back to the pool with its last reference.
 * The tables stay, for `free_page_directory`. The last user of an address
 * space calls it (`mm_put`). Each architecture's mmu.c defines it.
 */
void free_user_pages(page_dir_t *dir);

/**
 * Unmap the present user pages of [start, end) in `dir`, page-aligned: each
 * drops its frame's reference, which frees the frame with its last, has its
 * entry cleared, and has its translation dropped wherever a processor has
 * it. A page never backed is passed. A NULL `dir` changes nothing. munmap
 * and a brk that shrinks the heap call it.
 */
void unmap_user_range(page_dir_t *dir, uintptr_t start, uintptr_t end);

/**
 * Move the entries of [from, from + len) in `dir` to [to, to + len), for
 * mremap: each frame moves with its flags and keeps its references, and a
 * page never backed stays so. The two ranges must not overlap. Returns 0,
 * or -1 when a page table for the new range cannot be had, with nothing
 * moved.
 */
int paging_move_range(page_dir_t *dir, uintptr_t from, uintptr_t to, uintptr_t len);

/**
 * Bring the present pages of [start, end) in `dir` to the region protection
 * `prot` (VM_PROT_*): no access makes them PTE_NONE, read-only clears the
 * write bit, and write sets it only on a page this process alone holds; a
 * shared page becomes copy-on-write instead, unless `prot` has
 * VM_PROT_SHARED for a shared region, whose pages are written in place, and
 * a copy-on-write page stays so. The no-execute bit follows `prot`. A page
 * not present is left alone, since its fault reads the region's protection.
 */
void paging_protect_range(page_dir_t *dir, uintptr_t start, uintptr_t end, uint32_t prot);

/** Give `dir` and its own page tables back, after `free_user_pages` has
 * dropped its pages. The kernel's directory and the one in use are left in
 * place: a caller switches to another first. Each architecture's mmu.c
 * defines it. */
void free_page_directory(page_dir_t *dir);

/**
 * Map the device memory [phys_addr, phys_addr + size) into the kernel's
 * MMIO region, cache-disabled (PCD and PWT, PTE_NOCACHE), so that a read of
 * a register is never stale, and return its kernel address; 0, with a
 * record, when the range is empty or wraps or the region has no room left.
 * The address is not the physical one: a driver reaches its registers
 * through what this returns. A region is mapped for the life of the kernel,
 * in tables every directory shares.
 */
uintptr_t map_mmio(uintptr_t phys_addr, size_t size);

/**
 * Map an MMIO region as `map_mmio` does, with the entries' cache bits
 * `cache` in place of PTE_NOCACHE: PTE_WC for a framebuffer, whose writes
 * then merge into bursts, where the architecture made that entry
 * write-combining.
 */
uintptr_t map_mmio_typed(uintptr_t phys_addr, size_t size, pte_t cache);

/**
 * Map the frame at physical `phys` at a temporary kernel address and return
 * it, or 0, with a record, when every slot is held. The mapping lasts until
 * `kunmap_temp`; a caller may sleep while it holds one, since a slot is its
 * own. The paths that zero, copy or fill a user frame use these, because on
 * i686 a user frame may lie above the identity-mapped window. Where the
 * direct map reaches every frame (`ARCH_DIRECT_MAP_ALL`, x86-64), the
 * address is the frame's there and nothing is mapped.
 */
void *kmap_temp(uintptr_t phys);

/** Whether the frame of the physical address `phys` is not the allocator's:
 * past the top of memory, or one the memory map does not offer, which
 * firmware and devices have. /dev/mem reaches these and the first megabyte,
 * as Linux's STRICT_DEVMEM allows. */
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
 * reference: from above the window first, and from the window only while it
 * has frames to spare for `alloc_kernel_frame`. 0 when none is free, when
 * the free frames are the reserve's and the caller may not take them
 * (`mem_may_take`), or when `rach2.framefail` fails it. The caller reaches
 * it through a mapping, never through its physical address. It never
 * sleeps.
 */
uintptr_t alloc_frame(void);

/** Drop a reference to the frame at `phys`, as `frame_unref` does; the
 * frame goes back to the pool with its last. */
void free_frame(uintptr_t phys);

/** The physical address behind kernel virtual address `va` in the kernel
 * directory, or 0 when nothing is mapped there. Each architecture's mmu.c
 * walks its tables, large pages included where it has them. */
uintptr_t paging_virt_to_phys(uintptr_t va);

/** The physical address `dir` maps the user address `va` to, or 0 when the
 * page is not present. */
uintptr_t paging_user_phys(page_dir_t *dir, uintptr_t va);

/** Count the present pages of the user range [start, end) of `dir` into
 * `*present`, and of them those the processor marked dirty into `*dirty`,
 * passing a span of absent tables in one step (`mmu_user_skip`), so that a
 * large region with few pages costs few steps. */
void paging_user_count(page_dir_t *dir, uintptr_t start, uintptr_t end, unsigned long *present,
                       unsigned long *dirty);

struct mm;

/**
 * Read or write `len` bytes at `addr` of the address space `mm`, which need
 * not be the one in use, for ptrace and /proc/PID/mem: missing pages are
 * brought in as a fault would bring them, and a write reaches a read-only
 * private page, a breakpoint in the program's text, through a copy of its
 * own, as Linux's FOLL_FORCE does. An address outside user space, a page of
 * no region, of a PROT_NONE one or of device memory, a page that cannot be
 * backed, and a page of a read-only shared region for a write are -EIO.
 * Returns 0, -EIO, or -ENOMEM when no temporary mapping was free. It may
 * sleep, bringing a page in or releasing a file a region let go of
 * meanwhile, and looks each page up again after a sleep.
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
 * Take a free frame of the window for the kernel, with one reference and no
 * user mapping, and return the kernel's address of it; NULL when the window
 * has none free. The reserve does not apply. The frame is of the DMA pool,
 * below the window's top, HARD_MEMORY_LIMIT, so `virt_to_phys` gives a
 * device its address, which fits 32 bits. The caller owns it until
 * `free_kernel_frame`.
 */
void *alloc_kernel_frame(void);

/** Return a frame `alloc_kernel_frame` gave. */
void free_kernel_frame(void *frame);

/** Mark `frame` used by a boot reservation, with the counts kept; for
 * `mem_reserve`, which takes memory after the table exists. */
void paging_mark_reserved(uintptr_t frame);

/** The frames the allocator manages, counted at boot: those above the
 * boot's reservations that were free then. */
size_t frame_total_count(void);

/** The least memory, in KiB, the kernel boots with once its own image, the
 * modules and the frame table are reserved. Below it the boot faults in
 * whatever first takes a frame, so the boot stops such a machine with a
 * panic that names the cause instead (kernel.c); the recovery image on
 * 10 MiB has 1408 KiB and boots. */
#define FRAME_MIN_MANAGED_KIB 1024u

/** The physical frames free now, a count every writer of the table keeps,
 * read without a lock: a reader gets the count at one moment. */
size_t frame_free_count(void);

/**
 * Make every page table of [va_start, va_start + size) in the kernel's
 * directory, with no page mapped, so that `map_kernel_frame` can later write
 * an entry every directory sees: a table made after a directory copied the
 * kernel's entries would be missing from it. On i686 `va_start` must be
 * aligned to a table's span, 4 MiB; on x86-64 it need not be, and the range
 * must lie in the PML4 entries the boot made. Returns 0, or -1 when a table
 * cannot be had or the range runs past the end of the address space. Each
 * architecture's mmu.c defines it.
 */
int premap_kernel_window(uintptr_t va_start, size_t size);

/**
 * Map the frame at `phys` at the page-aligned kernel address `va`, present
 * and supervisor-only, writable when `writable` says, and drop the address's
 * old translation (`paging_flush_tlb`). The page table must exist
 * (`premap_kernel_window`); where it does not, nothing is mapped. The page
 * cache maps a frame read-only on a read fault, so that the first write
 * faults again and marks it dirty.
 */
void map_kernel_frame(uintptr_t va, uintptr_t phys, int writable);

/** Clear the entry of the page-aligned kernel address `va` and drop its
 * translation (`paging_flush_tlb`); nothing where the page table is
 * absent. */
void unmap_kernel_frame(uintptr_t va);

/** The kernel's directory, whose tables of the kernel's layout every
 * directory shares. */
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
extern size_t _frames_number; /* the words of the table, the map's top frame */

/**
 * Take another reference on the frame at the physical address `phys`, for a
 * page that another mapping shares, as after a fork. Nothing for a free
 * frame, one past the table, a state word, or a frame already at the most
 * references, FRAME_REF_MAX - 1.
 */
void frame_ref(uintptr_t phys);

/**
 * Drop a reference on the frame at `phys`; the frame goes back to the pool
 * with its last. Returns the references left, 0 when the frame is free now,
 * and also 0 for a frame past the table or one already free; a state word
 * is returned as it is and nothing changes.
 */
uint32_t frame_unref(uintptr_t phys);

#endif
