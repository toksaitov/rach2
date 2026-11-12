/*
 * pvclock.c: the kvmclock a guest reads, with its master clock and wall
 * clock, KVM_GET_CLOCK and KVM_SET_CLOCK, the matching of the vCPUs' TSCs,
 * the steal time, and KVM's own MSRs, as Linux's arch/x86/kvm/x86.c keeps
 * them. The VM's clock fields are its `clock_lock`'s, as Linux keeps them
 * under tsc_write_lock and pvclock_sc (x86.c:2240-2330, 2585-2650); a
 * vCPU copies them out under the lock and writes guest memory after.
 */

#include "kvm.h"

#include "../arch/arch.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../mm/mm.h"
#include "../mm/usercopy.h"
#include "../mm/vm.h"
#include "../proc/process.h"
#include "../proc/smp.h"
#include "../rtc.h"
#include "../timer.h"
#include "../vdso.h"

#define NSEC_PER_SEC 1000000000ull

/* Without the master clock, a vCPU's kvmclock is written again at least
 * this often, Linux's KVMCLOCK_SYNC_PERIOD, so that its formula from the
 * TSC does not drift from the host's clock. */
#define KVMCLOCK_SYNC_NS (300ull * NSEC_PER_SEC)

/* The tolerance within which a rate is the host's, in parts in a million,
 * Linux's tsc_tolerance_ppm. */
#define TSC_TOLERANCE_PPM 250u

uint64_t kvm_tsc_khz(void)
{
    return (uint64_t)timer_calibrate() * TICK_HZ / 1000u;
}

/** The backend's largest TSC ratio, with its fraction's width in `*frac`,
 * or 0 when the processor does not scale the TSC. */
static uint64_t max_ratio(uint32_t *frac)
{
    *frac = 0;
    return kvm_backend && kvm_backend->tsc_scaling ? kvm_backend->tsc_scaling(frac) : 0;
}

/** `a` times `b` over 2^`shift`, for a shift of 1 to 63, the product kept
 * whole in between. */
static uint64_t mul_shr(uint64_t a, uint64_t b, uint32_t shift)
{
    uint64_t a0 = a & 0xFFFFFFFFull, a1 = a >> 32, b0 = b & 0xFFFFFFFFull, b1 = b >> 32;
    uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0;
    uint64_t mid = (p00 >> 32) + (p01 & 0xFFFFFFFFull) + (p10 & 0xFFFFFFFFull);
    uint64_t lo = (p00 & 0xFFFFFFFFull) | (mid << 32);
    uint64_t hi = a1 * b1 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);

    return (lo >> shift) | (hi << (64u - shift));
}

int kvm_tsc_control(void)
{
    uint32_t frac;

    return max_ratio(&frac) != 0;
}

uint32_t kvm_tsc_max_khz(void)
{
    uint64_t max, top;
    uint32_t frac;

    max = max_ratio(&frac);
    if (!max) {
        return 0;
    }
    top = mul_shr(kvm_tsc_khz(), max, frac);
    return top < 0x7FFFFFFFull ? (uint32_t)top : 0x7FFFFFFFu;
}

uint32_t kvm_tsc_khz_arg(uintptr_t arg)
{
    uint32_t khz = (uint32_t)arg;

    if (kvm_tsc_control() && khz >= kvm_tsc_max_khz()) {
        return 0;
    }
    return khz ? khz : (uint32_t)kvm_tsc_khz();
}

uint64_t kvm_tsc_scale(const struct kvm_vcpu *v, uint64_t tsc)
{
    return v->tsc_ratio ? mul_shr(tsc, v->tsc_ratio, v->tsc_frac_bits) : tsc;
}

uint64_t kvm_tsc_read(const struct kvm_vcpu *v)
{
    return kvm_tsc_scale(v, arch_cycles()) + (uint64_t)v->tsc_offset;
}

uint64_t kvm_tsc_hz(const struct kvm_vcpu *v)
{
    return v->tsc_ratio ? (uint64_t)v->tsc_khz * 1000u : kvm_tsc_khz() * 1000u;
}

