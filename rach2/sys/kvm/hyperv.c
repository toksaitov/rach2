/*
 * hyperv.c: the Hyper-V enlightenments KVM gives a guest whose CPUID names
 * Hyper-V's interface, as Linux's arch/x86/kvm/hyperv.c: the synthetic
 * MSRs, the hypercalls, the reference TSC page, the synthetic interrupt
 * controller (SynIC) and its timers, and KVM_GET_SUPPORTED_HV_CPUID.
 */

#include "kvm.h"

#include "../arch/arch.h"
#include "../io/eventfd.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"
#include "../mm/usercopy.h"
#include "../proc/process.h"
#include "../timer.h"

/* The CPUID leaves, by the TLFS's names. */
#define HV_CPUID_VENDOR          0x40000000u
#define HV_CPUID_INTERFACE       0x40000001u
#define HV_CPUID_VERSION         0x40000002u
#define HV_CPUID_FEATURES        0x40000003u
#define HV_CPUID_ENLIGHTENMENTS  0x40000004u
#define HV_CPUID_LIMITS          0x40000005u
#define HV_CPUID_NESTED          0x4000000Au
#define HV_CPUID_SYNDBG_VENDOR   0x40000080u
#define HV_CPUID_SYNDBG_IFACE    0x40000081u
#define HV_CPUID_SYNDBG_PLATFORM 0x40000082u
#define HV_SIGNATURE_EAX         0x31237648u /* "Hv#1" */

/* HV_CPUID_FEATURES EAX: the MSRs the partition may use. */
#define HV_MSR_VP_RUNTIME_AVAILABLE     (1u << 0)
#define HV_MSR_TIME_REF_COUNT_AVAILABLE (1u << 1)
#define HV_MSR_SYNIC_AVAILABLE          (1u << 2)
#define HV_MSR_SYNTIMER_AVAILABLE       (1u << 3)
#define HV_MSR_APIC_ACCESS_AVAILABLE    (1u << 4)
#define HV_MSR_HYPERCALL_AVAILABLE      (1u << 5)
#define HV_MSR_VP_INDEX_AVAILABLE       (1u << 6)
#define HV_MSR_RESET_AVAILABLE          (1u << 7)
#define HV_MSR_REFERENCE_TSC_AVAILABLE  (1u << 9)
#define HV_ACCESS_FREQUENCY_MSRS        (1u << 11)
#define HV_ACCESS_REENLIGHTENMENT       (1u << 13)
#define HV_ACCESS_TSC_INVARIANT         (1u << 15)

/* HV_CPUID_FEATURES EBX: the hypercalls the partition may make. */
#define HV_POST_MESSAGES              (1u << 4)
#define HV_SIGNAL_EVENTS              (1u << 5)
#define HV_DEBUGGING                  (1u << 11)
#define HV_ENABLE_EXTENDED_HYPERCALLS (1u << 20)

/* HV_CPUID_FEATURES EDX: the other features. */
#define HV_GUEST_DEBUGGING_AVAILABLE    (1u << 1)
#define HV_HYPERCALL_XMM_INPUT          (1u << 4)
#define HV_FREQUENCY_MSRS_AVAILABLE     (1u << 8)
#define HV_GUEST_CRASH_MSR_AVAILABLE    (1u << 10)
#define HV_DEBUG_MSRS_AVAILABLE         (1u << 11)
#define HV_EXT_GVA_RANGES_FLUSH         (1u << 14)
#define HV_STIMER_DIRECT_MODE_AVAILABLE (1u << 19)

/* HV_CPUID_ENLIGHTENMENTS EAX: what the guest is advised to use. */
#define HV_REMOTE_TLB_FLUSH_RECOMMENDED   (1u << 2)
#define HV_APIC_ACCESS_RECOMMENDED        (1u << 3)
#define HV_RELAXED_TIMING_RECOMMENDED     (1u << 5)
#define HV_DEPRECATING_AEOI_RECOMMENDED   (1u << 9)
#define HV_CLUSTER_IPI_RECOMMENDED        (1u << 10)
#define HV_EX_PROCESSOR_MASKS_RECOMMENDED (1u << 11)

/* HV_CPUID_NESTED: EAX, then EBX. */
#define HV_NESTED_DIRECT_FLUSH           (1u << 17)
#define HV_NESTED_MSR_BITMAP             (1u << 19)
#define HV_NESTED_EVMCS1_PERF_GLOBAL_CTL (1u << 0)

/* HV_CPUID_SYNDBG_PLATFORM EAX. */
#define HV_SYNDBG_CAP_ALLOW_KERNEL_DEBUGGING (1u << 1)

/* The MSRs. */
#define HV_MSR_GUEST_OS_ID        0x40000000u
#define HV_MSR_HYPERCALL          0x40000001u
#define HV_MSR_VP_INDEX           0x40000002u
#define HV_MSR_RESET              0x40000003u
#define HV_MSR_VP_RUNTIME         0x40000010u
#define HV_MSR_TIME_REF_COUNT     0x40000020u
#define HV_MSR_REFERENCE_TSC      0x40000021u
#define HV_MSR_TSC_FREQUENCY      0x40000022u
#define HV_MSR_APIC_FREQUENCY     0x40000023u
#define HV_MSR_EOI                0x40000070u
#define HV_MSR_ICR                0x40000071u
#define HV_MSR_TPR                0x40000072u
#define HV_MSR_VP_ASSIST_PAGE     0x40000073u
#define HV_MSR_SCONTROL           0x40000080u
#define HV_MSR_SVERSION           0x40000081u
#define HV_MSR_SIEFP              0x40000082u
#define HV_MSR_SIMP               0x40000083u
#define HV_MSR_EOM                0x40000084u
#define HV_MSR_SINT0              0x40000090u
#define HV_MSR_SINT15             0x4000009Fu
#define HV_MSR_STIMER0_CONFIG     0x400000B0u
#define HV_MSR_STIMER3_COUNT      0x400000B7u
#define HV_MSR_SYNDBG_CONTROL     0x400000F1u
#define HV_MSR_SYNDBG_STATUS      0x400000F2u
#define HV_MSR_SYNDBG_SEND_BUFFER 0x400000F3u
#define HV_MSR_SYNDBG_RECV_BUFFER 0x400000F4u
#define HV_MSR_SYNDBG_PENDING     0x400000F5u
#define HV_MSR_SYNDBG_OPTIONS     0x400000FFu
#define HV_MSR_CRASH_P0           0x40000100u
#define HV_MSR_CRASH_P4           0x40000104u
#define HV_MSR_CRASH_CTL          0x40000105u
#define HV_MSR_REENLIGHTENMENT    0x40000106u
#define HV_MSR_TSC_EMULATION_CTL  0x40000107u
#define HV_MSR_TSC_EMULATION_STAT 0x40000108u
#define HV_MSR_TSC_INVARIANT_CTL  0x40000118u

/* The MSRs' bits. */
#define HV_HYPERCALL_ENABLE     1ull
#define HV_PAGE_ENABLE          1ull /* of the hypercall, TSC and assist pages */
#define HV_PAGE_MASK            (~0xFFFull)
#define HV_CRASH_CTL_NOTIFY     (1ull << 63)
#define HV_EXPOSE_INVARIANT_TSC 1ull
#define HV_SYNDBG_OPTION_HCALLS (1ull << 2)
#define HV_SYNIC_CONTROL_ENABLE 1ull
#define HV_SYNIC_VERSION_1      1ull
#define HV_SYNIC_SINT_MASKED    (1ull << 16)
#define HV_SYNIC_SINT_AUTO_EOI  (1ull << 17)
#define HV_SYNIC_FIRST_VECTOR   16u
#define HV_SYNIC_SINTS          16
#define HV_STIMERS              4

/* A synthetic timer's configuration MSR, union hv_stimer_config. */
#define STIMER_ENABLE      (1ull << 0)
#define STIMER_PERIODIC    (1ull << 1)
#define STIMER_AUTO_ENABLE (1ull << 3)
#define STIMER_DIRECT      (1ull << 12)
#define STIMER_VECTOR(c)   ((uint32_t)((c) >> 4) & 0xFFu)
#define STIMER_SINT(c)     ((uint32_t)((c) >> 16) & 0xFu)

/* The hypercalls, and the extended ones from 0x8001. */
#define HVCALL_FLUSH_SPACE         0x0002u
#define HVCALL_FLUSH_LIST          0x0003u
#define HVCALL_NOTIFY_LONG_SPIN    0x0008u
#define HVCALL_SEND_IPI            0x000Bu
#define HVCALL_FLUSH_SPACE_EX      0x0013u
#define HVCALL_FLUSH_LIST_EX       0x0014u
#define HVCALL_SEND_IPI_EX         0x0015u
#define HVCALL_POST_MESSAGE        0x005Cu
#define HVCALL_SIGNAL_EVENT        0x005Du
#define HVCALL_POST_DEBUG_DATA     0x0069u
#define HVCALL_RETRIEVE_DEBUG_DATA 0x006Au
#define HVCALL_RESET_DEBUG_SESSION 0x006Bu
#define HVCALL_EXT_QUERY           0x8001u
#define HVCALL_EXT_LAST            (0x8001u + 64u)

/* The hypercall's input value. */
#define HC_FAST          (1ull << 16)
#define HC_VARHEAD_SHIFT 17
#define HC_REP_SHIFT     32
#define HC_START_SHIFT   48
#define HC_RESERVED      0xF000F000F8000000ull /* bits 63:60, 47:44 and 31:27 */

/* The hypercall's status. */
#define HV_STATUS_SUCCESS           0u
#define HV_STATUS_INVALID_CODE      2u
#define HV_STATUS_INVALID_INPUT     3u
#define HV_STATUS_INVALID_ALIGNMENT 4u
#define HV_STATUS_INVALID_PARAMETER 5u
#define HV_STATUS_ACCESS_DENIED     6u
#define HV_STATUS_OPERATION_DENIED  8u
#define HV_STATUS_INVALID_PORT_ID   17u

/* The TLB flush's flags, a VP set's formats, and the bounds of both. */
#define HV_FLUSH_ALL_PROCESSORS 1ull
#define HV_SET_SPARSE_4K        0ull
#define HV_SET_ALL              1ull
#define HV_MAX_SPARSE_BANKS     64u
#define HV_XMM_REGISTERS        6u
#define HV_FLUSH_FIFO           16u /* Linux's KVM_HV_TLB_FLUSH_FIFO_SIZE */

/* The sparse banks that can hold a VP index below KVM_MAX_VCPUS, Linux's
 * KVM_HV_MAX_SPARSE_VCPU_SET_BITS. */
#define HV_BANKS_KEPT ((KVM_MAX_VCPUS + 63u) / 64u)

/* A message slot of the SynIC message page: its header and payload. */
#define HV_MSG_BYTES         256u
#define HV_MSG_TIMER_EXPIRED 0x80000010u
#define HV_MSG_FLAGS         5u  /* the header's message_flags byte */
#define HV_MSG_PENDING       1u  /* its msg_pending bit */
#define HV_TIMER_PAYLOAD     24u /* hv_timer_message_payload */

