/* Live KVM resources, counted under the big kernel lock. */
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

#endif