int kvm_tsc_set_khz(struct kvm_vcpu *v, uint32_t khz)
{
    uint64_t host = kvm_tsc_khz(), max, ratio, rest;
    uint32_t frac, done;

    v->tsc_ratio = 0;
    v->tsc_catchup = 0;
    v->dirty |= KVM_PART_TSC;
    if (khz == 0) {
        return -1;
    }
    v->tsc_khz = khz;
    if (khz >= host * (1000000u - TSC_TOLERANCE_PPM) / 1000000u &&
        khz <= host * (1000000u + TSC_TOLERANCE_PPM) / 1000000u) {
        return 0;
    }
    max = max_ratio(&frac);
    if (!max) {
        if (khz < host) {
            return -1;
        }
        v->tsc_catchup = 1;
        return 0;
    }
    if (host == 0) {
        return -1;
    }
    /* 2^frac * khz / host by long division, 16 bits of the fraction at a
     * time, since the remainder is below 2^32. */
    ratio = (khz / host) << frac;
    rest = khz % host;
    for (done = 0; done < frac; done += 16u) {
        rest <<= 16;
        ratio += (rest / host) << (frac - 16u - done);
        rest %= host;
    }
    if (ratio == 0 || ratio >= max) {
        return -1;
    }
    v->tsc_ratio = ratio;
    v->tsc_frac_bits = frac;
    return 0;
}

/**
 * The kvmclock's multiplier and shift that turn counts of a clock of
 * `base_hz` into counts of one of `scaled_hz`, as Linux's
 * kvm_get_time_scale: the shift brings the base below 2^32 and above the
 * scaled rate, and the multiplier is the scaled rate over the shifted base
 * in 32-bit fixed point. `base_hz` is not 0.
 */
static void time_scale(uint64_t scaled_hz, uint64_t base_hz, int8_t *shift_out, uint32_t *mul_out)
{
    uint64_t scaled = scaled_hz, base = base_hz;
    uint32_t base32;
    int shift = 0;

    while (base > scaled * 2u || (base & 0xFFFFFFFF00000000ull)) {
        base >>= 1;
        shift--;
    }
    base32 = (uint32_t)base;
    while (base32 <= scaled || (scaled & 0xFFFFFFFF00000000ull)) {
        if ((scaled & 0xFFFFFFFF00000000ull) || (base32 & 0x80000000u)) {
            scaled >>= 1;
        } else {
            base32 <<= 1;
        }
        shift++;
    }
    *shift_out = (int8_t)shift;
    *mul_out = (uint32_t)((scaled << 32) / base32);
}

/** `delta` counts scaled by `mul` and `shift` as the guest's
 * pvclock_scale_delta does it: shifted, then times `mul` over 2^32. */
static uint64_t pv_scale(uint64_t delta, uint32_t mul, int8_t shift)
{
    if (shift < 0) {
        delta >>= -shift;
    } else {
        delta <<= shift;
    }
    return (delta >> 32) * mul + (((delta & 0xFFFFFFFFull) * mul) >> 32);
}

/** `ns` nanoseconds as counts of a TSC of `khz`. */
static uint64_t ns_to_cycles(uint64_t ns, uint64_t khz)
{
    uint32_t mul;
    int8_t shift;

    time_scale(khz * 1000u, NSEC_PER_SEC, &shift, &mul);
    return pv_scale(ns, mul, shift);
}

/* A VM's master clock and offset, copied out under `clock_lock`. */
struct master {
    int on; /* `master_clock` */
    uint64_t ns, tsc;
    uint32_t mul;
    int8_t shift;
    int64_t offset; /* `clock_offset` */
};

/** Take `clock_lock` of `vm`: the flags to restore. */
static uint32_t clock_lock(struct kvm_vm *vm)
{
    return spin_lock_irqsave(&vm->clock_lock);
}

/** Give back what `clock_lock` took. */
static void clock_unlock(struct kvm_vm *vm, uint32_t flags)
{
    spin_unlock_irqrestore(&vm->clock_lock, flags);
}

/** Copy `vm`'s master clock into `m`, under `clock_lock`. */
static void master_copy_locked(const struct kvm_vm *vm, struct master *m)
{
    m->on = vm->master_clock;
    m->ns = vm->master_ns;
    m->tsc = vm->master_tsc;
    m->mul = vm->master_mul;
    m->shift = vm->master_shift;
    m->offset = vm->clock_offset;
}

/** Copy `vm`'s master clock into `m`, taking `clock_lock`. */
static void master_copy(struct kvm_vm *vm, struct master *m)
{
    uint32_t flags = clock_lock(vm);

    master_copy_locked(vm, m);
    clock_unlock(vm, flags);
}

/** The kvmclock at the host's TSC count `tsc`, by the master clock's
 * formula `m`; a count before the master clock's moment, as another
 * processor's may read by a few counts, reads as that moment. */
static uint64_t master_at(const struct master *m, uint64_t tsc)
{
    uint64_t delta = tsc > m->tsc ? tsc - m->tsc : 0;

    return m->ns + (uint64_t)m->offset + pv_scale(delta, m->mul, m->shift);
}

