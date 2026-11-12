/*
 * msr.c: the MSRs of a vCPU that KVM_GET_MSRS and KVM_SET_MSRS move, the
 * index lists of /dev/kvm, the feature MSRs, and the filter and exits by
 * which the caller takes a guest's RDMSR and WRMSR.
 */

#include "kvm.h"

#include "../arch/arch.h"
#include "../arch/x86/kcpu.h"
#include "../lib/kerrno.h"
#include "../lib/kirq.h"
#include "../mm/kheap.h"
#include "../mm/usercopy.h"
#include "../proc/percpu.h"

#define MSR_IA32_P5_MC_ADDR    0x000u
#define MSR_IA32_P5_MC_TYPE    0x001u
#define MSR_IA32_TSC           0x010u
#define MSR_IA32_TSC_ADJUST    0x03Bu
#define MSR_IA32_PRED_CMD      0x049u
#define MSR_IA32_UCODE_REV     0x08Bu
#define MSR_PLATFORM_INFO      0x0CEu
#define MSR_IA32_POWER_CTL     0x1FCu
#define MSR_MTRRCAP            0x0FEu
#define MSR_MISC_FEATURES      0x140u
#define MSR_IA32_SYSENTER_CS   0x174u
#define MSR_IA32_SYSENTER_ESP  0x175u
#define MSR_IA32_SYSENTER_EIP  0x176u
#define MSR_IA32_FLUSH_CMD     0x10Bu
#define MSR_IA32_MCG_CAP       0x179u
#define MSR_IA32_MCG_STATUS    0x17Au
#define MSR_IA32_MCG_CTL       0x17Bu
#define MSR_IA32_MISC_ENABLE   0x1A0u
#define MSR_IA32_DEBUGCTL      0x1D9u
#define MSR_LASTBRANCH_FROM    0x1DBu /* then LASTBRANCHTOIP, LASTINTFROMIP, LASTINTTOIP */
#define N_LASTBRANCH           4u
#define MSR_MTRR_PHYS_BASE0    0x200u
#define MSR_MTRR_FIX_64K       0x250u
#define MSR_MTRR_FIX_16K_80000 0x258u
#define MSR_MTRR_FIX_16K_A0000 0x259u
#define MSR_MTRR_FIX_4K_C0000  0x268u
#define MSR_IA32_CR_PAT        0x277u
#define MSR_IA32_MC0_CTL2      0x280u
#define MSR_IA32_TSC_DEADLINE  0x6E0u
#define MSR_IA32_XSS           0xDA0u
#define MSR_MTRR_DEF_TYPE      0x2FFu
#define MSR_IA32_MC0_CTL       0x400u
#define MSR_STAR               0xC0000081u
#define MSR_LSTAR              0xC0000082u
#define MSR_CSTAR              0xC0000083u
#define MSR_SYSCALL_MASK       0xC0000084u
#define MSR_KERNEL_GS_BASE     0xC0000102u
#define MSR_TSC_AUX            0xC0000103u
#define MSR_AMD64_SYSCFG       0xC0010010u
#define MSR_K7_HWCR            0xC0010015u
#define MSR_AMD64_NB_CFG       0xC001001Fu
#define MSR_AMD64_VIRT_SPEC    0xC001011Fu
#define MSR_AMD64_LS_CFG       0xC0011020u

#define EFER_SCE 0x001ull
#define EFER_LMA 0x400ull

/* The variable-range MTRRs a guest has, and MTRRcap's value: WC and the
 * fixed ranges with that count, as Linux's KVM_NR_VAR_MTRR gives. */
#define VAR_MTRRS   8u
#define MTRRCAP_VAL (0x500ull | VAR_MTRRS)

/* PAT at reset, and MISC_ENABLE with PEBS and BTS unavailable, as Linux
 * gives a new vCPU. */
#define PAT_RESET       0x0007040600070406ull
#define MISC_ENABLE_NEW 0x1800ull

/* HWCR's bits a write may set, McStatusWrEn and TscFreqSel, and those it
 * may set to no effect, TLB cache, flush filter and IGNNE disables, as
 * Linux's kvm_set_msr_common takes them. */
#define HWCR_MCSTATUSWREN (1ull << 18)
#define HWCR_VALID        (HWCR_MCSTATUSWREN | (1ull << 24))
#define HWCR_IGNORED      ((1ull << 3) | (1ull << 6) | (1ull << 8))

#define GP_VECTOR 13

/* SPEC_CTRL's and VIRT_SPEC_CTRL's SSBD; PRED_CMD's IBPB, the one command
 * offered; FLUSH_CMD's L1D flush. */
#define SPEC_CTRL_SSBD (1ull << 2)
#define PRED_CMD_IBPB  1ull
#define L1D_FLUSH      1ull

/* The x2APIC's MSRs, which no filter denies. */
#define MSR_X2APIC_FIRST 0x800u
#define MSR_X2APIC_LAST  0x8FFu

/* CPUID leaf 0xD's subleaf 1, EAX's bit for XSAVES. */
#define CPUID_D1_XSAVES 0x8u

/* MISC_ENABLE's MWAIT bit; PLATFORM_INFO's bit that offers CPUID
 * faulting, and MISC_FEATURES_ENABLES' that turns it on. */
#define MISC_ENABLE_MWAIT         (1ull << 18)
#define PLATFORM_INFO_CPUID_FAULT (1ull << 31)
#define MISC_FEATURES_CPUID_FAULT 1ull

/* The values of UCODE_REV a vCPU starts with under
 * KVM_X86_QUIRK_STUFF_FEATURE_MSRS, Linux's for VMX and for SVM. */
#define UCODE_REV_VMX 0x100000000ull
#define UCODE_REV_SVM 0x01000065ull

/* KVM_GET_MSR_INDEX_LIST: the MSRs a caller saves and restores, the TSC
 * deadline among them, which the local APIC keeps (vlapic.c), Linux's list
 * of Hyper-V's, which hyperv.c keeps, KVM's own that pvclock.c keeps, the
 * two of CPUID faulting, POWER_CTL, HWCR and the microcode revision.
 * TSC_AUX is last, and listed only where the host has RDTSCP. */
