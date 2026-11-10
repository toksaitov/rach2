/*
 * tdp.c: a VM's guest-physical tables, the four levels that EPT and AMD's
 * nested paging both walk, built at each fault from the pages of the
 * caller's memory behind a slot, in the entry format the backend gives.
 */

#include "kvm.h"

#include "../arch/arch.h"
#include "../arch/x86/kcpu.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"
#include "../mm/mm.h"
#include "../mm/paging.h"
#include "../proc/process.h"

/* The levels, the entries of a table, and the 4 KiB frames of a 2 MiB
 * leaf, which a table of the second level holds in one entry. */
#define TDP_LEVELS  4
#define TDP_ENTRIES 512u
#define TDP_LARGE   0x200000ull

/*
 * The tables of a VM. Each is a frame of the allocator's, reached through
 * the direct map, which covers every frame on x86-64; `tables` counts
 * them, the top one among them. A table below the top that a zap empties
 * waits on `dead`, linked through its first entry, until the flush after
 * the zap, since a vCPU in the guest may walk it, or hold it in its
 * paging-structure caches, until then; the rest go with the VM.
 */
struct tdp {
    uint64_t root; /* the top table's physical address */
    uint64_t dead; /* the first table to free after the flush, or 0 */
    uint32_t tables;
};

/** The backend's entry format. */
static const struct kvm_tdp_format *fmt(void)
{
    return kvm_backend->tdp;
}

/** The kernel's address of the table at `phys`. */
static uint64_t *table_at(uint64_t phys)
{
    return (uint64_t *)phys_to_virt(phys);
}

/** The entry of `gfn` in a table of level `level`, 1 for the leaves. */
static uint32_t index_of(uint64_t gfn, int level)
{
    return (uint32_t)(gfn >> (9 * (level - 1))) & (TDP_ENTRIES - 1u);
}

/** A zeroed table: its physical address, or 0 when no frame was free. */
static uint64_t table_new(struct tdp *t)
{
    uintptr_t p = alloc_frame();

    if (p) {
        k_memset(table_at(p), 0, 4096);
        t->tables++;
    }
    return p;
}

/** Give the table at `phys` back. */
static void table_free(struct tdp *t, uint64_t phys)
{
    free_frame((uintptr_t)phys);
    t->tables--;
}

/** Whether the entry `e` maps or names anything. */
static int present(uint64_t e)
{
    return (e & fmt()->present) != 0;
}

/** Whether the present entry `e` of level `level` is a leaf. */
static int leaf(uint64_t e, int level)
{
    return level == 1 || (e & fmt()->large) != 0;
}

/** An entry that names the table at `phys`, with every right, which the
 * leaves below narrow. */
static uint64_t table_entry(uint64_t phys)
{
    const struct kvm_tdp_format *f = fmt();

    return phys | f->read | f->write | f->exec | f->user | f->accessed;
}

/**
 * A leaf of `vm` for the frame at `phys`, 2 MiB for `large`, executable
 * and readable, writable for `writable`, with the memory type of Linux's
 * vmx_get_mt_mask: a device's frame is uncached with the guest's PAT in
 * charge, so that the guest may make it write-combining; RAM is write-back
 * with the guest's PAT ignored, or, while a device that does not snoop is
 * assigned to the VM, write-back with the guest's PAT in charge, so that
 * the guest's uncached buffers stay uncached for that device.
 */
static uint64_t leaf_entry(const struct kvm_vm *vm, uint64_t phys, int writable, int large,
                           int device)
{
    const struct kvm_tdp_format *f = fmt();
    uint64_t e = phys | f->read | f->exec | f->user | f->accessed;

    e |= device ? f->cache(KVM_MT_UC, 0) : f->cache(KVM_MT_WB, vm->noncoherent == 0);
    if (writable) {
        e |= f->write | f->dirty;
    }
    if (large) {
        e |= f->large;
    }
    return e;
}

int kvm_tdp_init(struct kvm_vm *vm)
{
    struct tdp *t = kzalloc(sizeof(*t));

    if (!t) {
        return -ENOMEM;
    }
    t->root = table_new(t);
    if (!t->root) {
        kfree(t);
        return -ENOMEM;
    }
    vm->tdp = t;
    return 0;
}

