/*
 * kvmsteal.c: KVM's steal time for each processor of this guest: the
 * nanoseconds the host kept its virtual processor ready to run but ran
 * something else, in a record of each processor's that KVM writes.
 */

#include "kvmsteal.h"
#include "kcpu.h"
#include "../arch.h"
#include "../../lib/kint.h"
#include "../../proc/percpu.h"

#define KVM_FEATURE_STEAL_TIME (1u << 5)
#define MSR_KVM_STEAL_TIME     0x4B564D03u
#define KVM_MSR_ENABLED        1u

/* Linux's struct kvm_steal_time (arch/x86/include/uapi/asm/kvm_para.h:62-
 * 69): 64 bytes, which KVM wants aligned to 64. */
struct kvm_steal_time {
    uint64_t steal;   /* nanoseconds stolen since the record was given */
    uint32_t version; /* odd while KVM writes the record */
    uint32_t flags;
    uint8_t preempted;
    uint8_t u8_pad[3];
    uint32_t pad[11];
};

/* The records, one for each processor, in room for one more so that they
 * can start on a 64-byte boundary; and which processors gave KVM theirs. */
static uint64_t steal_room[(NR_CPUS + 1) * 8];
static volatile uint8_t steal_on[NR_CPUS];

/** Processor `cpu`'s record. */
static struct kvm_steal_time *steal_record(int cpu)
{
    uintptr_t at = ((uintptr_t)steal_room + 63u) & ~(uintptr_t)63u;

    return (struct kvm_steal_time *)at + cpu;
}

void kvm_steal_cpu_setup(int cpu)
{
    struct kvm_steal_time *st;
    uint64_t phys;

    if (cpu < 0 || cpu >= NR_CPUS || !kcpu_msr_present() ||
        !(kcpu_kvm_features() & KVM_FEATURE_STEAL_TIME)) {
        return;
    }
    st = steal_record(cpu);
    phys = (uint64_t)virt_to_phys(st);
    kcpu_wrmsr(MSR_KVM_STEAL_TIME, (uint32_t)phys | KVM_MSR_ENABLED, (uint32_t)(phys >> 32));
    steal_on[cpu] = 1;
}

uint64_t arch_steal_ns(void)
{
    int cpu = arch_cpu_id();
    const volatile struct kvm_steal_time *st;
    uint32_t version;
    uint64_t steal;

    if (cpu < 0 || cpu >= NR_CPUS || !steal_on[cpu]) {
        return 0;
    }
    st = steal_record(cpu);
    do {
        version = st->version;
        arch_dma_fence();
        steal = st->steal;
        arch_dma_fence();
    } while ((version & 1u) != 0 || version != st->version);
    return steal;
}
