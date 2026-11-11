/* Guest performance counters, their event filters and hardware switching. */

#include "kvm.h"

#include "../arch/arch.h"
#include "../arch/x86/kcpu.h"
#include "../lib/kerrno.h"
#include "../lib/kirq.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"
#include "../mm/usercopy.h"

#define PMC0          0x0C1u
#define EVENTSEL0     0x186u
#define PERF_CAP      0x345u
#define FIXED0        0x309u
#define FIXED_CTRL    0x38Du
#define GLOBAL_STATUS 0x38Eu
#define GLOBAL_CTRL   0x38Fu
#define GLOBAL_OVF    0x390u
#define FULL_PMC0     0x4C1u
#define AMD_CTL0      0xC0010000u
#define AMD_CTR0      0xC0010004u
#define AMD_CORE_CTL0 0xC0010200u
#define AMD_CORE_CTR0 0xC0010201u
#define AMD_STATUS    0xC0000300u
#define AMD_GLOBAL    0xC0000301u
#define AMD_CLEAR     0xC0000302u

#define EV_USR         (1ull << 16)
#define EV_OS          (1ull << 17)
#define EV_EDGE        (1ull << 18)
#define EV_PIN         (1ull << 19)
#define EV_INT         (1ull << 20)
#define EV_ENABLE      (1ull << 22)
#define EV_INVERT      (1ull << 23)
#define EV_CMASK       (0xFFull << 24)
#define EV_GUEST       (1ull << 40)
#define EV_TX          (1ull << 32)
#define EV_TXCP        (1ull << 33)
#define FW_WRITES      (1ull << 13)
#define PDCM           (1u << 15)
#define CORE_EXT       (1u << 23)
#define AMD_EVENT      0xF000000FFull
#define FILTER_EXCLUDE (1ull << 55)
#define FILTER_MASK    (0xFFull << 56)
#define FILTER_MAX     300u

/* The backend's physical counters. Linux caps the architectural PMU at
 * version 2 and the fixed counters at three; AMD has at most six. */
struct pmu_host {
    int intel, core_ext, pdcm;
    uint32_t version, nr_gp, nr_fixed, width, fixed_width;
    uint32_t event_length, unavailable;
};
static struct pmu_host pmu_host;

/* Header values are copied once, then the event words, so a caller cannot
 * change the allocation's bound between the two copies. */
struct pmu_filter {
    struct kvm_pmu_event_filter head;
    uint64_t event[FILTER_MAX];
};

/** The low n bits, including the empty and full-width masks. */
static uint64_t mask_bits(uint32_t n)
{
    return n >= 64u ? ~0ull : ((1ull << n) - 1u);
}

/** The smaller of two unsigned values. */
static uint32_t minimum(uint32_t a, uint32_t b)
{
    return a < b ? a : b;
}

/** Read a physical PMU MSR, already qualified by the host's model. */
static uint64_t pmu_read(uint32_t index)
{
    uint32_t lo, hi;

    kcpu_rdmsr(index, &lo, &hi);
    return (uint64_t)lo | ((uint64_t)hi << 32);
}

/** Write a physical PMU MSR, with interrupts off and no host PMU user. */
static void pmu_write(uint32_t index, uint64_t value)
{
    kcpu_wrmsr(index, (uint32_t)value, (uint32_t)(value >> 32));
}

/** AMD's physical selector/counter index, using the extended aliases
 * when the host offers them. */
static uint32_t amd_index(uint32_t i, int counter)
{
    if (pmu_host.core_ext) {
        return AMD_CORE_CTL0 + 2u * i + (counter ? 1u : 0u);
    }
    return (counter ? AMD_CTR0 : AMD_CTL0) + i;
}

/** Test an AMD counter without risking a #GP when an outer hypervisor
 * disables its PMU. TCG accepts the MSRs but ignores counter writes. */
