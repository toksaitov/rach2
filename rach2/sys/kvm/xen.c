/*
 * xen.c: Xen's HVM interface for a guest, as Linux's arch/x86/kvm/xen.c
 * gives it (KVM_CAP_XEN_HVM): the hypercall page and the hypercalls the
 * kernel answers, the shared info page with its two-level event channels,
 * each vCPU's info, time and runstate areas, its one-shot timer, and
 * SCHEDOP_poll.
 */

#include "kvm.h"

#include "../io/eventfd.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"
#include "../mm/usercopy.h"
#include "../proc/mutex.h"
#include "../proc/process.h"

/* The hypercalls the kernel may answer, Xen's include/public/xen.h. */
#define HYPERVISOR_set_timer_op     15u
#define HYPERVISOR_xen_version      17u
#define HYPERVISOR_vcpu_op          24u
#define HYPERVISOR_sched_op         29u
#define HYPERVISOR_event_channel_op 32u

#define XENVER_version               0u
#define EVTCHNOP_send                4u
#define SCHEDOP_yield                0u
#define SCHEDOP_poll                 3u
#define VCPUOP_set_singleshot_timer  8u
#define VCPUOP_stop_singleshot_timer 9u

#define EVTCHNSTAT_interdomain 2u
#define EVTCHNSTAT_ipi         5u

/* The errors a hypercall returns in RAX, Xen's, which are Linux's. */
#define XEN_EFAULT 14
#define XEN_EINVAL 22
#define XEN_ENOMEM 12
#define XEN_ENOSYS 38

#define RUNSTATE_running  0u
#define RUNSTATE_runnable 1u
#define RUNSTATE_offline  3u

/* The top bit of state_entry_time, set while the host writes the record. */
#define XEN_RUNSTATE_UPDATE 0x8000000000000000ull

/* The ports of a two-level event channel ABI: 64 words of 64 bits for a
 * 64-bit guest, 32 of 32 for a 32-bit one. */
#define PORTS_64 4096u
#define PORTS_32 1024u

/* The shared info page: the pending and mask bitmaps and the wall clock,
 * whose 64-bit layout puts the mask after 64 words of 64 bits and the
 * clock after 32 vCPUs' info and both bitmaps. */
#define SHINFO_PENDING      0x800u
#define SHINFO_MASK_64      0xA00u
#define SHINFO_MASK_32      0x880u
#define SHINFO_WC_64        0xC00u
#define SHINFO_WC_32        0x900u
#define SHINFO_WC_SEC_HI_64 0xC0Cu
#define SHINFO_WC_SEC_HI_32 0x924u

/* A vCPU's vcpu_info: evtchn_upcall_pending at 0, evtchn_pending_sel a
 * word at 8 or 4, and the time at 0x20, 64 bytes in both layouts. */
#define INFO_SIZE   64u
#define INFO_SEL_64 8u
#define INFO_SEL_32 4u
#define INFO_TIME   0x20u
#define TIME_SIZE   32u

/* struct vcpu_runstate_info: the state, then state_entry_time and the four
 * times, at 8 in the 64-bit layout and at 4 in the 32-bit one. */
#define RS_SIZE_64  0x30u
#define RS_SIZE_32  0x2Cu
#define RS_TIMES_64 8u
#define RS_TIMES_32 4u

/* The most ports one SCHEDOP_poll waits for, Xen's unofficial bound. */
#define POLL_PORTS_MAX 128u

#define NSEC_PER_MSEC 1000000ull

/* Accesses of `mem_page`. */
#define MEM_READ  0
#define MEM_WRITE 1
#define MEM_OR    2

/* A guest area the kernel writes: guest-physical, or for `hva` the caller's
 * own address, as Linux's gfn_to_pfn_cache in either mode. */
struct xen_area {
    int active;
    int hva;
    uint64_t addr;
};

/* An outbound event channel, KVM_XEN_ATTR_TYPE_EVTCHN: a guest's
 * EVTCHNOP_send of `send_port` raises `port` of the vCPU whose APIC id is
 * `vcpu`, or for `port` 0 signals `ev`. */
struct xen_port {
    uint32_t send_port;
    uint32_t type; /* EVTCHNSTAT_ipi or EVTCHNSTAT_interdomain */
    uint32_t port;
    uint32_t vcpu;
    uint32_t priority;
    struct eventfd *ev; /* held while assigned */
    struct xen_port *next;
};

/*
 * xen.c's share of a VM. `lock` is over all of it and over every vCPU's
 * areas, shadow and poll, as Linux's xen_lock and the caches' locks; it is
 * taken under a vCPU's mutex and the big lock, and is held across copies to
 * guest memory, which take the slots' readers' lock and the regions' lock.
 * `long_mode` and `upcall_vector` are also read without it, a word that a
 * change under it leaves whole.
 */
struct xen_vm {
    struct mutex lock;
    struct kvm_xen_hvm_config config;
    int long_mode;                   /* the 64-bit layouts */
    volatile uint32_t upcall_vector; /* HVMIRQ_callback_vector, 0 for none */
    uint32_t xen_version;
    int runstate_update_flag;
    struct xen_area shinfo; /* a page */
    struct xen_port *ports;
};

/*
 * xen.c's share of a vCPU. The areas change under the vCPU's mutex and the
 * VM's `lock`, so the vCPU's thread reads them with its mutex alone.
 * `upcall` is the guest's evtchn_upcall_pending as last read or set: the
 * vCPU's thread reads it again before each injection, and a delivery sets
 * it after it writes the guest's byte. The runstate fields are the vCPU
 * thread's, and the thread's own switch away (`kvm_xen_preempted`) changes
 * them too unless `rs_busy` says the thread is in the middle of them.
 */
struct xen_vcpu {
    struct xen_area info;   /* vcpu_info, INFO_SIZE bytes */
    struct xen_area time;   /* vcpu_time_info, TIME_SIZE bytes */
    uint32_t vcpu_id;       /* Xen's id, KVM_XEN_VCPU_ATTR_TYPE_VCPU_ID */
    uint32_t upcall_vector; /* the vector each upcall raises in the APIC, 0 for none */
    volatile uint32_t upcall;
    uint64_t pending_sel;      /* words of evtchn_pending_sel the info could not take */
    volatile uint32_t unblock; /* KVM_REQ_UNBLOCK: work for the halted wait */

    int rs_active;
    uint64_t rs_gpa;
    uint32_t rs_state;    /* RUNSTATE_* */
    uint64_t rs_entry;    /* the kvmclock's ns at the state's start */
    uint64_t rs_times[4]; /* the ns in each state */
    uint64_t rs_steal;    /* the thread's run_delay at the last update */
    int64_t rs_skew;      /* the kvmclock less the host's ns at the last update */
    volatile int rs_busy;
    int rs_ok; /* the area was vouched for at `rs_seq` */
    uint32_t rs_seq;
    uint64_t rs_vouched; /* the address vouched for */

    uint32_t timer_virq;    /* the timer's port, 0 for none */
    uint64_t timer_expires; /* the kvmclock's ns, 0 when not armed */
    uint64_t timer_host;    /* the host's ns of the same moment */

    int polling;
    int poll_woken;
    int32_t poll_evtchn; /* the one port polled, or -1 for any */
    uint64_t poll_host;  /* the host's ns the poll ends at, 0 for none */

    int hcall_pending;  /* KVM_EXIT_XEN waits for its result */
    uint64_t hcall_rip; /* the call's linear RIP */
    uint32_t hcall_len; /* its instruction's bytes */
};

/** xen.c's share of `vm`. */
static struct xen_vm *xvm(struct kvm_vm *vm)
{
    return (struct xen_vm *)vm->xen;
}

/** xen.c's share of `v`. */
static struct xen_vcpu *xvc(struct kvm_vcpu *v)
{
    return (struct xen_vcpu *)v->xen;
}

int kvm_xen_msr_enabled(struct kvm_vm *vm)
{
    return xvm(vm) && xvm(vm)->config.msr != 0;
}

int kvm_xen_hypercall_enabled(struct kvm_vm *vm)
{
    return xvm(vm) && (xvm(vm)->config.flags & KVM_XEN_HVM_CONFIG_INTERCEPT_HCALL) != 0;
}

int32_t kvm_xen_caps(void)
{
    /* The thread's run_delay that the runstate needs is always counted,
     * Linux's sched_info_on(). */
    return (int32_t)(KVM_XEN_HVM_CONFIG_HYPERCALL_MSR | KVM_XEN_HVM_CONFIG_INTERCEPT_HCALL |
                     KVM_XEN_HVM_CONFIG_SHARED_INFO | KVM_XEN_HVM_CONFIG_EVTCHN_2LEVEL |
                     KVM_XEN_HVM_CONFIG_EVTCHN_SEND | KVM_XEN_HVM_CONFIG_PVCLOCK_TSC_UNSTABLE |
                     KVM_XEN_HVM_CONFIG_SHARED_INFO_HVA | KVM_XEN_HVM_CONFIG_RUNSTATE |
                     KVM_XEN_HVM_CONFIG_RUNSTATE_UPDATE_FLAG);
}

/** The ports of `x`'s ABI. */
static uint32_t max_port(const struct xen_vm *x)
{
    return x->long_mode ? PORTS_64 : PORTS_32;
}

/* ---- guest memory ---- */

/** Read, write or OR into, by `op`, the user memory at `uaddr`: `n` bytes
 * at `buf`, or for MEM_OR the bits of the aligned word that `buf` holds,
 * set by compare-and-exchange, which stores the word even when the bits
 * are set already or none are given, with the word as it was given back
 * in `buf`. 0 or -EFAULT. */
static int mem_user(uintptr_t uaddr, void *buf, uint32_t n, int op)
{
    uint32_t bits, w, seen;
    int r;

    if (op == MEM_READ) {
        return copy_from_user(buf, (const void __user *)uaddr, n);
    }
    if (op == MEM_WRITE) {
        return copy_to_user((void __user *)uaddr, buf, n);
    }
    k_memcpy(&bits, buf, 4);
    r = get_user_u32(&w, uaddr);
    while (r == 0) {
        r = cmpxchg_user_u32(uaddr, w, w | bits, &seen);
        if (r != 0 || seen == w) {
            break;
        }
        w = seen;
    }
    k_memcpy(buf, &w, 4);
    return r;
}

