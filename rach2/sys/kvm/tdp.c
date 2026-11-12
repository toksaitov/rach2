/*
 * tdp.c: a VM's guest-physical tables, the four levels that EPT and AMD's
 * nested paging both walk, built at each fault from the pages of the
 * caller's memory behind a slot, in the entry format the backend gives.
 * The tables and their counts are the VM's `mmu_lock`'s, as Linux's
 * kvm->mmu_lock owns the TDP MMU's (arch/x86/kvm/mmu/mmu.c:4699-4731).
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
#include "../proc/smp.h"

/* The levels, the entries of a table, and the 4 KiB frames of a 2 MiB
 * leaf, which a table of the second level holds in one entry. */
#define TDP_LEVELS  4
#define TDP_ENTRIES 512u
#define TDP_LARGE   0x200000ull

/*
 * The tables of a VM, under its `mmu_lock`. Each is a frame of the
 * allocator's, reached through the direct map, which covers every frame on
 * x86-64; `tables` counts them, the top one among them. A table below the
 * top that a zap empties waits on `dead`, linked through its first entry,
 * until the flush after the zap, since a vCPU in the guest may walk it, or
 * hold it in its paging-structure caches, until then; the rest go with the
 * VM. The allocator takes and frees a frame under a spinlock of its own,
 * so a table is made and freed under `mmu_lock`.
 */
struct tdp {
    uint64_t root; /* the top table's physical address */
    uint64_t dead; /* the first table to free after the flush, or 0 */
    uint32_t tables;
    uint64_t *pages; /* the VM's pages_4k, pages_2m and pages_1g: its leaves by level */
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
        kvm_count(KVM_COUNT_TDP_PAGE, 1);
    }
    return p;
}