/* The state of the reference TSC page, Linux's enum hv_tsc_page_status. */
#define TSC_UNSET         0 /* not enabled */
#define TSC_GUEST_CHANGED 1 /* the guest wrote the MSR; set it up */
#define TSC_HOST_CHANGED  2 /* the caller wrote it, or the clock moved */
#define TSC_SET           3 /* the page holds the parameters */
#define TSC_BROKEN        4 /* they could not be written or computed */

/* The work a KVM_EXIT_HYPERV leaves to the next KVM_RUN. */
#define DONE_NONE   0
#define DONE_HCALL  1 /* the caller's result of a hypercall */
#define DONE_SYNDBG 2 /* the caller's status of the debugger */

/* An eventfd that HvSignalEvent of `conn_id` signals. */
struct hv_conn {
    uint32_t conn_id;
    struct eventfd *ev;
    struct hv_conn *next;
};

/* hyperv.c's share of a VM, the partition-wide state. */
struct hv_vm {
    uint64_t guest_os_id;
    uint64_t hypercall;
    uint64_t tsc_msr; /* HV_MSR_REFERENCE_TSC */
    int tsc_status;   /* TSC_* */
    uint64_t tsc_scale;
    int64_t tsc_offset;
    uint64_t crash_param[5];
    uint64_t crash_ctl;
    uint64_t reenlightenment;
    uint64_t tsc_emul_control;
    uint64_t tsc_emul_status;
    uint64_t invtsc_control;
    uint64_t syndbg_control, syndbg_status, syndbg_options;
    uint64_t syndbg_send, syndbg_recv, syndbg_pending;
    struct hv_conn *conns;
};

/* A synthetic timer. `exp_time` is in the reference counter's 100 ns, 0 for
 * none; `due_ns` is the host's ns at which it fires while armed, else 0. */
struct hv_stimer {
    uint64_t config;
    uint64_t count;
    uint64_t exp_time;
    uint64_t due_ns;
    int msg_pending; /* its message waits for the slot to empty */
};

/* hyperv.c's share of a vCPU, made at its first need. */
struct hv_vcpu {
    int enabled; /* the CPUID names Hyper-V, Linux's hyperv_enabled */
    int enforce; /* KVM_CAP_HYPERV_ENFORCE_CPUID */
    uint32_t features_eax, features_ebx, features_edx;
    uint32_t enl_eax, enl_ebx, syndbg_cap_eax, nested_eax, nested_ebx;
    uint32_t vp_index;
    uint64_t vapic; /* HV_MSR_VP_ASSIST_PAGE */
    int64_t runtime_offset;

    int synic_active;
    int dont_zero; /* KVM_CAP_HYPERV_SYNIC2: the pages are not cleared */
    uint64_t scontrol, sversion, siefp, simp;
    volatile uint64_t sint[HV_SYNIC_SINTS];
    uint32_t vec_bitmap[8];      /* the vectors of unmasked SINTs */
    uint32_t auto_eoi_bitmap[8]; /* those of them with auto-EOI */

    struct hv_stimer stimer[HV_STIMERS];
    uint32_t stimer_pending; /* a bit a timer, to look at before the entry */

    struct kvm_hyperv_exit exit; /* the exit KVM_REQ_HV_EXIT gives */
    int done;                    /* DONE_* */
    uint32_t insn_len;           /* the hypercall's, for DONE_HCALL */
};

/* A hypercall's input, Linux's struct kvm_hv_hcall. */
struct hcall {
    uint64_t param, ingpa, outgpa;
    uint32_t code, var_cnt, rep_cnt, rep_idx;
    int fast, rep;
    uint64_t xmm[HV_XMM_REGISTERS * 2]; /* the halves, low first */
    uint64_t data_offset;               /* the bytes of ingpa read */
    uint32_t xmm_used;                  /* the halves read */
};

/** hyperv.c's share of `v`, or NULL before its first need. */
static struct hv_vcpu *hv_of(const struct kvm_vcpu *v)
{
    return (struct hv_vcpu *)v->hv;
}

/** hyperv.c's share of `vm`, or NULL before its first need. */
static struct hv_vm *hvm_of(const struct kvm_vm *vm)
{
    return (struct hv_vm *)vm->hv;
}

/** The VM's share of `vm`, made when it is first needed: NULL for -ENOMEM. */
static struct hv_vm *hvm_get(struct kvm_vm *vm)
{
    struct hv_vm *h = hvm_of(vm);

    if (!h) {
        h = kzalloc(sizeof(*h));
        vm->hv = h;
    }
    return h;
}

/** Make `v`'s share and its VM's, as Linux's kvm_hv_vcpu_init: the SINTs
 * masked, version 1, and the VP index the vCPU's place. 0 or -ENOMEM. */
static int hv_init(struct kvm_vcpu *v)
{
    struct hv_vcpu *hv;
    int i;

    if (!hvm_get(v->vm)) {
        return -ENOMEM;
    }
    if (hv_of(v)) {
        return 0;
    }
    hv = kzalloc(sizeof(*hv));
    if (!hv) {
        return -ENOMEM;
    }
    hv->sversion = HV_SYNIC_VERSION_1;
    for (i = 0; i < HV_SYNIC_SINTS; i++) {
        hv->sint[i] = HV_SYNIC_SINT_MASKED;
    }
    hv->vp_index = v->index;
    k_barrier();
    v->hv = hv;
    return 0;
}

/** Whether `vector` is set in the bitmap `map` of 256 bits. */
static int vec_test(const uint32_t *map, uint32_t vector)
{
    return (map[vector >> 5] >> (vector & 31u)) & 1u;
}

/** Set `vector` in the bitmap `map` of 256 bits with `on`, else clear it. */
static void vec_put(uint32_t *map, uint32_t vector, int on)
{
    if (on) {
        map[vector >> 5] |= 1u << (vector & 31u);
    } else {
        map[vector >> 5] &= ~(1u << (vector & 31u));
    }
}

/* The clocks. */

/** The high 64 bits of the product of `a` and `b`, from 32-bit halves,
 * since C89 has no wider type. */
static uint64_t mul_hi(uint64_t a, uint64_t b)
{
    uint64_t a0 = a & 0xFFFFFFFFull, a1 = a >> 32;
    uint64_t b0 = b & 0xFFFFFFFFull, b1 = b >> 32;
    uint64_t p01 = a0 * b1, p10 = a1 * b0;
    uint64_t mid = ((a0 * b0) >> 32) + (p01 & 0xFFFFFFFFull) + (p10 & 0xFFFFFFFFull);

    return a1 * b1 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
}

/** The host's TSC in hertz, at the rate the kernel measured, or 0 when it
 * measured none. */
static uint64_t tsc_hz(void)
{
    return (uint64_t)timer_calibrate() * TICK_HZ;
}

/** The guest's TSC of `v` now. */
static uint64_t guest_tsc(const struct kvm_vcpu *v)
{
    return arch_cycles() + (uint64_t)v->tsc_offset;
}

/** The guest's kvmclock of `vm` in ns now. */
static uint64_t kvmclock_ns(const struct kvm_vm *vm)
{
    return kvm_host_ns() + (uint64_t)vm->clock_offset;
}

/** HV_MSR_TIME_REF_COUNT of `vm`, in 100 ns, as Linux's
 * get_time_ref_counter: by the TSC page's formula from the first vCPU's
 * TSC while the page is set, else the kvmclock's. */
static uint64_t ref_counter(const struct kvm_vm *vm)
{
    const struct hv_vm *h = hvm_of(vm);

    if (!h || h->tsc_status != TSC_SET || vm->nr_vcpus == 0) {
        return kvmclock_ns(vm) / 100u;
    }
    return mul_hi(guest_tsc(vm->vcpus[0]), h->tsc_scale) + (uint64_t)h->tsc_offset;
}

/** The reference TSC page's scale for a TSC of `hz`, 2^64 * 10^7 / hz by
 * long division: 0 when it would not fit, at or below 10 MHz. */
static uint64_t tsc_scale(uint64_t hz)
{
    uint64_t r = 10000000ull, q = 0;
    int i;

    if (hz <= r || hz >> 62) {
        return 0;
    }
    for (i = 0; i < 64; i++) {
        r <<= 1;
        q <<= 1;
        if (r >= hz) {
            r -= hz;
            q |= 1u;
        }
    }
    return q;
}

/**
 * Write the reference TSC page of `v`'s VM once the guest or the caller
 * changed it, as Linux's kvm_hv_setup_tsc_page: its sequence 0 first, so
 * that the guest reads the MSR meanwhile, then the scale and offset that
 * make the page's formula the kvmclock's in 100 ns, then the next sequence,
 * which is never 0 or 0xFFFFFFFF. A page the guest asked to keep while it
 * emulates the TSC is read back as it stands. In `v`'s thread.
 */
static void tsc_page_setup(struct kvm_vcpu *v)
{
    struct hv_vm *h = hvm_of(v->vm);
    uint8_t page[24];
    uint64_t gpa, hz, ns, tsc;
    uint32_t seq;

    if (!h || (h->tsc_status != TSC_GUEST_CHANGED && h->tsc_status != TSC_HOST_CHANGED)) {
        return;
    }
    if (!(h->tsc_msr & HV_PAGE_ENABLE)) {
        return;
    }
    gpa = h->tsc_msr & HV_PAGE_MASK;
    if (kvm_guest_phys_rw(v, gpa, &seq, 4, 0) != 0) {
        goto broken;
    }
    if (seq && h->tsc_status != TSC_GUEST_CHANGED && h->tsc_emul_control) {
        if (kvm_guest_phys_rw(v, gpa, page, sizeof(page), 0) != 0) {
            goto broken;
        }
        k_memcpy(&h->tsc_scale, page + 8, 8);
        k_memcpy(&h->tsc_offset, page + 16, 8);
        h->tsc_status = TSC_SET;
        return;
    }
    k_memset(page, 0, sizeof(page));
    if (kvm_guest_phys_rw(v, gpa, page, 4, 1) != 0) {
        goto broken;
    }
    hz = tsc_hz();
    h->tsc_scale = tsc_scale(hz);
    if (h->tsc_scale == 0 || v->vm->nr_vcpus == 0) {
        goto broken;
    }
    tsc = guest_tsc(v->vm->vcpus[0]);
    ns = kvmclock_ns(v->vm);
    h->tsc_offset = (int64_t)(ns / 100u - mul_hi(tsc, h->tsc_scale));
    k_memcpy(page + 8, &h->tsc_scale, 8);
    k_memcpy(page + 16, &h->tsc_offset, 8);
    if (kvm_guest_phys_rw(v, gpa, page, sizeof(page), 1) != 0) {
        goto broken;
    }
    seq++;
    if (seq == 0xFFFFFFFFu || seq == 0) {
        seq = 1;
    }
    k_barrier();
    if (kvm_guest_phys_rw(v, gpa, &seq, 4, 1) != 0) {
        goto broken;
    }
    h->tsc_status = TSC_SET;
    return;
broken:
    h->tsc_status = TSC_BROKEN;
}

void kvm_hv_clock_changed(struct kvm_vm *vm)
{
    struct hv_vm *h = hvm_of(vm);

    if (h && h->tsc_status == TSC_SET && !h->tsc_emul_control) {
        h->tsc_status = TSC_HOST_CHANGED;
    }
}

/* The SynIC. */

/** The vector of SINT value `s`, or -1 while it is masked. */
static int sint_vector(uint64_t s)
{
    return (s & HV_SYNIC_SINT_MASKED) ? -1 : (int)(s & 0xFFu);
}