/**
 * Access `n` bytes within one page at `addr` of `vm`, a caller's address
 * for `hva` or else guest-physical, by `op` as `mem_user` does, in a thread
 * of the VM's address space. A write to guest-physical memory is logged
 * as the write of `self`, the vCPU whose mutex the caller holds, or of the
 * running vCPU for NULL, as Linux's mark_page_dirty_in_slot does; a
 * caller's address is not logged, as Linux's cache in that mode has no
 * slot. May sleep. 0; -EFAULT for memory in no slot, a write to a
 * read-only slot or memory that cannot be touched; or -ENOSPC for a full
 * dirty ring.
 */
static int mem_page(struct kvm_vm *vm, struct kvm_vcpu *self, int hva, uint64_t addr, void *buf,
                    uint32_t n, int op)
{
    struct kvm_slot_view s;
    struct kvm_dirty_gfn *entry = NULL;
    uint64_t gfn = addr >> 12;
    uint32_t idx, flags;
    int r;

    if (hva) {
        if (!user_range_ok((uintptr_t)addr, n)) {
            return -EFAULT;
        }
        return mem_user((uintptr_t)addr, buf, n, op);
    }
    idx = kvm_srcu_read_lock(vm);
    if (!kvm_slot_find_read(vm, gfn, &s) || (op != MEM_READ && (s.flags & KVM_MEM_READONLY))) {
        r = -EFAULT;
        goto out;
    }
    if (op != MEM_READ) {
        if (self) {
            r = kvm_dirty_reserve(self, &s, gfn, &entry);
        } else {
            flags = kvm_mmu_lock(vm);
            r = kvm_dirty_reserve_locked(vm, &s, gfn, &entry);
            kvm_mmu_unlock(vm, flags);
        }
        if (r != 0) {
            goto out;
        }
    }
    r = mem_user((uintptr_t)(s.hva + (addr & 0xFFFu)), buf, n, op);
    if (op != MEM_READ) {
        if (entry) {
            kvm_dirty_publish(entry);
        } else if (self) {
            (void)kvm_dirty_mark(self, &s, gfn);
        } else {
            flags = kvm_mmu_lock(vm);
            (void)kvm_dirty_mark_locked(vm, &s, gfn);
            kvm_mmu_unlock(vm, flags);
        }
    }
out:
    kvm_srcu_read_unlock(vm, idx);
    return r;
}

/** Read or write `n` bytes at `addr`, across pages as needed, as
 * `mem_page` does each page's share. */
static int mem_rw(struct kvm_vm *vm, struct kvm_vcpu *self, int hva, uint64_t addr, void *buf,
                  uint32_t n, int op)
{
    uint8_t *p = (uint8_t *)buf;
    uint32_t chunk;
    int r;

    while (n) {
        chunk = 4096u - (uint32_t)(addr & 0xFFFu);
        if (chunk > n) {
            chunk = n;
        }
        r = mem_page(vm, self, hva, addr, p, chunk, op);
        if (r != 0) {
            return r;
        }
        p += chunk;
        addr += chunk;
        n -= chunk;
    }
    return 0;
}

/** Access `n` bytes at `off` in the area `a`. */
static int area_rw(struct kvm_vm *vm, struct kvm_vcpu *self, const struct xen_area *a, uint32_t off,
                   void *buf, uint32_t n, int write)
{
    return mem_rw(vm, self, a->hva, a->addr + off, buf, n, write ? MEM_WRITE : MEM_READ);
}

/** Set bit `bit` of the little-endian bitmap at `off` in `a` atomically:
 * 0 with whether it was set in `*was`, or -EFAULT. */
static int area_set_bit(struct kvm_vm *vm, struct kvm_vcpu *self, const struct xen_area *a,
                        uint32_t off, uint32_t bit, int *was)
{
    uint32_t w = 1u << (bit & 31u);
    int r = mem_page(vm, self, a->hva, a->addr + off + (bit / 32u) * 4u, &w, 4, MEM_OR);

    *was = r == 0 && (w & (1u << (bit & 31u))) != 0;
    return r;
}

/** Bit `bit` of the bitmap at `off` in `a`: 1, 0, or -EFAULT. */
static int area_test_bit(struct kvm_vm *vm, const struct xen_area *a, uint32_t off, uint32_t bit)
{
    uint32_t w = 0;

    if (mem_page(vm, NULL, a->hva, a->addr + off + (bit / 32u) * 4u, &w, 4, MEM_READ) != 0) {
        return -EFAULT;
    }
    return (w >> (bit & 31u)) & 1u;
}

/**
 * Make `a` the area at `addr`, `len` bytes, a caller's address for `hva`,
 * as Linux's kvm_gpc_activate and kvm_gpc_activate_hva: the area within a
 * page, whose word is read and stored again to see that it may be written.
 * 0; -EINVAL for an area across a page or a caller's address outside user
 * space; -EFAULT for memory in no slot, in a read-only one or that cannot
 * be touched. The area is left as it was on a failure.
 */
static int area_activate(struct kvm_vm *vm, struct kvm_vcpu *self, struct xen_area *a, int hva,
                         uint64_t addr, uint32_t len)
{
    uint32_t w = 0;

    if ((addr & 0xFFFu) + len > 4096u) {
        return -EINVAL;
    }
    if (hva && !user_span_ok((uintptr_t)addr, len)) {
        return -EINVAL;
    }
    if (mem_page(vm, self, hva, addr & ~3ull, &w, 4, MEM_OR) != 0) {
        return -EFAULT;
    }
    a->active = 1;
    a->hva = hva;
    a->addr = addr;
    return 0;
}

/* ---- the vCPUs ---- */

/** The vCPU of `vm` whose APIC id is `id`, or NULL, as Linux's
 * kvm_get_vcpu_by_id. */
static struct kvm_vcpu *vcpu_by_id(struct kvm_vm *vm, uint32_t id)
{
    uint32_t i, n = vm->nr_vcpus;

    for (i = 0; i < n; i++) {
        if (vm->vcpus[i]->id == id) {
            return vm->vcpus[i];
        }
    }
    return NULL;
}

/** Wake `t` for work xen.c left it (`unblock`) and an event to look at. */
static void wake(struct kvm_vcpu *t)
{
    xvc(t)->unblock = 1;
    if (t->vm->irqchip != KVM_IRQCHIP_NONE) {
        kvm_lapic_wake(t, KVM_REQ_EVENT);
    } else {
        kvm_vcpu_kick(t, KVM_REQ_EVENT);
    }
}

/** Raise `t`'s own upcall vector in its local APIC as a fixed interrupt,
 * as Linux's kvm_xen_inject_vcpu_vector. */
static void inject_vcpu_vector(struct kvm_vcpu *t)
{
    struct kvm_lapic_irq irq;

    k_memset(&irq, 0, sizeof(irq));
    irq.dest_id = t->id;
    irq.vector = xvc(t)->upcall_vector;
    irq.delivery_mode = KVM_APIC_DM_FIXED;
    irq.level = 1;
    (void)kvm_lapic_deliver(t->vm, NULL, &irq);
}

/** A masked `port` was raised: end `t`'s poll when it waits for it, as
 * Linux's kvm_xen_check_poller. Under `lock`. */
static void check_poller(struct kvm_vcpu *t, uint32_t port)
{
    struct xen_vcpu *xv = xvc(t);

    if (xv->polling && !xv->poll_woken &&
        (xv->poll_evtchn == (int32_t)port || xv->poll_evtchn == -1)) {
        xv->poll_woken = 1;
        wake(t);
    }
}

/** The offset of evtchn_pending_sel's word that holds `word_bit`, and its
 * bit there. */
static uint32_t sel_off(const struct xen_vm *x, uint32_t word_bit)
{
    return x->long_mode ? INFO_SEL_64 + (word_bit / 32u) * 4u : INFO_SEL_32;
}

/**
 * Raise `port` for the vCPU `t`, as Linux's kvm_xen_set_evtchn: the
 * port's pending bit in the shared info, then, unless the port is masked,
 * its word's bit of evtchn_pending_sel and evtchn_upcall_pending in the
 * vCPU's info, or with no info the shadow `pending_sel` that `t` copies in
 * later, and the upcall: `t`'s own vector in its APIC, or the VM's, which
 * the vCPU sees at its next entry. Under `lock`. 1 delivered; 0 already
 * pending; -ENOTCONN masked; -EINVAL for a port out of range or no shared
 * info; -EFAULT.
 */
static int set_evtchn(struct kvm_vm *vm, struct kvm_vcpu *self, struct kvm_vcpu *t, uint32_t port)
{
    struct xen_vm *x = xvm(vm);
    struct xen_vcpu *xv = xvc(t);
    uint32_t word_bit = x->long_mode ? port / 64u : port / 32u, bit = 1u << (word_bit & 31u);
    uint32_t mask_off = x->long_mode ? SHINFO_MASK_64 : SHINFO_MASK_32, w;
    uint8_t one = 1;
    int was, r;

    if (port >= max_port(x)) {
        return -EINVAL;
    }
    if (!x->shinfo.active) {
        return -EINVAL;
    }
    r = area_set_bit(vm, self, &x->shinfo, SHINFO_PENDING, port, &was);
    if (r != 0) {
        return r;
    }
    if (was) {
        return 0;
    }
    r = area_test_bit(vm, &x->shinfo, mask_off, port);
    if (r < 0) {
        return r;
    }
    if (r) {
        check_poller(t, port);
        return -ENOTCONN;
    }
    w = bit;
    if (!xv->info.active || mem_page(vm, self, xv->info.hva, xv->info.addr + sel_off(x, word_bit),
                                     &w, 4, MEM_OR) != 0) {
        /* The info cannot be reached: the vCPU copies the bit in itself. */
        if (!(xv->pending_sel & (1ull << word_bit))) {
            xv->pending_sel |= 1ull << word_bit;
            wake(t);
        }
        return 1;
    }
    if (!(w & bit)) {
        (void)area_rw(vm, self, &xv->info, 0, &one, 1, 1);
        xv->upcall = 1;
        if (xv->upcall_vector) {
            inject_vcpu_vector(t);
        } else {
            wake(t);
        }
    }
    return 1;
}

