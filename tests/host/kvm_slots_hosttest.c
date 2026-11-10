/**
 * Check KVM's memory slots, guest-physical tables and dirty log over a
 * model of the caller's page tables.
 *
 * host-cflags: -DRACH2_HOST_X86_64 -DRACH2_HOST_CPU
 *
 * slots.c, tdp.c, dirty.c and mm/notify.c are the kernel's, included
 * whole. The caller's memory is a model: a window of user addresses whose
 * leaf tables are arrays here, a page brought in by `vm_prefault_range` as
 * a fault would, with a new frame for a write to a copy-on-write page, and
 * the changes munmap, mprotect, a copy-on-write break, madvise and a
 * truncation make, each told to the change notice before the tables change,
 * as the kernel's calls do. RAM is host memory reached through the direct
 * map's formula, so that a frame's address is its physical one, and a
 * device's frames are addresses below 1 TiB, which no host frame has. The
 * backend's entry format is EPT's.
 *
 * The rows: the dirty log's structures and ioctl numbers; the capabilities;
 * KVM_SET_USER_MEMORY_REGION's checks in Linux's order and its answers for a
 * new, moved, flagged, unchanged and deleted slot and an overlap; a 4 KiB slot
 * mapped at each fault with the caller's frame, write-back with the guest's PAT
 * ignored, or in charge while a device that does not snoop is assigned, the
 * first and the last such device dropping every leaf, read-only over a
 * copy-on-write page and writable after the write that breaks it; a slot of
 * 2 MiB-aligned contiguous RAM as one 2 MiB leaf, and
 * as 4 KiB leaves where the slot, the addresses or the frames do not allow one;
 * a device's frames uncached with the guest's PAT in charge, as a 2 MiB leaf
 * too; a read-only slot, a frame with no slot and an address the caller cannot
 * touch; each change of the caller's memory dropping what the tables map of it
 * and asking every vCPU to flush, a change to one page of a 2 MiB leaf dropping
 * the leaf; the end of the address space; dirty logging with KVM_GET_DIRTY_LOG
 * and, under KVM_CAP_MANUAL_DIRTY_LOG_PROTECT2, KVM_CLEAR_DIRTY_LOG, their
 * refusals, and no 2 MiB leaf in a slot that logs; a slot moved, its flags
 * changed and deleted with its leaves and tables gone; and every allocation and
 * frame given back.
 */
#include "kvm/dirty.c"
#include "kvm/slots.c"
#include "kvm/tdp.c"
#include "mm/notify.c"

extern int printf(const char *format, ...);
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

void arch_relax(void)
{}

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

struct mm *kvm_vm_mm(struct kvm_vm *v)
{
    (void)v;
    return &the_mm;
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
    if (va < HVA || va >= HVA + BLOCKS * MIB2) {
        return NULL;
    }
    return host_pt[(va - HVA) / MIB2];
}

/** A page of RAM from the top half of `ram`, which the rows that need
 * aligned runs leave alone. */
static uintptr_t fresh(void)
{
    return phys_of(ram + 4u * MIB2 + (ram_next++ % 2048u) * 4096u);
}

static int prefaults;

/** A page brought in as a fault would: a fresh frame, writable where the
 * region is, and for a write to a copy-on-write page a new frame, told to
 * the notice first. */
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
        mmn_notify(&the_mm, MMN_COW, addr & ~4095ul, (addr & ~4095ul) + 4096u);
        *e = fresh() | PTE_P | PTE_U | PTE_W;
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
    CHECK(kvm_slots_check_extension(KVM_CAP_READONLY_MEM) == 1, "READONLY_MEM");
    CHECK(kvm_slots_check_extension(KVM_CAP_SYNC_MMU) == 1, "SYNC_MMU");
    CHECK(kvm_slots_check_extension(KVM_CAP_NR_MEMSLOTS) == 256, "NR_MEMSLOTS");
    CHECK(kvm_slots_check_extension(KVM_CAP_MANUAL_DIRTY_LOG_PROTECT2) == 3,
          "MANUAL_DIRTY_LOG_PROTECT2");
    CHECK(kvm_slots_check_extension(KVM_CAP_IRQFD) == 0, "a capability not the slots'");
}

