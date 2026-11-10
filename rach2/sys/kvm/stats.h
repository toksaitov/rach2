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

#endif
