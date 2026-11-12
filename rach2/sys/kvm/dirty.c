/*
 * dirty.c: dirty logging, the log of the pages of a slot the guest wrote,
 * which QEMU reads to redraw a display and to migrate, as Linux's
 * KVM_GET_DIRTY_LOG and KVM_CLEAR_DIRTY_LOG give it. A log is read and
 * written under the VM's `mmu_lock`, which a fault from a vCPU's run holds
 * while it marks a page, as Linux's kvm_get_dirty_log_protect takes
 * mmu_lock (virt/kvm/kvm_main.c:2202-2312). With dirty rings enabled the
 * running vCPU publishes the pages in its shared ring instead.
 */

#include "kvm.h"

#include "../lib/kerrno.h"
#include "../mm/kheap.h"
#include "../mm/usercopy.h"

int32_t kvm_dirty_enable_ring(struct kvm_vm *vm, const struct kvm_enable_cap *cap)
{
    uint32_t size = (uint32_t)cap->args[0];

    /* Linux's ioctl takes the argument as u32. */
    if (cap->flags || !size || (size & (size - 1u)) || size < 4096u) {
        return -EINVAL;
    }
    if (size > KVM_DIRTY_RING_MAX_BYTES) {
        return -E2BIG;
    }
    if (vm->dirty_ring_size || vm->nr_vcpus) {
        return -EINVAL;
    }
    vm->dirty_ring_size = size;
    return 0;
}

int32_t kvm_dirty_enable_manual(struct kvm_vm *vm, const struct kvm_enable_cap *cap)
{
    uint64_t allowed = KVM_DIRTY_LOG_MANUAL_PROTECT_ENABLE;

    if (cap->args[0] & KVM_DIRTY_LOG_MANUAL_PROTECT_ENABLE) {
        allowed |= KVM_DIRTY_LOG_INITIALLY_SET;
    }
    if (cap->flags || (cap->args[0] & ~allowed)) {
        return -EINVAL;
    }
    vm->dirty_manual = (uint32_t)cap->args[0];
    return 0;
}

/** Reserve an entry for `v` under mmu_lock, leaving its flags clear until
 * the write finishes. 0 with *out NULL for a bitmap or an unlogged slot,
 * 0 with the entry for a ring, or -ENOSPC when every entry is in use. */
static int reserve(struct kvm_vm *vm, struct kvm_vcpu *v, const struct kvm_slot_view *s,
                   uint64_t gfn, struct kvm_dirty_gfn **out)
{
    struct kvm_dirty_gfn *entry;
    uint32_t entries;

    *out = NULL;
    if (!(s->flags & KVM_MEM_LOG_DIRTY_PAGES) || !vm->dirty_ring_size || !v) {
        return 0;
    }
    entries = vm->dirty_ring_size / sizeof(*entry);
    if (v->dirty_ring_put - v->dirty_ring_reset >= entries) {
        return -ENOSPC;
    }
    entry = &v->dirty_ring[v->dirty_ring_put & (entries - 1u)];
    entry->slot = s->id;
    entry->offset = gfn - s->base_gfn;
    v->dirty_ring_put++;
    *out = entry;
    return 0;
}

void kvm_dirty_publish(struct kvm_dirty_gfn *entry)
{
    /* x86's stores are ordered; publish only after the guest-page write. */
    k_barrier();
    *(volatile uint32_t *)&entry->flags = KVM_DIRTY_GFN_F_DIRTY;
}

int kvm_dirty_reserve(struct kvm_vcpu *v, const struct kvm_slot_view *s, uint64_t gfn,
                      struct kvm_dirty_gfn **entry)
{
    uint32_t flags = kvm_mmu_lock(v->vm);
    int r = reserve(v->vm, v, s, gfn, entry);

    kvm_mmu_unlock(v->vm, flags);
    return r;
}

/** Record a completed write under mmu_lock. A full ring is -ENOSPC;
 * software copies reserve their entry before writing to avoid that case. */
static int mark(struct kvm_vm *vm, struct kvm_vcpu *v, const struct kvm_slot_view *s, uint64_t gfn)
{
    struct kvm_dirty_gfn *entry;
    uint64_t i = gfn - s->base_gfn;
    int r = reserve(vm, v, s, gfn, &entry);

    if (r != 0) {
        return r;
    }
    if (entry) {
        kvm_dirty_publish(entry);
    } else if (s->dirty) {
        s->dirty[i / 64u] |= 1ull << (i % 64u);
    }
    return 0;
}

int kvm_dirty_mark_locked(struct kvm_vm *vm, const struct kvm_slot_view *s, uint64_t gfn)
{
    return mark(vm, vm->dirty_ring_size ? kvm_running_vcpu(vm) : NULL, s, gfn);
}