/** `set_evtchn` for the vCPU whose APIC id is `vcpu`: -EINVAL for none. */
static int set_evtchn_id(struct kvm_vm *vm, struct kvm_vcpu *self, uint32_t vcpu, uint32_t port)
{
    struct kvm_vcpu *t = vcpu_by_id(vm, vcpu);

    return t ? set_evtchn(vm, self, t, port) : -EINVAL;
}

/** Copy the shadow `pending_sel` of `v` into its info and raise the
 * upcall, as Linux's kvm_xen_inject_pending_events. Under `lock`. */
static void inject_pending(struct kvm_vcpu *v)
{
    struct xen_vm *x = xvm(v->vm);
    struct xen_vcpu *xv = xvc(v);
    uint32_t w, i;
    uint8_t one = 1;

    if (!xv->pending_sel || !xv->info.active) {
        return;
    }
    if (x->long_mode) {
        for (i = 0; i < 2u; i++) {
            w = (uint32_t)(xv->pending_sel >> (32u * i));
            if (w && mem_page(v->vm, v, xv->info.hva, xv->info.addr + INFO_SEL_64 + 4u * i, &w, 4,
                              MEM_OR)) {
                return;
            }
        }
    } else {
        w = (uint32_t)xv->pending_sel;
        if (mem_page(v->vm, v, xv->info.hva, xv->info.addr + INFO_SEL_32, &w, 4, MEM_OR)) {
            return;
        }
    }
    xv->pending_sel = 0;
    if (area_rw(v->vm, v, &xv->info, 0, &one, 1, 1) != 0) {
        return;
    }
    xv->upcall = 1;
    if (xv->upcall_vector) {
        inject_vcpu_vector(v);
    }
}

/* ---- the shared info and the clock ---- */

/** Write the wall clock into the shared info, where the layout puts it,
 * and ask every vCPU to write its clock again, as Linux's
 * kvm_xen_shared_info_init. Under `lock`. 0 or -EFAULT. */
static int shinfo_init(struct kvm_vm *vm)
{
    struct xen_vm *x = xvm(vm);
    uint32_t wc = x->long_mode ? SHINFO_WC_64 : SHINFO_WC_32;
    uint32_t hi_off = x->long_mode ? SHINFO_WC_SEC_HI_64 : SHINFO_WC_SEC_HI_32;
    uint32_t version, t[2], hi, i;
    uint64_t wall;

    if (area_rw(vm, NULL, &x->shinfo, wc, &version, 4, 0) != 0) {
        return -EFAULT;
    }
    version = (version + 1u) | 1u;
    if (area_rw(vm, NULL, &x->shinfo, wc, &version, 4, 1) != 0) {
        return -EFAULT;
    }
    wall = kvm_wall_epoch(vm);
    t[0] = (uint32_t)(wall / 1000000000ull); /* sec, modulo 2^32 */
    t[1] = (uint32_t)(wall % 1000000000ull); /* nsec */
    hi = (uint32_t)((wall / 1000000000ull) >> 32);
    if (area_rw(vm, NULL, &x->shinfo, wc + 4u, t, 8, 1) != 0 ||
        area_rw(vm, NULL, &x->shinfo, hi_off, &hi, 4, 1) != 0) {
        return -EFAULT;
    }
    version++;
    if (area_rw(vm, NULL, &x->shinfo, wc, &version, 4, 1) != 0) {
        return -EFAULT;
    }
    for (i = 0; i < vm->nr_vcpus; i++) {
        kvm_vcpu_kick(vm->vcpus[i], KVM_REQ_MASTERCLOCK_UPDATE);
    }
    return 0;
}

/** Write the clock `pv` at `off` in the area `a`, as Linux's
 * kvm_setup_guest_pvclock: the version odd, the record with the guest's
 * PVCLOCK_GUEST_STOPPED kept and `stopped` added, the stable bit taken off
 * under PVCLOCK_TSC_UNSTABLE, then the version even. */
static void clock_copy(struct kvm_vcpu *v, const struct xen_area *a, uint32_t off,
                       const struct pvclock_vcpu_time_info *pv, int stopped, int unstable)
{
    struct pvclock_vcpu_time_info guest, out = *pv;
    uint32_t version;

    if (area_rw(v->vm, v, a, off, &guest, sizeof(guest), 0) != 0) {
        return;
    }
    version = (guest.version + 1u) | 1u;
    if (area_rw(v->vm, v, a, off, &version, 4, 1) != 0) {
        return;
    }
    out.version = version;
    out.flags |= (uint8_t)(guest.flags & PVCLOCK_GUEST_STOPPED);
    if (stopped) {
        out.flags |= PVCLOCK_GUEST_STOPPED;
    }
    if (unstable) {
        out.flags &= (uint8_t)~PVCLOCK_TSC_STABLE_BIT;
    }
    if (area_rw(v->vm, v, a, off, &out, sizeof(out), 1) != 0) {
        return;
    }
    version++;
    (void)area_rw(v->vm, v, a, off, &version, 4, 1);
}

int kvm_xen_clock_active(const struct kvm_vcpu *v)
{
    const struct xen_vcpu *xv = (const struct xen_vcpu *)v->xen;

    return xv && (xv->info.active || xv->time.active);
}

void kvm_xen_clock_write(struct kvm_vcpu *v, const struct pvclock_vcpu_time_info *pv, int stopped)
{
    struct xen_vm *x = xvm(v->vm);
    struct xen_vcpu *xv = xvc(v);
    int unstable;

    if (!kvm_xen_clock_active(v)) {
        return;
    }
    mutex_lock(&x->lock);
    unstable = (x->config.flags & KVM_XEN_HVM_CONFIG_PVCLOCK_TSC_UNSTABLE) != 0;
    if (xv->info.active) {
        clock_copy(v, &xv->info, INFO_TIME, pv, stopped, unstable);
    }
    if (xv->time.active) {
        clock_copy(v, &xv->time, 0, pv, stopped, unstable);
    }
    mutex_unlock(&x->lock);
}

void kvm_xen_tsc_info(struct kvm_vcpu *v, uint32_t mul, int8_t shift, uint32_t khz)
{
    struct kvm_cpuid_entry2 *e;
    uint32_t base, i, leaf;

    for (base = 0x40000000u; base < 0x40010000u; base += 0x100u) {
        for (i = 0; i < v->cpuid_nent; i++) {
            e = &v->cpuid[i];
            /* "XenVMMXenVMM" in EBX, ECX and EDX. */
            if (e->function == base && e->ebx == 0x566E6558u && e->ecx == 0x65584D4Du &&
                e->edx == 0x4D4D566Eu) {
                goto found;
            }
        }
    }
    return;
found:
    leaf = base | 0x40000003u;
    if (leaf > e->eax) {
        return;
    }
    for (i = 0; i < v->cpuid_nent; i++) {
        e = &v->cpuid[i];
        if (e->function != leaf) {
            continue;
        }
        if (e->index == 1u) {
            e->ecx = mul;
            e->edx = (uint32_t)(int32_t)shift;
        } else if (e->index == 2u) {
            e->eax = khz;
        }
    }
}

/* ---- the runstate ---- */

/** The bytes of the runstate record and the offset of its entry time. */
static void rs_layout(const struct xen_vm *x, uint32_t *len, uint32_t *times)
{
    *len = x->long_mode ? RS_SIZE_64 : RS_SIZE_32;
    *times = x->long_mode ? RS_TIMES_64 : RS_TIMES_32;
}

/** The record `v`'s runstate area holds now, assembled at `buf`: the
 * state, the entry time with `flag`, and the four times. */
static void rs_image(const struct xen_vcpu *xv, uint32_t times, uint64_t flag, uint8_t *buf)
{
    uint64_t entry = xv->rs_entry | flag;

    k_memset(buf, 0, RS_SIZE_64);
    k_memcpy(buf, &xv->rs_state, 4);
    k_memcpy(buf + times, &entry, 8);
    k_memcpy(buf + times + 8u, xv->rs_times, sizeof(xv->rs_times));
}

/** Find whether the pages of `v`'s runstate area lie in no region of
 * memfd_secret, so that `kvm_xen_preempted` may write them without a
 * fault while `mmu_seq` holds, as the steal time's are vouched for. */
static void rs_vouch(struct kvm_vcpu *v, uint32_t len)
{
    struct xen_vcpu *xv = xvc(v);
    uint32_t seq = v->vm->mmu_seq;
    uint64_t last = xv->rs_gpa + len - 1u;

    k_barrier();
    xv->rs_ok = 0;
    if (!kvm_gpa_vouch(v->vm, xv->rs_gpa) ||
        ((last >> 12) != (xv->rs_gpa >> 12) && !kvm_gpa_vouch(v->vm, last))) {
        return;
    }
    xv->rs_vouched = xv->rs_gpa;
    xv->rs_seq = seq;
    xv->rs_ok = 1;
}

/**
 * Write `v`'s runstate into its area, as Linux's
 * kvm_xen_update_runstate_guest with `atomic` false: with the VM's
 * runstate update flag, the entry time's top byte first with
 * XEN_RUNSTATE_UPDATE, then the state and the times, then the byte again
 * without it. The state and the times are written apart, as Linux writes
 * them in place, so the padding of the 64-bit layout is left. An area
 * whose memory cannot be read whole is not written. In `v`'s thread or
 * under its mutex.
 */
