/*
 * cpuid.c: the CPUID KVM offers a guest, from the host's with what KVM
 * cannot give a guest taken out and KVM's own leaves added, and the lookups
 * of a vCPU's CPUID.
 */

#include "kvm.h"

#include "../arch/x86/kcpu.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"

/* The XCR0 components a guest may have: x87, SSE, AVX and AVX-512's three
 * (arch/x86/fpu.h). Not PKRU, which the kernel saves for its own threads:
 * a guest is offered no PKU, may not set CR4.PKE, and so never changes
 * PKRU, and runs under the host's, which the backends leave out of their
 * XSAVE and XRSTOR of its image (KVM_XFEATURE_PKRU in kvm.h). */
#define XCR0_KERNEL 0xE7ull

#define XFEATURE_YMM    0x4ull
#define XFEATURE_AVX512 0xE0ull

/* Leaf 0xD, subleaf 1, EAX: XSAVEOPT, XSAVEC and XGETBV1, offered where
 * the host has them, and XSAVES, offered where the backend's `xsaves` also
 * allows it. A guest has XSAVES with IA32_XSS at 0, its only value
 * (msr.c), as Linux 6.12 offers it with no supervisor state; the host's
 * IA32_XSS, which the kernel never writes, is 0 too, so neither is
 * switched around a run. */
#define XSAVE_D1_EAX    0x7u
#define XSAVE_D1_XSAVEC 0x2u
#define XSAVE_D1_XSAVES 0x8u

/*
 * The bits of each leaf a guest may have where the host has them, as
 * Linux's kvm_set_cpu_caps masks them, less what needs a part of KVM that
 * is not yet in the kernel. Leaf 1, ECX: SSE3, PCLMULQDQ, SSSE3, FMA,
 * CX16, PCID, SSE4.1, SSE4.2, MOVBE, POPCNT, AES, XSAVE, AVX, F16C and
 * RDRAND; not MONITOR, DS-CPL, VMX, SMX, EST, TM2, xTPR or DCA, nor
 * OSXSAVE, which the guest's CR4 gives, nor the TSC deadline, which
 * KVM_CAP_TSC_DEADLINE_TIMER offers instead; x2APIC and HYPERVISOR are
 * set whatever the host has, since the local APIC in the kernel emulates
 * x2APIC, as Linux sets it. EDX: all but PSN, DS, ACPI, HTT, TM and PBE.
 * pmu.c supplies PDCM when the host has performance counters.
 */
#define L1_ECX            0x76DA3203u
#define L1_ECX_X2APIC     (1u << 21)
#define L1_EDX            0x0F8BFBFFu
#define L1_ECX_HYPERVISOR (1u << 31)
/* Leaf 1's AVX and what needs its state: FMA, AVX and F16C; and XSAVE. */
#define L1_ECX_AVX        0x30001000u
#define L1_ECX_XSAVE      (1u << 26)

/* Leaf 7, subleaf 0. EBX: as Linux, less SGX, MPX and Intel PT, with
 * TSC_ADJUST always, since the MSR is emulated (Linux's cpuid.c:710-711).
 * ECX: AVX512VBMI, UMIP, VBMI2, GFNI, VAES, VPCLMULQDQ, VNNI, BITALG,
 * VPOPCNTDQ, RDPID, BUS_LOCK_DETECT, CLDEMOTE, MOVDIRI and MOVDIR64B; not
 * PKU, since a guest's XCR0 may not name PKRU (XCR0_KERNEL), nor LA57
 * before the guest-physical tables have five levels, nor WAITPKG or
 * SGX_LC. EDX: the
 * AVX-512 and string extensions, MD_CLEAR, SERIALIZE and TSXLDTRK, and the
 * speculation controls, which `spec_bits` completes; not ARCH_CAPABILITIES,
 * whose value names the host's flaws, which the kernel does not track, nor
 * AMX, whose state the kernel does not save. */
