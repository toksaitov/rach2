/**
 * Check KVM's memory slots, guest-physical tables and dirty log over a
 * model of the caller's page tables.
 *
 * host-deps: tests/host/compat/atomic_host.c tests/host/compat/spinlock_host.c
 * host-deps: tests/host/compat/kirq_none.c tests/host/compat/host_threads.c
 * host-cflags: -DRACH2_HOST_X86_64 -DRACH2_HOST_CPU -pthread
 *
 * slots.c, tdp.c, dirty.c and mm/notify.c are the kernel's, included
 * whole. The caller's memory is a model: a window of user addresses whose
 * leaf tables are arrays here, a page brought in by `vm_prefault_range` as
 * a fault would, with a new frame for a write to a copy-on-write page, and
 * the changes munmap, mprotect, a copy-on-write break, madvise and a
 * truncation make, each told to the change notice before the tables change,
 * as the kernel's calls do, the break counted under way across its notice
 * (`mmn_change_begin`). Hooks run a row's code at the fault's take of the
 * regions' lock, its read of an entry and its waits, as another
 * processor's change would run there. RAM is host memory reached through
 * the direct map's formula, so that a frame's address is its physical one,
 * and a device's frames are addresses below 1 TiB, which no host frame
 * has. The backend's entry format is EPT's.
 *
 * The rows: the dirty log's structures and ioctl numbers; the capabilities;
 * KVM_SET_USER_MEMORY_REGION's checks in Linux's order and its answers for a
 * new, moved, flagged, unchanged and deleted slot and an overlap, a move
 * and a deletion taking back the guest page maps over the slot's old frames
 * (`kvm_gpamap_zap`, recorded here); a 4 KiB slot
 * mapped at each fault with the caller's frame, write-back with the guest's PAT
 * ignored, or in charge while a device that does not snoop is assigned, the
 * first and the last such device dropping every leaf, read-only over a
 * copy-on-write page and writable after the write that breaks it; a slot of
 * 2 MiB-aligned contiguous RAM as one 2 MiB leaf, and
 * as 4 KiB leaves where the slot, the addresses or the frames do not allow one
 * or a page of memfd_secret lies in the block; a page that stands for the zero
 * page mapped read-only on a read and writable, its mark ended, on a write;
 * a device's frames uncached with the guest's PAT in charge, as a 2 MiB leaf
 * too; KVM_PRE_FAULT_MEMORY mapping a range a leaf at a time, moving it past
 * what it mapped, and its refusals; the leaves counted by size as pages_4k
 * and pages_2m; a read-only slot, a frame with no slot and an address the
 * caller cannot touch; each change of the caller's memory dropping what the
 * tables map of it and asking every vCPU to flush, a change to one page of a
 * 2 MiB leaf dropping the leaf; the end of the address space; dirty logging
 * with KVM_GET_DIRTY_LOG and, under KVM_CAP_MANUAL_DIRTY_LOG_PROTECT2,
 * KVM_CLEAR_DIRTY_LOG, their refusals, a kernel's write logged in the slot
 * it copied through while a move or a deletion of the slot waits for it, and
 * no 2 MiB leaf in a slot that logs; kvm_tdp_write_nofault refusing a
 * read-only slot, a page not yet stored into, read-only, a device's or
 * absent, a write across a page or of nothing, an address of no slot, a
 * caller's check made before the tables last changed and a thread of
 * another address space, and writing a stored-into page of RAM at its
 * offset, logged; a zap in batches of a few entries, with a fault between
 * two at a frame a move's zap passed mapping nothing, and a zap of every
 * frame passing absent subtrees whole; the fault's check before its leaf: a
 * page change finished, one under way and a notice's zap between its read
 * of the counts and its leaf each making it map nothing and look again,
 * and a copy-on-write break there giving it the new frame; a flush waiting
 * for a vCPU in the guest only until it takes the request; a slot moved,
 * its flags changed and deleted with its leaves and tables gone; two
 * replacements of the read side at once, on host threads that pass one
 * baton, the second coming while the first waits for a reader from before
 * both, and ending while new readers keep overlapping; and every
 * allocation and frame given back.
 */
/* A zap's batch of 16 entries, so that the rows' zaps break. */
#define TDP_ZAP_BATCH 16u

#include "kvm/dirty.c"
#include "kvm/slots.c"

/* tdp.c's and notify.c's lets go of a spinlock with interrupts restored
 * go through the hook that runs a row's code between two batches. */
static void unlock_hook(struct spinlock *lock, uint32_t flags);
#define spin_unlock_irqrestore(lock, flags) unlock_hook(lock, flags)

#include "kvm/tdp.c"
#include "mm/notify.c"

#include "host_threads.h"

extern int printf(const char *format, ...);
extern int sched_yield(void);
extern void *malloc(unsigned long n);
extern void free(void *p);
extern int posix_memalign(void **out, unsigned long align, unsigned long n);

static int failures;

/** Count and print a failure when `condition` is false. */
#define CHECK(condition, detail)                                \
    do {                                                        \
        if (!(condition)) {                                     \
            printf("kvm-slots-hosttest: failed: %s\n", detail); \
            failures++;                                         \
        }                                                       \
    } while (0)

/* The code a row runs once, as another processor's change would run then:
 * at the fault's next take of the regions' lock to read, at its next read
 * of a caller's entry under it (`mmu_user_table`), and at its next wait
 * (`arch_relax`). */
static void (*at_read_lock)(void);
static void (*at_entry)(void);
static void (*at_relax)(void);

/** Run `*hook` once, if a row set it. */
static void run_once(void (**hook)(void))
{
    void (*run)(void) = *hook;

    if (run) {
        *hook = NULL;
        run();
    }
}

static process_t caller;
struct percpu cpu_table[NR_CPUS] = {{.current = &caller}};
unsigned int host_cpu;

/* ---- the heap and the frames, counted ---- */

static int live, frames;

void *kmalloc(size_t size)
{
    live++;
    return malloc(size ? size : 1);
}

void *kzalloc(size_t size)
{
    void *p = kmalloc(size);

    k_memset(p, 0, size);
    return p;
}

void kfree(void *p)
{
    if (p) {
        live--;
        free(p);
    }
}

void *k_memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;

    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

void *k_memset(void *dst, int c, size_t n)
{
    uint8_t *d = dst;

    while (n--) {
        *d++ = (uint8_t)c;
    }
    return dst;
}

int copy_to_user(void __user *dst, const void *src, size_t n)
{
    if (!dst) {
        return -EFAULT;
    }
    k_memcpy(dst, src, n);
    return 0;
}

int copy_from_user(void *dst, const void __user *src, size_t n)
{
    if (!src) {
        return -EFAULT;
    }
    k_memcpy(dst, src, n);
    return 0;
}

/** The physical address of the host memory at `p`, by the direct map's
 * formula, which `phys_to_virt` turns back. */
static uintptr_t phys_of(const void *p)
{
    return (uintptr_t)p - (uintptr_t)KVA_DIRECT;
}

/** A table frame for tdp.c, page-aligned host memory. */
uintptr_t alloc_frame(void)
{
    void *p = NULL;

    if (posix_memalign(&p, 4096, 4096) != 0) {
        return 0;
    }
    frames++;
    return phys_of(p);
}

void free_frame(uintptr_t phys)
{
    frames--;
    free(phys_to_virt(phys));
}

/* A device's frames are below 1 TiB; the host's memory is far above. */
#define DEV_BASE 0xE0000000ull

int paging_frame_is_device(uintptr_t phys)
{
    return phys < (1ull << 40);
}

/* The host's physical width, 39 bits, the Lenovo's Haswell's. */
void kcpu_id(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    *a = *b = *c = *d = 0;
    if (leaf == 0x80000000u) {
        *a = 0x80000008u;
    } else if (leaf == 0x80000008u) {
        *a = 39;
    }
}

/* ---- the backend: EPT's format ---- */

/** EPT's memory type in bits 5:3 and its ignore-PAT bit 6. */
static uint64_t ept_cache(int type, int ignore_guest_pat)
{
    return ((uint64_t)type << 3) | (ignore_guest_pat ? 0x40u : 0u);
}

static const struct kvm_tdp_format ept = {
    7, 1, 2, 4, 0, 0, 0x80, 0x000FFFFFFFFFF000ull, 0x100, 0x200, ept_cache};
static const struct kvm_backend fake = {"fake", NULL, NULL, NULL, NULL, NULL, NULL,
                                        NULL,   NULL, NULL, NULL, NULL, &ept};
const struct kvm_backend *kvm_backend = &fake;

/** Resource accounting is reported by kvm.c, outside this memory model. */
void kvm_count(enum kvm_resource resource, int change)
{
    (void)resource;
    (void)change;
}

/* ---- the VM: two vCPUs, outside the guest ---- */

static struct mm the_mm;
static struct kvm_vm vm;
static struct kvm_vcpu vcpu[2];
static struct kvm_vcpu *running_vcpu;

/** The row's running vCPU, or NULL when it models a VM ioctl's thread. */
struct kvm_vcpu *kvm_running_vcpu(struct kvm_vm *v)
{
    return running_vcpu && running_vcpu->vm == v ? running_vcpu : NULL;
}

/* The waits of tdp.c, counted. A wait past 1000 turns would never end in
 * a test of one thread: it takes every vCPU out of the guest and ends any
 * page change under way, so that the row fails on its count instead. */
static int relaxes;

/** A turn of a wait: the row's code, once, then the guard. */
void arch_relax(void)
{
    relaxes++;
    run_once(&at_relax);
    if (relaxes > 1000) {
        vcpu[0].mode = vcpu[1].mode = KVM_MODE_OUTSIDE;
        the_mm.changing = 0;
    }
}

struct mm *kvm_vm_mm(struct kvm_vm *v)
{
    (void)v;
    return &the_mm;
}

/* The calls of kvm_gpamap_zap, and the range the last one named. */
static int zaps;
static uint64_t zap_gfn, zap_npages;

/** Record the range whose guest page maps a slot's change takes back. */
void kvm_gpamap_zap(struct kvm_vm *vm, uint64_t gfn, uint64_t npages)
{
    (void)vm;
    zaps++;
    zap_gfn = gfn;
    zap_npages = npages;
}

/** Whether the calls since the last look were one zap of [gfn, gfn +
 * npages), clearing the record. */
static int zapped(uint64_t gfn, uint64_t npages)
{
    int one = zaps == 1 && zap_gfn == gfn && zap_npages == npages;

    zaps = 0;
    zap_gfn = zap_npages = 0;
    return one;
}

void kvm_vcpu_kick(struct kvm_vcpu *v, uint32_t request)
{
    v->requests |= request;
}

/** Whether every vCPU was asked to flush, clearing the requests. */
static int flushed(void)
{
    int all = 1;
    uint32_t i;

    for (i = 0; i < 2; i++) {
        all &= (vcpu[i].requests & KVM_REQ_TLB_FLUSH) != 0;
        vcpu[i].requests = 0;
    }
    return all;
}