/** Bring the bitmaps' bit of `vector` in step with the SINTs, as Linux's
 * synic_update_vector. No APICv is offered, so auto-EOI inhibits nothing. */
static void synic_update_vector(struct hv_vcpu *hv, uint32_t vector)
{
    int i, used = 0, auto_eoi = 0;

    if (vector < HV_SYNIC_FIRST_VECTOR) {
        return;
    }
    for (i = 0; i < HV_SYNIC_SINTS; i++) {
        if (sint_vector(hv->sint[i]) == (int)vector) {
            used = 1;
            if (hv->sint[i] & HV_SYNIC_SINT_AUTO_EOI) {
                auto_eoi = 1;
            }
        }
    }
    vec_put(hv->vec_bitmap, vector, used);
    vec_put(hv->auto_eoi_bitmap, vector, auto_eoi);
}

/** Write SINT `n`, as Linux's synic_set_sint: a vector below 16 unmasked
 * is #GP from the guest. 0, or -1. */
static int synic_set_sint(struct hv_vcpu *hv, int n, uint64_t value, int host)
{
    uint32_t vector = (uint32_t)(value & 0xFFu), old;

    if (vector < HV_SYNIC_FIRST_VECTOR && !host && !(value & HV_SYNIC_SINT_MASKED)) {
        return -1;
    }
    old = (uint32_t)(hv->sint[n] & 0xFFu);
    hv->sint[n] = value;
    synic_update_vector(hv, old);
    synic_update_vector(hv, vector);
    return 0;
}

/** The vCPU of `vm` whose VP index is `vpidx`, or NULL. */
static struct kvm_vcpu *vcpu_by_vpidx(struct kvm_vm *vm, uint32_t vpidx)
{
    struct kvm_vcpu *v;
    uint32_t i;

    if (vpidx >= KVM_MAX_VCPUS) {
        return NULL;
    }
    if (vpidx < vm->nr_vcpus) {
        v = vm->vcpus[vpidx];
        if ((hv_of(v) ? hv_of(v)->vp_index : v->index) == vpidx) {
            return v;
        }
    }
    for (i = 0; i < vm->nr_vcpus; i++) {
        v = vm->vcpus[i];
        if ((hv_of(v) ? hv_of(v)->vp_index : v->index) == vpidx) {
            return v;
        }
    }
    return NULL;
}

/** The VP index of `v`. */
static uint32_t vp_index(const struct kvm_vcpu *v)
{
    return hv_of(v) ? hv_of(v)->vp_index : v->index;
}

/** Raise SINT `sint` of `v`, as Linux's synic_set_irq: the vector into the
 * APIC as a self IPI. The APICs that took it, 0 or 1; -1 for a masked SINT
 * or no APIC. Safe from any context. */
static int synic_set_irq(struct kvm_vcpu *v, uint32_t sint)
{
    struct hv_vcpu *hv = hv_of(v);
    int vector;

    if (!hv || sint >= HV_SYNIC_SINTS) {
        return -1;
    }
    vector = sint_vector(hv->sint[sint]);
    if (vector < 0) {
        return -1;
    }
    return kvm_lapic_set_irq(v, (uint32_t)vector);
}

int kvm_hv_synic_set_irq(struct kvm_vm *vm, uint32_t vpidx, uint32_t sint)
{
    struct kvm_vcpu *v = vcpu_by_vpidx(vm, vpidx);

    if (!v || !hv_of(v) || !hv_of(v)->synic_active) {
        return -1;
    }
    return synic_set_irq(v, sint);
}

/** Leave KVM_EXIT_HYPERV_SYNIC for the caller after the guest wrote `msr`,
 * as Linux's synic_exit. */
static void synic_exit(struct kvm_vcpu *v, uint32_t msr)
{
    struct hv_vcpu *hv = hv_of(v);

    k_memset(&hv->exit, 0, sizeof(hv->exit));
    hv->exit.type = KVM_EXIT_HYPERV_SYNIC;
    hv->exit.u.synic.msr = msr;
    hv->exit.u.synic.control = hv->scontrol;
    hv->exit.u.synic.evt_page = hv->siefp;
    hv->exit.u.synic.msg_page = hv->simp;
    kvm_vcpu_kick(v, KVM_REQ_HV_EXIT);
}

/** Clear the guest's page at `gpa`: 0, or nonzero when it is in no slot. */
static int clear_page(struct kvm_vcpu *v, uint64_t gpa)
{
    static uint8_t zero[256]; /* never written */
    uint32_t off;

    for (off = 0; off < 4096u; off += sizeof(zero)) {
        if (kvm_guest_phys_rw(v, gpa + off, zero, sizeof(zero), 1) != 0) {
            return 1;
        }
    }
    return 0;
}

/** Mark timer `i` of `hv` to be looked at before the next entry. */
static void stimer_mark(struct hv_vcpu *hv, int i)
{
    hv->stimer_pending |= 1u << i;
}

/** An end of interrupt on SINT `sint` of `v`: retry the message a timer
 * of that SINT could not leave, as Linux's kvm_hv_notify_acked_sint. With
 * the I/O APIC the caller's, no GSI has an ack notifier to call. */
static void synic_acked(struct kvm_vcpu *v, uint32_t sint)
{
    struct hv_vcpu *hv = hv_of(v);
    struct hv_stimer *s;
    int i;

    for (i = 0; i < HV_STIMERS; i++) {
        s = &hv->stimer[i];
        if (s->msg_pending && (s->config & STIMER_ENABLE) && !(s->config & STIMER_DIRECT) &&
            STIMER_SINT(s->config) == sint) {
            stimer_mark(hv, i);
        }
    }
}

void kvm_hv_synic_eoi(struct kvm_vcpu *v, uint32_t vector)
{
    struct hv_vcpu *hv = hv_of(v);
    int i;

    if (!hv || vector > 255u || !vec_test(hv->vec_bitmap, vector)) {
        return;
    }
    for (i = 0; i < HV_SYNIC_SINTS; i++) {
        if (sint_vector(hv->sint[i]) == (int)vector) {
            synic_acked(v, (uint32_t)i);
        }
    }
}

int kvm_hv_synic_auto_eoi(struct kvm_vcpu *v, uint32_t vector)
{
    struct hv_vcpu *hv = hv_of(v);

    return hv && vector <= 255u && vec_test(hv->auto_eoi_bitmap, vector);
}

/** A SynIC MSR of `v`, as Linux's synic_set_msr and synic_get_msr: 0, or
 * -1 for #GP or a refusal. */
static int synic_msr(struct kvm_vcpu *v, uint32_t msr, int write, uint64_t *value, int host)
{
    struct hv_vcpu *hv = hv_of(v);
    uint64_t data = *value;
    int i;

    if (!write) {
        if (!hv->synic_active && !host) {
            return -1;
        }
        switch (msr) {
        case HV_MSR_SCONTROL:
            *value = hv->scontrol;
            return 0;
        case HV_MSR_SVERSION:
            *value = hv->sversion;
            return 0;
        case HV_MSR_SIEFP:
            *value = hv->siefp;
            return 0;
        case HV_MSR_SIMP:
            *value = hv->simp;
            return 0;
        case HV_MSR_EOM:
            *value = 0;
            return 0;
        default:
            if (msr >= HV_MSR_SINT0 && msr <= HV_MSR_SINT15) {
                *value = hv->sint[msr - HV_MSR_SINT0];
                return 0;
            }
            return -1;
        }
    }
    if (!hv->synic_active && (!host || data)) {
        return -1;
    }
    switch (msr) {
    case HV_MSR_SCONTROL:
        hv->scontrol = data;
        if (!host) {
            synic_exit(v, msr);
        }
        return 0;
    case HV_MSR_SVERSION:
        if (!host) {
            return -1;
        }
        hv->sversion = data;
        return 0;
    case HV_MSR_SIEFP:
    case HV_MSR_SIMP:
        if ((data & HV_PAGE_ENABLE) && !host && !hv->dont_zero &&
            clear_page(v, data & HV_PAGE_MASK)) {
            return -1;
        }
        if (msr == HV_MSR_SIEFP) {
            hv->siefp = data;
        } else {
            hv->simp = data;
        }
        if (!host) {
            synic_exit(v, msr);
        }
        return 0;
    case HV_MSR_EOM:
        if (hv->synic_active) {
            for (i = 0; i < HV_SYNIC_SINTS; i++) {
                synic_acked(v, (uint32_t)i);
            }
        }
        return 0;
    default:
        if (msr >= HV_MSR_SINT0 && msr <= HV_MSR_SINT15) {
            return synic_set_sint(hv, (int)(msr - HV_MSR_SINT0), data, host);
        }
        return -1;
    }
}

/* The synthetic timers. */

/** Disarm timer `s` of `hv` and forget its pending message, as Linux's
 * stimer_cleanup. */
static void stimer_cleanup(struct hv_vcpu *hv, struct hv_stimer *s)
{
    s->due_ns = 0;
    hv->stimer_pending &= ~(1u << (uint32_t)(s - hv->stimer));
    s->msg_pending = 0;
    s->exp_time = 0;
}

/** Arm `s` to fire `delta` of 100 ns from now, saturating. */
static void stimer_arm(struct hv_stimer *s, uint64_t delta)
{
    uint64_t now = kvm_host_ns();

    s->due_ns = delta >= (~0ull - now) / 100u ? ~0ull : now + delta * 100u;
    if (s->due_ns == 0) {
        s->due_ns = 1;
    }
}

/** Start the enabled timer `s` with a nonzero count, as Linux's
 * stimer_start: a periodic one at its next period, a one-shot one at its
 * count, or at once when the count has passed. */
static void stimer_start(struct kvm_vcpu *v, struct hv_stimer *s)
{
    struct hv_vcpu *hv = hv_of(v);
    uint64_t now = ref_counter(v->vm);

    if (s->config & STIMER_PERIODIC) {
        if (s->exp_time) {
            if (now >= s->exp_time) {
                s->exp_time = now + (s->count - (now - s->exp_time) % s->count);
            }
        } else {
            s->exp_time = now + s->count;
        }
        stimer_arm(s, s->exp_time - now);
        return;
    }
    s->exp_time = s->count;
    if (now >= s->count) {
        stimer_mark(hv, (int)(s - hv->stimer));
        return;
    }
    stimer_arm(s, s->count - now);
}

/** Write timer `n`'s configuration, `count` 0, or its count, 1, as Linux's
 * stimer_set_config and stimer_set_count: 0, or -1. */
static int stimer_set(struct kvm_vcpu *v, int n, int count, uint64_t data, int host)
{
    struct hv_vcpu *hv = hv_of(v);
    struct hv_stimer *s = &hv->stimer[n];

    if (!hv->synic_active && (!host || data)) {
        return -1;
    }
    if (!count) {
        if (!host && hv->enforce && (data & STIMER_DIRECT) &&
            !(hv->features_edx & HV_STIMER_DIRECT_MODE_AVAILABLE)) {
            return -1;
        }
        stimer_cleanup(hv, s);
        if ((s->config & STIMER_ENABLE) && !(data & STIMER_DIRECT) && STIMER_SINT(data) == 0) {
            data &= ~STIMER_ENABLE;
        }
        s->config = data;
    } else {
        stimer_cleanup(hv, s);
        s->count = data;
        if (!host) {
            if (s->count == 0) {
                s->config &= ~STIMER_ENABLE;
            } else if (s->config & STIMER_AUTO_ENABLE) {
                s->config |= STIMER_ENABLE;
            }
        }
    }
    if (s->config & STIMER_ENABLE) {
        stimer_mark(hv, n);
    }
    return 0;
}

