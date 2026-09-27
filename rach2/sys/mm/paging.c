#include "../cmdline.h"
#include "../klog.h"
#include "paging.h"
#include "kheap.h"
#include "pagecache.h"
#include "frame.h"
#include "usercopy.h"
#include "../serial.h"
#include "../lib/ksignal.h"
#include "../proc/process.h"
#include "pressure.h"

/* For `terminate_current_process`, with which the fault handler ends a
 * process that faulted in user mode. */
#include "../../kernel.h"

/*
 * tests/host/paging_hosttest.c compiles this file natively with
 * RACH2_HOSTTEST defined. That build leaves out every instruction that
 * reaches the control registers, the flags or the TLB, which a host cannot
 * execute, and takes a fault's address from the test; the rest of the file
 * compiles as it is.
 */
#ifdef RACH2_HOSTTEST
uint32_t paging_hosttest_cr2(void);
#endif

page_dir_t *_kernel_dir;
page_dir_t *_curr_dir;

uint32_t *_frames;
size_t _frames_number;
/* The frames below the identity-mapped window, which `alloc_kernel_frame`
 * hands out: a frame the kernel reaches through its physical address. */
static size_t _window_frames;
static uint32_t _managed_frames;
static uint32_t _free_frames; /* words of the table that are 0, kept by every writer */

static void frame_take(uintptr_t frame);
/* Where the next scan for a free frame starts: no frame below it is free.
 * A scan from the bottom of a million-frame table for every page would be
 * the table's length squared over a large mapping. */
static uintptr_t _next_free_frame;
/* The hint for the frames above the identity-mapped window, which are
 * taken first: the window's frames are the only ones a device can be given
 * the address of, so the last WINDOW_KEEP of them are left to the requests
 * and the page cache, which take from the window's top. */
static uintptr_t _next_free_upper;
static uint32_t _free_window_frames; /* free frames below the window */
#define WINDOW_KEEP 64u
/* rach2.framefail: the allocation that fails on purpose, counted down. */
static uint32_t _framefail_left;
static int _framefail_on = -1;

/* Whether this allocation is the one armed to fail. */
static int framefail_now(void)
{
    if (_framefail_left == 0 || !current_proc || !current_proc->in_syscall) {
        return 0;
    }
    if (--_framefail_left != 0) {
        return 0;
    }
    klog(KLOG_WARN, "vm", "frame allocation failed on purpose (rach2.framefail)");
    return 1;
}

int paging_framefail_arm(uint32_t count)
{
    if (_framefail_on < 0) {
        _framefail_on = kcmdline_has("rach2.framefail");
    }
    if (!_framefail_on) {
        return -1;
    }
    _framefail_left = count;
    return 0;
}

/* The temporary mappings: one bit per slot of the region, set while a
 * caller holds it. */
static uint32_t _temp_slots[KVA_TEMP_SLOTS / 32];
/* The next free page of the MMIO region; a region is mapped for good. */
static uintptr_t _mmio_next = KVA_MMIO;

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
 * The first frame above the boot's reservations and the floor of every scan
 * of the window for a free frame. Set once at init time to
 * `mem_reserved_ptr / PAGE_SIZE`, so a frame freed below a break that a later
 * `mem_reserve` raised is still found.
 */
static uintptr_t _user_frames_start = 0;

/* Frames `alloc_kernel_frame` has taken from the top of the window, the page
 * cache's among them, for the record a failed user allocation writes. */
static uint32_t _kernel_frames_reserved;

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
        uintptr_t frame = (uintptr_t)table / PAGE_SIZE;
        if (frame < _frames_number) {
            frame_take(frame);
        }
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
        for (i = 0; i < num_frames && (frame + i) < _frames_number; i++) {
            _frames[frame + i] = 1;
        }
        k_memset(dir, 0, sizeof(page_dir_t));
    }
    return dir;
}

static void _page_fault_handler(isr_params_t *params);

static void _load_page_dir(page_dir_t *dir);

/**
 * Build the frame table and the kernel directory, turn paging on, and start
 * the heap.
 *
 * The frame table has one word per frame below the map's top, and lies
 * below the window with the other boot tables; a map with nothing usable
 * gets the window, so a machine whose firmware gives no map still boots.
 * The boot allocator placed the table before the map could be consulted, so
 * a table that lies over memory the map does not offer would overwrite
 * firmware's memory, and the machine stops rather than run on it. A frame
 * the map does not offer is marked `FRAME_ABSENT`.
 *
 * The kernel directory identity-maps the window, supervisor-only, so the
 * kernel reaches any physical address below `HARD_MEMORY_LIMIT` directly,
 * and has a page table for every 4 MiB of the kernel's gigabyte (256 tables,
 * a megabyte), shared by every directory cloned later, so a mapping made
 * there after the first fork is seen by every process. Every frame below
 * `mem_reserved_ptr` is then marked used: the identity loop marks the frames
 * below the break as it passes them, and a final pass marks them all once
 * more, for the tables carved at frames the loop had already passed and for
 * those carved after it. The map has the same range reserved, and what the
 * allocator manages and has free is counted from the table and not from the
 * map's arithmetic, so the boot's report compares the two.
 */