/** Free the tables below `table`, of level `level`. */
static void free_level(struct tdp *t, uint64_t *table, int level)
{
    uint32_t i;

    for (i = 0; i < TDP_ENTRIES; i++) {
        uint64_t e = table[i];

        if (present(e) && !leaf(e, level)) {
            free_level(t, table_at(e & fmt()->addr), level - 1);
            table_free(t, e & fmt()->addr);
        }
    }
}

void kvm_tdp_free(struct kvm_vm *vm)
{
    struct tdp *t = (struct tdp *)vm->tdp;

    if (!t) {
        return;
    }
    free_level(t, table_at(t->root), TDP_LEVELS);
    table_free(t, t->root);
    kfree(t);
    vm->tdp = NULL;
}

uint64_t kvm_tdp_root(struct kvm_vm *vm)
{
    return ((struct tdp *)vm->tdp)->root;
}

/** The host's physical width, CPUID 0x80000008's, 36 bits without the
 * leaf, and at most the 48 bits four levels reach, as Linux's
 * kvm_mmu_max_gfn bounds a slot's last frame. */
uint64_t kvm_tdp_frames(void)
{
    static uint64_t frames;
    uint32_t a = 0, b = 0, c = 0, d = 0, bits = 36;

    if (frames) {
        return frames;
    }
    kcpu_id(0x80000000u, &a, &b, &c, &d);
    if (a >= 0x80000008u) {
        kcpu_id(0x80000008u, &a, &b, &c, &d);
        bits = a & 0xFFu;
    }
    if (bits > 48u) {
        bits = 48u;
    }
    frames = 1ull << (bits - 12u);
    return frames;
}

/** A vCPU leaves the guest within a moment of its kick, and does not
 * enter again until it has the big lock, which the caller holds. */
void kvm_tdp_flush(struct kvm_vm *vm)
{
    uint32_t i;

    for (i = 0; i < vm->nr_vcpus; i++) {
        kvm_vcpu_kick(vm->vcpus[i], KVM_REQ_TLB_FLUSH);
    }
    for (i = 0; i < vm->nr_vcpus; i++) {
        while (vm->vcpus[i]->mode == KVM_MODE_IN_GUEST) {
            arch_relax();
        }
    }
}

/** Whether the table `table` maps and names nothing. */
static int table_empty(const uint64_t *table)
{
    uint32_t i;

    for (i = 0; i < TDP_ENTRIES; i++) {
        if (present(table[i])) {
            return 0;
        }
    }
    return 1;
}

/**
 * Clear the entries of `table`, of level `level` and with `base` its first
 * frame, that meet the frames [lo, hi): a leaf goes whole, a 2 MiB one
 * that the range only meets among them, and a table below goes to the
 * dead list once it maps nothing, so that a deleted slot leaves no table
 * behind and a later 2 MiB leaf finds no table in its place. Whether a
 * leaf was cleared.
 */
static int zap_level(struct tdp *t, uint64_t *table, int level, uint64_t base, uint64_t lo,
                     uint64_t hi)
{
    uint64_t span = 1ull << (9 * (level - 1));
    uint32_t i = lo > base ? (uint32_t)((lo - base) / span) : 0;
    int cleared = 0;

    for (; i < TDP_ENTRIES && base + i * span < hi; i++) {
        uint64_t e = table[i], from = base + i * span;

        if (!present(e)) {
            continue;
        }
        if (leaf(e, level)) {
            table[i] = 0;
            cleared = 1;
            continue;
        }
        cleared |= zap_level(t, table_at(e & fmt()->addr), level - 1, from, lo, hi);
        if ((lo <= from && from + span <= hi) || table_empty(table_at(e & fmt()->addr))) {
            table[i] = 0;
            table_at(e & fmt()->addr)[0] = t->dead;
            t->dead = e & fmt()->addr;
        }
    }
    return cleared;
}

/** A table on the dead list is freed only after the flush, and a table
 * that goes needs the flush even when no leaf did. */
void kvm_tdp_zap(struct kvm_vm *vm, uint64_t gfn, uint64_t npages)
{
    struct tdp *t = (struct tdp *)vm->tdp;

    if (!t || npages == 0 || gfn >= kvm_tdp_frames()) {
        return;
    }
    if (zap_level(t, table_at(t->root), TDP_LEVELS, 0, gfn, gfn + npages) || t->dead) {
        kvm_tdp_flush(vm);
    }
    while (t->dead) {
        uint64_t p = t->dead;

        t->dead = table_at(p)[0];
        table_free(t, p);
    }
}