/** Put `msg`, `len` bytes, in SINT `sint`'s slot of `v`'s message page and
 * raise the SINT, as Linux's synic_deliver_msg: 0; -1 when the slot is full,
 * with its msg_pending set unless `no_retry`, or 0 then with `no_retry`;
 * -2 for a page not enabled or in no slot, or a SINT no APIC took. */
static int synic_deliver(struct kvm_vcpu *v, uint32_t sint, uint8_t *msg, uint32_t len,
                         int no_retry)
{
    struct hv_vcpu *hv = hv_of(v);
    uint64_t slot;
    uint32_t type;
    uint8_t flags;

    if (!(hv->simp & HV_PAGE_ENABLE)) {
        return -2;
    }
    slot = (hv->simp & HV_PAGE_MASK) + sint * HV_MSG_BYTES;
    if (kvm_guest_phys_rw(v, slot, &type, 4, 0) != 0) {
        return -2;
    }
    if (type != 0) {
        if (no_retry) {
            return 0;
        }
        if (kvm_guest_phys_rw(v, slot + HV_MSG_FLAGS, &flags, 1, 0) != 0) {
            return -2;
        }
        flags |= HV_MSG_PENDING;
        if (kvm_guest_phys_rw(v, slot + HV_MSG_FLAGS, &flags, 1, 1) != 0) {
            return -2;
        }
        return -1;
    }
    if (kvm_guest_phys_rw(v, slot, msg, len, 1) != 0) {
        return -2;
    }
    return synic_set_irq(v, sint) > 0 ? 0 : -2;
}

/** Timer `n` of `v` expired: its message, or in direct mode its vector,
 * as Linux's stimer_expiration; a one-shot timer delivered is disabled. */
static void stimer_expire(struct kvm_vcpu *v, int n)
{
    struct hv_vcpu *hv = hv_of(v);
    struct hv_stimer *s = &hv->stimer[n];
    uint8_t msg[16 + HV_TIMER_PAYLOAD];
    uint64_t now;
    uint32_t w;
    int r;

    s->msg_pending = 1;
    if (s->config & STIMER_DIRECT) {
        r = kvm_lapic_set_irq(v, STIMER_VECTOR(s->config)) > 0 ? 0 : -1;
    } else {
        k_memset(msg, 0, sizeof(msg));
        w = HV_MSG_TIMER_EXPIRED;
        k_memcpy(msg, &w, 4);
        msg[4] = HV_TIMER_PAYLOAD;
        w = (uint32_t)n;
        k_memcpy(msg + 16, &w, 4);
        k_memcpy(msg + 24, &s->exp_time, 8);
        now = ref_counter(v->vm);
        k_memcpy(msg + 32, &now, 8);
        r = synic_deliver(v, STIMER_SINT(s->config), msg, sizeof(msg),
                          (s->config & STIMER_PERIODIC) != 0);
    }
    if (r == 0) {
        s->msg_pending = 0;
        if (!(s->config & STIMER_PERIODIC)) {
            s->config &= ~STIMER_ENABLE;
        }
    }
}

void kvm_hv_stimers(struct kvm_vcpu *v)
{
    struct hv_vcpu *hv = hv_of(v);
    struct hv_stimer *s;
    uint64_t now;
    int i;

    if (!hv) {
        return;
    }
    now = kvm_host_ns();
    for (i = 0; i < HV_STIMERS; i++) {
        s = &hv->stimer[i];
        if (s->due_ns && now >= s->due_ns) {
            s->due_ns = 0;
            stimer_mark(hv, i);
        }
    }
    for (i = 0; i < HV_STIMERS; i++) {
        if (!(hv->stimer_pending & (1u << i))) {
            continue;
        }
        hv->stimer_pending &= ~(1u << i);
        s = &hv->stimer[i];
        if (!(s->config & STIMER_ENABLE)) {
            continue;
        }
        if (s->exp_time && ref_counter(v->vm) >= s->exp_time) {
            stimer_expire(v, i);
        }
        if ((s->config & STIMER_ENABLE) && s->count) {
            if (!s->msg_pending) {
                stimer_start(v, s);
            }
        } else {
            stimer_cleanup(hv, s);
        }
    }
}

uint64_t kvm_hv_stimer_next(const struct kvm_vcpu *v)
{
    const struct hv_vcpu *hv = hv_of(v);
    uint64_t next = 0;
    int i;

    if (!hv) {
        return 0;
    }
    if (hv->stimer_pending) {
        return 1;
    }
    for (i = 0; i < HV_STIMERS; i++) {
        if (hv->stimer[i].due_ns && (!next || hv->stimer[i].due_ns < next)) {
            next = hv->stimer[i].due_ns;
        }
    }
    return next;
}

/* The MSRs. */

/** Whether `v`'s CPUID lets the guest use `msr` under
 * KVM_CAP_HYPERV_ENFORCE_CPUID, as Linux's hv_check_msr_access. */
static int msr_allowed(const struct hv_vcpu *hv, uint32_t msr)
{
    uint32_t a = hv->features_eax, d = hv->features_edx;

    if (!hv->enforce) {
        return 1;
    }
    switch (msr) {
    case HV_MSR_GUEST_OS_ID:
    case HV_MSR_HYPERCALL:
        return (a & HV_MSR_HYPERCALL_AVAILABLE) != 0;
    case HV_MSR_VP_RUNTIME:
        return (a & HV_MSR_VP_RUNTIME_AVAILABLE) != 0;
    case HV_MSR_TIME_REF_COUNT:
        return (a & HV_MSR_TIME_REF_COUNT_AVAILABLE) != 0;
    case HV_MSR_VP_INDEX:
        return (a & HV_MSR_VP_INDEX_AVAILABLE) != 0;
    case HV_MSR_RESET:
        return (a & HV_MSR_RESET_AVAILABLE) != 0;
    case HV_MSR_REFERENCE_TSC:
        return (a & HV_MSR_REFERENCE_TSC_AVAILABLE) != 0;
    case HV_MSR_EOI:
    case HV_MSR_ICR:
    case HV_MSR_TPR:
    case HV_MSR_VP_ASSIST_PAGE:
        return (a & HV_MSR_APIC_ACCESS_AVAILABLE) != 0;
    case HV_MSR_TSC_FREQUENCY:
    case HV_MSR_APIC_FREQUENCY:
        return (a & HV_ACCESS_FREQUENCY_MSRS) != 0;
    case HV_MSR_REENLIGHTENMENT:
    case HV_MSR_TSC_EMULATION_CTL:
    case HV_MSR_TSC_EMULATION_STAT:
        return (a & HV_ACCESS_REENLIGHTENMENT) != 0;
    case HV_MSR_TSC_INVARIANT_CTL:
        return (a & HV_ACCESS_TSC_INVARIANT) != 0;
    case HV_MSR_CRASH_CTL:
        return (d & HV_GUEST_CRASH_MSR_AVAILABLE) != 0;
    case HV_MSR_SYNDBG_OPTIONS:
        return (d & HV_DEBUG_MSRS_AVAILABLE) != 0;
    default:
        break;
    }
    if (msr >= HV_MSR_SCONTROL && msr <= HV_MSR_EOM) {
        return (a & HV_MSR_SYNIC_AVAILABLE) != 0;
    }
    if (msr >= HV_MSR_SINT0 && msr <= HV_MSR_SINT15) {
        return (a & HV_MSR_SYNIC_AVAILABLE) != 0;
    }
    if (msr >= HV_MSR_STIMER0_CONFIG && msr <= HV_MSR_STIMER3_COUNT) {
        return (a & HV_MSR_SYNTIMER_AVAILABLE) != 0;
    }
    if (msr >= HV_MSR_CRASH_P0 && msr <= HV_MSR_CRASH_P4) {
        return (d & HV_GUEST_CRASH_MSR_AVAILABLE) != 0;
    }
    if (msr >= HV_MSR_SYNDBG_CONTROL && msr <= HV_MSR_SYNDBG_PENDING) {
        return (d & HV_DEBUG_MSRS_AVAILABLE) != 0;
    }
    return 0;
}

/** Whether `msr` is one Linux routes to Hyper-V, as kvm_set_msr_common's
 * cases name them. */
static int is_hv_msr(uint32_t msr)
{
    return (msr >= HV_MSR_GUEST_OS_ID && msr <= HV_MSR_SINT15) ||
           (msr >= HV_MSR_SYNDBG_CONTROL && msr <= HV_MSR_SYNDBG_PENDING) ||
           msr == HV_MSR_SYNDBG_OPTIONS || (msr >= HV_MSR_CRASH_P0 && msr <= HV_MSR_CRASH_CTL) ||
           (msr >= HV_MSR_STIMER0_CONFIG && msr <= HV_MSR_STIMER3_COUNT) ||
           msr == HV_MSR_REENLIGHTENMENT || msr == HV_MSR_TSC_EMULATION_CTL ||
           msr == HV_MSR_TSC_EMULATION_STAT || msr == HV_MSR_TSC_INVARIANT_CTL;
}

/** Whether the synthetic debugger is offered to `v`'s guest. */
static int syndbg_enabled(const struct hv_vcpu *hv)
{
    return (hv->syndbg_cap_eax & HV_SYNDBG_CAP_ALLOW_KERNEL_DEBUGGING) != 0;
}

/** Leave KVM_EXIT_HYPERV_SYNDBG for the caller after the guest wrote `msr`,
 * as Linux's syndbg_exit; the caller's status comes back at the next run. */
static void syndbg_exit(struct kvm_vcpu *v, uint32_t msr)
{
    struct hv_vcpu *hv = hv_of(v);
    struct hv_vm *h = hvm_of(v->vm);

    k_memset(&hv->exit, 0, sizeof(hv->exit));
    hv->exit.type = KVM_EXIT_HYPERV_SYNDBG;
    hv->exit.u.syndbg.msr = msr;
    hv->exit.u.syndbg.control = h->syndbg_control;
    hv->exit.u.syndbg.send_page = h->syndbg_send;
    hv->exit.u.syndbg.recv_page = h->syndbg_recv;
    hv->exit.u.syndbg.pending_page = h->syndbg_pending;
    hv->done = DONE_SYNDBG;
    kvm_vcpu_kick(v, KVM_REQ_HV_EXIT);
}

/** A synthetic debugger MSR, as Linux's syndbg_set_msr and
 * syndbg_get_msr: 0, or -1 from a guest the debugger is not offered. */
