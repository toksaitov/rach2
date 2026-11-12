/**
 * kcpu_hosttest.c: the processor detection the local APIC's mode rests on
 * (`rach2/sys/arch/x86/kcpu.c`), over a modelled `cpuid` and the MSRs it
 * reads.
 *
 * Rows: the x2APIC identity from leaf 0xB, and from leaf 1 where leaf 0xB
 * is absent or describes no level; a hypervisor that allows x2APIC mode
 * unremapped, as Linux 6.12.115's x2apic_available answers: KVM at
 * 0x40000000 and at 0x40000100 behind a Hyper-V leaf, Hyper-V with and
 * without the VP index MSR and with too few leaves, Xen with two leaves and
 * with one, VMware, and none on bare metal or without the processor's
 * x2APIC bit; and a firmware's lock of x2APIC mode, which takes leaf 7's
 * bit, IA32_ARCH_CAPABILITIES' bit and IA32_XAPIC_DISABLE_STATUS together.
 * Each row fails when the code reads a wrong leaf, bound, register order or
 * byte order. Prints each failure; the exit status is the number of failed
 * checks, at most 255.
 */

#include "../../rach2/sys/arch/x86/kcpu.c"

extern int printf(const char *fmt, ...);

static int fails;

/** Count and print a failure when `cond` is false; `what` says what held. */
#define CHECK(cond, what)                 \
    do {                                  \
        if (!(cond)) {                    \
            printf("  FAIL: %s\n", what); \
            fails++;                      \
        }                                 \
    } while (0)

/* The modelled processor: up to 16 leaves, each with its subleaf and four
 * registers; a leaf not listed answers zeros. IA32_ARCH_CAPABILITIES
 * (0x10A) and IA32_XAPIC_DISABLE_STATUS (0xBD) are the two MSRs read. */
static struct {
    uint32_t leaf, sub, r[4];
} leaves[16];
static int nleaves;
static uint32_t arch_cap, xapic_disable_status;

/** Store `v` through `p` unless `p` is NULL. */
static void put(uint32_t *p, uint32_t v)
{
    if (p) {
        *p = v;
    }
}

/** `cpuid` of the model: the listed leaf and subleaf, else zeros. */
void kcpu_id_sub(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    uint32_t r[4] = {0, 0, 0, 0};
    int i, j;

    for (i = 0; i < nleaves; i++) {
        if (leaves[i].leaf == leaf && leaves[i].sub == sub) {
            for (j = 0; j < 4; j++) {
                r[j] = leaves[i].r[j];
            }
            break;
        }
    }
    put(a, r[0]);
    put(b, r[1]);
    put(c, r[2]);
    put(d, r[3]);
}

/** `rdmsr` of the model's two registers; any other reads 0. */
void kcpu_rdmsr(uint32_t msr, uint32_t *lo, uint32_t *hi)
{
    put(lo, msr == 0x10Au ? arch_cap : msr == 0xBDu ? xapic_disable_status : 0);
    put(hi, 0);
}

/** `wrmsr`, which the detection never makes. */
void kcpu_wrmsr(uint32_t msr, uint32_t lo, uint32_t hi)
{
    (void)msr;
    (void)lo;
    (void)hi;
    fails++;
    printf("  FAIL: detection wrote an MSR\n");
}

/** Set leaf `leaf`, subleaf `sub`, to the four registers. */
static void set_leaf(uint32_t leaf, uint32_t sub, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    leaves[nleaves].leaf = leaf;
    leaves[nleaves].sub = sub;
    leaves[nleaves].r[0] = a;
    leaves[nleaves].r[1] = b;
    leaves[nleaves].r[2] = c;
    leaves[nleaves].r[3] = d;
    nleaves++;
}

/** The 12 bytes of `sig` as EBX, ECX and EDX hold them, low byte first. */
static uint32_t sig_word(const char *sig, int n)
{
    return (uint32_t)(unsigned char)sig[4 * n] | (uint32_t)(unsigned char)sig[4 * n + 1] << 8 |
           (uint32_t)(unsigned char)sig[4 * n + 2] << 16 |
           (uint32_t)(unsigned char)sig[4 * n + 3] << 24;
}

/** Set hypervisor leaf `base` to the signature `sig`, its highest leaf
 * `top` in EAX. */