void init_paging(struct frame_map *map)
{
    unsigned int frame;

    uintptr_t tmp, phys_address, table_index, page_index;

    page_table_t *table;

    /* frame.h mirrors these two values so it can stay host-linkable; fail
     * loudly if the mirror ever drifts from the real definitions. */
    if (FRAME_DIRECT_MAP_LIMIT != HARD_MEMORY_LIMIT || FRAME_PAGE_SIZE != PAGE_SIZE) {
        k_panic("frame.h constants drifted from paging.h");
    }

    _frames_number = map->top_frame ? map->top_frame : (HARD_MEMORY_LIMIT / PAGE_SIZE);
    _window_frames = _frames_number < HARD_MEMORY_LIMIT / PAGE_SIZE ? _frames_number
                                                                    : HARD_MEMORY_LIMIT / PAGE_SIZE;
    _frames = mem_reserve_ptr(_frames_number * sizeof(uint32_t), true);
    if (_frames == NULL) {
        k_panic("init_paging: no reserved memory for the frame table");
    }
    if (map->top_frame) {
        uintptr_t first = (uintptr_t)_frames / PAGE_SIZE;
        uintptr_t last = ((uintptr_t)_frames + _frames_number * sizeof(uint32_t) - 1) / PAGE_SIZE;

        for (frame = first; frame <= last; frame++) {
            if (!frame_map_usable(map, (uint32_t)frame)) {
                k_panic("init_paging: the frame table lies over memory the map does not offer");
            }
        }
    }
    _managed_frames = 0;
    for (frame = 0; frame < _frames_number; frame++) {
        if (map->top_frame == 0 || frame_map_usable(map, (uint32_t)frame)) {
            _frames[frame] = 0;
            _managed_frames++;
        } else {
            _frames[frame] = FRAME_ABSENT;
        }
    }

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

            /* This page table is writable only by a supervisor.
             * Page table address | 'present' and 'rw' flags */
            _curr_dir->phys_page_tables[table_index] = tmp | 0x3;

            _curr_dir->logical_page_tables[table_index] = table;
        }

        /* The frame table ends first when memory is smaller than the window. */
        if (phys_address < mem_reserved_ptr && frame < _frames_number) {
            frame_take(frame);
        }
        frame++;

        /*
         * This page is writable only by a supervisor.
         * Page frame address | 'present' and 'rw' flags
         */
        table->pages[page_index] = phys_address | 0x3;

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
        _curr_dir->phys_page_tables[table_index] = tmp | 0x3;
        _curr_dir->logical_page_tables[table_index] = (page_table_t *)tmp;
    }

    {
        uintptr_t final_frame = mem_reserved_ptr / PAGE_SIZE;
        uintptr_t f;
        for (f = 0; f < final_frame && f < _frames_number; f++) {
            _frames[f] = 1;
        }
    }

    _user_frames_start = mem_reserved_ptr / PAGE_SIZE;
    _next_free_frame = _user_frames_start;
    _next_free_upper = _window_frames;
    if (map->top_frame) {
        frame_map_reserve(map, 0, mem_reserved_ptr);
    }
    _managed_frames = 0;
    for (frame = _user_frames_start; frame < _frames_number; frame++) {
        if (_frames[frame] == 0) {
            _managed_frames++;
        }
    }
    _free_frames = 0;
    _free_window_frames = 0;
    for (frame = 0; frame < _frames_number; frame++) {
        if (_frames[frame] == 0) {
            _free_frames++;
            if (frame < _window_frames) {
                _free_window_frames++;
            }
        }
    }

    register_isr_handler(14, &_page_fault_handler, "page fault");

    _load_page_dir(_curr_dir);

    /* From here the heap exists, and every page table and directory made
     * later comes from it. */
    kheap_init();
    _heap_ready = 1;
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

    __asm__ __volatile__("mov %0, %%cr3" ::"r"(&dir->phys_page_tables));
    __asm__ __volatile__("mov %%cr0, %0" : "=r"(cr0));

    /* PG is bit 31 and WP bit 16. */
    cr0 |= 0x80000000u | 0x00010000u;

    __asm__ __volatile__("mov %0, %%cr0" ::"r"(cr0));
#else
    (void)dir;
#endif
}

/* Page-fault error-code bits (Intel SDM Vol. 3A, 4.7). */
#define PF_ERR_PRESENT 0x1u /* 0 = not-present page, 1 = protection violation */
#define PF_ERR_WRITE   0x2u /* 0 = read access, 1 = write access */
#define PF_ERR_USER    0x4u /* 0 = supervisor mode, 1 = user mode */

static int cow_fault(uint32_t addr);

/**
 * Is the page containing `addr` writable in `dir`? Present and write-enabled;
 * a present copy-on-write page answers 0, which is what tells the resolver to
 * make it private before promising a caller it may write there.
 */
static int pte_writable(page_dir_t *dir, uint32_t addr)
{
    uint32_t page_num = addr / PAGE_SIZE;
    uint32_t table_index = page_num / 1024;
    uint32_t page_index = page_num % 1024;
    page_table_t *table;

    if (table_index >= 1024) {
        return 0;
    }
    table = dir->logical_page_tables[table_index];
    if (!table) {
        return 0;
    }
    return (table->pages[page_index] & 0x3) == 0x3;
}

/**
 * Is the page containing `addr` already present in `dir`? Keeps the
 * demand-paging resolver idempotent: an eager-loaded code/data page, or one
 * the pre-fault path already backed, must not be allocated a second time
 * (which would leak the old frame and wipe the page's contents).
 */
static int pte_present(page_dir_t *dir, uint32_t addr)
{
    uint32_t page_num = addr / PAGE_SIZE;
    uint32_t table_index = page_num / 1024;
    uint32_t page_index = page_num % 1024;
    page_table_t *table;

    if (table_index >= 1024) {
        return 0;
    }
    table = dir->logical_page_tables[table_index];
    if (!table) {
        return 0;
    }
    return (table->pages[page_index] & 0x1) != 0;
}