static const uint32_t index_list[] = {MSR_IA32_SYSENTER_CS,
                                      MSR_IA32_SYSENTER_ESP,
                                      MSR_IA32_SYSENTER_EIP,
                                      MSR_STAR,
                                      MSR_CSTAR,
                                      MSR_KERNEL_GS_BASE,
                                      MSR_SYSCALL_MASK,
                                      MSR_LSTAR,
                                      MSR_IA32_TSC,
                                      MSR_IA32_CR_PAT,
                                      MSR_IA32_TSC_ADJUST,
                                      MSR_IA32_MISC_ENABLE,
                                      MSR_IA32_MCG_STATUS,
                                      MSR_IA32_MCG_CTL,
                                      MSR_IA32_TSC_DEADLINE,
                                      0x40000000u, /* HV_X64_MSR_GUEST_OS_ID */
                                      0x40000001u, /* HYPERCALL */
                                      0x40000020u, /* TIME_REF_COUNT */
                                      0x40000021u, /* REFERENCE_TSC */
                                      0x40000022u, /* TSC_FREQUENCY */
                                      0x40000023u, /* APIC_FREQUENCY */
                                      0x40000100u, /* CRASH_P0 to P4 */
                                      0x40000101u,
                                      0x40000102u,
                                      0x40000103u,
                                      0x40000104u,
                                      0x40000105u, /* CRASH_CTL */
                                      0x40000003u, /* RESET */
                                      0x40000002u, /* VP_INDEX */
                                      0x40000010u, /* VP_RUNTIME */
                                      0x40000080u, /* SCONTROL */
                                      0x400000B0u, /* STIMER0_CONFIG */
                                      0x40000073u, /* VP_ASSIST_PAGE */
                                      0x40000106u, /* REENLIGHTENMENT_CONTROL */
                                      0x40000107u, /* TSC_EMULATION_CONTROL */
                                      0x40000108u, /* TSC_EMULATION_STATUS */
                                      0x40000118u, /* TSC_INVARIANT_CONTROL */
                                      0x400000FFu, /* SYNDBG_OPTIONS */
                                      0x400000F1u, /* SYNDBG_CONTROL */
                                      0x400000F2u, /* SYNDBG_STATUS */
                                      0x400000F3u, /* SYNDBG_SEND_BUFFER */
                                      0x400000F4u, /* SYNDBG_RECV_BUFFER */
                                      0x400000F5u, /* SYNDBG_PENDING_BUFFER */
                                      MSR_KVM_SYSTEM_TIME,
                                      MSR_KVM_WALL_CLOCK,
                                      MSR_KVM_SYSTEM_TIME_NEW,
                                      MSR_KVM_WALL_CLOCK_NEW,
                                      MSR_KVM_STEAL_TIME,
                                      MSR_KVM_POLL_CONTROL,
                                      MSR_PLATFORM_INFO,
                                      MSR_MISC_FEATURES,
                                      MSR_IA32_POWER_CTL,
                                      MSR_K7_HWCR,
                                      MSR_IA32_UCODE_REV,
                                      MSR_TSC_AUX};

static const uint32_t feature_list[] = {MSR_IA32_UCODE_REV, 0x345u}; /* PERF_CAPABILITIES */

#define N_INDEX   (sizeof(index_list) / sizeof(index_list[0]))
#define N_FEATURE (sizeof(feature_list) / sizeof(feature_list[0]))

/** Whether the host has RDTSCP, and so TSC_AUX. */
static int host_rdtscp(void)
{
    uint32_t a = 0, b = 0, c = 0, d = 0;

    kcpu_id(0x80000000u, &a, &b, &c, &d);
    if (a < 0x80000001u) {
        return 0;
    }
    kcpu_id(0x80000001u, &a, &b, &c, &d);
    return (d >> 27) & 1u;
}

/**
 * Whether the host takes `value` in IA32_SPEC_CTRL: written and the old
 * value put back, as Linux's kvm_spec_ctrl_test_value (x86.c:12867-12890),
 * which leaves to the processor which bits it has. 0 where the host has no
 * such MSR.
 */
static int spec_ctrl_ok(uint64_t value)
{
    uint64_t saved;
    uint32_t flags = kirq_save();
    int ok = 0;

    if (kcpu_rdmsr_safe(MSR_IA32_SPEC_CTRL, &saved) == 0 &&
        kcpu_wrmsr_safe(MSR_IA32_SPEC_CTRL, value) == 0) {
        (void)kcpu_wrmsr_safe(MSR_IA32_SPEC_CTRL, saved);
        ok = 1;
    }
    kirq_restore(flags);
    return ok;
}

/* Each processor's IA32_SPEC_CTRL, once `spec_host_read` is set there. The
 * kernel never writes it. */
static uint64_t spec_host[NR_CPUS];
static uint8_t spec_host_read[NR_CPUS];

uint64_t kvm_spec_ctrl_host(int cpu)
{
    if (!spec_host_read[cpu]) {
        if (kcpu_rdmsr_safe(MSR_IA32_SPEC_CTRL, &spec_host[cpu]) != 0) {
            spec_host[cpu] = 0;
        }
        spec_host_read[cpu] = 1;
    }
    return spec_host[cpu];
}

/* How the host carries out the SSBD a guest asks for in VIRT_SPEC_CTRL,
 * as `kvm_spec_start` chose: not at all, in IA32_SPEC_CTRL, in the host's
 * own VIRT_SPEC_CTRL beneath another hypervisor, or in LS_CFG's bit of the
 * processor's family. */
enum virt_ssbd { VSSBD_NONE, VSSBD_SPEC, VSSBD_VIRT, VSSBD_LS_CFG };
static enum virt_ssbd vssbd;

/* VIRT_SPEC_CTRL's SSBD as the host had it at `kvm_spec_start`. */
static uint64_t virt_host;

/* LS_CFG as the host had it at `kvm_spec_start`, and its bit that disables
 * speculative store bypass. */
static uint64_t ls_cfg_base, ls_cfg_ssbd;

/* With `ls_cfg_smt`, the two threads of a core share LS_CFG, as on family
 * 17h with SMT: SSBD stays set while a guest on either asks for it.
 * `ls_core` is each processor's core, its extended APIC ID halved, once
 * `ls_core_known` is set there; `ls_want` whether a guest's SSBD is in force
 * there. `ls_cfg_lock` keeps all three. */
static int ls_cfg_smt;
static struct spinlock ls_cfg_lock = SPINLOCK_INIT_NAMED("kvm_ls_cfg");
static uint32_t ls_core[NR_CPUS];
static uint8_t ls_core_known[NR_CPUS];
static uint8_t ls_want[NR_CPUS];

/** The family of a CPUID leaf 1 signature, the extended family added to
 * family 15, as Linux's x86_family. */
static uint32_t sig_family(uint32_t signature)
{
    uint32_t family = (signature >> 8) & 0xFu;

    return family == 0xFu ? family + ((signature >> 20) & 0xFFu) : family;
}