static int amd_probe(void)
{
    uint64_t control, counter, value = 0;
    uint32_t flags = kirq_save();
    int result = 0;

    if (kcpu_rdmsr_safe(amd_index(0, 0), &control) != 0 ||
        kcpu_rdmsr_safe(amd_index(0, 1), &counter) != 0 ||
        kcpu_wrmsr_safe(amd_index(0, 0), 0) != 0) {
        goto out;
    }
    if (kcpu_wrmsr_safe(amd_index(0, 1), counter ^ 1u) == 0) {
        result = kcpu_rdmsr_safe(amd_index(0, 1), &value) == 0 && value == (counter ^ 1u);
        (void)kcpu_wrmsr_safe(amd_index(0, 1), counter);
    }
    (void)kcpu_wrmsr_safe(amd_index(0, 0), control);
out:
    kirq_restore(flags);
    return result;
}

void kvm_pmu_start(void)
{
    uint32_t a, b, c, d, top;

    k_memset(&pmu_host, 0, sizeof(pmu_host));
    if (!kvm_backend) {
        return;
    }
    pmu_host.intel = kvm_backend == &kvm_vmx;
    if (pmu_host.intel) {
        kcpu_id(0, &a, &b, &c, &d);
        if (a < 0xAu) {
            return;
        }
        kcpu_id(0xA, &a, &b, &c, &d);
        if (!(a & 0xFFu) || !((a >> 8) & 0xFFu)) {
            return;
        }
        pmu_host.version = minimum(a & 0xFFu, 2u);
        pmu_host.nr_gp = minimum((a >> 8) & 0xFFu, KVM_PMU_GP_MAX);
        pmu_host.width = minimum((a >> 16) & 0xFFu, 64u);
        pmu_host.event_length = minimum(a >> 24, 7u);
        pmu_host.unavailable = b & (uint32_t)mask_bits(pmu_host.event_length);
        if (pmu_host.version > 1u) {
            pmu_host.nr_fixed = minimum(d & 0x1Fu, KVM_PMU_FIXED_MAX);
            pmu_host.fixed_width = minimum((d >> 5) & 0xFFu, 64u);
        }
        kcpu_id(1, &a, &b, &c, &d);
        pmu_host.pdcm = (c & PDCM) != 0;
    } else {
        kcpu_id(0x80000000u, &top, &b, &c, &d);
        kcpu_id(0x80000001u, &a, &b, &c, &d);
        pmu_host.version = 1;
        pmu_host.core_ext = (c & CORE_EXT) != 0;
        pmu_host.nr_gp = pmu_host.core_ext ? 6u : 4u;
        pmu_host.width = 48;
        if (top >= 0x80000022u) {
            kcpu_id(0x80000022u, &a, &b, &c, &d);
            if (a & 1u) {
                pmu_host.version = 2;
                pmu_host.nr_gp = minimum(b & 0xFu, 6u);
            }
        }
        if (!amd_probe()) {
            pmu_host.version = pmu_host.nr_gp = 0;
        }
    }
    if (!pmu_host.width) {
        pmu_host.version = pmu_host.nr_gp = pmu_host.nr_fixed = 0;
    }
}

int kvm_pmu_enabled(void)
{
    return pmu_host.version != 0;
}

void kvm_pmu_cpuid(struct kvm_cpuid_entry2 *e)
{
    int enabled = kvm_pmu_enabled();

    switch (e->function) {
    case 1:
        e->ecx &= ~PDCM;
        if (enabled && pmu_host.pdcm) {
            e->ecx |= PDCM;
        }
        break;
    case 0xA:
        e->eax = e->ebx = e->ecx = e->edx = 0;
        if (enabled && pmu_host.intel) {
            e->eax = pmu_host.version | (pmu_host.nr_gp << 8) | (pmu_host.width << 16) |
                     (pmu_host.event_length << 24);
            e->ebx = pmu_host.unavailable;
            e->edx = pmu_host.nr_fixed | (pmu_host.fixed_width << 5);
        }
        break;
    case 0x80000001u:
        e->ecx &= ~CORE_EXT;
        if (enabled && pmu_host.core_ext) {
            e->ecx |= CORE_EXT;
        }
        break;
    case 0x80000022u:
        e->eax = e->ebx = e->ecx = e->edx = 0;
        if (enabled && !pmu_host.intel && pmu_host.version > 1u) {
            e->eax = 1;
            e->ebx = pmu_host.nr_gp;
        }
        break;
    default:
        break;
    }
}

/** The guest's valid GLOBAL_CTRL bits. */
static uint64_t global_mask(const struct kvm_pmu *p)
{
    return mask_bits(p->nr_gp) | (mask_bits(p->nr_fixed) << 32);
}