uint64_t kvm_clock_ns(struct kvm_vm *vm)
{
    struct master m;

    master_copy(vm, &m);
    if (m.on) {
        return master_at(&m, arch_cycles());
    }
    return kvm_host_ns() + (uint64_t)m.offset;
}

int kvm_clock_stable_read(struct kvm_vm *vm, uint64_t *ns, uint64_t *host_tsc)
{
    struct master m;

    master_copy(vm, &m);
    if (!m.on) {
        return 0;
    }
    *host_tsc = arch_cycles();
    *ns = master_at(&m, *host_tsc);
    return 1;
}

/** Whether every vCPU of `vm` has joined the generation of matched TSCs,
 * Linux's nr_vcpus_matched_tsc + 1 == online_vcpus. */
static int tsc_all_matched(const struct kvm_vm *vm)
{
    return vm->tsc_matched + 1u == vm->nr_vcpus;
}

/**
 * Take the master clock's moment anew and decide whether the kvmclock runs
 * from it, as Linux's pvclock_update_vm_gtod_copy between
 * kvm_start_pvclock_update and kvm_end_pvclock_update: it does where the
 * host's clock runs from the TSC, every vCPU's TSC is of one generation and
 * the boot vCPU's guest uses the new MSR, and it is decided again when the
 * host's clock stops or starts (`master_recheck`). Every vCPU first leaves
 * the guest with a request to write its kvmclock, which it cannot act on
 * before the caller lets `clock_lock` go, since the write copies the master
 * clock under it and a request cancels an entry (KVM_REQ_NOENTER), so that
 * none reads the old formula once another reads the new, as Linux's
 * KVM_REQ_MCLOCK_INPROGRESS holds them. `hv_clock_moved` is set before the
 * kicks, as Linux's kvm_update_masterclock asks for the reference TSC page's
 * update first (x86.c:2652-2654), so that a kicked vCPU that takes the
 * request sees it and rewrites the page from the new clock, which it copies
 * under `clock_lock`, before it enters: in `kvm_clock_prepare`, which takes
 * the request, or in `kvm_hv_prepare`, which comes before it in the run
 * loop. Under `clock_lock`.
 */
static void master_update(struct kvm_vm *vm)
{
    uint64_t ns, tsc, khz = kvm_tsc_khz();
    uint32_t i, n = vm->nr_vcpus;

    vm->hv_clock_moved = 1;
    for (i = 0; i < n; i++) {
        kvm_vcpu_kick(vm->vcpus[i], KVM_REQ_CLOCK_UPDATE);
    }
    for (i = 0; i < n; i++) {
        while (vm->vcpus[i]->mode == KVM_MODE_IN_GUEST) {
            smp_service();
            arch_relax();
        }
    }
    vm->host_clock_gen = vclock_generation();
    if (khz != 0 && vclock_read_cycles(VCLOCK_READ_RAW, &ns, &tsc)) {
        vm->master_clock = tsc_all_matched(vm) && !vm->old_kvmclock;
        vm->master_ns = ns;
        vm->master_tsc = tsc;
        time_scale(NSEC_PER_SEC, khz * 1000u, &vm->master_shift, &vm->master_mul);
    } else {
        vm->master_clock = 0;
    }
}

int kvm_clock_host_moved(const struct kvm_vm *vm)
{
    return vm->host_clock_gen != vclock_generation();
}

/**
 * Decide `vm`'s master clock again when the host's clock has stopped or
 * started running from the TSC since it was last decided, as Linux's
 * pvclock_gtod_notify asks every vCPU for KVM_REQ_MASTERCLOCK_UPDATE once
 * the host's clocksource leaves the TSC (arch/x86/kvm/x86.c:9159-9204), so
 * that no guest keeps PVCLOCK_TSC_STABLE_BIT, or a reference TSC page, from
 * a clock that no longer holds: `master_update` has every vCPU write its
 * kvmclock, and the reference TSC page be written, before its next entry.
 * The generation is read before the host's clock (`master_update`), so a
 * change after the read is seen at the next call, and a change after a
 * vCPU's call is seen by its entry, which `kvm_clock_host_moved` cancels.
 * The test takes no lock; the first vCPU to see the change decides for all,
 * under `clock_lock`. In a vCPU's thread or an ioctl's.
 */
static void master_recheck(struct kvm_vm *vm)
{
    uint32_t flags;

    if (!kvm_clock_host_moved(vm)) {
        return;
    }
    flags = clock_lock(vm);
    if (kvm_clock_host_moved(vm)) {
        master_update(vm);
    }
    clock_unlock(vm, flags);
}

void kvm_clock_vm_init(struct kvm_vm *vm)
{
    uint32_t flags = clock_lock(vm);

    vm->clock_offset = -(int64_t)kvm_host_ns();
    master_update(vm);
    clock_unlock(vm, flags);
}

