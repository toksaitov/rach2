/* The guest performance counters and the backend's counter switching. */
#ifndef _KVM_PMU_H_
#define _KVM_PMU_H_

#include "../lib/kint.h"

/* Linux's maximum architectural counters, including AMD's six. */
#define KVM_PMU_GP_MAX    8u
#define KVM_PMU_FIXED_MAX 3u

struct kvm_vcpu;
struct kvm_vm;
struct kvm_cpuid_entry2;
struct kvm_enable_cap;

/* A vCPU's counters. The ioctl lock owns them, including during an entry
 * without the big lock. Only prepare reads the VM's filter, under the big
 * lock; the backend uses the resulting run masks until the next exit. */
struct kvm_pmu {
    uint64_t counter[KVM_PMU_GP_MAX], eventsel[KVM_PMU_GP_MAX];
    uint64_t fixed[KVM_PMU_FIXED_MAX];
    uint64_t global_ctrl, global_status, fixed_ctrl, perf_cap;
    uint64_t gp_mask, fixed_mask; /* the guest's counter widths */
    uint32_t version, nr_gp, nr_fixed;
    uint32_t events;            /* available Intel architectural events */
    uint32_t run_gp, run_fixed; /* counters enabled for the next entry */
    int core_ext;               /* AMD's extended CTL/CTR aliases */
    int pmi;                    /* an overflow waits for delivery under the big lock */
};

/* An entry in VMX's load/store lists, whose format is architectural. */
struct kvm_pmu_msr {
    uint32_t index, reserved;
    uint64_t value;
};

/** Discover the host PMU once at boot, after the backend is chosen.
 * The hardware counters are reserved for KVM; the host has no perf user. */
void kvm_pmu_start(void);

/** Whether the host has usable performance counters. */
int kvm_pmu_enabled(void);

/** Filter the host CPUID entry to the PMU the selected backend supplies. */
void kvm_pmu_cpuid(struct kvm_cpuid_entry2 *e);

/** Reset the counters and realize the guest's current CPUID, before its
 * first run. A reset keeps the caller's PERF_CAPABILITIES value. */
void kvm_pmu_refresh(struct kvm_vcpu *v);

/** Initialize a new vCPU's PERF_CAPABILITIES and counter state. */
void kvm_pmu_init(struct kvm_vcpu *v);

/** Reset the counters on INIT, leaving the guest's CPUID model in place. */
void kvm_pmu_reset(struct kvm_vcpu *v);

/** Append the host PMU's MSRs to out; at most 32 entries. Return the count. */
uint32_t kvm_pmu_msr_list(uint32_t *out);

/** Read the feature MSR index into value: 0, or -1 for no such feature. */
int kvm_pmu_feature(uint32_t index, uint64_t *value);

/** Access a guest PMU MSR. host distinguishes the state ioctls from the
 * guest's instructions. Return 0, -1 for #GP/refusal, or 1 for another MSR. */
int kvm_pmu_msr(struct kvm_vcpu *v, uint32_t index, int write, uint64_t *value, int host);

/** Emulate RDPMC into RDX:RAX. Return 0, or -1 with #GP queued. */
int kvm_pmu_rdpmc(struct kvm_vcpu *v);

/** Count one emulated retired instruction in its enabled events. */
void kvm_pmu_retired(struct kvm_vcpu *v);

/** Replace the VM's event filter from the caller. Return 0, -EFAULT,
 * -EINVAL, -E2BIG for more than 300 events, or -ENOMEM. Under the big lock. */
int32_t kvm_pmu_filter(struct kvm_vm *vm, uintptr_t arg);

/** Enable or disable the PMU before any vCPU exists, as KVM_ENABLE_CAP
 * of KVM_CAP_PMU_CAPABILITY. Return 0 or -EINVAL. */
int32_t kvm_pmu_enable(struct kvm_vm *vm, const struct kvm_enable_cap *cap);

/** Free the VM's filter after its vCPUs have stopped. */
void kvm_pmu_vm_free(struct kvm_vm *vm);

/** Deliver a pending PMI and select this entry's events under the big
 * lock, before the backend injects events and lets the guest run. */
void kvm_pmu_prepare(struct kvm_vcpu *v);

/** Load/stop AMD's physical counters with interrupts off, outside the
 * big lock. Selectors use GuestOnly and never enable a physical PMI. */
void kvm_pmu_svm_enter(struct kvm_vcpu *v);
void kvm_pmu_svm_exit(struct kvm_vcpu *v);

/** Build Intel's guest load/store and host load lists. The caller has
 * room for 24 entries in each. Return their lengths in ng and nh. A
 * host entry disables every guest selector before host code can count. */
void kvm_pmu_vmx_enter(struct kvm_vcpu *v, struct kvm_pmu_msr *guest, struct kvm_pmu_msr *host,
                       uint32_t *ng, uint32_t *nh);

/** Accumulate the guest counters VMX stored on exit. The MSR load list
 * has already stopped them. Call only after a successful guest entry. */
void kvm_pmu_vmx_exit(struct kvm_vcpu *v, const struct kvm_pmu_msr *guest);

#endif /* _KVM_PMU_H_ */
