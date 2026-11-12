/*
 * kvmsteal.c: the records of each processor of this guest that KVM writes:
 * its steal time, the nanoseconds the host kept its virtual processor ready
 * to run but ran something else, and its word for the paravirtual end of
 * interrupt.
 */

#include "kvmsteal.h"
#include "kcpu.h"
#include "lapic.h"
#include "../arch.h"
#include "../../lib/kint.h"
#include "../../proc/percpu.h"

#define KVM_FEATURE_STEAL_TIME (1u << 5)
#define KVM_FEATURE_PV_EOI     (1u << 6)
#define MSR_KVM_STEAL_TIME     0x4B564D03u
#define MSR_KVM_PV_EOI_EN      0x4B564D04u
#define KVM_MSR_ENABLED        1u
#define KVM_VCPU_PREEMPTED     1u /* in `preempted` */

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

/* The words for the paravirtual end of interrupt, one in each 64 bytes, so
 * that KVM's write of one processor's word leaves the others' cache lines
 * alone. */
static volatile uint32_t eoi_words[NR_CPUS * 16];

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
    uint32_t features;

    if (cpu < 0 || cpu >= NR_CPUS || !kcpu_msr_present()) {
        return;
    }
    features = kcpu_kvm_features();
    if (features & KVM_FEATURE_PV_EOI) {
        eoi_words[cpu * 16] = 0;
        phys = (uint64_t)virt_to_phys(&eoi_words[cpu * 16]);
        kcpu_wrmsr(MSR_KVM_PV_EOI_EN, (uint32_t)phys | KVM_MSR_ENABLED, (uint32_t)(phys >> 32));
        lapic_set_pv_eoi(cpu, &eoi_words[cpu * 16]);
    }
    if (!(features & KVM_FEATURE_STEAL_TIME)) {
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

int arch_cpu_preempted(int cpu)
{
    const volatile struct kvm_steal_time *st;

    if (cpu < 0 || cpu >= NR_CPUS || !steal_on[cpu]) {
        return 0;
    }
    st = steal_record(cpu);
    return (st->preempted & KVM_VCPU_PREEMPTED) != 0;
}
