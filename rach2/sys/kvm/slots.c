/*
 * slots.c: a VM's memory slots, ranges of the caller's memory that the
 * guest sees at guest-physical addresses, each on the address space's
 * change notice so that the guest-physical tables follow every change to
 * the pages behind it.
 */

#include "kvm.h"

#include "../arch/arch.h"
#include "../lib/kerrno.h"
#include "../mm/kheap.h"
#include "../mm/notify.h"

/* The most pages a slot has, Linux's KVM_MEM_MAX_NR_PAGES. */
#define SLOT_MAX_PAGES 0x7FFFFFFFull

/* The flags KVM_SET_USER_MEMORY_REGION takes. */
#define SLOT_FLAGS (KVM_MEM_LOG_DIRTY_PAGES | KVM_MEM_READONLY)

/*
 * A slot. `note` covers [hva, hva + npages pages) of the VM's address
 * space, and its `invalidate` drops what the tables map of the part that
 * changes; `note.mm` is NULL once the address space has ended.
 */
struct slot {
    struct mmn_range note;
    struct kvm_vm *vm;
    uint32_t id;
    uint32_t flags; /* KVM_MEM_* */
    uint64_t base_gfn;
    uint64_t npages;
    uint64_t hva;
    uint64_t *dirty; /* the dirty log, a bit a page, while it logs */
};

/* A VM's slots, by number; `last` is the one `kvm_slot_find` last found. */
struct slots {
    struct slot *by_id[KVM_MEM_SLOTS];
    struct slot *last;
    uint64_t pages; /* of every slot, which Linux bounds by an unsigned long */
};

/** The bytes of a dirty log of `npages` pages, whole 64-bit words as
 * Linux's kvm_dirty_bitmap_bytes. */
static uint64_t log_bytes(uint64_t npages)
{
    return (npages + 63u) / 64u * 8u;
}

/** A dirty log for `s`, all clear or, with KVM_DIRTY_LOG_INITIALLY_SET,
 * its pages all set, as Linux's kvm_alloc_dirty_bitmap and its caller
 * make it: the log or NULL. */
static uint64_t *log_new(struct kvm_vm *vm, uint64_t npages)
{
    uint64_t bytes = log_bytes(npages);
    uint64_t *log;
    uint64_t i;

    if (bytes != (size_t)bytes) {
        return NULL;
    }
    log = kzalloc((size_t)bytes);
    if (log && (vm->dirty_manual & KVM_DIRTY_LOG_INITIALLY_SET)) {
        for (i = 0; i < npages / 64u; i++) {
            log[i] = ~0ull;
        }
        if (npages % 64u) {
            log[npages / 64u] = (1ull << (npages % 64u)) - 1u;
        }
    }
    return log;
}

/**
 * The change notice of a slot's range: drop what the tables map of the
 * frames behind [start, end) before the pages change, and every vCPU out of
 * the guest, so that no frame is reused while a guest reaches it. At the
 * end of the address space the whole slot goes from the tables; the slot
 * stays until it is deleted or the VM ends, and no fault can map it again,
 * since no thread of that address space is left to make one.
 */
static void slot_invalidate(struct mmn_range *r, int event, uintptr_t start, uintptr_t end)
{
    struct slot *s = (struct slot *)r->owner;

    (void)event;
    kvm_tdp_zap(s->vm, s->base_gfn + (start - s->hva) / 4096u, (end - start) / 4096u);
}

static const struct mmn_ops slot_ops = {slot_invalidate};

int kvm_slots_init(struct kvm_vm *vm)
{
    vm->slots = kzalloc(sizeof(struct slots));
    return vm->slots ? 0 : -ENOMEM;
}

/** Free `s`, off the notice. */
static void slot_free(struct slot *s)
{
    kvm_count(KVM_COUNT_SLOT, -1);
    mmn_unregister(&s->note);
    kfree(s->dirty);
    kfree(s);
}

void kvm_slots_free(struct kvm_vm *vm)
{
    struct slots *t = (struct slots *)vm->slots;
    uint32_t i;

    if (!t) {
        return;
    }
    for (i = 0; i < KVM_MEM_SLOTS; i++) {
        if (t->by_id[i]) {
            slot_free(t->by_id[i]);
        }
    }
    kfree(t);
    vm->slots = NULL;
}

