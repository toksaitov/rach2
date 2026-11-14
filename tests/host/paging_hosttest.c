/**
 * Host test of the range checks of `map_mmio` and `premap_kernel_window`, of
 * the temporary mappings, and of the panic for a supervisor fault in a
 * kernel stack's guard page, on i686.
 *
 * host-deps: tests/host/compat/vm_iter_host.c
 * host-deps: rach2/sys/mm/frame.c rach2/sys/mm/notify.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/spinlock_host.c tests/host/compat/atomic_host.c
 * host-deps: tests/host/compat/mmap_lock_host.c
 * host-cflags: -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast
 * host-cflags: -U__UINTPTR_TYPE__ -D__UINTPTR_TYPE__=unsigned
 * host-cflags: -U__INTPTR_TYPE__ -D__INTPTR_TYPE__=int
 * host-cflags: -U__SIZE_TYPE__ -D__SIZE_TYPE__=unsigned
 * host-cflags: -U__PTRDIFF_TYPE__ -D__PTRDIFF_TYPE__=int
 * host-cflags: -DRACH2_HOST_CPU
 *
 * `map_mmio` (mm/paging.c) and `premap_kernel_window` (arch/i686/mmu.c)
 * each take a range of the 32-bit address space, and each fails, as its
 * header says, for a range that runs past the top of it. No caller in the
 * kernel passes such a range, so only a test can show the failures.
 * lib/kint.h and lib/kdef.h take `uintptr_t` and `size_t` from the
 * compiler's own names for them, which the flags above make 32 bits wide, as
 * they are on an i686, so every sum in the two functions wraps at 2^32 as it
 * does there.
 *
 * paging.c and the i686's mmu.c are included whole, which reaches the MMIO
 * region's next free page. The processor primitives they call through arch.h,
 * for the control registers, the flags and the TLB, are the test's, and so is
 * the page directory. The heap is a stub that counts the page tables it gives,
 * so a row can say how many tables a window took; the boot allocator must not
 * be reached after the heap exists, and a row that reaches it fails.
 *
 * The rows: a register page mapped at the MMIO region's start; two spans
 * that begin in the first page and end within the last one, whose page
 * count rounded up by `+ PAGE_SIZE - 1` would wrap to a few bytes, which
 * fail with the region's next page unmoved; a region that wraps, which
 * fails; a window below the top given its two tables; a window that runs
 * past the top of the address space, which fails and makes no table; a
 * window that ends exactly at the top given its table; and a window at
 * address 0, given its table.
 *
 * Five rows hold the temporary mappings to their contract on i686: a
 * released slot stays mapped and no caller is given it again until a sweep,
 * which comes only when no slot is free and unmaps every released slot with
 * one change of kernel mappings, so the 960 mappings of the pools make one
 * change; a held slot survives the sweeps; and a mapping fails when every
 * slot is held.
 *
 * Seven more split the slots between processors, as Linux gives each its
 * own kmap_local slots, with `host_cpu` naming the processor that runs: two
 * processors mapping at once take the first slot of each one's own pool and
 * each drops its own translation of it; a slot released on another
 * processor goes back to its owner's pool; a processor that has spent its
 * pool sweeps that pool alone; one whose pool is all held borrows another's
 * free slot, which the owner is then not given; one processor may still hold
 * every slot; and three and 64 processors split the slots in whole words.
 * Every pool's lock is free after each call, and the host's spinlocks trap
 * on a lock taken twice. A processor's own slot past the pools
 * (`kmap_cpu`) maps a frame with interrupts off while every slot of the
 * pools is held, drops its translation on that processor alone, and goes
 * with interrupts restored, touching no pool's bits.
 *
 * Two rows drive the page fault handler with a supervisor write fault: at an
 * address in the guard page below the running thread's kernel stack, which
 * `kstack_overrun_pid` answers with the thread's pid, it must panic as that
 * stack's overrun, and at the stack's lowest byte, above the guard, with the
 * handler's general panic. Each runs in a forked child, whose exit status
 * says which panic came. Two more drive it with faults it recovers from, a
 * page-cache window fault that the fill serves and a user write that
 * becomes SIGSEGV, and check that neither notes a fault for a panic's
 * report, which a later plain panic would show.
 *
 * Four rows give `cow_fault` the entry a second thread's write fault finds
 * when another fault changed it after this one found it present and
 * read-only, as the faults of one address space may at once: made writable
 * by the other's copy, made absent, or its table gone, each of which must
 * run the resolution again and not end in SIGSEGV; and a present read-only
 * page that is not copy-on-write, which stays refused.
 *
 * Three rows run a write-back's walk (`paging_wrprotect_where`) between a
 * shared store's mark and its entry, through the real walk and both fault
 * paths: a fault of an absent page installs nothing (`install_entry`), and
 * one of a read-only page leaves it read-only (`shared_mkwrite`), each
 * because the walk moved the space's count of zaps under the page-table
 * lock; and a walk over two regions of a space takes write access from
 * both with one flush of the span it changed, as paging.h says.
 *
 * Returns 0 when every row passes, 1 otherwise.
 */

#include "mm/paging.c"
#include "arch/i686/mmu.c"

extern int printf(const char *format, ...);
extern void *calloc(unsigned long count, unsigned long size);
extern void free(void *p);
extern void exit(int status);
extern int fork(void);
extern int waitpid(int pid, int *status, int options);
extern void _exit(int status);

static int fails;

