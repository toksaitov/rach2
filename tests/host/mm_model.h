/**
 * A model of what mm/paging.c reaches outside itself, for the host tests
 * that include it whole: pin_hosttest.c, mmnotify_hosttest.c and
 * tlb_hosttest.c, each on i686's `cpu.h` and, with RACH2_HOST_X86_64, on
 * x86-64's.
 *
 * A user page directory is a run of leaf tables over a window of user
 * space, which is all `mmu_user_table` and `mmu_user_skip` are asked for.
 * The kernel's tables are those of the MMIO region alone, made whole as the
 * boot makes them; on x86-64 each has a directory entry that
 * `mmu_kernel_large` replaces with a 2 MiB page and `mmu_kernel_unlarge`
 * puts back, as x86-64's mmu.c does. Memory is never short unless a test
 * defines MM_MODEL_PRESSURE and includes mm/pressure.c itself. Two address
 * spaces and their processes, A the running one, are set up by
 * `model_start` and freed by `model_end`.
 */

#include "arch/arch.h"
#include "lib/kstring.h"

/* The frames' contents, which the direct map reaches on x86-64 and the
 * temporary slots on i686: host memory, FRAMES pages from physical 0. */
static unsigned char *frame_mem;
static void *model_frame(uintptr_t phys);
#undef phys_to_virt
#define phys_to_virt(p) model_frame((uintptr_t)(p))
#ifndef RACH2_HOST_X86_64
static void *model_memset(void *p, int c, size_t n);
static void *model_memcpy(void *d, const void *s, size_t n);
#define k_memset model_memset
#define k_memcpy model_memcpy
#endif

#include "mm/paging.c"

#ifndef RACH2_HOST_X86_64
#undef k_memset
#undef k_memcpy
#endif

extern int printf(const char *format, ...);
extern void *calloc(unsigned long count, unsigned long size);
extern void *malloc(unsigned long n);
extern void free(void *p);
extern void exit(int status);

/* The test's name, which every line it prints starts with. */
static const char *model_name;
static int fails;

/** Count a failure and print `row` when `ok` is 0. */
static void check(int ok, const char *row)
{
    if (!ok) {
        printf("%s: fail: %s\n", model_name, row);
        fails++;
    }
}

/* ---- the user page directory: leaf tables over a window of user space ---- */

#define SPAN       ((uintptr_t)MMU_TABLE_ENTRIES * PAGE_SIZE)
#define WIN_BASE   (64 * SPAN) /* above USER_ADDR_MIN on both */
#define WIN_TABLES 4u

struct page_dir {
    pte_t *tables[WIN_TABLES];
};

/** The leaf table of `dir` for `va`, made with `create`; NULL outside the
 * window. */
pte_t *mmu_user_table(page_dir_t *dir, uintptr_t va, int create)
{
    uintptr_t i;

    if (va < WIN_BASE || va >= WIN_BASE + WIN_TABLES * SPAN) {
        return NULL;
    }
    i = (va - WIN_BASE) / SPAN;
    if (!dir->tables[i] && create) {
        dir->tables[i] = calloc(MMU_TABLE_ENTRIES, sizeof(pte_t));
    }
    return dir->tables[i];
}

/** `va` when `dir` has its table, else the start of the next table's span. */
uintptr_t mmu_user_skip(page_dir_t *dir, uintptr_t va)
{
    if (mmu_user_table(dir, va, 0)) {
        return va;
    }
    return (va & ~(SPAN - 1)) + SPAN;
}

/** 0: no row loads a directory. */
uintptr_t get_page_dir_phys(page_dir_t *dir)
{
    (void)dir;
    return 0;
}

/** Free the tables of `dir`. */
static void dir_free(page_dir_t *dir)
{
    uint32_t i;

    for (i = 0; i < WIN_TABLES; i++) {
        free(dir->tables[i]);
        dir->tables[i] = NULL;
    }
}

/* ---- the kernel's tables: the MMIO region's ---- */

#define MODEL_TABLES (KVA_MMIO_SIZE / MMU_TABLE_SPAN)
static pte_t *mmio_leaf[MODEL_TABLES];

#ifdef RACH2_HOST_X86_64
pte_t mmu_nx = PTE_NX;
/* The directory entry of each table: MODEL_TABLE while it names its leaf
 * table, a 2 MiB page's entry while `mmu_kernel_large` replaced it. */