/** GLOBAL_STATUS and OVF_CTRL additionally have Intel's buffer overflow
 * and condition-changed flags, as Linux's global_status_rsvd allows. */
static uint64_t status_mask(const struct kvm_pmu *p)
{
    return global_mask(p) | (pmu_host.intel ? (3ull << 62) : 0);
}

void kvm_pmu_reset(struct kvm_vcpu *v)
{
    struct kvm_pmu *p = &v->pmu;

    k_memset(p->counter, 0, sizeof(p->counter));
    k_memset(p->eventsel, 0, sizeof(p->eventsel));
    k_memset(p->fixed, 0, sizeof(p->fixed));
    p->fixed_ctrl = p->global_status = 0;
    p->global_ctrl = 0;
    p->run_gp = p->run_fixed = 0;
    p->pmi = 0;
}

void kvm_pmu_refresh(struct kvm_vcpu *v)
{
    struct kvm_pmu *p = &v->pmu;
    const struct kvm_cpuid_entry2 *e;

    p->version = p->nr_gp = p->nr_fixed = 0;
    p->gp_mask = p->fixed_mask = 0;
    p->events = 0;
    p->core_ext = 0;
    if (!kvm_pmu_enabled() || v->vm->pmu_disabled) {
        kvm_pmu_reset(v);
        return;
    }
    if (pmu_host.intel) {
        e = kvm_cpuid_find(v, 0xA, 0);
        if (e && (e->eax & 0xFFu)) {
            p->version = minimum(e->eax & 0xFFu, pmu_host.version);
            p->nr_gp = minimum((e->eax >> 8) & 0xFFu, pmu_host.nr_gp);
            p->gp_mask = mask_bits(minimum((e->eax >> 16) & 0xFFu, pmu_host.width));
            p->events = ~e->ebx & (uint32_t)mask_bits(minimum(e->eax >> 24, pmu_host.event_length));
            if (p->version > 1u) {
                p->nr_fixed = minimum(e->edx & 0x1Fu, pmu_host.nr_fixed);
                p->fixed_mask = mask_bits(minimum((e->edx >> 5) & 0xFFu, pmu_host.fixed_width));
            }
        }
    } else {
        p->version = 1;
        e = kvm_cpuid_find(v, 0x80000001u, 0);
        p->core_ext = pmu_host.core_ext && e && (e->ecx & CORE_EXT);
        p->nr_gp = p->core_ext ? 6u : 4u;
        e = kvm_cpuid_find(v, 0x80000022u, 0);
        if (e && (e->eax & 1u) && pmu_host.version > 1u) {
            p->version = 2;
            p->nr_gp = e->ebx & 0xFu;
        }
        p->nr_gp = minimum(p->nr_gp, pmu_host.nr_gp);
        p->gp_mask = mask_bits(48);
    }
    kvm_pmu_reset(v);
    if (p->version > 1u) {
        p->global_ctrl = mask_bits(p->nr_gp);
    }
}

void kvm_pmu_init(struct kvm_vcpu *v)
{
    v->pmu.perf_cap = pmu_host.intel && kvm_pmu_enabled() ? FW_WRITES : 0;
    kvm_pmu_refresh(v);
}

uint32_t kvm_pmu_msr_list(uint32_t *out)
{
    uint32_t n = 0, i;

    if (!kvm_pmu_enabled()) {
        return 0;
    }
    for (i = 0; i < pmu_host.nr_gp; i++) {
        out[n++] = pmu_host.intel ? EVENTSEL0 + i : amd_index(i, 0);
        out[n++] = pmu_host.intel ? PMC0 + i : amd_index(i, 1);
    }
    if (pmu_host.intel) {
        out[n++] = PERF_CAP;
        for (i = 0; i < pmu_host.nr_fixed; i++) {
            out[n++] = FIXED0 + i;
        }
        if (pmu_host.version > 1u) {
            out[n++] = FIXED_CTRL;
        }
    }
    if (pmu_host.version > 1u) {
        out[n++] = pmu_host.intel ? GLOBAL_CTRL : AMD_GLOBAL;
        out[n++] = pmu_host.intel ? GLOBAL_STATUS : AMD_STATUS;
        out[n++] = pmu_host.intel ? GLOBAL_OVF : AMD_CLEAR;
    }
    return n;
}