/* Take back the page `alloc_user_page` mapped at `addr` in `dir`: the
 * entry cleared and the frame given back. For a page that could not be
 * made ready. */
static void unmap_user_page_frame(page_dir_t *dir, uint32_t addr)
{
    page_table_t *table = dir->logical_page_tables[addr / PAGE_SIZE / 1024];
    uint32_t index = (addr / PAGE_SIZE) % 1024;
    uint32_t pte;

    if (!table) {
        return;
    }
    pte = table->pages[index];
    if (!(pte & 0x1)) {
        return;
    }
    table->pages[index] = 0;
    paging_flush_tlb(addr);
    frame_unref(pte & ~0xFFFu);
}

/**
 * Demand-paging resolver. Decide whether an access at the (not necessarily
 * aligned) `addr` is legal for the current process and, if so, make sure the
 * page is backed. `write` is 1 for a write.
 *
 * Outside every region the one legal access grows the stack down
 * (`vm_grow_stack`, under RLIMIT_AS). Inside one, the region's protection
 * must allow the access: a write to a read-only region and any access to a
 * PROT_NONE region are never backed. A present page is left as it is, except
 * that a write to one that is not writable goes to `cow_fault`: a page shared
 * by a fork is present, read-only and copy-on-write in a writable region, and
 * a pre-fault that answered "backed" would promise the kernel a page it
 * cannot write. A present page that is neither writable nor copy-on-write
 * then reports unresolved, since reporting it backed would fault on the same
 * instruction forever.
 *
 * A missing page is heap, stack or an anonymous mapping, since exec loads
 * code and data whole. It gets a zeroed frame, read-only in the PTE when its
 * region is, so a later write faults; a grown stack page stays writable. The
 * caches give frames back first when memory is short (`mem_balance`), which
 * inside a system call may sleep in a writeback.
 *
 * Returns 1 if the page is now mapped, 0 if the access is illegal or no
 * frame could be had or zeroed; the caller then segfaults the process, or a
 * pre-fault answers -EFAULT. The current process always has a user address
 * space here (the handler and the pre-fault path both check
 * `current_proc->cr3 != 0`).
 */
static int vm_resolve(uint32_t addr, int write)
{
    vm_space_t *vm = &current_proc->vm;
    vma_t *vma = vm_find(vm, addr);
    uintptr_t phys;

    if (!vma) {
        if (!vm_grow_stack(vm, addr & ~(PAGE_SIZE - 1), current_proc->rlim[RLIM_AS].cur)) {
            return 0;
        }
        /* A grown stack page is read+write; fall through to back it. */
    } else {
        if (write) {
            if (!(vma->prot & VM_PROT_WRITE)) {
                return 0;
            }
        } else if (!(vma->prot & VM_PROT_READ)) {
            return 0;
        }
        if (pte_present(_curr_dir, addr)) {
            if (write && !pte_writable(_curr_dir, addr)) {
                return cow_fault(addr);
            }
            return 1; /* already backed; nothing to do */
        }
    }

    mem_balance();
    phys = alloc_user_page(_curr_dir, addr);
    if (phys == 0) {
        return 0; /* out of frames */
    }

    if (paging_zero_frame(phys) != 0) {
        unmap_user_page_frame(_curr_dir, addr);
        return 0;
    }

    if (vma && !(vma->prot & VM_PROT_WRITE)) {
        paging_set_writable(_curr_dir, addr, false);
    }
    return 1;
}

int vm_prefault_range(uint32_t addr, uint32_t len, int write)
{
    uint32_t page, end;

    if (!current_proc || current_proc->cr3 == 0) {
        return 0; /* no user address space to fault against */
    }
    if (len == 0) {
        return 1; /* an empty range touches nothing */
    }

    /* A wrapping range would end the loop early and leave its tail unbacked. */
    end = addr + len;
    if (end < addr) {
        return 0;
    }
    for (page = addr & ~(PAGE_SIZE - 1); page < end; page += PAGE_SIZE) {
        if (!vm_resolve(page, write)) {
            return 0;
        }
    }
    return 1;
}

/**
 * Copy-on-write resolver for a write fault on a present, PTE_COW page in the
 * current address space. If this process is the frame's last owner (refcount
 * 1) the page is just made writable in place; otherwise a private writable
 * copy is allocated, the shared bytes are copied in, and the shared frame's
 * reference is dropped. Returns 1 if it handled a COW page, 0 if the page is
 * not COW or no frame could be had or copied for it (the caller continues to
 * demand paging / segfault).
 */
static int cow_fault(uint32_t addr)
{
    uint32_t page_num = addr / PAGE_SIZE;
    uint32_t table_index = page_num / 1024;
    uint32_t page_index = page_num % 1024;
    page_table_t *table;
    uint32_t pte, old_phys, frame;
    uintptr_t new_phys;

    if (table_index >= 1024) {
        return 0;
    }
    table = _curr_dir->logical_page_tables[table_index];
    if (!table) {
        return 0;
    }
    pte = table->pages[page_index];
    if (!(pte & 0x1) || !(pte & PTE_COW)) {
        return 0; /* not a copy-on-write page */
    }

    old_phys = pte & ~0xFFFu;
    frame = old_phys / PAGE_SIZE;

    /* Sole owner: re-grant write and drop the COW mark in place, no copy. */
    if (frame >= _frames_number || _frames[frame] <= 1) {
        table->pages[page_index] = (pte & ~PTE_COW) | 0x2u; /* +write, -COW */
        paging_flush_tlb(addr);
        return 1;
    }

    mem_balance(); /* the caches give frames back first when memory is short */
    new_phys = alloc_user_page(_curr_dir, addr);
    if (new_phys == 0) {
        return 0; /* out of frames -> caller segfaults the process */
    }
    if (paging_copy_frame(new_phys, old_phys) != 0) {
        /* Nothing was copied: the shared page is put back as it was, the
         * fresh frame goes back, and the fault is unresolved. */
        table->pages[page_index] = pte;
        paging_flush_tlb(addr);
        frame_unref(new_phys);
        return 0;
    }
    frame_unref(old_phys);
    return 1;
}