/** Count a failure and print `row` when `ok` is 0. */
static void check(int ok, const char *row)
{
    if (!ok) {
        printf("paging-hosttest: fail: %s\n", row);
        fails++;
    }
}

/* ---- what paging.c reaches outside itself ---- */

struct percpu cpu_table[NR_CPUS];
unsigned int host_cpu; /* the processor that runs, for `arch_cpu_id` */
uintptr_t mem_reserved_ptr;
volatile uint32_t init_pid = 1; /* no row faults as init */
bool debug_shell_is_home;

static int tables_given;   /* page tables the heap gave since a row began */
static int boot_carves;    /* calls to the boot allocator, which must be none */
static int refusals_noted; /* calls to klog, one for each range that failed */
static unsigned give_ways; /* calls to sched_give_way since a row began */

/** A zeroed page from the host heap, counted as a page table; 0 for more
 * than one page. `*phys` is set to 0. */
void *kalloc_pages(uint32_t count, uintptr_t *phys)
{
    if (phys) {
        *phys = 0;
    }
    if (count != 1) {
        return 0;
    }
    tables_given++;
    return calloc(1, PAGE_SIZE);
}

/** Free a page `kalloc_pages` gave. */
void kfree_pages(void *p, uint32_t count)
{
    (void)count;
    free(p);
}

/** The boot allocator: counts the call and gives 0. */
uintptr_t mem_reserve(size_t size, bool align)
{
    (void)size;
    (void)align;
    boot_carves++;
    return 0;
}

/** The boot allocator: counts the call and gives 0. */
void *mem_reserve_ptr(size_t size, bool align)
{
    (void)size;
    (void)align;
    boot_carves++;
    return 0;
}

/** The boot allocator's move past the map's holes: counts the call. */
void mem_reserve_usable(const struct frame_map *map, size_t size)
{
    (void)map;
    (void)size;
    boot_carves++;
}

/** Count the record and print nothing. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
    refusals_noted++;
}

/* Set in a forked child before a fault that must panic: k_panic exits 42
 * when its text contains this and 43 when it does not. */
static const char *expect_panic;

/** Whether the string `hay` contains `needle`. */
static int contains(const char *hay, const char *needle)
{
    const char *h;

    for (h = hay; *h; h++) {
        const char *a = h;
        const char *b = needle;

        while (*a && *b && *a == *b) {
            a++;
            b++;
        }
        if (!*b) {
            return 1;
        }
    }
    return 0;
}

/** With `expect_panic` set, exit 42 when `str` contains it and 43 when it
 * does not; otherwise print `str` and exit 1. */
void k_panic(const char *str)
{
    if (expect_panic) {
        _exit(contains(str, expect_panic) ? 42 : 43);
    }
    printf("paging-hosttest: panic: %s\n", str);
    exit(1);
}

static int notes; /* faults noted for a panic's report since a row began */

/** Count the note: the panic's report shows the last one, and panic.c
 * never clears it, so a note on a path that returns is shown under any
 * later panic of the processor. */
void k_panic_note_fault(const char *name, uintptr_t ip, long error)
{
    (void)name;
    (void)ip;
    (void)error;
    notes++;
}

/** Nothing: the address belongs to the note `k_panic_note_fault` counts. */
void k_panic_note_address(uintptr_t address)
{
    (void)address;
}

/** No panic reads a process here. */
int k_panic_reading(void)
{
    return 0;
}

/** Nothing: the host has no task state segment. */
void tss_set_double_fault_dir(uint32_t dir)
{
    (void)dir;
}

static int segvs; /* SIGSEGVs the fault handler forced since a row began */

/** Count a SIGSEGV; the process is not touched. */
void signal_force(process_t *target, int signo, const struct k_siginfo *info)
{
    (void)target;
    (void)info;
    segvs += signo == SIGSEGV;
}

static uint32_t window_bytes; /* the page-cache window's size, 0 for none */
static int window_fills;      /* what `pc_fault` returns */

/** `window_bytes`. */
uint32_t pc_window_size(void)
{
    return window_bytes;
}

/** `window_fills`: 1 when a row has the page cache serve the fault. */
int pc_fault(uintptr_t fault_address, int write)
{
    (void)fault_address;
    (void)write;
    return window_fills;
}

/** Nothing: no row maps a file. */
void vm_file_get(struct vm_file *f)
{
    (void)f;
}

/** Nothing: no row maps a file. */
void vm_file_put(struct vm_file *f)
{
    (void)f;
}

/** NULL: no row maps a device object. */
void *vm_device_object(const struct vm_file *f)
{
    (void)f;
    return 0;
}

/** 0: no row maps a device object. */
int vm_device_whole(const struct vm_file *f)
{
    (void)f;
    return 0;
}

/* The processes whose address spaces a walk changes: none, but for the
 * fault-race rows' one. */
process_t *proc_list;

/** The write-combining bits, which no row maps. */
pte_t arch_pte_wc(void)
{
    return PTE_WC;
}

/* The guard page below the running thread's kernel stack, and the thread's
 * pid, as the process layer would answer for them. */
#define GUARD_PAGE 0xC0104000u
#define GUARD_PID  7

/** `GUARD_PID` for an address in `GUARD_PAGE`, -1 for any other. */
int kstack_overrun_pid(uintptr_t addr)
{
    return addr - GUARD_PAGE < PAGE_SIZE ? GUARD_PID : -1;
}

/** 0: the command line has no flags. */
int kcmdline_has(const char *flag)
{
    (void)flag;
    return 0;
}

/** `most`, the largest heap. */
size_t kheap_size_for(uint64_t managed, uint64_t most)
{
    (void)managed;
    return (size_t)most;
}