/* ---- the caller's memory ---- */

/* The window of user addresses the model has, 8 leaf tables of 2 MiB. */
#define HVA    0x40000000ull
#define BLOCKS 8u
#define MIB2   0x200000ull

static pte_t host_pt[BLOCKS][512];
static int prot_w[BLOCKS][512];   /* the region allows a write */
static int unmapped[BLOCKS][512]; /* no region */
static uint8_t *ram;              /* 16 MiB, 2 MiB-aligned */
static uint32_t ram_next;         /* the next page `fresh` gives */

/** The leaf entry of `hva`, or NULL outside the window. */
static pte_t *host_entry(uintptr_t hva)
{
    if (hva < HVA || hva >= HVA + BLOCKS * MIB2) {
        return NULL;
    }
    return &host_pt[(hva - HVA) / MIB2][((hva - HVA) / 4096u) % 512u];
}

pte_t *mmu_user_table(page_dir_t *dir, uintptr_t va, int create)
{
    (void)dir;
    (void)create;
    run_once(&at_entry);
    if (va < HVA || va >= HVA + BLOCKS * MIB2) {
        return NULL;
    }
    return host_pt[(va - HVA) / MIB2];
}

/** 1: the test runs as the one thread, which holds the big lock. */
int bkl_held(void)
{
    return 1;
}

/** Nothing: the test runs under the big lock throughout. */
void bkl_enter(void)
{}

/** Nothing, as `bkl_enter`. */
void bkl_exit(void)
{}

/* The read holds of the caller's address space that the fault holds, and
 * those it took. */
static int mmap_reads, mmap_takes;

/** Take a read hold of `mm`: one thread, so nothing to wait for. */
void mmap_read_lock(struct mm *mm)
{
    (void)mm;
    mmap_reads++;
    mmap_takes++;
    run_once(&at_read_lock);
}

/** Give back a hold of `mm`; a release of no hold stops the test. */
void mmap_unlock(struct mm *mm)
{
    (void)mm;
    if (mmap_reads == 0) {
        __builtin_trap();
    }
    mmap_reads--;
}

/** Nothing waits on this processor's requests in a test of one thread. */
void smp_service(void)
{}

/* The slots' read side: the waits of a change for the sections that may
 * have the old index, which the rows count, two for each change, one for
 * each count, and the rest of a copy under way, which a row sets to run
 * while a change waits for it, as another thread's copy would end then. */
#define SYNC_WAITS 2
static int slot_syncs;
static void (*copy_rest)(void);

/** Make `wq` empty: the model keeps no sleepers. */
void wait_queue_init(wait_queue_t *wq)
{
    (void)wq;
}

/** A section's end wakes a waiting change; the model's change has already
 * run the rest it waits for. */
void wq_wake_all(wait_queue_t *wq)
{
    (void)wq;
}

/* While `gp_threads` is set, the threads of `gp_rows` run one at a time:
 * the one whose number `gp_turn` holds, 0 for the row's own, which plays
 * the readers, and 1 and 2 for the replacements, whose waits and takes of
 * a held mutex give the turn back to 0 (`gp_yield`). */
static int gp_threads;
static volatile int gp_turn;

/** Give the turn back to the row's thread and wait to be given it again,
 * as a sleeper of thread `me` does until it is woken. */
static void gp_yield(int me)
{
    __atomic_store_n(&gp_turn, 0, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&gp_turn, __ATOMIC_SEQ_CST) != me) {
        (void)sched_yield();
    }
}

/* The holders of a mutex: the thread of each turn. */
static int gp_holder[3];

/** Make `m` free. */
void mutex_init_key(struct mutex *m, const char *name, const struct lockdep_key *key)
{
    (void)name;
    (void)key;
    m->owner = NULL;
}

/** Take `m`: while `gp_rows` runs, a held one gives the turn back until it
 * is free, as a sleeper on it does; otherwise it must be free. */
void mutex_lock(struct mutex *m)
{
    int me = gp_threads ? __atomic_load_n(&gp_turn, __ATOMIC_SEQ_CST) : 0;

    while (m->owner) {
        if (!gp_threads || m->owner == &gp_holder[me]) {
            __builtin_trap();
        }
        gp_yield(me);
    }
    m->owner = &gp_holder[me];
}

/** Give `m` back; it must be held. */
void mutex_unlock(struct mutex *m)
{
    if (!m->owner) {
        __builtin_trap();
    }
    m->owner = NULL;
}

/** A change's wait for a count: the rest of the copy under way runs, and
 * must end its section; with none to run, the count must be 0. While
 * `gp_rows` runs, the replacement gives the turn back until the count is
 * 0. */
int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    void (*rest)(void) = copy_rest;

    (void)wq;
    if (gp_threads) {
        int me = __atomic_load_n(&gp_turn, __ATOMIC_SEQ_CST);

        while (!cond(arg)) {
            gp_yield(me);
        }
        return 0;
    }
    if (!cond(arg) && rest) {
        copy_rest = NULL;
        rest();
    }
    if (!cond(arg)) {
        __builtin_trap();
    }
    slot_syncs++;
    return 0;
}

/** Take `mm`'s page-table lock, around the notice list's changes: the
 * test has one thread, so a held lock is the same path twice. */
uint32_t mm_ptl_lock(struct mm *mm)
{
    if (mm->ptl.locked) {
        __builtin_trap();
    }
    mm->ptl.locked = 1;
    return 0;
}

/** Give back the page-table lock. */
void mm_ptl_unlock(struct mm *mm, uint32_t flags)
{
    (void)flags;
    mm->ptl.locked = 0;
}

/** Clear `bits` in the present model entry of `addr`. */
void paging_entry_clear(struct mm *mm, uintptr_t addr, pte_t bits)
{
    pte_t *table = mmu_user_table(mm->dir, addr, 0);

    if (table && (table[MMU_INDEX(addr)] & PTE_P)) {
        table[MMU_INDEX(addr)] &= ~bits;
    }
}

/** A page of RAM from the top half of `ram`, which the rows that need
 * aligned runs leave alone. */
static uintptr_t fresh(void)
{
    return phys_of(ram + 4u * MIB2 + (ram_next++ % 2048u) * 4096u);
}

static int prefaults;

/* The regions the model knows: while `secret_on` only `secret`, of
 * memfd_secret, and otherwise `plain`, the window's pages that have a
 * region (`unmapped`), readable and as writable as `prot_w` says. */
static vma_t secret;
static int secret_on;
static vma_t plain;

/** The region of `addr`: the secret one, a plain one of its page, or
 * none. */
vma_t *vm_find(vm_space_t *vm, uintptr_t addr)
{
    size_t b, i;

    (void)vm;
    if (secret_on) {
        return addr >= secret.start && addr < secret.end ? &secret : NULL;
    }
    if (addr < HVA || addr >= HVA + BLOCKS * MIB2) {
        return NULL;
    }
    b = (addr - HVA) / MIB2;
    i = ((addr - HVA) / 4096u) % 512u;
    if (unmapped[b][i]) {
        return NULL;
    }
    plain.start = addr & ~(uintptr_t)4095u;
    plain.end = plain.start + 4096u;
    plain.prot = VM_PROT_READ | (prot_w[b][i] ? VM_PROT_WRITE : 0u);
    return &plain;
}

/** The first region that ends above `addr`: the secret one, or a plain
 * one of the window from `addr`'s page, or none. */
vma_t *vm_lower(const vm_space_t *vm, uintptr_t addr)
{
    (void)vm;
    if (secret_on) {
        return secret.end > addr ? &secret : NULL;
    }
    if (addr >= HVA + BLOCKS * MIB2) {
        return NULL;
    }
    plain.start = addr < HVA ? HVA : addr & ~(uintptr_t)4095u;
    plain.end = HVA + BLOCKS * MIB2;
    plain.prot = VM_PROT_READ | VM_PROT_WRITE;
    return &plain;
}

/** The region after `r`: none, since the model has one. */
vma_t *vm_next(const vma_t *r)
{
    (void)r;
    return NULL;
}

/** A page brought in as a fault would: a fresh frame, writable where the
 * region is, and for a write to a copy-on-write page a new frame, told to
 * the notice first and counted under way from before the notice to after
 * the new entry, as paging.c's break counts itself (`mmn_change_begin`). */
int vm_prefault_range(uintptr_t addr, uintptr_t len, int write)
{
    pte_t *e = host_entry(addr);
    size_t b = (addr - HVA) / MIB2, i = ((addr - HVA) / 4096u) % 512u;

    (void)len;
    prefaults++;
    if (!e || unmapped[b][i] || (write && !prot_w[b][i])) {
        return 0;
    }
    if (!(*e & PTE_P)) {
        *e = fresh() | PTE_P | PTE_U | (prot_w[b][i] ? PTE_W : 0);
    } else if (write && !(*e & PTE_W)) {
        mmn_change_begin(&the_mm);
        mmn_notify(&the_mm, MMN_COW, addr & ~4095ul, (addr & ~4095ul) + 4096u);
        *e = fresh() | PTE_P | PTE_U | PTE_W;
        mmn_change_end(&the_mm);
    }
    return 1;
}

/** Map [hva, hva + n pages) as a region, writable for `w`, nothing
 * present. */
static void region(uintptr_t hva, uint32_t n, int w)
{
    uint32_t k;

    for (k = 0; k < n; k++) {
        uintptr_t a = hva + k * 4096u;
        size_t b = (a - HVA) / MIB2, i = ((a - HVA) / 4096u) % 512u;

        host_pt[b][i] = 0;
        prot_w[b][i] = w;
        unmapped[b][i] = 0;
    }
}

/** Make the 512 pages of the block at `hva` present over the 2 MiB of
 * frames from `phys`, writable. */
static void fill_block(uintptr_t hva, uint64_t phys)
{
    uint32_t i;

    region(hva, 512, 1);
    for (i = 0; i < 512; i++) {
        *host_entry(hva + i * 4096u) = (phys + i * 4096u) | PTE_P | PTE_U | PTE_W;
    }
}

/* ---- the guest-physical tables, read back ---- */

/** The leaf of `gfn` and its level, or 0. */
static uint64_t leaf_of(uint64_t gfn, int *level)
{
    int l = 0;
    uint64_t *e = find_leaf((struct tdp *)vm.tdp, gfn, &l);

    *level = l;
    return e ? *e : 0;
}

/** Set a slot: KVM_SET_USER_MEMORY_REGION's answer. */
static int32_t set(uint32_t slot, uint32_t flags, uint64_t gpa, uint64_t size, uint64_t hva)
{
    struct kvm_userspace_memory_region r;

    r.slot = slot;
    r.flags = flags;
    r.guest_phys_addr = gpa;
    r.memory_size = size;
    r.userspace_addr = hva;
    return kvm_slots_set(&vm, &r);
}

/* ---- the rows ---- */