/**
 * Resolve a page fault, or end what caused it.
 *
 * Tried in order. Any fault at a user-space address of a live user process
 * goes to `vm_resolve`, keyed on the address and not on the processor's mode,
 * so a kernel touch of a lazy user page resolves too. A write to a page shared
 * by a fork reaches `cow_fault` through it, and only once the region's
 * protection allows the write: a region mprotect made read-only keeps its
 * copy-on-write pages read-only. A supervisor fault inside
 * the page-cache window is filled by `pc_fault`, whose polled read is safe
 * with interrupts off; a fault past the backed size, or one the fill cannot
 * serve, panics. The window is a kernel-only mapping, so a user-mode fault
 * there, which would fault again on the supervisor-only page the fill maps,
 * is not filled and falls through to the segfault.
 *
 * What is left kills the process with SIGSEGV when the fault came from user
 * mode, and otherwise panics: the usercopy helpers return -EFAULT before the
 * kernel touches a bad user pointer, so a kernel-mode fault here, or one
 * with no user context, is a kernel bug.
 */
static void _page_fault_handler(isr_params_t *params)
{
    uint32_t fault_address;
    int write, user_mode, have_user_proc, in_user_range;

#ifndef RACH2_HOSTTEST
    __asm__ __volatile__("mov %%cr2, %0" : "=r"(fault_address));
#else
    fault_address = paging_hosttest_cr2();
#endif

    write = (params->error_code & PF_ERR_WRITE) != 0;
    user_mode = (params->error_code & PF_ERR_USER) != 0;
    have_user_proc = (current_proc != NULL && current_proc->cr3 != 0);
    in_user_range = (fault_address >= USER_ADDR_MIN && fault_address < USER_ADDR_MAX);

    if (have_user_proc && in_user_range) {
        if (vm_resolve(fault_address, write)) {
            return; /* page mapped; iret retries the faulting instruction */
        }
    }

    if (!user_mode && fault_address >= PCWIN_BASE &&
        fault_address < PCWIN_BASE + pc_window_size()) {
        if (!current_proc) {
            k_panic("pagecache: window fault with no current process");
        }
        if (pc_fault(fault_address, write)) {
            return;
        }
        k_panic("pagecache: unrecoverable disk fault");
    }

    if (user_mode && have_user_proc) {
        struct k_siginfo info;

        /* SIGSEGV with the address, SEGV_ACCERR when a region holds it and
           SEGV_MAPERR when none does, forced as Linux forces it: a handler
           runs, and the faulting instruction runs again after it returns. */
        k_memset(&info, 0, sizeof(info));
        info.code = vm_find(&current_proc->vm, fault_address) ? SEGV_ACCERR : SEGV_MAPERR;
        info.addr = fault_address;
        current_proc->trap_nr = 14;
        current_proc->trap_err = params->error_code;
        current_proc->trap_addr = fault_address;
        signal_force(current_proc, SIGSEGV, &info);
        if (current_proc->sigacts[SIGSEGV].handler == KSIG_DFL) {
            klog(KLOG_WARN, "vm", "fault: killing pid %d: bad %s at 0x%x (err 0x%x)",
                 current_proc->pid, write ? "write" : "read", fault_address, params->error_code);
        }
        return;
    }

    klog(KLOG_CRIT, "vm", "fault: in kernel mode at 0x%X (err 0x%X), eip 0x%X", fault_address,
         params->error_code, params->eip);

    k_panic("Unrecoverable error: page fault while in kernel mode.");
}

/**
 * Map a physical MMIO region into the kernel's MMIO region and return the
 * virtual address it was given, or 0 when the region has no room or the
 * range is empty or wraps. The entries are cache-disabled (PCD|PWT) so a
 * device-register read is never stale, whatever the region's address. The
 * page tables of the kernel's gigabyte exist at boot and are shared by
 * every directory, so a mapping made here at any time is seen by every
 * process.
 *
 * The page count is rounded up by division and not by adding a page less
 * one: a region reaching the top of the address space would wrap that sum to
 * a few bytes, ask for no pages, map nothing and still return an address.
 */
uintptr_t map_mmio(uintptr_t phys_addr, size_t size)
{
    uintptr_t start_page = phys_addr & ~(PAGE_SIZE - 1);
    uintptr_t end_addr = phys_addr + size;
    uintptr_t pages, span, i, va;

    if (size == 0 || end_addr < phys_addr) {
        klog(KLOG_ERR, "vm", "map_mmio: bad region 0x%x, %d bytes", phys_addr, size);
        return 0;
    }
    span = end_addr - start_page;
    pages = span / PAGE_SIZE + ((span % PAGE_SIZE) ? 1u : 0u);
    if (pages > (KVA_MMIO + KVA_MMIO_SIZE - _mmio_next) / PAGE_SIZE) {
        klog(KLOG_ERR, "vm", "map_mmio: the MMIO region is full at 0x%x", _mmio_next);
        return 0;
    }
    va = _mmio_next;
    for (i = 0; i < pages; i++) {
        uintptr_t page_num = (va + i * PAGE_SIZE) / PAGE_SIZE;
        page_table_t *table = _kernel_dir->logical_page_tables[page_num / 1024];

        /* Present, writable, cache-disabled: a register is not memory. */
        table->pages[page_num % 1024] = (start_page + i * PAGE_SIZE) | 0x1B;
    }
    _mmio_next += pages * PAGE_SIZE;

    /* Flush TLB */
#ifndef RACH2_HOSTTEST
    __asm__ __volatile__("movl %%cr3, %%eax\n\t"
                         "movl %%eax, %%cr3" ::
                             : "eax", "memory");
#endif

    return va + (phys_addr - start_page);
}

