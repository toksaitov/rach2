/*
 * svm.c: AMD's SVM backend: a stub, which finds SVM and refuses to enter a
 * guest, until the SVM backend replaces it.
 */

#include "kvm.h"

#include "../arch/x86/kcpu.h"
#include "../lib/kerrno.h"

/* VM_CR, whose SVMDIS bit the firmware sets to turn SVM off. */
#define MSR_VM_CR    0xC0010114u
#define VM_CR_SVMDIS 0x10u

/* The guest's entry, svm_run.s. */
int kvm_svm_vmrun(void *vcpu);

/** Whether the processor has SVM, leaf 0x8000000A among its CPUID, and
 * the firmware left it on. */
static int svm_probe(void)
{
    uint32_t a = 0, b = 0, c = 0, d = 0, lo = 0, hi = 0;

    kcpu_id(0x80000000u, &a, &b, &c, &d);
    if (a < 0x8000000Au) {
        return 0;
    }
    kcpu_id(0x80000001u, &a, &b, &c, &d);
    if (!(c & (1u << 2))) {
        return 0;
    }
    kcpu_rdmsr(MSR_VM_CR, &lo, &hi);
    return !(lo & VM_CR_SVMDIS);
}

/** The entry, which the stub refuses: -EOPNOTSUPP. */
static int svm_enter(struct kvm_vcpu *v)
{
    return kvm_svm_vmrun(v);
}

/** The bits of a memory type in a 4 KiB leaf: PCD and PWT for UC, PWT for
 * WT, none for WB, with the guest's PAT combined with them. */
static uint64_t svm_cache(int type, int ignore_guest_pat)
{
    (void)ignore_guest_pat;
    return type == KVM_MT_UC ? 0x18u : 0u;
}

/* The long-mode format: P, R/W, U/S, PS, A, D and NX. */
static const struct kvm_tdp_format svm_tdp = {
    0x1u, 0x1u, 0x2u, 0, 1ull << 63, 0x4u, 0x80u, 0x000FFFFFFFFFF000ull, 0x20u, 0x40u, svm_cache};

const struct kvm_backend kvm_svm = {"svm", svm_probe, NULL, NULL, NULL, NULL,    svm_enter,
                                    NULL,  NULL,      NULL, NULL, NULL, &svm_tdp};