/** KVM_SET_USER_MEMORY_REGION's checks and answers. */
static void region_rows(void)
{
    CHECK(set(0, 4, 0, 0x10000, HVA) == -EINVAL, "an unknown flag");
    CHECK(set(0, 0, 0, 0x10001, HVA) == -EINVAL, "a size not page-aligned");
    CHECK(set(0, 0, 0x800, 0x10000, HVA) == -EINVAL, "an address not page-aligned");
    CHECK(set(0, 0, 0, 0x10000, HVA + 8) == -EINVAL, "a caller's address not page-aligned");
    CHECK(set(0, 0, 0, 0x2000, USER_ADDR_MAX - 0x1000) == -EINVAL, "a range past user space");
    CHECK(set(0x10000, 0, 0, 0x10000, HVA) == -EINVAL, "address space 1");
    CHECK(set(256, 0, 0, 0x10000, HVA) == -EINVAL, "slot 256");
    CHECK(set(0, 0, 0xFFFFFFFFFFFFF000ull, 0x2000, HVA) == -EINVAL, "a range that wraps");
    CHECK(set(0, 0, 1ull << 39, 0x1000, HVA) == -EINVAL, "a range past the physical width");
    CHECK(set(0, 0, 0, 0, HVA) == -EINVAL, "the deletion of no slot");
    CHECK(set(0, 0, 0x100000, 0x10000, HVA) == 0, "a new slot");
    CHECK(set(1, 0, 0x108000, 0x10000, HVA + 0x10000) == -EEXIST, "an overlap");
    CHECK(set(1, 0, 0x110000, 0x10000, HVA) == 0, "a neighbour, aliasing the same memory");
    CHECK(set(0, 0, 0x100000, 0x10000, HVA + 0x1000) == -EINVAL, "a change of address");
    CHECK(set(0, 0, 0x100000, 0x20000, HVA) == -EINVAL, "a change of size");
    CHECK(set(0, KVM_MEM_READONLY, 0x100000, 0x10000, HVA) == -EINVAL, "a change of READONLY");
    CHECK(set(0, 0, 0x100000, 0x10000, HVA) == 0, "no change");
    CHECK(set(0, 0, 0x118000, 0x10000, HVA) == -EEXIST, "a move into another slot");
    CHECK(set(0, 0, 0x200000, 0x10000, HVA) == 0, "a move");
    CHECK(set(0, 0, 1ull << 39, 0x10000, HVA) == -EINVAL, "a move past the physical width");
    CHECK(set(0, 0, 0, 0, 0) == 0 && set(1, 0, 0, 0, 0) == 0, "the deletions");
    CHECK(((struct slots *)vm.slots)->pages == 0 && the_mm.notify == NULL,
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

    /* One page of the run goes: the leaf goes whole. */
    flushed();
    mmn_notify(&the_mm, MMN_UNMAP, a + 9 * 4096u, a + 10 * 4096u);
    CHECK(leaf_of(block + 300, &level) == 0 && flushed(), "a change to one page drops it");
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
    CHECK(set(3, 0, 0, 0, 0) == 0, "deleted");
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
    CHECK(get_dirty(256, bits) == -EINVAL, "slot 256");
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
    kvm_dirty_mark(&vm, gfn + 65);
    CHECK(get_dirty(5, bits) == 0 && bits[1] == 3, "is logged, and so is a kernel's write");
    CHECK(set(5, 0, 0, 0, 0) == 0, "deleted");
    vm.dirty_manual = 0;

    /* No 2 MiB leaf in a slot that logs. */
    fill_block(HVA + 2 * MIB2, phys_of(ram));
    CHECK(set(3, KVM_MEM_LOG_DIRTY_PAGES, 0x400000, MIB2, HVA + 2 * MIB2) == 0,
          "an aligned slot that logs");
    CHECK(kvm_tdp_fault(&vm, 0x400, 1) == 0 && leaf_of(0x400, &level) != 0 && level == 1,
          "4 KiB leaves");
    CHECK(set(3, 0, 0, 0, 0) == 0, "deleted");
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

int main(void)
{
    uint32_t i;

    if (posix_memalign((void **)&ram, MIB2, 8 * MIB2) != 0) {
        printf("kvm-slots-hosttest: failed: no memory\n");
        return 1;
    }
    vm.nr_vcpus = 2;
    for (i = 0; i < 2; i++) {
        vm.vcpus[i] = &vcpu[i];
    }
    caller.mm = &the_mm;
    CHECK(kvm_slots_init(&vm) == 0 && kvm_tdp_init(&vm) == 0, "the VM's tables");
    capability_rows();
    region_rows();
    small_rows();
    readonly_rows();
    large_rows();
    device_rows();
    dirty_rows();
    release_rows();
    CHECK(((struct tdp *)vm.tdp)->tables == 1, "no table below the top once every slot went");
    CHECK(set(0, 0, 0, 0x1000, HVA) == 0 && kvm_tdp_fault(&vm, 0, 0) == 0, "a slot left");
    kvm_tdp_free(&vm);
    kvm_slots_free(&vm);
    CHECK(live == 0 && frames == 0, "every allocation and frame given back");
    free(ram);
    if (failures) {
        return 1;
    }
    printf("kvm-slots-hosttest: ok\n");
    return 0;
}