static uint32_t paging_irq_save(void)
{
    uint32_t flags;
#ifndef RACH2_HOSTTEST
    __asm__ __volatile__("pushfl; popl %0; cli" : "=r"(flags)::"memory");
#else
    flags = 0;
#endif
    return flags;
}

static void paging_irq_restore(uint32_t flags)
{
#ifndef RACH2_HOSTTEST
    if (flags & 0x200) {
        __asm__ __volatile__("sti");
    }
#else
    (void)flags;
#endif
}

void *kmap_temp(uintptr_t phys)
{
    uint32_t flags = paging_irq_save();
    uint32_t slot;

    for (slot = 0; slot < KVA_TEMP_SLOTS; slot++) {
        if (!(_temp_slots[slot / 32] & (1u << (slot % 32)))) {
            uintptr_t va = KVA_TEMP + slot * PAGE_SIZE;
            page_table_t *table = _kernel_dir->logical_page_tables[va / PAGE_SIZE / 1024];

            _temp_slots[slot / 32] |= 1u << (slot % 32);
            table->pages[(va / PAGE_SIZE) % 1024] = (phys & ~(PAGE_SIZE - 1)) | 0x3;
            paging_flush_tlb(va);
            paging_irq_restore(flags);
            return (void *)(va + (phys & (PAGE_SIZE - 1)));
        }
    }
    paging_irq_restore(flags);
    klog(KLOG_ERR, "vm", "temporary mappings: every slot is held");
    return 0;
}

void kunmap_temp(void *va)
{
    uintptr_t addr = (uintptr_t)va & ~(PAGE_SIZE - 1);
    uint32_t slot = (addr - KVA_TEMP) / PAGE_SIZE;
    uint32_t flags;
    page_table_t *table;

    if (addr < KVA_TEMP || slot >= KVA_TEMP_SLOTS) {
        return;
    }
    flags = paging_irq_save();
    table = _kernel_dir->logical_page_tables[addr / PAGE_SIZE / 1024];
    table->pages[(addr / PAGE_SIZE) % 1024] = 0;
    paging_flush_tlb(addr);
    _temp_slots[slot / 32] &= ~(1u << (slot % 32));
    paging_irq_restore(flags);
}

int paging_zero_frame(uintptr_t phys)
{
    void *va = kmap_temp(phys);

    if (!va) {
        return -1;
    }
    k_memset(va, 0, PAGE_SIZE);
    kunmap_temp(va);
    return 0;
}

int paging_copy_frame(uintptr_t dst, uintptr_t src)
{
    void *d = kmap_temp(dst);
    void *s = kmap_temp(src);
    int rc = d && s ? 0 : -1;

    if (d && s) {
        k_memcpy(d, s, PAGE_SIZE);
    }
    if (s) {
        kunmap_temp(s);
    }
    if (d) {
        kunmap_temp(d);
    }
    return rc;
}

/* Mark `frame` used with one reference, whatever it held. */
static void frame_take(uintptr_t frame)
{
    if (_frames[frame] == 0) {
        _free_frames--;
        if (frame < _window_frames) {
            _free_window_frames--;
        }
    }
    _frames[frame] = 1;
}

/**
 * A free frame for a user page or a kernel object: from above the window
 * first, then from below it while more than WINDOW_KEEP are free there;
 * `_frames_number` when there is none. The two hints say no frame below
 * each is free in its region.
 */
static uintptr_t find_free_frame(void)
{
    uintptr_t frame;

    for (frame = _next_free_upper; frame < _frames_number; frame++) {
        if (_frames[frame] == 0) {
            _next_free_upper = frame + 1;
            return frame;
        }
    }
    _next_free_upper = _frames_number;
    if (_free_window_frames <= WINDOW_KEEP) {
        return _frames_number;
    }
    for (frame = _next_free_frame; frame < _window_frames; frame++) {
        if (_frames[frame] == 0) {
            _next_free_frame = frame + 1;
            return frame;
        }
    }
    return _frames_number;
}

uintptr_t alloc_frame(void)
{
    uintptr_t frame;
    uint32_t flags;

    if (framefail_now() || !mem_may_take(0)) {
        return 0;
    }
    /* Selection and claim as one step: a handler's own allocation between
     * them would take the frame this one had chosen. */
    flags = paging_irq_save();
    frame = find_free_frame();
    if (frame >= _frames_number) {
        paging_irq_restore(flags);
        return 0;
    }
    frame_take(frame);
    paging_irq_restore(flags);
    return frame * PAGE_SIZE;
}

void free_frame(uintptr_t phys)
{
    frame_unref(phys);
}

uintptr_t paging_virt_to_phys(uintptr_t va)
{
    page_table_t *table = _kernel_dir->logical_page_tables[va / PAGE_SIZE / 1024];
    uint32_t pte;

    if (!table) {
        return 0;
    }
    pte = table->pages[(va / PAGE_SIZE) % 1024];
    if (!(pte & 0x1)) {
        return 0;
    }
    return (pte & ~(PAGE_SIZE - 1)) | (va & (PAGE_SIZE - 1));
}