/** Nothing: the heap is the host's. */
void kheap_init(size_t bytes)
{
    (void)bytes;
}

/** Nothing: the rows call the handler directly. */
void register_isr_handler(uint8_t vector, isr_handler_t handler, const char *name)
{
    (void)vector;
    (void)handler;
    (void)name;
}

/** Nothing: the model has no big lock to leave out of an entry. */
void exception_unlock(uint8_t vector)
{
    (void)vector;
}

/** Nothing: the model's address spaces live as long as the test. */
void mm_get(struct mm *mm)
{
    (void)mm;
}

/** Nothing, as `mm_get`. */
void mm_put(struct mm *mm)
{
    (void)mm;
}

/** Release the files whose last region went, as mm.c's does with no lock
 * held. */
void mm_reap(void)
{
    vm_file_reap();
}

/** 0: memory is never short. */
int mem_short(void)
{
    return 0;
}

/** Nothing: a user fault here is signalled through `signal_force`. */
void terminate_current_process(int32_t signo)
{
    (void)signo;
}

/* The one region of the fault-race rows, in the space whose regions are
 * `race_space`, or NULL for none. */
static vma_t *race_vma;
static vm_space_t *race_space;

/** `race_vma` for an address within it in `race_space`; 0 for any other
 * address, which no region holds. */
vma_t *vm_find(vm_space_t *vm, uintptr_t addr)
{
    if (race_vma && vm == race_space && addr >= race_vma->start && addr < race_vma->end) {
        return race_vma;
    }
    return 0;
}

/** 0: no page of the rows is a guard page. */
int vm_guarded(const vm_space_t *vm, uintptr_t addr)
{
    (void)vm;
    (void)addr;
    return 0;
}

/** 0: no stack grows. */
int vm_grow_stack(vm_space_t *vm, uintptr_t page, uintptr_t as_limit)
{
    (void)vm;
    (void)page;
    (void)as_limit;
    return 0;
}

/** Nothing: no file waits to be released. */
void vm_file_reap(void)
{}

/** Nothing: memory is never short. */
void mem_balance(void)
{}

/** 1: memory is never short. */
int mem_may_take(int user_page)
{
    (void)user_page;
    return 1;
}

/* The fault address the handler reads, which the fault rows set. */
static uint32_t fault_cr2;

/** The fault address a fault row set, as CR2 would have it. */
uintptr_t arch_fault_address(void)
{
    return fault_cr2;
}

/** 0: no fault here is a kernel copy's, so none has a fixup. */
int arch_user_copy_fixup(isr_params_t *p)
{
    (void)p;
    return 0;
}

/** 0: the host has no translation root. */
uintptr_t arch_space_root(void)
{
    return 0;
}

/* The reloads of the translation root and the changes of kernel mappings,
 * which the temporary mappings' rows count. */
static int space_loads;
static int kernel_changes;

/** Count a reload of the translation root. */
void arch_space_load(uintptr_t root)
{
    (void)root;
    space_loads++;
}

/* The flushes of user translations, one each, and the span of the last. */
static int shootdowns;
static uintptr_t shot_start, shot_end;

/** One processor: no other has a translation to drop. Counted. */
void smp_tlb_shootdown(const struct page_dir *dir, uintptr_t start, uintptr_t end)
{
    (void)dir;
    shootdowns++;
    shot_start = start;
    shot_end = end;
}

/** Count a change of kernel mappings. */
void smp_kernel_mapping_changed(void)
{
    kernel_changes++;
}

/** Count a change of kernel mappings, as `smp_kernel_mapping_changed`. */
void smp_kernel_flush(uintptr_t start, uintptr_t end)
{
    (void)start;
    (void)end;
    kernel_changes++;
}

/** 1: the test runs as the kernel, under the big lock. */
int bkl_held(void)
{
    return 1;
}

/** Nothing: there is no big lock. */
void bkl_enter(void)
{}

/** Nothing: there is no big lock. */
void bkl_exit(void)
{}

/** Nothing: the test has no interrupt flag. */
void arch_irq_enable(void)
{}

/** Nothing: the test has no interrupt flag. */
void arch_irq_disable(void)
{}

/** Nothing: no tick asks for a switch. */
void sched_take_preempt(void)
{}

/** Nothing: the test has no address space that counts its resident pages. */
void mm_resident_add(const struct page_dir *dir, long pages)
{
    (void)dir;
    (void)pages;
}

/* The drops of one page's translation, and the last page dropped. */
static int page_drops;
static uintptr_t page_dropped;

/** Count the drop of `va`'s translation. */
void arch_tlb_flush_page(uintptr_t va)
{
    page_drops++;
    page_dropped = va;
}

/* Whether the model's interrupts are off, which `kirq_save` turns off. */
static uint32_t irqs_off;

/** Turn the model's interrupts off; whether they were. */
uint32_t kirq_save(void)
{
    uint32_t was = irqs_off;

    irqs_off = 1;
    return was;
}

/** Put the model's interrupts back as `flags`, from `kirq_save`, says. */
void kirq_restore(uint32_t flags)
{
    irqs_off = flags;
}

/* ---- the rows ---- */

static page_dir_t test_dir;

/* The table index of a kernel virtual address. */
#define TABLE_OF(va) ((uint32_t)(va) / PAGE_SIZE / 1024u)

/** Drop the table at `index`, if the directory has one, so that a row can
 * ask whether a window made it again. */