#define MODEL_TABLE ((pte_t)0x5000 | PTE_P | PTE_W)
static pte_t mmio_pde[MODEL_TABLES];
/* Set when `mmu_kernel_large` found a leaf entry still present. */
static int large_over_entries;

/** 0: no row asks a kernel address's frame. */
phys_addr_t mmu_virt_to_phys(const volatile void *v)
{
    (void)v;
    return 0;
}

/** As x86-64's mmu.c: the entry must name a table, and both addresses
 * must be aligned to the span. */
int mmu_kernel_large(uintptr_t va, phys_addr_t phys, pte_t flags, pte_t *saved)
{
    uintptr_t i = (va - KVA_MMIO) / MMU_TABLE_SPAN;
    uint32_t n;

    if (va < KVA_MMIO || i >= MODEL_TABLES || mmio_pde[i] != MODEL_TABLE ||
        ((va | phys) & (MMU_TABLE_SPAN - 1)) != 0) {
        return -1;
    }
    for (n = 0; n < MMU_TABLE_ENTRIES; n++) {
        large_over_entries |= mmio_leaf[i][n] != 0;
    }
    *saved = mmio_pde[i];
    mmio_pde[i] = (pte_t)phys | PTE_PS | flags;
    return 0;
}

/** Put the entry back. */
void mmu_kernel_unlarge(uintptr_t va, pte_t saved)
{
    mmio_pde[(va - KVA_MMIO) / MMU_TABLE_SPAN] = saved;
}

/** The directory entry of the MMIO region's span at `va`. */
static pte_t model_pde(uintptr_t va)
{
    return mmio_pde[(va - KVA_MMIO) / MMU_TABLE_SPAN];
}
#endif

#ifndef RACH2_HOST_X86_64
/* The leaf table of i686's temporary slots, one table's span. */
static pte_t temp_table[KVA_TEMP_SLOTS];
#endif

/** The MMIO region's leaf table for `va`, none under a large page, as the
 * kernel's walk finds none there, or on i686 the temporary slots' table;
 * NULL elsewhere. */
pte_t *mmu_kernel_table(uintptr_t va)
{
    uintptr_t i;

#ifndef RACH2_HOST_X86_64
    if (va >= KVA_TEMP && va - KVA_TEMP < KVA_TEMP_SLOTS * PAGE_SIZE) {
        return temp_table;
    }
#endif
    if (va < KVA_MMIO || va - KVA_MMIO >= KVA_MMIO_SIZE) {
        return NULL;
    }
    i = (va - KVA_MMIO) / MMU_TABLE_SPAN;
#ifdef RACH2_HOST_X86_64
    if (mmio_pde[i] & PTE_PS) {
        return NULL;
    }
#endif
    return mmio_leaf[i];
}

/** The MMIO region's leaf entry for `va`, 0 where there is none. */
static pte_t model_kpte(uintptr_t va)
{
    pte_t *t = mmu_kernel_table(va);

    return t ? t[MMU_INDEX(va)] : 0;
}

/* ---- what paging.c reaches outside itself ---- */

struct percpu cpu_table[NR_CPUS];
uintptr_t mem_reserved_ptr;
process_t *proc_list;

/** Write-combining, as a machine with the page attribute table has it. */
pte_t arch_pte_wc(void)
{
    return PTE_WC;
}

/** Nothing: the rows need no frame map. */
void mmu_init(void)
{}

/** Nothing. */
void mmu_start(void)
{}

/** Nothing. */
void mmu_heap_ready(void)
{}

/** NULL: no row builds the frame table from a map. */
void *mmu_frame_table(struct frame_map *map, size_t bytes)
{
    (void)map;
    (void)bytes;
    return NULL;
}

/* The shootdowns and kernel flushes asked for, which a row may count; the
 * last shootdown's range; and a row's check, run at each shootdown. */
static uint32_t shootdowns, kernel_flushes;
static uintptr_t shot_start, shot_end;
static void (*shot_check)(const struct page_dir *dir, uintptr_t start, uintptr_t end);

/** Count, keep the range and run the row's check: one processor, no TLB. */
void smp_tlb_shootdown(const struct page_dir *dir, uintptr_t start, uintptr_t end)
{
    shootdowns++;
    shot_start = start;
    shot_end = end;
    if (shot_check) {
        shot_check(dir, start, end);
    }
}