/**
 * Reserve one free physical frame for kernel-internal use and return its
 * physical address, or 0 if none is free.
 *
 * The frame is marked used at reference count 1 and has no user mapping, so
 * `alloc_user_page` and `free_user_pages` skip it; the caller owns it until
 * `free_kernel_frame`. The scan runs down from the top of the
 * identity-mapped window, while user pages come from above the window first
 * and then from the window's bottom up, so a kernel reservation and user
 * pages rarely contend. The returned address is also the frame's virtual
 * address, since the scan stays inside the window.
 */
void *alloc_kernel_frame(void)
{
    uintptr_t frame = _window_frames;

    while (frame-- > _user_frames_start) {
        if (_frames[frame] == 0) {
            frame_take(frame);
            _kernel_frames_reserved++;
            return (void *)(frame * PAGE_SIZE);
        }
    }

    return 0;
}

void paging_mark_reserved(uintptr_t frame)
{
    if (frame < _frames_number) {
        frame_take(frame);
    }
}

void free_kernel_frame(void *frame)
{
    _kernel_frames_reserved--;
    frame_unref((uintptr_t)frame);
}

/**
 * Return the number of physical frames the pool manages.
 */
uint32_t frame_total_count(void)
{
    return _managed_frames;
}

/**
 * Return the number of physical frames currently free.
 *
 * Every writer of the frame table keeps the count, so this reads one word
 * and scans nothing. A reader gets a point-in-time count, which is what
 * sysinfo, the leak probe and reclaim need, so no locking is needed under
 * the IF discipline.
 */
uint32_t frame_free_count(void)
{
    return _free_frames;
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
            _kernel_dir->phys_page_tables[table_index] = table_phys(table) | 0x3;
        }
    }

    return 0;
}

/**
 * Map physical frame `phys` at the page-aligned kernel virtual address `va`
 * with a supervisor-only leaf PTE: present, no user bit, writable iff
 * `writable`.
 *
 * The page table for `va` must already exist (see `premap_kernel_window`).
 * The page cache maps a frame read-only on a read fault so the first write
 * faults again and can be recorded dirty, and read-write on a write fault.
 * No-op if the table is somehow absent.
 */
void map_kernel_frame(uintptr_t va, uintptr_t phys, int writable)
{
    uintptr_t page_num = va / PAGE_SIZE;
    uintptr_t table_index = page_num / 1024;
    uintptr_t page_index = page_num % 1024;
    page_table_t *table;

    if (table_index >= 1024) {
        return;
    }
    table = _kernel_dir->logical_page_tables[table_index];
    if (!table) {
        return;
    }
    table->pages[page_index] = (phys & ~(PAGE_SIZE - 1)) | (writable ? 0x3u : 0x1u);
    paging_flush_tlb(va);
}

/**
 * Clear the leaf PTE for the page-aligned kernel virtual address `va` and
 * flush its TLB entry. No-op if the page table is absent. Used by page-cache
 * eviction.
 */
void unmap_kernel_frame(uintptr_t va)
{
    uintptr_t page_num = va / PAGE_SIZE;
    uintptr_t table_index = page_num / 1024;
    uintptr_t page_index = page_num % 1024;
    page_table_t *table;

    if (table_index >= 1024) {
        return;
    }
    table = _kernel_dir->logical_page_tables[table_index];
    if (!table) {
        return;
    }
    table->pages[page_index] = 0;
    paging_flush_tlb(va);
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

    /* U/S is bit 2 (0x4): set in both the page table and directory entry. */
    if (user) {
        table->pages[page_index] |= 0x4;
        _curr_dir->phys_page_tables[table_index] |= 0x4;
    } else {
        table->pages[page_index] &= ~0x4;
    }

    paging_flush_tlb(addr);
}

void paging_set_writable(page_dir_t *dir, uintptr_t addr, bool writable)
{
    uintptr_t page_num = addr / PAGE_SIZE;
    uintptr_t table_index = page_num / 1024;
    uintptr_t page_index = page_num % 1024;
    page_table_t *table;

    if (table_index >= 1024) {
        return;
    }
    table = dir->logical_page_tables[table_index];
    if (!table) {
        return;
    }
    if (!(table->pages[page_index] & 0x1)) {
        return; /* not present */
    }

    if (writable) {
        table->pages[page_index] |= 0x2;
    } else {
        table->pages[page_index] &= ~0x2; /* read-only for both ring 3 and 0 */
    }

    if (dir == _curr_dir) {
        paging_flush_tlb(addr);
    }
}

void paging_flush_tlb(uintptr_t addr)
{
#ifndef RACH2_HOSTTEST
    __asm__ __volatile__("invlpg (%0)" ::"r"(addr) : "memory");
#else
    (void)addr;
#endif
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
    __asm__ __volatile__("mov %0, %%cr3" ::"r"(get_page_dir_phys(dir)) : "memory");
#endif
}

void frame_ref(uintptr_t phys)
{
    uintptr_t frame = phys / PAGE_SIZE;

    /* Only a live frame can gain a sharer, and the words at and above
     * `FRAME_REF_MAX` are states, not counts. */
    if (frame < _frames_number && _frames[frame] != 0 && _frames[frame] < FRAME_REF_MAX - 1) {
        _frames[frame]++;
    }
}