#define L7_EBX         0xFDBF2FFBu
#define L7_ECX         0x1B405F46u
#define L7_EDX         0x9C81451Cu
/* The bits of leaf 7 that need AVX-512's state, and AVX2, AVX's. */
#define L7_EBX_AVX512  0xDC230000u
#define L7_ECX_AVX512  0x00005842u
#define L7_EDX_AVX512  0x0080010Cu
#define L7_EBX_AVX2    0x00000020u
#define L7_EBX_TSC_ADJ 0x00000002u

/* Leaf 0x80000008's EBX: the speculation controls, AMD's names of Intel's,
 * STIBP always on, SSB_NO and PSFD. */
#define LX8_EBX 0x1502D000u

/* Leaf 0x80000001. ECX: LAHF_LM, CMP_LEGACY, CR8_LEGACY, ABM, SSE4A,
 * MISALIGNSSE, 3DNOWPREFETCH, XOP, FMA4 and TBM; not SVM, the extended
 * APIC space, OSVW, IBS, SKINIT, WDT, TOPOEXT, whose leaves are not
 * offered. pmu.c supplies the core performance counters. EDX: the long-mode
 * bits and the bits of leaf 1 AMD repeats. */
#define LX1_ECX     0x002109F3u
#define LX1_EDX     0xEFD3FBFFu
#define LX1_ECX_AVX 0x00010800u /* XOP, FMA4 */

/* KVM's signature in the EBX, ECX and EDX of KVM_CPUID_SIGNATURE,
 * "KVMKVMKVM\0\0\0". */
#define KVM_SIG_EBX 0x4B4D564Bu
#define KVM_SIG_ECX 0x564B4D56u
#define KVM_SIG_EDX 0x0000004Du

/* The hypervisor leaves' bases Linux searches for a signature: 0x40000000
 * and each 0x100 above it to 0x4000FF00. */
#define HV_BASE_FIRST 0x40000000u
#define HV_BASE_LAST  0x4000FF00u

/** The host's CPUID of `leaf` and `sub` into `e`, with its identity. */
static void host_leaf(struct kvm_cpuid_entry2 *e, uint32_t leaf, uint32_t sub, uint32_t flags)
{
    k_memset(e, 0, sizeof(*e));
    e->function = leaf;
    e->index = sub;
    e->flags = flags;
    kcpu_id_sub(leaf, sub, &e->eax, &e->ebx, &e->ecx, &e->edx);
}

/** The host's leaf 7 EDX, or leaf 0x80000008's EBX with `amd`, 0 where
 * the leaf is past the host's highest. */
static uint32_t host_spec(int amd)
{
    uint32_t leaf = amd ? 0x80000008u : 7u, a = 0, b = 0, c = 0, d = 0;

    kcpu_id(leaf & 0x80000000u, &a, &b, &c, &d);
    if (a < leaf) {
        return 0;
    }
    kcpu_id_sub(leaf, 0, &a, &b, &c, &d);
    return amd ? b : d;
}

/**
 * Complete the speculation controls of `e`, leaf 7 or 0x80000008, whose
 * mask has kept the host's own bits: each named as well in the other
 * vendor's terms where the host has it in either, as Linux's
 * kvm_set_cpu_caps (arch/x86/kvm/cpuid.c:713-719, 787-794), and SVM's
 * VIRT_SSBD where the host carries out VIRT_SPEC_CTRL's SSBD in SPEC_CTRL
 * or LS_CFG, as svm_set_cpu_caps (svm/svm.c:5423-5425).
 */