void kvm_spec_start(void)
{
    uint32_t a = 0, b = 0, c = 0, d = 0, top, family, bit;
    uint64_t value;

    vssbd = VSSBD_NONE;
    ls_cfg_smt = 0;
    /* SSBD in IA32_SPEC_CTRL first: Linux clears VIRT_SSBD where the host
     * has AMD_SSBD (arch/x86/kernel/cpu/common.c:970-974). */
    if (spec_ctrl_ok(SPEC_CTRL_SSBD)) {
        vssbd = VSSBD_SPEC;
        return;
    }
    kcpu_id(0x80000000u, &top, &b, &c, &d);
    if (top >= 0x80000008u) {
        kcpu_id(0x80000008u, &a, &b, &c, &d);
        if ((b & CPUID_X8_VIRT_SSBD) && kcpu_rdmsr_safe(MSR_AMD64_VIRT_SPEC, &value) == 0) {
            vssbd = VSSBD_VIRT;
            virt_host = value & SPEC_CTRL_SSBD;
            return;
        }
    }
    /* LS_CFG's bit on AMD's families 15h to 17h, where the register reads,
     * as Linux's bsp_init_amd (arch/x86/kernel/cpu/amd.c:463-483). */
    kcpu_id(0, &a, &b, &c, &d);
    if (b != 0x68747541u || d != 0x69746E65u || c != 0x444D4163u) { /* AuthenticAMD */
        return;
    }
    kcpu_id(1, &a, &b, &c, &d);
    family = sig_family(a);
    bit = family == 0x15u ? 54u : family == 0x16u ? 33u : family == 0x17u ? 10u : 0u;
    if (bit == 0 || kcpu_rdmsr_safe(MSR_AMD64_LS_CFG, &ls_cfg_base) != 0) {
        return;
    }
    ls_cfg_ssbd = 1ull << bit;
    vssbd = VSSBD_LS_CFG;
    /* Zen's threads share the register (arch/x86/kernel/process.c:555-561);
     * leaf 0x8000001E's EBX names two threads to a core. */
    if (family == 0x17u && top >= 0x8000001Eu) {
        kcpu_id(0x8000001Eu, &a, &b, &c, &d);
        ls_cfg_smt = ((b >> 8) & 0xFFu) == 1u;
    }
}

int kvm_virt_ssbd_offered(void)
{
    return vssbd == VSSBD_SPEC || vssbd == VSSBD_LS_CFG;
}

uint64_t kvm_spec_ctrl_guest(const struct kvm_vcpu *v)
{
    return v->spec_ctrl | (vssbd == VSSBD_SPEC ? v->virt_spec_ctrl & SPEC_CTRL_SSBD : 0);
}

/**
 * Set LS_CFG's SSBD on processor `cpu` for a guest's entry with `in`, or
 * clear it after the exit without. Where a core's two threads share the
 * register, the first of them to set it writes it and the last to clear it
 * writes it back, as Linux's amd_set_core_ssb_state
 * (arch/x86/kernel/process.c:562-599). Interrupts off.
 */
static void ls_cfg_switch(int cpu, int in)
{
    uint32_t a = 0, b = 0, c = 0, d = 0;
    int i, others = 0;

    if (!ls_cfg_smt) {
        (void)kcpu_wrmsr_safe(MSR_AMD64_LS_CFG, ls_cfg_base | (in ? ls_cfg_ssbd : 0));
        return;
    }
    spin_lock(&ls_cfg_lock);
    if (!ls_core_known[cpu]) {
        kcpu_id(0x8000001Eu, &a, &b, &c, &d);
        ls_core[cpu] = a >> 1;
        ls_core_known[cpu] = 1;
    }
    for (i = 0; i < NR_CPUS; i++) {
        if (i != cpu && ls_want[i] && ls_core[i] == ls_core[cpu]) {
            others = 1;
        }
    }
    ls_want[cpu] = (uint8_t)(in != 0);
    if (!others) {
        (void)kcpu_wrmsr_safe(MSR_AMD64_LS_CFG, ls_cfg_base | (in ? ls_cfg_ssbd : 0));
    }
    spin_unlock(&ls_cfg_lock);
}

void kvm_virt_ssbd_switch(const struct kvm_vcpu *v, int cpu, int in)
{
    uint64_t guest = v->virt_spec_ctrl & SPEC_CTRL_SSBD;

    if (vssbd == VSSBD_VIRT && guest != virt_host) {
        (void)kcpu_wrmsr_safe(MSR_AMD64_VIRT_SPEC, in ? guest : virt_host);
    } else if (vssbd == VSSBD_LS_CFG && guest && !(ls_cfg_base & ls_cfg_ssbd)) {
        ls_cfg_switch(cpu, in);
    }
}

const uint32_t *kvm_msr_list(int feature, uint32_t *n)
{
    static uint32_t list[N_INDEX + 40u];
    uint64_t probe;
    uint32_t i;

    if (feature) {
        *n = N_FEATURE;
        return feature_list;
    }
    *n = (uint32_t)N_INDEX - (host_rdtscp() ? 0u : 1u);
    for (i = 0; i < *n; i++) {
        list[i] = index_list[i];
    }
    /* Linux saves DEBUGCTL among msrs_to_save_base (arch/x86/kvm/x86.c:323)
     * wherever the host reads it, as every host with VMX or SVM does, and
     * the four last-branch MSRs beside it (x86.c:324-325) wherever AMD's
     * host has them. */
    list[(*n)++] = MSR_IA32_DEBUGCTL;
    /* SPEC_CTRL where the host has it, as Linux's kvm_probe_msr_to_save,
     * and SVM's VIRT_SPEC_CTRL among the emulated MSRs (x86.c:313, 403). */
    if (kcpu_rdmsr_safe(MSR_IA32_SPEC_CTRL, &probe) == 0) {
        list[(*n)++] = MSR_IA32_SPEC_CTRL;
    }
    if (kvm_backend == &kvm_svm) {
        list[(*n)++] = MSR_AMD64_VIRT_SPEC;
    }
    for (i = 0; kvm_backend == &kvm_svm && i < N_LASTBRANCH; i++) {
        list[(*n)++] = MSR_LASTBRANCH_FROM + i;
    }
    *n += kvm_pmu_msr_list(list + *n);
    return list;
}

int kvm_msr_feature(uint32_t index, uint64_t *value)
{
    uint32_t lo = 0, hi = 0;

    if (index == MSR_IA32_UCODE_REV) {
        /* The host's own revision, as Linux's kvm_get_feature_msr reads;
         * a processor with VMX or SVM has the MSR. */
        kcpu_rdmsr(MSR_IA32_UCODE_REV, &lo, &hi);
        *value = ((uint64_t)hi << 32) | lo;
        return 0;
    }
    return kvm_pmu_feature(index, value);
}

/** Whether `t` is a memory type an MTRR or PAT may name: UC, WC, WT, WP
 * or WB. */