int kvm_pmu_feature(uint32_t index, uint64_t *value)
{
    if (index != PERF_CAP) {
        return -1;
    }
    *value = pmu_host.intel && kvm_pmu_enabled() ? FW_WRITES : 0;
    return 0;
}

/** Whether the guest has PERF_CAPABILITIES through CPUID.PDCM. */
static int guest_pdcm(const struct kvm_vcpu *v)
{
    const struct kvm_cpuid_entry2 *e = kvm_cpuid_find(v, 1, 0);

    return e && (e->ecx & PDCM);
}

/** Find a general counter MSR or selector, returning its index or -1.
 * full identifies Intel's full-width aliases, ctl its event selectors. */
static int gp_index(struct kvm_vcpu *v, uint32_t msr, int *ctl, int *full)
{
    struct kvm_pmu *p = &v->pmu;
    uint32_t i;

    *full = *ctl = 0;
    if (!p->version) {
        return -1;
    }
    if (pmu_host.intel) {
        if (msr >= PMC0 && msr < PMC0 + p->nr_gp) {
            return (int)(msr - PMC0);
        }
        if (msr >= FULL_PMC0 && msr < FULL_PMC0 + p->nr_gp && guest_pdcm(v) &&
            (p->perf_cap & FW_WRITES)) {
            *full = 1;
            return (int)(msr - FULL_PMC0);
        }
        if (msr >= EVENTSEL0 && msr < EVENTSEL0 + p->nr_gp) {
            *ctl = 1;
            return (int)(msr - EVENTSEL0);
        }
    } else {
        if (msr >= AMD_CTL0 && msr < AMD_CTL0 + 4u) {
            *ctl = 1;
            i = msr - AMD_CTL0;
        } else if (msr >= AMD_CTR0 && msr < AMD_CTR0 + 4u) {
            i = msr - AMD_CTR0;
        } else if (p->core_ext && msr >= AMD_CORE_CTL0 && msr < AMD_CORE_CTL0 + 2u * p->nr_gp) {
            *ctl = !(msr & 1u);
            i = (msr - AMD_CORE_CTL0) / 2u;
        } else {
            return -1;
        }
        return i < p->nr_gp ? (int)i : -1;
    }
    return -1;
}

/** Intel's selector bits KVM accepts, as Linux's reserved_bits. TSX's
 * two flags require the guest's HLE/RTM and the checkpoint flag counter 2. */
static uint64_t intel_selector_mask(struct kvm_vcpu *v, int i)
{
    const struct kvm_cpuid_entry2 *e = kvm_cpuid_find(v, 7, 0);
    uint32_t a, b, c, d;
    uint64_t mask = 0xFFDFFFFFull;

    kcpu_id_sub(7, 0, &a, &b, &c, &d);
    if (e && (e->ebx & b & ((1u << 4) | (1u << 11)))) {
        mask |= EV_TX;
        if (i == 2) {
            mask |= EV_TXCP;
        }
    }
    return mask;
}

/** Read or write one counter, truncating to its architectural width. */
static void counter_io(uint64_t *counter, uint64_t mask, int write, uint64_t *value)
{
    if (write) {
        *counter = *value & mask;
    } else {
        *value = *counter & mask;
    }
}

/** Access PERF_CAPABILITIES, writable only by the caller before a run. */
static int capabilities_io(struct kvm_vcpu *v, int write, uint64_t *value, int host)
{
    struct kvm_pmu *p = &v->pmu;
    uint64_t mask = pmu_host.intel && kvm_pmu_enabled() ? FW_WRITES : 0;

    if (!host && (!guest_pdcm(v) || write)) {
        return -1;
    }
    if (!write) {
        *value = p->perf_cap;
    } else {
        if ((*value & ~mask) || (v->has_run && *value != p->perf_cap)) {
            return -1;
        }
        if (p->perf_cap != *value) {
            p->perf_cap = *value;
            kvm_pmu_refresh(v);
        }
    }
    return 0;
}