static void spec_bits(struct kvm_cpuid_entry2 *e)
{
    uint32_t l7 = host_spec(0), x8 = host_spec(1);

    if (e->function == 7u) {
        if ((l7 & CPUID_7_SPEC_CTRL) || ((x8 & CPUID_X8_IBPB) && (x8 & CPUID_X8_IBRS))) {
            e->edx |= CPUID_7_SPEC_CTRL;
        }
        if ((l7 & CPUID_7_STIBP) || (x8 & CPUID_X8_STIBP)) {
            e->edx |= CPUID_7_STIBP;
        }
        if ((l7 & CPUID_7_SSBD) || (x8 & CPUID_X8_SSBD)) {
            e->edx |= CPUID_7_SSBD;
        }
        return;
    }
    if (l7 & CPUID_7_SPEC_CTRL) {
        e->ebx |= CPUID_X8_IBPB | CPUID_X8_IBRS;
    }
    if (l7 & CPUID_7_STIBP) {
        e->ebx |= CPUID_X8_STIBP;
    }
    if (l7 & CPUID_7_SSBD) {
        e->ebx |= CPUID_X8_SSBD;
    }
    if (kvm_backend == &kvm_svm && kvm_virt_ssbd_offered()) {
        e->ebx |= CPUID_X8_VIRT_SSBD;
    }
}

/* The host's answer to `kvm_xcr0_supported` once `xcr0_known` is set. A
 * CPUID traps to the hypervisor beneath a nested host, and each entry asks
 * twice. Two processors that ask first store the same answer. */
static volatile uint64_t xcr0_host;
static volatile int xcr0_known;

uint64_t kvm_xcr0_supported(void)
{
    uint32_t a = 0, b = 0, c = 0, d = 0;
    uint64_t mask = 0;

    if (xcr0_known) {
        return xcr0_host;
    }
    kcpu_id(1, &a, &b, &c, &d);
    if (c & (1u << 26)) {
        kcpu_id_sub(0xD, 0, &a, &b, &c, &d);
        mask = ((uint64_t)a | ((uint64_t)d << 32)) & XCR0_KERNEL;
    }
    xcr0_host = mask;
    xcr0_known = 1;
    return mask;
}

#ifdef RACH2_HOSTTEST
void kvm_xcr0_forget(void)
{
    xcr0_known = 0;
}
#endif

uint32_t kvm_xsave_size(uint64_t features, int compacted)
{
    uint32_t a, b, c, d, size = 576, off, i;

    for (i = 2; i < 64; i++) {
        if ((features >> i) & 1u) {
            /* EAX the size, EBX the standard form's offset, ECX bit 1
             * the compacted form's alignment to 64 bytes. */
            kcpu_id_sub(0xD, i, &a, &b, &c, &d);
            off = !compacted ? b : (c & 0x2u) ? (size + 63u) & ~63u : size;
            if (off + a > size) {
                size = off + a;
            }
        }
    }
    return size;
}

/**
 * Make `e`, the host's entry, the one KVM offers for a host whose XCR0 the
 * guest may have is `xcr0`: 1 to keep it, 0 to leave it out. A leaf this
 * does not name is offered empty, as Linux offers SGX, Intel PT, resource
 * monitoring and the leaves it does not know.
 */