static int syndbg_msr(struct kvm_vcpu *v, uint32_t msr, int write, uint64_t *value, int host)
{
    struct hv_vm *h = hvm_of(v->vm);
    uint64_t *reg;

    if (!syndbg_enabled(hv_of(v)) && !host) {
        return -1;
    }
    switch (msr) {
    case HV_MSR_SYNDBG_CONTROL:
        reg = &h->syndbg_control;
        break;
    case HV_MSR_SYNDBG_STATUS:
        reg = &h->syndbg_status;
        break;
    case HV_MSR_SYNDBG_SEND_BUFFER:
        reg = &h->syndbg_send;
        break;
    case HV_MSR_SYNDBG_RECV_BUFFER:
        reg = &h->syndbg_recv;
        break;
    case HV_MSR_SYNDBG_PENDING:
        reg = &h->syndbg_pending;
        break;
    default:
        reg = &h->syndbg_options;
        break;
    }
    if (!write) {
        *value = *reg;
        return 0;
    }
    *reg = *value;
    if (!host && (msr == HV_MSR_SYNDBG_CONTROL || msr == HV_MSR_SYNDBG_PENDING)) {
        syndbg_exit(v, msr);
    }
    return 0;
}

/** Write the hypercall page at `gpa`: VMCALL, or VMMCALL on SVM, then RET,
 * as the backends' patch_hypercall. 0, or nonzero for a page in no slot. */
static int hypercall_page(struct kvm_vcpu *v, uint64_t gpa)
{
    uint8_t insn[4];

    insn[0] = 0x0F;
    insn[1] = 0x01;
    insn[2] = kvm_backend == &kvm_svm ? 0xD9 : 0xC1;
    insn[3] = 0xC3;
    return kvm_guest_phys_rw(v, gpa, insn, sizeof(insn), 1);
}

/** Read a partition-wide MSR, as Linux's kvm_hv_get_msr_pw: 0, or -1. */
static int pw_get(struct kvm_vcpu *v, uint32_t msr, uint64_t *value, int host)
{
    struct hv_vm *h = hvm_of(v->vm);

    switch (msr) {
    case HV_MSR_GUEST_OS_ID:
        *value = h->guest_os_id;
        return 0;
    case HV_MSR_HYPERCALL:
        *value = h->hypercall;
        return 0;
    case HV_MSR_TIME_REF_COUNT:
        *value = ref_counter(v->vm);
        return 0;
    case HV_MSR_REFERENCE_TSC:
        *value = h->tsc_msr;
        return 0;
    case HV_MSR_CRASH_CTL:
        *value = h->crash_ctl;
        return 0;
    case HV_MSR_RESET:
        *value = 0;
        return 0;
    case HV_MSR_REENLIGHTENMENT:
        *value = h->reenlightenment;
        return 0;
    case HV_MSR_TSC_EMULATION_CTL:
        *value = h->tsc_emul_control;
        return 0;
    case HV_MSR_TSC_EMULATION_STAT:
        *value = h->tsc_emul_status;
        return 0;
    case HV_MSR_TSC_INVARIANT_CTL:
        *value = h->invtsc_control;
        return 0;
    default:
        break;
    }
    if (msr >= HV_MSR_CRASH_P0 && msr <= HV_MSR_CRASH_P4) {
        *value = h->crash_param[msr - HV_MSR_CRASH_P0];
        return 0;
    }
    return syndbg_msr(v, msr, 0, value, host);
}

/** Write a partition-wide MSR, as Linux's kvm_hv_set_msr_pw: 0, or -1. */
static int pw_set(struct kvm_vcpu *v, uint32_t msr, uint64_t *value, int host)
{
    struct hv_vm *h = hvm_of(v->vm);
    uint64_t data = *value;

    switch (msr) {
    case HV_MSR_GUEST_OS_ID:
        h->guest_os_id = data;
        if (!data) {
            h->hypercall &= ~HV_HYPERCALL_ENABLE;
        }
        return 0;
    case HV_MSR_HYPERCALL:
        if (!h->guest_os_id) {
            return 0;
        }
        if ((data & HV_HYPERCALL_ENABLE) && hypercall_page(v, data & HV_PAGE_MASK) != 0) {
            return -1;
        }
        h->hypercall = data;
        return 0;
    case HV_MSR_REFERENCE_TSC:
        h->tsc_msr = data;
        if (data & HV_PAGE_ENABLE) {
            h->tsc_status = host ? TSC_HOST_CHANGED : TSC_GUEST_CHANGED;
        } else {
            h->tsc_status = TSC_UNSET;
        }
        return 0;
    case HV_MSR_CRASH_CTL:
        if (host) {
            h->crash_ctl = data & HV_CRASH_CTL_NOTIFY;
        } else if (data & HV_CRASH_CTL_NOTIFY) {
            kvm_vcpu_kick(v, KVM_REQ_HV_CRASH);
        }
        return 0;
    case HV_MSR_RESET:
        if (data == 1) {
            kvm_vcpu_kick(v, KVM_REQ_HV_RESET);
        }
        return 0;
    case HV_MSR_REENLIGHTENMENT:
        h->reenlightenment = data;
        return 0;
    case HV_MSR_TSC_EMULATION_CTL:
        h->tsc_emul_control = data;
        return 0;
    case HV_MSR_TSC_EMULATION_STAT:
        if (data && !host) {
            return -1;
        }
        h->tsc_emul_status = data;
        return 0;
    case HV_MSR_TIME_REF_COUNT:
        return host ? 0 : -1;
    case HV_MSR_TSC_INVARIANT_CTL:
        if ((data & ~HV_EXPOSE_INVARIANT_TSC) || (!host && h->invtsc_control && !data)) {
            return -1;
        }
        h->invtsc_control = data;
        return 0;
    default:
        break;
    }
    if (msr >= HV_MSR_CRASH_P0 && msr <= HV_MSR_CRASH_P4) {
        h->crash_param[msr - HV_MSR_CRASH_P0] = data;
        return 0;
    }
    return syndbg_msr(v, msr, 1, value, host);
}

/** Whether `msr` is partition-wide, as Linux's kvm_hv_msr_partition_wide. */
static int is_pw_msr(uint32_t msr)
{
    switch (msr) {
    case HV_MSR_GUEST_OS_ID:
    case HV_MSR_HYPERCALL:
    case HV_MSR_REFERENCE_TSC:
    case HV_MSR_TIME_REF_COUNT:
    case HV_MSR_CRASH_CTL:
    case HV_MSR_RESET:
    case HV_MSR_REENLIGHTENMENT:
    case HV_MSR_TSC_EMULATION_CTL:
    case HV_MSR_TSC_EMULATION_STAT:
    case HV_MSR_TSC_INVARIANT_CTL:
    case HV_MSR_SYNDBG_OPTIONS:
        return 1;
    default:
        return (msr >= HV_MSR_CRASH_P0 && msr <= HV_MSR_CRASH_P4) ||
               (msr >= HV_MSR_SYNDBG_CONTROL && msr <= HV_MSR_SYNDBG_PENDING);
    }
}

/** The CPU time of the calling thread in 100 ns, for HV_MSR_VP_RUNTIME. */
static uint64_t runtime_100ns(void)
{
    return (current_proc->utime + current_proc->stime) * (TICK_NS / 100u);
}

/** The VP assist page written, as Linux's: its first word, the APIC assist,
 * cleared. The paravirtual EOI it would carry is never armed, so the guest
 * always writes its EOI. 0, or -1 for a page in no slot. */
static int set_vp_assist(struct kvm_vcpu *v, uint64_t data)
{
    uint32_t zero = 0;

    if ((data & HV_PAGE_ENABLE) && kvm_guest_phys_rw(v, data & HV_PAGE_MASK, &zero, 4, 1) != 0) {
        return -1;
    }
    hv_of(v)->vapic = data;
    return 0;
}

/** The MSRs of one vCPU, as Linux's kvm_hv_set_msr and kvm_hv_get_msr: 0,
 * or -1. */
static int msr_vcpu(struct kvm_vcpu *v, uint32_t msr, int write, uint64_t *value, int host)
{
    struct hv_vcpu *hv = hv_of(v);
    uint64_t data = *value;
    int n;

    if (msr == HV_MSR_EOI || msr == HV_MSR_ICR || msr == HV_MSR_TPR) {
        n = msr == HV_MSR_EOI ? 0x0B0 : msr == HV_MSR_ICR ? 0x300 : 0x080;
        return kvm_lapic_hv_msr(v, (uint32_t)n, write, value) ? -1 : 0;
    }
    if (msr >= HV_MSR_SCONTROL && msr <= HV_MSR_SINT15) {
        return synic_msr(v, msr, write, value, host);
    }
    if (msr >= HV_MSR_STIMER0_CONFIG && msr <= HV_MSR_STIMER3_COUNT) {
        n = (int)(msr - HV_MSR_STIMER0_CONFIG) / 2;
        if (write) {
            return stimer_set(v, n, (msr & 1u) != 0, data, host);
        }
        *value = (msr & 1u) ? hv->stimer[n].count : hv->stimer[n].config;
        return 0;
    }
    switch (msr) {
    case HV_MSR_VP_INDEX:
        if (!write) {
            *value = hv->vp_index;
            return 0;
        }
        if (!host || (uint32_t)data >= KVM_MAX_VCPUS) {
            return -1;
        }
        hv->vp_index = (uint32_t)data;
        return 0;
    case HV_MSR_VP_ASSIST_PAGE:
        if (!write) {
            *value = hv->vapic;
            return 0;
        }
        return set_vp_assist(v, data);
    case HV_MSR_VP_RUNTIME:
        if (!write) {
            *value = runtime_100ns() + (uint64_t)hv->runtime_offset;
            return 0;
        }
        if (!host) {
            return -1;
        }
        hv->runtime_offset = (int64_t)(data - runtime_100ns());
        return 0;
    case HV_MSR_TSC_FREQUENCY:
    case HV_MSR_APIC_FREQUENCY:
        if (write) {
            return host ? 0 : -1;
        }
        /* The APIC timer counts at 1 GHz, one ns a bus cycle. */
        *value = msr == HV_MSR_TSC_FREQUENCY ? tsc_hz() / 1000u * 1000u : 1000000000ull;
        return 0;
    default:
        return -1;
    }
}

int kvm_hv_msr(struct kvm_vcpu *v, uint32_t index, int write, uint64_t *value, int host)
{
    struct hv_vcpu *hv = hv_of(v);

    if (!is_hv_msr(index)) {
        return 1;
    }
    if (!host && (!hv || !hv->enabled)) {
        return -1;
    }
    if (hv_init(v) != 0) {
        return -1;
    }
    hv = hv_of(v);
    if (!host && !msr_allowed(hv, index)) {
        return -1;
    }
    if (is_pw_msr(index)) {
        return write ? pw_set(v, index, value, host) : pw_get(v, index, value, host);
    }
    return msr_vcpu(v, index, write, value, host);
}

/* The CPUID. */

/** The entry of `fn` in the `n` entries `e`, or NULL. */
static const struct kvm_cpuid_entry2 *cpuid_find(const struct kvm_cpuid_entry2 *e, uint32_t n,
                                                 uint32_t fn)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        if (e[i].function == fn &&
            (!(e[i].flags & KVM_CPUID_FLAG_SIGNIFCANT_INDEX) || e[i].index == 0)) {
            return &e[i];
        }
    }
    return NULL;
}

