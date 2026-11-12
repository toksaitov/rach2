/* Model KVM's per-CPU EOI word and steal-time record in a guest. */
/* host-cflags: -DRACH2_HOST_CPU */
#include "arch/x86/kvmsteal.c"

extern int dprintf(int fd, const char *format, ...);
unsigned int host_cpu;
static unsigned int features, writes, last_msr;
static uint64_t last_value;
static int failures;

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

/** Check feature gating, alignment, local completion and the fallback. */
int main(void)
{
    host_cpu = 0;
    CHECK(!kvm_guest_eoi(), "before setup the APIC must receive the EOI");
    features = KVM_FEATURE_PV_EOI;
    kvm_steal_cpu_setup(0);
    CHECK(writes == 1 && last_msr == MSR_KVM_PV_EOI_EN &&
              last_value == ((uint64_t)(uint32_t)(uintptr_t)eoi_record(0) | 1u) &&
              ((uintptr_t)eoi_record(0) & 63u) == 0,
          "the EOI feature alone registers its aligned physical word");
    CHECK(!kvm_guest_eoi(), "a clear shared bit needs the ordinary EOI");
    *eoi_record(0) = 1;
    CHECK(kvm_guest_eoi() && *eoi_record(0) == 0 && !kvm_guest_eoi(),
          "the host's bit is consumed exactly once");
    kvm_steal_cpu_setup(1);
    *eoi_record(0) = 1;
    *eoi_record(1) = 1;
    host_cpu = 1;
    CHECK(kvm_guest_eoi() && *eoi_record(0) == 1 && *eoi_record(1) == 0,
          "one CPU completes no other CPU's interrupt");
    features = KVM_FEATURE_STEAL_TIME;
    kvm_steal_cpu_setup(2);
    host_cpu = 2;
    CHECK(last_msr == MSR_KVM_STEAL_TIME && !kvm_guest_eoi(),
          "steal time alone leaves the EOI fallback in place");
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
