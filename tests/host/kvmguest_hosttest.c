/* Model KVM's per-CPU EOI word and steal-time record in a guest. */
/* host-cflags: -DRACH2_HOST_CPU */
#include "arch/x86/kvmsteal.c"

extern int dprintf(int fd, const char *format, ...);
unsigned int host_cpu;
static unsigned int features, writes, last_msr;
static uint64_t last_value;
static int failures;
static volatile uint32_t *registered_eoi[NR_CPUS];

#define CHECK(test, message)                                        \
    do {                                                            \
        if (!(test)) {                                              \
            failures++;                                             \
            dprintf(2, "kvmguest-hosttest: failed: %s\n", message); \
        }                                                           \
    } while (0)

/** The modeled CPU implements MSRs. */
int kcpu_msr_present(void)
{
    return 1;
}

/** Return the feature bits selected by this row. */
uint32_t kcpu_kvm_features(void)
{
    return features;
}

/** No exclusive-CPU hint is supplied by this model. */
uint32_t kcpu_kvm_hints(void)
{
    return 0;
}

/** The model offers no hypercall feature. */
int kcpu_vmmcall(void)
{
    return 0;
}

/** Reject an unexpected hypercall; these rows offer no IPI feature. */
long kcpu_hypercall4(int vmmcall, unsigned long nr, unsigned long a0, unsigned long a1,
                     unsigned long a2, unsigned long a3)
{
    (void)vmmcall;
    (void)nr;
    (void)a0;
    (void)a1;
    (void)a2;
    (void)a3;
    CHECK(0, "the shared-record rows request no hypercall");
    return -1;
}

/** Give each model CPU its own APIC identity. */
uint32_t smpboot_apic_id(int cpu)
{
    return (uint32_t)cpu;
}

/** Complete a serial model compare and exchange. */
unsigned int arch_atomic_cmpxchg(volatile unsigned int *p, unsigned int old, unsigned int value)
{
    unsigned int was = *p;

    if (was == old) {
        *p = value;
    }
    return was;
}

/** Record a setup write without touching a processor register. */
void kcpu_wrmsr(uint32_t msr, uint32_t lo, uint32_t hi)
{
    writes++;
    last_msr = msr;
    last_value = ((uint64_t)hi << 32) | lo;
}

/** The model changes records only between complete reads. */
void arch_dma_fence(void)
{}

/** Record the local APIC's shared completion word. */
void lapic_set_pv_eoi(int cpu, volatile uint32_t *word)
{
    registered_eoi[cpu] = word;
}

/** Check independent feature setup and local and remote steal reads. */
int main(void)
{
    host_cpu = 0;
    features = KVM_FEATURE_PV_EOI;
    kvm_steal_cpu_setup(0);
    CHECK(writes == 1 && last_msr == MSR_KVM_PV_EOI_EN &&
              last_value == ((uint64_t)(uint32_t)(uintptr_t)registered_eoi[0] | 1u),
          "the EOI feature alone registers its physical word with the APIC");
    kvm_steal_cpu_setup(1);
    CHECK(registered_eoi[1] != registered_eoi[0] &&
              (uintptr_t)registered_eoi[1] - (uintptr_t)registered_eoi[0] == 64,
          "each CPU registers its own cache line");
    features = KVM_FEATURE_STEAL_TIME;
    kvm_steal_cpu_setup(2);
    host_cpu = 2;
    CHECK(last_msr == MSR_KVM_STEAL_TIME && !registered_eoi[2],
          "steal time alone registers no EOI word");
    steal_record(2)->steal = 123456789u;
    CHECK(arch_steal_ns() == 123456789u && arch_cpu_steal_ns(2) == 123456789u,
          "the local and remote readers keep the selected steal record");
    features = 0;
    writes = 0;
    kvm_steal_cpu_setup(3);
    kvm_steal_cpu_setup(-1);
    kvm_steal_cpu_setup(NR_CPUS);
    CHECK(!writes, "no feature and invalid CPU numbers register nothing");
    dprintf(1, "kvmguest-hosttest: %s\n", failures ? "fail" : "pass");
    return failures != 0;
}