/** Access a general counter or selector; 1 when index is neither. */
static int general_io(struct kvm_vcpu *v, uint32_t index, int write, uint64_t *value, int host)
{
    struct kvm_pmu *p = &v->pmu;
    uint64_t data = *value;
    int ctl, full, i = gp_index(v, index, &ctl, &full);

    if (i < 0) {
        return 1;
    }
    if (ctl) {
        if (!write) {
            *value = p->eventsel[i];
            return 0;
        }
        if (pmu_host.intel) {
            if (data & ~intel_selector_mask(v, i)) {
                return -1;
            }
        } else {
            data &= ~0xFFFFFFF000280000ull;
        }
        p->eventsel[i] = data;
    } else {
        if (write && pmu_host.intel) {
            if (full && (data & ~p->gp_mask)) {
                return -1;
            }
            if (!host && !full) {
                data = (uint64_t)(int64_t)(int32_t)data;
            }
        }
        counter_io(&p->counter[i], p->gp_mask, write, &data);
        if (!write) {
            *value = data;
        }
    }
    return 0;
}

/** Access the version-two global registers; 1 for another MSR. */
static int global_io(struct kvm_pmu *p, uint32_t index, int write, uint64_t *value, int host)
{
    uint64_t mask;

    if (index == (pmu_host.intel ? GLOBAL_CTRL : AMD_GLOBAL)) {
        if (!write) {
            *value = p->global_ctrl;
        } else {
            mask = global_mask(p);
            if (pmu_host.intel && (*value & ~mask)) {
                return -1;
            }
            p->global_ctrl = *value & mask;
        }
        return 0;
    }
    if (index == (pmu_host.intel ? GLOBAL_STATUS : AMD_STATUS)) {
        if (!write) {
            *value = p->global_status;
        } else if (host) {
            if (*value & ~status_mask(p)) {
                return -1;
            }
            p->global_status = *value;
        } else if (pmu_host.intel) {
            return -1;
        }
        return 0;
    }
    if (index == (pmu_host.intel ? GLOBAL_OVF : AMD_CLEAR)) {
        if (!write) {
            *value = 0;
        } else {
            if (pmu_host.intel && (*value & ~status_mask(p))) {
                return -1;
            }
            if (!host) {
                p->global_status &= ~*value;
            }
        }
        return 0;
    }
    return 1;
}

/** Access Intel's fixed counters and their control; 1 for another MSR. */
static int fixed_io(struct kvm_pmu *p, uint32_t index, int write, uint64_t *value)
{
    uint64_t mask = 0;
    uint32_t i;

    if (index >= FIXED0 && index < FIXED0 + p->nr_fixed) {
        counter_io(&p->fixed[index - FIXED0], p->fixed_mask, write, value);
        return 0;
    }
    if (index != FIXED_CTRL || p->version < 2u) {
        return 1;
    }
    if (!write) {
        *value = p->fixed_ctrl;
    } else {
        for (i = 0; i < p->nr_fixed; i++) {
            mask |= 0xBull << (4u * i);
        }
        if (*value & ~mask) {
            return -1;
        }
        p->fixed_ctrl = *value;
    }
    return 0;
}

int kvm_pmu_msr(struct kvm_vcpu *v, uint32_t index, int write, uint64_t *value, int host)
{
    struct kvm_pmu *p = &v->pmu;
    int r;

    if (index == PERF_CAP) {
        return capabilities_io(v, write, value, host);
    }
    r = general_io(v, index, write, value, host);
    if (r != 1) {
        return r;
    }
    if (pmu_host.intel && p->version) {
        r = fixed_io(p, index, write, value);
        if (r != 1) {
            return r;
        }
    }
    if (p->version > 1u) {
        r = global_io(p, index, write, value, host);
        if (r != 1) {
            return r;
        }
    }
    /* Linux keeps the legacy P6 and K7 MSRs as zero without their PMU. */
    if (index == PMC0 || index == PMC0 + 1u || index == EVENTSEL0 || index == EVENTSEL0 + 1u ||
        (index >= AMD_CTL0 && index < AMD_CTR0 + 4u)) {
        if (!write) {
            *value = 0;
        }
        return 0;
    }
    return 1;
}