/** The ABI's sizes and numbers, and the capabilities of slots.c. */
static void capability_rows(void)
{
    CHECK(sizeof(struct kvm_dirty_log) == 16 && sizeof(struct kvm_clear_dirty_log) == 24,
          "the dirty log's structures, Linux's sizes");
    CHECK(KVM_GET_DIRTY_LOG == 0x4010AE42u && KVM_CLEAR_DIRTY_LOG == 0xC018AEC0u,
          "KVM_GET_DIRTY_LOG and KVM_CLEAR_DIRTY_LOG");
    CHECK(kvm_slots_check_extension(KVM_CAP_USER_MEMORY) == 1, "USER_MEMORY");
    CHECK(kvm_slots_check_extension(KVM_CAP_USER_MEMORY2) == 1, "USER_MEMORY2");
    CHECK(kvm_slots_check_extension(KVM_CAP_READONLY_MEM) == 1, "READONLY_MEM");
    CHECK(kvm_slots_check_extension(KVM_CAP_SYNC_MMU) == 1, "SYNC_MMU");
    CHECK(kvm_slots_check_extension(KVM_CAP_NR_MEMSLOTS) == 32764, "NR_MEMSLOTS, Linux's on x86");
    CHECK(kvm_slots_check_extension(KVM_CAP_MANUAL_DIRTY_LOG_PROTECT2) == 3,
          "MANUAL_DIRTY_LOG_PROTECT2");
    CHECK(kvm_slots_check_extension(KVM_CAP_IRQFD) == 0, "a capability not the slots'");
}

/** KVM_SET_USER_MEMORY_REGION's checks and answers. */
static void region_rows(void)
{
    int syncs;

    CHECK(set(0, 4, 0, 0x10000, HVA) == -EINVAL, "an unknown flag");
    CHECK(set(0, 0, 0, 0x10001, HVA) == -EINVAL, "a size not page-aligned");
    CHECK(set(0, 0, 0x800, 0x10000, HVA) == -EINVAL, "an address not page-aligned");
    CHECK(set(0, 0, 0, 0x10000, HVA + 8) == -EINVAL, "a caller's address not page-aligned");
    CHECK(set(0, 0, 0, 0x2000, USER_ADDR_MAX - 0x1000) == -EINVAL, "a range past user space");
    CHECK(set(0x10000, 0, 0, 0x10000, HVA) == -EINVAL, "address space 1");
    CHECK(set(32764, 0, 0, 0x10000, HVA) == -EINVAL, "slot 32764");
    CHECK(set(0, 0, 0xFFFFFFFFFFFFF000ull, 0x2000, HVA) == -EINVAL, "a range that wraps");
    CHECK(set(0, 0, 1ull << 39, 0x1000, HVA) == -EINVAL, "a range past the physical width");
    CHECK(set(0, 0, 0, 0, HVA) == -EINVAL, "the deletion of no slot");
    CHECK(set(0, 0, 0x100000, 0x10000, HVA) == 0, "a new slot");
    CHECK(set(1, 0, 0x108000, 0x10000, HVA + 0x10000) == -EEXIST, "an overlap");
    CHECK(set(1, 0, 0x110000, 0x10000, HVA) == 0, "a neighbour, aliasing the same memory");
    CHECK(set(0, 0, 0x100000, 0x10000, HVA + 0x1000) == -EINVAL, "a change of address");
    CHECK(set(0, 0, 0x100000, 0x20000, HVA) == -EINVAL, "a change of size");
    CHECK(set(0, KVM_MEM_READONLY, 0x100000, 0x10000, HVA) == -EINVAL, "a change of READONLY");
    zapped(0, 0);
    CHECK(set(0, 0, 0x100000, 0x10000, HVA) == 0 && zaps == 0, "no change, and no map taken back");
    CHECK(set(0, 0, 0x118000, 0x10000, HVA) == -EEXIST && zaps == 0, "a move into another slot");
    syncs = slot_syncs;
    CHECK(set(0, 0, 0x200000, 0x10000, HVA) == 0 && slot_syncs == syncs + SYNC_WAITS,
          "a move, which waits for the copies through the slot");
    CHECK(zapped(0x100, 16), "a move takes back the guest page maps over the slot's old frames");
    CHECK(set(0, 0, 1ull << 39, 0x10000, HVA) == -EINVAL, "a move past the physical width");
    CHECK(set(0, 0, 0, 0, 0) == 0 && zapped(0x200, 16) && slot_syncs == syncs + 2 * SYNC_WAITS,
          "a deletion takes back the guest page maps over the slot and waits for the copies");
    CHECK(set(1, 0, 0, 0, 0) == 0 && zapped(0x110, 16), "the other deletion");
    CHECK(((struct slots *)vm.slots)->pages == 0 && the_mm.notify == NULL,
          "nothing left of the slots");
}

/** Every slot number in use at once, made in an order apart from their
 * frames', found by frame and by number, moved, and deleted. Slot `i * P`
 * modulo the count, P a prime that does not divide it, holds the frame
 * 0x1000 + 2i, and the odd frames between are holes. */
static void many_rows(void)
{
    const uint32_t n = KVM_MEM_SLOTS, step = 7919u;
    struct kvm_slot_view v;
    uint32_t i, made = 0, found = 0, holes = 0, gone = 0;

    for (i = 0; i < n; i++) {
        made += set(i * step % n, 0, (0x1000u + 2u * i) * 4096u, 4096u, HVA) == 0;
    }
    CHECK(made == n, "every slot number in use");
    CHECK(set(n, 0, 0x100000000ull, 4096u, HVA) == -EINVAL, "no slot past the last");
    for (i = 0; i < n; i++) {
        found += kvm_slot_find(&vm, 0x1000u + 2u * i, &v) && v.id == i * step % n &&
                 v.base_gfn == 0x1000u + 2u * i;
        holes += !kvm_slot_find(&vm, 0x1001u + 2u * i, &v);
    }
    CHECK(found == n && holes == n, "each frame found in its slot, and no slot at a hole");
    CHECK(!kvm_slot_find(&vm, 0xFFF, &v) && kvm_slot_get(&vm, 5u * step % n, &v) &&
              v.base_gfn == 0x100A,
          "no slot below the first, and a slot found by number");
    CHECK(set(5u * step % n, 0, 0x100Cu * 4096u, 4096u, HVA) == -EEXIST,
          "a move onto the next slot");
    CHECK(set(5u * step % n, 0, 0x100Bu * 4096u, 4096u, HVA) == 0 &&
              kvm_slot_find(&vm, 0x100B, &v) && v.id == 5u * step % n &&
              !kvm_slot_find(&vm, 0x100A, &v),
          "a move into the hole past the next slot");
    CHECK(set(5u * step % n, 0, 0x100Au * 4096u, 2u * 4096u, HVA) == -EINVAL,
          "a change of a moved slot's size");
    for (i = 0; i < n; i += 2) {
        gone += set(i * step % n, 0, 0, 0, 0) == 0;
    }
    for (i = 0, found = 0, holes = 0; i < n; i++) {
        if (i % 2) {
            found += kvm_slot_find(&vm, 0x1000u + 2u * i + (i == 5), &v) && v.id == i * step % n;
        } else {
            holes +=
                !kvm_slot_find(&vm, 0x1000u + 2u * i, &v) && !kvm_slot_get(&vm, i * step % n, &v);
        }
    }
    CHECK(gone == n / 2 && found == n / 2 && holes == n / 2,
          "half the slots deleted, and the other half found");
    CHECK(set(0, 0, 0x1000u * 4096u, 3u * 4096u, HVA) == -EEXIST &&
              set(0, 0, 0x1000u * 4096u, 2u * 4096u, HVA) == 0,
          "a new slot over a deleted one's frames, and not over a live one's");
    CHECK(set(0, 0, 0, 0, 0) == 0, "its deletion");
    for (i = 1, gone = 0; i < n; i += 2) {
        gone += set(i * step % n, 0, 0, 0, 0) == 0;
    }
    CHECK(gone == n / 2 && ((struct slots *)vm.slots)->ids == NULL &&
              ((struct slots *)vm.slots)->gfns == NULL && ((struct slots *)vm.slots)->pages == 0 &&
              the_mm.notify == NULL,
          "nothing left of the slots");
}

/** A 4 KiB slot's faults and each change of the caller's memory. */
static void small_rows(void)
{
    uint64_t e, gfn = 0x100;
    int level;
    uintptr_t a = HVA;

    region(a, 16, 1);
    CHECK(set(0, 0, gfn * 4096u, 16 * 4096u, a) == 0, "a 4 KiB slot");
    CHECK(kvm_tdp_fault(&vm, gfn + 3, 0) == 0, "a read fault");
    e = leaf_of(gfn + 3, &level);
    CHECK(level == 1 && (e & ept.addr) == PTE_ADDR(*host_entry(a + 3 * 4096u)),
          "a 4 KiB leaf on the caller's frame");
    CHECK((e & 7) == 7 && (e & 0x78) == 0x70 && (e & 0x80) == 0,
          "readable, writable, executable, write-back with the guest's PAT ignored");
    CHECK(kvm_tdp_fault(&vm, 0x50, 0) == 1, "a frame with no slot is MMIO");
    CHECK(vm.stat[KVM_VM_STAT_PAGES_4K] == 1 && vm.stat[KVM_VM_STAT_PAGES_2M] == 0,
          "pages_4k counts the leaf");

    /* A device that does not snoop: RAM honours the guest's PAT while one
     * is assigned, and the first and the last drop every leaf. */
    flushed();
    kvm_tdp_noncoherent(&vm, 1);
    CHECK(leaf_of(gfn + 3, &level) == 0 && flushed(), "the first such device drops the leaves");
    CHECK(kvm_tdp_fault(&vm, gfn + 3, 0) == 0 && (leaf_of(gfn + 3, &level) & 0x78) == 0x30,
          "RAM is write-back with the guest's PAT in charge while it is assigned");
    kvm_tdp_noncoherent(&vm, 1);
    CHECK(leaf_of(gfn + 3, &level) != 0 && !flushed(), "a second drops nothing");
    kvm_tdp_noncoherent(&vm, 0);
    CHECK(leaf_of(gfn + 3, &level) != 0 && !flushed(), "nor does taking one of two away");
    kvm_tdp_noncoherent(&vm, 0);
    CHECK(leaf_of(gfn + 3, &level) == 0 && flushed() && vm.noncoherent == 0,
          "the last one taken away drops the leaves");
    CHECK(kvm_tdp_fault(&vm, gfn + 3, 0) == 0 && (leaf_of(gfn + 3, &level) & 0x78) == 0x70,
          "and RAM is write-back with the guest's PAT ignored again");

    /* A copy-on-write page: read-only, then a write gives a new frame. */
    *host_entry(a + 5 * 4096u) = fresh() | PTE_P | PTE_U;
    CHECK(kvm_tdp_fault(&vm, gfn + 5, 0) == 0 && (leaf_of(gfn + 5, &level) & 2) == 0,
          "read-only over a copy-on-write page");
    flushed();
    CHECK(kvm_tdp_fault(&vm, gfn + 5, 1) == 0, "the write that breaks it");
    e = leaf_of(gfn + 5, &level);
    CHECK((e & 2) && (e & ept.addr) == PTE_ADDR(*host_entry(a + 5 * 4096u)),
          "writable on the new frame");
    CHECK(flushed(), "the break flushed the old frame");

    /* munmap: the notice, then the tables. */
    flushed();
    mmn_notify(&the_mm, MMN_UNMAP, a + 3 * 4096u, a + 4 * 4096u);
    *host_entry(a + 3 * 4096u) = 0;
    unmapped[0][3] = 1;
    CHECK(leaf_of(gfn + 3, &level) == 0 && flushed(), "munmap dropped the leaf and flushed");
    CHECK(leaf_of(gfn + 5, &level) != 0, "and no other");
    CHECK(kvm_tdp_fault(&vm, gfn + 3, 0) == -EFAULT, "a fault on an unmapped page");
    unmapped[0][3] = 0;

    /* mprotect, madvise and a truncation. */
    CHECK(kvm_tdp_fault(&vm, gfn + 6, 1) == 0, "a write fault");
    mmn_notify(&the_mm, MMN_PROTECT, a + 6 * 4096u, a + 7 * 4096u);
    *host_entry(a + 6 * 4096u) &= ~(pte_t)PTE_W;
    prot_w[0][6] = 0;
    CHECK(leaf_of(gfn + 6, &level) == 0 && flushed(), "mprotect dropped the leaf");
    CHECK(kvm_tdp_fault(&vm, gfn + 6, 1) == -EFAULT, "a write to a read-only page");
    CHECK(kvm_tdp_fault(&vm, gfn + 6, 0) == 0 && !(leaf_of(gfn + 6, &level) & 2),
          "a read maps it read-only");
    mmn_notify(&the_mm, MMN_DROP, a, a + 16 * 4096u);
    CHECK(leaf_of(gfn + 5, &level) == 0 && leaf_of(gfn + 6, &level) == 0 && flushed(),
          "madvise dropped every leaf");
    CHECK(kvm_tdp_fault(&vm, gfn + 7, 0) == 0, "a fault again");
    mmn_notify(&the_mm, MMN_TRUNCATE, a + 7 * 4096u, a + 8 * 4096u);
    CHECK(leaf_of(gfn + 7, &level) == 0 && flushed(), "a truncation dropped the leaf");
    mmn_notify(&the_mm, MMN_UNMAP, a + 7 * 4096u, a + 8 * 4096u);
    CHECK(!flushed(), "no flush when nothing was mapped");
    CHECK(set(0, 0, 0, 0, 0) == 0, "deleted");
    CHECK(vm.stat[KVM_VM_STAT_PAGES_4K] == 0, "pages_4k is 0 with every leaf dropped");
}

