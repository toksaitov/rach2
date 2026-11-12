/*
 * kvmsteal.c: the records of each processor of this guest that KVM writes:
 * its steal time, the nanoseconds the host kept its virtual processor ready
 * to run but ran something else, and its word for the paravirtual end of
 * interrupt; and the two uses Linux makes of KVM to reach other processors,
 * the hypercall that sends an interrupt to many and the TLB flush KVM does
 * for a processor it has preempted.
 */

#include "kvmsteal.h"
#include "kcpu.h"
#include "lapic.h"
#include "smpboot.h"
#include "../arch.h"
#include "../../lib/kint.h"
#include "../../proc/percpu.h"

#define KVM_FEATURE_STEAL_TIME   (1u << 5)
#define KVM_FEATURE_PV_EOI       (1u << 6)
#define KVM_FEATURE_PV_TLB_FLUSH (1u << 9)
#define KVM_FEATURE_PV_SEND_IPI  (1u << 11)
#define KVM_HINTS_REALTIME       (1u << 0)
#define MSR_KVM_STEAL_TIME       0x4B564D03u
#define MSR_KVM_PV_EOI_EN        0x4B564D04u
#define KVM_MSR_ENABLED          1u
#define KVM_VCPU_PREEMPTED       1u /* in `preempted` */
#define KVM_VCPU_FLUSH_TLB       2u /* in `preempted`: flush before the next entry */
#define KVM_HC_SEND_IPI          10u

/* The bits of one KVM_HC_SEND_IPI's bitmap, two longs, as Linux's
 * KVM_IPI_CLUSTER_SIZE (arch/x86/kernel/kvm.c); KVM reads each argument
 * as 32 bits outside long mode. */
#define LONG_BITS     (8u * (unsigned int)sizeof(unsigned long))
#define IPI_SPAN_BITS (2u * LONG_BITS)

/* Linux's struct kvm_steal_time (arch/x86/include/uapi/asm/kvm_para.h:62-
 * 69): 64 bytes, which KVM wants aligned to 64. */
struct kvm_steal_time {
    uint64_t steal;   /* nanoseconds stolen since the record was given */
    uint32_t version; /* odd while KVM writes the record */
    uint32_t flags;
    /* `preempted`, KVM_VCPU_* bits, and its three bytes of padding, which
     * KVM leaves 0, as one word for a compare-and-exchange. */
    union {
        uint8_t preempted;
        unsigned int word;
    } pre;
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

/* Set on the boot processor before the others start: KVM offers
 * KVM_HC_SEND_IPI, and the KVM_VCPU_FLUSH_TLB mark; and the instruction
 * that calls it is VMMCALL. Linux turns the flush off under
 * KVM_HINTS_REALTIME, whose processors are never preempted. */
static int pv_ipi, pv_flush, hc_vmmcall;

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
    if (cpu == 0) {
        hc_vmmcall = kcpu_vmmcall();
        pv_ipi = (features & KVM_FEATURE_PV_SEND_IPI) != 0;
        pv_flush = (features & KVM_FEATURE_PV_TLB_FLUSH) && (features & KVM_FEATURE_STEAL_TIME) &&
                   !(kcpu_kvm_hints() & KVM_HINTS_REALTIME);
    }
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
    return (st->pre.preempted & KVM_VCPU_PREEMPTED) != 0;
}

/**
 * Linux's kvm_flush_tlb_multi (arch/x86/kernel/kvm.c): a processor whose
 * record says KVM preempted it runs no instruction until KVM enters it
 * again, and KVM, which clears `preempted` with an exchange as it enters,
 * flushes the processor's TLB first when it finds the mark there. The
 * mark is set by a compare-and-exchange against the word read, so a
 * processor KVM entered meanwhile, whose word is 0, is not marked.
 */
int arch_cpu_tlb_deferred(int cpu)
{
    volatile struct kvm_steal_time *st;
    unsigned int w;

    if (!pv_flush || cpu < 0 || cpu >= NR_CPUS || !steal_on[cpu]) {
        return 0;
    }
    st = steal_record(cpu);
    w = st->pre.word;
    return (w & KVM_VCPU_PREEMPTED) != 0 &&
           arch_atomic_cmpxchg(&st->pre.word, w, w | KVM_VCPU_FLUSH_TLB) == w;
}

/** Shift the two-long bitmap `b` up by `n` bits, 0 < `n` < IPI_SPAN_BITS. */
static void span_shift(unsigned long *b, unsigned int n)
{
    if (n >= LONG_BITS) {
        b[1] = b[0] << (n - LONG_BITS);
        b[0] = 0;
    } else {
        b[1] = (b[1] << n) | (b[0] >> (LONG_BITS - n));
        b[0] <<= n;
    }
}

/** One KVM_HC_SEND_IPI of the fixed interrupt `vector` to the processors
 * whose APIC ids are `min` plus the bits of `b`: 0, or KVM's error. */
static long span_send(const unsigned long *b, uint32_t min, uint8_t vector)
{
    long r = kcpu_hypercall4(hc_vmmcall, KVM_HC_SEND_IPI, b[0], b[1], min, vector);

    return r < 0 ? r : 0;
}

/**
 * Linux's __send_ipi_mask (arch/x86/kernel/kvm.c): the APIC ids of the
 * set, in processor order, gather into spans of IPI_SPAN_BITS ids from
 * the lowest, a span growing down while its highest id stays within it,
 * and each span is one hypercall, which KVM delivers to every vCPU the
 * bits name. A call KVM refuses turns the hypercall off and returns 0, so
 * that the caller sends the set through the APIC; a processor of a span
 * already sent then takes the interrupt twice, which costs it a look at
 * its requests.
 */
int kvm_send_ipi_many(uint64_t cpus, uint8_t vector)
{
    unsigned long b[2] = {0, 0};
    uint32_t id, min = 0, max = 0;
    int c;

    if (!pv_ipi) {
        return 0;
    }
    /* A store made before the interrupt must reach the processor it wakes,
     * as the fence before an x2APIC command's write orders it (lapic.c). */
    arch_dma_fence();
    for (c = 0; c < NR_CPUS; c++) {
        if (!((cpus >> c) & 1u)) {
            continue;
        }
        id = smpboot_apic_id(c);
        if ((b[0] | b[1]) == 0) {
            min = max = id;
        } else if (id < min && max - id < IPI_SPAN_BITS) {
            span_shift(b, min - id);
            min = id;
        } else if (id > min && id < min + IPI_SPAN_BITS) {
            max = id > max ? id : max;
        } else {
            if (span_send(b, min, vector) != 0) {
                pv_ipi = 0;
                return 0;
            }
            b[0] = b[1] = 0;
            min = max = id;
        }
        b[(id - min) / LONG_BITS] |= 1ul << ((id - min) % LONG_BITS);
    }
    if ((b[0] | b[1]) != 0 && span_send(b, min, vector) != 0) {
        pv_ipi = 0;
        return 0;
    }
    return 1;
}