int kvm_pmu_rdpmc(struct kvm_vcpu *v)
{
    struct kvm_pmu *p = &v->pmu;
    uint32_t index = (uint32_t)v->regs.rcx, type = index & 0xFFFF0000u;
    uint64_t value;

    if (!p->version || ((v->sregs.cr0 & 1u) && !(v->sregs.cr4 & 0x100u) &&
                        ((v->regs.rflags & 0x20000u) || (v->sregs.cs.selector & 3u)))) {
        goto fault;
    }
    if (pmu_host.intel && type == (1u << 30)) {
        index &= 0xFFFFu;
        if (index >= p->nr_fixed) {
            goto fault;
        }
        value = p->fixed[index] & p->fixed_mask;
    } else {
        if ((pmu_host.intel && type) || index >= p->nr_gp) {
            goto fault;
        }
        value = p->counter[index] & p->gp_mask;
    }
    v->regs.rax = (uint32_t)value;
    v->regs.rdx = (uint32_t)(value >> 32);
    return 0;
fault:
    kvm_queue_exception(v, 13, 1, 0);
    return -1;
}

/** Whether one filter word matches the selector, including AMD's high
 * event nibble, under the legacy or masked ABI. */
static int filter_match(const struct pmu_filter *f, uint64_t word, uint64_t selector)
{
    uint64_t events = pmu_host.intel ? 0xFFu : AMD_EVENT;

    if (!(f->head.flags & KVM_PMU_EVENT_FLAG_MASKED_EVENTS)) {
        return word == (selector & (events | 0xFF00u));
    }
    return (word & events) == (selector & events) &&
           (selector & ((word >> 48) & 0xFF00u)) == (word & 0xFF00u);
}

/** Whether the VM's filter permits this general or fixed counter. */
static int allowed(struct kvm_vcpu *v, uint32_t i, int fixed)
{
    const struct pmu_filter *f = v->vm->pmu_filter;
    uint32_t n;
    int include = 0, exclude = 0, match;

    if (!f) {
        return 1;
    }
    if (fixed) {
        match = (f->head.fixed_counter_bitmap & (1u << i)) != 0;
    } else {
        for (n = 0; n < f->head.nevents; n++) {
            if (!filter_match(f, f->event[n], v->pmu.eventsel[i])) {
                continue;
            }
            if ((f->head.flags & KVM_PMU_EVENT_FLAG_MASKED_EVENTS) &&
                (f->event[n] & FILTER_EXCLUDE)) {
                exclude = 1;
            } else {
                include = 1;
            }
        }
        match = include && !exclude;
    }
    return f->head.action == KVM_PMU_EVENT_ALLOW ? match : !match;
}

int32_t kvm_pmu_filter(struct kvm_vm *vm, uintptr_t arg)
{
    struct kvm_pmu_event_filter head;
    struct pmu_filter *f, *old;
    uint64_t valid = (pmu_host.intel ? 0xFFu : AMD_EVENT) | 0xFF00u | FILTER_MASK | FILTER_EXCLUDE;
    uint32_t i;

    if (copy_from_user(&head, (const void __user *)arg, sizeof(head)) != 0) {
        return -EFAULT;
    }
    if (head.action > KVM_PMU_EVENT_DENY || (head.flags & ~KVM_PMU_EVENT_FLAG_MASKED_EVENTS)) {
        return -EINVAL;
    }
    if (head.nevents > FILTER_MAX) {
        return -E2BIG;
    }
    f = kmalloc(sizeof(*f));
    if (!f) {
        return -ENOMEM;
    }
    f->head = head;
    if (head.nevents && copy_from_user(f->event, (const void __user *)(arg + sizeof(head)),
                                       head.nevents * sizeof(uint64_t)) != 0) {
        kfree(f);
        return -EFAULT;
    }
    if (head.flags & KVM_PMU_EVENT_FLAG_MASKED_EVENTS) {
        for (i = 0; i < head.nevents; i++) {
            if (f->event[i] & ~valid) {
                kfree(f);
                return -EINVAL;
            }
        }
    }
    old = vm->pmu_filter;
    vm->pmu_filter = f;
    for (i = 0; i < vm->nr_vcpus; i++) {
        kvm_vcpu_kick(vm->vcpus[i], KVM_REQ_PMU);
    }
    kfree(old);
    return 0;
}

