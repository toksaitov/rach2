/* xen.c's interface (kvm/kvm.h) for a host test of KVM without Xen: no VM
 * has a Xen MSR, an intercept, a shared info page or a vCPU area, so every
 * hook does nothing, as xen.c's do before the caller sets one up. */
#include "../../../rach2/sys/kvm/kvm.h"
#include "../../../rach2/sys/lib/kerrno.h"

/** No Xen capability. */
int32_t kvm_xen_caps(void)
{
    return 0;
}

/** No Xen MSR. */
int kvm_xen_msr_enabled(struct kvm_vm *vm)
{
    (void)vm;
    return 0;
}

/** No intercept. */
int kvm_xen_hypercall_enabled(struct kvm_vm *vm)
{
    (void)vm;
    return 0;
}

/** Never reached without the intercept; the call goes to the caller. */
int kvm_xen_hypercall(struct kvm_vcpu *v, uint32_t len)
{
    (void)len;
    v->exit.kind = KVM_X_AGAIN;
    return 0;
}

/** No KVM_EXIT_XEN waits for an answer. */
void kvm_xen_complete(struct kvm_vcpu *v)
{
    (void)v;
}

/** No MSR is the Xen hypercall MSR. */
int kvm_xen_msr_write(struct kvm_vcpu *v, uint32_t index, uint64_t data)
{
    (void)v;
    (void)index;
    (void)data;
    return 1;
}

/** No Xen ioctl of a VM. */
int32_t kvm_xen_vm_ioctl(struct kvm_vm *vm, uint32_t request, uintptr_t arg)
{
    (void)vm;
    (void)request;
    (void)arg;
    return -ENOTTY;
}

/** No Xen ioctl of a vCPU. */
int32_t kvm_xen_vcpu_ioctl(struct kvm_vcpu *v, uint32_t request, uintptr_t arg)
{
    (void)v;
    (void)request;
    (void)arg;
    return -ENOTTY;
}

/** No route is a Xen event channel's. */
int kvm_xen_route_check(struct kvm_vm *vm, uint32_t port, uint32_t priority)
{
    (void)vm;
    (void)port;
    (void)priority;
    return -EINVAL;
}

/** No shared info to raise a port in. */
int kvm_xen_route_set(struct kvm_vm *vm, uint32_t vcpu, uint32_t port)
{
    (void)vm;
    (void)vcpu;
    (void)port;
    return -EINVAL;
}

/** No Xen clock area. */
int kvm_xen_clock_active(const struct kvm_vcpu *v)
{
    (void)v;
    return 0;
}

/** No Xen clock area to write. */
void kvm_xen_clock_write(struct kvm_vcpu *v, const struct pvclock_vcpu_time_info *pv, int stopped)
{
    (void)v;
    (void)pv;
    (void)stopped;
}

/** No Xen CPUID leaves to fill. */
void kvm_xen_tsc_info(struct kvm_vcpu *v, uint32_t mul, int8_t shift, uint32_t khz)
{
    (void)v;
    (void)mul;
    (void)shift;
    (void)khz;
}

/** No runstate. */
void kvm_xen_runstate_running(struct kvm_vcpu *v)
{
    (void)v;
}

/** No runstate. */
void kvm_xen_preempted(struct kvm_vcpu *v)
{
    (void)v;
}

/** No upcall. */
int kvm_xen_upcall(struct kvm_vcpu *v)
{
    (void)v;
    return -1;
}

/** No upcall to read. */
void kvm_xen_refresh(struct kvm_vcpu *v)
{
    (void)v;
}

/** No Xen work before an entry. */
void kvm_xen_entry(struct kvm_vcpu *v)
{
    (void)v;
}

/** No Xen work in the halted wait. */
void kvm_xen_prepare(struct kvm_vcpu *v)
{
    (void)v;
}

/** Nothing of Xen's ends a wait. */
int kvm_xen_wake(struct kvm_vcpu *v)
{
    (void)v;
    return 0;
}

/** No Xen timer. */
uint64_t kvm_xen_timers(struct kvm_vcpu *v)
{
    (void)v;
    return 0;
}

/** No Xen timer. */
uint64_t kvm_xen_next(struct kvm_vcpu *v)
{
    (void)v;
    return 0;
}

/** No poll to end. */
void kvm_xen_run_end(struct kvm_vcpu *v)
{
    (void)v;
}

/** No per-vCPU upcall vector. */
void kvm_xen_sw_enable_lapic(struct kvm_vcpu *v)
{
    (void)v;
}

/** Nothing to make. */
int kvm_xen_vm_init(struct kvm_vm *vm)
{
    vm->xen = NULL;
    return 0;
}

/** Nothing to free. */
void kvm_xen_vm_free(struct kvm_vm *vm)
{
    (void)vm;
}

/** Nothing to make. */
int kvm_xen_vcpu_init(struct kvm_vcpu *v)
{
    v->xen = NULL;
    return 0;
}

/** Nothing to free. */
void kvm_xen_vcpu_free(struct kvm_vcpu *v)
{
    (void)v;
}