int kvm_dirty_reserve_locked(struct kvm_vm *vm, const struct kvm_slot_view *s, uint64_t gfn,
                             struct kvm_dirty_gfn **entry)
{
    return reserve(vm, vm->dirty_ring_size ? kvm_running_vcpu(vm) : NULL, s, gfn, entry);
}

int kvm_dirty_mark(struct kvm_vcpu *v, const struct kvm_slot_view *s, uint64_t gfn)
{
    struct kvm_vm *vm = v->vm;
    uint32_t flags;
    int r;

    flags = kvm_mmu_lock(vm);
    r = mark(vm, v, s, gfn);
    kvm_mmu_unlock(vm, flags);
    return r;
}

/**
 * Reset the collected prefix of every ring, as Linux's
 * kvm_dirty_ring_reset: copy each caller-writable entry once, validate its
 * slot and offset, and protect that page again before freeing the entry.
 * mmu_lock serializes producers and the reset indices; the VM ioctl's big
 * lock serializes resets and slot changes. Batches of 64 entries let
 * interrupts run. A final flush ends every old writable translation
 * before the ioctl returns the count, including entries of deleted slots.
 */
static int32_t reset_rings(struct kvm_vm *vm)
{
    struct kvm_vcpu *v;
    struct kvm_dirty_gfn *entry;
    struct kvm_slot_view s;
    uint32_t n, i, flags, slot, batch, entries;
    uint64_t offset;
    int32_t count = 0;

    if (!vm->dirty_ring_size) {
        return -EINVAL;
    }
    entries = vm->dirty_ring_size / sizeof(*entry);
    for (n = 0; n < vm->nr_vcpus; n++) {
        v = vm->vcpus[n];
        for (;;) {
            flags = kvm_mmu_lock(vm);
            for (batch = 0; batch < 64u && v->dirty_ring_reset != v->dirty_ring_put; batch++) {
                i = v->dirty_ring_reset & (entries - 1u);
                entry = &v->dirty_ring[i];
                if (!(*(volatile uint32_t *)&entry->flags & KVM_DIRTY_GFN_F_RESET)) {
                    break;
                }
                /* x86's loads are ordered; acquire the caller's RESET. */
                k_barrier();
                slot = *(volatile uint32_t *)&entry->slot;
                offset = *(volatile uint64_t *)&entry->offset;
                if (slot < KVM_MEM_SLOTS && kvm_slot_get(vm, slot, &s) && offset < s.npages) {
                    (void)kvm_tdp_protect(vm, s.base_gfn + offset, 1);
                }
                k_barrier();
                *(volatile uint32_t *)&entry->flags = 0;
                v->dirty_ring_reset++;
                count++;
            }
            kvm_mmu_unlock(vm, flags);
            if (batch < 64u) {
                break;
            }
        }
    }
    if (count) {
        flags = kvm_mmu_lock(vm);
        kvm_tdp_flush(vm);
        kvm_mmu_unlock(vm, flags);
    }
    return count;
}

/** The slot of a log ioctl's `slot` into `*s`: 0, -EINVAL for an address
 * space other than 0 or a number past KVM_MEM_SLOTS, or -ENOENT for no
 * slot or one that does not log. Under `mmu_lock`. */
static int32_t log_slot(struct kvm_vm *vm, uint32_t slot, struct kvm_slot_view *s)
{
    if ((slot >> 16) != 0 || (slot & 0xFFFFu) >= KVM_MEM_SLOTS) {
        return -EINVAL;
    }
    if (!kvm_slot_get(vm, slot & 0xFFFFu, s) || !s->dirty) {
        return -ENOENT;
    }
    return 0;
}

/**
 * KVM_GET_DIRTY_LOG, as Linux's kvm_get_dirty_log_protect: the log of the
 * slot to the caller's bitmap. Without KVM_DIRTY_LOG_MANUAL_PROTECT_ENABLE
 * the log is taken and cleared, and the pages it named write-protected
 * again before the copy, so that a write after the call is logged anew;
 * with it the log is copied as it stands, for KVM_CLEAR_DIRTY_LOG to clear.
 * The log is copied to a buffer of the kernel's first, under `mmu_lock`,
 * since the copy to the caller may sleep, and the slot may go meanwhile;
 * the buffer is had before the lock, for the slot's size then, and the
 * slot looked up again under it. 0, -EINVAL, -ENOENT, -ENOMEM or -EFAULT.
 */
