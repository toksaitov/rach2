/*
 * msr.c: the MSRs of a vCPU that KVM_GET_MSRS and KVM_SET_MSRS move, the
 * index lists of /dev/kvm, and the feature MSRs.
 */

#include "kvm.h"

#include "../arch/arch.h"
#include "../arch/x86/kcpu.h"

#define MSR_IA32_TSC           0x010u
#define MSR_IA32_TSC_ADJUST    0x03Bu
#define MSR_IA32_UCODE_REV     0x08Bu
#define MSR_MTRRCAP            0x0FEu
#define MSR_IA32_SYSENTER_CS   0x174u
#define MSR_IA32_SYSENTER_ESP  0x175u
#define MSR_IA32_SYSENTER_EIP  0x176u
#define MSR_IA32_MCG_CAP       0x179u
#define MSR_IA32_MCG_STATUS    0x17Au
#define MSR_IA32_MCG_CTL       0x17Bu
#define MSR_IA32_MISC_ENABLE   0x1A0u
#define MSR_MTRR_PHYS_BASE0    0x200u
#define MSR_MTRR_FIX_64K       0x250u
#define MSR_MTRR_FIX_16K_80000 0x258u
#define MSR_MTRR_FIX_16K_A0000 0x259u
#define MSR_MTRR_FIX_4K_C0000  0x268u
#define MSR_IA32_CR_PAT        0x277u
#define MSR_IA32_TSC_DEADLINE  0x6E0u
#define MSR_MTRR_DEF_TYPE      0x2FFu
#define MSR_IA32_MC0_CTL       0x400u
#define MSR_STAR               0xC0000081u
#define MSR_LSTAR              0xC0000082u
#define MSR_CSTAR              0xC0000083u
#define MSR_SYSCALL_MASK       0xC0000084u
#define MSR_KERNEL_GS_BASE     0xC0000102u
#define MSR_TSC_AUX            0xC0000103u

/* The variable-range MTRRs a guest has, and MTRRcap's value: WC and the
 * fixed ranges with that count, as Linux's KVM_NR_VAR_MTRR gives. */
#define VAR_MTRRS   8u
#define MTRRCAP_VAL (0x500ull | VAR_MTRRS)

/* PAT at reset, and MISC_ENABLE with PEBS and BTS unavailable, as Linux
 * gives a new vCPU. */
#define PAT_RESET       0x0007040600070406ull
#define MISC_ENABLE_NEW 0x1800ull

/* The value of UCODE_REV a guest reads, Linux's. */
#define UCODE_REV_VAL 0x100000000ull

/* KVM_GET_MSR_INDEX_LIST: the MSRs a caller saves and restores, the TSC
 * deadline among them, which the local APIC keeps (vlapic.c), and Linux's
 * list of Hyper-V's, which hyperv.c keeps. TSC_AUX is last, and listed only
 * where the host has RDTSCP. */
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
                                      MSR_TSC_AUX};

static const uint32_t feature_list[] = {MSR_IA32_UCODE_REV};

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

const uint32_t *kvm_msr_list(int feature, uint32_t *n)
{
    if (feature) {
        *n = N_FEATURE;
        return feature_list;
    }
    *n = (uint32_t)N_INDEX - (host_rdtscp() ? 0u : 1u);
    return index_list;
}