int32_t kvm_clock_get(struct kvm_vm *vm, uintptr_t arg)
{
    struct kvm_clock_data d;
    struct master m;
    uint64_t real;

    k_memset(&d, 0, sizeof(d));
    master_recheck(vm);
    master_copy(vm, &m);
    if (m.on) {
        if (vclock_read_cycles(VCLOCK_READ_REAL, &real, &d.host_tsc)) {
            d.realtime = real;
            d.flags = KVM_CLOCK_REALTIME | KVM_CLOCK_HOST_TSC;
        } else {
            d.host_tsc = arch_cycles();
        }
        d.flags |= KVM_CLOCK_TSC_STABLE;
        d.clock = master_at(&m, d.host_tsc);
    } else {
        d.clock = kvm_host_ns() + (uint64_t)m.offset;
    }
    return copy_to_user((void __user *)arg, &d, sizeof(d)) ? -EFAULT : 0;
}

int32_t kvm_clock_set(struct kvm_vm *vm, uintptr_t arg)
{
    struct kvm_clock_data d;
    uint64_t now, real;
    uint32_t flags;
    int64_t s, ns;

    if (copy_from_user(&d, (const void __user *)arg, sizeof(d)) != 0) {
        return -EFAULT;
    }
    if (d.flags & ~KVM_CLOCK_VALID_FLAGS) {
        return -EINVAL;
    }
    flags = clock_lock(vm);
    master_update(vm);
    if (d.flags & KVM_CLOCK_REALTIME) {
        rtc_realtime(&s, &ns);
        real = (uint64_t)s * NSEC_PER_SEC + (uint64_t)ns;
        if (real > d.realtime) {
            d.clock += real - d.realtime;
        }
    }
    /* With the master clock, its own moment, so that the kvmclock's
     * system_time, which is unsigned, is the caller's clock exactly. */
    now = vm->master_clock ? vm->master_ns : kvm_host_ns();
    vm->clock_offset = (int64_t)(d.clock - now);
    clock_unlock(vm, flags);
    return 0;
}

/** Have `v`'s kvmclock and the VM's reference TSC page written again
 * before the next entry, since `v`'s TSC changed, and its TSC deadline
 * found again (KVM_REQ_TIMER); the flag goes before the kick, as in
 * `master_update`. */
static void tsc_changed(struct kvm_vcpu *v)
{
    v->vm->hv_clock_moved = 1;
    kvm_vcpu_kick(v, KVM_REQ_CLOCK_UPDATE | KVM_REQ_TIMER);
}

/**
 * Give `v`'s TSC the offset `offset`, which reads `data` at the host's
 * `ns`, as Linux's __kvm_synchronize_tsc: the VM keeps it as the last
 * write, and `matched`, that it joins the generation of matched writes,
 * keeps the generation, where otherwise it starts a new one. A vCPU that
 * joins a generation counts from the generation's moment and value, not
 * from now, as Linux sets this_tsc_nsec and this_tsc_write, so that
 * catch-up moves it with the others. The master clock follows, as Linux's
 * kvm_track_tsc_matching: a new generation takes a new moment, after it,
 * and so does the master clock turned on or off. Under `clock_lock`.
 */
static void tsc_commit(struct kvm_vcpu *v, int64_t offset, uint64_t data, uint64_t ns, int matched)
{
    struct kvm_vm *vm = v->vm;
    int use;

    vm->tsc_last_ns = ns;
    vm->tsc_last_write = data;
    vm->tsc_last_khz = v->tsc_khz;
    vm->tsc_last_offset = offset;
    v->tsc_offset = offset;
    v->dirty |= KVM_PART_TSC;
    if (!matched) {
        vm->tsc_gen++;
        vm->tsc_cur_ns = ns;
        vm->tsc_cur_write = data;
        vm->tsc_cur_offset = offset;
        vm->tsc_matched = 0;
    } else if (v->tsc_gen != vm->tsc_gen) {
        vm->tsc_matched++;
    }
    v->tsc_gen = vm->tsc_gen;
    v->tsc_this_ns = vm->tsc_cur_ns;
    v->tsc_this_write = vm->tsc_cur_write;
    use = tsc_all_matched(vm) && vclock_running();
    if ((vm->master_clock && !matched) || vm->master_clock != use) {
        master_update(vm);
    }
    tsc_changed(v);
}