static int mem_type_ok(uint64_t t)
{
    return t == 0 || t == 1 || t == 4 || t == 5 || t == 6;
}

/** Whether each of the eight bytes of `v` is such a type, or for PAT, also
 * UC-, 7. */
static int types_ok(uint64_t v, int pat)
{
    int i;

    for (i = 0; i < 8; i++) {
        uint64_t t = (v >> (i * 8)) & 0xFFu;

        if (!mem_type_ok(t) && !(pat && t == 7)) {
            return 0;
        }
    }
    return 1;
}

/** Whether `index` is a fixed-range MTRR. */
static int mtrr_fixed(uint32_t index)
{
    return index == MSR_MTRR_FIX_64K || index == MSR_MTRR_FIX_16K_80000 ||
           index == MSR_MTRR_FIX_16K_A0000 ||
           (index >= MSR_MTRR_FIX_4K_C0000 && index < MSR_MTRR_FIX_4K_C0000 + 8u);
}

/** Whether `index` is an MSR the vCPU's table keeps: one of the index
 * list but TSC, MCG_STATUS, MCG_CTL, Hyper-V's, KVM's, the two of CPUID
 * faulting, POWER_CTL and HWCR, or an MTRR. */
static int table_msr(uint32_t index)
{
    uint32_t i;

    if (index == MSR_IA32_TSC || index == MSR_IA32_MCG_STATUS || index == MSR_IA32_MCG_CTL ||
        index == MSR_PLATFORM_INFO || index == MSR_MISC_FEATURES || index == MSR_IA32_POWER_CTL ||
        index == MSR_K7_HWCR || (index >> 12) == 0x40000u || index == MSR_KVM_SYSTEM_TIME ||
        index == MSR_KVM_WALL_CLOCK || (index >> 8) == (MSR_KVM_WALL_CLOCK_NEW >> 8)) {
        return 0;
    }
    for (i = 0; i < N_INDEX; i++) {
        if (index_list[i] == index) {
            return index != MSR_TSC_AUX || host_rdtscp();
        }
    }
    return index == MSR_MTRR_DEF_TYPE || mtrr_fixed(index) ||
           (index >= MSR_MTRR_PHYS_BASE0 && index < MSR_MTRR_PHYS_BASE0 + 2u * VAR_MTRRS);
}

/** The slot of `index` in `v`'s table, made with the value 0 when it has
 * none: the slot, or -1 when the table is full. */
static int table_slot(struct kvm_vcpu *v, uint32_t index)
{
    uint32_t i;

    for (i = 0; i < v->msr_count; i++) {
        if (v->msr_index[i] == index) {
            return (int)i;
        }
    }
    if (v->msr_count == KVM_VCPU_MSRS) {
        return -1;
    }
    v->msr_index[v->msr_count] = index;
    v->msr_value[v->msr_count] = 0;
    return (int)v->msr_count++;
}

void kvm_msr_reset(struct kvm_vcpu *v)
{
    v->msr_count = 0;
    v->msr_value[table_slot(v, MSR_IA32_CR_PAT)] = PAT_RESET;
    v->msr_value[table_slot(v, MSR_IA32_MISC_ENABLE)] = MISC_ENABLE_NEW;
    v->mcg_cap = KVM_MAX_MCE_BANKS;
    v->mcg_status = 0;
    v->mcg_ctl = 0;
    /* CPUID faulting is offered, and the microcode revision set, unless
     * the caller fills the feature MSRs itself, as Linux's
     * kvm_arch_vcpu_create. */
    v->platform_info =
        KVM_QUIRK(v->vm, KVM_X86_QUIRK_STUFF_FEATURE_MSRS) ? PLATFORM_INFO_CPUID_FAULT : 0;
    v->ucode_rev = 0;
    if (KVM_QUIRK(v->vm, KVM_X86_QUIRK_STUFF_FEATURE_MSRS)) {
        v->ucode_rev = kvm_backend == &kvm_svm ? UCODE_REV_SVM : UCODE_REV_VMX;
    }
    v->misc_features = 0;
    v->hwcr = 0;
    v->power_ctl = 0;
    v->spec_ctrl = 0;
    v->virt_spec_ctrl = 0;
}

/** The value of the table's MSR `index` of `v`, 0 when it has none. */
static uint64_t table_value(const struct kvm_vcpu *v, uint32_t index)
{
    uint32_t i;

    for (i = 0; i < v->msr_count; i++) {
        if (v->msr_index[i] == index) {
            return v->msr_value[i];
        }
    }
    return 0;
}

int kvm_msr_guest_mwait(const struct kvm_vcpu *v)
{
    const struct kvm_cpuid_entry2 *e = kvm_cpuid_find(v, 1, 0);

    if (!e) {
        return 0;
    }
    if (!KVM_QUIRK(v->vm, KVM_X86_QUIRK_MISC_ENABLE_NO_MWAIT)) {
        return (table_value(v, MSR_IA32_MISC_ENABLE) & MISC_ENABLE_MWAIT) != 0;
    }
    return (e->ecx >> 3) & 1u;
}

/** Whether `index` is an MSR of the KVM_MAX_MCE_BANKS machine-check
 * banks: 1 for one of a bank `v` has, whose bank and register, 0 CTL to
 * 3 MISC, go in `*bank` and `*reg`; -1 for one of a bank it lacks, which
 * Linux refuses as it refuses a value; 0 for another MSR. */
static int mce_bank_msr(const struct kvm_vcpu *v, uint32_t index, uint32_t *bank, uint32_t *reg)
{
    uint32_t off;

    if (index < MSR_IA32_MC0_CTL || index >= MSR_IA32_MC0_CTL + 4u * KVM_MAX_MCE_BANKS) {
        return 0;
    }
    off = index - MSR_IA32_MC0_CTL;
    if (off / 4u >= (uint32_t)(v->mcg_cap & 0xFFu)) {
        return -1;
    }
    *bank = off / 4u;
    *reg = off % 4u;
    return 1;
}

/** Whether `index` is the CTL2 MSR of a machine-check bank: 1 for one of
 * a bank `v` has, -1 for one of a bank it lacks, 0 for another MSR. No
 * MCG_CAP offers MCG_CMCI_P, which Linux 6.12 offers on VMX alone, so a
 * CTL2 of a bank that is there holds 0 alone: the caller reads 0 and
 * writes 0, and the guest neither reads nor writes it, as Linux's
 * get_msr_mce and set_msr_mce. */
static int mce_ctl2_msr(const struct kvm_vcpu *v, uint32_t index)
{
    if (index < MSR_IA32_MC0_CTL2 || index >= MSR_IA32_MC0_CTL2 + KVM_MAX_MCE_BANKS) {
        return 0;
    }
    return index - MSR_IA32_MC0_CTL2 < (uint32_t)(v->mcg_cap & 0xFFu) ? 1 : -1;
}