int32_t kvm_pmu_enable(struct kvm_vm *vm, const struct kvm_enable_cap *cap)
{
    if (cap->flags || (cap->args[0] & ~(uint64_t)KVM_PMU_CAP_DISABLE) || vm->nr_vcpus) {
        return -EINVAL;
    }
    vm->pmu_disabled = (cap->args[0] & KVM_PMU_CAP_DISABLE) != 0;
    return 0;
}

void kvm_pmu_vm_free(struct kvm_vm *vm)
{
    kfree(vm->pmu_filter);
    vm->pmu_filter = NULL;
}

/** Whether a general event is enabled by its selector and global bit. */
static int gp_enabled(const struct kvm_pmu *p, uint32_t i)
{
    return (p->eventsel[i] & EV_ENABLE) && (p->version < 2u || (p->global_ctrl & (1ull << i)));
}

/** Whether a fixed event is enabled by its control field and global bit. */
static int fixed_enabled(const struct kvm_pmu *p, uint32_t i)
{
    return (p->fixed_ctrl & (3ull << (4u * i))) && (p->global_ctrl & (1ull << (32u + i)));
}

void kvm_pmu_prepare(struct kvm_vcpu *v)
{
    struct kvm_pmu *p = &v->pmu;
    uint32_t i;

    (void)kvm_vcpu_test_clear(v, KVM_REQ_PMU);
    if (p->pmi) {
        p->pmi = 0;
        kvm_lapic_pmi(v);
    }
    p->run_gp = p->run_fixed = 0;
    for (i = 0; i < p->nr_gp; i++) {
        if (gp_enabled(p, i) && allowed(v, i, 0)) {
            p->run_gp |= 1u << i;
        }
    }
    for (i = 0; i < p->nr_fixed; i++) {
        if (fixed_enabled(p, i) && allowed(v, i, 1)) {
            p->run_fixed |= 1u << i;
        }
    }
}

/** Add a measured delta, recording a wrap even when its interrupt is
 * disabled. Physical counters start at zero and cannot wrap in one run. */
static void accumulate(struct kvm_pmu *p, uint32_t i, int fixed, uint64_t delta)
{
    uint64_t *counter = fixed ? &p->fixed[i] : &p->counter[i];
    uint64_t mask = fixed ? p->fixed_mask : p->gp_mask;
    uint64_t interrupt = fixed ? p->fixed_ctrl & (8ull << (4u * i)) : p->eventsel[i] & EV_INT;

    if (delta > mask - *counter) {
        p->global_status |= 1ull << (fixed ? 32u + i : i);
        if (interrupt) {
            p->pmi = 1;
        }
    }
    *counter = (*counter + delta) & mask;
}

void kvm_pmu_retired(struct kvm_vcpu *v)
{
    struct kvm_pmu *p = &v->pmu;
    uint64_t select, event_mask = pmu_host.intel ? 0xFFu : AMD_EVENT;
    uint64_t mode = ((v->regs.rflags & 0x20000u) || (v->sregs.cs.selector & 3u)) ? EV_USR : EV_OS;
    uint32_t i;

    for (i = 0; i < p->nr_gp; i++) {
        select = p->eventsel[i];
        if ((select & (event_mask | 0xFF00u)) == 0xC0u && !(select & (EV_TX | EV_TXCP)) &&
            (select & mode) && gp_enabled(p, i) && allowed(v, i, 0)) {
            accumulate(p, i, 0, 1);
        }
    }
    if (p->nr_fixed && fixed_enabled(p, 0) && allowed(v, 0, 1) &&
        (p->fixed_ctrl & (mode == EV_USR ? 2u : 1u))) {
        accumulate(p, 0, 1, 1);
    }
}