static void drop_table(uint32_t index)
{
    free(test_dir.logical_page_tables[index]);
    test_dir.logical_page_tables[index] = 0;
    test_dir.phys_page_tables[index] = 0;
}

/**
 * The MMIO region: a page mapped at its start, and three regions refused
 * with the next free page where it was. The region's tables exist, as the
 * kernel's gigabyte has all of its tables from boot.
 */
static void mmio_rows(void)
{
    uint32_t t;
    uintptr_t va, next;

    for (t = TABLE_OF(KVA_MMIO); t < TABLE_OF(KVA_MMIO + KVA_MMIO_SIZE); t++) {
        test_dir.logical_page_tables[t] = calloc(1, PAGE_SIZE);
    }

    va = map_mmio(0xFEC00000u, 0x400u);
    check(va == KVA_MMIO &&
              test_dir.logical_page_tables[TABLE_OF(KVA_MMIO)]->pages[0] == (0xFEC00000u | 0x1Bu),
          "a register page is mapped at the MMIO region's start, uncached");

    next = KVA_MMIO + PAGE_SIZE;
    refusals_noted = 0;
    va = map_mmio(0x0u, 0xFFFFF001u);
    check(va == 0 && refusals_noted == 1,
          "a span from the first page into the last, whose rounding would wrap, is refused");
    va = map_mmio(0x800u, 0xFFFFF7FFu);
    check(va == 0 && refusals_noted == 2,
          "a span ending on the last byte of the address space is refused");
    va = map_mmio(0xFFFFF000u, 0x2000u);
    check(va == 0 && refusals_noted == 3, "a region that wraps is refused");
    check(test_dir.logical_page_tables[TABLE_OF(KVA_MMIO)]->pages[1] == 0,
          "a refused region maps nothing");

    va = map_mmio(0xFED00010u, 0x20u);
    check(va == next + 0x10u,
          "the next region takes the page after the first, at its offset in the page");
    mmio_unmap(KVA_MMIO, 0x400u);
    check(test_dir.logical_page_tables[TABLE_OF(KVA_MMIO)]->pages[0] == 0 &&
              map_mmio(0xFEE00000u, 0x10u) == KVA_MMIO,
          "an unmapped page is cleared, and the next region takes it again");
}

/**
 * The windows: tables made below the top, none for a window past it, one
 * for a window that ends at the top and one for a window at address 0.
 */
static void window_rows(void)
{
    uint32_t t;

    for (t = 0; t < 1024; t++) {
        drop_table(t);
    }

    tables_given = 0;
    check(premap_kernel_window(0xC0000000u, 0x800000u) == 0 && tables_given == 2 &&
              test_dir.logical_page_tables[768] != 0 && test_dir.logical_page_tables[769] != 0 &&
              (test_dir.phys_page_tables[768] & 0x3u) == 0x3u,
          "a window below the top of the address space gets its two tables");

    tables_given = 0;
    check(premap_kernel_window(0xC0000000u, 0x800000u) == 0 && tables_given == 0,
          "a window whose tables exist takes no table");

    tables_given = 0;
    check(premap_kernel_window(0xFFC00000u, 0x800000u) == -1 && tables_given == 0 &&
              test_dir.logical_page_tables[1023] == 0 && test_dir.logical_page_tables[0] == 0,
          "a window that runs past the top of the address space is refused and makes no table");

    tables_given = 0;
    check(premap_kernel_window(0xFFC00000u, 0x400000u) == 0 && tables_given == 1 &&
              test_dir.logical_page_tables[1023] != 0 && test_dir.logical_page_tables[0] == 0,
          "a window that ends exactly at the top of the address space gets its table");

    tables_given = 0;
    check(premap_kernel_window(0x0u, 0x400000u) == 0 && tables_given == 1 &&
              test_dir.logical_page_tables[0] != 0,
          "a window that starts at address zero gets its table");

    for (t = 0; t < 1024; t++) {
        drop_table(t);
    }
}

/* The entry of temporary slot `slot`. */
#define TEMP_PTE(slot) (test_dir.logical_page_tables[TABLE_OF(KVA_TEMP)]->pages[slot])

static int temp_unlocked(void);

/**
 * A processor's own slot (`kmap_cpu`), called while every slot of the pools
 * is held: the slot past the pools of the processor's number, mapped at the
 * frame's offset with interrupts off and its translation dropped on that
 * processor alone, and unmapped with interrupts as they were and the
 * translation dropped again; no pool's bits change.
 */
static void temp_own_rows(void)
{
    uint32_t flags, slots0 = _temp_slots[0], stale0 = _temp_stale[0];
    uint8_t *va;

    host_cpu = 5;
    page_drops = 0;
    irqs_off = 0;
    va = kmap_cpu(0x5000123u, &flags);
    check(va == (uint8_t *)KVA_TEMP + (TEMP_POOL_SLOTS + 5u) * PAGE_SIZE + 0x123 &&
              TEMP_PTE(TEMP_POOL_SLOTS + 5u) == (0x5000000u | 0x3u) && irqs_off &&
              page_drops == 1 && page_dropped == ((uintptr_t)va & ~(uintptr_t)0xFFFu),
          "a processor's own slot maps a frame with every pool slot held, interrupts off");
    kunmap_cpu(va, flags);
    check(TEMP_PTE(TEMP_POOL_SLOTS + 5u) == 0 && !irqs_off && page_drops == 2 &&
              _temp_slots[0] == slots0 && _temp_stale[0] == stale0 && temp_unlocked(),
          "and goes with interrupts as they were, touching no pool");
    host_cpu = 0;
}