void kvm_tsc_sync(struct kvm_vcpu *v, const uint64_t *value)
{
    struct kvm_vm *vm = v->vm;
    uint32_t flags = clock_lock(vm);
    uint64_t data = value ? *value : 0, khz = v->tsc_khz, ns = kvm_host_ns();
    uint64_t elapsed = ns - vm->tsc_last_ns, expect, hz;
    int64_t offset = (int64_t)(data - kvm_tsc_scale(v, arch_cycles()));
    int synchronizing = 0, matched = 0;

    if (khz) {
        if (data == 0) {
            /* A new vCPU's, or the caller's write of 0. */
            synchronizing = 1;
        } else if (vm->user_set_tsc) {
            /* A caller's write within a second of the TSC the last write
             * would read now was meant to match it, Linux's allowance for
             * the vCPUs' writes made one after another. */
            expect = vm->tsc_last_write + ns_to_cycles(elapsed, khz);
            hz = khz * 1000u;
            synchronizing = data < expect + hz && data + hz > expect;
        }
    }
    if (value) {
        vm->user_set_tsc = 1;
    }
    if (synchronizing && khz == vm->tsc_last_khz) {
        if (vclock_running()) {
            offset = vm->tsc_cur_offset; /* the processors' TSCs agree */
        } else {
            data += ns_to_cycles(elapsed, khz);
            offset = (int64_t)(data - kvm_tsc_scale(v, arch_cycles()));
        }
        matched = 1;
    }
    tsc_commit(v, offset, data, ns, matched);
    clock_unlock(vm, flags);
}

void kvm_tsc_offset_set(struct kvm_vcpu *v, int64_t offset)
{
    struct kvm_vm *vm = v->vm;
    uint32_t flags = clock_lock(vm);
    int matched;

    matched = v->tsc_khz != 0 && vm->tsc_last_khz == v->tsc_khz && vm->tsc_last_offset == offset;
    vm->user_set_tsc = 1;
    tsc_commit(v, offset, kvm_tsc_scale(v, arch_cycles()) + (uint64_t)offset, kvm_host_ns(),
               matched);
    clock_unlock(vm, flags);
}

void kvm_tsc_moved(struct kvm_vcpu *v)
{
    v->tsc_this_ns = kvm_host_ns();
    v->tsc_this_write = kvm_tsc_read(v);
    tsc_changed(v);
}

/** The real time at which `vm`'s kvmclock read 0, in ns, as Linux's
 * kvm_get_wall_clock_epoch: with the master clock, the real time and the
 * kvmclock at one TSC count. */
static uint64_t wall_epoch(struct kvm_vm *vm)
{
    struct master m;
    uint64_t real, tsc;
    int64_t s, ns;

    master_copy(vm, &m);
    if (m.on && vclock_read_cycles(VCLOCK_READ_REAL, &real, &tsc)) {
        return real - master_at(&m, tsc);
    }
    rtc_realtime(&s, &ns);
    return (uint64_t)s * NSEC_PER_SEC + (uint64_t)ns - kvm_clock_ns(vm);
}

/** Write the wall clock at the guest's `gpa`, as Linux's
 * kvm_write_wall_clock: its version odd, the time, its version even. A
 * `gpa` of 0, which QEMU writes at each reset, writes nothing, and a write
 * that misses guest memory is dropped, as Linux drops both. */
static void wall_clock_write(struct kvm_vcpu *v, uint64_t gpa)
{
    struct pvclock_wall_clock wc;
    uint64_t wall;
    uint32_t version = 0;

    if (gpa == 0) {
        return;
    }
    if (kvm_guest_phys_rw(v, gpa, &version, 4, 0) != 0) {
        return;
    }
    if (version & 1u) {
        version++; /* the first write, over what the guest left there */
    }
    version++;
    if (kvm_guest_phys_rw(v, gpa, &version, 4, 1) != 0) {
        return;
    }
    wall = wall_epoch(v->vm);
    wc.version = version;
    wc.sec = (uint32_t)(wall / NSEC_PER_SEC); /* modulo 2^32, as Linux's */
    wc.nsec = (uint32_t)(wall % NSEC_PER_SEC);
    if (kvm_guest_phys_rw(v, gpa, &wc, sizeof(wc), 1) != 0) {
        return;
    }
    version++;
    (void)kvm_guest_phys_rw(v, gpa, &version, 4, 1);
}

/**
 * Write `v`'s kvmclock into guest memory, as Linux's kvm_guest_time_update
 * and kvm_setup_guest_pvclock: the host's TSC and clock at the master
 * clock's moment, or now without it, as the guest's TSC and its kvmclock,
 * the TSC's rate, and PVCLOCK_TSC_STABLE_BIT with the master clock; the
 * version odd while the rest is written and even after, and the guest's
 * PVCLOCK_GUEST_STOPPED kept. A clock that crosses a page, or misses guest
 * memory, is not written, as Linux's cache of the page refuses it.
 */