static void rs_write(struct kvm_vcpu *v)
{
    struct xen_vm *x = xvm(v->vm);
    struct xen_vcpu *xv = xvc(v);
    uint8_t buf[RS_SIZE_64], top;
    uint32_t len, times;
    int flag = x->runstate_update_flag;

    if (!xv->rs_active) {
        return;
    }
    rs_layout(x, &len, &times);
    if (mem_rw(v->vm, v, 0, xv->rs_gpa, buf, len, MEM_READ) != 0) {
        return;
    }
    rs_image(xv, times, flag ? XEN_RUNSTATE_UPDATE : 0, buf);
    if (flag) {
        top = buf[times + 7u];
        if (mem_rw(v->vm, v, 0, xv->rs_gpa + times + 7u, &top, 1, MEM_WRITE) != 0) {
            return;
        }
    }
    if (mem_rw(v->vm, v, 0, xv->rs_gpa, buf, 4, MEM_WRITE) != 0 ||
        mem_rw(v->vm, v, 0, xv->rs_gpa + times, buf + times, 40, MEM_WRITE) != 0) {
        return;
    }
    if (flag) {
        top = (uint8_t)(xv->rs_entry >> 56);
        (void)mem_rw(v->vm, v, 0, xv->rs_gpa + times + 7u, &top, 1, MEM_WRITE);
    }
    if (!xv->rs_ok || xv->rs_seq != v->vm->mmu_seq || xv->rs_vouched != xv->rs_gpa) {
        rs_vouch(v, len);
    }
}

/** Account the time since the last change of `v`'s runstate to the state
 * it was in, the thread's wait for a processor while running to runnable,
 * and enter `state` at `now`, as Linux's kvm_xen_update_runstate. */
static void rs_account(struct xen_vcpu *xv, uint32_t state, uint64_t now, uint64_t run_delay)
{
    uint64_t delta = now - xv->rs_entry, steal;

    if (!xv->rs_entry) {
        xv->rs_state = RUNSTATE_offline;
    }
    if (xv->rs_state == RUNSTATE_running) {
        steal = run_delay - xv->rs_steal;
        delta -= steal;
        xv->rs_times[RUNSTATE_runnable] += steal;
    }
    xv->rs_steal = run_delay;
    xv->rs_times[xv->rs_state] += delta;
    xv->rs_state = state;
    xv->rs_entry = now;
}

/** Enter `state` now and write the area, in `v`'s thread or under its
 * mutex. */
static void rs_update(struct kvm_vcpu *v, uint32_t state)
{
    struct xen_vcpu *xv = xvc(v);
    uint64_t now;

    xv->rs_busy = 1;
    k_barrier();
    now = kvm_clock_ns(v->vm);
    xv->rs_skew = (int64_t)(now - kvm_host_ns());
    rs_account(xv, state, now, current_proc->run_delay);
    k_barrier();
    xv->rs_busy = 0;
    rs_write(v);
}

void kvm_xen_runstate_running(struct kvm_vcpu *v)
{
    rs_update(v, RUNSTATE_running);
}

/** Write `n` bytes at `gpa` without a fault, across a page as needed:
 * 0, or 1 with what came before the failing page written. */
static int rs_nofault(struct kvm_vm *vm, uint64_t gpa, const uint8_t *buf, uint32_t n, uint32_t seq)
{
    uint32_t chunk;

    while (n) {
        chunk = 4096u - (uint32_t)(gpa & 0xFFFu);
        if (chunk > n) {
            chunk = n;
        }
        if (kvm_tdp_write_nofault(vm, gpa, buf, chunk, seq) != 0) {
            return 1;
        }
        buf += chunk;
        gpa += chunk;
        n -= chunk;
    }
    return 0;
}

void kvm_xen_preempted(struct kvm_vcpu *v)
{
    struct xen_vm *x = xvm(v->vm);
    struct xen_vcpu *xv = xvc(v);
    uint8_t buf[RS_SIZE_64], top;
    uint32_t len, times;
    int flag = x->runstate_update_flag;

    if (xv->rs_busy) {
        return;
    }
    rs_account(xv, RUNSTATE_runnable, kvm_host_ns() + (uint64_t)xv->rs_skew,
               current_proc->run_delay);
    if (!xv->rs_active || !xv->rs_ok || xv->rs_vouched != xv->rs_gpa) {
        return;
    }
    rs_layout(x, &len, &times);
    rs_image(xv, times, flag ? XEN_RUNSTATE_UPDATE : 0, buf);
    if (flag) {
        top = buf[times + 7u];
        if (rs_nofault(v->vm, xv->rs_gpa + times + 7u, &top, 1, xv->rs_seq) != 0) {
            return;
        }
    }
    if (rs_nofault(v->vm, xv->rs_gpa, buf, 4, xv->rs_seq) != 0 ||
        rs_nofault(v->vm, xv->rs_gpa + times, buf + times, 40, xv->rs_seq) != 0) {
        return;
    }
    if (flag) {
        top = (uint8_t)(xv->rs_entry >> 56);
        (void)rs_nofault(v->vm, xv->rs_gpa + times + 7u, &top, 1, xv->rs_seq);
    }
}

/* ---- the timer ---- */

/** Deliver `v`'s timer and disarm it, as Linux's xen_timer_callback and
 * kvm_xen_inject_timer_irqs: an event lost to a missing shared info is
 * dropped. Under `lock`. */
static void timer_fire(struct kvm_vcpu *v)
{
    struct xen_vcpu *xv = xvc(v);

    xv->timer_expires = 0;
    xv->timer_host = 0;
    (void)set_evtchn(v->vm, v, v, xv->timer_virq);
}

/**
 * Arm `v`'s timer for `guest_abs`, the kvmclock's ns, as Linux's
 * kvm_xen_start_timer: with `linux_wa`, for set_timer_op, a negative time
 * or one some 13 days ahead is 100 ms ahead, Xen's workaround for Linux's
 * overflows. A time already past fires now. Under `lock`, in `v`'s thread
 * or under its mutex; `own` says which, for how the run loop is told.
 */
static void timer_start(struct kvm_vcpu *v, uint64_t guest_abs, int linux_wa, int own)
{
    struct xen_vcpu *xv = xvc(v);
    uint64_t guest_now = kvm_clock_ns(v->vm), host_now = kvm_host_ns();
    int64_t delta = (int64_t)(guest_abs - guest_now);

    if (linux_wa && ((int64_t)guest_abs < 0 || (delta > 0 && (uint32_t)(delta >> 50) != 0))) {
        delta = (int64_t)(100u * NSEC_PER_MSEC);
        guest_abs = guest_now + (uint64_t)delta;
    }
    xv->timer_expires = guest_abs;
    if (delta <= 0) {
        timer_fire(v);
        return;
    }
    xv->timer_host = host_now + (uint64_t)delta;
    if (own) {
        kvm_timer_arm(v, xv->timer_host);
    } else {
        kvm_vcpu_kick(v, KVM_REQ_TIMER);
    }
}

/** Disarm `v`'s timer, as Linux's kvm_xen_stop_timer. */
static void timer_stop(struct kvm_vcpu *v)
{
    xvc(v)->timer_expires = 0;
    xvc(v)->timer_host = 0;
}

uint64_t kvm_xen_next(struct kvm_vcpu *v)
{
    struct xen_vcpu *xv = xvc(v);
    uint64_t a, b;

    if (!xv) {
        return 0;
    }
    a = xv->timer_host;
    b = xv->polling ? xv->poll_host : 0;
    return !a || (b && b < a) ? b : a;
}

uint64_t kvm_xen_timers(struct kvm_vcpu *v)
{
    struct xen_vcpu *xv = xvc(v);
    uint64_t now;

    if (!xv || (!xv->timer_host && !xv->poll_host)) {
        return 0;
    }
    now = kvm_host_ns();
    if (xv->timer_host && now >= xv->timer_host) {
        mutex_lock(&xvm(v->vm)->lock);
        timer_fire(v);
        mutex_unlock(&xvm(v->vm)->lock);
    }
    if (xv->poll_host && now >= xv->poll_host) {
        xv->poll_host = 0;
        xv->poll_woken = 1;
        xv->unblock = 1;
    }
    return kvm_xen_next(v);
}

/* ---- the upcall and the poll ---- */

int kvm_xen_upcall(struct kvm_vcpu *v)
{
    struct xen_vm *x = xvm(v->vm);
    struct xen_vcpu *xv = xvc(v);
    uint32_t vector;

    if (!x || !xv) {
        return -1;
    }
    vector = x->upcall_vector;
    return vector && xv->info.active && xv->upcall ? (int)vector : -1;
}

void kvm_xen_refresh(struct kvm_vcpu *v)
{
    struct xen_vm *x = xvm(v->vm);
    struct xen_vcpu *xv = xvc(v);
    uint8_t pending = 0;

    if (!x || !xv || !x->upcall_vector || !xv->info.active) {
        return;
    }
    if (area_rw(v->vm, v, &xv->info, 0, &pending, 1, 0) != 0) {
        pending = 0;
    }
    xv->upcall = pending != 0;
}

/** End `v`'s poll: the vCPU runs again. */
static void poll_end(struct kvm_vcpu *v)
{
    struct xen_vcpu *xv = xvc(v);

    xv->polling = 0;
    xv->poll_woken = 0;
    xv->poll_host = 0;
    if (v->mp_state == KVM_MP_STATE_HALTED) {
        v->mp_state = KVM_MP_STATE_RUNNABLE;
        v->dirty |= KVM_PART_MP;
    }
}

void kvm_xen_prepare(struct kvm_vcpu *v)
{
    struct xen_vm *x = xvm(v->vm);
    struct xen_vcpu *xv = xvc(v);

    if (!x || !xv) {
        return;
    }
    if (xv->unblock) {
        xv->unblock = 0;
        if (xv->pending_sel) {
            mutex_lock(&x->lock);
            inject_pending(v);
            mutex_unlock(&x->lock);
        }
    }
    if (xv->polling && (xv->poll_woken || v->mp_state != KVM_MP_STATE_HALTED)) {
        mutex_lock(&x->lock);
        poll_end(v);
        mutex_unlock(&x->lock);
    }
    kvm_xen_refresh(v);
}

void kvm_xen_entry(struct kvm_vcpu *v)
{
    struct xen_vm *x = xvm(v->vm);
    struct xen_vcpu *xv = xvc(v);

    if (!x || !xv) {
        return;
    }
    if ((xv->pending_sel && xv->info.active) ||
        (xv->polling && v->mp_state != KVM_MP_STATE_HALTED)) {
        mutex_lock(&x->lock);
        inject_pending(v);
        if (xv->polling && v->mp_state != KVM_MP_STATE_HALTED) {
            poll_end(v);
        }
        mutex_unlock(&x->lock);
    }
    if (x->upcall_vector && xv->info.active) {
        kvm_xen_refresh(v);
        if (xv->upcall) {
            kvm_vcpu_kick(v, KVM_REQ_EVENT);
        }
    }
}