uint32_t frame_unref(uintptr_t phys)
{
    uintptr_t frame = phys / PAGE_SIZE;

    if (frame >= _frames_number || _frames[frame] == 0) {
        return 0;
    }
    /* A state word is not a count: a frame the map does not offer or the
     * kernel cannot reach has no mapping to drop. */
    if (_frames[frame] >= FRAME_REF_MAX) {
        return _frames[frame];
    }
    _frames[frame]--;
    if (_frames[frame] == 0) {
        _free_frames++;
        if (frame < _window_frames) {
            _free_window_frames++;
            if (frame >= _user_frames_start && frame < _next_free_frame) {
                _next_free_frame = frame;
            }
        } else if (frame < _next_free_upper) {
            _next_free_upper = frame;
        }
    }
    return _frames[frame];
}

/**
 * Ensure `dir` has a PRIVATE (not kernel-shared) page table for
 * `table_index` and return it, or NULL on allocation failure. A table
 * inherited from the kernel directory by clone_page_directory is copied so
 * per-process mappings stay isolated; a missing table is allocated; an
 * existing private table is made user-accessible. Used by alloc_user_page
 * and by map_user_frame, through which fork shares a frame copy-on-write.
 */
static page_table_t *ensure_user_table(page_dir_t *dir, uintptr_t table_index)
{
    page_table_t *table = dir->logical_page_tables[table_index];

    if (table && dir != _kernel_dir && table == _kernel_dir->logical_page_tables[table_index]) {
        page_table_t *new_table = alloc_page_table();
        uint32_t i;

        if (!new_table) {
            klog(KLOG_ERR, "vm", "user table: the kernel page table did not copy");
            return NULL;
        }
        for (i = 0; i < 1024; i++) {
            new_table->pages[i] = table->pages[i];
        }
        dir->phys_page_tables[table_index] = table_phys(new_table) | 0x7; /* P+RW+U */
        dir->logical_page_tables[table_index] = new_table;
        table = new_table;
    } else if (!table) {
        table = alloc_page_table();
        if (!table) {
            klog(KLOG_ERR, "vm", "user table: no table could be allocated for index %d",
                 (int)table_index);
            return NULL;
        }
        dir->phys_page_tables[table_index] = table_phys(table) | 0x7; /* P+RW+U */
        dir->logical_page_tables[table_index] = table;
    } else if (!(dir->phys_page_tables[table_index] & 0x4)) {
        dir->phys_page_tables[table_index] |= 0x4; /* make table user-accessible */
    }
    return table;
}

int map_user_frame(page_dir_t *dir, uintptr_t virt, uintptr_t phys, uint32_t flags)
{
    uintptr_t page_num, table_index, page_index;
    page_table_t *table;

    virt &= ~(PAGE_SIZE - 1);
    page_num = virt / PAGE_SIZE;
    table_index = page_num / 1024;
    page_index = page_num % 1024;
    if (table_index >= 1024) {
        return -1;
    }

    table = ensure_user_table(dir, table_index);
    if (!table) {
        return -1;
    }

    /* The caller owns the frame and its reference count; we only write the
     * PTE. */
    table->pages[page_index] = (phys & ~0xFFFu) | (flags & 0xFFFu);
    if (dir == _curr_dir) {
        paging_flush_tlb(virt);
    }
    return 0;
}

uintptr_t alloc_user_page(page_dir_t *dir, uintptr_t virt_addr)
{
    uintptr_t phys_addr;
    uintptr_t page_num;
    uintptr_t table_index;
    uintptr_t page_index;
    uintptr_t frame;
    page_table_t *table;
    uint32_t flags;

    /* Align to page boundary */
    virt_addr &= ~(PAGE_SIZE - 1);

    /* Calculate table and page indices */
    page_num = virt_addr / PAGE_SIZE;
    table_index = page_num / 1024;
    page_index = page_num % 1024;

    if (table_index >= 1024) {
        klog(KLOG_ERR, "vm", "user page: table index %d is past the directory", (int)table_index);
        return 0;
    }

    /* The table first: one that cannot be had leaves no frame to give back. */
    table = ensure_user_table(dir, table_index);
    if (!table) {
        return 0;
    }

    if (!mem_may_take(1)) {
        klog(KLOG_WARN, "vm", "user page: refused: the free frames are the reserve's, pid %d",
             current_proc ? (int)current_proc->pid : 0);
        return 0;
    }
    /* Selection and claim as one step, as in `alloc_frame`; the refusals
     * come first, so a refused selection does not move the hint past a
     * free frame. */
    flags = paging_irq_save();
    frame = framefail_now() ? _frames_number : find_free_frame();
    if (frame < _frames_number) {
        frame_take(frame);
    }
    paging_irq_restore(flags);

    if (frame >= _frames_number) {
        /* Who holds the pool, so that a machine that runs out says what
         * filled it: the boot's reservations below the user frames, the
         * kernel's frames from the top, and the user frames between, with
         * the shared ones counted apart. */
        uint32_t user_used = 0, user_shared = 0;
        uintptr_t f;

        for (f = _user_frames_start; f < _frames_number; f++) {
            if (_frames[f] != 0 && _frames[f] < FRAME_REF_MAX) {
                user_used++;
            }
            if (_frames[f] > 1 && _frames[f] < FRAME_REF_MAX) {
                user_shared++;
            }
        }
        user_used -= user_used < _kernel_frames_reserved ? user_used : _kernel_frames_reserved;
        klog(KLOG_ERR, "vm",
             "user page: no free frames in %d: %d boot, %d kernel, %d user of which %d shared, pid "
             "%d",
             (int)_frames_number, (int)_user_frames_start, (int)_kernel_frames_reserved,
             (int)user_used, (int)user_shared, current_proc ? current_proc->pid : 0);
        return 0; /* No free frames */
    }

    phys_addr = frame * PAGE_SIZE;

    /* Map the page with present, rw, and user bits */
    table->pages[page_index] = phys_addr | 0x7; /* P + RW + U/S */

    /* Flush TLB for this address if we're in the current directory */
    if (dir == _curr_dir) {
        paging_flush_tlb(virt_addr);
    }

    return phys_addr;
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
            uint32_t pte = table->pages[page_idx];
            uint32_t virt_addr;
            uint32_t phys_addr;

            /* Present is bit 0 (0x1), U/S is bit 2 (0x4); a PROT_NONE page
               is a user page without U/S. */
            if (!(pte & 0x1) || !(pte & (0x4 | PTE_NONE))) {
                continue;
            }

            virt_addr = (dir_idx << 22) | (page_idx << 12);
            phys_addr = pte & ~0xFFF;

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

            /* Flush TLB if this is the current directory */
            if (dir == _curr_dir) {
                paging_flush_tlb(virt_addr);
            }
        }
    }
}