/** A read-only slot. */
static void readonly_rows(void)
{
    int level;
    uintptr_t a = HVA + 0x20000;

    region(a, 4, 1);
    CHECK(set(2, KVM_MEM_READONLY, 0x300000, 4 * 4096u, a) == 0, "a read-only slot");
    CHECK(kvm_tdp_fault(&vm, 0x300, 1) == 1, "a write to it is MMIO");
    CHECK(kvm_tdp_fault(&vm, 0x300, 0) == 0 && !(leaf_of(0x300, &level) & 2),
          "a read maps it read-only");
    CHECK(set(2, 0, 0, 0, 0) == 0 && leaf_of(0x300, &level) == 0 && flushed(),
          "its deletion dropped the leaf");
}

/** 2 MiB leaves, and the 4 KiB ones where a 2 MiB leaf cannot be. */
static void large_rows(void)
{
    uintptr_t a = HVA + 2 * MIB2;
    uint64_t e, block = 0x400000 / 4096u;
    int level;

    /* 2 MiB of contiguous, aligned RAM, at an aligned guest address. */
    fill_block(a, phys_of(ram));
    CHECK(set(3, 0, 0x400000, 2 * MIB2, a) == 0, "a 4 MiB slot");
    CHECK(kvm_tdp_fault(&vm, block + 17, 0) == 0, "a fault in its first 2 MiB");
    e = leaf_of(block + 300, &level);
    CHECK(level == 2 && (e & 0x80) && (e & ept.addr) == phys_of(ram) && (e & 2),
          "a writable 2 MiB leaf on the run");
    CHECK(vm.stat[KVM_VM_STAT_PAGES_2M] == 1 && vm.stat[KVM_VM_STAT_PAGES_4K] == 0,
          "pages_2m counts it");

    /* One page of the run goes: the leaf goes whole. */
    flushed();
    mmn_notify(&the_mm, MMN_UNMAP, a + 9 * 4096u, a + 10 * 4096u);
    CHECK(leaf_of(block + 300, &level) == 0 && flushed(), "a change to one page drops it");
    CHECK(vm.stat[KVM_VM_STAT_PAGES_2M] == 0, "and pages_2m with it");
    *host_entry(a + 9 * 4096u) = fresh() | PTE_P | PTE_U | PTE_W;
    CHECK(kvm_tdp_fault(&vm, block + 17, 0) == 0 && leaf_of(block + 17, &level) != 0 &&
              level == 1 && leaf_of(block + 300, &level) == 0,
          "with a frame out of the run, 4 KiB leaves");

    /* The second 2 MiB is sparse: 4 KiB leaves. */
    region(a + MIB2, 512, 1);
    CHECK(kvm_tdp_fault(&vm, block + 512 + 1, 1) == 0 && leaf_of(block + 513, &level) != 0 &&
              level == 1,
          "a sparse block maps 4 KiB");
    CHECK(set(3, 0, 0, 0, 0) == 0 && ((struct tdp *)vm.tdp)->tables == 1,
          "the deletion freed every table below the top");

    /* The guest address not congruent with the caller's: 4 KiB leaves. */
    fill_block(a, phys_of(ram));
    CHECK(set(3, 0, 0x401000, MIB2, a) == 0, "a slot one page off");
    CHECK(kvm_tdp_fault(&vm, 0x401, 0) == 0 && leaf_of(0x401, &level) != 0 && level == 1,
          "4 KiB leaves off the boundary");
    CHECK(set(3, 0, 0, 0, 0) == 0, "deleted");

    /* A slot shorter than the block: 4 KiB leaves. */
    CHECK(set(3, 0, 0x400000, MIB2 - 4096u, a) == 0, "a slot short of 2 MiB");
    CHECK(kvm_tdp_fault(&vm, block, 0) == 0 && leaf_of(block, &level) != 0 && level == 1,
          "4 KiB leaves in a short slot");
    CHECK(set(3, 0, 0, 0, 0) == 0, "deleted");

    /* A read-only page in the run: a read maps 2 MiB read-only, a write
     * after it 4 KiB leaves. */
    fill_block(a, phys_of(ram));
    *host_entry(a + 4 * 4096u) &= ~(pte_t)PTE_W;
    CHECK(set(3, 0, 0x400000, MIB2, a) == 0, "a slot with a read-only page");
    CHECK(kvm_tdp_fault(&vm, block + 1, 0) == 0 && leaf_of(block + 1, &level) != 0 && level == 2 &&
              !(leaf_of(block + 1, &level) & 2),
          "a read-only 2 MiB leaf");
    flushed();
    CHECK(kvm_tdp_fault(&vm, block + 1, 1) == 0, "a write fault in it");
    e = leaf_of(block + 1, &level);
    CHECK(level == 1 && (e & 2) && flushed(), "split into a writable 4 KiB leaf, flushed");
    CHECK(vm.stat[KVM_VM_STAT_PAGES_2M] == 0 && vm.stat[KVM_VM_STAT_PAGES_4K] == 1,
          "the split moves the count from pages_2m to pages_4k");
    CHECK(vm.stat[KVM_VM_STAT_REMOTE_TLB_FLUSH_REQUESTS] != 0, "the flushes are counted");
    CHECK(set(3, 0, 0, 0, 0) == 0, "deleted");
    CHECK(vm.stat[KVM_VM_STAT_PAGES_2M] == 0 && vm.stat[KVM_VM_STAT_PAGES_4K] == 0,
          "and no leaf is counted with the slots gone");
}

/** memfd_secret's pages, refused at their own fault and kept out of a
 * 2 MiB leaf that a fault on an ordinary page of their block would make. */
static void secret_rows(void)
{
    uintptr_t a = HVA + 2 * MIB2;
    uint64_t block = 0x400000 / 4096u;
    int level;

    fill_block(a, phys_of(ram));
    secret.start = a + 100 * 4096u;
    secret.end = a + 101 * 4096u;
    secret.prot = VM_PROT_READ | VM_PROT_WRITE | VM_PROT_SECRET;
    secret_on = 1;
    CHECK(set(3, 0, 0x400000, MIB2, a) == 0, "a slot over a block with a page of memfd_secret");
    CHECK(kvm_tdp_fault(&vm, block + 100, 0) == -EFAULT && leaf_of(block + 100, &level) == 0,
          "a fault on the secret page is -EFAULT");
    CHECK(kvm_tdp_fault(&vm, block + 17, 0) == 0 && leaf_of(block + 17, &level) != 0 &&
              level == 1 && leaf_of(block + 100, &level) == 0,
          "a fault on another page of the block maps 4 KiB, not the secret page with it");
    CHECK(set(3, 0, 0, 0, 0) == 0, "deleted");
    secret_on = 0;
}

/** A page that stands for the zero page (PTE_ZEROFILL): mapped read-only
 * on a read, with its mark kept, and writable on a write, with the mark
 * gone, alone and in a 2 MiB leaf. */
static void zero_rows(void)
{
    uintptr_t a = HVA + 2 * MIB2;
    uint64_t block = 0x400000 / 4096u, e;
    int level;

    fill_block(a, phys_of(ram));
    *host_entry(a + 5 * 4096u) |= PTE_ZEROFILL;
    CHECK(set(3, 0, 0x400000, MIB2, a) == 0, "a slot over a block with a zero page");
    CHECK(kvm_tdp_fault(&vm, block + 1, 0) == 0 && (e = leaf_of(block + 1, &level)) != 0 &&
              level == 2 && !(e & 2) && (*host_entry(a + 5 * 4096u) & PTE_ZEROFILL),
          "a read maps the block read-only and keeps the mark");
    CHECK(kvm_tdp_fault(&vm, block + 1, 1) == 0 && (e = leaf_of(block + 1, &level)) != 0 &&
              level == 2 && (e & 2) && !(*host_entry(a + 5 * 4096u) & PTE_ZEROFILL),
          "a write maps it writable and ends the mark");
    CHECK(set(3, 0, 0, 0, 0) == 0, "deleted");

    region(a, 512, 1);
    *host_entry(a + 7 * 4096u) = fresh() | PTE_P | PTE_U | PTE_W | PTE_ZEROFILL;
    CHECK(set(3, 0, 0x400000, MIB2, a) == 0, "a slot over a sparse block");
    CHECK(kvm_tdp_fault(&vm, block + 7, 0) == 0 && (e = leaf_of(block + 7, &level)) != 0 &&
              level == 1 && !(e & 2) && (*host_entry(a + 7 * 4096u) & PTE_ZEROFILL),
          "a read maps a lone zero page read-only and keeps the mark");
    CHECK(kvm_tdp_fault(&vm, block + 7, 1) == 0 && (e = leaf_of(block + 7, &level)) != 0 &&
              level == 1 && (e & 2) && !(*host_entry(a + 7 * 4096u) & PTE_ZEROFILL),
          "a write maps it writable and ends the mark");
    CHECK(set(3, 0, 0, 0, 0) == 0, "deleted");
}