static int32_t get_log(struct kvm_vm *vm, uintptr_t arg)
{
    struct kvm_dirty_log log;
    struct kvm_slot_view s;
    uint64_t *out, words, i;
    uint32_t flags;
    int32_t r;
    int flush = 0;

    if (copy_from_user(&log, (const void __user *)arg, sizeof(log)) != 0) {
        return -EFAULT;
    }
    if (vm->dirty_ring_size) {
        return -ENXIO;
    }
    flags = kvm_mmu_lock(vm);
    r = log_slot(vm, log.slot, &s);
    kvm_mmu_unlock(vm, flags);
    if (r != 0) {
        return r;
    }
    words = (s.npages + 63u) / 64u;
    out = kmalloc((size_t)(words * 8u));
    if (!out) {
        return -ENOMEM;
    }
    flags = kvm_mmu_lock(vm);
    r = log_slot(vm, log.slot, &s);
    if (r == 0 && (s.npages + 63u) / 64u != words) {
        r = -ENOENT; /* the slot was made anew meanwhile */
    }
    for (i = 0; r == 0 && i < words; i++) {
        out[i] = s.dirty[i];
        if (!(vm->dirty_manual & KVM_DIRTY_LOG_MANUAL_PROTECT_ENABLE) && out[i]) {
            s.dirty[i] = 0;
            flush |= kvm_tdp_protect(vm, s.base_gfn + i * 64u, out[i]);
        }
    }
    if (flush) {
        kvm_tdp_flush(vm);
    }
    kvm_mmu_unlock(vm, flags);
    if (r == 0 &&
        copy_to_user((void __user *)(uintptr_t)log.dirty_bitmap, out, (size_t)(words * 8u)) != 0) {
        r = -EFAULT;
    }
    kfree(out);
    return r;
}

/**
 * KVM_CLEAR_DIRTY_LOG, as Linux's kvm_clear_dirty_log_protect: clear the
 * bits the caller's bitmap sets of `num_pages` pages from `first_page`,
 * which is a multiple of 64, and write-protect the pages whose bits were
 * set, so that their next write is logged. The range must lie in the slot,
 * and end at its end when `num_pages` is not a multiple of 64; bits past
 * the slot's end are ignored. The caller's bitmap is read before the slot
 * is looked up again under `mmu_lock`, since the read may sleep. 0,
 * -EINVAL, -ENOENT, -ENOMEM or -EFAULT.
 */
static int32_t clear_log(struct kvm_vm *vm, uintptr_t arg)
{
    struct kvm_clear_dirty_log c;
    struct kvm_slot_view s;
    uint64_t *in, words, i;
    uint32_t flags;
    int32_t r;
    int flush = 0;

    if (copy_from_user(&c, (const void __user *)arg, sizeof(c)) != 0) {
        return -EFAULT;
    }
    if (vm->dirty_ring_size) {
        return -ENXIO;
    }
    if ((c.slot >> 16) != 0 || (c.slot & 0xFFFFu) >= KVM_MEM_SLOTS || (c.first_page & 63u)) {
        return -EINVAL;
    }
    flags = kvm_mmu_lock(vm);
    r = log_slot(vm, c.slot, &s);
    kvm_mmu_unlock(vm, flags);
    if (r != 0) {
        return r;
    }
    if (c.first_page > s.npages || c.num_pages > s.npages - c.first_page ||
        (c.num_pages < s.npages - c.first_page && (c.num_pages & 63u))) {
        return -EINVAL;
    }
    words = ((uint64_t)c.num_pages + 63u) / 64u;
    if (words == 0) {
        return 0;
    }
    in = kmalloc((size_t)(words * 8u));
    if (!in) {
        return -ENOMEM;
    }
    if (copy_from_user(in, (const void __user *)(uintptr_t)c.dirty_bitmap, (size_t)(words * 8u)) !=
        0) {
        kfree(in);
        return -EFAULT;
    }
    flags = kvm_mmu_lock(vm);
    r = log_slot(vm, c.slot, &s);
    if (r == 0 && c.first_page + c.num_pages > s.npages) {
        r = -EINVAL;
    }
    for (i = 0; r == 0 && i < words; i++) {
        uint64_t w = c.first_page / 64u + i;
        uint64_t mask = in[i] & s.dirty[w];

        if (mask) {
            s.dirty[w] &= ~mask;
            flush |= kvm_tdp_protect(vm, s.base_gfn + w * 64u, mask);
        }
    }
    if (flush) {
        kvm_tdp_flush(vm);
    }
    kvm_mmu_unlock(vm, flags);
    kfree(in);
    return r;
}

int32_t kvm_dirty_ioctl(struct kvm_vm *vm, uint32_t request, uintptr_t arg)
{
    if (request == KVM_RESET_DIRTY_RINGS) {
        return reset_rings(vm);
    }
    switch (request) {
    case KVM_GET_DIRTY_LOG:
        return get_log(vm, arg);
    case KVM_CLEAR_DIRTY_LOG:
        return clear_log(vm, arg);
    default:
        return -ENOTTY;
    }
}