/** Whether the guest of `v` may write a nonzero value to a bank's STATUS:
 * a guest whose vendor is AMD or Hygon, with HWCR's McStatusWrEn set, as
 * Linux's can_set_mci_status. */
static int mci_status_writable(const struct kvm_vcpu *v)
{
    return kvm_guest_amd(v) && (v->hwcr & HWCR_MCSTATUSWREN);
}

/** Whether `v`'s CPUID offers XSAVES, in leaf 0xD's subleaf 1, without
 * which IA32_XSS is not the guest's, as Linux's kvm_get_msr_common and
 * kvm_set_msr_common have it. */
static int guest_xsaves(const struct kvm_vcpu *v)
{
    const struct kvm_cpuid_entry2 *e = kvm_cpuid_find(v, 0xD, 1);

    return e && (e->eax & CPUID_D1_XSAVES);
}

/** Whether `v`'s CPUID has any of `bits` in leaf 7's EDX, or with `amd` in
 * leaf 0x80000008's EBX. */
static int guest_spec(const struct kvm_vcpu *v, int amd, uint32_t bits)
{
    const struct kvm_cpuid_entry2 *e = kvm_cpuid_find(v, amd ? 0x80000008u : 7u, 0);

    return e && ((amd ? e->ebx : e->edx) & bits) != 0;
}

/** Whether `v`'s guest has IA32_SPEC_CTRL, as Linux's
 * guest_has_spec_ctrl_msr (arch/x86/kvm/cpuid.h:153-159). */
static int guest_spec_ctrl(const struct kvm_vcpu *v)
{
    return guest_spec(v, 0, CPUID_7_SPEC_CTRL) ||
           guest_spec(v, 1, CPUID_X8_IBRS | CPUID_X8_STIBP | CPUID_X8_SSBD);
}

/**
 * Write the command `value` to PRED_CMD or FLUSH_CMD, `index`, for the
 * caller with `host` or the guest, as Linux's kvm_set_msr_common
 * (x86.c:3381-3421): IBPB where the guest's CPUID and the host have the
 * barrier, the L1D flush where both have it, and 0 as no command. The
 * command runs on the host's processor. 0, or KVM_MSR_INVAL.
 */
static int command_set(const struct kvm_vcpu *v, uint32_t index, uint64_t value, int host)
{
    if (index == MSR_IA32_PRED_CMD) {
        if (!host && !guest_spec(v, 0, CPUID_7_SPEC_CTRL) && !guest_spec(v, 1, CPUID_X8_IBPB)) {
            return KVM_MSR_INVAL;
        }
        if ((value & ~PRED_CMD_IBPB) || (value && !kvm_cpuid_host_has(0x80000008u, 1, 12))) {
            return KVM_MSR_INVAL;
        }
    } else {
        if (!host && !guest_spec(v, 0, CPUID_7_FLUSH_L1D)) {
            return KVM_MSR_INVAL;
        }
        if ((value & ~L1D_FLUSH) || !kvm_cpuid_host_has(7, 3, 28)) {
            return KVM_MSR_INVAL;
        }
    }
    if (value) {
        (void)kcpu_wrmsr_safe(index, value);
    }
    return 0;
}

/**
 * Read or, with `write`, write `v`'s speculation control `index` for the
 * caller with `host` or the guest: SPEC_CTRL, which the backend loads
 * around each run and every access of which exits, PRED_CMD and FLUSH_CMD,
 * which only take a write, and SVM's VIRT_SPEC_CTRL, whose SSBD the backend
 * carries out while the guest runs through the mechanism the host has:
 * SPEC_CTRL, the host's own VIRT_SPEC_CTRL or LS_CFG, and skips on a host
 * with none, as Linux's x86_virt_spec_ctrl. As Linux's svm_get_msr,
 * svm_set_msr (svm/svm.c:2987-3002, 3120-3156) and vmx's: a guest's access
 * needs the CPUID that offers the register, and SPEC_CTRL takes a value the
 * host's register takes. 0, KVM_MSR_INVAL, or 1 when `index` is none of
 * them, or PRED_CMD or FLUSH_CMD read.
 */
static int spec_msr(struct kvm_vcpu *v, uint32_t index, int write, uint64_t *value, int host)
{
    uint64_t *reg = &v->spec_ctrl;

    if (index == MSR_IA32_PRED_CMD || index == MSR_IA32_FLUSH_CMD) {
        return write ? command_set(v, index, *value, host) : 1;
    }
    if (index == MSR_IA32_SPEC_CTRL) {
        if ((!host && !guest_spec_ctrl(v)) || (write && !spec_ctrl_ok(*value))) {
            return KVM_MSR_INVAL;
        }
    } else if (index == MSR_AMD64_VIRT_SPEC && kvm_backend == &kvm_svm) {
        if ((!host && !guest_spec(v, 1, CPUID_X8_VIRT_SSBD)) ||
            (write && (*value & ~SPEC_CTRL_SSBD))) {
            return KVM_MSR_INVAL;
        }
        reg = &v->virt_spec_ctrl;
    } else {
        return 1;
    }
    if (!write) {
        *value = *reg;
        return 0;
    }
    *reg = *value;
    v->dirty |= KVM_PART_MSRS;
    return 0;
}