static void pvclock_write(struct kvm_vcpu *v)
{
    struct pvclock_vcpu_time_info pv, guest;
    struct master m;
    uint64_t gpa = v->pv_time & ~(uint64_t)KVM_MSR_ENABLED, khz = kvm_tsc_khz();
    uint64_t host_tsc, ns;
    uint32_t version;

    if (khz == 0 || (gpa & 0xFFFu) + sizeof(pv) > 4096u) {
        return;
    }
    master_copy(v->vm, &m);
    if (m.on) {
        host_tsc = m.tsc;
        ns = m.ns;
    } else if (!vclock_read_cycles(VCLOCK_READ_RAW, &ns, &host_tsc)) {
        host_tsc = arch_cycles();
        ns = kvm_host_ns();
    }
    if (kvm_guest_phys_rw(v, gpa, &guest, sizeof(guest), 0) != 0) {
        return;
    }
    k_memset(&pv, 0, sizeof(pv));
    pv.tsc_timestamp = kvm_tsc_scale(v, host_tsc) + (uint64_t)v->tsc_offset;
    pv.system_time = ns + (uint64_t)m.offset;
    time_scale(NSEC_PER_SEC, kvm_tsc_hz(v), &pv.tsc_shift, &pv.tsc_to_system_mul);
    pv.flags = m.on ? PVCLOCK_TSC_STABLE_BIT : 0u;
    pv.flags |= (uint8_t)(guest.flags & PVCLOCK_GUEST_STOPPED);
    if (v->pv_stopped) {
        pv.flags |= PVCLOCK_GUEST_STOPPED;
        v->pv_stopped = 0;
    }
    version = (guest.version + 1u) | 1u;
    if (kvm_guest_phys_rw(v, gpa, &version, 4, 1) != 0) {
        return;
    }
    pv.version = version;
    if (kvm_guest_phys_rw(v, gpa, &pv, sizeof(pv), 1) != 0) {
        return;
    }
    version++;
    (void)kvm_guest_phys_rw(v, gpa, &version, 4, 1);
    v->clock_cpu = (int32_t)arch_cpu_id();
    v->clock_ns = kvm_host_ns();
    kvm_timer_arm(v, kvm_clock_next(v));
}

/** Move `v`'s TSC up to where its rate would have it since its last match
 * or move, when the host's has fallen behind that, as Linux's
 * kvm_guest_time_update does under tsc_catchup. Whether it moved. */
static int tsc_catch_up(struct kvm_vcpu *v)
{
    uint64_t want = v->tsc_this_write + ns_to_cycles(kvm_host_ns() - v->tsc_this_ns, v->tsc_khz);
    uint64_t have = kvm_tsc_read(v);

    if (want <= have) {
        return 0;
    }
    v->tsc_offset += (int64_t)(want - have);
    v->dirty |= KVM_PART_TSC;
    return 1;
}

uint64_t kvm_clock_next(const struct kvm_vcpu *v)
{
    if (!(v->pv_time & KVM_MSR_ENABLED) || v->vm->master_clock || v->clock_cpu < 0) {
        return 0;
    }
    return v->clock_ns + KVMCLOCK_SYNC_NS;
}

void kvm_clock_moved_cpu(struct kvm_vcpu *v)
{
    if ((v->pv_time & KVM_MSR_ENABLED) && !v->vm->master_clock) {
        kvm_vcpu_kick(v, KVM_REQ_CLOCK_UPDATE);
    }
}

void kvm_clock_prepare(struct kvm_vcpu *v)
{
    int update, pv = (v->pv_time & KVM_MSR_ENABLED) != 0;

    (void)kvm_vcpu_test_clear(v, KVM_REQ_MASTERCLOCK_UPDATE);
    master_recheck(v->vm);
    update = kvm_vcpu_test_clear(v, KVM_REQ_CLOCK_UPDATE);
    if (v->tsc_catchup && tsc_catch_up(v)) {
        update = 1;
    }
    if (pv && !update && !v->vm->master_clock &&
        (v->clock_cpu != (int32_t)arch_cpu_id() ||
         kvm_host_ns() - v->clock_ns >= KVMCLOCK_SYNC_NS)) {
        update = 1;
    }
    if (!update) {
        return;
    }
    if (pv) {
        pvclock_write(v);
    }
    kvm_hv_clock_update(v);
}

/** A write of MSR_KVM_SYSTEM_TIME, `old`, or MSR_KVM_SYSTEM_TIME_NEW, as
 * Linux's kvm_write_system_time: the boot vCPU's guest choosing the old MSR
 * stops the master clock, since that MSR's guests read no stable bit. */