/** Give the table at `phys` back. */
static void table_free(struct tdp *t, uint64_t phys)
{
    free_frame((uintptr_t)phys);
    t->tables--;
    kvm_count(KVM_COUNT_TDP_PAGE, -1);
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
 * assigned to the VM or once the caller turned
 * KVM_X86_QUIRK_IGNORE_GUEST_PAT off, write-back with the guest's PAT in
 * charge, so that the guest's uncached buffers stay uncached.
 */
static uint64_t leaf_entry(const struct kvm_vm *vm, uint64_t phys, int writable, int large,
                           int device)
{
    const struct kvm_tdp_format *f = fmt();
    uint64_t e = phys | f->read | f->exec | f->user | f->accessed;

    e |= device ? f->cache(KVM_MT_UC, 0)
                : f->cache(KVM_MT_WB,
                           vm->noncoherent == 0 && KVM_QUIRK(vm, KVM_X86_QUIRK_IGNORE_GUEST_PAT));
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
    t->pages = &vm->stat[KVM_VM_STAT_PAGES_4K];
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

uint32_t kvm_mmu_lock(struct kvm_vm *vm)
{
    return spin_lock_irqsave(&vm->mmu_lock);
}

void kvm_mmu_unlock(struct kvm_vm *vm, uint32_t flags)
{
    spin_unlock_irqrestore(&vm->mmu_lock, flags);
}

/** A vCPU leaves the guest within a moment of its kick. One that enters
 * again takes the request first and drops its translations in the entry
 * (the backend's `enter`), so the wait ends for a vCPU that is out of the
 * guest or has taken the request, as Linux's kvm_make_vcpu_request kicks
 * only a vCPU in guest mode and waits for it to take the interrupt
 * (virt/kvm/kvm_main.c:255-320). The wait answers this processor's
 * requests, since another may wait for its answer with `mmu_lock` wanted. */
void kvm_tdp_flush(struct kvm_vm *vm)
{
    uint32_t i, n = vm->nr_vcpus;
    struct kvm_vcpu *v;

    vm->stat[KVM_VM_STAT_REMOTE_TLB_FLUSH_REQUESTS]++;
    if (n) {
        vm->stat[KVM_VM_STAT_REMOTE_TLB_FLUSH]++;
    }
    for (i = 0; i < n; i++) {
        kvm_vcpu_kick(vm->vcpus[i], KVM_REQ_TLB_FLUSH);
    }
    for (i = 0; i < n; i++) {
        v = vm->vcpus[i];
        while (v->mode == KVM_MODE_IN_GUEST && (v->requests & KVM_REQ_TLB_FLUSH)) {
            smp_service();
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

/* The entries, at every level, that one batch of a zap visits between two
 * holds of `mmu_lock`: as many as 64 tables of 4 KiB leaves hold, so that a
 * batch clears up to 128 MiB of a guest mapped page by page, and covers
 * far more where whole subtrees are absent. kvm_slots_hosttest sets fewer,
 * so that its rows' zaps break. */
#ifndef TDP_ZAP_BATCH
#define TDP_ZAP_BATCH (64u * TDP_ENTRIES)
#endif

/* A zap under way: the frames [lo, hi) left to zap, the entries the batch
 * may still visit, and, when it stops for them, the first frame it did not
 * reach. */
struct zap {
    uint64_t lo, hi;
    uint64_t next;
    uint32_t budget;
    int cleared; /* a leaf went in this batch */
};

/**
 * Clear the entries of `table`, of level `level` and with `base` its first
 * frame, that meet the frames [z->lo, z->hi): a leaf goes whole, a 2 MiB
 * one that the range only meets among them, and a table below goes to the
 * dead list once it maps nothing, so that a deleted slot leaves no table
 * behind and a later 2 MiB leaf finds no table in its place. Each entry
 * looked at spends one of the batch's `budget`; an absent one is passed at
 * its own level, with the whole subtree it would have named, as Linux's
 * tdp_iter steps beside a non-present entry instead of down
 * (arch/x86/kvm/mmu/tdp_iter.c:68, 78-100, 110-122). 1 when the budget ran
 * out, with `z->next` the first frame not reached, and every frame below it
 * zapped; 0 when the table's part of the range is done.
 */
static int zap_level(struct tdp *t, uint64_t *table, int level, uint64_t base, struct zap *z)
{
    uint64_t span = 1ull << (9 * (level - 1));
    uint32_t i = z->lo > base ? (uint32_t)((z->lo - base) / span) : 0;

    for (; i < TDP_ENTRIES && base + i * span < z->hi; i++) {
        uint64_t e = table[i], from = base + i * span;

        if (z->budget == 0) {
            z->next = from > z->lo ? from : z->lo;
            return 1;
        }
        z->budget--;
        if (!present(e)) {
            continue;
        }
        if (leaf(e, level)) {
            table[i] = 0;
            t->pages[level - 1]--;
            z->cleared = 1;
            continue;
        }
        if (zap_level(t, table_at(e & fmt()->addr), level - 1, from, z)) {
            return 1;
        }
        if ((z->lo <= from && from + span <= z->hi) || table_empty(table_at(e & fmt()->addr))) {
            table[i] = 0;
            table_at(e & fmt()->addr)[0] = t->dead;
            t->dead = e & fmt()->addr;
        }
    }
    return 0;
}

/** A table on the dead list is freed only after the flush, and a table
 * that goes needs the flush even when no leaf did. The count moves first,
 * whatever the tables held, so that a fault that read the caller's entry
 * before the change maps nothing from it, and again after each break. A
 * batch ends after TDP_ZAP_BATCH entries, which every batch spends on
 * frames past the last one's, as Linux yields only after progress
 * (arch/x86/kvm/mmu/tdp_mmu.c:711-713, 859-864). */
void kvm_tdp_zap_locked(struct kvm_vm *vm, uint64_t gfn, uint64_t npages, uint32_t *flags)
{
    struct tdp *t = (struct tdp *)vm->tdp;
    struct zap z;
    int more;

    if (!t) {
        return;
    }
    vm->mmu_seq++;
    if (npages == 0 || gfn >= kvm_tdp_frames()) {
        return;
    }
    z.lo = gfn;
    z.hi = gfn + npages;
    for (;;) {
        z.budget = TDP_ZAP_BATCH;
        z.cleared = 0;
        more = zap_level(t, table_at(t->root), TDP_LEVELS, 0, &z);
        if (z.cleared || t->dead) {
            kvm_tdp_flush(vm);
        }
        while (t->dead) {
            uint64_t p = t->dead;

            t->dead = table_at(p)[0];
            table_free(t, p);
        }
        if (!more) {
            return;
        }
        z.lo = z.next;
        kvm_mmu_unlock(vm, *flags);
        *flags = kvm_mmu_lock(vm);
        vm->mmu_seq++;
    }
}

void kvm_tdp_zap(struct kvm_vm *vm, uint64_t gfn, uint64_t npages)
{
    uint32_t flags = kvm_mmu_lock(vm);

    kvm_tdp_zap_locked(vm, gfn, npages, &flags);
    kvm_mmu_unlock(vm, flags);
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
            t->pages[level - 1]--;
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
 * mapped until their next fault. What the entry held is a leaf of this
 * level, set anew, or nothing, which the VM's page counts follow. 0 or
 * -ENOMEM.
 */
static int map(struct kvm_vm *vm, struct tdp *t, uint64_t gfn, int level, uint64_t entry)
{
    uint64_t *table = table_at(t->root), *e;
    int l;

    for (l = TDP_LEVELS; l > level; l--) {
        e = &table[index_of(gfn, l)];
        if (present(*e) && l <= 3 && leaf(*e, l)) {
            *e = 0;
            t->pages[l - 1]--;
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
    e = &table[index_of(gfn, level)];
    if (!present(*e)) {
        t->pages[level - 1]++;
    }
    *e = entry;
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

/** Whether the host entry `e` stands for Linux's zero page: an anonymous
 * page read but not yet stored into (PTE_ZEROFILL without PTE_DIRTY). */
static int host_zero(pte_t e)
{
    return (e & (pte_t)(PTE_ZEROFILL | PTE_DIRTY)) == (pte_t)PTE_ZEROFILL;
}

/** Whether a region of memfd_secret meets the caller's [start, end). */
static int host_secret(struct mm *mm, uintptr_t start, uintptr_t end)
{
    const vma_t *r;

    for (r = vm_lower(&mm->vm, start); r && r->start < end; r = vm_next(r)) {
        if (r->prot & VM_PROT_SECRET) {
            return 1;
        }
    }
    return 0;
}

/**
 * Whether the caller's 512 entries of `host` are each the caller's to
 * touch and map 512 consecutive frames from a 2 MiB boundary, all RAM or
 * all a device's, with the first frame and the kind in `*out`. `*w` is
 * cleared when a page is read-only, or on a read (`write` 0) when one
 * stands for the zero page. 1, or 0 when they cannot be one leaf or a
 * write fault would need a write right `*w` lacks.
 */
static int host_block(const pte_t *host, int write, int *w, struct leaf_info *out)
{
    uint64_t base = PTE_ADDR(host[0]);
    uint32_t i;
    int dev;

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
        if (!(e & PTE_W) || (!write && host_zero(e))) {
            *w = 0;
        }
    }
    out->phys = base;
    out->device = dev;
    return !write || *w;
}

/**
 * Whether the 2 MiB of guest frames around `gfn` of the slot `s` can be one
 * leaf: they lie in the slot, which does not log, their caller's addresses
 * are one leaf table of `mm`, 2 MiB-aligned, whose 512 pages are present,
 * the caller's to touch, in no region of memfd_secret, and 512 consecutive
 * frames from a 2 MiB boundary, all RAM or all a device's, and no table of
 * `t` lies there already. The leaf is writable when every page is and the
 * slot is not read-only, and a write fault needs it so; a read fault leaves
 * it read-only while a page stands for the zero page, so that the first
 * store faults and ends the mark. 1 with the leaf in `*out`, or 0.
 */
static int large_ok(struct mm *mm, struct tdp *t, uint64_t gfn, int write,
                    const struct kvm_slot_view *s, struct leaf_info *out)
{
    uint64_t block = gfn & ~(uint64_t)(TDP_ENTRIES - 1u);
    uintptr_t hva = (uintptr_t)(s->hva - (gfn - block) * 4096u);
    uint64_t *table = table_at(t->root);
    pte_t *host;
    uint32_t i, flags;
    int l, w = !(s->flags & KVM_MEM_READONLY);

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
    if (!host || !host_ok(host[0], 0) || host_secret(mm, hva, hva + TDP_LARGE)) {
        return 0;
    }
    if (!host_block(host, write, &w, out)) {
        return 0;
    }
    if (w) {
        flags = mm_ptl_lock(mm);
        for (i = 0; i < TDP_ENTRIES; i++) {
            if (host[i] & PTE_P) {
                host[i] &= ~(pte_t)PTE_ZEROFILL;
            }
        }
        mm_ptl_unlock(mm, flags);
    }
    out->writable = w;
    return 1;
}

/**
 * Map the 4 KiB leaf of `gfn` in slot `s` from the caller's entry `pte`,
 * for a fault that `write` names: the leaf result of `map`. Under the
 * regions' lock held to read and `mmu_lock`, as `kvm_tdp_fault` holds them.
 */
static int map_small(struct kvm_vm *vm, uint64_t gfn, int write, const struct kvm_slot_view *s,
                     pte_t pte)
{
    struct tdp *t = (struct tdp *)vm->tdp;
    uint64_t phys = PTE_ADDR(pte), i;
    int writable = (pte & PTE_W) && !(s->flags & KVM_MEM_READONLY) && (write || !host_zero(pte));

    if (s->dirty) {
        i = gfn - s->base_gfn;
        if (write) {
            s->dirty[i / 64u] |= 1ull << (i % 64u);
        } else if (!((s->dirty[i / 64u] >> (i % 64u)) & 1u)) {
            writable = 0;
        }
    }
    if (writable) {
        paging_entry_clear(kvm_vm_mm(vm), (uintptr_t)s->hva, (pte_t)PTE_ZEROFILL);
    }
    return map(vm, t, gfn, 1, leaf_entry(vm, phys, writable, 0, paging_frame_is_device(phys)));
}

/**
 * The fault follows Linux's kvm_faultin_pfn, is_page_fault_stale and
 * kvm_tdp_mmu_page_fault (arch/x86/kvm/mmu/mmu.c:4459-4598, 4699-4731):
 * `mmu_seq` and the address space's count of finished page changes are
 * read first, the slot looked up, and the caller's entry read under the
 * regions' lock held to read, which keeps every change of the regions out;
 * the leaf is set under `mmu_lock` only while neither count has moved and
 * no page change is under way (`mm->changing`), as Linux's
 * mmu_invalidate_retry_gfn checks mmu_invalidate_seq and
 * mmu_invalidate_in_progress. A change of a page by a holder of the
 * regions' lock to read, a copy-on-write break or a pin, counts itself
 * under way from before its notice, which zaps the slot's leaves and moves
 * `mmu_seq` under `mmu_lock`, to after the new entry, so a fault that read
 * the old entry either sees the change under way or sees it finished and
 * looks again, and one whose leaf came first has it zapped. A slot that
 * moves, changes its flags or goes zaps under `mmu_lock` too.
 *
 * The caller's page is brought in with `vm_prefault_range`, which may
 * sleep, with no lock held, and the fault starts again. A page of
 * memfd_secret is -EFAULT, as Linux's get_user_pages refuses one.
 *
 * A page that stands for the zero page (PTE_ZEROFILL) is mapped read-only
 * on a read, as Linux maps its zero page, and a write fault, which maps it
 * writable, ends the host entry's mark under the page-table lock, since
 * the guest's stores set no dirty bit in it.
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
    const vma_t *r;
    uint32_t seq, changes, flags;
    int rc;
    pte_t pte;

    for (;;) {
        seq = vm->mmu_seq;
        changes = mm ? mm->changes : 0;
        k_barrier();
        if (!kvm_slot_find(vm, gfn, &s) || (write && (s.flags & KVM_MEM_READONLY))) {
            return 1;
        }
        if (!t || !mm || !current_proc || current_proc->mm != mm) {
            return -EFAULT;
        }
        mmap_read_lock(mm);
        if (mm->changing) {
            mmap_unlock(mm);
            arch_relax();
            continue;
        }
        r = vm_find(&mm->vm, (uintptr_t)s.hva);
        if (r && (r->prot & VM_PROT_SECRET)) {
            mmap_unlock(mm);
            return -EFAULT;
        }
        pte = host_pte(mm, (uintptr_t)s.hva);
        if (!host_ok(pte, write)) {
            mmap_unlock(mm);
            if (!vm_prefault_range((uintptr_t)s.hva, 4096u, write)) {
                return -EFAULT;
            }
            continue;
        }
        flags = kvm_mmu_lock(vm);
        if (vm->mmu_seq == seq && !mm->changing && mm->changes == changes) {
            break;
        }
        kvm_mmu_unlock(vm, flags);
        mmap_unlock(mm);
    }
    if (large_ok(mm, t, gfn, write, &s, &big)) {
        rc = map(vm, t, gfn, 2, leaf_entry(vm, big.phys, big.writable, 1, big.device));
    } else {
        rc = map_small(vm, gfn, write, &s, pte);
    }
    kvm_mmu_unlock(vm, flags);
    mmap_unlock(mm);
    return rc;
}

/**
 * The slot is looked up and the caller's page read from its tables under
 * `mmu_lock`, with nothing between the read and the write that answers
 * another processor's requests, and written through the kernel's map of
 * the frame, so nothing can fault. A page not yet stored into (no
 * PTE_DIRTY) is refused with the rest, since this write would not set the
 * bit: the zero page's rules and the counts of dirty pages read it.
 */
int kvm_tdp_write_nofault(struct kvm_vm *vm, uint64_t gpa, const void *buf, uint32_t n,
                          uint32_t seq)
{
    struct mm *mm = kvm_vm_mm(vm);
    struct kvm_slot_view s;
    uint32_t flags;
    uint64_t i;
    pte_t pte;
    int rc = 1;

    if (n == 0 || (gpa & 0xFFFu) + n > 4096u || !mm || !current_proc || current_proc->mm != mm) {
        return 1;
    }
    flags = kvm_mmu_lock(vm);
    if (vm->mmu_seq != seq || !kvm_slot_find_locked(vm, gpa >> 12, &s) ||
        (s.flags & KVM_MEM_READONLY)) {
        goto out;
    }
    pte = host_pte(mm, (uintptr_t)s.hva);
    if (!host_ok(pte, 1) || !(pte & PTE_DIRTY) || paging_frame_is_device(PTE_ADDR(pte))) {
        goto out;
    }
    k_memcpy((uint8_t *)phys_to_virt(PTE_ADDR(pte)) + (gpa & 0xFFFu), buf, n);
    if (s.dirty) {
        i = (gpa >> 12) - s.base_gfn;
        s.dirty[i / 64u] |= 1ull << (i % 64u);
    }
    rc = 0;
out:
    kvm_mmu_unlock(vm, flags);
    return rc;
}

int32_t kvm_tdp_pre_fault(struct kvm_vm *vm, struct kvm_pre_fault_memory *range)
{
    uint64_t size = range->size, step;
    uint32_t flags;
    int32_t r = 0;
    int level;

    if (range->flags || ((range->gpa | range->size) & 4095u) != 0 ||
        range->gpa + range->size <= range->gpa) {
        return -EINVAL;
    }
    while (range->size) {
        if (signal_pending()) {
            r = -EINTR;
            break;
        }
        r = kvm_tdp_fault(vm, range->gpa >> 12, 0);
        if (r != 0) {
            r = r > 0 ? -ENOENT : r;
            break;
        }
        step = 4096u;
        flags = kvm_mmu_lock(vm);
        if (find_leaf((struct tdp *)vm->tdp, range->gpa >> 12, &level) && level == 2) {
            step = TDP_LARGE - (range->gpa & (TDP_LARGE - 1u));
        }
        kvm_mmu_unlock(vm, flags);
        if (step > range->size) {
            step = range->size;
        }
        range->gpa += step;
        range->size -= step;
    }
    return size == range->size ? r : 0;
}

/** The count changes under `mmu_lock`, which a fault holds while it reads
 * the count for a leaf's type. */
void kvm_tdp_noncoherent(struct kvm_vm *vm, int assigned)
{
    uint32_t before, flags = kvm_mmu_lock(vm);

    before = vm->noncoherent;
    if (assigned) {
        vm->noncoherent++;
    } else if (vm->noncoherent) {
        vm->noncoherent--;
    }
    /* The first such device and the last one change every RAM leaf's
     * type, so every leaf goes and the next fault makes it anew, as Linux's
     * kvm_noncoherent_dma_assignment_start_or_stop zaps them. */
    if ((before == 0) != (vm->noncoherent == 0)) {
        kvm_tdp_zap_locked(vm, 0, kvm_tdp_frames(), &flags);
    }
    kvm_mmu_unlock(vm, flags);
}