/* Whether the caller has a signal pending. */
static int sig_pending;

int signal_pending(void)
{
    return sig_pending;
}

/** KVM_PRE_FAULT_MEMORY of `size` bytes at `gpa`: the result, with what is
 * left of the range in `*left` and where it begins in `*at`. */
static int32_t pre_fault(uint64_t gpa, uint64_t size, uint64_t *at, uint64_t *left)
{
    struct kvm_pre_fault_memory range;
    int32_t r;

    k_memset(&range, 0, sizeof(range));
    range.gpa = gpa;
    range.size = size;
    r = kvm_tdp_pre_fault(&vm, &range);
    *at = range.gpa;
    *left = range.size;
    return r;
}

/** KVM_PRE_FAULT_MEMORY over a 2 MiB leaf, 4 KiB leaves and a slot's end,
 * as pre_fault_memory_test asks. */
static void pre_fault_rows(void)
{
    uintptr_t a = HVA + 2 * MIB2;
    uint64_t gpa = 0x400000, at, left;
    struct kvm_pre_fault_memory range;
    int level;

    fill_block(a, phys_of(ram));
    region(a + MIB2, 1, 1);
    CHECK(set(3, 0, gpa, MIB2 + 4096u, a) == 0, "a slot of 2 MiB and a page");
    CHECK(pre_fault(gpa, MIB2, &at, &left) == 0 && left == 0 && at == gpa + MIB2 &&
              leaf_of(gpa / 4096u + 300, &level) != 0 && level == 2 &&
              vm.stat[KVM_VM_STAT_PAGES_2M] == 1 && vm.stat[KVM_VM_STAT_PAGES_4K] == 0,
          "2 MiB mapped whole by one leaf");
    CHECK(pre_fault(gpa + 0x5000, 0x3000, &at, &left) == 0 && left == 0 && at == gpa + 0x8000,
          "a range inside the leaf, mapped already");
    CHECK(pre_fault(gpa + MIB2, 2 * 4096u, &at, &left) == 0 && left == 4096u &&
              at == gpa + MIB2 + 4096u && leaf_of((gpa + MIB2) / 4096u, &level) != 0 && level == 1,
          "a range past the slot's end is 0 with its last page left");
    CHECK(pre_fault(at, left, &at, &left) == -ENOENT && left == 4096u && at == gpa + MIB2 + 4096u,
          "and -ENOENT, unmoved, when its first page has no slot");
    CHECK(pre_fault(gpa + MIB2 + 4096u, 4096u, &at, &left) == -ENOENT && left == 4096u,
          "a page no slot holds");
    CHECK(pre_fault(gpa + 1, 4096u, &at, &left) == -EINVAL &&
              pre_fault(gpa, 4097u, &at, &left) == -EINVAL &&
              pre_fault(gpa, 0, &at, &left) == -EINVAL &&
              pre_fault(~0xFFFull, 0x2000, &at, &left) == -EINVAL,
          "a range not of whole pages, empty or wrapping is -EINVAL");
    k_memset(&range, 0, sizeof(range));
    range.gpa = gpa;
    range.size = 4096u;
    range.flags = 1;
    CHECK(kvm_tdp_pre_fault(&vm, &range) == -EINVAL, "a flag is -EINVAL");
    range.flags = 0;
    range.padding[0] = 1;
    CHECK(kvm_tdp_pre_fault(&vm, &range) == 0 && range.size == 0, "the padding is not read");
    sig_pending = 1;
    CHECK(pre_fault(gpa, MIB2, &at, &left) == -EINTR && left == MIB2 && at == gpa,
          "a signal pending is -EINTR with nothing mapped");
    sig_pending = 0;
    CHECK(set(3, 0, 0, 0, 0) == 0 && vm.stat[KVM_VM_STAT_PAGES_2M] == 0 &&
              vm.stat[KVM_VM_STAT_PAGES_4K] == 0,
          "deleted");
}

/** A slot over a device's frames. */
static void device_rows(void)
{
    uintptr_t a = HVA + 4 * MIB2;
    uint64_t e;
    int level;

    fill_block(a, DEV_BASE);
    CHECK(set(4, 0, 0xE0000000ull, MIB2, a) == 0, "a slot over a device's mapping");
    CHECK(kvm_tdp_fault(&vm, 0xE0000, 1) == 0, "a write fault in it");
    e = leaf_of(0xE0000, &level);
    CHECK(level == 2 && (e & ept.addr) == DEV_BASE && (e & 0x78) == 0,
          "a 2 MiB leaf, uncached with the guest's PAT in charge");
    CHECK(set(4, 0, 0, 0, 0) == 0, "deleted");
    CHECK(set(4, 0, 0xE0001000ull, 4096u, a + 4096u) == 0, "a one-page device slot");
    CHECK(kvm_tdp_fault(&vm, 0xE0001, 0) == 0, "a fault in it");
    e = leaf_of(0xE0001, &level);
    CHECK(level == 1 && (e & ept.addr) == DEV_BASE + 4096u && (e & 0x78) == 0,
          "a 4 KiB uncached leaf");
    CHECK(set(4, 0, 0, 0, 0) == 0, "deleted");
}

/** KVM_GET_DIRTY_LOG of `slot` into `bits`. */
static int32_t get_dirty(uint32_t slot, uint64_t *bits)
{
    struct kvm_dirty_log d;

    d.slot = slot;
    d.padding1 = 0;
    d.dirty_bitmap = (uint64_t)(uintptr_t)bits;
    return kvm_dirty_ioctl(&vm, KVM_GET_DIRTY_LOG, (uintptr_t)&d);
}

/** KVM_CLEAR_DIRTY_LOG of `n` pages of `slot` from `first`, by `bits`. */
static int32_t clear_dirty(uint32_t slot, uint64_t first, uint32_t n, uint64_t *bits)
{
    struct kvm_clear_dirty_log c;

    c.slot = slot;
    c.num_pages = n;
    c.first_page = first;
    c.dirty_bitmap = (uint64_t)(uintptr_t)bits;
    return kvm_dirty_ioctl(&vm, KVM_CLEAR_DIRTY_LOG, (uintptr_t)&c);
}

/* A kernel's write to guest memory under way: the view of the slot it
 * found and copied through, and the frame it wrote. */
static struct kvm_slot_view copy_view;
static uint64_t copy_gfn;

/* Its section's number. */
static uint32_t copy_idx;

/** The end of that write, as `gpa_rw_page` ends it: the mark in the slot it
 * copied through, then the section ended. */
static void copy_mark(void)
{
    kvm_dirty_mark(&vcpu[0], &copy_view, copy_gfn);
    kvm_srcu_read_unlock(&vm, copy_idx);
}

/** Begin a kernel's write to the frame `gfn`, as `gpa_rw_page` does, whose
 * mark and end run while the next change of the slots waits: 1 when a slot
 * holds the frame. */
static int copy_begin(uint64_t gfn)
{
    copy_idx = kvm_srcu_read_lock(&vm);
    copy_gfn = gfn;
    if (!kvm_slot_find_read(&vm, gfn, &copy_view)) {
        kvm_srcu_read_unlock(&vm, copy_idx);
        return 0;
    }
    copy_rest = copy_mark;
    return 1;
}

/**
 * A kernel's write that a move of its slot overtakes, and one that a
 * deletion does, in slot 5 at `gfn`, of 100 pages at `a`, which logs under
 * manual protection: each change waits for the write, whose mark goes to
 * the page it wrote, in the log the move keeps, and to a log the deletion
 * has not yet freed. Deletes the slot.
 */
static void overtaken_rows(uint64_t gfn, uintptr_t a)
{
    uint64_t bits[2];
    int syncs;

    bits[0] = ~0ull;
    bits[1] = ~0ull;
    CHECK(clear_dirty(5, 0, 64, bits) == 0 && clear_dirty(5, 64, 36, bits) == 0 &&
              get_dirty(5, bits) == 0 && bits[0] == 0 && bits[1] == 0,
          "the log cleared");
    syncs = slot_syncs;
    CHECK(copy_begin(gfn + 9), "a kernel's write finds its slot");
    CHECK(set(5, KVM_MEM_LOG_DIRTY_PAGES, (gfn + 0x100) * 4096u, 100 * 4096u, a) == 0 &&
              copy_rest == NULL && slot_syncs == syncs + SYNC_WAITS,
          "a move of the slot waits for the write");
    CHECK(get_dirty(5, bits) == 0 && bits[0] == 1ull << 9 && bits[1] == 0,
          "which is logged at the page it wrote");
    CHECK(copy_begin(gfn + 0x100 + 3), "a write at the slot's new place");
    CHECK(set(5, 0, 0, 0, 0) == 0 && copy_rest == NULL && slot_syncs == syncs + 2 * SYNC_WAITS,
          "deleted, once the write through it has ended");
}