/**
 * The temporary mappings: a released slot stays mapped and is not given
 * again before a sweep, which comes only when no slot is free and unmaps
 * every released slot with one change of kernel mappings; a held slot
 * survives the sweeps; and with every slot held a mapping is refused.
 */
static void temp_rows(void)
{
    static uint8_t *held[TEMP_POOL_SLOTS];
    uint8_t *va;
    uint32_t i, mapped;
    int wrong = 0;

    test_dir.logical_page_tables[TABLE_OF(KVA_TEMP)] = calloc(1, PAGE_SIZE);
    space_loads = 0;
    kernel_changes = 0;

    va = kmap_temp(0x5000123u);
    check(va == (uint8_t *)KVA_TEMP + 0x123 && TEMP_PTE(0) == (0x5000000u | 0x3u),
          "a frame is mapped at the first temporary slot, at its offset in the page");
    kunmap_temp(va);
    check(TEMP_PTE(0) == (0x5000000u | 0x3u) && kernel_changes == 0,
          "a released temporary slot stays mapped and changes no kernel mapping");

    for (i = 1; i < TEMP_POOL_SLOTS; i++) {
        va = kmap_temp(0x6000000u + i * PAGE_SIZE);
        wrong |= va != (uint8_t *)KVA_TEMP + i * PAGE_SIZE;
        kunmap_temp(va);
    }
    check(!wrong && kernel_changes == 0 && space_loads == 0,
          "960 temporary mappings take the pools' 960 slots in turn with no change of kernel "
          "mappings");

    held[0] = kmap_temp(0x7000000u);
    for (i = 1, mapped = 0; i < TEMP_POOL_SLOTS; i++) {
        mapped += TEMP_PTE(i) != 0;
    }
    check(held[0] == (uint8_t *)KVA_TEMP && mapped == 0 && kernel_changes == 1 && space_loads == 1,
          "with no slot free, a mapping sweeps every released slot with one change of kernel "
          "mappings and takes the first");

    kernel_changes = 0;
    for (i = 0; i < 2 * TEMP_POOL_SLOTS; i++) {
        va = kmap_temp(0x8000000u);
        wrong |= va == held[0];
        kunmap_temp(va);
    }
    check(!wrong && TEMP_PTE(0) == (0x7000000u | 0x3u) && kernel_changes == 2,
          "a held temporary slot is not given again and survives two sweeps");

    for (i = 1; i < TEMP_POOL_SLOTS; i++) {
        held[i] = kmap_temp(0x9000000u);
        wrong |= held[i] == 0;
    }
    check(!wrong && kmap_temp(0xA000000u) == 0,
          "with every temporary slot held, a mapping is refused");
    temp_own_rows();
    for (i = 0; i < TEMP_POOL_SLOTS; i++) {
        kunmap_temp(held[i]);
    }
}

/** Unmap every temporary slot and mark each free. */
static void temp_clear(void)
{
    uint32_t i;

    for (i = 0; i < KVA_TEMP_SLOTS; i++) {
        TEMP_PTE(i) = 0;
    }
    k_memset(_temp_slots, 0, sizeof _temp_slots);
    k_memset(_temp_stale, 0, sizeof _temp_stale);
}

/** Whether no pool's lock is held. */
static int temp_unlocked(void)
{
    uint32_t p;

    for (p = 0; p < TEMP_POOLS_MAX; p++) {
        if (_temp_pools[p].lock.locked) {
            return 0;
        }
    }
    return 1;
}

/** The kernel address of temporary slot `slot`. */
#define TEMP_VA(slot) ((uint8_t *)KVA_TEMP + (slot) * PAGE_SIZE)

/* The slots of the two halves of the pools, for the rows of two pools. */
#define TEMP_HALF (TEMP_POOL_SLOTS / 2)

/**
 * Two processors' pools: each maps from its own, drops its own translation
 * of the slot, and a release on the other processor goes to the owner's
 * pool, still mapped.
 */
static void temp_two_rows(void)
{
    uint8_t *a, *b;

    temp_clear();
    paging_temp_split(2);
    check(_temp_pool_count == 2 && _temp_pools[0].first == 0 && _temp_pools[0].end == TEMP_HALF &&
              _temp_pools[1].first == TEMP_HALF && _temp_pools[1].end == TEMP_POOL_SLOTS,
          "two processors' pools are the two halves of the slots");

    page_drops = 0;
    host_cpu = 0;
    a = kmap_temp(0x5000000u);
    check(page_drops == 1 && page_dropped == (uintptr_t)a,
          "a processor drops its own translation of the slot it maps");
    host_cpu = 1;
    b = kmap_temp(0x5001000u);
    check(a == TEMP_VA(0) && b == TEMP_VA(TEMP_HALF) && TEMP_PTE(0) == (0x5000000u | 0x3u) &&
              TEMP_PTE(TEMP_HALF) == (0x5001000u | 0x3u) && page_drops == 2 &&
              page_dropped == (uintptr_t)b && temp_unlocked(),
          "two processors mapping at once take the first slot of each one's own pool");

    kunmap_temp(a); /* on processor 1, the slot of processor 0 */
    host_cpu = 0;
    kunmap_temp(b);
    check((_temp_stale[0] & 1u) && (_temp_stale[TEMP_HALF / 32] & 1u) && !(_temp_slots[0] & 1u) &&
              !(_temp_slots[TEMP_HALF / 32] & 1u) && TEMP_PTE(0) != 0 && temp_unlocked(),
          "a slot released on another processor goes back to its owner's pool, still mapped");
}

