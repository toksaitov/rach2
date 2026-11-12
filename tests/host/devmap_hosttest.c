/**
 * Host test of device mappings, `vm_device_map` (mm/vm.c) and the fault,
 * fork, unmap and revocation paths of mm/paging.c, with i686's `cpu.h`;
 * devmap64_hosttest.c runs the same rows with x86-64's.
 *
 * host-deps: rach2/sys/mm/vm.c rach2/sys/mm/frame.c rach2/sys/mm/notify.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-deps: tests/host/compat/spinlock_host.c
 * host-deps: tests/host/compat/mmap_lock_host.c
 * host-deps: tests/host/compat/atomic_host.c
 * host-cflags: -Wno-pointer-to-int-cast -Wno-int-to-pointer-cast
 * host-cflags: -U__UINTPTR_TYPE__ -D__UINTPTR_TYPE__=unsigned
 * host-cflags: -U__INTPTR_TYPE__ -D__INTPTR_TYPE__=int
 * host-cflags: -U__SIZE_TYPE__ -D__SIZE_TYPE__=unsigned
 * host-cflags: -U__PTRDIFF_TYPE__ -D__PTRDIFF_TYPE__=int
 *
 * paging.c is included whole, which reaches its fault path and its frame
 * counts. The page directory is the test's own: a run of leaf tables over a
 * window of user space, which is all `mmu_user_table` and `mmu_user_skip`
 * are asked for, so the rows are the same on both architectures. Two
 * objects stand for a driver's: one a range of a device's frames above the
 * frames the allocator counts, mapped uncached, and one of frames the
 * allocator counts, which the object holds, mapped write-combining.
 *
 * The rows: a fault maps each object's page with its cache bits, writable
 * as the region is, and takes a reference on a counted frame and none on a
 * device's; a private mapping is refused; fork shares the pages and the
 * region's file, and leaves out a region marked MADV_DONTFORK; munmap and
 * exit drop the pages' references and the regions', and the object's
 * reference goes with the last region, once; a zap unmaps a range of an
 * object's pages in every address space and no other object's, after which
 * a moved page faults in at its new frame and a revoked one raises SIGBUS
 * with BUS_ADRERR at the address, from the page fault handler; and
 * paging_access reaches a device region through its object's `access`.
 * A fault calls a file's fill or share without the big lock when the file
 * sets `unlocked`, and under it otherwise, with the regions' lock let go
 * either way. Kernel process-memory writes mark their held shared file
 * frame before copying, even with a writable PTE; a refused mark copies
 * nothing.
 * Before them, the index of the free frames is searched up and down over
 * groups with no free frame.
 *
 * Returns 0 when every row passes, 1 otherwise.
 */

#define k_memcpy model_memcpy
#include "mm/paging.c"
#undef k_memcpy

extern void *k_memcpy(void *dst, const void *src, size_t n);

extern int printf(const char *format, ...);
extern void *calloc(unsigned long count, unsigned long size);
extern void *malloc(unsigned long n);
extern void free(void *p);
extern void exit(int status);

static int fails;

/** Count a failure and print `row` when `ok` is 0. */
static void check(int ok, const char *row)
{
    if (!ok) {
        printf("devmap-hosttest: fail: %s\n", row);
        fails++;
    }
}

/* ---- the page directory: leaf tables over a window of user space ---- */

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

#ifdef RACH2_HOST_X86_64
pte_t mmu_nx = PTE_NX;

/** 0: no row asks a kernel address's frame. */
phys_addr_t mmu_virt_to_phys(const volatile void *v)
{
    (void)v;
    return 0;
}

/** -1: no row maps the MMIO region in large pages. */
int mmu_kernel_large(uintptr_t va, phys_addr_t phys, pte_t flags, pte_t *saved)
{
    (void)va;
    (void)phys;
    (void)flags;
    (void)saved;
    return -1;
}

/** Nothing. */
void mmu_kernel_unlarge(uintptr_t va, pte_t saved)
{
    (void)va;
    (void)saved;
}
#endif

#ifndef ARCH_DIRECT_MAP_ALL
static pte_t temp_table[MMU_TABLE_ENTRIES];
#endif