static void set_hypervisor(uint32_t base, const char *sig, uint32_t top)
{
    set_leaf(base, 0, top, sig_word(sig, 0), sig_word(sig, 1), sig_word(sig, 2));
}

/**
 * A processor with leaves 0 to `max` (leaf 0 and leaf 1 only), whose leaf
 * 1 has the APIC identity `apic` and ECX `ecx`: bit 21 for x2APIC mode,
 * bit 31 for a hypervisor beneath.
 */
static void reset(uint32_t max, uint32_t apic, uint32_t ecx)
{
    nleaves = 0;
    arch_cap = 0;
    xapic_disable_status = 0;
    set_leaf(0, 0, max, 0, 0, 0);
    set_leaf(1, 0, 0, apic << 24, ecx, 0);
}

#define X2APIC_BIT     (1u << 21)
#define HYPERVISOR_BIT (1u << 31)

/** The identity: leaf 0xB's EDX when the leaf describes a level, leaf 1's
 * EBX bits 31:24 when it does not or is past the highest leaf. */
static void rows_id(void)
{
    reset(0xB, 0x12, X2APIC_BIT);
    set_leaf(0xB, 0, 1, 2, 0x100, 0x1234);
    CHECK(kcpu_x2apic_id() == 0x1234, "leaf 0xB's EDX is the identity whole");

    reset(0xB, 0x12, X2APIC_BIT);
    set_leaf(0xB, 0, 0, 0, 0, 0x1234);
    CHECK(kcpu_x2apic_id() == 0x12, "a leaf 0xB that counts no processor gives way to leaf 1");

    reset(0xA, 0x12, X2APIC_BIT);
    set_leaf(0xB, 0, 1, 2, 0x100, 0x1234);
    CHECK(kcpu_x2apic_id() == 0x12, "leaf 0xB past the highest leaf is not read");
}

/** Bare metal and the processor's bit: no hypervisor allows the mode. */
static void rows_none(void)
{
    reset(0xB, 0, X2APIC_BIT);
    set_hypervisor(0x40000000u, "KVMKVMKVM\0\0\0", 0x40000001u);
    CHECK(!kcpu_x2apic_unremapped(),
          "a KVM signature without leaf 1's hypervisor bit is passed over");

    reset(0xB, 0, HYPERVISOR_BIT);
    set_hypervisor(0x40000000u, "KVMKVMKVM\0\0\0", 0x40000001u);
    CHECK(!kcpu_x2apic_unremapped(), "KVM without the processor's x2APIC bit does not allow it");

    reset(0xB, 0, X2APIC_BIT | HYPERVISOR_BIT);
    set_hypervisor(0x40000000u, "VMwareVMware", 0x40000010u);
    CHECK(!kcpu_x2apic_unremapped(), "VMware is not asked");

    reset(0xB, 0, X2APIC_BIT | HYPERVISOR_BIT);
    set_hypervisor(0x40000000u, "KVMKVMKVM\0\0\0" + 1, 0x40000001u);
    CHECK(!kcpu_x2apic_unremapped(), "a signature one byte off is not KVM's");
}

/** KVM's signature at any base, as kvm_cpuid_base searches. */
static void rows_kvm(void)
{
    reset(0xB, 0, X2APIC_BIT | HYPERVISOR_BIT);
    set_hypervisor(0x40000000u, "KVMKVMKVM\0\0\0", 0x40000001u);
    CHECK(kcpu_x2apic_unremapped(), "KVM at 0x40000000 allows x2APIC mode");

    reset(0xB, 0, X2APIC_BIT | HYPERVISOR_BIT);
    set_hypervisor(0x40000000u, "Microsoft Hv", 0x4000000Au);
    set_leaf(0x40000003u, 0, 1u << 5, 0, 0, 0); /* no VP index MSR */
    set_hypervisor(0x40000100u, "KVMKVMKVM\0\0\0", 0x40000101u);
    CHECK(kcpu_x2apic_unremapped(), "KVM at 0x40000100 behind a Hyper-V leaf allows it");
}

/** Hyper-V's signature at 0x40000000 with leaves to 0x40000005 and its
 * hypercall and VP index MSRs, bits 5 and 6 of leaf 0x40000003's EAX. */