/**
 * Processor 1, after `temp_two_rows`, spends its pool, which it sweeps
 * alone, leaving processor 0's released slot mapped.
 */
static void temp_sweep_rows(void)
{
    uint8_t *va;
    uint32_t i, mapped;
    int wrong = 0;

    host_cpu = 1;
    kernel_changes = 0;
    space_loads = 0;
    for (i = 1; i < TEMP_HALF; i++) {
        va = kmap_temp(0x6000000u);
        wrong |= va != TEMP_VA(TEMP_HALF + i);
        kunmap_temp(va);
    }
    va = kmap_temp(0x7000000u);
    for (i = TEMP_HALF, mapped = 0; i < TEMP_POOL_SLOTS; i++) {
        mapped += TEMP_PTE(i) != 0;
    }
    check(!wrong && va == TEMP_VA(TEMP_HALF) && mapped == 1 && kernel_changes == 1 &&
              space_loads == 1 && TEMP_PTE(0) == (0x5000000u | 0x3u) && temp_unlocked(),
          "a processor that has spent its pool sweeps that pool alone, with one change of kernel "
          "mappings, and the other pool's released slot stays mapped");
    kunmap_temp(va);
    host_cpu = 0;
}

/**
 * Processor 1, after `temp_sweep_rows`, holds its whole pool and borrows
 * from processor 0's, which is then not given the borrowed slot.
 */
static void temp_borrow_rows(void)
{
    static uint8_t *held[TEMP_HALF];
    uint8_t *a, *va;
    uint32_t i;
    int wrong = 0;

    host_cpu = 1;
    for (i = 0; i < TEMP_HALF; i++) {
        held[i] = kmap_temp(0x8000000u);
        wrong |= held[i] < TEMP_VA(TEMP_HALF);
    }
    page_drops = 0;
    va = kmap_temp(0x9000000u);
    host_cpu = 0;
    a = kmap_temp(0xA000000u);
    check(!wrong && va == TEMP_VA(1) && TEMP_PTE(1) == (0x9000000u | 0x3u) && a == TEMP_VA(2) &&
              page_drops == 2 && temp_unlocked(),
          "a processor whose pool is all held borrows a free slot of another's, which the owner "
          "is then not given");
    kunmap_temp(a);
    kunmap_temp(va);
    for (i = 0; i < TEMP_HALF; i++) {
        kunmap_temp(held[i]);
    }
}

/** One processor holds every slot of both pools, and is refused one more. */
static void temp_all_rows(void)
{
    static uint8_t *held[TEMP_POOL_SLOTS];
    uint32_t i;
    int wrong = 0, unlocked = 1;

    host_cpu = 0;
    for (i = 0; i < TEMP_POOL_SLOTS; i++) {
        held[i] = kmap_temp(0xB000000u);
        wrong |= held[i] == 0;
        unlocked &= temp_unlocked();
    }
    for (i = 0; i < TEMP_POOL_SLOTS / 32; i++) {
        wrong |= _temp_slots[i] != 0xFFFFFFFFu;
    }
    check(!wrong && unlocked && kmap_temp(0xC000000u) == 0 && temp_unlocked(),
          "one processor may hold every slot of both pools, and a mapping past them is refused");
    for (i = 0; i < TEMP_POOL_SLOTS; i++) {
        kunmap_temp(held[i]);
    }
}

/** The split for three processors and for 64, in whole words. */
static void temp_split_rows(void)
{
    uint8_t *va;

    paging_temp_split(3);
    check(_temp_pool_count == 3 && _temp_pool_size == 320 && _temp_pools[1].first == 320 &&
              _temp_pools[1].end == 640 && _temp_pools[2].first == 640 &&
              _temp_pools[2].end == TEMP_POOL_SLOTS &&
              temp_pool_of(TEMP_POOL_SLOTS - 1u) == &_temp_pools[2],
          "three processors' pools are whole words, and the last has the rest");

    temp_clear();
    paging_temp_split(64);
    host_cpu = 33;
    va = kmap_temp(0xD000000u);
    check(_temp_pool_count == 30 && _temp_pool_size == 32 && va == TEMP_VA(96) &&
              temp_pool_of(96) == &_temp_pools[3],
          "64 processors share 30 pools of a word each, processor 33 taking pool 3's slots");
    kunmap_temp(va);
    host_cpu = 0;
}

/** The slots split between processors, in order, each row from the state
 * the one before left. */
static void temp_cpu_rows(void)
{
    temp_two_rows();
    temp_sweep_rows();
    temp_borrow_rows();
    temp_all_rows();
    temp_split_rows();
}

/**
 * Drive the page fault handler in a forked child with a supervisor write
 * fault at `addr`, which must panic with a text containing `text`. `what` is
 * the row's text.
 */
static void fault_row(uint32_t addr, const char *text, const char *what)
{
    isr_params_t params;
    int pid = fork();
    int status = 0;

    if (pid == 0) {
        k_memset(&params, 0, sizeof(params));
        params.vector = 14;
        params.error_code = 0x2; /* a write, from supervisor mode */
        params.cs = 0x08;
        fault_cr2 = addr;
        expect_panic = text;
        _page_fault_handler(&params);
        _exit(44);
    }
    check(pid > 0 && waitpid(pid, &status, 0) == pid, "the fault's child ran");
    check((status & 0x7f) == 0 && ((status >> 8) & 0xff) == 42, what);
}