static void system_time_write(struct kvm_vcpu *v, uint64_t value, int old, int host)
{
    struct kvm_vm *vm = v->vm;
    uint32_t flags = clock_lock(vm);

    if (v->id == 0 && !host && vm->old_kvmclock != old) {
        vm->old_kvmclock = old;
        master_update(vm);
    }
    clock_unlock(vm, flags);
    v->pv_time = value;
    kvm_vcpu_kick(v, KVM_REQ_CLOCK_UPDATE);
}

int kvm_pv_msr(struct kvm_vcpu *v, uint32_t index, int write, uint64_t *value, int host)
{
    uint32_t flags;
    int bit;

    switch (index) {
    case MSR_KVM_WALL_CLOCK:
    case MSR_KVM_SYSTEM_TIME:
        bit = KVM_FEATURE_CLOCKSOURCE;
        break;
    case MSR_KVM_WALL_CLOCK_NEW:
    case MSR_KVM_SYSTEM_TIME_NEW:
        bit = KVM_FEATURE_CLOCKSOURCE2;
        break;
    case MSR_KVM_STEAL_TIME:
        bit = KVM_FEATURE_STEAL_TIME;
        break;
    case MSR_KVM_POLL_CONTROL:
        bit = KVM_FEATURE_POLL_CONTROL;
        break;
    default:
        return 1;
    }
    if (!kvm_pv_has(v, bit)) {
        /* The caller's access to an MSR KVM lists, which Linux answers
         * with 0 for a read and takes for a write of 0. */
        if (host && (!write || *value == 0)) {
            if (!write) {
                *value = 0;
            }
            return 0;
        }
        /* Linux's KVM_MSR_RET_UNSUPPORTED, which a caller's MSR exit
         * reports as KVM_MSR_EXIT_REASON_UNKNOWN. */
        return KVM_MSR_UNKNOWN;
    }
    if (!write) {
        if (index == MSR_KVM_POLL_CONTROL) {
            *value = v->poll_control;
        } else if (index == MSR_KVM_STEAL_TIME) {
            *value = v->st_msr;
        } else if (index == MSR_KVM_WALL_CLOCK || index == MSR_KVM_WALL_CLOCK_NEW) {
            flags = clock_lock(v->vm);
            *value = v->vm->wall_clock;
            clock_unlock(v->vm, flags);
        } else {
            *value = v->pv_time;
        }
        return 0;
    }
    switch (index) {
    case MSR_KVM_POLL_CONTROL:
        /* Bit 0 alone, whether the host may poll a halted vCPU; this
         * kernel never polls, so either value holds. */
        if (*value & ~1ull) {
            return -1;
        }
        v->poll_control = *value;
        return 0;
    case MSR_KVM_STEAL_TIME:
        if (*value & KVM_STEAL_RESERVED_MASK) {
            return -1;
        }
        v->st_msr = *value;
        v->st_ok = 0; /* the address `steal_vouch` checked may be another */
        if (*value & KVM_MSR_ENABLED) {
            kvm_vcpu_kick(v, KVM_REQ_STEAL_UPDATE);
        }
        return 0;
    case MSR_KVM_WALL_CLOCK:
    case MSR_KVM_WALL_CLOCK_NEW:
        flags = clock_lock(v->vm);
        v->vm->wall_clock = *value;
        clock_unlock(v->vm, flags);
        wall_clock_write(v, *value);
        return 0;
    default:
        system_time_write(v, *value, index == MSR_KVM_SYSTEM_TIME, host);
        return 0;
    }
}

/* The offsets of kvm_steal_time's fields that the host writes. */
#define ST_VERSION   8u
#define ST_PREEMPTED 16u

/**
 * Write `v`'s steal time, as Linux's record_steal_time without
 * KVM_FEATURE_PV_TLB_FLUSH: `preempted` cleared, the version odd, the
 * thread's run_delay since the last write added to `steal`, the version
 * even. A version the guest left odd is made even first. A record that
 * misses guest memory, or lies in a read-only slot, is not written. The
 * structure is 64 bytes at a 64-byte boundary, so within one page. 0 when
 * the record was written whole.
 */