int kvm_xen_wake(struct kvm_vcpu *v)
{
    struct xen_vcpu *xv = xvc(v);

    return xv && xv->unblock;
}

void kvm_xen_run_end(struct kvm_vcpu *v)
{
    struct xen_vcpu *xv = xvc(v);

    if (xv && xv->polling) {
        mutex_lock(&xvm(v->vm)->lock);
        poll_end(v);
        mutex_unlock(&xvm(v->vm)->lock);
    }
}

void kvm_xen_sw_enable_lapic(struct kvm_vcpu *v)
{
    struct xen_vcpu *xv = xvc(v);

    if (!xv || !xv->upcall_vector || !xv->info.active) {
        return;
    }
    kvm_xen_refresh(v);
    if (xv->upcall) {
        inject_vcpu_vector(v);
    }
}

/* ---- the hypercalls ---- */

/** Put the hypercall's `result` in RAX and step past it, as Linux's
 * kvm_xen_hypercall_set_result. */
static void hcall_result(struct kvm_vcpu *v, uint64_t result, uint32_t len)
{
    v->regs.rax = result;
    kvm_emul_skip(v, len);
}

/** Read `n` bytes of the guest's linear memory at `la`, at privilege 0,
 * with no fault for the guest: 0, or nonzero. */
static int read_virt(struct kvm_vcpu *v, uint64_t la, void *buf, uint32_t n)
{
    return kvm_guest_virt_read(v, la, buf, n);
}

/** EVTCHNOP_send, as Linux's kvm_xen_hcall_evtchn_send: the outbound
 * channel of the guest's port raises its port or signals its eventfd.
 * Whether the kernel answered, with the result in `*r`. */
static int hcall_evtchn_send(struct kvm_vcpu *v, uint64_t param, uint64_t *r)
{
    struct xen_vm *x = xvm(v->vm);
    struct xen_port *p;
    uint32_t port;
    int ret, handled = 1;

    if (read_virt(v, param, &port, 4) != 0) {
        *r = (uint64_t)-(int64_t)XEN_EFAULT;
        return 1;
    }
    mutex_lock(&x->lock);
    for (p = x->ports; p && p->send_port != port; p = p->next) {
    }
    if (!p) {
        handled = 0;
    } else if (p->port) {
        ret = set_evtchn_id(v->vm, v, p->vcpu, p->port);
        if (ret < 0 && ret != -ENOTCONN) {
            handled = 0;
        }
    } else {
        eventfd_signal(p->ev);
    }
    mutex_unlock(&x->lock);
    if (handled) {
        *r = 0;
    }
    return handled;
}

/** Whether any of the `n` ports is pending in the shared info, or the
 * shared info is missing, as Linux's wait_pending_event. Under `lock`. */
static int ports_pending(struct kvm_vm *vm, const uint32_t *ports, uint32_t n)
{
    struct xen_vm *x = xvm(vm);
    uint32_t i;
    int r;

    if (!x->shinfo.active) {
        return 1;
    }
    for (i = 0; i < n; i++) {
        r = area_test_bit(vm, &x->shinfo, SHINFO_PENDING, ports[i]);
        if (r != 0) {
            return 1;
        }
    }
    return 0;
}

/**
 * SCHEDOP_poll, as Linux's kvm_xen_schedop_poll: with the APIC in the
 * kernel and KVM_XEN_HVM_CONFIG_EVTCHN_SEND, read the guest's sched_poll
 * and its ports and, when none is pending, halt the vCPU until one of
 * them is raised while masked (`check_poller`), an event wakes it, its
 * timeout passes or a signal comes. The call returns 0 at once; the run
 * loop waits before the guest's next instruction (`kvm_xen_prepare`).
 * Whether the kernel answered.
 */
static int hcall_poll(struct kvm_vcpu *v, int longmode, uint64_t param, uint64_t *r)
{
    struct xen_vm *x = xvm(v->vm);
    struct xen_vcpu *xv = xvc(v);
    uint8_t sp[24];
    uint64_t ports_la, timeout;
    uint32_t nr, i, one, *ports = &one;
    int waits;

    if (v->vm->irqchip == KVM_IRQCHIP_NONE || !(x->config.flags & KVM_XEN_HVM_CONFIG_EVTCHN_SEND)) {
        return 0;
    }
    if (read_virt(v, param, sp, longmode ? 24u : 16u) != 0) {
        *r = (uint64_t)-(int64_t)XEN_EFAULT;
        return 1;
    }
    if (longmode) {
        k_memcpy(&ports_la, sp, 8);
        k_memcpy(&nr, sp + 8, 4);
        k_memcpy(&timeout, sp + 16, 8);
    } else {
        ports_la = 0;
        k_memcpy(&ports_la, sp, 4);
        k_memcpy(&nr, sp + 4, 4);
        k_memcpy(&timeout, sp + 8, 8);
    }
    if (nr > 1u) {
        if (nr > POLL_PORTS_MAX) {
            *r = (uint64_t)-(int64_t)XEN_EINVAL;
            return 1;
        }
        ports = (uint32_t *)kmalloc(nr * 4u);
        if (!ports) {
            *r = (uint64_t)-(int64_t)XEN_ENOMEM;
            return 1;
        }
    }
    *r = 0;
    if (nr && read_virt(v, ports_la, ports, nr * 4u) != 0) {
        *r = (uint64_t)-(int64_t)XEN_EFAULT;
        goto out;
    }
    for (i = 0; i < nr; i++) {
        if (ports[i] >= max_port(x)) {
            *r = (uint64_t)-(int64_t)XEN_EINVAL;
            goto out;
        }
    }
    mutex_lock(&x->lock);
    xv->poll_evtchn = nr == 1u ? (int32_t)ports[0] : -1;
    waits = !ports_pending(v->vm, ports, nr);
    if (waits) {
        xv->polling = 1;
        xv->poll_woken = 0;
        xv->poll_host = timeout ? kvm_host_ns() + timeout : 0;
        v->mp_state = KVM_MP_STATE_HALTED;
        v->dirty |= KVM_PART_MP;
        if (xv->poll_host) {
            kvm_timer_arm(v, xv->poll_host);
        }
    }
    mutex_unlock(&x->lock);
out:
    if (ports != &one) {
        kfree(ports);
    }
    return 1;
}

/** VCPUOP_set_singleshot_timer and VCPUOP_stop_singleshot_timer for the
 * Xen vCPU `vcpu_id`, as Linux's kvm_xen_hcall_vcpu_op. Whether the kernel
 * answered. */
static int hcall_vcpu_op(struct kvm_vcpu *v, int longmode, uint32_t cmd, uint32_t vcpu_id,
                         uint64_t param, uint64_t *r)
{
    struct xen_vcpu *xv = xvc(v);
    struct xen_vm *x = xvm(v->vm);
    uint8_t oneshot[16];
    uint64_t timeout;

    if (!xv->timer_virq) {
        return 0;
    }
    if (cmd != VCPUOP_set_singleshot_timer && cmd != VCPUOP_stop_singleshot_timer) {
        return 0;
    }
    if (xv->vcpu_id != vcpu_id) {
        *r = (uint64_t)-(int64_t)XEN_EINVAL;
        return 1;
    }
    if (cmd == VCPUOP_set_singleshot_timer) {
        /* The 32-bit struct is 12 bytes, the 64-bit one 16 with padding,
         * which Xen reads too. */
        if (read_virt(v, param, oneshot, longmode ? 16u : 12u) != 0) {
            *r = (uint64_t)-(int64_t)XEN_EFAULT;
            return 1;
        }
        k_memcpy(&timeout, oneshot, 8);
        mutex_lock(&x->lock);
        timer_start(v, timeout, 0, 1);
        mutex_unlock(&x->lock);
    } else {
        timer_stop(v);
    }
    *r = 0;
    return 1;
}

int kvm_xen_hypercall(struct kvm_vcpu *v, uint32_t len)
{
    struct xen_vm *x = xvm(v->vm);
    struct xen_vcpu *xv = xvc(v);
    uint64_t input = v->regs.rax, params[6], r = (uint64_t)-(int64_t)XEN_ENOSYS, timeout;
    int longmode, handled = 0, cpl;
    uint32_t i;

    if ((input & 0x80000000ull) && kvm_hv_hypercall_enabled(v)) {
        return kvm_hv_hypercall(v, len);
    }
    v->exit.kind = KVM_X_AGAIN;
    v->stat[KVM_VCPU_STAT_HYPERCALLS]++;
    longmode = kvm_emul_mode(v) == 64;
    if (!longmode) {
        input &= 0xFFFFFFFFull;
    }
    if (longmode) {
        params[0] = v->regs.rdi;
        params[1] = v->regs.rsi;
        params[2] = v->regs.rdx;
        params[3] = v->regs.r10;
        params[4] = v->regs.r8;
        params[5] = v->regs.r9;
    } else {
        params[0] = v->regs.rbx;
        params[1] = v->regs.rcx;
        params[2] = v->regs.rdx;
        params[3] = v->regs.rsi;
        params[4] = v->regs.rdi;
        params[5] = v->regs.rbp;
        for (i = 0; i < 6u; i++) {
            params[i] &= 0xFFFFFFFFull;
        }
    }
    cpl = kvm_emul_cpl(v);
    /* Only CPL 0's calls are answered here; the few a guest's user space
     * may make go to the caller. */
    if (cpl == 0) {
        switch (input) {
        case HYPERVISOR_xen_version:
            if (params[0] == XENVER_version && x->xen_version) {
                r = x->xen_version;
                handled = 1;
            }
            break;
        case HYPERVISOR_event_channel_op:
            if (params[0] == EVTCHNOP_send) {
                handled = hcall_evtchn_send(v, params[1], &r);
            }
            break;
        case HYPERVISOR_sched_op:
            if (params[0] == SCHEDOP_poll) {
                handled = hcall_poll(v, longmode, params[1], &r);
            }
            if (!handled && (params[0] == SCHEDOP_poll || params[0] == SCHEDOP_yield)) {
                r = 0;
                handled = 1;
            }
            break;
        case HYPERVISOR_vcpu_op:
            handled =
                hcall_vcpu_op(v, longmode, (uint32_t)params[0], (uint32_t)params[1], params[2], &r);
            break;
        case HYPERVISOR_set_timer_op:
            timeout = params[0];
            if (!longmode) {
                timeout |= params[1] << 32;
            }
            if (xv->timer_virq) {
                mutex_lock(&x->lock);
                if (timeout) {
                    timer_start(v, timeout, 1, 1);
                } else {
                    timer_stop(v);
                }
                mutex_unlock(&x->lock);
                r = 0;
                handled = 1;
            }
            break;
        default:
            break;
        }
    }
    if (handled) {
        hcall_result(v, r, len);
        return 0;
    }
    k_memset(&v->exit.xen, 0, sizeof(v->exit.xen));
    v->exit.kind = KVM_X_XEN;
    v->exit.xen.type = KVM_EXIT_XEN_HCALL;
    v->exit.xen.u.hcall.longmode = (uint32_t)longmode;
    v->exit.xen.u.hcall.cpl = (uint32_t)cpl;
    v->exit.xen.u.hcall.input = input;
    for (i = 0; i < 6u; i++) {
        v->exit.xen.u.hcall.params[i] = params[i];
    }
    xv->hcall_pending = 1;
    xv->hcall_rip = v->sregs.cs.base + v->regs.rip;
    xv->hcall_len = len;
    return 0;
}