int kvm_msr_get(struct kvm_vcpu *v, uint32_t index, uint64_t *value, int host)
{
    uint32_t bank, reg;
    int r;

    r = kvm_lapic_msr(v, index, 0, value, host);
    if (r == 1) {
        r = kvm_hv_msr(v, index, 0, value, host);
    }
    if (r == 1) {
        r = kvm_pv_msr(v, index, 0, value, host);
    }
    if (r == 1) {
        r = kvm_pmu_msr(v, index, 0, value, 1);
    }
    if (r == 1) {
        r = spec_msr(v, index, 0, value, host);
    }
    if (r != 1) {
        return r;
    }
    switch (index) {
    case MSR_AMD64_SYSCFG:
    case MSR_AMD64_NB_CFG:
    case MSR_IA32_P5_MC_ADDR:
    case MSR_IA32_P5_MC_TYPE:
        *value = 0;
        return 0;
    case MSR_IA32_TSC:
        *value = kvm_tsc_read(v);
        return 0;
    case MSR_MTRRCAP:
        *value = MTRRCAP_VAL;
        return 0;
    case MSR_IA32_MCG_CAP:
        *value = v->mcg_cap;
        return 0;
    case MSR_IA32_MCG_STATUS:
        *value = v->mcg_status;
        return 0;
    case MSR_IA32_MCG_CTL:
        /* The caller reads it whatever MCG_CAP says, as Linux's
         * get_msr_mce; the guest only with MCG_CTL_P. */
        if (!(v->mcg_cap & MCG_CTL_P) && !host) {
            return KVM_MSR_INVAL;
        }
        *value = v->mcg_ctl;
        return 0;
    case MSR_PLATFORM_INFO:
        *value = v->platform_info;
        return 0;
    case MSR_MISC_FEATURES:
        *value = v->misc_features;
        return 0;
    case MSR_IA32_POWER_CTL:
        *value = v->power_ctl;
        return 0;
    case MSR_K7_HWCR:
        *value = v->hwcr;
        return 0;
    case MSR_IA32_UCODE_REV:
        *value = v->ucode_rev;
        return 0;
    case MSR_IA32_XSS:
        if (!host && !guest_xsaves(v)) {
            return KVM_MSR_INVAL;
        }
        *value = 0;
        return 0;
    case MSR_IA32_DEBUGCTL:
        /* SVM's, which keeps no bit; VMX's is kvm_pmu_msr's. */
        *value = 0;
        return 0;
    default:
        break;
    }
    if (index - MSR_LASTBRANCH_FROM < N_LASTBRANCH && kvm_backend == &kvm_svm) {
        /* Linux's svm_get_msr without lbrv (svm/svm.c:2969-2980). */
        *value = 0;
        return 0;
    }
    r = mce_bank_msr(v, index, &bank, &reg);
    if (r != 0) {
        if (r < 0) {
            return KVM_MSR_INVAL;
        }
        *value = v->mce_banks[bank * 4u + reg];
        return 0;
    }
    r = mce_ctl2_msr(v, index);
    if (r != 0) {
        if (r < 0 || !host) {
            return KVM_MSR_INVAL;
        }
        *value = 0;
        return 0;
    }
    if (!table_msr(index)) {
        return KVM_MSR_UNKNOWN;
    }
    *value = table_value(v, index);
    return 0;
}

/**
 * Whether the caller or the guest may write `value` to the table's MSR
 * `index`, as Linux's checks judge it: PAT's eight types, the MTRRs' types
 * and reserved bits, a canonical address for the MSRs that hold one, and
 * no reserved bit, 63:32, of TSC_AUX. SYSCALL_MASK's reserved bits depend
 * on the vCPU and the backend, and `sfmask_ok` judges them.
 */
static int table_value_ok(uint32_t index, uint64_t value)
{
    int64_t s = (int64_t)value;

    switch (index) {
    case MSR_IA32_CR_PAT:
        return types_ok(value, 1);
    case MSR_MTRR_DEF_TYPE:
        return (value & ~0xCFFull) == 0 && mem_type_ok(value & 0xFFu);
    case MSR_LSTAR:
    case MSR_CSTAR:
    case MSR_KERNEL_GS_BASE:
    case MSR_IA32_SYSENTER_ESP:
    case MSR_IA32_SYSENTER_EIP:
        return (s >> 47) == 0 || (s >> 47) == -1;
    case MSR_TSC_AUX:
        return (value >> 32) == 0;
    default:
        break;
    }
    if (mtrr_fixed(index)) {
        return types_ok(value, 0);
    }
    if (index >= MSR_MTRR_PHYS_BASE0 && index < MSR_MTRR_PHYS_BASE0 + 2u * VAR_MTRRS) {
        if ((index - MSR_MTRR_PHYS_BASE0) % 2u == 0) {
            return mem_type_ok(value & 0xFFu) && (value & 0xF00u) == 0;
        }
        return (value & 0x7FFu) == 0;
    }
    return 1;
}

/**
 * Whether `v` takes `value` for SYSCALL_MASK from the caller, with `host`,
 * or from the guest, as Linux takes it. The processor refuses bits 63:32
 * with #GP. VMX loads the MSR into the processor only for a guest in
 * 64-bit mode with EFER.SCE, whose SYSCALL reads it, and refuses there the
 * value whose WRMSR fails (vmx/vmx.c:718-731, 1916-1925, x86.c:631-647);
 * otherwise it keeps any value, which vmx.c's load writes with a WRMSR that
 * may fail, as Linux's vmx_prepare_switch_to_guest leaves the processor's
 * value when its write fails (vmx/vmx.c:1302-1311). SVM keeps the caller's
 * value in the VMCB (svm/svm.c:3177-3178) and lets the guest's WRMSR reach
 * the processor (svm/svm.c:99), whose #GP the guest takes here.
 */
static int sfmask_ok(const struct kvm_vcpu *v, uint64_t value, int host)
{
    if ((value >> 32) == 0) {
        return 1;
    }
    if (kvm_backend == &kvm_svm) {
        return host;
    }
    return (v->sregs.efer & (EFER_LMA | EFER_SCE)) != (EFER_LMA | EFER_SCE);
}

/**
 * Write `value` to MSR_PLATFORM_INFO, MSR_MISC_FEATURES_ENABLES or
 * MISC_ENABLE of `v`, as Linux's kvm_set_msr_common: PLATFORM_INFO keeps
 * offering CPUID faulting while the guest has it on, which it turns on
 * only while it is offered; and with KVM_X86_QUIRK_MISC_ENABLE_NO_MWAIT
 * off MISC_ENABLE's MWAIT bit, which then decides CPUID's, changes only
 * for a guest with SSE3. 0, KVM_MSR_INVAL for a value refused, or 1 for
 * another MSR.
 */
static int feature_msr_set(struct kvm_vcpu *v, uint32_t index, uint64_t value)
{
    const struct kvm_cpuid_entry2 *e;
    int slot;

    switch (index) {
    case MSR_PLATFORM_INFO:
        if (!(value & PLATFORM_INFO_CPUID_FAULT) &&
            (v->misc_features & MISC_FEATURES_CPUID_FAULT)) {
            return KVM_MSR_INVAL;
        }
        v->platform_info = value;
        return 0;
    case MSR_MISC_FEATURES:
        if ((value & ~MISC_FEATURES_CPUID_FAULT) ||
            (value && !(v->platform_info & PLATFORM_INFO_CPUID_FAULT))) {
            return KVM_MSR_INVAL;
        }
        v->misc_features = value;
        return 0;
    case MSR_IA32_MISC_ENABLE:
        if (!KVM_QUIRK(v->vm, KVM_X86_QUIRK_MISC_ENABLE_NO_MWAIT) &&
            ((table_value(v, index) ^ value) & MISC_ENABLE_MWAIT)) {
            e = kvm_cpuid_find(v, 1, 0);
            if (!e || !(e->ecx & 1u)) {
                return KVM_MSR_INVAL;
            }
        }
        slot = table_slot(v, index);
        if (slot < 0) {
            return KVM_MSR_INVAL;
        }
        v->msr_value[slot] = value;
        return 0;
    default:
        return 1;
    }
}