static int steal_write(struct kvm_vcpu *v)
{
    uint64_t gpa = v->st_msr & KVM_STEAL_VALID_BITS, steal, delay = current_proc->run_delay;
    uint32_t version;
    uint8_t zero = 0;

    if (kvm_guest_phys_rw(v, gpa + ST_PREEMPTED, &zero, 1, 1) != 0) {
        return -1;
    }
    v->st_preempted = 0;
    if (kvm_guest_phys_rw(v, gpa + ST_VERSION, &version, 4, 0) != 0) {
        return -1;
    }
    if (version & 1u) {
        version++; /* the first write, over what the guest left there */
    }
    version++;
    if (kvm_guest_phys_rw(v, gpa + ST_VERSION, &version, 4, 1) != 0 ||
        kvm_guest_phys_rw(v, gpa, &steal, 8, 0) != 0) {
        return -1;
    }
    steal += delay - v->st_last;
    v->st_last = delay;
    if (kvm_guest_phys_rw(v, gpa, &steal, 8, 1) != 0) {
        return -1;
    }
    version++;
    return kvm_guest_phys_rw(v, gpa + ST_VERSION, &version, 4, 1) != 0 ? -1 : 0;
}

/** Find whether `v`'s steal time record at `gpa` lies in no region of
 * memfd_secret, with `mmu_seq` read first, so that `kvm_steal_preempted`
 * may write it while no zap has come since and the MSR names `gpa` still
 * (`st_ok`, `st_seq`, `st_gpa`). In `v`'s thread; may sleep. */
static void steal_vouch(struct kvm_vcpu *v, uint64_t gpa)
{
    struct kvm_vm *vm = v->vm;
    struct mm *mm = kvm_vm_mm(vm);
    struct kvm_slot_view s;
    const vma_t *r;
    uint32_t seq = vm->mmu_seq;

    k_barrier();
    v->st_ok = 0;
    if (!mm || !kvm_slot_find(vm, gpa >> 12, &s)) {
        return;
    }
    mmap_read_lock(mm);
    r = vm_find(&mm->vm, (uintptr_t)s.hva);
    v->st_gpa = gpa;
    v->st_seq = seq;
    v->st_ok = r && !(r->prot & VM_PROT_SECRET);
    mmap_unlock(mm);
}

void kvm_steal_prepare(struct kvm_vcpu *v)
{
    int update = kvm_vcpu_test_clear(v, KVM_REQ_STEAL_UPDATE);
    uint64_t gpa;

    if (current_proc->pcount != v->st_pcount) {
        v->st_pcount = current_proc->pcount;
        update = 1;
    }
    if ((update || v->st_preempted) && (v->st_msr & KVM_MSR_ENABLED)) {
        gpa = v->st_msr & KVM_STEAL_VALID_BITS;
        if (steal_write(v) != 0) {
            v->st_ok = 0;
        } else if (!v->st_ok || v->st_seq != v->vm->mmu_seq || v->st_gpa != gpa) {
            steal_vouch(v, gpa);
        }
    }
}

void kvm_steal_preempted(struct kvm_vcpu *v)
{
    uint8_t preempted = KVM_VCPU_PREEMPTED;
    uint64_t gpa = v->st_msr & KVM_STEAL_VALID_BITS;

    if (!v->at_boundary) {
        v->stat[KVM_VCPU_STAT_PREEMPTION_OTHER]++;
        return;
    }
    v->stat[KVM_VCPU_STAT_PREEMPTION_REPORTED]++;
    if (!(v->st_msr & KVM_MSR_ENABLED) || v->st_preempted || !v->st_ok || v->st_gpa != gpa) {
        return;
    }
    if (kvm_tdp_write_nofault(v->vm, gpa + ST_PREEMPTED, &preempted, 1, v->st_seq) == 0) {
        v->st_preempted = 1;
    }
}

int32_t kvm_clock_ctrl(struct kvm_vcpu *v)
{
    if (!(v->pv_time & KVM_MSR_ENABLED)) {
        return -EINVAL;
    }
    v->pv_stopped = 1;
    kvm_vcpu_kick(v, KVM_REQ_CLOCK_UPDATE);
    return 0;
}

uint64_t kvm_clock_pairing(struct kvm_vcpu *v, uint64_t gpa, uint64_t type)
{
    struct kvm_clock_pairing p;
    uint64_t real, tsc;

    if (type != KVM_CLOCK_PAIRING_WALLCLOCK || !vclock_read_cycles(VCLOCK_READ_REAL, &real, &tsc)) {
        return (uint64_t)-(int64_t)KVM_EOPNOTSUPP;
    }
    k_memset(&p, 0, sizeof(p));
    p.sec = (int64_t)(real / NSEC_PER_SEC);
    p.nsec = (int64_t)(real % NSEC_PER_SEC);
    p.tsc = kvm_tsc_scale(v, tsc) + (uint64_t)v->tsc_offset;
    if (kvm_guest_phys_rw(v, gpa, &p, sizeof(p), 1) != 0) {
        return (uint64_t)-(int64_t)KVM_EFAULT;
    }
    return 0;
}
