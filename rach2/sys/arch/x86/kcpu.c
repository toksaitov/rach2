/*
 * kcpu.c: `cpuid` and the model-specific register instructions, as kcpu.h
 * describes them.
 */

#include "kcpu.h"

void kcpu_id(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    kcpu_id_sub(leaf, 0, a, b, c, d);
}

/* A host test gives its own `cpuid`, `rdmsr` and `wrmsr`. */
#ifndef RACH2_HOSTTEST
void kcpu_id_sub(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    uint32_t ra = leaf, rb = 0, rc = sub, rd = 0;

    __asm__ __volatile__("cpuid" : "+a"(ra), "=b"(rb), "+c"(rc), "=d"(rd) : : "memory");
    if (a) {
        *a = ra;
    }
    if (b) {
        *b = rb;
    }
    if (c) {
        *c = rc;
    }
    if (d) {
        *d = rd;
    }
}

#endif /* RACH2_HOSTTEST */

int kcpu_msr_present(void)
{
    uint32_t a = 0, d = 0;

    kcpu_id(0, &a, 0, 0, 0);
    if (a < 1) {
        return 0;
    }
    kcpu_id(1, 0, 0, 0, &d);
    return (d & (1u << 5)) != 0;
}

int kcpu_apic_present(void)
{
    uint32_t a = 0, d = 0;

    kcpu_id(0, &a, 0, 0, 0);
    if (a < 1) {
        return 0;
    }
    kcpu_id(1, 0, 0, 0, &d);
    return (d & (1u << 9)) != 0;
}

#ifndef RACH2_HOSTTEST
void kcpu_rdmsr(uint32_t msr, uint32_t *lo, uint32_t *hi)
{
    uint32_t l = 0, h = 0;

    __asm__ __volatile__("rdmsr" : "=a"(l), "=d"(h) : "c"(msr));
    if (lo) {
        *lo = l;
    }
    if (hi) {
        *hi = h;
    }
}