/** Whether a slot other than `id` holds any frame of [gfn, gfn + npages). */
static int overlaps(const struct slots *t, uint32_t id, uint64_t gfn, uint64_t npages)
{
    uint32_t i;

    for (i = 0; i < KVM_MEM_SLOTS; i++) {
        const struct slot *s = t->by_id[i];

        if (s && i != id && s->base_gfn < gfn + npages && gfn < s->base_gfn + s->npages) {
            return 1;
        }
    }
    return 0;
}

/** Delete `s`, the slot `id`: its frames out of the tables, then the
 * record. */
static int32_t slot_delete(struct kvm_vm *vm, struct slots *t, struct slot *s)
{
    t->by_id[s->id] = NULL;
    if (t->last == s) {
        t->last = NULL;
    }
    t->pages -= s->npages;
    kvm_gpamap_zap(vm, s->base_gfn, s->npages);
    kvm_tdp_zap(vm, s->base_gfn, s->npages);
    slot_free(s);
    return 0;
}

/** Make the slot `id` from `r`, its range on the address space's notice. */
static int32_t slot_create(struct kvm_vm *vm, struct slots *t, uint32_t id,
                           const struct kvm_userspace_memory_region *r)
{
    struct mm *mm = kvm_vm_mm(vm);
    struct slot *s;

    s = kzalloc(sizeof(*s));
    if (!s) {
        return -ENOMEM;
    }
    s->vm = vm;
    s->id = id;
    s->flags = r->flags;
    s->base_gfn = r->guest_phys_addr / 4096u;
    s->npages = r->memory_size / 4096u;
    s->hva = r->userspace_addr;
    if (s->flags & KVM_MEM_LOG_DIRTY_PAGES) {
        s->dirty = log_new(vm, s->npages);
        if (!s->dirty) {
            kfree(s);
            return -ENOMEM;
        }
    }
    if (mm) {
        mmn_register(mm, &s->note, (uintptr_t)s->hva, (uintptr_t)(s->hva + r->memory_size),
                     &slot_ops, s);
    }
    kvm_count(KVM_COUNT_SLOT, 1);
    t->by_id[id] = s;
    t->pages += s->npages;
    return 0;
}

/** Move `s` to the frame `gfn`, or change its flags to `flags`: what the
 * tables map of it goes first, so that each frame is mapped again by the
 * new rules at its next fault. */
static int32_t slot_change(struct kvm_vm *vm, struct slot *s, uint64_t gfn, uint32_t flags)
{
    uint64_t *log = s->dirty;

    if ((flags & KVM_MEM_LOG_DIRTY_PAGES) && !log) {
        log = log_new(vm, s->npages);
        if (!log) {
            return -ENOMEM;
        }
    } else if (!(flags & KVM_MEM_LOG_DIRTY_PAGES)) {
        log = NULL;
    }
    if (gfn != s->base_gfn) {
        kvm_gpamap_zap(vm, s->base_gfn, s->npages);
    }
    kvm_tdp_zap(vm, s->base_gfn, s->npages);
    if (log != s->dirty) {
        kfree(s->dirty);
        s->dirty = log;
    }
    s->base_gfn = gfn;
    s->flags = flags;
    return 0;
}

/** The checks of KVM_SET_USER_MEMORY_REGION that need no slot, in the
 * order of Linux's kvm_set_memory_region: 0 or -EINVAL. */
static int32_t region_ok(const struct kvm_userspace_memory_region *r)
{
    if (r->flags & ~SLOT_FLAGS) {
        return -EINVAL;
    }
    if ((r->memory_size & 4095u) || (r->guest_phys_addr & 4095u) || (r->userspace_addr & 4095u)) {
        return -EINVAL;
    }
    if (r->userspace_addr + r->memory_size < r->userspace_addr ||
        r->userspace_addr + r->memory_size > USER_ADDR_MAX) {
        return -EINVAL;
    }
    if ((r->slot >> 16) != 0 || (r->slot & 0xFFFFu) >= KVM_MEM_SLOTS) {
        return -EINVAL;
    }
    if (r->guest_phys_addr + r->memory_size < r->guest_phys_addr ||
        r->memory_size / 4096u > SLOT_MAX_PAGES) {
        return -EINVAL;
    }
    return 0;
}