/**
 * A supervisor fault in the guard page below the running thread's kernel
 * stack panics as that stack's overrun; one at the stack's lowest byte,
 * just above the guard, panics as any other kernel fault does.
 */
static void fault_rows(void)
{
    fault_row(GUARD_PAGE + PAGE_SIZE - 4, "a kernel stack ran out",
              "a supervisor fault in the guard page below the running thread's kernel stack "
              "panics as that stack's overrun");
    fault_row(GUARD_PAGE + PAGE_SIZE, "page fault while in kernel mode",
              "a supervisor fault at the stack's lowest byte panics as any kernel fault");
}

/**
 * Faults the handler recovers from note nothing for a panic's report, so
 * that a plain `k_panic` after them reports no exception: a supervisor
 * fault in the page-cache window that the fill serves, and a user write
 * that no region holds, which becomes SIGSEGV. The note was once written
 * before both.
 */
static void recovered_rows(void)
{
    static process_t proc;
    static struct mm mm;
    static struct k_sigaction acts[KSIG_MAX + 1];
    process_t *was = cpu_table[0].current;
    isr_params_t params;

    cpu_table[0].current = &proc;
    k_memset(&params, 0, sizeof(params));
    params.vector = 14;
    params.error_code = 0x2; /* a write, from supervisor mode */
    params.cs = 0x08;
    fault_cr2 = PCWIN_BASE;
    window_bytes = PAGE_SIZE;
    window_fills = 1;
    notes = 0;
    _page_fault_handler(&params);
    check(notes == 0, "a page-cache window fault that the fill serves notes no fault, so a later "
                      "panic reports no exception");
    window_bytes = 0;
    window_fills = 0;

    proc.mm = &mm;
    proc.sigacts = acts;
    params.error_code = 0x6; /* a write, from user mode */
    params.cs = 0x1B;
    fault_cr2 = USER_ADDR_MIN + 0x1000u;
    notes = 0;
    segvs = 0;
    _page_fault_handler(&params);
    check(segvs == 1 && notes == 0, "a user fault that becomes SIGSEGV notes no fault, so a later "
                                    "panic reports no exception");
    proc.mm = 0;
    proc.sigacts = 0;
    cpu_table[0].current = was;
}

/**
 * What `cow_fault` answers for the entry it finds when another fault of the
 * same address space changed it after the caller's look, which found the
 * page present, read-only and copy-on-write: -1, to resolve again, for a
 * page made writable, one made absent and one whose table is gone, as
 * Linux's handle_pte_fault leaves an entry that is not the one it read; 0
 * for a present read-only page that is not copy-on-write. Each answer comes
 * before the frame is looked at, so the entries name frame 1.
 */
static void cow_race_rows(void)
{
    static struct mm mm;
    page_dir_t *dir = calloc(1, sizeof(*dir));
    page_table_t *table = calloc(1, sizeof(*table));
    uintptr_t addr = USER_ADDR_MIN + 0x3000u;
    uint32_t index = (uint32_t)addr / PAGE_SIZE / 1024u;
    pte_t *e = &table->pages[MMU_INDEX(addr)];

    dir->logical_page_tables[index] = table;
    mm.dir = dir;
    *e = (pte_t)PAGE_SIZE | PTE_P | PTE_U | PTE_W;
    check(cow_fault(&mm, addr + 0x10u) == -1,
          "a copy-on-write page another thread's fault made writable after this one found it "
          "read-only is resolved again, not refused");
    *e = 0;
    check(cow_fault(&mm, addr) == -1,
          "a copy-on-write page made absent after the look is resolved again, not refused");
    *e = (pte_t)PAGE_SIZE | PTE_P | PTE_U;
    check(cow_fault(&mm, addr) == 0,
          "a present read-only page that is not copy-on-write is refused");
    dir->logical_page_tables[index] = 0;
    check(cow_fault(&mm, addr) == -1,
          "a page whose table went after the look is resolved again, not refused");
    free(table);
    free(dir);
}

/* ---- a write-back's walk between a shared fault's mark and its entry ---- */

/* The page-table lock's hook, from mmap_lock_host.c. */
extern void (*host_ptl_hook)(struct mm *mm);

/* The file of the fault-race rows, the frame its page has, and the marks
 * its `mkwrite` made. */
static struct vm_file race_file;
#define RACE_FRAME 0x7000000u
static int race_marks;

/** The page of `race_file`: `RACE_FRAME`, in memory, so 0. */
static int race_share(struct vm_file *f, uint32_t pgoff, uintptr_t *phys)
{
    (void)f;
    (void)pgoff;
    *phys = RACE_FRAME;
    return 0;
}

/** The `match` of the walk: the whole of a region that maps `race_file`. */
static int race_match(const struct vma *r, void *arg, uintptr_t *lo, uintptr_t *hi)
{
    (void)arg;
    if (r->file != &race_file) {
        return 0;
    }
    *lo = r->start;
    *hi = r->end;
    return 1;
}

/** A write-back's walk of every space, as filemap.c's pass makes it after
 * the mark, run from the page-table lock's hook. */
static void race_walk(struct mm *mm)
{
    (void)mm;
    paging_wrprotect_where(race_match, 0);
}

/** Count the mark and have the walk run before the fault's next take of
 * the page-table lock, which comes after the fault's look at its region
 * (`region_same`) and is the lock its recheck is made under. */
static int race_mkwrite(struct vm_file *f, uint32_t pgoff, uintptr_t phys)
{
    (void)f;
    (void)pgoff;
    (void)phys;
    race_marks++;
    host_ptl_hook = race_walk;
    return 0;
}