int kvm_hv_set_cpuid(struct kvm_vcpu *v, const struct kvm_cpuid_entry2 *e, uint32_t n)
{
    const struct kvm_cpuid_entry2 *f = cpuid_find(e, n, HV_CPUID_INTERFACE);
    struct hv_vcpu *hv;
    int enabled = f && f->eax == HV_SIGNATURE_EAX;

    if (enabled && hv_init(v) != 0) {
        return -ENOMEM;
    }
    hv = hv_of(v);
    if (!hv) {
        return 0;
    }
    hv->enabled = enabled;
    hv->features_eax = hv->features_ebx = hv->features_edx = 0;
    hv->enl_eax = hv->enl_ebx = hv->syndbg_cap_eax = hv->nested_eax = hv->nested_ebx = 0;
    if (!enabled) {
        return 0;
    }
    if ((f = cpuid_find(e, n, HV_CPUID_FEATURES)) != NULL) {
        hv->features_eax = f->eax;
        hv->features_ebx = f->ebx;
        hv->features_edx = f->edx;
    }
    if ((f = cpuid_find(e, n, HV_CPUID_ENLIGHTENMENTS)) != NULL) {
        hv->enl_eax = f->eax;
        hv->enl_ebx = f->ebx;
    }
    if ((f = cpuid_find(e, n, HV_CPUID_SYNDBG_PLATFORM)) != NULL) {
        hv->syndbg_cap_eax = f->eax;
    }
    if ((f = cpuid_find(e, n, HV_CPUID_NESTED)) != NULL) {
        hv->nested_eax = f->eax;
        hv->nested_ebx = f->ebx;
    }
    return 0;
}

int kvm_hv_invtsc_suppressed(const struct kvm_vcpu *v)
{
    const struct hv_vcpu *hv = hv_of(v);
    const struct hv_vm *h = hvm_of(v->vm);

    if (!hv || !(hv->features_eax & HV_ACCESS_TSC_INVARIANT)) {
        return 0;
    }
    return !(h->invtsc_control & HV_EXPOSE_INVARIANT_TSC);
}

/** Store the 12 bytes of `sig` in EBX, ECX and EDX of `e`. */
static void put_signature(struct kvm_cpuid_entry2 *e, const char *sig)
{
    k_memcpy(&e->ebx, sig, 4);
    k_memcpy(&e->ecx, sig + 4, 4);
    k_memcpy(&e->edx, sig + 8, 4);
}