/** The rest of the checks and their order are Linux's
 * kvm_set_memory_region and kvm_arch_prepare_memory_region's. */
int32_t kvm_slots_set(struct kvm_vm *vm, const struct kvm_userspace_memory_region *r)
{
    struct slots *t = (struct slots *)vm->slots;
    uint32_t id = r->slot & 0xFFFFu;
    uint64_t gfn = r->guest_phys_addr / 4096u, npages = r->memory_size / 4096u;
    int32_t err = region_ok(r);
    struct slot *old;
    int placed;

    if (err != 0) {
        return err;
    }
    old = t->by_id[id];
    if (npages == 0) {
        return old ? slot_delete(vm, t, old) : -EINVAL;
    }
    if (old) {
        if (r->userspace_addr != old->hva || npages != old->npages ||
            ((r->flags ^ old->flags) & KVM_MEM_READONLY)) {
            return -EINVAL;
        }
        if (gfn == old->base_gfn && r->flags == old->flags) {
            return 0;
        }
    } else if (t->pages + npages < t->pages) {
        return -EINVAL;
    }
    placed = !old || gfn != old->base_gfn;
    if (placed && gfn + npages - 1u >= kvm_tdp_frames()) {
        return -EINVAL;
    }
    if (placed && overlaps(t, id, gfn, npages)) {
        return -EEXIST;
    }
    return old ? slot_change(vm, old, gfn, r->flags) : slot_create(vm, t, id, r);
}

/** Fill `out` from `s` for its frame `gfn`. */
static void view(const struct slot *s, uint64_t gfn, struct kvm_slot_view *out)
{
    out->base_gfn = s->base_gfn;
    out->npages = s->npages;
    out->flags = s->flags;
    out->id = s->id;
    out->hva = s->hva + (gfn - s->base_gfn) * 4096u;
    out->dirty = s->dirty;
}

/** A guest has a few slots, so a walk of the table, with the last slot
 * found tried first, costs less than keeping them sorted. */
int kvm_slot_find(struct kvm_vm *vm, uint64_t gfn, struct kvm_slot_view *out)
{
    struct slots *t = (struct slots *)vm->slots;
    struct slot *s = t ? t->last : NULL;
    uint32_t i;

    if (!s || gfn < s->base_gfn || gfn - s->base_gfn >= s->npages) {
        s = NULL;
        for (i = 0; t && i < KVM_MEM_SLOTS; i++) {
            struct slot *c = t->by_id[i];

            if (c && gfn >= c->base_gfn && gfn - c->base_gfn < c->npages) {
                s = c;
                t->last = c;
                break;
            }
        }
    }
    if (!s) {
        return 0;
    }
    view(s, gfn, out);
    return 1;
}

int kvm_slot_get(struct kvm_vm *vm, uint32_t id, struct kvm_slot_view *out)
{
    struct slots *t = (struct slots *)vm->slots;
    struct slot *s = t && id < KVM_MEM_SLOTS ? t->by_id[id] : NULL;

    if (!s) {
        return 0;
    }
    view(s, s->base_gfn, out);
    return 1;
}

int32_t kvm_slots_check_extension(long cap)
{
    switch (cap) {
    case KVM_CAP_USER_MEMORY:
    case KVM_CAP_USER_MEMORY2:
    case KVM_CAP_DESTROY_MEMORY_REGION_WORKS:
    case KVM_CAP_JOIN_MEMORY_REGIONS_WORKS:
    case KVM_CAP_READONLY_MEM:
    case KVM_CAP_SYNC_MMU:
        return 1;
    case KVM_CAP_NR_MEMSLOTS:
        return KVM_MEM_SLOTS;
    case KVM_CAP_MANUAL_DIRTY_LOG_PROTECT2:
        return (int32_t)(KVM_DIRTY_LOG_MANUAL_PROTECT_ENABLE | KVM_DIRTY_LOG_INITIALLY_SET);
    default:
        return 0;
    }
}