static int filter(struct kvm_cpuid_entry2 *e, uint64_t xcr0)
{
    switch (e->function) {
    case 1:
        e->ecx = (e->ecx & L1_ECX) | L1_ECX_X2APIC | L1_ECX_HYPERVISOR;
        e->edx &= L1_EDX;
        if (!(xcr0 & XFEATURE_YMM)) {
            e->ecx &= ~L1_ECX_AVX;
        }
        if (xcr0 == 0) {
            e->ecx &= ~L1_ECX_XSAVE;
        }
        return 1;
    case 5: /* MONITOR and MWAIT, not offered */
    case 6:
        e->eax = e->function == 6 ? 0x4u : 0u; /* ARAT alone, as Linux */
        e->ebx = e->ecx = e->edx = 0;
        return 1;
    case 7:
        if (e->index == 0) {
            e->eax = e->eax > 1u ? 1u : e->eax;
            e->ebx = (e->ebx & L7_EBX) | L7_EBX_TSC_ADJ;
            e->ecx &= L7_ECX;
            e->edx &= L7_EDX;
            spec_bits(e);
            if ((xcr0 & XFEATURE_AVX512) != XFEATURE_AVX512) {
                e->ebx &= ~L7_EBX_AVX512;
                e->ecx &= ~L7_ECX_AVX512;
                e->edx &= ~L7_EDX_AVX512;
            }
            if (!(xcr0 & XFEATURE_YMM)) {
                e->ebx &= ~L7_EBX_AVX2;
            }
        } else {
            e->eax = e->ebx = e->ecx = e->edx = 0;
        }
        return 1;
    case 0xA: /* pmu.c supplies the architectural performance counters */
        e->eax = e->ebx = e->ecx = e->edx = 0;
        return 1;
    case 0xD:
        if (e->index == 0) {
            e->eax = (uint32_t)xcr0;
            e->edx = (uint32_t)(xcr0 >> 32);
            e->ebx = e->ecx = kvm_xsave_size(xcr0, 0);
            return 1;
        }
        if (e->index == 1) {
            e->eax &= XSAVE_D1_EAX |
                      (kvm_backend && kvm_backend->xsaves && kvm_backend->xsaves() ? XSAVE_D1_XSAVES
                                                                                   : 0u);
            e->ebx = (e->eax & (XSAVE_D1_XSAVEC | XSAVE_D1_XSAVES)) ? kvm_xsave_size(xcr0, 1) : 0u;
            e->ecx = e->edx = 0; /* IA32_XSS names no component */
            return 1;
        }
        return (xcr0 >> e->index) & 1u;
    case 0x80000001u:
        e->ecx &= LX1_ECX;
        e->edx &= LX1_EDX;
        if (!(xcr0 & XFEATURE_YMM)) {
            e->ecx &= ~LX1_ECX_AVX;
        }
        return 1;
    case 0x80000007u:
        e->eax = e->ebx = e->ecx = 0;
        e->edx &= 1u << 8; /* the invariant TSC alone */
        return 1;
    case 0x80000008u:
        e->ebx &= LX8_EBX;
        spec_bits(e);
        e->ecx = e->edx = 0;
        e->eax &= 0xFFFFu; /* the address sizes */
        return 1;
    case 0:
    case 2:
    case 4:
    case 0xB:
    case 0x15:
    case 0x16:
    case 0x1F:
    case 0x80000000u:
    case 0x80000002u:
    case 0x80000003u:
    case 0x80000004u:
    case 0x80000005u:
    case 0x80000006u:
        return 1; /* identity, caches, topology and frequencies, as the host's */
    default:
        e->eax = e->ebx = e->ecx = e->edx = 0;
        return 1;
    }
}

/** Whether `leaf` has subleaves, each an entry with
 * KVM_CPUID_FLAG_SIGNIFCANT_INDEX; `*n` gets how many to offer. */
static int indexed(uint32_t leaf, uint32_t *n)
{
    uint32_t a = 0, b = 0, c = 0, d = 0;

    switch (leaf) {
    case 4: /* the caches, until one of type 0 */
        for (*n = 0; *n < 16; (*n)++) {
            kcpu_id_sub(4, *n, &a, &b, &c, &d);
            if ((a & 0x1Fu) == 0) {
                break;
            }
        }
        return 1;
    case 7:
        kcpu_id_sub(7, 0, &a, &b, &c, &d);
        *n = (a < 1u ? a : 1u) + 1u;
        return 1;
    case 0xB: /* the topology levels, until one of type 0 */
    case 0x1F:
        for (*n = 0; *n < 8; (*n)++) {
            kcpu_id_sub(leaf, *n, &a, &b, &c, &d);
            if (((c >> 8) & 0xFFu) == 0) {
                break;
            }
        }
        return 1;
    case 0xD:
        *n = 64;
        return 1;
    default:
        return 0;
    }
}