void kvm_xen_complete(struct kvm_vcpu *v)
{
    struct xen_vcpu *xv = xvc(v);

    if (!xv || !xv->hcall_pending) {
        return;
    }
    xv->hcall_pending = 0;
    /* A caller that moved RIP meanwhile took the call over. */
    if (v->sregs.cs.base + v->regs.rip != xv->hcall_rip) {
        return;
    }
    hcall_result(v, v->run->u.xen.u.hcall.result, xv->hcall_len);
    v->dirty |= KVM_PART_REGS;
}

/* ---- the hypercall page ---- */

/**
 * The guest's write of `data` to the hypercall MSR, as Linux's
 * kvm_xen_write_hypercall_page: the layouts follow the guest's mode, then
 * the page at `data`'s address gets 128 stubs of `mov $i, %eax; vmcall;
 * ret` under the kernel's intercept, VMMCALL on SVM, or else page `data`'s
 * low bits of the caller's blob for the mode. 0 or KVM_MSR_INVAL.
 */
static int hypercall_page(struct kvm_vcpu *v, uint64_t data)
{
    struct xen_vm *x = xvm(v->vm);
    uint32_t page_num = (uint32_t)(data & 0xFFFu), i;
    uint64_t page = data & ~0xFFFull, blob;
    int lm = (v->sregs.efer & 0x400u) != 0, r = 0;
    uint8_t insn[32], *buf;
    uint8_t size;

    mutex_lock(&x->lock);
    if (x->long_mode != lm) {
        x->long_mode = lm;
        if (x->shinfo.active && shinfo_init(v->vm) != 0) {
            r = KVM_MSR_INVAL;
        }
    }
    mutex_unlock(&x->lock);
    if (r != 0) {
        return r;
    }
    if (x->config.flags & KVM_XEN_HVM_CONFIG_INTERCEPT_HCALL) {
        if (page_num) {
            return KVM_MSR_INVAL;
        }
        insn[0] = 0xB8; /* mov imm32, %eax */
        insn[5] = 0x0F;
        insn[6] = 0x01;
        insn[7] = kvm_backend == &kvm_svm ? 0xD9 : 0xC1;
        insn[8] = 0xC3;
        k_memset(insn + 9, 0xCC, sizeof(insn) - 9u);
        for (i = 0; i < 4096u / sizeof(insn); i++) {
            k_memcpy(insn + 1, &i, 4);
            if (kvm_guest_phys_rw(v, page + i * sizeof(insn), insn, sizeof(insn), 1) != 0) {
                return KVM_MSR_INVAL;
            }
        }
        return 0;
    }
    blob = lm ? x->config.blob_addr_64 : x->config.blob_addr_32;
    size = lm ? x->config.blob_size_64 : x->config.blob_size_32;
    if (page_num >= size) {
        return KVM_MSR_INVAL;
    }
    buf = (uint8_t *)kmalloc(4096u);
    if (!buf) {
        return KVM_MSR_INVAL;
    }
    if (copy_from_user(buf, (const void __user *)(uintptr_t)(blob + page_num * 4096ull), 4096u) !=
            0 ||
        kvm_guest_phys_rw(v, page, buf, 4096u, 1) != 0) {
        r = KVM_MSR_INVAL;
    }
    kfree(buf);
    return r;
}

int kvm_xen_msr_write(struct kvm_vcpu *v, uint32_t index, uint64_t data)
{
    struct xen_vm *x = xvm(v->vm);

    if (!x || !x->config.msr || index != x->config.msr) {
        return 1;
    }
    return hypercall_page(v, data);
}

/* ---- the VM's ioctls ---- */

/** KVM_XEN_HVM_CONFIG, as Linux's kvm_xen_hvm_config: -EINVAL for a flag
 * the caller may not set or a blob with the intercept. */
static int32_t hvm_config(struct kvm_vm *vm, const struct kvm_xen_hvm_config *c)
{
    struct xen_vm *x = xvm(vm);
    uint32_t permitted = KVM_XEN_HVM_CONFIG_INTERCEPT_HCALL | KVM_XEN_HVM_CONFIG_EVTCHN_SEND |
                         KVM_XEN_HVM_CONFIG_PVCLOCK_TSC_UNSTABLE;
    uint32_t old, i;

    if (c->flags & ~permitted) {
        return -EINVAL;
    }
    if ((c->flags & KVM_XEN_HVM_CONFIG_INTERCEPT_HCALL) &&
        (c->blob_addr_32 || c->blob_addr_64 || c->blob_size_32 || c->blob_size_64)) {
        return -EINVAL;
    }
    mutex_lock(&x->lock);
    old = x->config.flags;
    x->config = *c;
    mutex_unlock(&x->lock);
    if ((old ^ c->flags) & KVM_XEN_HVM_CONFIG_PVCLOCK_TSC_UNSTABLE) {
        for (i = 0; i < vm->nr_vcpus; i++) {
            kvm_vcpu_kick(vm->vcpus[i], KVM_REQ_CLOCK_UPDATE);
        }
    }
    return 0;
}

/** Find the outbound channel of `send_port`, and the link to it. Under
 * `lock`. */
static struct xen_port **port_find(struct xen_vm *x, uint32_t send_port)
{
    struct xen_port **pp;

    for (pp = &x->ports; *pp && (*pp)->send_port != send_port; pp = &(*pp)->next) {
    }
    return pp;
}

/** Free an outbound channel taken off the list. */
static void port_free(struct xen_port *p)
{
    if (p->ev) {
        eventfd_put(p->ev);
    }
    kfree(p);
}

/** KVM_XEN_EVTCHN_UPDATE of the outbound channel `a` names: only the
 * vCPU and the priority of a channel to a port change. 0, -ENOENT for no
 * channel on its port, or -EINVAL. */
static int32_t port_update(struct xen_vm *x, const struct kvm_xen_hvm_attr *a)
{
    struct xen_port *p;
    int32_t r = -ENOENT;

    mutex_lock(&x->lock);
    p = *port_find(x, a->u.evtchn.send_port);
    if (p) {
        r = -EINVAL;
        if (p->type == a->u.evtchn.type && p->port && p->port == a->u.evtchn.deliver.port.port &&
            a->u.evtchn.deliver.port.priority == KVM_IRQ_ROUTING_XEN_EVTCHN_PRIO_2LEVEL) {
            p->priority = a->u.evtchn.deliver.port.priority;
            p->vcpu = a->u.evtchn.deliver.port.vcpu;
            r = 0;
        }
    }
    mutex_unlock(&x->lock);
    return r;
}

/** KVM_XEN_ATTR_TYPE_EVTCHN, as Linux's kvm_xen_setattr_evtchn: assign,
 * update, deassign or reset the outbound channels. */
static int32_t setattr_evtchn(struct kvm_vm *vm, const struct kvm_xen_hvm_attr *a)
{
    struct xen_vm *x = xvm(vm);
    struct xen_port *p, **pp;
    uint32_t send = a->u.evtchn.send_port, dport = a->u.evtchn.deliver.port.port;
    int32_t err = 0, r;

    if (a->u.evtchn.flags == KVM_XEN_EVTCHN_RESET) {
        mutex_lock(&x->lock);
        while (x->ports) {
            p = x->ports;
            x->ports = p->next;
            port_free(p);
        }
        mutex_unlock(&x->lock);
        return 0;
    }
    if (!send || send >= max_port(x)) {
        return -EINVAL;
    }
    if (a->u.evtchn.flags == KVM_XEN_EVTCHN_DEASSIGN) {
        mutex_lock(&x->lock);
        pp = port_find(x, send);
        p = *pp;
        if (p) {
            *pp = p->next;
        }
        mutex_unlock(&x->lock);
        if (!p) {
            return -ENOENT;
        }
        port_free(p);
        return 0;
    }
    if (a->u.evtchn.flags == KVM_XEN_EVTCHN_UPDATE) {
        return port_update(x, a);
    }
    if (a->u.evtchn.flags) {
        return -EINVAL;
    }
    p = (struct xen_port *)kzalloc(sizeof(*p));
    if (!p) {
        return -ENOMEM;
    }
    r = -EINVAL;
    if (a->u.evtchn.type == EVTCHNSTAT_ipi) {
        /* An IPI comes back on its own port. */
        if (dport != send) {
            goto fail;
        }
    } else if (a->u.evtchn.type == EVTCHNSTAT_interdomain) {
        if (dport && dport >= max_port(x)) {
            goto fail;
        }
        if (!dport) {
            p->ev = eventfd_get((int)a->u.evtchn.deliver.eventfd.fd, &err);
            if (!p->ev) {
                r = err;
                goto fail;
            }
        }
    } else {
        goto fail;
    }
    p->send_port = send;
    p->type = a->u.evtchn.type;
    if (!p->ev) {
        if (a->u.evtchn.deliver.port.priority != KVM_IRQ_ROUTING_XEN_EVTCHN_PRIO_2LEVEL) {
            goto fail;
        }
        p->port = dport;
        p->vcpu = a->u.evtchn.deliver.port.vcpu;
        p->priority = a->u.evtchn.deliver.port.priority;
    }
    mutex_lock(&x->lock);
    if (*port_find(x, send)) {
        mutex_unlock(&x->lock);
        r = -EEXIST;
        goto fail;
    }
    p->next = x->ports;
    x->ports = p;
    mutex_unlock(&x->lock);
    return 0;
fail:
    port_free(p);
    return r;
}

