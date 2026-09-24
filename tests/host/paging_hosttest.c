/**
 * Host test for the two range checks in mm/paging.c.
 *
 * host-deps: rach2/sys/mm/frame.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-cflags: -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast
 *
 * `map_mmio` and `premap_kernel_window` each take a range of the 32-bit
 * address space, and each promises in its header to refuse a range that
 * runs past the top of it (entry 140 of milestone 50). Neither is reachable
 * with such a range from the kernel's callers, so only a test can show the
 * refusals. The kernel's `uintptr_t` and `size_t` are 32 bits wide here as
 * on the machine, from lib/kint.h and lib/kdef.h, so every sum in the two
 * functions wraps at 2^32 as it does there.
 *
 * paging.c is included whole, which reaches the MMIO region's next free
 * page, and is compiled with RACH2_HOSTTEST, which leaves out its control
 * register, flags and TLB instructions. The page directory is the test's.
 * The heap is a stub that counts the page tables it gives, so a row can say
 * how many tables a window took; the boot allocator must not be reached
 * after the heap exists, and a row that reaches it fails.
 *
 * The rows: a register page mapped at the MMIO region's start; two spans
 * that begin in the first page and end within the last one, whose rounding
 * up by the old `+ PAGE_SIZE - 1` wraps to a few bytes, refused with the
 * region's next page unmoved; a region that wraps, refused; a window below
 * the top given its two tables; a window that runs past the top of the
 * address space, refused with no table made; a window that ends exactly at
 * the top given its table; and a window at address 0, given its table,
 * which is the regression a868ba10 fixed. tests/host/paging_mutate.sh
 * compiles the old rounding, the old loop bounded by `va_start + size`, and
 * the refusal without its exception for address 0, and requires each to
 * fail its row. Returns 0 when every row passes, 1 otherwise.
 */

#include "mm/paging.c"

extern int printf(const char *format, ...);
extern void *calloc(unsigned long count, unsigned long size);
extern void free(void *p);
extern void exit(int status);

static int fails;

static void check(int ok, const char *row)
{
    if (!ok) {
        printf("paging-hosttest: fail: %s\n", row);
        fails++;
    }
}

/* ---- what paging.c reaches outside itself ---- */

process_t *current_proc;
uintptr_t mem_reserved_ptr;

static int tables_given;   /* page tables the heap gave since a row began */
static int boot_carves;    /* calls to the boot allocator, which must be none */
static int refusals_noted; /* the refusals map_mmio recorded */

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

void kfree_pages(void *p, uint32_t count)
{
    (void)count;
    free(p);
}

uintptr_t mem_reserve(size_t size, bool align)
{
    (void)size;
    (void)align;
    boot_carves++;
    return 0;
}

void *mem_reserve_ptr(size_t size, bool align)
{
    (void)size;
    (void)align;
    boot_carves++;
    return 0;
}

void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
    refusals_noted++;
}

void k_panic(const char *str)
{
    printf("paging-hosttest: panic: %s\n", str);
    exit(1);
}

int kcmdline_has(const char *flag)
{
    (void)flag;
    return 0;
}

void kheap_init(void)
{}

void register_isr_handler(uint8_t vector, isr_handler_t handler, const char *name)
{
    (void)vector;
    (void)handler;
    (void)name;
}

void terminate_current_process(int32_t signo)
{
    (void)signo;
}

vma_t *vm_find(vm_space_t *vm, uint32_t addr)
{
    (void)vm;
    (void)addr;
    return 0;
}

int vm_grow_stack(vm_space_t *vm, uint32_t page, uint32_t as_limit)
{
    (void)vm;
    (void)page;
    (void)as_limit;
    return 0;
}

void mem_balance(void)
{}

int mem_may_take(int user_page)
{
    (void)user_page;
    return 1;
}

/* The fault address the handler reads in place of CR2; no row faults. */
uint32_t paging_hosttest_cr2(void)
{
    return 0;
}

/* ---- the rows ---- */

static page_dir_t test_dir;

/* The table index of a kernel virtual address. */
#define TABLE_OF(va) ((uint32_t)(va) / PAGE_SIZE / 1024u)

/* Drop the table at `index`, if the directory has one, so a row can ask
 * whether a window made it again. */
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

int main(void)
{
    _kernel_dir = &test_dir;
    _curr_dir = &test_dir;
    _heap_ready = 1;

    mmio_rows();
    window_rows();
    check(boot_carves == 0, "no row reached the boot allocator after the heap exists");

    if (fails) {
        printf("paging-hosttest: result: fail: %d rows\n", fails);
        return 1;
    }
    printf("paging-hosttest: result: pass\n");
    return 0;
}