/**
 * Write `value` to the machine-check MSR `index` of `v`, as Linux's
 * set_msr_mce: MCG_CTL 0 or all ones, and only with MCG_CTL_P but for the
 * caller's 0; a bank's CTL 0 or all ones but for bit 10 and bit 0, which
 * some guests clear; a bank's STATUS anything from the caller, but only 0
 * from the guest unless `mci_status_writable`; a bank's CTL2 0 from the
 * caller alone, as `mce_ctl2_msr` says. 0, KVM_MSR_INVAL, or 1 for
 * another MSR.
 */
static int mce_set(struct kvm_vcpu *v, uint32_t index, uint64_t value, int host)
{
    uint32_t bank, reg;
    int r;

    switch (index) {
    case MSR_IA32_MCG_STATUS:
        v->mcg_status = value;
        return 0;
    case MSR_IA32_MCG_CTL:
        if (!(v->mcg_cap & MCG_CTL_P) && (value || !host)) {
            return KVM_MSR_INVAL;
        }
        if (value != 0 && value != ~0ull) {
            return KVM_MSR_INVAL;
        }
        v->mcg_ctl = value;
        return 0;
    default:
        break;
    }
    r = mce_ctl2_msr(v, index);
    if (r != 0) {
        return r > 0 && host && value == 0 ? 0 : KVM_MSR_INVAL;
    }
    r = mce_bank_msr(v, index, &bank, &reg);
    if (r <= 0) {
        return r < 0 ? KVM_MSR_INVAL : 1;
    }
    if (reg == 0 && value != 0 && (value | (1ull << 10) | 1ull) != ~0ull) {
        return KVM_MSR_INVAL;
    }
    if (reg == 1 && !host && value != 0 && !mci_status_writable(v)) {
        return KVM_MSR_INVAL;
    }
    v->mce_banks[bank * 4u + reg] = value;
    return 0;
}

int kvm_msr_set(struct kvm_vcpu *v, uint32_t index, uint64_t value, int host)
{
    int slot;

    slot = kvm_lapic_msr(v, index, 1, &value, host);
    if (slot == 1) {
        slot = kvm_hv_msr(v, index, 1, &value, host);
    }
    if (slot == 1) {
        slot = kvm_pv_msr(v, index, 1, &value, host);
    }
    if (slot == 1) {
        slot = kvm_pmu_msr(v, index, 1, &value, 1);
    }
    if (slot == 1) {
        slot = spec_msr(v, index, 1, &value, host);
    }
    if (slot != 1) {
        return slot;
    }
    slot = feature_msr_set(v, index, value);
    if (slot == 1) {
        slot = mce_set(v, index, value, host);
    }
    if (slot != 1) {
        if (slot == 0) {
            v->dirty |= KVM_PART_MSRS;
        }
        return slot;
    }
    switch (index) {
    case MSR_AMD64_NB_CFG:
        return 0;
    case MSR_IA32_XSS:
        /* No supervisor state is offered, so 0 is its only value, as
         * Linux 6.12's kvm_caps.supported_xss; Linux lists it with
         * neither the MSRs to save nor the emulated ones. */
        return (host || guest_xsaves(v)) && value == 0 ? 0 : KVM_MSR_INVAL;
    case MSR_IA32_DEBUGCTL:
        /* SVM's, which does not virtualize the LBR stack: any write, the
         * caller's or the guest's, is taken and ignored, as Linux's
         * svm_set_msr without lbrv (arch/x86/kvm/svm/svm.c:3223-3227). */
        return 0;
    case MSR_IA32_UCODE_REV:
        v->ucode_rev = value;
        break;
    case MSR_IA32_TSC:
        kvm_tsc_sync(v, &value);
        return 0;
    case MSR_IA32_MCG_CAP:
        /* Set only by KVM_X86_SETUP_MCE: Linux's kvm_set_msr_common has
         * no case for it, and no list advertises it, so the caller's write
         * of any value is an unknown MSR's as much as the guest's. */
        return KVM_MSR_UNKNOWN;
    case MSR_IA32_POWER_CTL:
        v->power_ctl = value;
        break;
    case MSR_K7_HWCR:
        /* Kept for every vendor, as Linux keeps it; a write of a bit HWCR
         * does not offer is refused, of one it ignores dropped. */
        value &= ~HWCR_IGNORED;
        if (value & ~HWCR_VALID) {
            return KVM_MSR_INVAL;
        }
        v->hwcr = value;
        break;
    default:
        if (index - MSR_LASTBRANCH_FROM < N_LASTBRANCH && kvm_backend == &kvm_svm) {
            /* svm_set_msr without lbrv has no such MSR: Linux takes only
             * the caller's 0 of a listed one, and any other write is
             * unsupported, an UNKNOWN exit for the guest's
             * (svm/svm.c:3259-3290, x86.c:508-518, 1590). */
            return host && value == 0 ? 0 : KVM_MSR_UNKNOWN;
        }
        if (!table_msr(index)) {
            return KVM_MSR_UNKNOWN;
        }
        if (!table_value_ok(index, value) ||
            (index == MSR_SYSCALL_MASK && !sfmask_ok(v, value, host))) {
            return KVM_MSR_INVAL;
        }
        slot = table_slot(v, index);
        if (slot < 0) {
            return KVM_MSR_INVAL;
        }
        v->msr_value[slot] = value;
        break;
    }
    v->dirty |= KVM_PART_MSRS;
    return 0;
}

/* ---- the filter and the caller's exits ---- */

/* One range of a filter, as KVM_X86_SET_MSR_FILTER gave it, with its own
 * copy of the bitmap. */
struct msr_range {
    uint32_t flags; /* KVM_MSR_FILTER_READ and KVM_MSR_FILTER_WRITE */
    uint32_t base;
    uint32_t nmsrs;
    uint8_t *bitmap; /* a bit for each MSR from `base`, 1 to allow */
};

/* A VM's filter, `vm->msr_filter`: the ranges with MSRs, in the order
 * given, of which the first that names an access decides it. A vCPU reads
 * it under `filter_lock`, which a new filter takes the old one's place
 * under, as Linux's readers hold kvm->srcu and kvm_vm_ioctl_set_msr_filter
 * frees the old filter after synchronize_srcu (arch/x86/kvm/x86.c:6416). */
struct msr_filter {
    int default_allow;
    uint32_t count;
    struct msr_range ranges[KVM_MSR_FILTER_MAX_RANGES];
};