uint32_t kvm_pv_supported(void)
{
    uint32_t f = (1u << KVM_FEATURE_NOP_IO_DELAY) | (1u << KVM_FEATURE_STEAL_TIME) |
                 (1u << KVM_FEATURE_PV_UNHALT) | (1u << KVM_FEATURE_PV_TLB_FLUSH) |
                 (1u << KVM_FEATURE_PV_SEND_IPI) | (1u << KVM_FEATURE_POLL_CONTROL) |
                 (1u << KVM_FEATURE_PV_SCHED_YIELD);

    /* Without the TSC's rate no kvmclock can be written, and a guest that
     * enabled one would read a clock that never moves. */
    if (kvm_tsc_khz() != 0) {
        f |= (1u << KVM_FEATURE_CLOCKSOURCE) | (1u << KVM_FEATURE_CLOCKSOURCE2) |
             (1u << KVM_FEATURE_CLOCKSOURCE_STABLE_BIT);
    }
    return f;
}

/** Append KVM's two leaves to `out`, which holds `*count` of `max`, as
 * Linux's KVM_GET_SUPPORTED_CPUID gives them: 0, or -E2BIG. */
static int32_t kvm_leaves(struct kvm_cpuid_entry2 *out, uint32_t *count, uint32_t max)
{
    struct kvm_cpuid_entry2 e;

    if (*count + 2u > max) {
        return -E2BIG;
    }
    k_memset(&e, 0, sizeof(e));
    e.function = KVM_CPUID_SIGNATURE;
    e.eax = KVM_CPUID_FEATURES;
    e.ebx = KVM_SIG_EBX;
    e.ecx = KVM_SIG_ECX;
    e.edx = KVM_SIG_EDX;
    out[(*count)++] = e;
    k_memset(&e, 0, sizeof(e));
    e.function = KVM_CPUID_FEATURES;
    e.eax = kvm_pv_supported();
    out[(*count)++] = e;
    return 0;
}

int32_t kvm_cpuid_emulated(struct kvm_cpuid_entry2 *out, uint32_t max)
{
    uint32_t a = 0, b = 0, c = 0, d = 0;

    if (max < 3u) {
        return -E2BIG;
    }
    k_memset(out, 0, 3u * sizeof(*out));
    out[0].eax = 7;
    out[1].function = 1;
    out[1].ecx = 1u << 22;
    out[2].function = 7;
    out[2].flags = KVM_CPUID_FLAG_SIGNIFCANT_INDEX;
    kcpu_id(0x80000000u, &a, &b, &c, &d);
    if (a >= 0x80000001u) {
        kcpu_id(0x80000001u, &a, &b, &c, &d);
        if ((d >> 27) & 1u) {
            out[2].ecx = 1u << 22;
        }
    }
    return 3;
}

int32_t kvm_cpuid_supported(struct kvm_cpuid_entry2 *out, uint32_t max)
{
    uint64_t xcr0 = kvm_xcr0_supported();
    uint32_t a = 0, b = 0, c = 0, d = 0;
    uint32_t base[2] = {0, 0x80000000u};
    uint32_t top, leaf, sub, subs, count = 0;
    struct kvm_cpuid_entry2 e;
    int i;

    for (i = 0; i < 2; i++) {
        kcpu_id(base[i], &a, &b, &c, &d);
        top = a;
        if (i == 0 && top > 0x1Fu) {
            top = 0x1Fu;
        }
        if (i == 1 && top > 0x80000022u) {
            top = 0x80000022u;
        }
        for (leaf = base[i]; leaf <= top; leaf++) {
            subs = 1;
            if (!indexed(leaf, &subs)) {
                subs = 0;
            }
            for (sub = 0; sub < (subs ? subs : 1u); sub++) {
                host_leaf(&e, leaf, sub, subs ? KVM_CPUID_FLAG_SIGNIFCANT_INDEX : 0u);
                if (i == 0 && leaf == 0 && e.eax > 0x1Fu) {
                    e.eax = 0x1F;
                }
                if (i == 1 && leaf == base[1] && e.eax > 0x80000022u) {
                    e.eax = 0x80000022u;
                }
                if (!filter(&e, xcr0)) {
                    continue;
                }
                kvm_pmu_cpuid(&e);
                if (count == max) {
                    return -E2BIG;
                }
                out[count++] = e;
            }
        }
    }
    if (kvm_leaves(out, &count, max) != 0) {
        return -E2BIG;
    }
    return (int32_t)count;
}