/** The leaf that maps `gfn`, with its level in `*level`, or NULL. */
static uint64_t *find_leaf(struct tdp *t, uint64_t gfn, int *level)
{
    uint64_t *table = table_at(t->root);
    int l;

    for (l = TDP_LEVELS; l >= 1; l--) {
        uint64_t *e = &table[index_of(gfn, l)];

        if (!present(*e)) {
            return NULL;
        }
        if (leaf(*e, l)) {
            *level = l;
            return e;
        }
        table = table_at(*e & fmt()->addr);
    }
    return NULL;
}

/** A 2 MiB leaf is dropped whole, since its write right covers frames the
 * mask may not name; the next write to any of them faults. */
int kvm_tdp_protect(struct kvm_vm *vm, uint64_t gfn, uint64_t mask)
{
    struct tdp *t = (struct tdp *)vm->tdp;
    int changed = 0, level;
    uint32_t i;

    for (i = 0; t && i < 64u; i++) {
        uint64_t *e;

        if (!((mask >> i) & 1u) || !(e = find_leaf(t, gfn + i, &level))) {
            continue;
        }
        if (level > 1) {
            *e = 0;
            changed = 1;
        } else if (*e & fmt()->write) {
            *e &= ~fmt()->write;
            changed = 1;
        }
    }
    return changed;
}

/**
 * Set the entry of `gfn` at level `level`, 1 or 2, to `entry`, making the
 * tables above it. A 2 MiB leaf on the way, where a finer leaf is wanted,
 * goes first, with a flush, since the frames it mapped are no longer
 * mapped until their next fault. 0 or -ENOMEM.
 */
static int map(struct kvm_vm *vm, struct tdp *t, uint64_t gfn, int level, uint64_t entry)
{
    uint64_t *table = table_at(t->root);
    int l;

    for (l = TDP_LEVELS; l > level; l--) {
        uint64_t *e = &table[index_of(gfn, l)];

        if (present(*e) && l <= 3 && leaf(*e, l)) {
            *e = 0;
            kvm_tdp_flush(vm);
        }
        if (!present(*e)) {
            uint64_t p = table_new(t);

            if (!p) {
                return -ENOMEM;
            }
            *e = table_entry(p);
        }
        table = table_at(*e & fmt()->addr);
    }
    table[index_of(gfn, level)] = entry;
    return 0;
}

/** The caller's entry for the page at `hva` of `mm`, 0 when it has none;
 * the page is the caller's to touch when the entry has PTE_P and PTE_U,
 * since a page of a PROT_NONE region keeps PTE_P and loses PTE_U. */
static pte_t host_pte(struct mm *mm, uintptr_t hva)
{
    pte_t *table = mmu_user_table(mm->dir, hva, 0);

    return table ? table[MMU_INDEX(hva)] : 0;
}

/** Whether the caller may touch the page of the entry `e`, for a write
 * when `write` says so. */
static int host_ok(pte_t e, int write)
{
    return (e & (PTE_P | PTE_U)) == (PTE_P | PTE_U) && (!write || (e & PTE_W));
}

/* A leaf to set: its frame, its write right and its kind. */
struct leaf_info {
    uint64_t phys;
    int writable;
    int device;
};

/**
 * Whether the 2 MiB of guest frames around `gfn` of the slot `s` can be one
 * leaf: they lie in the slot, which does not log, their caller's addresses
 * are one leaf table of `mm`, 2 MiB-aligned, whose 512 pages are present,
 * the caller's to touch, and 512 consecutive frames from a 2 MiB boundary,
 * all RAM or all a device's, and no table of `t` lies there already. The
 * leaf is writable when every page is and the slot is not read-only, and a
 * write fault needs it so. 1 with the leaf in `*out`, or 0.
 */