int kvm_msr_allowed(const struct kvm_vcpu *v, uint32_t index, int write)
{
    struct kvm_vm *vm = v->vm;
    const struct msr_filter *f;
    uint32_t type = write ? KVM_MSR_FILTER_WRITE : KVM_MSR_FILTER_READ;
    uint32_t i, end, flags;
    int allowed;

    if ((index >= MSR_X2APIC_FIRST && index <= MSR_X2APIC_LAST) || !vm->msr_filter) {
        return 1;
    }
    flags = spin_lock_irqsave(&vm->filter_lock);
    f = (const struct msr_filter *)vm->msr_filter;
    allowed = f ? f->default_allow : 1;
    for (i = 0; f && i < f->count; i++) {
        /* `end` wraps for a range past 0xFFFFFFFF, as Linux's does. */
        end = f->ranges[i].base + f->ranges[i].nmsrs;
        if (index >= f->ranges[i].base && index < end && (f->ranges[i].flags & type)) {
            index -= f->ranges[i].base;
            allowed = (f->ranges[i].bitmap[index / 8u] >> (index % 8u)) & 1u;
            break;
        }
    }
    spin_unlock_irqrestore(&vm->filter_lock, flags);
    return allowed;
}

uint32_t kvm_msr_user_reason(const struct kvm_vcpu *v, int r)
{
    uint32_t reason = KVM_MSR_EXIT_REASON_INVAL;

    if (r == KVM_MSR_UNKNOWN) {
        reason = KVM_MSR_EXIT_REASON_UNKNOWN;
    } else if (r == KVM_MSR_FILTERED) {
        reason = KVM_MSR_EXIT_REASON_FILTER;
    }
    return (v->vm->user_space_msr & reason) ? reason : 0;
}

int kvm_msr_user_exit(struct kvm_vcpu *v, uint32_t index, int write, uint64_t value, int r,
                      uint32_t len)
{
    uint32_t reason = kvm_msr_user_reason(v, r);

    if (!reason) {
        return 0;
    }
    v->exit.kind = KVM_X_MSR;
    v->exit.msr.write = write;
    v->exit.msr.index = index;
    v->exit.msr.reason = reason;
    v->exit.msr.len = len;
    v->exit.msr.data = write ? value : 0;
    return 1;
}

void kvm_msr_complete(struct kvm_vcpu *v)
{
    const struct kvm_run_msr *m = &v->run->u.msr;

    if (!v->msr_pending) {
        return;
    }
    v->msr_pending = 0;
    if (m->error) {
        kvm_queue_exception(v, GP_VECTOR, 1, 0);
        return;
    }
    if (!v->exit.msr.write) {
        v->regs.rax = m->data & 0xFFFFFFFFull;
        v->regs.rdx = m->data >> 32;
    }
    kvm_emul_skip(v, v->exit.msr.len);
    v->dirty |= KVM_PART_REGS;
}

/** Free the filter `f`, which may be NULL, with its bitmaps. */
static void filter_free(struct msr_filter *f)
{
    uint32_t i;

    if (!f) {
        return;
    }
    for (i = 0; i < f->count; i++) {
        kfree(f->ranges[i].bitmap);
    }
    kfree(f);
}

/**
 * Add the caller's range `u` to `f`, as Linux's kvm_add_msr_filter: a
 * range of no MSRs is left out; one with no flag or an unknown one, or
 * whose bitmap, rounded up to whole longs as Linux copies it, passes
 * KVM_MSR_FILTER_MAX_BITMAP_SIZE, is -EINVAL. 0, -ENOMEM or -EFAULT.
 */
static int filter_add(struct msr_filter *f, const struct kvm_msr_filter_range *u)
{
    struct msr_range *r;
    uint32_t bytes;

    if (!u->nmsrs) {
        return 0;
    }
    if (!u->flags || (u->flags & ~KVM_MSR_FILTER_RANGE_VALID_MASK)) {
        return -EINVAL;
    }
    bytes = (u->nmsrs + 63u) / 64u * 8u;
    if (!bytes || bytes > KVM_MSR_FILTER_MAX_BITMAP_SIZE) {
        return -EINVAL;
    }
    r = &f->ranges[f->count];
    r->bitmap = (uint8_t *)kmalloc(bytes);
    if (!r->bitmap) {
        return -ENOMEM;
    }
    if (copy_from_user(r->bitmap, (const void __user *)(uintptr_t)u->bitmap, bytes) != 0) {
        kfree(r->bitmap);
        r->bitmap = NULL;
        return -EFAULT;
    }
    r->flags = u->flags;
    r->base = u->base;
    r->nmsrs = u->nmsrs;
    f->count++;
    return 0;
}

int32_t kvm_msr_set_filter(struct kvm_vm *vm, uintptr_t arg)
{
    struct kvm_msr_filter *u;
    struct msr_filter *f, *old;
    uint32_t i, flags;
    int empty = 1, r = 0;

    u = (struct kvm_msr_filter *)kmalloc(sizeof(*u));
    if (!u) {
        return -ENOMEM;
    }
    if (copy_from_user(u, (const void __user *)arg, sizeof(*u)) != 0) {
        kfree(u);
        return -EFAULT;
    }
    for (i = 0; i < KVM_MSR_FILTER_MAX_RANGES; i++) {
        empty &= u->ranges[i].nmsrs == 0;
    }
    if ((u->flags & ~KVM_MSR_FILTER_VALID_MASK) ||
        (empty && (u->flags & KVM_MSR_FILTER_DEFAULT_DENY))) {
        kfree(u);
        return -EINVAL;
    }
    f = (struct msr_filter *)kzalloc(sizeof(*f));
    if (!f) {
        kfree(u);
        return -ENOMEM;
    }
    f->default_allow = !(u->flags & KVM_MSR_FILTER_DEFAULT_DENY);
    for (i = 0; i < KVM_MSR_FILTER_MAX_RANGES && r == 0; i++) {
        r = filter_add(f, &u->ranges[i]);
    }
    kfree(u);
    if (r != 0) {
        filter_free(f);
        return r;
    }
    flags = spin_lock_irqsave(&vm->filter_lock);
    old = (struct msr_filter *)vm->msr_filter;
    vm->msr_filter = f;
    spin_unlock_irqrestore(&vm->filter_lock, flags);
    filter_free(old);
    for (i = 0; i < vm->nr_vcpus; i++) {
        kvm_vcpu_kick(vm->vcpus[i], KVM_REQ_MSR_FILTER);
    }
    return 0;
}

void kvm_msr_free_filter(struct kvm_vm *vm)
{
    filter_free((struct msr_filter *)vm->msr_filter);
    vm->msr_filter = NULL;
}