/**
 * A write-back's walk (`paging_wrprotect_where`) that runs after a shared
 * store's fault had its file's `mkwrite` mark the page, and after the fault
 * found its region unchanged, but before the fault takes the page-table
 * lock for its entry: the walk moves the space's count of zaps and the
 * fault makes no entry writable, as the mark it made is the one the
 * write-back cleared. A write fault of an absent page installs nothing
 * (`install_entry`), and one of a present read-only page leaves it
 * read-only (`shared_mkwrite`); each answers -1, so the store faults and
 * marks again. Without the recheck under the lock, either would leave a
 * writable entry whose stores no mark records. The real walk, the real
 * fault paths and one address space on the process list.
 */
static void fault_race_rows(void)
{
    static process_t proc;
    static struct mm mm;
    static vma_t vma, other;
    page_dir_t *dir = calloc(1, sizeof(*dir));
    page_table_t *table = calloc(1, sizeof(*table));
    uintptr_t addr = USER_ADDR_MIN + 0x400000u;
    pte_t *e = &table->pages[MMU_INDEX(addr)];
    pte_t *o = &table->pages[MMU_INDEX(addr + 8u * PAGE_SIZE)];
    int rc;

    dir->logical_page_tables[(uint32_t)addr / PAGE_SIZE / 1024u] = table;
    mm.dir = dir;
    vma.start = addr;
    vma.end = addr + 4u * PAGE_SIZE;
    vma.prot = VM_PROT_READ | VM_PROT_WRITE;
    vma.type = VM_SHARED;
    vma.file = &race_file;
    other = vma;
    other.start = addr + 8u * PAGE_SIZE;
    other.end = other.start + PAGE_SIZE;
    other.pgoff = 8;
    vma.next = &other;
    mm.vm.first = &vma;
    mm.vm.last = &other;
    race_file.share = race_share;
    race_file.mkwrite = race_mkwrite;
    race_vma = &vma;
    race_space = &mm.vm;
    proc.mm = &mm;
    proc_list = &proc;

    mmap_read_lock(&mm);
    race_marks = 0;
    rc = shared_fault(&mm, &vma, addr, 1);
    check(rc == -1 && race_marks == 1 && *e == 0 && mm.zaps == 1 && !host_ptl_hook,
          "a store's fault of an absent page that a write-back's walk passes between its mark "
          "and its entry installs nothing and resolves again");
    *e = (pte_t)RACE_FRAME | PTE_P | PTE_U;
    rc = shared_mkwrite(&mm, &vma, addr);
    check(rc == -1 && race_marks == 2 && *e == ((pte_t)RACE_FRAME | PTE_P | PTE_U) &&
              mm.zaps == 2 && !host_ptl_hook,
          "a store's fault of a read-only page that a write-back's walk passes between its "
          "mark and its entry leaves it read-only and resolves again");
    *e = (pte_t)RACE_FRAME | PTE_P | PTE_U | PTE_W;
    *o = (pte_t)RACE_FRAME | PTE_P | PTE_U | PTE_W;
    shootdowns = 0;
    paging_wrprotect_where(race_match, 0);
    check(*e == ((pte_t)RACE_FRAME | PTE_P | PTE_U) && *o == ((pte_t)RACE_FRAME | PTE_P | PTE_U) &&
              mm.zaps == 3 && mm.changing == 0 && mm.changes == 3,
          "the walk takes write access from both regions' entries, the space under way once");
    check(shootdowns == 1 && shot_start == addr && shot_end == addr + 9u * PAGE_SIZE,
          "with one flush for the space, over the span of the pages it changed");
    mmap_unlock(&mm);

    proc_list = 0;
    race_vma = 0;
    race_space = 0;
    free(table);
    free(dir);
}

/** The protection keys are off in the host test, as on a processor without
 * them. */
int fpu_pkeys(void)
{
    return 0;
}

/** PKRU with the keys off. */
uint32_t fpu_pkru(void)
{
    return 0;
}

/** Count a give-way of a walk; one thread has no one to give way to. */
void sched_give_way(void)
{
    give_ways++;
}

/**
 * The give-ways of the walks that free an address space of 1,000 page
 * tables: `free_user_pages` gives way after each table, and
 * `free_page_directory` after each 512 tables it frees, once for the
 * 1,000.
 */
static void give_way_rows(void)
{
    page_dir_t *dir = calloc(1, sizeof(*dir));
    uint32_t i;

    for (i = 0; i < 1000u; i++) {
        dir->logical_page_tables[i] = calloc(1, PAGE_SIZE);
    }
    give_ways = 0;
    free_user_pages(dir);
    check(give_ways == 1000u, "freeing the pages of 1,000 tables gives way after each table");
    give_ways = 0;
    free_page_directory(dir);
    check(give_ways == 1u, "freeing 1,000 tables gives way once, after the 512th");
}

/** Run every row; 0 when each passes, 1 otherwise. */
int main(void)
{
    _kernel_dir = &test_dir;
    _curr_dir = &test_dir;
    _heap_ready = 1;

    mmio_rows();
    window_rows();
    temp_rows();
    temp_cpu_rows();
    fault_rows();
    recovered_rows();
    cow_race_rows();
    fault_race_rows();
    give_way_rows();
    check(boot_carves == 0, "no row reached the boot allocator after the heap exists");

    if (fails) {
        printf("paging-hosttest: result: fail: %d rows\n", fails);
        return 1;
    }
    printf("paging-hosttest: result: pass\n");
    return 0;
}