/** The dirty log, GET and CLEAR, and manual protection. */
static void dirty_rows(void)
{
    uintptr_t a = HVA + 6 * MIB2;
    uint64_t bits[2], gfn = 0x500;
    struct kvm_enable_cap cap;
    int level;

    region(a, 100, 1);
    CHECK(set(5, KVM_MEM_LOG_DIRTY_PAGES, gfn * 4096u, 100 * 4096u, a) == 0, "a slot that logs");
    CHECK(kvm_tdp_fault(&vm, gfn + 2, 0) == 0 && !(leaf_of(gfn + 2, &level) & 2),
          "a read maps a clean page read-only");
    CHECK(kvm_tdp_fault(&vm, gfn + 70, 1) == 0 && (leaf_of(gfn + 70, &level) & 2),
          "a write maps it writable");
    CHECK(get_dirty(5, bits) == 0 && bits[0] == 0 && bits[1] == (1ull << 6),
          "the log names the page written");
    CHECK(!(leaf_of(gfn + 70, &level) & 2) && flushed(), "and the page is write-protected again");
    CHECK(get_dirty(5, bits) == 0 && bits[0] == 0 && bits[1] == 0, "the log was cleared");
    CHECK(get_dirty(6, bits) == -ENOENT, "no slot");
    CHECK(get_dirty(256, bits) == -ENOENT && get_dirty(32764, bits) == -EINVAL,
          "slot 256, which may be, and slot 32764, which may not");
    CHECK(get_dirty(0x10005, bits) == -EINVAL, "address space 1");

    /* Flags: the log off, then on, each change dropping the leaves. */
    CHECK(kvm_tdp_fault(&vm, gfn + 2, 1) == 0, "a write");
    CHECK(set(5, 0, gfn * 4096u, 100 * 4096u, a) == 0 && leaf_of(gfn + 2, &level) == 0 && flushed(),
          "logging off dropped the leaves");
    CHECK(get_dirty(5, bits) == -ENOENT, "no log once off");

    /* KVM_CAP_MANUAL_DIRTY_LOG_PROTECT2. */
    k_memset(&cap, 0, sizeof(cap));
    cap.cap = KVM_CAP_MANUAL_DIRTY_LOG_PROTECT2;
    cap.args[0] = KVM_DIRTY_LOG_INITIALLY_SET;
    CHECK(kvm_dirty_enable_manual(&vm, &cap) == -EINVAL, "INITIALLY_SET alone");
    cap.args[0] = 3;
    cap.flags = 1;
    CHECK(kvm_dirty_enable_manual(&vm, &cap) == -EINVAL, "a flag");
    cap.flags = 0;
    CHECK(kvm_dirty_enable_manual(&vm, &cap) == 0, "manual protection, initially set");
    CHECK(set(5, KVM_MEM_LOG_DIRTY_PAGES, gfn * 4096u, 100 * 4096u, a) == 0, "logging on");
    CHECK(get_dirty(5, bits) == 0 && bits[0] == ~0ull && bits[1] == (1ull << 36) - 1u,
          "every page set at first");
    CHECK(kvm_tdp_fault(&vm, gfn + 3, 0) == 0 && (leaf_of(gfn + 3, &level) & 2),
          "a page set is mapped writable on a read");
    CHECK(get_dirty(5, bits) == 0 && bits[0] == ~0ull, "GET leaves the log as it is");
    bits[0] = 0xF;
    CHECK(clear_dirty(5, 0, 64, bits) == 0 && !(leaf_of(gfn + 3, &level) & 2) && flushed(),
          "CLEAR write-protects what it clears");
    CHECK(get_dirty(5, bits) == 0 && bits[0] == ~0xFull, "and clears it");
    CHECK(clear_dirty(5, 1, 64, bits) == -EINVAL, "a first page not a multiple of 64");
    CHECK(clear_dirty(5, 0, 30, bits) == -EINVAL, "a short count not to the end");
    CHECK(clear_dirty(5, 64, 37, bits) == -EINVAL, "past the end");
    bits[0] = ~0ull;
    CHECK(clear_dirty(5, 64, 36, bits) == 0, "a short count to the end");
    CHECK(get_dirty(5, bits) == 0 && bits[1] == 0, "cleared, the bits past the end ignored");
    CHECK(clear_dirty(6, 0, 64, bits) == -ENOENT, "CLEAR of no slot");
    CHECK(kvm_tdp_fault(&vm, gfn + 64, 1) == 0, "a write after CLEAR");
    CHECK(kvm_slot_find(&vm, gfn + 65, &copy_view), "a kernel's write finds its slot");
    kvm_dirty_mark(&vcpu[0], &copy_view, gfn + 65);
    CHECK(get_dirty(5, bits) == 0 && bits[1] == 3, "is logged, and so is a kernel's write");
    overtaken_rows(gfn, a);
    vm.dirty_manual = 0;

    /* No 2 MiB leaf in a slot that logs. */
    fill_block(HVA + 2 * MIB2, phys_of(ram));
    CHECK(set(3, KVM_MEM_LOG_DIRTY_PAGES, 0x400000, MIB2, HVA + 2 * MIB2) == 0,
          "an aligned slot that logs");
    CHECK(kvm_tdp_fault(&vm, 0x400, 1) == 0 && leaf_of(0x400, &level) != 0 && level == 1,
          "4 KiB leaves");
    CHECK(set(3, 0, 0, 0, 0) == 0, "deleted");
}

/** kvm_tdp_write_nofault, the steal time's write from inside `schedule`:
 * it writes only a present, writable page of a slot of the caller's that
 * was already stored into, of RAM, within one page, while the tables'
 * sequence is the one the caller checked the region at, and marks the
 * slot's dirty log. */
