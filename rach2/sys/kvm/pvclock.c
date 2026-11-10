/*
 * pvclock.c: the kvmclock a guest reads, with its master clock and wall
 * clock, KVM_GET_CLOCK and KVM_SET_CLOCK, the matching of the vCPUs' TSCs,
 * and KVM's own MSRs, as Linux's arch/x86/kvm/x86.c keeps them.
 */

#include "kvm.h"

#include "../arch/arch.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../mm/usercopy.h"
#include "../rtc.h"
#include "../timer.h"
#include "../vdso.h"

#define NSEC_PER_SEC 1000000000ull

/* Without the master clock, a vCPU's kvmclock is written again at least
 * this often, Linux's KVMCLOCK_SYNC_PERIOD, so that its formula from the
 * TSC does not drift from the host's clock. */
#define KVMCLOCK_SYNC_NS (300ull * NSEC_PER_SEC)

uint64_t kvm_tsc_khz(void)
{
    return (uint64_t)timer_calibrate() * TICK_HZ / 1000u;
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

/** The kvmclock of `vm` at the host's TSC count `tsc`, by the master
 * clock's formula; a count before the master clock's moment, as another
 * processor's may read by a few counts, reads as that moment. */
static uint64_t master_at(const struct kvm_vm *vm, uint64_t tsc)
{
    uint64_t delta = tsc > vm->master_tsc ? tsc - vm->master_tsc : 0;

    return vm->master_ns + (uint64_t)vm->clock_offset +
           pv_scale(delta, vm->master_mul, vm->master_shift);
}

uint64_t kvm_clock_ns(const struct kvm_vm *vm)
{
    if (vm->master_clock) {
        return master_at(vm, arch_cycles());
    }
    return kvm_host_ns() + (uint64_t)vm->clock_offset;
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
 * the boot vCPU's guest uses the new MSR. Every vCPU first leaves the guest
 * with a request to write its kvmclock, which it cannot act on before the
 * caller lets the big lock go, so that none reads the old formula once
 * another reads the new.
 */
static void master_update(struct kvm_vm *vm)
{
    uint64_t ns, tsc, khz = kvm_tsc_khz();
    uint32_t i;
    int pair;

    for (i = 0; i < vm->nr_vcpus; i++) {
        kvm_vcpu_kick(vm->vcpus[i], KVM_REQ_CLOCK_UPDATE);
    }
    for (i = 0; i < vm->nr_vcpus; i++) {
        while (vm->vcpus[i]->mode == KVM_MODE_IN_GUEST) {
            arch_relax();
        }
    }
    pair = khz != 0 && vclock_read_cycles(0, &ns, &tsc);
    vm->master_clock = pair && tsc_all_matched(vm) && !vm->old_kvmclock;
    if (pair) {
        vm->master_ns = ns;
        vm->master_tsc = tsc;
        time_scale(NSEC_PER_SEC, khz * 1000u, &vm->master_shift, &vm->master_mul);
    }
    kvm_hv_clock_changed(vm);
}

void kvm_clock_vm_init(struct kvm_vm *vm)
{
    vm->clock_offset = -(int64_t)kvm_host_ns();
    master_update(vm);
}

int32_t kvm_clock_get(struct kvm_vm *vm, uintptr_t arg)
{
    struct kvm_clock_data d;
    uint64_t real;

    k_memset(&d, 0, sizeof(d));
    if (vm->master_clock) {
        if (vclock_read_cycles(1, &real, &d.host_tsc)) {
            d.realtime = real;
            d.flags = KVM_CLOCK_REALTIME | KVM_CLOCK_HOST_TSC;
        } else {
            d.host_tsc = arch_cycles();
        }
        d.flags |= KVM_CLOCK_TSC_STABLE;
        d.clock = master_at(vm, d.host_tsc);
    } else {
        d.clock = kvm_host_ns() + (uint64_t)vm->clock_offset;
    }
    return copy_to_user((void __user *)arg, &d, sizeof(d)) ? -EFAULT : 0;
}

int32_t kvm_clock_set(struct kvm_vm *vm, uintptr_t arg)
{
    struct kvm_clock_data d;
    uint64_t now, real;
    int64_t s, ns;

    if (copy_from_user(&d, (const void __user *)arg, sizeof(d)) != 0) {
        return -EFAULT;
    }
    if (d.flags & ~KVM_CLOCK_VALID_FLAGS) {
        return -EINVAL;
    }
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
    return 0;
}

void kvm_tsc_sync(struct kvm_vcpu *v, const uint64_t *value)
{
    struct kvm_vm *vm = v->vm;
    uint64_t data = value ? *value : 0, khz = kvm_tsc_khz(), ns = kvm_host_ns();
    uint64_t elapsed = ns - vm->tsc_last_ns, expect, hz;
    int64_t offset = (int64_t)(data - arch_cycles());
    int synchronizing = 0, matched = 0, use;

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
            offset = (int64_t)(data - arch_cycles());
        }
        matched = 1;
    }
    vm->tsc_last_ns = ns;
    vm->tsc_last_write = data;
    vm->tsc_last_khz = khz;
    v->tsc_offset = offset;
    v->dirty |= KVM_PART_TSC;
    if (!matched) {
        vm->tsc_gen++;
        vm->tsc_cur_offset = offset;
        vm->tsc_matched = 0;
    } else if (v->tsc_gen != vm->tsc_gen) {
        vm->tsc_matched++;
    }
    v->tsc_gen = vm->tsc_gen;
    /* As Linux's kvm_track_tsc_matching: a new generation takes a new
     * moment, after it, and so does the master clock turned on or off. */
    use = tsc_all_matched(vm) && vclock_running();
    if ((vm->master_clock && !matched) || vm->master_clock != use) {
        master_update(vm);
    }
    kvm_tsc_moved(v);
}

