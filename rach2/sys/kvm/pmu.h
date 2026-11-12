/* The guest performance counters and the backend's counter switching. */
#ifndef _KVM_PMU_H_
#define _KVM_PMU_H_

#include "../lib/kint.h"

/* Linux's maximum architectural counters, including AMD's six. */
#define KVM_PMU_GP_MAX    8u
#define KVM_PMU_FIXED_MAX 3u

/* A guest counter's slot: general counter i is slot i, and fixed counter j
 * slot KVM_PMU_GP_MAX + j. */
#define KVM_PMU_SLOTS (KVM_PMU_GP_MAX + KVM_PMU_FIXED_MAX)

/* The most last-branch records a legacy LBR stack has: Skylake's 32. */
#define KVM_LBR_MAX 32u

struct kvm_vcpu;
struct kvm_vm;
struct kvm_cpuid_entry2;
struct kvm_enable_cap;

/* A vCPU's counters and last-branch records. The ioctl lock owns them,
 * including during an entry without the big lock. Only prepare reads the
 * VM's filter, under the big lock; the backend uses the resulting run masks
 * until the next exit. */
struct kvm_pmu {
    uint64_t counter[KVM_PMU_GP_MAX], eventsel[KVM_PMU_GP_MAX];
    uint64_t fixed[KVM_PMU_FIXED_MAX];
    uint64_t global_ctrl, global_status, fixed_ctrl, perf_cap;
    uint64_t gp_mask, fixed_mask; /* the guest's counter widths */
    uint32_t version, nr_gp, nr_fixed;
    uint32_t events; /* available Intel architectural events */
    /* The counters the guest has enabled and the filter allows, found
     * again at KVM_REQ_PMU (`kvm_pmu_prepare`), and of those the ones each
     * entry runs, less any left without a physical counter. */
    uint32_t want_gp, want_fixed;
    uint32_t run_gp, run_fixed;
    int core_ext; /* AMD's extended CTL/CTR aliases */
    int pmi;      /* an overflow waits for delivery (KVM_REQ_PMI) */
    /* The physical counter each running counter has during an entry, by
     * slot: general counter n, or fixed counter n - 32 from 32 on; the
     * physical counts read before it, for the deltas the exit adds; and
     * the guest's general counters whose AMD counter merges the next. */
    uint8_t phys[KVM_PMU_SLOTS];
    uint64_t base[KVM_PMU_SLOTS];
    uint32_t pair;
    uint64_t host_global; /* AMD's PerfMonV2 global control before an entry */
    /* VMX's IA32_DEBUGCTL: only LBR, FREEZE_LBRS_ON_PMI, BUS_LOCK_DETECT
     * and RTM_DEBUG are kept. */
    uint64_t debugctl;
    /* The guest's LBR stack, 0 records without one, and its registers as
     * the last exit left them, read back as the hardware stored them. */
    uint32_t lbr_nr;
    uint32_t lbr_list; /* the stack's first entry in VMX's guest list, or ~0 */
    uint64_t lbr_select, lbr_tos;
    uint64_t lbr_from[KVM_LBR_MAX], lbr_to[KVM_LBR_MAX], lbr_info[KVM_LBR_MAX];
};

/* An entry in VMX's load/store lists, whose format is architectural. */
struct kvm_pmu_msr {
    uint32_t index, reserved;
    uint64_t value;
};

/** Discover the host PMU, its event constraints and its LBR stack once at
 * boot, after the backend is chosen. The host's own user, the hard-lockup
 * detector, keeps the counter it has enabled; a guest's counters run on
 * the others that their events may use. */
void kvm_pmu_start(void);

/** Whether the host has usable performance counters. */
int kvm_pmu_enabled(void);

/** Whether the host has Intel's IA32_PERF_GLOBAL_CTRL, of version 2. */
int kvm_pmu_global(void);

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

/** Deliver a pending PMI and find again the counters the guest has
 * enabled and the filter allows, which the entries run, as Linux's
 * kvm_pmu_handle_event and kvm_pmu_deliver_pmi do at KVM_REQ_PMU and
 * KVM_REQ_PMI (arch/x86/kvm/x86.c:10369-10372), which the run loop calls it
 * for and it clears; before the backend injects events and lets the guest
 * run. In `v`'s thread. */
void kvm_pmu_prepare(struct kvm_vcpu *v);

/** Access VMX's IA32_DEBUGCTL, as Linux's vmx_set_msr: LBR and
 * FREEZE_LBRS_ON_PMI with the guest's LBR stack or from the caller,
 * BUS_LOCK_DETECT and RTM_DEBUG where the host has them and the guest's
 * CPUID or the caller asks, BTF and an LBR without a stack dropped, any
 * other bit refused. 0 or -1. */
int kvm_pmu_debugctl(struct kvm_vcpu *v, int write, uint64_t *value, int host);

/** The guest's IA32_DEBUGCTL as the VMCS loads it: the kept bits, less LBR
 * and FREEZE_LBRS_ON_PMI without the guest's LBR stack. The backend adds
 * the host's FREEZE_WHILE_SMM, which the guest never reads (vmx.c). */
uint64_t kvm_pmu_vmcs_debugctl(const struct kvm_vcpu *v);

/** Load/stop AMD's physical counters with interrupts off, outside the
 * big lock. Selectors use GuestOnly and never enable a physical PMI; a
 * counter the host has enabled is left alone, and each guest counter runs
 * on one its event may use. */
void kvm_pmu_svm_enter(struct kvm_vcpu *v);
void kvm_pmu_svm_exit(struct kvm_vcpu *v);

/** The room a VMX MSR list needs for the PMU: the guest's selectors and
 * global control and an LBR stack of 32 records with their information. */
#define KVM_PMU_LIST_MAX (KVM_PMU_GP_MAX + 3u + 3u * KVM_LBR_MAX)

/**
 * Program Intel's counters for an entry, with interrupts off, and build the
 * guest's load/store list and the host's load list, of at most
 * KVM_PMU_LIST_MAX entries each; their lengths go to `*ng` and `*nh`. Each
 * running guest counter, general or fixed, takes a physical counter the
 * host does not hold and its event may use. The selectors and FIXED_CTRL,
 * whose host fields are kept, are written here, gated by the global
 * control, and no counter is written: each running counter's count is read
 * now and again after the exit. `global`, when not NULL, receives the
 * guest's and the host's IA32_PERF_GLOBAL_CTRL for the VMCS's load
 * controls; otherwise the lists switch it. The guest's LBR stack is in the
 * lists while its DEBUGCTL.LBR is set.
 */
void kvm_pmu_vmx_enter(struct kvm_vcpu *v, struct kvm_pmu_msr *guest, struct kvm_pmu_msr *host,
                       uint32_t *ng, uint32_t *nh, uint64_t *global);

/** After the run `kvm_pmu_vmx_enter` prepared, whose exit stopped the
 * counters: add each running counter's delta when `ran`, and take the LBR
 * stack the exit stored in `guest`; release the selectors either way. */
void kvm_pmu_vmx_exit(struct kvm_vcpu *v, const struct kvm_pmu_msr *guest, int ran);

#endif /* _KVM_PMU_H_ */