static void write_nofault_rows(void)
{
    uintptr_t a = HVA + 5 * MIB2;
    uint64_t gfn = 0x600, gpa = gfn * 4096u, bits[2];
    uintptr_t page = fresh();
    uint8_t *mem = (uint8_t *)phys_to_virt(page);
    uint8_t buf[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    struct mm other;

    region(a, 5, 1);
    *host_entry(a) = page | PTE_P | PTE_U | PTE_W | PTE_DIRTY;
    *host_entry(a + 4096u) = fresh() | PTE_P | PTE_U | PTE_W;
    *host_entry(a + 2 * 4096u) = fresh() | PTE_P | PTE_U | PTE_DIRTY;
    *host_entry(a + 3 * 4096u) = DEV_BASE | PTE_P | PTE_U | PTE_W | PTE_DIRTY;
    k_memset(mem, 0, 4096u);

    CHECK(set(6, KVM_MEM_READONLY, gpa, 5 * 4096u, a) == 0, "a read-only slot to write");
    CHECK(kvm_tdp_write_nofault(&vm, gpa + 8, buf, 4, vm.mmu_seq) == 1 && mem[8] == 0,
          "a read-only slot is refused");
    CHECK(set(6, 0, 0, 0, 0) == 0, "deleted");
    CHECK(set(6, KVM_MEM_LOG_DIRTY_PAGES, gpa, 5 * 4096u, a) == 0, "a slot that logs, to write");
    CHECK(get_dirty(6, bits) == 0 && bits[0] == 0, "nothing logged yet");
    CHECK(kvm_tdp_write_nofault(&vm, gpa + 4096u, buf, 4, vm.mmu_seq) == 1,
          "a page not yet stored into is refused");
    CHECK(kvm_tdp_write_nofault(&vm, gpa + 2 * 4096u, buf, 4, vm.mmu_seq) == 1,
          "a read-only page is refused");
    CHECK(kvm_tdp_write_nofault(&vm, gpa + 3 * 4096u, buf, 4, vm.mmu_seq) == 1,
          "a device's frame is refused");
    CHECK(kvm_tdp_write_nofault(&vm, gpa + 4 * 4096u, buf, 4, vm.mmu_seq) == 1,
          "a page not present is refused");
    CHECK(kvm_tdp_write_nofault(&vm, gpa + 4090u, buf, 8, vm.mmu_seq) == 1 && mem[4090] == 0,
          "a write across a page is refused");
    CHECK(kvm_tdp_write_nofault(&vm, gpa + 8, buf, 0, vm.mmu_seq) == 1,
          "an empty write is refused");
    CHECK(kvm_tdp_write_nofault(&vm, 0x7000000ull, buf, 4, vm.mmu_seq) == 1,
          "an address of no slot is refused");
    CHECK(kvm_tdp_write_nofault(&vm, gpa + 8, buf, 4, vm.mmu_seq - 1) == 1 && mem[8] == 0,
          "a write whose caller checked the region before the tables last changed is refused, "
          "as a memfd_secret page found since would be");
    k_memset(&other, 0, sizeof(other));
    caller.mm = &other;
    CHECK(kvm_tdp_write_nofault(&vm, gpa + 8, buf, 4, vm.mmu_seq) == 1 && mem[8] == 0,
          "a thread of another address space is refused");
    caller.mm = &the_mm;
    CHECK(get_dirty(6, bits) == 0 && bits[0] == 0, "no refusal was logged");
    CHECK(kvm_tdp_write_nofault(&vm, gpa + 8, buf, 4, vm.mmu_seq) == 0 && mem[7] == 0 &&
              mem[8] == 1 && mem[11] == 4 && mem[12] == 0,
          "a stored-into page of RAM takes the write at its offset");
    CHECK(get_dirty(6, bits) == 0 && bits[0] == 1, "and the slot's log names the page");
    CHECK(set(6, 0, 0, 0, 0) == 0, "deleted");
}

/* The code a row runs once, the next time `mmu_lock` is let go, as
 * another vCPU's fault runs while a zap lets the lock go between two
 * batches; and what it found. */
static void (*between)(void);
static uint64_t between_gfn;
static uint32_t between_seq;
static int between_fault;

/** Let `lock` go with `flags`, then run `between` once if `lock` is
 * `mmu_lock`. */
static void unlock_hook(struct spinlock *lock, uint32_t flags)
{
    void (*run)(void) = between;

    (spin_unlock_irqrestore)(lock, flags);
    if (run && lock == &vm.mmu_lock) {
        between = NULL;
        run();
    }
}

/** A read fault at `between_gfn` between two batches, and `mmu_seq` then. */
static void fault_between(void)
{
    between_seq = vm.mmu_seq;
    between_fault = kvm_tdp_fault(&vm, between_gfn, 0);
}

/**
 * A zap in batches of TDP_ZAP_BATCH entries: a fault between two batches of
 * a move's zap, at a frame the zap already passed, finds the slot moved and
 * maps nothing there, as Linux takes the old slot out of use before it
 * zaps it (virt/kvm/kvm_main.c:1791-1808); a zap of every frame visits the
 * entries that are there and passes absent subtrees whole, as tdp_iter
 * (arch/x86/kvm/mmu/tdp_iter.c:68, 110-122), so its batches follow the
 * tables, not the frames. Each batch after a break walks down again from
 * the root, as tdp_iter_restart (tdp_iter.c:23-33), and spends up to
 * TDP_LEVELS - 1 entries on the path above the one it stopped at, so the
 * bound counts only the entries a batch has left for new ones.
 */
static void zap_batch_rows(void)
{
    uintptr_t a = HVA + 3 * MIB2;
    uint64_t gfn = 0x900, i;
    uint32_t seq;
    int level, ok = 1;

    region(a, 64, 1);
    CHECK(set(4, 0, gfn * 4096u, 64 * 4096u, a) == 0, "a slot of 64 pages");
    for (i = 0; i < 64; i++) {
        ok = ok && kvm_tdp_fault(&vm, gfn + i, 0) == 0;
    }
    CHECK(ok, "each page mapped, more leaves than a batch visits");
    between = fault_between;
    between_gfn = gfn + 1;
    seq = vm.mmu_seq;
    CHECK(set(4, 0, (gfn + 0x100) * 4096u, 64 * 4096u, a) == 0 && between == NULL &&
              between_seq == seq + 1 && vm.mmu_seq > between_seq,
          "a move's zap lets mmu_lock go between two batches, moving mmu_seq after");
    CHECK(between_fault == 1, "a fault there at a frame the zap passed finds no slot");
    for (i = 0; i < 64; i++) {
        ok = ok && leaf_of(gfn + i, &level) == 0;
    }
    CHECK(ok && flushed(), "and no leaf of the old frames is left");
    CHECK(kvm_tdp_fault(&vm, gfn + 0x101, 0) == 0, "a fault at the new place maps");
    seq = vm.mmu_seq;
    kvm_tdp_zap(&vm, 0, kvm_tdp_frames());
    CHECK(vm.mmu_seq - seq <= (1u + 3u * TDP_ENTRIES) / (TDP_ZAP_BATCH - (TDP_LEVELS - 1)) + 1u &&
              leaf_of(gfn + 0x101, &level) == 0 && ((struct tdp *)vm.tdp)->tables == 1,
          "a zap of every frame over one path of tables takes batches by the entries");
    seq = vm.mmu_seq;
    kvm_tdp_zap(&vm, 0, kvm_tdp_frames());
    CHECK(vm.mmu_seq - seq == 1, "and over no table, one batch");
    CHECK(set(4, 0, 0, 0, 0) == 0, "deleted");
}

static void change_ended(void);
static void take_flush(void);

/** vCPU 0 enters the guest again and takes its flush request first, as the
 * backend's entry drops its translations. */
static void take_flush(void)
{
    vcpu[0].requests &= ~KVM_REQ_TLB_FLUSH;
}

/* The page of the stale-fault rows, its frame, and what a row's code
 * found of its leaf at the fault's next take of the regions' lock. */
#define STALE_GFN 0xB00u
#define STALE_HVA (HVA + 3 * MIB2 + 128 * 4096u)
static uint64_t seen_leaf = ~0ull;

/** Note the stale page's leaf: the fault's look again finds none if its
 * last pass mapped nothing. */
static void look_leaf(void)
{
    int level;

    seen_leaf = leaf_of(STALE_GFN, &level);
}

/** A page change that finished, under the regions' lock to read, between
 * the fault's read of `changes` and its leaf, with no notice to move
 * `mmu_seq`. */
static void change_finished(void)
{
    mmn_change_begin(&the_mm);
    mmn_change_end(&the_mm);
    at_read_lock = look_leaf;
}

/** A page change begun there and still under way at the leaf, which ends
 * at the fault's wait for it. */
static void change_begun(void)
{
    mmn_change_begin(&the_mm);
    at_read_lock = look_leaf;
    at_relax = change_ended;
}

/** The end of `change_begun`'s change. */
static void change_ended(void)
{
    mmn_change_end(&the_mm);
}

/** A notice of the page there, whose zap moves `mmu_seq` alone. */
static void notice_between(void)
{
    mmn_notify(&the_mm, MMN_COW, STALE_HVA, STALE_HVA + 4096u);
    at_read_lock = look_leaf;
}

/** The model's copy-on-write break of the page there, by another thread
 * that holds the regions' lock to read. */
static void break_between(void)
{
    CHECK(vm_prefault_range(STALE_HVA, 4096u, 1), "the break between");
    at_read_lock = look_leaf;
}

/** Run a read fault at the stale page, present in the caller's tables,
 * with `hook` at its read of the caller's entry: 1 when it mapped after
 * `passes` takes of the regions' lock, its look again finding no leaf. The
 * page's leaf is dropped before and every hook and count put back after. */
static int stale_fault(void (*hook)(void), int passes)
{
    int takes = mmap_takes, ok, level;

    mmn_notify(&the_mm, MMN_DROP, STALE_HVA, STALE_HVA + 4096u);
    flushed();
    seen_leaf = ~0ull;
    at_entry = hook;
    ok = kvm_tdp_fault(&vm, STALE_GFN, 0) == 0 && mmap_takes == takes + passes &&
         (!hook || seen_leaf == 0) && leaf_of(STALE_GFN, &level) != 0 && the_mm.changing == 0;
    at_entry = at_read_lock = at_relax = NULL;
    while (the_mm.changing) {
        mmn_change_end(&the_mm);
    }
    return ok;
}

/**
 * The fault's check before its leaf, as Linux's is_page_fault_stale and
 * mmu_invalidate_retry_gfn (arch/x86/kvm/mmu/mmu.c:4587-4598,
 * include/linux/kvm_host.h:2080-2110): a page change that finished between
 * the fault's read of the counts and its leaf, one still under way there,
 * and a notice's zap each make the fault map nothing and look again, and a
 * copy-on-write break there gives it the new frame.
 */
static void stale_rows(void)
{
    uint64_t e;
    int level;

    region(STALE_HVA, 1, 1);
    CHECK(vm_prefault_range(STALE_HVA, 4096u, 0), "the page brought in");
    CHECK(set(8, 0, (uint64_t)STALE_GFN * 4096u, 4096u, STALE_HVA) == 0, "a slot of a page");
    CHECK(stale_fault(NULL, 1), "a fault with no change maps at its first pass");
    CHECK(stale_fault(change_finished, 2),
          "a change finished between the fault's read of changes and its leaf: the fault maps "
          "nothing and looks again");
    CHECK(stale_fault(change_begun, 3),
          "a change under way at the leaf: the fault maps nothing, waits for its end at the "
          "next take and looks again");
    CHECK(stale_fault(notice_between, 2),
          "a notice's zap between the read and the leaf moves mmu_seq: the fault maps nothing "
          "and looks again");
    *host_entry(STALE_HVA) &= ~(pte_t)PTE_W;
    CHECK(stale_fault(break_between, 2) &&
              ((e = leaf_of(STALE_GFN, &level)) & ept.addr) == PTE_ADDR(*host_entry(STALE_HVA)),
          "a copy-on-write break between the look and the leaf: the fault looks again and maps "
          "the new frame");
    CHECK(set(8, 0, 0, 0, 0) == 0 && flushed(), "deleted");
}

/** A flush waits for a vCPU in the guest only until it takes the request,
 * as Linux's kvm_make_all_cpus_request waits for the kicked vCPU's
 * acknowledgement, not for it to leave the guest
 * (virt/kvm/kvm_main.c:255-320). */
static void flush_wait_rows(void)
{
    flushed();
    relaxes = 0;
    kvm_tdp_flush(&vm);
    CHECK(relaxes == 0 && flushed(), "a flush with every vCPU outside the guest waits for none");
    vcpu[0].mode = KVM_MODE_IN_GUEST;
    at_relax = take_flush;
    kvm_tdp_flush(&vm);
    CHECK(relaxes == 1 && vcpu[0].mode == KVM_MODE_IN_GUEST &&
              !(vcpu[0].requests & KVM_REQ_TLB_FLUSH) && (vcpu[1].requests & KVM_REQ_TLB_FLUSH),
          "a flush waits for a vCPU in the guest until it takes the request, and it may stay in "
          "the guest");
    vcpu[0].mode = KVM_MODE_OUTSIDE;
    at_relax = NULL;
    relaxes = 0;
    vcpu[0].requests = vcpu[1].requests = 0;
}

/** The end of the address space. */
static void release_rows(void)
{
    uintptr_t a = HVA + 7 * MIB2;
    int level;

    region(a, 8, 1);
    CHECK(set(7, 0, 0x700000, 8 * 4096u, a) == 0, "a slot");
    CHECK(kvm_tdp_fault(&vm, 0x701, 1) == 0, "a fault");
    mmn_release(&the_mm);
    CHECK(leaf_of(0x701, &level) == 0 && flushed(), "the end of the address space drops it");
    CHECK(set(7, 0, 0, 0, 0) == 0, "the slot is deleted after it");
}

/** Check counter wrap, an unpublished copy, full-ring refusal and a
 * deleted slot, with slot 8 at `gfn` backed by `a` and empty rings. */
static void dirty_ring_limit_rows(struct kvm_dirty_gfn rings[2][256], const struct kvm_slot_view *s,
                                  uint64_t gfn, uintptr_t a)
{
    struct kvm_dirty_gfn *entry;
    uint8_t byte = 0x5A;
    uint32_t i;
    int level;

    vcpu[0].dirty_ring_put = vcpu[0].dirty_ring_reset = 0xFFFFFFFEu;
    running_vcpu = &vcpu[0];
    for (i = 0; i < 3; i++) {
        kvm_dirty_mark(running_vcpu, s, gfn + i);
        rings[0][(254u + i) & 255u].flags = KVM_DIRTY_GFN_F_RESET;
    }
    CHECK(kvm_dirty_ioctl(&vm, KVM_RESET_DIRTY_RINGS, 0) == 3 && vcpu[0].dirty_ring_reset == 1,
          "both the ring and its free-running counters wrap");
    running_vcpu = NULL;
    CHECK(kvm_dirty_reserve(&vcpu[0], s, gfn, &entry) == 0 && entry == &rings[0][1] &&
              entry->flags == 0 && kvm_dirty_ioctl(&vm, KVM_RESET_DIRTY_RINGS, 0) == 0,
          "a state ioctl reserves an entry, and reset waits for its copy to finish");
    kvm_dirty_publish(entry);
    entry->flags = KVM_DIRTY_GFN_F_RESET;
    CHECK(kvm_dirty_ioctl(&vm, KVM_RESET_DIRTY_RINGS, 0) == 1,
          "the completed copy's published entry resets");
    for (i = 0; i < 256; i++) {
        CHECK(kvm_dirty_reserve(&vcpu[0], s, gfn, &entry) == 0,
              "a state ioctl can use the ring's reserve");
        kvm_dirty_publish(entry);
    }
    CHECK(kvm_dirty_reserve(&vcpu[0], s, gfn, &entry) == -ENOSPC && !entry,
          "a full ring refuses another copy before it overwrites an entry");
    running_vcpu = &vcpu[0];
    byte = 0x5A;
    CHECK(kvm_tdp_write_nofault(&vm, gfn * 4096u, &byte, 1, vm.mmu_seq) == 1 &&
              *(uint8_t *)phys_to_virt(PTE_ADDR(*host_entry(a))) == 0xA5,
          "a no-fault write cannot change memory when its ring is full");
    CHECK(kvm_tdp_fault(&vm, gfn + 5, 1) == 0 && !leaf_of(gfn + 5, &level),
          "a full ring leaves a guest's write fault pending");
    for (i = 0; i < 256; i++) {
        rings[0][i].flags = KVM_DIRTY_GFN_F_RESET;
    }
    CHECK(kvm_dirty_ioctl(&vm, KVM_RESET_DIRTY_RINGS, 0) == 256,
          "every entry remains available for collection after the refusals");
    i = vcpu[0].dirty_ring_put & 255u;
    kvm_dirty_mark(running_vcpu, s, gfn);
    rings[0][i].flags = KVM_DIRTY_GFN_F_RESET;
    CHECK(set(8, 0, 0, 0, 0) == 0 && kvm_dirty_ioctl(&vm, KVM_RESET_DIRTY_RINGS, 0) == 1,
          "an entry of a deleted slot can be reset");
}

/**
 * The ring logs fault and software writes without a bitmap; RESET takes
 * only the collected prefix, protects its pages and flushes, tolerates
 * stale or modified entries, and wraps its counters. Two vCPUs keep
 * separate rings. The arrays model the pages mapped to the caller.
 */
static void dirty_ring_rows(void)
{
    static struct kvm_dirty_gfn rings[2][256];
    struct kvm_enable_cap cap;
    struct kvm_slot_view s;
    uint64_t bits[1];
    uint64_t gfn = 0x900;
    uintptr_t a = HVA + 6 * MIB2;
    uint8_t byte = 0xA5;
    int level;
    uint32_t i;

    k_memset(&cap, 0, sizeof(cap));
    cap.cap = KVM_CAP_DIRTY_LOG_RING;
    cap.args[0] = 4096;
    CHECK(kvm_dirty_enable_ring(&vm, &cap) == -EINVAL, "no ring enabled after vCPU creation");
    vm.nr_vcpus = 0;
    CHECK(kvm_dirty_enable_ring(&vm, &cap) == 0, "ring enabled before vCPUs");
    CHECK(kvm_dirty_enable_ring(&vm, &cap) == -EINVAL, "ring enabled only once");
    vm.nr_vcpus = 2;
    for (i = 0; i < 2; i++) {
        vcpu[i].vm = &vm;
        vcpu[i].dirty_ring = rings[i];
    }
    region(a, 8, 1);
    CHECK(set(8, KVM_MEM_LOG_DIRTY_PAGES, gfn * 4096u, 8 * 4096u, a) == 0 &&
              kvm_slot_find(&vm, gfn, &s) && !s.dirty,
          "ring logging has no bitmap");
    CHECK(get_dirty(8, bits) == -ENXIO && clear_dirty(8, 0, 8, bits) == -ENXIO,
          "ring mode refuses the bitmap ioctls");
    running_vcpu = &vcpu[0];
    CHECK(kvm_tdp_fault(&vm, gfn, 0) == 0 && !(leaf_of(gfn, &level) & 2),
          "a ring's clean page is read-only");
    CHECK(kvm_tdp_fault(&vm, gfn, 1) == 0 && (leaf_of(gfn, &level) & 2) &&
              rings[0][0].flags == KVM_DIRTY_GFN_F_DIRTY && rings[0][0].slot == 8 &&
              rings[0][0].offset == 0,
          "the write fault publishes the slot and page in its vCPU's ring");
    kvm_dirty_mark(running_vcpu, &s, gfn + 1);
    CHECK(rings[0][1].offset == 1 && vcpu[0].dirty_ring_put == 2,
          "a software write enters the same ring");
    running_vcpu = &vcpu[1];
    kvm_dirty_mark(running_vcpu, &s, gfn + 2);
    CHECK(vcpu[1].dirty_ring_put == 1 && rings[1][0].offset == 2,
          "another vCPU has a separate ring");
    rings[0][1].flags = KVM_DIRTY_GFN_F_RESET;
    CHECK(kvm_dirty_ioctl(&vm, KVM_RESET_DIRTY_RINGS, 0) == 0,
          "reset stops before an entry not collected");
    rings[0][0].flags = KVM_DIRTY_GFN_F_RESET;
    rings[1][0].flags = KVM_DIRTY_GFN_F_RESET;
    CHECK(kvm_dirty_ioctl(&vm, KVM_RESET_DIRTY_RINGS, 0) == 3 && rings[0][0].flags == 0 &&
              rings[1][0].flags == 0 && !(leaf_of(gfn, &level) & 2) && flushed(),
          "reset frees both rings' collected prefixes and protects the page");
    CHECK(kvm_tdp_fault(&vm, gfn, 1) == 0 && rings[1][1].offset == 0,
          "the next write is logged again");
    rings[1][1].slot = 0x10008;
    rings[1][1].offset = ~0ull;
    rings[1][1].flags = KVM_DIRTY_GFN_F_RESET;
    CHECK(kvm_dirty_ioctl(&vm, KVM_RESET_DIRTY_RINGS, 0) == 1,
          "an invalid slot supplied by the caller is skipped");
    kvm_dirty_mark(running_vcpu, &s, gfn);
    rings[1][2].offset = ~0ull;
    rings[1][2].flags = KVM_DIRTY_GFN_F_RESET;
    CHECK(kvm_dirty_ioctl(&vm, KVM_RESET_DIRTY_RINGS, 0) == 1,
          "an invalid offset supplied by the caller is skipped");
    *host_entry(a) |= PTE_DIRTY;
    CHECK(kvm_tdp_write_nofault(&vm, gfn * 4096u, &byte, 1, vm.mmu_seq) == 0 &&
              rings[1][3].offset == 0 && rings[1][3].flags == KVM_DIRTY_GFN_F_DIRTY,
          "the steal time's no-fault write enters its running vCPU's ring");
    rings[1][3].flags = KVM_DIRTY_GFN_F_RESET;
    CHECK(kvm_dirty_ioctl(&vm, KVM_RESET_DIRTY_RINGS, 0) == 1, "the no-fault write's entry resets");
    dirty_ring_limit_rows(rings, &s, gfn, a);
    running_vcpu = NULL;
    vm.dirty_ring_size = 0;
    for (i = 0; i < 2; i++) {
        vcpu[i].dirty_ring = NULL;
        vcpu[i].dirty_ring_put = vcpu[i].dirty_ring_reset = 0;
    }
    CHECK(kvm_dirty_ioctl(&vm, KVM_RESET_DIRTY_RINGS, 0) == -EINVAL,
          "reset without a ring is refused");
}

/* The readers of `gp_rows`, the counts they joined, oldest first. */
#define GP_READERS 8
static uint32_t gp_reader[GP_READERS];
static int gp_readers;

/* The most rounds of overlapping readers a replacement is given to end:
 * one that waits for two counts no new reader joins ends in 3. */
#define GP_ROUNDS 8

/* Whether each replacement has returned. */
static volatile int gp_done[3];

/** A reader begins a section, which joins the count of `srcu_gen`. */
static void gp_join(void)
{
    gp_reader[gp_readers++] = kvm_srcu_read_lock(&vm);
}

/** The oldest reader ends its section. */
static void gp_leave(void)
{
    int i;

    kvm_srcu_read_unlock(&vm, gp_reader[0]);
    for (i = 1; i < gp_readers; i++) {
        gp_reader[i - 1] = gp_reader[i];
    }
    gp_readers--;
}

/** Give the turn to replacement `w` and wait until it gives it back, by a
 * wait, a take of a held mutex or its return. */
static void gp_run(int w)
{
    __atomic_store_n(&gp_turn, w, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&gp_turn, __ATOMIC_SEQ_CST) != 0) {
        (void)sched_yield();
    }
}