/** Count. */
void smp_kernel_mapping_changed(void)
{
    kernel_flushes++;
}

/** Nothing: the test has no address space that counts its resident pages. */
void mm_resident_add(const struct page_dir *dir, long pages)
{
    (void)dir;
    (void)pages;
}

/** Nothing. */
void arch_tlb_flush_page(uintptr_t va)
{
    (void)va;
}

/** 0. */
uintptr_t arch_space_root(void)
{
    return 0;
}

/** Nothing. */
void arch_space_load(uintptr_t root)
{
    (void)root;
}

/** 0: no row takes a fault through the handler. */
uintptr_t arch_fault_address(void)
{
    return 0;
}

/** 0. */
uint32_t kirq_save(void)
{
    return 0;
}

/** Nothing. */
void kirq_restore(uint32_t flags)
{
    (void)flags;
}

/* The records klog was asked for, which a refusal row counts. */
static uint32_t logged;

/** Count, and print nothing. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
    logged++;
}

/** Print `str` and stop. */
void k_panic(const char *str)
{
    printf("%s: panic: %s\n", model_name, str);
    exit(1);
}

/** Nothing: no row takes a fault through the handler. */
void signal_force(process_t *target, int signo, const struct k_siginfo *info)
{
    (void)target;
    (void)signo;
    (void)info;
}

/** 0. */
uint32_t pc_window_size(void)
{
    return 0;
}

/** 0. */
int pc_fault(uintptr_t fault_address, int write)
{
    (void)fault_address;
    (void)write;
    return 0;
}

/** -1: no address is a kernel stack's guard. */
int kstack_overrun_pid(uintptr_t addr)
{
    (void)addr;
    return -1;
}

/** 0. */
int kcmdline_has(const char *flag)
{
    (void)flag;
    return 0;
}

/** `most`. */
size_t kheap_size_for(uint64_t managed, uint64_t most)
{
    (void)managed;
    return (size_t)most;
}

/** Nothing. */
void kheap_init(size_t bytes)
{
    (void)bytes;
}

/** The host's malloc. */
void *kmalloc(size_t n)
{
    return malloc(n);
}

/** The host's calloc. */
void *kzalloc(size_t n)
{
    return calloc(1, n);
}

/** The host's free. */
void kfree(void *p)
{
    free(p);
}

/** NULL: no row takes pages from the heap. */
void *kalloc_pages(uint32_t count, uintptr_t *phys)
{
    (void)count;
    if (phys) {
        *phys = 0;
    }
    return NULL;
}

/** Nothing. */
void kfree_pages(void *p, uint32_t count)
{
    (void)p;
    (void)count;
}

/** 0. */
uintptr_t mem_reserve(size_t size, bool align)
{
    (void)size;
    (void)align;
    return 0;
}

/** NULL. */
void *mem_reserve_ptr(size_t size, bool align)
{
    (void)size;
    (void)align;
    return NULL;
}

/** Nothing. */
void mem_reserve_usable(const struct frame_map *map, size_t size)
{
    (void)map;
    (void)size;
}

/** Nothing. */
void register_isr_handler(uint8_t vector, isr_handler_t handler, const char *name)
{
    (void)vector;
    (void)handler;
    (void)name;
}

/** Nothing. */
void terminate_current_process(int32_t signo)
{
    (void)signo;
}

/** Nothing: the fault's note is for the panic's report, which no row reads. */
void k_panic_note_fault(const char *name, uintptr_t ip, long error)
{
    (void)name;
    (void)ip;
    (void)error;
}

/** Nothing, as `k_panic_note_fault`. */
void k_panic_note_address(uintptr_t address)
{
    (void)address;
}

/** No panic reads a process here. */
int k_panic_reading(void)
{
    return 0;
}

#ifndef MM_MODEL_PRESSURE
/** Nothing: memory is never short. */
void mem_balance(void)
{}

/** 1: memory is never short. */
int mem_may_take(int user_page)
{
    (void)user_page;
    return 1;
}
#endif

/* ---- the frames, the address spaces and the processes ---- */

/* The frames the allocator counts, the first USER_FRAME0 of them kept back
 * so that a frame a row names stays apart from those faults take. */
#define FRAMES      2048u
#define USER_FRAME0 64u