/** The temporary mappings' table on i686; no other kernel mapping. */
pte_t *mmu_kernel_table(uintptr_t va)
{
#ifndef ARCH_DIRECT_MAP_ALL
    if (va >= KVA_TEMP && va < KVA_TEMP + KVA_TEMP_SLOTS * PAGE_SIZE) {
        return temp_table;
    }
#endif
    (void)va;
    return NULL;
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

/* ---- what paging.c reaches outside itself ---- */

struct percpu cpu_table[NR_CPUS];
uintptr_t mem_reserved_ptr;
process_t *proc_list;
volatile uint32_t init_pid = 1; /* no row faults as init */
bool debug_shell_is_home;

/* Whether the page attribute table gives write-combining, which a row
 * turns off to see the uncached bits in its place. */
static int pat_wc = 1;

/** PTE_WC, or the uncached bits with `pat_wc` 0. */
pte_t arch_pte_wc(void)
{
    return pat_wc ? PTE_WC : PTE_NOCACHE;
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

/** Nothing: one processor, no TLB. */
void smp_tlb_shootdown(const struct page_dir *dir, uintptr_t start, uintptr_t end)
{
    (void)dir;
    (void)start;
    (void)end;
}

/** Nothing. */
void smp_kernel_mapping_changed(void)
{}

/** Nothing. */
void smp_kernel_flush(uintptr_t start, uintptr_t end)
{
    (void)start;
    (void)end;
}

/* Nonzero while a row stands for a fault that runs without the big lock,
 * and the depth of the code's own takes of it then. */
static int bkl_free, bkl_depth;

/** Whether the big lock is held: always, as the kernel's callers hold it,
 * unless a row stands for a fault without it (`bkl_free`), and then only
 * inside the code's own takes. */
int bkl_held(void)
{
    return !bkl_free || bkl_depth > 0;
}

/** Take the big lock: counted while a row stands for a fault without it. */
void bkl_enter(void)
{
    if (bkl_free) {
        bkl_depth++;
    }
}

/** Give back a take of `bkl_enter`. */
void bkl_exit(void)
{
    if (bkl_free) {
        bkl_depth--;
    }
}

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

/* The fault address a fault row sets, as CR2 would have it. */
static uintptr_t fault_cr2;

/** The fault address a row set. */
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

/** Print nothing. */
void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

/** Print `str` and stop. */
void k_panic(const char *str)
{
    printf("devmap-hosttest: panic: %s\n", str);
    exit(1);
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

/* The last signal the fault handler sent. */
static int sent_signo;
static struct k_siginfo sent_info;

/** Record the signal. */
void signal_force(process_t *target, int signo, const struct k_siginfo *info)
{
    (void)target;
    sent_signo = signo;
    sent_info = *info;
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

/** Nothing. */
void terminate_current_process(int32_t signo)
{
    (void)signo;
}

/** `vm_resolve_in` as the fault handler calls it, with `mm`'s regions' lock
 * held to read once, which a file's or a device's fill lets go. */
static int resolve_held(struct mm *mm, uintptr_t addr, int write, uintptr_t as_limit)
{
    int rc;

    mmap_read_lock(mm);
    rc = vm_resolve_in(mm, addr, write, as_limit);
    mmap_unlock(mm);
    return rc;
}

/** Nothing: memory is never short. */
void mem_balance(void)
{}

/** 1: memory is never short. */
int mem_may_take(int user_page)
{
    (void)user_page;
    return 1;
}

/* ---- the objects ---- */

#define FRAMES     64u    /* the frames the allocator counts */
#define DEV_FRAME  0x800u /* the device range's first frame, past them */
#define OBJ_PAGES  4u
#define RAM_FRAME0 8u /* the RAM object's first frame */

/* A driver's object: its pages, references, and whether it is revoked. */
struct obj {
    uintptr_t frame[OBJ_PAGES]; /* the physical address of each page */
    int refs;                   /* references the mappings hold */
    int puts;                   /* `put` calls */
    int revoked;
    int busy;           /* `page` answers 1, to run the access again */
    uint64_t access_at; /* the offset of the last `access` */
    int accesses;       /* `access` calls */
};

/** The object's page `pgoff`, or -1 past its end or once revoked. */
static int obj_page(void *o, uint32_t pgoff, uintptr_t *phys)
{
    struct obj *ob = o;

    if (ob->revoked || pgoff >= OBJ_PAGES) {
        return -1;
    }
    if (ob->busy) {
        return 1;
    }
    *phys = ob->frame[pgoff];
    return 0;
}

/** Drop a mapping's reference. */
static void obj_put(void *o)
{
    struct obj *ob = o;

    ob->refs--;
    ob->puts++;
}

/** ptrace's reach into the object: a read gives 0x5a, a write is counted;
 * the device range refuses with -ENXIO. */
static int obj_access(void *o, uint64_t offset, void *buf, uint32_t len, int write)
{
    struct obj *ob = o;

    ob->access_at = offset;
    ob->accesses++;
    if (ob->revoked) {
        return -ENXIO;
    }
    if (!write) {
        k_memset(buf, 0x5a, len);
    }
    return 0;
}

static const struct vm_device_ops obj_ops = {obj_page, obj_put, obj_access, 0};

static struct obj dev_obj, ram_obj;

/* Two address spaces and their processes, A the running one. */
static struct mm mm_a, mm_b;
static page_dir_t dir_a, dir_b;
static process_t proc_a, proc_b;
static struct k_sigaction acts[KSIG_MAX + 1];

/** The frame count of the physical address `phys`. */
static uint32_t frame_count(uintptr_t phys)
{
    return _frames[phys / PAGE_SIZE];
}

/** The entry of `dir` for `va`, 0 when it has none. */
static pte_t pte_of(page_dir_t *dir, uintptr_t va)
{
    pte_t *t = mmu_user_table(dir, va, 0);

    return t ? t[MMU_INDEX(va)] : 0;
}

/**
 * Map `ob` at `va`, `pages` long, shared, with `prot` and the cache type
 * `cache`, into `vm` the way mmap does: a region given to `vm_device_map`,
 * then inserted, the caller's reference put. 0 or the error.
 */
static int map_obj(vm_space_t *vm, uintptr_t va, uint32_t pages, uint32_t prot, struct obj *ob,
                   uint32_t cache)
{
    vma_t vma;
    int rc;

    k_memset(&vma, 0, sizeof(vma));
    vma.end = (uintptr_t)pages * PAGE_SIZE;
    vma.prot = prot;
    vma.type = VM_SHARED;
    ob->refs++;
    rc = vm_device_map(&vma, &obj_ops, ob, cache);
    if (rc != 0) {
        ob->refs--;
        return rc;
    }
    rc = vm_insert_file(vm, va, va + vma.end, vma.prot, vma.type, vma.file, vma.pgoff);
    vm_file_put(vma.file);
    vm_file_reap();
    return rc;
}

#define VA_DEV (WIN_BASE)
#define VA_RAM (WIN_BASE + 16 * PAGE_SIZE)
#define VA_DF  (WIN_BASE + 32 * PAGE_SIZE)
#define RW     (VM_PROT_READ | VM_PROT_WRITE)

/**
 * A fault maps each kind with its cache bits and rights, taking a reference
 * on a counted frame alone; a private mapping is refused.
 */
static void fault_rows(void)
{
    pte_t e;
    vma_t vma;

    check(map_obj(&mm_a.vm, VA_DEV, OBJ_PAGES, RW, &dev_obj, VM_PROT_UC) == 0,
          "a device range maps");
    check(map_obj(&mm_a.vm, VA_RAM, OBJ_PAGES, VM_PROT_READ, &ram_obj, VM_PROT_WC) == 0,
          "an object's frames map");
    check(vm_find(&mm_a.vm, VA_DEV)->type == VM_SHARED &&
              (vm_find(&mm_a.vm, VA_DEV)->prot & (VM_PROT_IO | VM_PROT_UC)) ==
                  (VM_PROT_IO | VM_PROT_UC) &&
              vm_find(&mm_a.vm, VA_DEV)->file->refs == 1,
          "the region is shared device memory with the region's one reference to its file");
    check(dev_obj.puts == 0 && dev_obj.refs == 1, "the object keeps the mapping's reference");

    check(pte_of(&dir_a, VA_DEV) == 0, "nothing is mapped before the first touch");
    check(resolve_held(&mm_a, VA_DEV + PAGE_SIZE + 8, 1, ~(uintptr_t)0) == 1,
          "a write to the device range faults in");
    e = pte_of(&dir_a, VA_DEV + PAGE_SIZE);
    check(PTE_ADDR(e) == (DEV_FRAME + 1) * PAGE_SIZE && (e & PTE_NOCACHE) == PTE_NOCACHE &&
              (e & (PTE_P | PTE_W | PTE_U)) == (PTE_P | PTE_W | PTE_U),
          "the device page is its frame, uncached, present, writable and the user's");
    check(paging_user_phys(&dir_a, VA_DEV + PAGE_SIZE + 8) == (DEV_FRAME + 1) * PAGE_SIZE + 8,
          "a read of the mapping reaches the device's frame");

    check(resolve_held(&mm_a, VA_RAM + 2 * PAGE_SIZE, 0, ~(uintptr_t)0) == 1,
          "a read of the object's frames faults in");
    e = pte_of(&dir_a, VA_RAM + 2 * PAGE_SIZE);
    check(PTE_ADDR(e) == ram_obj.frame[2] && (e & PTE_NOCACHE) == PTE_WC && !(e & PTE_W),
          "the object's page is its frame, write-combining and read-only as its region");
    check(frame_count(ram_obj.frame[2]) == 2, "the mapping holds a reference on a counted frame");
    check(resolve_held(&mm_a, VA_RAM + 2 * PAGE_SIZE, 1, ~(uintptr_t)0) == 0,
          "a write to a read-only device region is refused");

    pat_wc = 0;
    check(resolve_held(&mm_a, VA_RAM + 3 * PAGE_SIZE, 0, ~(uintptr_t)0) == 1 &&
              (pte_of(&dir_a, VA_RAM + 3 * PAGE_SIZE) & PTE_NOCACHE) == PTE_NOCACHE,
          "without write-combining the page is uncached");
    pat_wc = 1;

    k_memset(&vma, 0, sizeof(vma));
    vma.end = PAGE_SIZE;
    vma.type = VM_FILE;
    check(vm_device_map(&vma, &obj_ops, &dev_obj, 0) == -EINVAL && vma.file == NULL,
          "a private mapping of device memory is refused");
}

/* ---- files a fault reads with or without the big lock ---- */

/* The frame a model file lends, past the RAM object's. */
#define SHARE_FRAME (RAM_FRAME0 + OBJ_PAGES)
#define VA_FILE     (WIN_BASE + 48 * PAGE_SIZE)

/* A file a region maps, which counts its operations: all of them, those
 * made with the big lock held, and those made with the regions' lock held. */
struct model_file {
    struct vm_file vf; /* first, so that the file is the record */
    int calls, locked, regions;
};

/** Count an operation of the file `f` and how it was called. */
static void model_count(struct vm_file *f)
{
    struct model_file *m = (struct model_file *)f;

    m->calls++;
    m->locked += bkl_held();
    m->regions += mmap_held(&mm_a, 0);
}

/* The shared frame's bytes and the copies that change them. */
static uint8_t shared_bytes[PAGE_SIZE];
static int shared_copies, shared_marks, mark_error, mark_retry;

/** Translate a temporary or direct mapping of SHARE_FRAME for a copy. */
static void *model_pointer(const void *p)
{
    uintptr_t va = (uintptr_t)p, phys;

#ifdef ARCH_DIRECT_MAP_ALL
    if (va < KVA_DIRECT || va >= KVA_DIRECT + KVA_DIRECT_SIZE) {
        return (void *)p;
    }
    phys = va - KVA_DIRECT;
#else
    if (va < KVA_TEMP || va >= KVA_TEMP + KVA_TEMP_SLOTS * PAGE_SIZE) {
        return (void *)p;
    }
    phys = PTE_ADDR(temp_table[MMU_INDEX(va)]) + (va & (PAGE_SIZE - 1));
#endif
    check(phys / PAGE_SIZE == SHARE_FRAME, "the copy reaches the shared file's frame");
    return shared_bytes + (phys & (PAGE_SIZE - 1));
}

/** Copy through the model's kernel mapping, counting stores to the frame. */
void *model_memcpy(void *dst, const void *src, size_t n)
{
    void *to = model_pointer(dst);

    if (to != dst) {
        shared_copies++;
        check(shared_marks > 0, "the file page is marked before the kernel stores");
    }
    k_memcpy(to, model_pointer(src), n);
    return dst;
}

/** Mark the held resident frame, or fail without allowing a copy. */
static int model_mkwrite(struct vm_file *f, uint32_t pgoff, uintptr_t phys)
{
    model_count(f);
    check(!mmap_held(&mm_a, 0) && pgoff == 0 && phys == SHARE_FRAME * PAGE_SIZE &&
              frame_count(phys) == 3,
          "the mark gets the file page held across the unlocked callback");
    shared_marks++;
    if (mark_retry) {
        mark_retry = 0;
        unmap_user_range(mm_a.dir, VA_FILE, VA_FILE + PAGE_SIZE);
        mm_a.zaps++;
        check(frame_count(phys) == 2, "the callback keeps the frame alive after its PTE goes");
        return -EAGAIN;
    }
    return mark_error;
}

/** A private mapping's fill: counted, the frame left as it is. */
static int model_fill(struct vm_file *f, uint32_t pgoff, uintptr_t phys)
{
    (void)pgoff;
    (void)phys;
    model_count(f);
    return 0;
}

/** A share: counted, and SHARE_FRAME lent with a reference. */
static int model_share(struct vm_file *f, uint32_t pgoff, uintptr_t *phys)
{
    (void)pgoff;
    model_count(f);
    *phys = (uintptr_t)SHARE_FRAME * PAGE_SIZE;
    frame_ref(*phys);
    return 0;
}

/** Nothing: the files are the test's own, held from the start. */
static void model_release(struct vm_file *f)
{
    (void)f;
}

/**
 * Map the model file `m` one page long at `va` of `mm_a`, as a region of
 * `type`, its file's operations run without the big lock when `unlocked`;
 * then fault the page in by a read from a fault that holds no big lock. 1
 * when it is mapped.
 */
static int file_fault_row(struct model_file *m, uintptr_t va, vm_type_t type, int unlocked)
{
    int rc;

    m->vf.refs = 1;
    m->vf.release = model_release;
    m->vf.unlocked = unlocked;
    if (vm_insert_file(&mm_a.vm, va, va + PAGE_SIZE, VM_PROT_READ, type, &m->vf, 0) != 0) {
        return 0;
    }
    bkl_free = 1;
    rc = resolve_held(&mm_a, va, 0, ~(uintptr_t)0);
    bkl_free = 0;
    return rc == 1 && bkl_depth == 0 && (pte_of(&dir_a, va) & PTE_P);
}

/**
 * A fault calls a file's fill or share with the regions' lock let go, and
 * with the big lock only for a file whose operations need it: an ext2
 * file's mapping sets `unlocked`, as Linux's filemap_fault takes no global
 * lock (mm/filemap.c:3305-3370), and a driver's or another type's does not.
 * Each fault gives back every take of the big lock it made.
 */
static void file_rows(void)
{
    static struct model_file fill_free, fill_locked, share_free, share_locked;

    fill_free.vf.fill = model_fill;
    fill_locked.vf.fill = model_fill;
    share_free.vf.share = model_share;
    share_locked.vf.share = model_share;
    _frames[SHARE_FRAME] = 1;
    _frames[0] = 1; /* frame 0's address is 0, which an allocator never gives */

    check(file_fault_row(&fill_free, VA_FILE, VM_FILE, 1),
          "a read of a file whose reads need no big lock faults in");
    check(fill_free.calls == 1 && fill_free.locked == 0 && fill_free.regions == 0,
          "and its fill runs with neither the big lock nor the regions' lock");
    check(file_fault_row(&fill_locked, VA_FILE + 2 * PAGE_SIZE, VM_FILE, 0),
          "a read of a file whose reads need the big lock faults in");
    check(fill_locked.calls == 1 && fill_locked.locked == 1 && fill_locked.regions == 0,
          "and its fill runs under the big lock, the regions' lock let go");
    check(file_fault_row(&share_free, VA_FILE + 4 * PAGE_SIZE, VM_SHARED, 1),
          "a read of a shared file whose pages need no big lock faults in");
    check(share_free.calls == 1 && share_free.locked == 0 && share_free.regions == 0,
          "and its share runs with neither the big lock nor the regions' lock");
    check(file_fault_row(&share_locked, VA_FILE + 6 * PAGE_SIZE, VM_SHARED, 0),
          "a read of a shared file whose pages need the big lock faults in");
    check(share_locked.calls == 1 && share_locked.locked == 1 && share_locked.regions == 0,
          "and its share runs under the big lock, the regions' lock let go");
    check(frame_count((uintptr_t)SHARE_FRAME * PAGE_SIZE) == 3,
          "each shared page holds a reference to the frame lent");

    vm_remove(&mm_a.vm, VA_FILE, VA_FILE + 8 * PAGE_SIZE);
    unmap_user_range(&dir_a, VA_FILE, VA_FILE + 8 * PAGE_SIZE);
    vm_file_reap();
    check(frame_count((uintptr_t)SHARE_FRAME * PAGE_SIZE) == 1 && fill_free.vf.refs == 1 &&
              share_locked.vf.refs == 1,
          "the unmap drops the pages' and the regions' references");
    _frames[0] = 0;
}

/** Kernel writes mark even a page whose PTE already allows writes. */
static void shared_access_rows(void)
{
    static struct model_file m;
    uint8_t byte = 0x6a;

    m.vf.refs = 1;
    m.vf.share = model_share;
    m.vf.mkwrite = model_mkwrite;
    m.vf.release = model_release;
    m.vf.unlocked = 1;
    _frames[SHARE_FRAME] = 1;
    check(vm_insert_file(&mm_a.vm, VA_FILE, VA_FILE + PAGE_SIZE, RW, VM_SHARED, &m.vf, 0) == 0 &&
              resolve_held(&mm_a, VA_FILE, 0, ~(uintptr_t)0) == 1 &&
              !(pte_of(&dir_a, VA_FILE) & PTE_W) && shared_marks == 0,
          "a read of a writable shared file leaves its page clean and read-only");
    check(paging_access(&mm_a, VA_FILE + 7, &byte, 1, 1) == 0 && shared_marks == 1 &&
              shared_copies == 1 && shared_bytes[7] == byte,
          "a process-memory write marks and changes a page with no earlier user store");
    byte++;
    check(paging_access(&mm_a, VA_FILE + 7, &byte, 1, 1) == 0 && shared_marks == 2 &&
              shared_copies == 2 && shared_bytes[7] == byte,
          "a second kernel store validates and marks the already writable page again");
    mark_error = -ENOMEM;
    byte++;
    check(paging_access(&mm_a, VA_FILE + 7, &byte, 1, 1) == -EIO && shared_marks == 3 &&
              shared_copies == 2 && shared_bytes[7] == byte - 1,
          "a refused mark fails the kernel write without copying any bytes");
    mark_error = 0;
    mark_retry = 1;
    check(paging_access(&mm_a, VA_FILE + 7, &byte, 1, 1) == 0 && shared_marks == 5 &&
              shared_copies == 3 && shared_bytes[7] == byte && !mark_retry,
          "a removed page is resolved and marked again before the kernel copies");
    vm_remove(&mm_a.vm, VA_FILE, VA_FILE + PAGE_SIZE);
    unmap_user_range(&dir_a, VA_FILE, VA_FILE + PAGE_SIZE);
    vm_file_reap();
    check(frame_count(SHARE_FRAME * PAGE_SIZE) == 1 && m.vf.refs == 1,
          "the callbacks and unmap give every frame and file reference back");
}

/**
 * paging_access, ptrace's and /proc/PID/mem's reach, goes through the
 * object's `access` at the byte of the object, a page at a time, and its
 * error is EIO.
 */
static void access_rows(void)
{
    uint8_t buf[16];
    uint32_t i;
    int ok = 1;

    k_memset(buf, 0, sizeof(buf));
    check(paging_access(&mm_a, VA_RAM + 2 * PAGE_SIZE + 8, buf, 8, 0) == 0 &&
              ram_obj.accesses == 1 && ram_obj.access_at == 2 * PAGE_SIZE + 8,
          "a peek at a device region asks its object at the byte of the object");
    for (i = 0; i < 8; i++) {
        ok &= buf[i] == 0x5a;
    }
    check(ok && buf[8] == 0, "the peek gives the object's bytes");
    check(paging_access(&mm_a, VA_RAM + PAGE_SIZE - 4, buf, 8, 1) == 0 && ram_obj.accesses == 3 &&
              ram_obj.access_at == PAGE_SIZE,
          "a poke across a page asks once a page");
    dev_obj.revoked = 1;
    check(paging_access(&mm_a, VA_DEV, buf, 4, 0) == -EIO && dev_obj.accesses == 1,
          "the object's refusal is EIO");
    dev_obj.revoked = 0;
}

/**
 * Fork shares the pages and the file, and leaves out a region marked
 * MADV_DONTFORK; munmap drops what the child had.
 */
static void fork_rows(void)
{
    vma_t *r;

    check(map_obj(&mm_a.vm, VA_DF, 2, RW, &ram_obj, 0) == 0 &&
              resolve_held(&mm_a, VA_DF, 1, ~(uintptr_t)0) == 1 &&
              frame_count(ram_obj.frame[0]) == 2,
          "a second mapping of the object faults in, cached");
    check((pte_of(&dir_a, VA_DF) & PTE_NOCACHE) == 0, "a cached device page has no cache bits");
    check(vm_advise(&mm_a.vm, VA_DF, VA_DF + PAGE_SIZE, VM_PROT_DONTFORK, 0) == 0 &&
              (vm_find(&mm_a.vm, VA_DF)->prot & VM_PROT_DONTFORK) &&
              !(vm_find(&mm_a.vm, VA_DF + PAGE_SIZE)->prot & VM_PROT_DONTFORK),
          "MADV_DONTFORK marks the first page's region and splits it from the second");

    check(paging_fork_copy(&mm_a, &dir_b) == 0 && vm_copy(&mm_b.vm, &mm_a.vm) == 0, "fork copies");
    check(PTE_ADDR(pte_of(&dir_b, VA_DEV + PAGE_SIZE)) == (DEV_FRAME + 1) * PAGE_SIZE &&
              (pte_of(&dir_b, VA_DEV + PAGE_SIZE) & (PTE_W | PTE_NOCACHE)) == (PTE_W | PTE_NOCACHE),
          "the child has the device page, writable and uncached, shared and not copied");
    check(frame_count(ram_obj.frame[2]) == 3, "the child's page holds its own reference");
    r = vm_find(&mm_b.vm, VA_RAM);
    check(r && r->file == vm_find(&mm_a.vm, VA_RAM)->file && r->file->refs == 2,
          "the child's region shares the parent's file, which counts both");
    check(!vm_find(&mm_b.vm, VA_DF) && pte_of(&dir_b, VA_DF) == 0 &&
              vm_find(&mm_b.vm, VA_DF + PAGE_SIZE),
          "the child has no region or page where MADV_DONTFORK was, and has the rest");
    check(frame_count(ram_obj.frame[0]) == 2, "a page left out of the child takes no reference");

    vm_remove(&mm_b.vm, VA_RAM, VA_RAM + OBJ_PAGES * PAGE_SIZE);
    unmap_user_range(&dir_b, VA_RAM, VA_RAM + OBJ_PAGES * PAGE_SIZE);
    vm_file_reap();
    check(frame_count(ram_obj.frame[2]) == 2 && vm_find(&mm_a.vm, VA_RAM)->file->refs == 1 &&
              ram_obj.puts == 0,
          "munmap in the child drops its page and its region, and the object stays");
}

/**
 * A zap unmaps a range of an object's pages in every address space and no
 * other object's; a moved page faults in again at its new frame, and a
 * revoked object raises SIGBUS from the fault handler.
 */
static void zap_rows(void)
{
    isr_params_t params;

    check(resolve_held(&mm_a, VA_DEV, 0, ~(uintptr_t)0) == 1 &&
              resolve_held(&mm_b, VA_DEV, 0, ~(uintptr_t)0) == 1 &&
              resolve_held(&mm_b, VA_DEV + 2 * PAGE_SIZE, 0, ~(uintptr_t)0) == 1,
          "both spaces fault device pages in");
    paging_zap_device(&dev_obj, 1, 1);
    check(pte_of(&dir_a, VA_DEV) != 0 && pte_of(&dir_a, VA_DEV + PAGE_SIZE) == 0 &&
              pte_of(&dir_b, VA_DEV + PAGE_SIZE) == 0 &&
              pte_of(&dir_b, VA_DEV + 2 * PAGE_SIZE) != 0,
          "a zap of one page unmaps it in both spaces and leaves its neighbours");
    check(pte_of(&dir_a, VA_RAM + 2 * PAGE_SIZE) != 0, "a zap leaves another object's pages");

    ram_obj.frame[2] = (RAM_FRAME0 + 9) * PAGE_SIZE;
    _frames[RAM_FRAME0 + 9] = 1;
    paging_zap_device(&ram_obj, 2, 0xFFFFFFFFu);
    check(pte_of(&dir_a, VA_RAM + 2 * PAGE_SIZE) == 0 &&
              pte_of(&dir_a, VA_RAM + 3 * PAGE_SIZE) == 0 &&
              frame_count((RAM_FRAME0 + 2) * PAGE_SIZE) == 1,
          "a zap to the object's end unmaps its pages and drops their references");
    check(resolve_held(&mm_a, VA_RAM + 2 * PAGE_SIZE, 0, ~(uintptr_t)0) == 1 &&
              PTE_ADDR(pte_of(&dir_a, VA_RAM + 2 * PAGE_SIZE)) == ram_obj.frame[2] &&
              frame_count(ram_obj.frame[2]) == 2,
          "a moved page faults in at its new frame");

    dev_obj.revoked = 1;
    paging_zap_device(&dev_obj, 0, OBJ_PAGES);
    check(pte_of(&dir_a, VA_DEV) == 0 && pte_of(&dir_b, VA_DEV + 2 * PAGE_SIZE) == 0,
          "a revocation unmaps every page in every space");
    k_memset(&params, 0, sizeof(params));
    params.vector = 14;
    params.error_code = 0x4 | 0x2; /* a write, from user mode, to a page not present */
    fault_cr2 = VA_DEV + PAGE_SIZE + 0x10;
    sent_signo = 0;
    _page_fault_handler(&params);
    check(sent_signo == SIGBUS && sent_info.code == BUS_ADRERR && sent_info.addr == fault_cr2 &&
              pte_of(&dir_a, VA_DEV + PAGE_SIZE) == 0,
          "a touch of a revoked page raises SIGBUS with BUS_ADRERR at its address");
    fault_cr2 = WIN_BASE + 3 * SPAN;
    sent_signo = 0;
    _page_fault_handler(&params);
    check(sent_signo == SIGSEGV && sent_info.code == SEGV_MAPERR,
          "a touch outside every region still raises SIGSEGV");
    ram_obj.busy = 1;
    paging_zap_device(&ram_obj, 0, OBJ_PAGES);
    params.error_code = 0x4; /* a read, from user mode, of a page not present */
    fault_cr2 = VA_RAM + 0x20;
    sent_signo = 0;
    _page_fault_handler(&params);
    check(sent_signo == 0 && proc_a.fault_retry && !proc_a.bus_fault && pte_of(&dir_a, VA_RAM) == 0,
          "an object that cannot give the page yet: no signal, nothing mapped, the access to run "
          "again");
    check(resolve_held(&mm_a, VA_RAM, 0, ~(uintptr_t)0) == 0,
          "and the kernel's own resolution of it fails");
    ram_obj.busy = 0;
}

/** munmap and exit drop the last references, and the object's goes once. */
static void exit_rows(void)
{
    vm_remove(&mm_a.vm, VA_DEV, VA_DEV + PAGE_SIZE);
    unmap_user_range(&dir_a, VA_DEV, VA_DEV + PAGE_SIZE);
    vm_file_reap();
    check(dev_obj.puts == 0, "munmap of part of a region keeps the object");

    vm_free(&mm_b.vm);
    unmap_user_range(&dir_b, WIN_BASE, WIN_BASE + WIN_TABLES * SPAN);
    vm_file_reap();
    check(dev_obj.puts == 0 && ram_obj.puts == 0, "the child's exit keeps what the parent maps");

    unmap_user_range(&dir_a, WIN_BASE, WIN_BASE + WIN_TABLES * SPAN);
    vm_free(&mm_a.vm);
    vm_file_reap();
    check(dev_obj.puts == 1 && dev_obj.refs == 0 && ram_obj.puts == 2 && ram_obj.refs == 0,
          "the last exit drops each mapping's reference to its object once");
    check(frame_count((RAM_FRAME0 + 0) * PAGE_SIZE) == 1 && frame_count(ram_obj.frame[2]) == 1 &&
              frame_count((RAM_FRAME0 + 3) * PAGE_SIZE) == 1,
          "every counted frame is back to the object's own reference");
}

/* The frames of the index rows: 40 groups, past the 32 that one word of the
 * index's bits covers. */
#define INDEX_FRAMES (FRAME_GROUP * 40u)

/**
 * The index of the free frames (`frame_index_build`), on a table of its own
 * with every frame taken but one in group 37 and one in group 39: the
 * search up passes the 37 groups with none, a word of 32 at a time, and
 * finds the first; the search down from the top finds the second, and from
 * there the first, passing group 38; a frame taken empties its group and
 * clears its bit, so that the search up goes on to the next group with
 * one; a frame put back is found again; and a search with none left in its
 * range answers its end.
 */
static void index_rows(void)
{
    uint32_t *saved = _frames;
    size_t saved_number = _frames_number;
    uintptr_t first = 37u * FRAME_GROUP + 5u, last = 39u * FRAME_GROUP + 60u, i;

    _frames_number = INDEX_FRAMES;
    _frames = calloc(frame_index_offset() + frame_index_bytes(), 1);
    _frame_group_bits = _frames + frame_index_offset() / sizeof(uint32_t);
    _frame_group_free = (uint8_t *)(_frame_group_bits + frame_index_words());
    for (i = 0; i < INDEX_FRAMES; i++) {
        _frames[i] = i != first && i != last;
    }
    frame_index_build();
    check(frame_next_free(0, INDEX_FRAMES) == first,
          "the index finds the first free frame past 37 groups with none");
    check(frame_prev_free(1, INDEX_FRAMES) == last, "and the last from the top");
    check(frame_prev_free(1, last) == first, "and from there the first, passing a group");
    check(frame_next_free(first + 1u, last) == last, "and none between them");
    _frames[first] = 1;
    frame_index_take(first);
    check(_frame_group_free[37] == 0 && !(_frame_group_bits[1] & (1u << 5)),
          "a frame taken empties its group and clears its bit");
    check(frame_next_free(0, INDEX_FRAMES) == last, "and the search goes on to the next group");
    _frames[first] = 0;
    frame_index_put(first);
    check(frame_next_free(0, INDEX_FRAMES) == first, "a frame put back is found again");
    check(frame_prev_free(1, first) == 0, "and none below it");
    free(_frames);
    _frames = saved;
    _frames_number = saved_number;
    _frame_group_bits = 0;
    _frame_group_free = 0;
    _frame_index_ready = 0;
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

/** Run every row; 0 when each passes, 1 otherwise. */
int main(void)
{
    uint32_t i;

    _frames = calloc(FRAMES, sizeof(uint32_t));
    _frames_number = FRAMES;
    _frame_anon = calloc((FRAMES + 7u) / 8u, 1);
    for (i = 0; i < OBJ_PAGES; i++) {
        dev_obj.frame[i] = (uintptr_t)(DEV_FRAME + i) * PAGE_SIZE;
        ram_obj.frame[i] = (uintptr_t)(RAM_FRAME0 + i) * PAGE_SIZE;
        _frames[RAM_FRAME0 + i] = 1;
    }
    mm_a.dir = &dir_a;
    mm_b.dir = &dir_b;
    proc_a.sigacts = acts;
    proc_a.mm = &mm_a;
    proc_b.mm = &mm_b;
    proc_a.next = &proc_b;
    proc_list = &proc_a;
    cpu_table[0].current = &proc_a;
    _curr_dir = &dir_a;
    _kernel_dir = &dir_a;

    index_rows();
    fault_rows();
    file_rows();
    shared_access_rows();
    access_rows();
    fork_rows();
    zap_rows();
    exit_rows();
    dir_free(&dir_a);
    dir_free(&dir_b);
    vm_free(&mm_a.vm);
    vm_free(&mm_b.vm);
    free(_frame_anon);
    free(_frames);

    if (fails) {
        printf("devmap-hosttest: result: fail: %d rows\n", fails);
        return 1;
    }
    printf("devmap-hosttest: result: pass\n");
    return 0;
}