int kvm_msr_feature(uint32_t index, uint64_t *value)
{
    if (index == MSR_IA32_UCODE_REV) {
        *value = UCODE_REV_VAL;
        return 0;
    }
    return -1;
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
 * list but TSC, MCG_STATUS, MCG_CTL and Hyper-V's, or an MTRR. */
static int table_msr(uint32_t index)
{
    uint32_t i;

    if (index == MSR_IA32_TSC || index == MSR_IA32_MCG_STATUS || index == MSR_IA32_MCG_CTL ||
        (index >> 12) == 0x40000u) {
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
}

/** Whether `index` is an MSR of a machine-check bank `v` has; `*bank`
 * and `*reg` get the bank and the register, 0 CTL to 3 MISC. */
static int mce_bank_msr(const struct kvm_vcpu *v, uint32_t index, uint32_t *bank, uint32_t *reg)
{
    uint32_t off;

    if (index < MSR_IA32_MC0_CTL) {
        return 0;
    }
    off = index - MSR_IA32_MC0_CTL;
    if (off / 4u >= (uint32_t)(v->mcg_cap & 0xFFu)) {
        return 0;
    }
    *bank = off / 4u;
    *reg = off % 4u;
    return 1;
}

int kvm_msr_get(struct kvm_vcpu *v, uint32_t index, uint64_t *value)
{
    uint32_t bank, reg, i;
    int r;

    r = kvm_lapic_msr(v, index, 0, value, 1);
    if (r == 1) {
        r = kvm_hv_msr(v, index, 0, value, 1);
    }
    if (r != 1) {
        return r;
    }
    switch (index) {
    case MSR_IA32_TSC:
        *value = arch_cycles() + (uint64_t)v->tsc_offset;
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
        if (!(v->mcg_cap & MCG_CTL_P)) {
            return -1;
        }
        *value = v->mcg_ctl;
        return 0;
    default:
        break;
    }
    if (mce_bank_msr(v, index, &bank, &reg)) {
        *value = v->mce_banks[bank * 4u + reg];
        return 0;
    }
    if (!table_msr(index)) {
        return -1;
    }
    *value = 0;
    for (i = 0; i < v->msr_count; i++) {
        if (v->msr_index[i] == index) {
            *value = v->msr_value[i];
        }
    }
    return 0;
}

/**
 * Whether the caller may write `value` to the table's MSR `index`, as
 * Linux's checks of a write from the host judge it: PAT's eight types,
 * the MTRRs' types and reserved bits, and a canonical address for the
 * MSRs that hold one.
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

int kvm_msr_set(struct kvm_vcpu *v, uint32_t index, uint64_t value)
{
    uint32_t bank, reg;
    int slot;

    slot = kvm_lapic_msr(v, index, 1, &value, 1);
    if (slot == 1) {
        slot = kvm_hv_msr(v, index, 1, &value, 1);
    }
    if (slot != 1) {
        return slot;
    }
    switch (index) {
    case MSR_IA32_TSC:
        v->tsc_offset = (int64_t)(value - arch_cycles());
        v->dirty |= KVM_PART_TSC;
        kvm_hv_clock_changed(v->vm);
        return 0;
    case MSR_IA32_MCG_CAP:
        if (value & ~(MCG_CTL_P | MCG_SER_P | 0xFFull | 0xFF0000ull) ||
            (value & 0xFFu) > KVM_MAX_MCE_BANKS) {
            return -1;
        }
        v->mcg_cap = value;
        break;
    case MSR_IA32_MCG_STATUS:
        v->mcg_status = value;
        break;
    case MSR_IA32_MCG_CTL:
        if (!(v->mcg_cap & MCG_CTL_P) && value) {
            return -1;
        }
        if (value != 0 && value != ~0ull) {
            return -1;
        }
        v->mcg_ctl = value;
        break;
    default:
        if (mce_bank_msr(v, index, &bank, &reg)) {
            /* A bank's CTL is 0 or all ones, but for bit 10 and bit 0,
             * which some guests clear. */
            if (reg == 0 && value != 0 && (value | (1ull << 10) | 1ull) != ~0ull) {
                return -1;
            }
            v->mce_banks[bank * 4u + reg] = value;
            break;
        }
        if (!table_msr(index) || !table_value_ok(index, value)) {
            return -1;
        }
        slot = table_slot(v, index);
        if (slot < 0) {
            return -1;
        }
        v->msr_value[slot] = value;
        break;
    }
    v->dirty |= KVM_PART_MSRS;
    return 0;
}
