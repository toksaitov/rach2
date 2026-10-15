/**
 * Host test of the range checks of `map_mmio` and `premap_kernel_window`, of
 * the temporary mappings, and of the panic for a supervisor fault in a
 * kernel stack's guard page, on i686.
 *
 * host-deps: rach2/sys/mm/frame.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/spinlock_host.c
 * host-cflags: -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast
 * host-cflags: -U__UINTPTR_TYPE__ -D__UINTPTR_TYPE__=unsigned
 * host-cflags: -U__INTPTR_TYPE__ -D__INTPTR_TYPE__=int
 * host-cflags: -U__SIZE_TYPE__ -D__SIZE_TYPE__=unsigned
 * host-cflags: -U__PTRDIFF_TYPE__ -D__PTRDIFF_TYPE__=int
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
 * one change of kernel mappings, so 1024 mappings make one change; a held
 * slot survives the sweeps; and a mapping fails when every slot is held.
 *
 * Two rows drive the page fault handler with a supervisor write fault: at an
 * address in the guard page below the running thread's kernel stack, which
 * `kstack_overrun_pid` answers with the thread's pid, it must panic as that
 * stack's overrun, and at the stack's lowest byte, above the guard, with the
 * handler's general panic. Each runs in a forked child, whose exit status
 * says which panic came.
 *
 * tests/host/paging_mutate.sh compiles the rounding by `+ PAGE_SIZE - 1`,
 * the loop bounded by `va_start + size`, the range check without its
 * exception for address 0, the handler without its test of the guard page,
 * and four faults of the temporary mappings, and requires each to fail its
 * row. Returns 0 when every row passes, 1 otherwise.
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
uintptr_t mem_reserved_ptr;

static int tables_given;   /* page tables the heap gave since a row began */
static int boot_carves;    /* calls to the boot allocator, which must be none */
static int refusals_noted; /* calls to klog, one for each range that failed */

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

/** Nothing: the host has no task state segment. */
void tss_set_double_fault_dir(uint32_t dir)
{
    (void)dir;
}

/** Nothing: the fault handler calls it on a path no row takes. */
void signal_force(process_t *target, int signo, const struct k_siginfo *info)
{
    (void)target;
    (void)signo;
    (void)info;
}

/** 0: the page-cache window is empty. */
uint32_t pc_window_size(void)
{
    return 0;
}

/** 0, as for an address the page cache does not back. */
int pc_fault(uintptr_t fault_address, int write)
{
    (void)fault_address;
    (void)write;
    return 0;
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

/** Nothing: no row faults in user mode. */
void terminate_current_process(int32_t signo)
{
    (void)signo;
}

/** 0: no address is in a mapping. */
vma_t *vm_find(vm_space_t *vm, uintptr_t addr)
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

/** One processor: no other has a translation to drop. */
void smp_tlb_shootdown(const struct page_dir *dir, uintptr_t va)
{
    (void)dir;
    (void)va;
}

/** Count a change of kernel mappings. */
void smp_kernel_mapping_changed(void)
{
    kernel_changes++;
}

/** Nothing: the host has no TLB. */
void arch_tlb_flush_page(uintptr_t va)
{
    (void)va;
}

/** 0: the host has no interrupt flag. */
uint32_t kirq_save(void)
{
    return 0;
}

/** Nothing: the host has no interrupt flag. */
void kirq_restore(uint32_t flags)
{
    (void)flags;
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
    check(va == KVA_MMIO && _mmio_next == KVA_MMIO + PAGE_SIZE &&
              test_dir.logical_page_tables[TABLE_OF(KVA_MMIO)]->pages[0] == (0xFEC00000u | 0x1Bu),
          "a register page is mapped at the MMIO region's start, uncached");

    next = _mmio_next;
    refusals_noted = 0;
    va = map_mmio(0x0u, 0xFFFFF001u);
    check(va == 0 && _mmio_next == next && refusals_noted == 1,
          "a span from the first page into the last, whose rounding would wrap, is refused");
    va = map_mmio(0x800u, 0xFFFFF7FFu);
    check(va == 0 && _mmio_next == next && refusals_noted == 2,
          "a span ending on the last byte of the address space is refused");
    va = map_mmio(0xFFFFF000u, 0x2000u);
    check(va == 0 && _mmio_next == next && refusals_noted == 3, "a region that wraps is refused");
    check(test_dir.logical_page_tables[TABLE_OF(KVA_MMIO)]->pages[1] == 0,
          "a refused region maps nothing");

    va = map_mmio(0xFED00010u, 0x20u);
    check(va == next + 0x10u && _mmio_next == next + PAGE_SIZE,
          "the next region takes the page after the first, at its offset in the page");
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

/**
 * The temporary mappings: a released slot stays mapped and is not given
 * again before a sweep, which comes only when no slot is free and unmaps
 * every released slot with one change of kernel mappings; a held slot
 * survives the sweeps; and with every slot held a mapping is refused.
 */
static void temp_rows(void)
{
    static uint8_t *held[KVA_TEMP_SLOTS];
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

    for (i = 1; i < KVA_TEMP_SLOTS; i++) {
        va = kmap_temp(0x6000000u + i * PAGE_SIZE);
        wrong |= va != (uint8_t *)KVA_TEMP + i * PAGE_SIZE;
        kunmap_temp(va);
    }
    check(!wrong && kernel_changes == 0 && space_loads == 0,
          "1024 temporary mappings take the 1024 slots in turn with no change of kernel mappings");

    held[0] = kmap_temp(0x7000000u);
    for (i = 1, mapped = 0; i < KVA_TEMP_SLOTS; i++) {
        mapped += TEMP_PTE(i) != 0;
    }
    check(held[0] == (uint8_t *)KVA_TEMP && mapped == 0 && kernel_changes == 1 && space_loads == 1,
          "with no slot free, a mapping sweeps every released slot with one change of kernel "
          "mappings and takes the first");

    kernel_changes = 0;
    for (i = 0; i < 2 * KVA_TEMP_SLOTS; i++) {
        va = kmap_temp(0x8000000u);
        wrong |= va == held[0];
        kunmap_temp(va);
    }
    check(!wrong && TEMP_PTE(0) == (0x7000000u | 0x3u) && kernel_changes == 2,
          "a held temporary slot is not given again and survives two sweeps");

    for (i = 1; i < KVA_TEMP_SLOTS; i++) {
        held[i] = kmap_temp(0x9000000u);
        wrong |= held[i] == 0;
    }
    check(!wrong && kmap_temp(0xA000000u) == 0,
          "with every temporary slot held, a mapping is refused");
    for (i = 0; i < KVA_TEMP_SLOTS; i++) {
        kunmap_temp(held[i]);
    }
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

/** Run every row; 0 when each passes, 1 otherwise. */
int main(void)
{
    _kernel_dir = &test_dir;
    _curr_dir = &test_dir;
    _heap_ready = 1;

    mmio_rows();
    window_rows();
    temp_rows();
    fault_rows();
    check(boot_carves == 0, "no row reached the boot allocator after the heap exists");

    if (fails) {
        printf("paging-hosttest: result: fail: %d rows\n", fails);
        return 1;
    }
    printf("paging-hosttest: result: pass\n");
    return 0;
}
