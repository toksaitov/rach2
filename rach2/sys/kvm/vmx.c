/*
 * vmx.c: Intel's VMX backend: a stub, which finds VMX and refuses to enter
 * a guest, until the VMX backend replaces it.
 */

#include "kvm.h"

#include "../arch/x86/kcpu.h"
#include "../lib/kerrno.h"

/* IA32_FEATURE_CONTROL: its lock and the enable of VMX outside SMX. */
#define MSR_IA32_FEATURE_CONTROL 0x3Au
#define FC_LOCKED                0x1u
#define FC_VMX_OUTSIDE_SMX       0x4u

/* The guest's entry, vmx_run.s. */
int kvm_vmx_vmrun(void *vcpu);

/** Whether the processor has VMX and the firmware has not locked it off:
 * IA32_FEATURE_CONTROL locked with VMX outside SMX on, or not locked, when
 * the backend may turn it on. */
static int vmx_probe(void)
{
    uint32_t a = 0, b = 0, c = 0, d = 0, lo = 0, hi = 0;

    kcpu_id(1, &a, &b, &c, &d);
    if (!(c & (1u << 5))) {
        return 0;
    }
    kcpu_rdmsr(MSR_IA32_FEATURE_CONTROL, &lo, &hi);
    return !(lo & FC_LOCKED) || (lo & FC_VMX_OUTSIDE_SMX);
}

/** The entry, which the stub refuses: -EOPNOTSUPP. */
static int vmx_enter(struct kvm_vcpu *v)
{
    return kvm_vmx_vmrun(v);
}

/** The bits of a memory type in a leaf: the type in bits 3 to 5, and IPAT,
 * bit 6, when the guest's PAT is ignored. */
static uint64_t vmx_cache(int type, int ignore_guest_pat)
{
    return ((uint64_t)type << 3) | (ignore_guest_pat ? 0x40u : 0u);
}

/* EPT's format: read, write and execute, PS, and the accessed and dirty
 * bits of EPT's A/D. */
static const struct kvm_tdp_format vmx_tdp = {
    0x7u, 0x1u, 0x2u, 0x4u, 0, 0, 0x80u, 0x000FFFFFFFFFF000ull, 0x100u, 0x200u, vmx_cache};

const struct kvm_backend kvm_vmx = {"vmx", vmx_probe, NULL, NULL, NULL, NULL,    vmx_enter,
                                    NULL,  NULL,      NULL, NULL, NULL, &vmx_tdp};