void kvm_pmu_svm_enter(struct kvm_vcpu *v)
{
    struct kvm_pmu *p = &v->pmu;
    uint32_t i;
    uint64_t select;

    if (pmu_host.intel || !p->version) {
        return;
    }
    /* Every counter is stopped and cleared before the first starts, so
     * that a started one counts none of the writes after it, each of which
     * is an exit when Rach2 itself runs under a hypervisor. */
    if (pmu_host.version > 1u) {
        pmu_write(AMD_GLOBAL, 0);
    }
    for (i = 0; i < pmu_host.nr_gp; i++) {
        pmu_write(amd_index(i, 0), 0);
        pmu_write(amd_index(i, 1), 0);
    }
    if (pmu_host.version > 1u) {
        pmu_write(AMD_CLEAR, mask_bits(pmu_host.nr_gp));
    }
    for (i = 0; i < pmu_host.nr_gp; i++) {
        if (p->run_gp & (1u << i)) {
            select = (p->eventsel[i] & ~(EV_INT | EV_PIN)) | EV_GUEST;
            pmu_write(amd_index(i, 0), select);
        }
    }
    if (pmu_host.version > 1u) {
        pmu_write(AMD_GLOBAL, p->run_gp);
    }
}

void kvm_pmu_svm_exit(struct kvm_vcpu *v)
{
    struct kvm_pmu *p = &v->pmu;
    uint32_t i;

    if (pmu_host.intel || !p->version) {
        return;
    }
    /* Every running counter stops before any is read, as at entry. */
    if (pmu_host.version > 1u) {
        pmu_write(AMD_GLOBAL, 0);
    }
    for (i = 0; i < pmu_host.nr_gp; i++) {
        if (p->run_gp & (1u << i)) {
            pmu_write(amd_index(i, 0), 0);
        }
    }
    for (i = 0; i < pmu_host.nr_gp; i++) {
        if (p->run_gp & (1u << i)) {
            accumulate(p, i, 0, pmu_read(amd_index(i, 1)) & mask_bits(pmu_host.width));
            pmu_write(amd_index(i, 1), 0);
        }
    }
}

/** Append one VMX MSR entry, including its required zero reserved word. */
static void list_add(struct kvm_pmu_msr *list, uint32_t *n, uint32_t index, uint64_t value)
{
    list[*n].index = index;
    list[*n].reserved = 0;
    list[*n].value = value;
    (*n)++;
}

void kvm_pmu_vmx_enter(struct kvm_vcpu *v, struct kvm_pmu_msr *guest, struct kvm_pmu_msr *host,
                       uint32_t *ng, uint32_t *nh)
{
    struct kvm_pmu *p = &v->pmu;
    uint64_t select, fixed = 0;
    uint32_t i;

    *ng = *nh = 0;
    if (!pmu_host.intel || !p->version) {
        return;
    }
    if (pmu_host.version > 1u) {
        list_add(host, nh, GLOBAL_CTRL, 0);
    }
    for (i = 0; i < pmu_host.nr_gp; i++) {
        list_add(guest, ng, PMC0 + i, 0);
    }
    for (i = 0; i < pmu_host.nr_fixed; i++) {
        list_add(guest, ng, FIXED0 + i, 0);
    }
    for (i = 0; i < pmu_host.nr_gp; i++) {
        select = (p->run_gp & (1u << i)) ? p->eventsel[i] & ~(EV_INT | EV_PIN) : 0;
        list_add(guest, ng, EVENTSEL0 + i, select);
        list_add(host, nh, EVENTSEL0 + i, 0);
    }
    if (pmu_host.version > 1u) {
        for (i = 0; i < p->nr_fixed; i++) {
            if (p->run_fixed & (1u << i)) {
                fixed |= p->fixed_ctrl & (3ull << (4u * i));
            }
        }
        list_add(guest, ng, FIXED_CTRL, fixed);
        list_add(host, nh, FIXED_CTRL, 0);
        list_add(guest, ng, GLOBAL_CTRL, (uint64_t)p->run_gp | ((uint64_t)p->run_fixed << 32));
    }
}

void kvm_pmu_vmx_exit(struct kvm_vcpu *v, const struct kvm_pmu_msr *guest)
{
    struct kvm_pmu *p = &v->pmu;
    uint32_t i;

    if (!pmu_host.intel || !p->version) {
        return;
    }
    for (i = 0; i < p->nr_gp; i++) {
        if (p->run_gp & (1u << i)) {
            accumulate(p, i, 0, guest[i].value & mask_bits(pmu_host.width));
        }
    }
    for (i = 0; i < p->nr_fixed; i++) {
        if (p->run_fixed & (1u << i)) {
            accumulate(p, i, 1, guest[pmu_host.nr_gp + i].value & mask_bits(pmu_host.fixed_width));
        }
    }
}