static int large_ok(struct mm *mm, struct tdp *t, uint64_t gfn, int write,
                    const struct kvm_slot_view *s, struct leaf_info *out)
{
    uint64_t block = gfn & ~(uint64_t)(TDP_ENTRIES - 1u);
    uintptr_t hva = (uintptr_t)(s->hva - (gfn - block) * 4096u);
    uint64_t *table = table_at(t->root), base;
    pte_t *host;
    uint32_t i;
    int l, w = !(s->flags & KVM_MEM_READONLY), dev;

    if (s->dirty || block < s->base_gfn || block + TDP_ENTRIES > s->base_gfn + s->npages ||
        (hva & (TDP_LARGE - 1u)) != 0) {
        return 0;
    }
    for (l = TDP_LEVELS; l > 2; l--) {
        uint64_t e = table[index_of(block, l)];

        if (!present(e)) {
            break;
        }
        if (leaf(e, l)) {
            return 0;
        }
        table = table_at(e & fmt()->addr);
    }
    if (l == 2 && present(table[index_of(block, 2)]) && !leaf(table[index_of(block, 2)], 2)) {
        return 0;
    }
    host = mmu_user_table(mm->dir, hva, 0);
    if (!host || !host_ok(host[0], 0)) {
        return 0;
    }
    base = PTE_ADDR(host[0]);
    if (base & (TDP_LARGE - 1u)) {
        return 0;
    }
    dev = paging_frame_is_device((uintptr_t)base);
    for (i = 0; i < TDP_ENTRIES; i++) {
        pte_t e = host[i];

        if (!host_ok(e, 0) || PTE_ADDR(e) != base + (uint64_t)i * 4096u ||
            paging_frame_is_device((uintptr_t)PTE_ADDR(e)) != dev) {
            return 0;
        }
        if (!(e & PTE_W)) {
            w = 0;
        }
    }
    if (write && !w) {
        return 0;
    }
    out->phys = base;
    out->writable = w;
    out->device = dev;
    return 1;
}

/**
 * The caller's page is brought in with `vm_prefault_range`, which may sleep,
 * and then read again from the caller's tables with nothing that sleeps
 * before the leaf is set, so that the change notice, which runs only under
 * the big lock, cannot come between the read and the leaf. The slot is
 * looked up again after each sleep, since it may have moved or gone.
 *
 * A page of a slot that logs is mapped writable only when it is dirty
 * already: a write fault marks it, and a read leaves a clean page
 * read-only, so that its first write faults and is logged.
 */
int kvm_tdp_fault(struct kvm_vm *vm, uint64_t gfn, int write)
{
    struct tdp *t = (struct tdp *)vm->tdp;
    struct mm *mm = kvm_vm_mm(vm);
    struct kvm_slot_view s;
    struct leaf_info big;
    uint64_t phys, i;
    int writable;
    pte_t pte;

    for (;;) {
        if (!kvm_slot_find(vm, gfn, &s) || (write && (s.flags & KVM_MEM_READONLY))) {
            return 1;
        }
        if (!t || !mm || !current_proc || current_proc->mm != mm) {
            return -EFAULT;
        }
        pte = host_pte(mm, (uintptr_t)s.hva);
        if (host_ok(pte, write)) {
            break;
        }
        if (!vm_prefault_range((uintptr_t)s.hva, 4096u, write)) {
            return -EFAULT;
        }
    }
    if (large_ok(mm, t, gfn, write, &s, &big)) {
        return map(vm, t, gfn, 2, leaf_entry(vm, big.phys, big.writable, 1, big.device));
    }
    phys = PTE_ADDR(pte);
    writable = (pte & PTE_W) && !(s.flags & KVM_MEM_READONLY);
    if (s.dirty) {
        i = gfn - s.base_gfn;
        if (write) {
            s.dirty[i / 64u] |= 1ull << (i % 64u);
        } else if (!((s.dirty[i / 64u] >> (i % 64u)) & 1u)) {
            writable = 0;
        }
    }
    return map(vm, t, gfn, 1, leaf_entry(vm, phys, writable, 0, paging_frame_is_device(phys)));
}

void kvm_tdp_noncoherent(struct kvm_vm *vm, int assigned)
{
    uint32_t before = vm->noncoherent;

    if (assigned) {
        vm->noncoherent++;
    } else if (vm->noncoherent) {
        vm->noncoherent--;
    }
    /* The first such device and the last one change every RAM leaf's
     * type, so every leaf goes and the next fault makes it anew, as Linux's
     * kvm_noncoherent_dma_assignment_start_or_stop zaps them. */
    if ((before == 0) != (vm->noncoherent == 0)) {
        kvm_tdp_zap(vm, 0, kvm_tdp_frames());
    }
}
