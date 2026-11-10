/* Live KVM resources, counted under the big kernel lock, and the work done
 * for guests without an exit to the caller. */
#ifndef _KVM_STATS_H_
#define _KVM_STATS_H_

#include "../lib/kint.h"

enum kvm_resource {
    KVM_COUNT_VM,
    KVM_COUNT_VCPU,
    KVM_COUNT_SLOT,
    KVM_COUNT_TDP_PAGE,
    KVM_COUNT_RUN_MAPPING,
    KVM_COUNT_TOTAL
};

/** Add change, 1 or -1, as a resource is made or finally released. */
void kvm_count(enum kvm_resource resource, int change);

/** The live count of resource; called under the big kernel lock. */
uint32_t kvm_resource_count(enum kvm_resource resource);

/** Completed backend entries since boot; read under the big lock. */
uint64_t kvm_exit_count(void);

/* Work the kernel did for a guest without an exit to the caller, counted
 * since boot modulo 2^32: a guest's write an ioeventfd took, and an MSI an
 * irqfd raised. */
enum kvm_event { KVM_EVENT_IOEVENTFD, KVM_EVENT_IRQFD, KVM_EVENT_TOTAL };

/** Count one `event`, in any context, with interrupts on or off. */
void kvm_event_add(enum kvm_event event);

/** The count of `event` since boot. */
uint32_t kvm_event_count(enum kvm_event event);

/* The steps a vCPU's interrupt of one vector reached, counted since the
 * vCPU's creation modulo 2^32: accepted into IRR, refused by an APIC
 * disabled in hardware or software, acknowledged into ISR or taken from
 * the PIC, injected at an entry, taken back from an exit that cut its
 * delivery off (SVM's EXITINTINFO, VMX's IDT-vectoring information), and
 * ended by an EOI. An interrupt that reaches one step and never the next
 * shows where it was lost. */
enum kvm_vec_step {
    KVM_VEC_ACCEPT,
    KVM_VEC_DROP,
    KVM_VEC_ACK,
    KVM_VEC_INJECT,
    KVM_VEC_SAVE,
    KVM_VEC_EOI,
    KVM_VEC_STEPS
};

/* The steps of an input of the in-kernel I/O APIC, counted since the VM's
 * KVM_CREATE_IRQCHIP modulo 2^32: raised, coalesced into an edge still
 * high or an RTC interrupt not yet ended, held by the entry's mask or
 * remote IRR, delivered to at least one APIC, and delivered to none. */
enum kvm_pin_step {
    KVM_PIN_RAISE,
    KVM_PIN_COALESCE,
    KVM_PIN_HELD,
    KVM_PIN_DELIVER,
    KVM_PIN_MISS,
    KVM_PIN_STEPS
};

/* One line of a VM's interrupt counts, for its descriptor's fdinfo. */
struct kvm_irq_row {
    int pin;                        /* 1: an I/O APIC pin's; 0: a vCPU's vector's */
    uint32_t vcpu;                  /* the vCPU's index in the VM, for a vector */
    uint32_t index;                 /* the pin or the vector */
    uint32_t counts[KVM_VEC_STEPS]; /* by enum kvm_pin_step or kvm_vec_step */
};

struct fd_entry;

/** The VM `e` is a descriptor of, with a reference the caller drops by
 * `kvm_vm_irq_done`, or NULL for any other descriptor. Under the big
 * lock. */
void *kvm_vm_irq_hold(const struct fd_entry *e);

/** Fill `row` with the `n`-th row with a nonzero count of the VM `vm`
 * holds, the pins first, then each vCPU's vectors in order. 1, or 0 past
 * the last. Under the big lock. */
int kvm_vm_irq_row(void *vm, uint32_t n, struct kvm_irq_row *row);

/** Drop the reference `kvm_vm_irq_hold` took. */
void kvm_vm_irq_done(void *vm);

/** The VM's interrupt controller in the kernel, "kernel", "split" or
 * "none". */
const char *kvm_vm_irqchip_name(void *vm);

#endif