static void rows_hyperv(void)
{
    reset(0xB, 0, X2APIC_BIT | HYPERVISOR_BIT);
    set_hypervisor(0x40000000u, "Microsoft Hv", 0x40000005u);
    set_leaf(0x40000003u, 0, 3u << 5, 0, 0, 0);
    CHECK(kcpu_x2apic_unremapped(), "Hyper-V with both MSRs allows x2APIC mode");

    reset(0xB, 0, X2APIC_BIT | HYPERVISOR_BIT);
    set_hypervisor(0x40000000u, "Microsoft Hv", 0x40000005u);
    set_leaf(0x40000003u, 0, 1u << 5, 0, 0, 0);
    CHECK(!kcpu_x2apic_unremapped(), "Hyper-V without the VP index MSR does not");

    reset(0xB, 0, X2APIC_BIT | HYPERVISOR_BIT);
    set_hypervisor(0x40000000u, "Microsoft Hv", 0x40000005u);
    set_leaf(0x40000003u, 0, 1u << 6, 0, 0, 0);
    CHECK(!kcpu_x2apic_unremapped(), "nor without the hypercall MSR");

    reset(0xB, 0, X2APIC_BIT | HYPERVISOR_BIT);
    set_hypervisor(0x40000000u, "Microsoft Hv", 0x40000004u);
    set_leaf(0x40000003u, 0, 3u << 5, 0, 0, 0);
    CHECK(!kcpu_x2apic_unremapped(), "nor with leaves only to 0x40000004");
}

/** Xen's signature with at least two leaves past its base. */
static void rows_xen(void)
{
    reset(0xB, 0, X2APIC_BIT | HYPERVISOR_BIT);
    set_hypervisor(0x40000000u, "XenVMMXenVMM", 0x40000002u);
    CHECK(kcpu_x2apic_unremapped(), "Xen with two leaves allows x2APIC mode");

    reset(0xB, 0, X2APIC_BIT | HYPERVISOR_BIT);
    set_hypervisor(0x40000000u, "XenVMMXenVMM", 0x40000001u);
    CHECK(!kcpu_x2apic_unremapped(), "Xen with one leaf does not");

    reset(0xB, 0, X2APIC_BIT | HYPERVISOR_BIT);
    set_hypervisor(0x40000000u, "Microsoft Hv", 0x40000005u);
    set_hypervisor(0x40000100u, "XenVMMXenVMM", 0x40000105u);
    CHECK(kcpu_x2apic_unremapped(), "Xen at 0x40000100, behind its Hyper-V leaf, allows it");
}

/** The lock: leaf 7's EDX bit 29, IA32_ARCH_CAPABILITIES bit 21 and
 * IA32_XAPIC_DISABLE_STATUS bit 0, all three. */
static void rows_locked(void)
{
    reset(7, 0, X2APIC_BIT);
    set_leaf(7, 0, 0, 0, 0, 1u << 29);
    arch_cap = 1u << 21;
    xapic_disable_status = 1;
    CHECK(kcpu_x2apic_locked(), "all three bits lock x2APIC mode");

    xapic_disable_status = 0;
    CHECK(!kcpu_x2apic_locked(), "the status bit clear does not");

    xapic_disable_status = 1;
    arch_cap = 0;
    CHECK(!kcpu_x2apic_locked(), "nor the capability bit clear");

    reset(7, 0, X2APIC_BIT);
    arch_cap = 1u << 21;
    xapic_disable_status = 1;
    CHECK(!kcpu_x2apic_locked(), "nor leaf 7 without IA32_ARCH_CAPABILITIES");

    reset(6, 0, X2APIC_BIT);
    set_leaf(7, 0, 0, 0, 0, 1u << 29);
    arch_cap = 1u << 21;
    xapic_disable_status = 1;
    CHECK(!kcpu_x2apic_locked(), "nor a leaf 7 past the highest leaf");
}

/** Run the rows. */
int main(void)
{
    rows_id();
    rows_none();
    rows_kvm();
    rows_hyperv();
    rows_xen();
    rows_locked();
    if (fails == 0) {
        printf("kcpu_hosttest: OK\n");
        return 0;
    }
    printf("kcpu_hosttest: %d failure(s)\n", fails);
    return fails > 255 ? 255 : fails;
}