/** A replacement's thread: at its first turn, `kvm_srcu_synchronize`. */
static void *gp_writer(void *arg)
{
    int me = (int)(intptr_t)arg;

    while (__atomic_load_n(&gp_turn, __ATOMIC_SEQ_CST) != me) {
        (void)sched_yield();
    }
    kvm_srcu_synchronize(&vm);
    gp_done[me] = 1;
    __atomic_store_n(&gp_turn, 0, __ATOMIC_SEQ_CST);
    return NULL;
}

/**
 * The readers' thread, in the order of j71's finding: replacement 1
 * waits for count 0, which a reader from before it holds; replacement 2
 * comes meanwhile; the reader ends and 1 returns; a new reader joins, and
 * then readers keep overlapping, each new one joining before the oldest
 * ends. Without `srcu_gp_lock`, 2 changes `srcu_gen` under 1 and is left
 * waiting for count 0, which each new reader joins, and never ends.
 */
static void *gp_script(void *arg)
{
    int round;

    (void)arg;
    gp_join();
    gp_run(1);
    gp_run(2);
    gp_leave();
    gp_run(1);
    CHECK(gp_done[1], "the first replacement ends once the reader from before it has");
    gp_join();
    for (round = 0; round < GP_ROUNDS && !gp_done[2]; round++) {
        gp_run(2);
        if (!gp_done[2]) {
            gp_join();
            gp_leave();
        }
    }
    CHECK(gp_done[2], "the second replacement ends while new readers keep overlapping");
    while (gp_readers) {
        gp_leave();
    }
    while (!gp_done[1] || !gp_done[2]) {
        gp_run(gp_done[1] ? 2 : 1);
    }
    return NULL;
}

/** A thread of `gp_rows`: 0 the readers', 1 and 2 the replacements'. */
static void *gp_thread(void *arg)
{
    return arg ? gp_writer(arg) : gp_script(arg);
}

/** Two replacements of the read side at once, which wait one at a time,
 * so that neither waits for a count new readers join. */
static void gp_rows(void)
{
    void *args[3];

    args[0] = NULL;
    args[1] = (void *)(intptr_t)1;
    args[2] = (void *)(intptr_t)2;
    gp_turn = 0;
    gp_threads = 1;
    CHECK(host_threads_run(3, gp_thread, args) == 0, "the replacements' threads");
    gp_threads = 0;
    CHECK(vm.srcu_readers[0] == 0 && vm.srcu_readers[1] == 0 && vm.srcu_waiting == 0 &&
              vm.srcu_gp_lock.owner == NULL,
          "the read side idle after both");
}

/** Whether `a` and `b` are the same string. */
static int same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/** `--grace-only` runs `gp_rows` alone. */
int main(int argc, char **argv)
{
    uint32_t i;
    int grace_only = argc > 1 && same(argv[1], "--grace-only");

    if (posix_memalign((void **)&ram, MIB2, 8 * MIB2) != 0) {
        printf("kvm-slots-hosttest: failed: no memory\n");
        return 1;
    }
    vm.nr_vcpus = 2;
    for (i = 0; i < 2; i++) {
        vm.vcpus[i] = &vcpu[i];
        vcpu[i].vm = &vm;
    }
    caller.mm = &the_mm;
    CHECK(kvm_slots_init(&vm) == 0 && kvm_tdp_init(&vm) == 0, "the VM's tables");
    if (grace_only) {
        gp_rows();
        kvm_tdp_free(&vm);
        kvm_slots_free(&vm);
        free(ram);
        if (failures) {
            return 1;
        }
        printf("kvm-slots-hosttest: ok\n");
        return 0;
    }
    capability_rows();
    region_rows();
    many_rows();
    small_rows();
    readonly_rows();
    large_rows();
    secret_rows();
    zero_rows();
    pre_fault_rows();
    device_rows();
    dirty_rows();
    write_nofault_rows();
    zap_batch_rows();
    stale_rows();
    flush_wait_rows();
    gp_rows();
    dirty_ring_rows();
    release_rows();
    CHECK(((struct tdp *)vm.tdp)->tables == 1, "no table below the top once every slot went");
    CHECK(set(0, 0, 0, 0x1000, HVA) == 0 && kvm_tdp_fault(&vm, 0, 0) == 0, "a slot left");
    kvm_tdp_free(&vm);
    kvm_slots_free(&vm);
    CHECK(live == 0 && frames == 0, "every allocation and frame given back");
    CHECK(mmap_reads == 0 && !vm.mmu_lock.locked,
          "every hold of the address space and of mmu_lock given back");
    free(ram);
    if (failures) {
        return 1;
    }
    printf("kvm-slots-hosttest: ok\n");
    return 0;
}