int32_t kvm_hv_get_cpuid(struct kvm_vcpu *v, uintptr_t arg)
{
    static const uint32_t fns[10] = {
        HV_CPUID_VENDOR,          HV_CPUID_INTERFACE, HV_CPUID_VERSION,       HV_CPUID_FEATURES,
        HV_CPUID_ENLIGHTENMENTS,  HV_CPUID_LIMITS,    HV_CPUID_SYNDBG_VENDOR, HV_CPUID_SYNDBG_IFACE,
        HV_CPUID_SYNDBG_PLATFORM, HV_CPUID_NESTED};
    struct kvm_cpuid_entry2 ent[10];
    struct kvm_cpuid2 head;
    int apic = !v || v->vm->irqchip != KVM_IRQCHIP_NONE;
    uint32_t i;

    if (copy_from_user(&head, (const void __user *)arg, sizeof(head)) != 0) {
        return -EFAULT;
    }
    if (head.nent < 10u) {
        return -E2BIG;
    }
    head.nent = 10u;
    k_memset(ent, 0, sizeof(ent));
    for (i = 0; i < 10u; i++) {
        ent[i].function = fns[i];
    }
    ent[0].eax = HV_CPUID_SYNDBG_PLATFORM;
    put_signature(&ent[0], "Linux KVM Hv");
    ent[1].eax = HV_SIGNATURE_EAX;
    /* Hyper-V 2016, whose functions KVM implements in part. */
    ent[2].eax = 0x00003839u;
    ent[2].ebx = 0x000A0000u;
    ent[3].eax = HV_MSR_VP_RUNTIME_AVAILABLE | HV_MSR_TIME_REF_COUNT_AVAILABLE |
                 HV_MSR_SYNIC_AVAILABLE | HV_MSR_SYNTIMER_AVAILABLE | HV_MSR_APIC_ACCESS_AVAILABLE |
                 HV_MSR_HYPERCALL_AVAILABLE | HV_MSR_VP_INDEX_AVAILABLE | HV_MSR_RESET_AVAILABLE |
                 HV_MSR_REFERENCE_TSC_AVAILABLE | HV_ACCESS_FREQUENCY_MSRS |
                 HV_ACCESS_REENLIGHTENMENT | HV_ACCESS_TSC_INVARIANT;
    ent[3].ebx = HV_POST_MESSAGES | HV_SIGNAL_EVENTS | HV_ENABLE_EXTENDED_HYPERCALLS | HV_DEBUGGING;
    ent[3].edx = HV_HYPERCALL_XMM_INPUT | HV_FREQUENCY_MSRS_AVAILABLE |
                 HV_GUEST_CRASH_MSR_AVAILABLE | HV_GUEST_DEBUGGING_AVAILABLE |
                 HV_DEBUG_MSRS_AVAILABLE | HV_EXT_GVA_RANGES_FLUSH;
    if (apic) {
        ent[3].edx |= HV_STIMER_DIRECT_MODE_AVAILABLE;
    }
    /* No enlightened VMCS, since no nested guest is offered, and not
     * HV_X64_NO_NONARCH_CORESHARING, since SMT is not known to be off. */
    ent[4].eax = HV_REMOTE_TLB_FLUSH_RECOMMENDED | HV_APIC_ACCESS_RECOMMENDED |
                 HV_RELAXED_TIMING_RECOMMENDED | HV_EX_PROCESSOR_MASKS_RECOMMENDED |
                 HV_DEPRECATING_AEOI_RECOMMENDED;
    if (apic) {
        ent[4].eax |= HV_CLUSTER_IPI_RECOMMENDED;
    }
    ent[4].ebx = 0x00000FFFu; /* spinlock retries, as Hyper-V 2016 */
    ent[5].eax = KVM_MAX_VCPUS;
    ent[5].ebx = 64; /* logical processors, as Hyper-V 2016 */
    put_signature(&ent[6], "Linux KVM Hv");
    k_memcpy(&ent[7].eax, "VS#1", 4);
    ent[8].eax = HV_SYNDBG_CAP_ALLOW_KERNEL_DEBUGGING;
    ent[9].eax = HV_NESTED_DIRECT_FLUSH | HV_NESTED_MSR_BITMAP;
    ent[9].ebx = HV_NESTED_EVMCS1_PERF_GLOBAL_CTL;
    if (copy_to_user((void __user *)arg, &head, sizeof(head)) != 0 ||
        copy_to_user((void __user *)(arg + sizeof(head)), ent, sizeof(ent)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/* The capabilities. */

int32_t kvm_hv_check_extension(long cap)
{
    switch (cap) {
    case KVM_CAP_HYPERV:
    case KVM_CAP_HYPERV_VAPIC:
    case KVM_CAP_HYPERV_SPIN:
    case KVM_CAP_HYPERV_TIME:
    case KVM_CAP_HYPERV_SYNIC:
    case KVM_CAP_HYPERV_SYNIC2:
    case KVM_CAP_HYPERV_VP_INDEX:
    case KVM_CAP_HYPERV_EVENTFD:
    case KVM_CAP_HYPERV_TLBFLUSH:
    case KVM_CAP_HYPERV_SEND_IPI:
    case KVM_CAP_HYPERV_CPUID:
    case KVM_CAP_HYPERV_ENFORCE_CPUID:
    case KVM_CAP_SYS_HYPERV_CPUID:
        return 1;
    default:
        return 0;
    }
}

int32_t kvm_hv_enable_cap(struct kvm_vcpu *v, const struct kvm_enable_cap *cap)
{
    struct hv_vcpu *hv;

    switch (cap->cap) {
    case KVM_CAP_HYPERV_SYNIC2:
        if (cap->args[0]) {
            return -EINVAL;
        }
        /* fall through */
    case KVM_CAP_HYPERV_SYNIC:
        if (v->vm->irqchip == KVM_IRQCHIP_NONE) {
            return -EINVAL;
        }
        if (hv_init(v) != 0) {
            return -ENOMEM;
        }
        hv = hv_of(v);
        hv->synic_active = 1;
        hv->dont_zero = cap->cap == KVM_CAP_HYPERV_SYNIC2;
        hv->scontrol = HV_SYNIC_CONTROL_ENABLE;
        return 0;
    case KVM_CAP_HYPERV_ENLIGHTENED_VMCS:
    case KVM_CAP_HYPERV_DIRECT_TLBFLUSH:
        /* Both are a nested guest's, which is not offered. */
        return -ENOTTY;
    case KVM_CAP_HYPERV_ENFORCE_CPUID:
        if (!hv_of(v)) {
            if (!cap->args[0]) {
                return 0;
            }
            if (hv_init(v) != 0) {
                return -ENOMEM;
            }
        }
        hv_of(v)->enforce = cap->args[0] != 0;
        return 0;
    default:
        return -EINVAL;
    }
}

/* KVM_HYPERV_EVENTFD. */

int32_t kvm_hv_eventfd(struct kvm_vm *vm, uintptr_t arg)
{
    struct kvm_hyperv_eventfd a;
    struct hv_conn *c, **p;
    struct hv_vm *h;
    struct eventfd *ev;
    int32_t err = 0;

    if (copy_from_user(&a, (const void __user *)arg, sizeof(a)) != 0) {
        return -EFAULT;
    }
    if ((a.flags & ~KVM_HYPERV_EVENTFD_DEASSIGN) || (a.conn_id & ~KVM_HYPERV_CONN_ID_MASK)) {
        return -EINVAL;
    }
    h = hvm_of(vm);
    if (a.flags == KVM_HYPERV_EVENTFD_DEASSIGN) {
        for (p = h ? &h->conns : NULL; p && *p; p = &(*p)->next) {
            if ((*p)->conn_id == a.conn_id) {
                c = *p;
                *p = c->next;
                eventfd_put(c->ev);
                kfree(c);
                return 0;
            }
        }
        return -ENOENT;
    }
    ev = eventfd_get(a.fd, &err);
    if (!ev) {
        return err;
    }
    h = hvm_get(vm);
    c = h ? kzalloc(sizeof(*c)) : NULL;
    if (!c) {
        eventfd_put(ev);
        return -ENOMEM;
    }
    for (p = &h->conns; *p; p = &(*p)->next) {
        if ((*p)->conn_id == a.conn_id) {
            kfree(c);
            eventfd_put(ev);
            return -EEXIST;
        }
    }
    c->conn_id = a.conn_id;
    c->ev = ev;
    *p = c;
    return 0;
}

/* The hypercalls. */

int kvm_hv_hypercall_enabled(const struct kvm_vcpu *v)
{
    const struct hv_vcpu *hv = hv_of(v);
    const struct hv_vm *h = hvm_of(v->vm);

    return hv && hv->enabled && h && h->guest_os_id != 0;
}

/** Whether `v`'s CPUID lets the guest make hypercall `code` under
 * KVM_CAP_HYPERV_ENFORCE_CPUID, as Linux's hv_check_hypercall_access. */
static int hcall_allowed(const struct hv_vcpu *hv, uint32_t code)
{
    uint32_t enl = hv->enl_eax;

    if (!hv->enforce) {
        return 1;
    }
    switch (code) {
    case HVCALL_NOTIFY_LONG_SPIN:
        return hv->enl_ebx != 0 && hv->enl_ebx != 0xFFFFFFFFu;
    case HVCALL_POST_MESSAGE:
        return (hv->features_ebx & HV_POST_MESSAGES) != 0;
    case HVCALL_SIGNAL_EVENT:
        return (hv->features_ebx & HV_SIGNAL_EVENTS) != 0;
    case HVCALL_POST_DEBUG_DATA:
    case HVCALL_RETRIEVE_DEBUG_DATA:
    case HVCALL_RESET_DEBUG_SESSION:
        /* With the debugger off the call is answered as an unknown code. */
        return !syndbg_enabled(hv) || (hv->features_ebx & HV_DEBUGGING) != 0;
    case HVCALL_FLUSH_LIST_EX:
    case HVCALL_FLUSH_SPACE_EX:
        return (enl & HV_EX_PROCESSOR_MASKS_RECOMMENDED) && (enl & HV_REMOTE_TLB_FLUSH_RECOMMENDED);
    case HVCALL_FLUSH_LIST:
    case HVCALL_FLUSH_SPACE:
        return (enl & HV_REMOTE_TLB_FLUSH_RECOMMENDED) != 0;
    case HVCALL_SEND_IPI_EX:
        return (enl & HV_EX_PROCESSOR_MASKS_RECOMMENDED) && (enl & HV_CLUSTER_IPI_RECOMMENDED);
    case HVCALL_SEND_IPI:
        return (enl & HV_CLUSTER_IPI_RECOMMENDED) != 0;
    default:
        if (code >= HVCALL_EXT_QUERY && code <= HVCALL_EXT_LAST) {
            return (hv->features_ebx & HV_ENABLE_EXTENDED_HYPERCALLS) != 0;
        }
        return 1;
    }
}

/** Whether `code` takes its fast input in XMM registers as well. */
static int xmm_fast(uint32_t code)
{
    return code == HVCALL_FLUSH_LIST || code == HVCALL_FLUSH_SPACE ||
           code == HVCALL_FLUSH_LIST_EX || code == HVCALL_FLUSH_SPACE_EX ||
           code == HVCALL_SEND_IPI_EX;
}

/** Read `cnt` words of the hypercall's variable input from where the
 * fixed part ends, the first `cap` of them into `data`, as Linux's
 * kvm_hv_get_hc_data: 0, or nonzero for an input the registers or the
 * guest's memory cannot give. */
static int hc_data(struct kvm_vcpu *v, struct hcall *hc, uint32_t cnt, uint32_t cap, uint64_t *data)
{
    uint32_t n = cnt < cap ? cnt : cap, i;

    if (hc->fast) {
        if (hc->xmm_used > 2u * HV_XMM_REGISTERS || cnt > 2u * HV_XMM_REGISTERS - hc->xmm_used) {
            return 1;
        }
        for (i = 0; i < n; i++) {
            data[i] = hc->xmm[i + hc->xmm_used];
        }
        return 0;
    }
    return n ? kvm_guest_phys_rw(v, hc->ingpa + hc->data_offset, data, n * 8u, 0) : 0;
}

/** Read a sparse VP set's banks into `banks`, those that can hold a VP
 * index of the VM: 0, or nonzero for a set the input cannot give. */
static int sparse_banks(struct kvm_vcpu *v, struct hcall *hc, uint64_t *banks)
{
    if (hc->var_cnt > HV_MAX_SPARSE_BANKS) {
        return 1;
    }
    return hc_data(v, hc, hc->var_cnt, HV_BANKS_KEPT, banks);
}

/** The number of bits set in `x`. */
static uint32_t popcount(uint64_t x)
{
    uint32_t n = 0;

    while (x) {
        x &= x - 1u;
        n++;
    }
    return n;
}

/** Whether VP `vp` is in the sparse set of `mask` and `banks`, as Linux's
 * hv_is_vp_in_sparse_set. */
static int vp_in_set(uint32_t vp, uint64_t mask, const uint64_t *banks)
{
    uint32_t bank = vp / 64u, sbank;

    if (bank >= HV_MAX_SPARSE_BANKS || !((mask >> bank) & 1u)) {
        return 0;
    }
    sbank = bank ? popcount(mask & ((1ull << bank) - 1u)) : 0;
    if (sbank >= HV_BANKS_KEPT) {
        return 0;
    }
    return (banks[sbank] >> (vp % 64u)) & 1u;
}

/** Flush the TLBs of the vCPUs of `vm` that `mask` and `banks` name, or of
 * all of them with `all`: each drops its translations before its next
 * entry, and none is left in the guest with old ones when this returns, as
 * Linux's KVM_REQ_HV_TLB_FLUSH waits. KVM_REQ_TLB_FLUSH drops the guest's
 * own translations with the guest-physical ones: INVEPT drops the combined
 * mappings of the EPT tables on VMX, and SVM flushes the ASID. */
static void flush_vcpus(struct kvm_vm *vm, int all, uint64_t mask, const uint64_t *banks)
{
    struct kvm_vcpu *t;
    uint32_t i;

    for (i = 0; i < vm->nr_vcpus; i++) {
        t = vm->vcpus[i];
        if (all || vp_in_set(vp_index(t), mask, banks)) {
            kvm_vcpu_kick(t, KVM_REQ_TLB_FLUSH);
        }
    }
    for (i = 0; i < vm->nr_vcpus; i++) {
        t = vm->vcpus[i];
        while (t->mode == KVM_MODE_IN_GUEST && (t->requests & KVM_REQ_TLB_FLUSH)) {
            arch_relax();
        }
    }
}

/** Whether the rep count, start index and variable header of `hc`, a TLB
 * flush or an IPI, are outside what its code takes, as Linux's
 * kvm_hv_hypercall checks them: a list has reps from 0, the others none,
 * and the simple forms no variable header. */
static int hc_form_bad(const struct hcall *hc)
{
    switch (hc->code) {
    case HVCALL_FLUSH_LIST:
        return hc->var_cnt || !hc->rep_cnt || hc->rep_idx;
    case HVCALL_FLUSH_LIST_EX:
        return !hc->rep_cnt || hc->rep_idx;
    case HVCALL_FLUSH_SPACE:
    case HVCALL_SEND_IPI:
        return hc->var_cnt || hc->rep;
    default:
        return hc->rep;
    }
}

/** HvFlushVirtualAddressSpace and HvFlushVirtualAddressList, and their Ex
 * forms, as Linux's kvm_hv_flush_tlb: the input checked as Linux checks
 * it, then each named vCPU's TLB flushed whole, which the TLFS allows for
 * any list. The status, with every rep counted complete. */
static uint64_t hc_flush(struct kvm_vcpu *v, struct hcall *hc)
{
    uint64_t hdr[4], banks[HV_BANKS_KEPT], entries[HV_FLUSH_FIFO - 1u], mask;
    int all, ex = hc->code == HVCALL_FLUSH_LIST_EX || hc->code == HVCALL_FLUSH_SPACE_EX;

    if (hc_form_bad(hc)) {
        return HV_STATUS_INVALID_INPUT;
    }
    k_memset(banks, 0, sizeof(banks));
    if (!ex) {
        if (hc->fast) {
            hdr[1] = hc->outgpa;
            hdr[2] = hc->xmm[0];
            hc->xmm_used = 1;
        } else {
            if (kvm_guest_phys_rw(v, hc->ingpa, hdr, 24, 0) != 0) {
                return HV_STATUS_INVALID_INPUT;
            }
            hc->data_offset = 24;
        }
        mask = 1;
        banks[0] = hdr[2];
        /* Windows Server 2012 sends an empty mask for every processor. */
        all = (hdr[1] & HV_FLUSH_ALL_PROCESSORS) || hdr[2] == 0;
    } else {
        if (hc->fast) {
            hdr[1] = hc->outgpa;
            hdr[2] = hc->xmm[0];
            hdr[3] = hc->xmm[1];
            hc->xmm_used = 2;
        } else {
            if (kvm_guest_phys_rw(v, hc->ingpa, hdr, 32, 0) != 0) {
                return HV_STATUS_INVALID_INPUT;
            }
            hc->data_offset = 32;
        }
        mask = hdr[3];
        all = hdr[2] != HV_SET_SPARSE_4K;
        if (hc->var_cnt != popcount(mask)) {
            return HV_STATUS_INVALID_INPUT;
        }
        if (!all) {
            if (!hc->var_cnt) {
                return HV_STATUS_SUCCESS | (uint64_t)hc->rep_cnt << HC_REP_SHIFT;
            }
            if (sparse_banks(v, hc, banks) != 0) {
                return HV_STATUS_INVALID_INPUT;
            }
        }
        if (hc->fast) {
            hc->xmm_used += hc->var_cnt;
        } else {
            hc->data_offset += hc->var_cnt * 8u;
        }
    }
    if ((hc->code == HVCALL_FLUSH_LIST || hc->code == HVCALL_FLUSH_LIST_EX) &&
        hc->rep_cnt <= HV_FLUSH_FIFO - 1u && hc_data(v, hc, hc->rep_cnt, hc->rep_cnt, entries)) {
        return HV_STATUS_INVALID_INPUT;
    }
    flush_vcpus(v->vm, all, mask, banks);
    return HV_STATUS_SUCCESS | (uint64_t)hc->rep_cnt << HC_REP_SHIFT;
}

/** HvCallSendSyntheticClusterIpi and its Ex form, as Linux's
 * kvm_hv_send_ipi: a fixed interrupt of the vector to each named vCPU. The
 * status. */
static uint64_t hc_send_ipi(struct kvm_vcpu *v, struct hcall *hc)
{
    uint64_t hdr[3], banks[HV_BANKS_KEPT], mask;
    uint32_t vector, i;
    int all;

    if (hc_form_bad(hc) || v->vm->irqchip == KVM_IRQCHIP_NONE) {
        return HV_STATUS_INVALID_INPUT;
    }
    k_memset(banks, 0, sizeof(banks));
    if (hc->code == HVCALL_SEND_IPI) {
        if (!hc->fast) {
            if (kvm_guest_phys_rw(v, hc->ingpa, hdr, 16, 0) != 0) {
                return HV_STATUS_INVALID_INPUT;
            }
            banks[0] = hdr[1];
            vector = (uint32_t)hdr[0];
        } else {
            if (hc->ingpa >> 32) {
                return HV_STATUS_INVALID_INPUT;
            }
            banks[0] = hc->outgpa;
            vector = (uint32_t)hc->ingpa;
        }
        all = 0;
        mask = 1;
    } else {
        if (!hc->fast) {
            if (kvm_guest_phys_rw(v, hc->ingpa, hdr, 24, 0) != 0) {
                return HV_STATUS_INVALID_INPUT;
            }
        } else {
            hdr[0] = (uint32_t)hc->ingpa;
            hdr[1] = hc->outgpa;
            hdr[2] = hc->xmm[0];
        }
        vector = (uint32_t)hdr[0];
        mask = hdr[2];
        all = hdr[1] == HV_SET_ALL;
        if (hc->var_cnt != popcount(mask)) {
            return HV_STATUS_INVALID_INPUT;
        }
        if (!all) {
            if (!hc->var_cnt) {
                return HV_STATUS_SUCCESS;
            }
            if (!hc->fast) {
                hc->data_offset = 24;
            } else {
                hc->xmm_used = 1;
            }
            if (sparse_banks(v, hc, banks) != 0) {
                return HV_STATUS_INVALID_INPUT;
            }
        }
    }
    if (vector < 0x10u || vector > 0xFFu) {
        return HV_STATUS_INVALID_INPUT;
    }
    for (i = 0; i < v->vm->nr_vcpus; i++) {
        struct kvm_vcpu *t = v->vm->vcpus[i];

        if (all || vp_in_set(vp_index(t), mask, banks)) {
            kvm_lapic_set_irq(t, vector);
        }
    }
    return HV_STATUS_SUCCESS;
}

/** HvSignalEvent: the eventfd of the connection the input names, as
 * Linux's kvm_hvcall_signal_event. The status. */
static uint64_t hc_signal_event(struct kvm_vcpu *v, struct hcall *hc)
{
    struct hv_vm *h = hvm_of(v->vm);
    struct hv_conn *c;

    if (!hc->fast) {
        if ((hc->ingpa & 7u) || (hc->ingpa & 0xFFFu) + 8u > 4096u) {
            return HV_STATUS_INVALID_ALIGNMENT;
        }
        if (kvm_guest_phys_rw(v, hc->ingpa, &hc->ingpa, 8, 0) != 0) {
            return HV_STATUS_INVALID_ALIGNMENT;
        }
    }
    /* Bits 47:32 are the flag number, which no known user sets. */
    if (hc->ingpa & 0xFFFF00000000ull) {
        return HV_STATUS_INVALID_PORT_ID;
    }
    if (hc->ingpa & ~(uint64_t)KVM_HYPERV_CONN_ID_MASK) {
        return HV_STATUS_INVALID_INPUT;
    }
    for (c = h->conns; c; c = c->next) {
        if (c->conn_id == (uint32_t)hc->ingpa) {
            eventfd_signal(c->ev);
            return HV_STATUS_SUCCESS;
        }
    }
    return HV_STATUS_INVALID_PORT_ID;
}

/** Put the hypercall's `result` in RAX, or EDX:EAX outside 64-bit mode. */
static void hc_result(struct kvm_vcpu *v, uint64_t result)
{
    if (kvm_emul_mode(v) == 64) {
        v->regs.rax = result;
    } else {
        v->regs.rdx = result >> 32;
        v->regs.rax = result & 0xFFFFFFFFull;
    }
}

/** Read the hypercall's input of `v` into `hc`, as Linux's kvm_hv_hypercall:
 * from RCX, RDX and R8 in 64-bit mode, else from EDX:EAX, EBX:ECX and
 * EDI:ESI. */
static void hc_decode(const struct kvm_vcpu *v, struct hcall *hc)
{
    k_memset(hc, 0, sizeof(*hc));
    if (kvm_emul_mode(v) == 64) {
        hc->param = v->regs.rcx;
        hc->ingpa = v->regs.rdx;
        hc->outgpa = v->regs.r8;
    } else {
        hc->param = v->regs.rdx << 32 | (v->regs.rax & 0xFFFFFFFFull);
        hc->ingpa = v->regs.rbx << 32 | (v->regs.rcx & 0xFFFFFFFFull);
        hc->outgpa = v->regs.rdi << 32 | (v->regs.rsi & 0xFFFFFFFFull);
    }
    hc->code = (uint32_t)(hc->param & 0xFFFFu);
    hc->var_cnt = (uint32_t)(hc->param >> HC_VARHEAD_SHIFT) & 0x3FFu;
    hc->fast = (hc->param & HC_FAST) != 0;
    hc->rep_cnt = (uint32_t)(hc->param >> HC_REP_SHIFT) & 0xFFFu;
    hc->rep_idx = (uint32_t)(hc->param >> HC_START_SHIFT) & 0xFFFu;
    hc->rep = hc->rep_cnt || hc->rep_idx;
}

/** HvSignalEvent and HvPostMessage: 1 to hand the call to the caller, as
 * Linux does for a message and for an event whose connection the kernel
 * does not know, else 0 with the status in `*ret`. */
static int hc_message(struct kvm_vcpu *v, struct hcall *hc, uint64_t *ret)
{
    if (hc->rep || hc->var_cnt) {
        *ret = HV_STATUS_INVALID_INPUT;
        return 0;
    }
    if (hc->code == HVCALL_SIGNAL_EVENT) {
        *ret = hc_signal_event(v, hc);
        if (*ret != HV_STATUS_INVALID_PORT_ID) {
            return 0;
        }
    }
    if (!hv_of(v)->synic_active) {
        *ret = HV_STATUS_INVALID_INPUT;
        return 0;
    }
    return 1;
}

/** The synthetic debugger's hypercalls: 1 to hand the call to the caller,
 * else 0 with the status in `*ret`. */
static int hc_debug(struct kvm_vcpu *v, const struct hcall *hc, uint64_t *ret)
{
    if (hc->code != HVCALL_RESET_DEBUG_SESSION && hc->fast) {
        *ret = HV_STATUS_INVALID_PARAMETER;
        return 0;
    }
    if (!syndbg_enabled(hv_of(v))) {
        *ret = HV_STATUS_INVALID_CODE;
        return 0;
    }
    if (!(hvm_of(v->vm)->syndbg_options & HV_SYNDBG_OPTION_HCALLS)) {
        *ret = HV_STATUS_OPERATION_DENIED;
        return 0;
    }
    return 1;
}

/** Carry out hypercall `hc` of `v`, as the switch of Linux's
 * kvm_hv_hypercall: 1 to hand it to the caller, else 0 with the status in
 * `*ret`. */
static int hc_dispatch(struct kvm_vcpu *v, struct hcall *hc, uint64_t *ret)
{
    *ret = HV_STATUS_SUCCESS;
    switch (hc->code) {
    case HVCALL_NOTIFY_LONG_SPIN:
        if (hc->rep || hc->var_cnt) {
            *ret = HV_STATUS_INVALID_INPUT;
        }
        return 0;
    case HVCALL_SIGNAL_EVENT:
    case HVCALL_POST_MESSAGE:
        return hc_message(v, hc, ret);
    case HVCALL_FLUSH_LIST:
    case HVCALL_FLUSH_LIST_EX:
    case HVCALL_FLUSH_SPACE:
    case HVCALL_FLUSH_SPACE_EX:
        *ret = hc_flush(v, hc);
        return 0;
    case HVCALL_SEND_IPI:
    case HVCALL_SEND_IPI_EX:
        *ret = hc_send_ipi(v, hc);
        return 0;
    case HVCALL_POST_DEBUG_DATA:
    case HVCALL_RETRIEVE_DEBUG_DATA:
    case HVCALL_RESET_DEBUG_SESSION:
        return hc_debug(v, hc, ret);
    default:
        break;
    }
    if (hc->code < HVCALL_EXT_QUERY || hc->code > HVCALL_EXT_LAST) {
        *ret = HV_STATUS_INVALID_CODE;
        return 0;
    }
    if (hc->fast) {
        *ret = HV_STATUS_INVALID_PARAMETER;
        return 0;
    }
    return 1;
}

int kvm_hv_hypercall(struct kvm_vcpu *v, uint32_t len)
{
    struct hv_vcpu *hv = hv_of(v);
    struct hcall hc;
    uint64_t ret;
    uint32_t i;

    v->exit.kind = KVM_X_AGAIN;
    /* The TLFS's #UD above CPL 0 and in real mode. */
    if (kvm_emul_cpl(v) != 0 || !(v->sregs.cr0 & 1u)) {
        kvm_queue_exception(v, 6, 0, 0);
        return 0;
    }
    hc_decode(v, &hc);
    if (!hcall_allowed(hv, hc.code)) {
        ret = HV_STATUS_ACCESS_DENIED;
    } else if (hc.param & HC_RESERVED) {
        ret = HV_STATUS_INVALID_INPUT;
    } else {
        if (hc.fast && xmm_fast(hc.code)) {
            if (hv->enforce && !(hv->features_edx & HV_HYPERCALL_XMM_INPUT)) {
                kvm_queue_exception(v, 6, 0, 0);
                return 0;
            }
            /* XMM0 to XMM5 at their place in the XSAVE image's legacy area. */
            for (i = 0; i < 2u * HV_XMM_REGISTERS; i++) {
                k_memcpy(&hc.xmm[i], v->xsave + 160u + i * 8u, 8);
            }
        }
        if (hc_dispatch(v, &hc, &ret)) {
            k_memset(&v->exit.hyperv, 0, sizeof(v->exit.hyperv));
            v->exit.kind = KVM_X_HYPERV;
            v->exit.hyperv.type = KVM_EXIT_HYPERV_HCALL;
            v->exit.hyperv.u.hcall.input = hc.param;
            v->exit.hyperv.u.hcall.params[0] = hc.ingpa;
            v->exit.hyperv.u.hcall.params[1] = hc.outgpa;
            hv->done = DONE_HCALL;
            hv->insn_len = len;
            return 0;
        }
    }
    hc_result(v, ret);
    kvm_emul_skip(v, len);
    return 0;
}

void kvm_hv_complete(struct kvm_vcpu *v)
{
    struct hv_vcpu *hv = hv_of(v);
    struct hv_vm *h = hvm_of(v->vm);

    if (!hv || hv->done == DONE_NONE) {
        return;
    }
    if (hv->done == DONE_HCALL) {
        hc_result(v, v->run->u.hyperv.u.hcall.result);
        kvm_emul_skip(v, hv->insn_len);
        v->dirty |= KVM_PART_REGS;
    } else if (v->run->u.hyperv.u.syndbg.msr == HV_MSR_SYNDBG_CONTROL) {
        h->syndbg_status = v->run->u.hyperv.u.syndbg.status;
    }
    hv->done = DONE_NONE;
}

/** End KVM_RUN in KVM_EXIT_SYSTEM_EVENT of `type`, with no data. */
static void system_event(struct kvm_run *run, uint32_t type)
{
    run->exit_reason = KVM_EXIT_SYSTEM_EVENT;
    k_memset(&run->u.system_event, 0, sizeof(run->u.system_event));
    run->u.system_event.type = type;
}

int32_t kvm_hv_prepare(struct kvm_vcpu *v)
{
    struct hv_vcpu *hv = hv_of(v);
    struct kvm_run *run = v->run;

    tsc_page_setup(v);
    if (!hv) {
        return 0;
    }
    if (kvm_vcpu_test_clear(v, KVM_REQ_HV_CRASH)) {
        system_event(run, KVM_SYSTEM_EVENT_CRASH);
        return 1;
    }
    if (kvm_vcpu_test_clear(v, KVM_REQ_HV_RESET)) {
        system_event(run, KVM_SYSTEM_EVENT_RESET);
        return 1;
    }
    if (kvm_vcpu_test_clear(v, KVM_REQ_HV_EXIT)) {
        run->exit_reason = KVM_EXIT_HYPERV;
        run->u.hyperv = hv->exit;
        return 1;
    }
    return 0;
}

void kvm_hv_vcpu_free(struct kvm_vcpu *v)
{
    kfree(v->hv);
    v->hv = NULL;
}

void kvm_hv_vm_free(struct kvm_vm *vm)
{
    struct hv_vm *h = hvm_of(vm);
    struct hv_conn *c;

    if (!h) {
        return;
    }
    while ((c = h->conns) != NULL) {
        h->conns = c->next;
        eventfd_put(c->ev);
        kfree(c);
    }
    kfree(h);
    vm->hv = NULL;
}