static struct mm mm_a, mm_b;
static page_dir_t dir_a, dir_b;
static process_t proc_a, proc_b;
static struct k_sigaction acts[KSIG_MAX + 1];

/** The host address of the physical address `phys`, NULL past the frames. */
static void *model_frame(uintptr_t phys)
{
    return phys < (uintptr_t)FRAMES * PAGE_SIZE ? frame_mem + phys : NULL;
}

#ifndef RACH2_HOST_X86_64
/** The host address of `p`: of the frame its slot maps when it is one of
 * the temporary slots, else `p`. */
static void *model_host(const void *p)
{
    unsigned long long va = (unsigned long long)p; /* the host's width, not uintptr_t's */

    if (va >= KVA_TEMP && va - KVA_TEMP < KVA_TEMP_SLOTS * PAGE_SIZE) {
        pte_t e = temp_table[(va - KVA_TEMP) / PAGE_SIZE];

        return model_frame((uintptr_t)PTE_ADDR(e) + (uintptr_t)(va & (PAGE_SIZE - 1)));
    }
    return (void *)p;
}

/** k_memset through the temporary slots. */
static void *model_memset(void *p, int c, size_t n)
{
    k_memset(model_host(p), c, n);
    return p;
}

/** k_memcpy through the temporary slots. */
static void *model_memcpy(void *d, const void *s, size_t n)
{
    k_memcpy(model_host(d), model_host(s), n);
    return d;
}
#endif

/** The frame word of the physical address `phys`. */
static uint32_t frame_word(uintptr_t phys)
{
    return _frames[phys / PAGE_SIZE];
}

/** The entry of `dir` for `va`, 0 when it has none. */
static pte_t pte_of(page_dir_t *dir, uintptr_t va)
{
    pte_t *t = mmu_user_table(dir, va, 0);

    return t ? t[MMU_INDEX(va)] : 0;
}

/** Set up the frame table, the MMIO region's tables and A and B, with A
 * running, for the test `name`. */
static void model_start(const char *name)
{
    uint32_t i;

    model_name = name;
    _frames = calloc(FRAMES, sizeof(uint32_t));
    _frame_anon = calloc((FRAMES + 7u) / 8u, 1); /* init_paging's anonymous-frame bits */
    frame_mem = calloc(FRAMES, PAGE_SIZE);
    _frames_number = FRAMES;
    _window_frames = FRAMES;
    _managed_frames = FRAMES - USER_FRAME0;
    _free_frames = FRAMES - USER_FRAME0;
    _free_window_frames = FRAMES - USER_FRAME0;
    _user_frames_start = USER_FRAME0;
    _next_free_frame = USER_FRAME0;
    _next_free_upper = FRAMES;
    for (i = 0; i < USER_FRAME0; i++) {
        _frames[i] = 1;
    }
    for (i = 0; i < MODEL_TABLES; i++) {
        mmio_leaf[i] = calloc(MMU_TABLE_ENTRIES, sizeof(pte_t));
#ifdef RACH2_HOST_X86_64
        mmio_pde[i] = MODEL_TABLE;
#endif
    }
    mm_a.dir = &dir_a;
    mm_b.dir = &dir_b;
    mm_a.users = mm_b.users = 1;
    proc_a.sigacts = acts;
    proc_a.mm = &mm_a;
    proc_b.mm = &mm_b;
    proc_a.next = &proc_b;
    for (i = 0; i < RLIM_COUNT; i++) {
        proc_a.rlim[i].cur = proc_a.rlim[i].max = RLIM_INFINITY;
    }
    proc_list = &proc_a;
    cpu_table[0].current = &proc_a;
    _curr_dir = &dir_a;
    _kernel_dir = &dir_a;
}

/** Free what `model_start` and the rows made, and print the result: 0 when
 * every row passed, 1 otherwise. */
static int model_end(void)
{
    uint32_t i;

    dir_free(&dir_a);
    dir_free(&dir_b);
    vm_free(&mm_a.vm);
    vm_free(&mm_b.vm);
    vm_file_reap();
    for (i = 0; i < MODEL_TABLES; i++) {
        free(mmio_leaf[i]);
    }
    free(_frame_anon);
    free(_frames);
    free(frame_mem);
    if (fails) {
        printf("%s: result: fail: %d rows\n", model_name, fails);
        return 1;
    }
    printf("%s: result: pass\n", model_name);
    return 0;
}