int kvm_cpuid_host_has(uint32_t function, int reg, int bit)
{
    uint32_t top = 0, b = 0, c = 0, d = 0, r;
    struct kvm_cpuid_entry2 e;

    kcpu_id(function & 0x80000000u, &top, &b, &c, &d);
    if (function > top) {
        return 0;
    }
    host_leaf(&e, function, 0, 0);
    if (!filter(&e, kvm_xcr0_supported())) {
        return 0;
    }
    r = reg == 0 ? e.eax : reg == 1 ? e.ebx : reg == 2 ? e.ecx : e.edx;
    return (r >> bit) & 1u;
}

const struct kvm_cpuid_entry2 *kvm_cpuid_find(const struct kvm_vcpu *v, uint32_t function,
                                              uint32_t index)
{
    uint32_t i;

    for (i = 0; i < v->cpuid_nent; i++) {
        const struct kvm_cpuid_entry2 *e = &v->cpuid[i];

        if (e->function == function &&
            (!(e->flags & KVM_CPUID_FLAG_SIGNIFCANT_INDEX) || e->index == index)) {
            return e;
        }
    }
    return NULL;
}

uint64_t kvm_xcr0_guest(const struct kvm_vcpu *v)
{
    const struct kvm_cpuid_entry2 *e = kvm_cpuid_find(v, 0xD, 0);
    uint64_t xcr0;

    if (!e) {
        return 0x1;
    }
    xcr0 = (uint64_t)e->eax | ((uint64_t)e->edx << 32);
    return (xcr0 & kvm_xcr0_supported()) | 0x1u;
}

void kvm_cpuid_xsave_sizes(const struct kvm_vcpu *v, struct kvm_cpuid_entry2 *e, uint32_t n)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        if (e[i].function != 0xDu) {
            continue;
        }
        if (e[i].index == 0) {
            e[i].ebx = kvm_xsave_size(v->xcr0, 0);
        } else if (e[i].index == 1 && (e[i].eax & (XSAVE_D1_XSAVEC | XSAVE_D1_XSAVES))) {
            e[i].ebx = kvm_xsave_size(v->xcr0, 1);
        }
    }
}

/** The entry of `v`'s CPUID for `function` that a caller may change,
 * subleaf 0 or the only one, or NULL. */
static struct kvm_cpuid_entry2 *entry_of(struct kvm_vcpu *v, uint32_t function)
{
    uint32_t i;

    for (i = 0; i < v->cpuid_nent; i++) {
        if (v->cpuid[i].function == function) {
            return &v->cpuid[i];
        }
    }
    return NULL;
}

void kvm_pv_cpuid_update(struct kvm_vcpu *v, int set)
{
    struct kvm_cpuid_entry2 *sig, *feat;
    uint32_t base;

    for (base = HV_BASE_FIRST; base <= HV_BASE_LAST; base += 0x100u) {
        sig = entry_of(v, base);
        if (sig && sig->ebx == KVM_SIG_EBX && sig->ecx == KVM_SIG_ECX && sig->edx == KVM_SIG_EDX) {
            break;
        }
    }
    if (base > HV_BASE_LAST) {
        return;
    }
    feat = entry_of(v, base | KVM_CPUID_FEATURES);
    if (!feat) {
        return;
    }
    if (set && (v->vm->disable_exits & KVM_X86_DISABLE_EXITS_HLT)) {
        feat->eax &= ~(1u << KVM_FEATURE_PV_UNHALT);
    }
    v->pv_features = feat->eax;
}

int kvm_pv_has(const struct kvm_vcpu *v, int bit)
{
    return !v->pv_enforce || ((v->pv_features >> bit) & 1u) != 0;
}