/** KVM_XEN_HVM_SET_ATTR, as Linux's kvm_xen_hvm_set_attr: -ENOENT for an
 * attribute it does not know. */
static int32_t hvm_set_attr(struct kvm_vm *vm, const struct kvm_xen_hvm_attr *a)
{
    struct xen_vm *x = xvm(vm);
    uint64_t addr;
    int32_t r = 0;

    switch (a->type) {
    case KVM_XEN_ATTR_TYPE_LONG_MODE:
        mutex_lock(&x->lock);
        x->long_mode = a->u.long_mode != 0;
        /* The wall clock moves with the layout. */
        if (x->shinfo.active) {
            r = shinfo_init(vm);
        }
        mutex_unlock(&x->lock);
        return r;
    case KVM_XEN_ATTR_TYPE_SHARED_INFO:
    case KVM_XEN_ATTR_TYPE_SHARED_INFO_HVA:
        mutex_lock(&x->lock);
        if (a->type == KVM_XEN_ATTR_TYPE_SHARED_INFO) {
            addr = a->u.shared_info.gfn;
            if (addr == KVM_XEN_INVALID_GFN) {
                x->shinfo.active = 0;
            } else if (addr >> 52) {
                r = -EFAULT;
            } else {
                r = area_activate(vm, NULL, &x->shinfo, 0, addr << 12, 4096u);
            }
        } else {
            addr = a->u.shared_info.hva;
            if (addr & 0xFFFu) {
                r = -EINVAL;
            } else if (!addr) {
                x->shinfo.active = 0;
            } else {
                r = area_activate(vm, NULL, &x->shinfo, 1, addr, 4096u);
            }
        }
        if (r == 0 && x->shinfo.active) {
            r = shinfo_init(vm);
        }
        mutex_unlock(&x->lock);
        return r;
    case KVM_XEN_ATTR_TYPE_UPCALL_VECTOR:
        if (a->u.vector && a->u.vector < 0x10u) {
            return -EINVAL;
        }
        mutex_lock(&x->lock);
        x->upcall_vector = a->u.vector;
        mutex_unlock(&x->lock);
        return 0;
    case KVM_XEN_ATTR_TYPE_EVTCHN:
        return setattr_evtchn(vm, a);
    case KVM_XEN_ATTR_TYPE_XEN_VERSION:
        mutex_lock(&x->lock);
        x->xen_version = a->u.xen_version;
        mutex_unlock(&x->lock);
        return 0;
    case KVM_XEN_ATTR_TYPE_RUNSTATE_UPDATE_FLAG:
        mutex_lock(&x->lock);
        x->runstate_update_flag = a->u.runstate_update_flag != 0;
        mutex_unlock(&x->lock);
        return 0;
    default:
        return -ENOENT;
    }
}

/** KVM_XEN_HVM_GET_ATTR, as Linux's kvm_xen_hvm_get_attr. */
static int32_t hvm_get_attr(struct kvm_vm *vm, struct kvm_xen_hvm_attr *a)
{
    struct xen_vm *x = xvm(vm);
    int32_t r = 0;

    mutex_lock(&x->lock);
    switch (a->type) {
    case KVM_XEN_ATTR_TYPE_LONG_MODE:
        a->u.long_mode = (uint8_t)x->long_mode;
        break;
    case KVM_XEN_ATTR_TYPE_SHARED_INFO:
        a->u.shared_info.gfn =
            x->shinfo.active && !x->shinfo.hva ? x->shinfo.addr >> 12 : KVM_XEN_INVALID_GFN;
        break;
    case KVM_XEN_ATTR_TYPE_SHARED_INFO_HVA:
        a->u.shared_info.hva = x->shinfo.active && x->shinfo.hva ? x->shinfo.addr : 0;
        break;
    case KVM_XEN_ATTR_TYPE_UPCALL_VECTOR:
        a->u.vector = (uint8_t)x->upcall_vector;
        break;
    case KVM_XEN_ATTR_TYPE_XEN_VERSION:
        a->u.xen_version = x->xen_version;
        break;
    case KVM_XEN_ATTR_TYPE_RUNSTATE_UPDATE_FLAG:
        a->u.runstate_update_flag = (uint8_t)x->runstate_update_flag;
        break;
    default:
        r = -ENOENT;
        break;
    }
    mutex_unlock(&x->lock);
    return r;
}

/** KVM_XEN_HVM_EVTCHN_SEND, as Linux's kvm_xen_hvm_evtchn_send: 0 for an
 * event raised, already pending or masked. */
static int32_t evtchn_send(struct kvm_vm *vm, const struct kvm_irq_routing_xen_evtchn *e)
{
    struct xen_vm *x = xvm(vm);
    int r;

    if (!e->port || e->port >= max_port(x) ||
        e->priority != KVM_IRQ_ROUTING_XEN_EVTCHN_PRIO_2LEVEL) {
        return -EINVAL;
    }
    mutex_lock(&x->lock);
    r = set_evtchn_id(vm, NULL, e->vcpu, e->port);
    mutex_unlock(&x->lock);
    return r > 0 || r == -ENOTCONN ? 0 : r;
}

int32_t kvm_xen_vm_ioctl(struct kvm_vm *vm, uint32_t request, uintptr_t arg)
{
    struct kvm_xen_hvm_config c;
    struct kvm_xen_hvm_attr a;
    struct kvm_irq_routing_xen_evtchn e;
    int32_t r;

    switch (request) {
    case KVM_XEN_HVM_CONFIG:
        if (copy_from_user(&c, (const void __user *)arg, sizeof(c)) != 0) {
            return -EFAULT;
        }
        return hvm_config(vm, &c);
    case KVM_XEN_HVM_GET_ATTR:
    case KVM_XEN_HVM_SET_ATTR:
        if (copy_from_user(&a, (const void __user *)arg, sizeof(a)) != 0) {
            return -EFAULT;
        }
        if (request == KVM_XEN_HVM_SET_ATTR) {
            return hvm_set_attr(vm, &a);
        }
        r = hvm_get_attr(vm, &a);
        if (r == 0 && copy_to_user((void __user *)arg, &a, sizeof(a)) != 0) {
            r = -EFAULT;
        }
        return r;
    case KVM_XEN_HVM_EVTCHN_SEND:
        if (copy_from_user(&e, (const void __user *)arg, sizeof(e)) != 0) {
            return -EFAULT;
        }
        return evtchn_send(vm, &e);
    default:
        return -ENOTTY;
    }
}

int kvm_xen_route_check(struct kvm_vm *vm, uint32_t port, uint32_t priority)
{
    return port >= max_port(xvm(vm)) || priority != KVM_IRQ_ROUTING_XEN_EVTCHN_PRIO_2LEVEL ? -EINVAL
                                                                                           : 0;
}

int kvm_xen_route_set(struct kvm_vm *vm, uint32_t vcpu, uint32_t port)
{
    struct xen_vm *x = xvm(vm);
    int r;

    mutex_lock(&x->lock);
    r = set_evtchn_id(vm, NULL, vcpu, port);
    mutex_unlock(&x->lock);
    return r;
}

/* ---- the vCPU's ioctls ---- */

/** Activate or, for KVM_XEN_INVALID_GPA, deactivate the vCPU area `a` at
 * the guest's `gpa`, `len` bytes. */
static int32_t vcpu_area_gpa(struct kvm_vcpu *v, struct xen_area *a, uint64_t gpa, uint32_t len)
{
    if (gpa == KVM_XEN_INVALID_GPA) {
        a->active = 0;
        return 0;
    }
    return area_activate(v->vm, v, a, 0, gpa, len);
}

/** KVM_XEN_VCPU_ATTR_TYPE_RUNSTATE_ADDR: the area at `gpa`, which may
 * cross into a second page, both pages in guest memory; the record is
 * written at once. */
static int32_t runstate_addr(struct kvm_vcpu *v, uint64_t gpa)
{
    struct xen_vm *x = xvm(v->vm);
    struct xen_vcpu *xv = xvc(v);
    struct xen_area a;
    uint32_t len, times, sz1;
    int32_t r;

    xv->rs_active = 0;
    if (gpa == KVM_XEN_INVALID_GPA) {
        return 0;
    }
    rs_layout(x, &len, &times);
    sz1 = 4096u - (uint32_t)(gpa & 0xFFFu);
    r = area_activate(v->vm, v, &a, 0, gpa, sz1);
    if (r == 0 && sz1 < len) {
        r = area_activate(v->vm, v, &a, 0, gpa + sz1, len - sz1);
    }
    if (r != 0) {
        return r;
    }
    xv->rs_gpa = gpa;
    xv->rs_active = 1;
    xv->rs_ok = 0;
    rs_write(v);
    return 0;
}

/** Whether the times of `rs` add up to its entry time. */
static int rs_sums(const struct kvm_xen_vcpu_attr *a)
{
    return a->u.runstate.state_entry_time ==
           a->u.runstate.time_running + a->u.runstate.time_runnable + a->u.runstate.time_blocked +
               a->u.runstate.time_offline;
}

/** KVM_XEN_VCPU_ATTR_TYPE_VCPU_INFO or its _HVA form: move `v`'s
 * vcpu_info to the guest or caller's address `a` gives, or take it away
 * for KVM_XEN_INVALID_GPA or a caller's address of 0. 0 or
 * `area_activate`'s error. Under the Xen lock. */