void kcpu_wrmsr(uint32_t msr, uint32_t lo, uint32_t hi)
{
    __asm__ __volatile__("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}
#endif /* RACH2_HOSTTEST */

int kcpu_x2apic_present(void)
{
    uint32_t a = 0, c = 0;

    kcpu_id(0, &a, 0, 0, 0);
    if (a < 1) {
        return 0;
    }
    kcpu_id(1, 0, 0, &c, 0);
    return (c & (1u << 21)) != 0;
}

/** Leaf 0xB describes a level when subleaf 0's EBX counts its processors,
 * as Linux's topo_subleaf asks (arch/x86/kernel/cpu/topology_ext.c, line
 * 75); its EDX is then the x2APIC identity whole (SDM, CPUID leaf 0BH). */
uint32_t kcpu_x2apic_id(void)
{
    uint32_t a = 0, b = 0, d = 0;

    kcpu_id(0, &a, 0, 0, 0);
    if (a >= 0xBu) {
        kcpu_id_sub(0xBu, 0, 0, &b, 0, &d);
        if ((b & 0xFFFFu) != 0) {
            return d;
        }
    }
    if (a < 1) {
        return 0;
    }
    kcpu_id(1, 0, &b, 0, 0);
    return b >> 24;
}

/**
 * The first hypervisor leaf, from 0x40000000 in steps of 0x100, whose
 * signature is the 12 bytes `sig` and that has at least `leaves` leaves
 * past it, or 0: Linux's hypervisor_cpuid_base
 * (arch/x86/include/asm/cpuid.h, lines 190 to 211).
 */
static uint32_t hypervisor_base(const char *sig, uint32_t leaves)
{
    uint32_t base, r[4];
    int i;

    for (base = 0x40000000u; base < 0x40010000u; base += 0x100u) {
        kcpu_id(base, &r[0], &r[1], &r[2], &r[3]);
        for (i = 0; i < 12; i++) {
            if ((char)((r[1 + i / 4] >> (8 * (i % 4))) & 0xFFu) != sig[i]) {
                break;
            }
        }
        if (i == 12 && (leaves == 0 || r[0] - base >= leaves)) {
            return base;
        }
    }
    return 0;
}

/**
 * Linux 6.12.115 asks each hypervisor it found through
 * `x86_init.hyper.x2apic_available`: KVM's is kvm_para_available
 * (arch/x86/kernel/kvm.c, lines 902 to 926 and 1058), the KVM signature at
 * any base; Hyper-V's is the processor's x2APIC bit (mshyperv.c, lines 653
 * to 656), once ms_hyperv_platform has found its signature at 0x40000000
 * with 0x40000005 to 0x4000FFFF leaves and the hypercall and VP index MSRs
 * offered (lines 285 to 310); Xen HVM's is the same bit (enlighten_hvm.c,
 * lines 255 to 258) under the Xen signature with two leaves. Bare metal's
 * is false. VMware's needs its backdoor hypercall and is not asked.
 */
int kcpu_x2apic_unremapped(void)
{
    uint32_t c = 0, a = 0, b = 0, d = 0;

    if (!kcpu_x2apic_present()) {
        return 0;
    }
    kcpu_id(1, 0, 0, &c, 0);
    if ((c & (1u << 31)) == 0) {
        return 0; /* no hypervisor beneath */
    }
    if (hypervisor_base("KVMKVMKVM\0\0\0", 0) != 0 || hypervisor_base("XenVMMXenVMM", 2) != 0) {
        return 1;
    }
    kcpu_id(0x40000000u, &a, &b, &c, &d);
    if (a >= 0x40000005u && a <= 0x4000FFFFu && b == 0x7263694Du && c == 0x666F736Fu &&
        d == 0x76482074u) { /* "Microsoft Hv" */
        kcpu_id(0x40000003u, &a, 0, 0, 0);
        return (a & (3u << 5)) == (3u << 5);
    }
    return 0;
}

/**
 * The leaf after the base that holds KVM's signature, which may sit at any
 * base from 0x40000000 when another hypervisor's leaves come first: its EAX
 * in `*a` and EDX in `*d`, both 0 without KVM beneath.
 */
static void kvm_leaf(uint32_t *a, uint32_t *d)
{
    uint32_t c = 0, base;

    *a = 0;
    *d = 0;
    kcpu_id(1, 0, 0, &c, 0);
    if ((c & (1u << 31)) == 0) {
        return; /* no hypervisor beneath */
    }
    base = hypervisor_base("KVMKVMKVM\0\0\0", 0);
    if (base != 0) {
        kcpu_id(base + 1u, a, 0, 0, d);
    }
}

uint32_t kcpu_kvm_features(void)
{
    uint32_t a, d;

    kvm_leaf(&a, &d);
    return a;
}

uint32_t kcpu_kvm_hints(void)
{
    uint32_t a, d;

    kvm_leaf(&a, &d);
    return d;
}

/**
 * Linux's x2apic_hw_locked (arch/x86/kernel/apic/apic.c, lines 1693 to
 * 1704): IA32_ARCH_CAPABILITIES, which leaf 7's EDX bit 29 offers, has
 * XAPIC_DISABLE (bit 21), and IA32_XAPIC_DISABLE_STATUS (0xBD) has bit 0.
 */
int kcpu_x2apic_locked(void)
{
    uint32_t a = 0, d = 0, lo = 0;

    kcpu_id(0, &a, 0, 0, 0);
    if (a < 7) {
        return 0;
    }
    kcpu_id_sub(7, 0, 0, 0, 0, &d);
    if ((d & (1u << 29)) == 0) {
        return 0;
    }
    kcpu_rdmsr(0x10Au, &lo, 0);
    if ((lo & (1u << 21)) == 0) {
        return 0;
    }
    kcpu_rdmsr(0xBDu, &lo, 0);
    return (lo & 1u) != 0;
}