void unmap_user_range(page_dir_t *dir, uint32_t start, uint32_t end)
{
    uint32_t addr;

    if (!dir) {
        return;
    }

    for (addr = start; addr < end; addr += PAGE_SIZE) {
        uint32_t page_num = addr / PAGE_SIZE;
        uint32_t table_index = page_num / 1024;
        uint32_t page_index = page_num % 1024;
        page_table_t *table;
        uint32_t pte, phys;

        if (table_index >= 1024) {
            continue;
        }
        table = dir->logical_page_tables[table_index];
        if (!table) {
            continue;
        }
        /* Never touch a page table shared with the kernel directory. */
        if (table == _kernel_dir->logical_page_tables[table_index]) {
            continue;
        }

        pte = table->pages[page_index];
        if (!(pte & 0x1)) {
            continue; /* not present: a never-faulted lazy page, nothing to free */
        }

        /* Drop the mapping's reference (frees the frame on the last
         * reference). */
        phys = pte & ~0xFFF;
        frame_unref(phys);
        table->pages[page_index] = 0;
        if (dir == _curr_dir) {
            paging_flush_tlb(addr);
        }
    }
}

int paging_store_u32(page_dir_t *dir, struct vm_space *vm, uint32_t addr, uint32_t value)
{
    uint32_t page_num = addr / PAGE_SIZE;
    uint32_t table_index = page_num / 1024;
    uint32_t page_index = page_num % 1024;
    page_table_t *table;
    vma_t *vma = vm_find(vm, addr);
    uint32_t pte, frame;
    uintptr_t phys;
    uint32_t *word;

    if (!vma || !(vma->prot & VM_PROT_WRITE) || (addr & 3u) != 0) {
        return -1;
    }
    table = dir->logical_page_tables[table_index];
    pte = table ? table->pages[page_index] : 0;
    if (!(pte & 0x1)) {
        phys = alloc_user_page(dir, addr);
        if (phys == 0 || paging_zero_frame(phys) != 0) {
            return -1;
        }
    } else {
        phys = pte & ~0xFFFu;
        frame = phys / PAGE_SIZE;
        if (frame < _frames_number && _frames[frame] > 1) {
            uintptr_t copy = alloc_user_page(dir, addr);

            if (copy == 0) {
                return -1;
            }
            if (paging_copy_frame(copy, phys) != 0) {
                table->pages[page_index] = pte;
                frame_unref(copy);
                return -1;
            }
            frame_unref(phys);
            phys = copy;
        } else if (pte & PTE_COW) {
            table->pages[page_index] = (pte & ~PTE_COW) | 0x2u;
            if (dir == _curr_dir) {
                paging_flush_tlb(addr);
            }
        }
    }
    word = (uint32_t *)kmap_temp(phys);
    if (!word) {
        return -1;
    }
    word[(addr & (PAGE_SIZE - 1)) / 4] = value;
    kunmap_temp(word);
    return 0;
}

void paging_protect_range(page_dir_t *dir, uint32_t start, uint32_t end, uint32_t prot)
{
    uint32_t addr;

    for (addr = start; addr < end; addr += PAGE_SIZE) {
        uint32_t page_num = addr / PAGE_SIZE;
        uint32_t table_index = page_num / 1024;
        uint32_t page_index = page_num % 1024;
        page_table_t *table;
        uint32_t pte, frame;

        if (table_index >= 1024) {
            break;
        }
        table = dir->logical_page_tables[table_index];
        if (!table || table == _kernel_dir->logical_page_tables[table_index]) {
            continue;
        }
        pte = table->pages[page_index];
        if (!(pte & 0x1)) {
            continue; /* not resident: the fault reads the region's protection */
        }
        if (!(prot & (VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXEC))) {
            pte = (pte & ~0x6u) | PTE_NONE;
        } else {
            pte = (pte & ~PTE_NONE) | 0x4u;
            frame = (pte & ~0xFFFu) / PAGE_SIZE;
            if (!(prot & VM_PROT_WRITE)) {
                pte &= ~0x2u;
            } else if (pte & PTE_COW) {
                /* Still shared: the first write copies or takes it. */
            } else if (frame < _frames_number && _frames[frame] > 1) {
                pte = (pte & ~0x2u) | PTE_COW;
            } else {
                pte |= 0x2u;
            }
        }
        table->pages[page_index] = pte;
        if (dir == _curr_dir) {
            paging_flush_tlb(addr);
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