static int32_t info_set(struct kvm_vcpu *v, const struct kvm_xen_vcpu_attr *a)
{
    struct xen_vcpu *xv = xvc(v);
    int hva = a->type == KVM_XEN_VCPU_ATTR_TYPE_VCPU_INFO_HVA;
    int32_t r;

    if (hva ? a->u.hva == 0 : a->u.gpa == KVM_XEN_INVALID_GPA) {
        xv->info.active = 0;
        return 0;
    }
    r = area_activate(v->vm, v, &xv->info, hva, hva ? a->u.hva : a->u.gpa, INFO_SIZE);
    if (r == 0) {
        kvm_vcpu_kick(v, KVM_REQ_CLOCK_UPDATE);
    }
    return r;
}

/** KVM_XEN_VCPU_SET_ATTR, as Linux's kvm_xen_vcpu_set_attr. Under the
 * vCPU's mutex and the big lock. */
static int32_t vcpu_set_attr(struct kvm_vcpu *v, const struct kvm_xen_vcpu_attr *a)
{
    struct xen_vm *x = xvm(v->vm);
    struct xen_vcpu *xv = xvc(v);
    int32_t r = 0;

    mutex_lock(&x->lock);
    switch (a->type) {
    case KVM_XEN_VCPU_ATTR_TYPE_VCPU_INFO:
    case KVM_XEN_VCPU_ATTR_TYPE_VCPU_INFO_HVA:
        r = info_set(v, a);
        break;
    case KVM_XEN_VCPU_ATTR_TYPE_VCPU_TIME_INFO:
        r = vcpu_area_gpa(v, &xv->time, a->u.gpa, TIME_SIZE);
        if (r == 0 && a->u.gpa != KVM_XEN_INVALID_GPA) {
            kvm_vcpu_kick(v, KVM_REQ_CLOCK_UPDATE);
        }
        break;
    case KVM_XEN_VCPU_ATTR_TYPE_RUNSTATE_ADDR:
        r = runstate_addr(v, a->u.gpa);
        break;
    case KVM_XEN_VCPU_ATTR_TYPE_RUNSTATE_CURRENT:
        if (a->u.runstate.state > RUNSTATE_offline) {
            r = -EINVAL;
            break;
        }
        rs_update(v, (uint32_t)a->u.runstate.state);
        break;
    case KVM_XEN_VCPU_ATTR_TYPE_RUNSTATE_DATA:
        if (a->u.runstate.state > RUNSTATE_offline || !rs_sums(a) ||
            kvm_clock_ns(v->vm) < a->u.runstate.state_entry_time) {
            r = -EINVAL;
            break;
        }
        xv->rs_state = (uint32_t)a->u.runstate.state;
        xv->rs_entry = a->u.runstate.state_entry_time;
        xv->rs_times[0] = a->u.runstate.time_running;
        xv->rs_times[1] = a->u.runstate.time_runnable;
        xv->rs_times[2] = a->u.runstate.time_blocked;
        xv->rs_times[3] = a->u.runstate.time_offline;
        xv->rs_steal = current_proc->run_delay;
        break;
    case KVM_XEN_VCPU_ATTR_TYPE_RUNSTATE_ADJUST:
        if ((a->u.runstate.state > RUNSTATE_offline && a->u.runstate.state != ~0ull) ||
            !rs_sums(a) || kvm_clock_ns(v->vm) < xv->rs_entry + a->u.runstate.state_entry_time) {
            r = -EINVAL;
            break;
        }
        xv->rs_entry += a->u.runstate.state_entry_time;
        xv->rs_times[0] += a->u.runstate.time_running;
        xv->rs_times[1] += a->u.runstate.time_runnable;
        xv->rs_times[2] += a->u.runstate.time_blocked;
        xv->rs_times[3] += a->u.runstate.time_offline;
        if (a->u.runstate.state <= RUNSTATE_offline) {
            rs_update(v, (uint32_t)a->u.runstate.state);
        } else {
            rs_write(v);
        }
        break;
    case KVM_XEN_VCPU_ATTR_TYPE_VCPU_ID:
        if (a->u.vcpu_id >= KVM_MAX_VCPUS) {
            r = -EINVAL;
        } else {
            xv->vcpu_id = a->u.vcpu_id;
        }
        break;
    case KVM_XEN_VCPU_ATTR_TYPE_TIMER:
        if (a->u.timer.port && a->u.timer.priority != KVM_IRQ_ROUTING_XEN_EVTCHN_PRIO_2LEVEL) {
            r = -EINVAL;
            break;
        }
        timer_stop(v);
        xv->timer_virq = a->u.timer.port;
        if (a->u.timer.port && a->u.timer.expires_ns) {
            timer_start(v, a->u.timer.expires_ns, 0, 0);
        }
        break;
    case KVM_XEN_VCPU_ATTR_TYPE_UPCALL_VECTOR:
        if (a->u.vector && a->u.vector < 0x10u) {
            r = -EINVAL;
        } else {
            xv->upcall_vector = a->u.vector;
        }
        break;
    default:
        r = -ENOENT;
        break;
    }
    mutex_unlock(&x->lock);
    return r;
}

/** KVM_XEN_VCPU_GET_ATTR, as Linux's kvm_xen_vcpu_get_attr. A timer that
 * is due is delivered first, so the answer either has it armed or has its
 * event raised. */
static int32_t vcpu_get_attr(struct kvm_vcpu *v, struct kvm_xen_vcpu_attr *a)
{
    struct xen_vm *x = xvm(v->vm);
    struct xen_vcpu *xv = xvc(v);
    int32_t r = 0;

    mutex_lock(&x->lock);
    switch (a->type) {
    case KVM_XEN_VCPU_ATTR_TYPE_VCPU_INFO:
        a->u.gpa = xv->info.active && !xv->info.hva ? xv->info.addr : KVM_XEN_INVALID_GPA;
        break;
    case KVM_XEN_VCPU_ATTR_TYPE_VCPU_INFO_HVA:
        a->u.hva = xv->info.active && xv->info.hva ? xv->info.addr : 0;
        break;
    case KVM_XEN_VCPU_ATTR_TYPE_VCPU_TIME_INFO:
        a->u.gpa = xv->time.active ? xv->time.addr : KVM_XEN_INVALID_GPA;
        break;
    case KVM_XEN_VCPU_ATTR_TYPE_RUNSTATE_ADDR:
        if (xv->rs_active) {
            a->u.gpa = xv->rs_gpa;
        } else {
            r = -ENOENT;
        }
        break;
    case KVM_XEN_VCPU_ATTR_TYPE_RUNSTATE_CURRENT:
        a->u.runstate.state = xv->rs_state;
        break;
    case KVM_XEN_VCPU_ATTR_TYPE_RUNSTATE_DATA:
        a->u.runstate.state = xv->rs_state;
        a->u.runstate.state_entry_time = xv->rs_entry;
        a->u.runstate.time_running = xv->rs_times[0];
        a->u.runstate.time_runnable = xv->rs_times[1];
        a->u.runstate.time_blocked = xv->rs_times[2];
        a->u.runstate.time_offline = xv->rs_times[3];
        break;
    case KVM_XEN_VCPU_ATTR_TYPE_RUNSTATE_ADJUST:
        r = -EINVAL;
        break;
    case KVM_XEN_VCPU_ATTR_TYPE_VCPU_ID:
        a->u.vcpu_id = xv->vcpu_id;
        break;
    case KVM_XEN_VCPU_ATTR_TYPE_TIMER:
        if (xv->timer_host && kvm_host_ns() >= xv->timer_host) {
            timer_fire(v);
        }
        a->u.timer.port = xv->timer_virq;
        a->u.timer.priority = KVM_IRQ_ROUTING_XEN_EVTCHN_PRIO_2LEVEL;
        a->u.timer.expires_ns = xv->timer_expires;
        break;
    case KVM_XEN_VCPU_ATTR_TYPE_UPCALL_VECTOR:
        a->u.vector = (uint8_t)xv->upcall_vector;
        break;
    default:
        r = -ENOENT;
        break;
    }
    mutex_unlock(&x->lock);
    return r;
}

int32_t kvm_xen_vcpu_ioctl(struct kvm_vcpu *v, uint32_t request, uintptr_t arg)
{
    struct kvm_xen_vcpu_attr a;
    int32_t r;

    if (copy_from_user(&a, (const void __user *)arg, sizeof(a)) != 0) {
        return -EFAULT;
    }
    if (request == KVM_XEN_VCPU_SET_ATTR) {
        return vcpu_set_attr(v, &a);
    }
    r = vcpu_get_attr(v, &a);
    if (r == 0 && copy_to_user((void __user *)arg, &a, sizeof(a)) != 0) {
        r = -EFAULT;
    }
    return r;
}

/* ---- life ---- */

int kvm_xen_vm_init(struct kvm_vm *vm)
{
    struct xen_vm *x = (struct xen_vm *)kzalloc(sizeof(*x));

    if (!x) {
        return -ENOMEM;
    }
    mutex_init(&x->lock, "kvm_xen");
    vm->xen = x;
    return 0;
}

void kvm_xen_vm_free(struct kvm_vm *vm)
{
    struct xen_vm *x = xvm(vm);
    struct xen_port *p;

    if (!x) {
        return;
    }
    while (x->ports) {
        p = x->ports;
        x->ports = p->next;
        port_free(p);
    }
    kfree(x);
    vm->xen = NULL;
}

int kvm_xen_vcpu_init(struct kvm_vcpu *v)
{
    struct xen_vcpu *xv = (struct xen_vcpu *)kzalloc(sizeof(*xv));

    if (!xv) {
        return -ENOMEM;
    }
    xv->vcpu_id = v->index;
    xv->rs_skew = (int64_t)(kvm_clock_ns(v->vm) - kvm_host_ns());
    v->xen = xv;
    return 0;
}

void kvm_xen_vcpu_free(struct kvm_vcpu *v)
{
    kfree(v->xen);
    v->xen = NULL;
}