void kvm_tsc_moved(struct kvm_vcpu *v)
{
    kvm_vcpu_kick(v, KVM_REQ_CLOCK_UPDATE);
    kvm_hv_clock_changed(v->vm);
}

/** The real time at which `vm`'s kvmclock read 0, in ns, as Linux's
 * kvm_get_wall_clock_epoch: with the master clock, the real time and the
 * kvmclock at one TSC count. */
static uint64_t wall_epoch(const struct kvm_vm *vm)
{
    uint64_t real, tsc;
    int64_t s, ns;

    if (vm->master_clock && vclock_read_cycles(1, &real, &tsc)) {
        return real - master_at(vm, tsc);
    }
    rtc_realtime(&s, &ns);
    return (uint64_t)s * NSEC_PER_SEC + (uint64_t)ns - kvm_clock_ns(vm);
}

/** Write the wall clock at the guest's `gpa`, as Linux's
 * kvm_write_wall_clock: its version odd, the time, its version even. A
 * write that misses guest memory is dropped, as Linux drops it. */
static void wall_clock_write(struct kvm_vcpu *v, uint64_t gpa)
{
    struct pvclock_wall_clock wc;
    uint64_t wall;
    uint32_t version = 0;

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
    struct kvm_vm *vm = v->vm;
    struct pvclock_vcpu_time_info pv, guest;
    uint64_t gpa = v->pv_time & ~(uint64_t)KVM_MSR_ENABLED, khz = kvm_tsc_khz();
    uint64_t host_tsc, ns;
    uint32_t version;

    if (khz == 0 || (gpa & 0xFFFu) + sizeof(pv) > 4096u) {
        return;
    }
    if (vm->master_clock) {
        host_tsc = vm->master_tsc;
        ns = vm->master_ns;
    } else if (!vclock_read_cycles(0, &ns, &host_tsc)) {
        host_tsc = arch_cycles();
        ns = kvm_host_ns();
    }
    if (kvm_guest_phys_rw(v, gpa, &guest, sizeof(guest), 0) != 0) {
        return;
    }
    k_memset(&pv, 0, sizeof(pv));
    pv.tsc_timestamp = host_tsc + (uint64_t)v->tsc_offset;
    pv.system_time = ns + (uint64_t)vm->clock_offset;
    time_scale(NSEC_PER_SEC, khz * 1000u, &pv.tsc_shift, &pv.tsc_to_system_mul);
    pv.flags = vm->master_clock ? PVCLOCK_TSC_STABLE_BIT : 0u;
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
}

void kvm_clock_prepare(struct kvm_vcpu *v)
{
    int update = kvm_vcpu_test_clear(v, KVM_REQ_CLOCK_UPDATE);

    if (!(v->pv_time & KVM_MSR_ENABLED)) {
        return;
    }
    if (!update && !v->vm->master_clock &&
        (v->clock_cpu != (int32_t)arch_cpu_id() ||
         kvm_host_ns() - v->clock_ns >= KVMCLOCK_SYNC_NS)) {
        update = 1;
    }
    if (update) {
        pvclock_write(v);
    }
}

/** A write of MSR_KVM_SYSTEM_TIME, `old`, or MSR_KVM_SYSTEM_TIME_NEW, as
 * Linux's kvm_write_system_time: the boot vCPU's guest choosing the old MSR
 * stops the master clock, since that MSR's guests read no stable bit. */
static void system_time_write(struct kvm_vcpu *v, uint64_t value, int old, int host)
{
    struct kvm_vm *vm = v->vm;

    if (v->id == 0 && !host && vm->old_kvmclock != old) {
        vm->old_kvmclock = old;
        master_update(vm);
    }
    v->pv_time = value;
    kvm_vcpu_kick(v, KVM_REQ_CLOCK_UPDATE);
}

int kvm_pv_msr(struct kvm_vcpu *v, uint32_t index, int write, uint64_t *value, int host)
{
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
        return -1;
    }
    if (!write) {
        if (index == MSR_KVM_POLL_CONTROL) {
            *value = v->poll_control;
        } else if (index == MSR_KVM_WALL_CLOCK || index == MSR_KVM_WALL_CLOCK_NEW) {
            *value = v->vm->wall_clock;
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
    case MSR_KVM_WALL_CLOCK:
    case MSR_KVM_WALL_CLOCK_NEW:
        v->vm->wall_clock = *value;
        wall_clock_write(v, *value);
        return 0;
    default:
        system_time_write(v, *value, index == MSR_KVM_SYSTEM_TIME, host);
        return 0;
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

    if (type != KVM_CLOCK_PAIRING_WALLCLOCK || !vclock_read_cycles(1, &real, &tsc)) {
        return (uint64_t)-(int64_t)KVM_EOPNOTSUPP;
    }
    k_memset(&p, 0, sizeof(p));
    p.sec = (int64_t)(real / NSEC_PER_SEC);
    p.nsec = (int64_t)(real % NSEC_PER_SEC);
    p.tsc = tsc + (uint64_t)v->tsc_offset;
    if (kvm_guest_phys_rw(v, gpa, &p, sizeof(p), 1) != 0) {
        return (uint64_t)-(int64_t)KVM_EFAULT;
    }
    return 0;
}
