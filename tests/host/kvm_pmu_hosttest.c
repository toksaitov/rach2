/* Check guest PMUs against Intel and AMD register models.
 * host-cflags: -DRACH2_HOST_X86_64 -DRACH2_HOST_CPU
 */
#include "kvm/pmu.c"

extern int printf(const char *format, ...);
extern void *malloc(unsigned long n);
extern void free(void *p);

static int failures, live, alloc_fail, copy_fail, pmi_count, fault_count;
static int model_intel, model_ext, model_v2, model_missing, model_ignored;
static uint32_t irq_flags = 0x200u;
static uint64_t physical[64];
static uint32_t physical_index[64], physical_n;
const struct kvm_backend kvm_vmx, kvm_svm;
const struct kvm_backend *kvm_backend;
static struct kvm_vm vm;
static struct kvm_vcpu v, other;
static struct kvm_cpuid_entry2 cpuid[5];

/** Record one failed contract. */
#define CHECK(c, why)                                      \
    do {                                                   \
        if (!(c)) {                                        \
            printf("kvm-pmu-hosttest: failed: %s\n", why); \
            failures++;                                    \
        }                                                  \
    } while (0)

/** Copy bytes without the host's conflicting integer declarations. */
void *k_memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

/** Clear the modeled kernel records. */
void *k_memset(void *dst, int byte, size_t n)
{
    unsigned char *d = dst;
    while (n--) {
        *d++ = (unsigned char)byte;
    }
    return dst;
}

/** Count each filter allocation and optionally refuse it. */
void *kmalloc(size_t n)
{
    void *p;
    if (alloc_fail) {
        return NULL;
    }
    p = malloc(n);
    if (p) {
        live++;
    }
    return p;
}

/** Return an allocation to the model's heap. */
void kfree(void *p)
{
    if (p) {
        live--;
        free(p);
    }
}

/** Model an absent caller mapping, including the filter's second copy. */
int copy_from_user(void *dst, const void __user *src, size_t n)
{
    if (!src || (copy_fail && --copy_fail == 0)) {
        return -EFAULT;
    }
    k_memcpy(dst, (const void *)src, n);
    return 0;
}

/** Hold the probe's interrupt state. */
uint32_t kirq_save(void)
{
    uint32_t old = irq_flags;
    irq_flags = 0;
    return old;
}

/** Restore precisely the flags the probe found. */
void kirq_restore(uint32_t flags)
{
    irq_flags = flags;
}

/** The host's advertised PMU, with independently selectable AMD modes. */
void kcpu_id_sub(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    (void)sub;
    *a = *b = *c = *d = 0;
    if (leaf == 0) {
        *a = 0x16;
    }
    if (leaf == 1 && model_intel) {
        *c = PDCM;
    }
    if (leaf == 0xA && model_intel) {
        *a = 0x07300403u;
        *d = 3u | (48u << 5);
    }
    if (leaf == 7 && model_intel) {
        *b = 1u << 11;
    }
    if (leaf == 0x80000000u) {
        *a = model_v2 ? 0x80000022u : 0x80000008u;
    }
    if (leaf == 0x80000001u && model_ext) {
        *c = CORE_EXT;
    }
    if (leaf == 0x80000022u && model_v2) {
        *a = 1;
        *b = 6;
    }
}

/** Subleaf zero of the host model. */
void kcpu_id(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    kcpu_id_sub(leaf, 0, a, b, c, d);
}

/** Find a physical register in a bounded table. */
static uint64_t *reg(uint32_t index)
{
    uint32_t i;
    for (i = 0; i < physical_n; i++) {
        if (physical_index[i] == index) {
            return &physical[i];
        }
    }
    CHECK(physical_n < 64u, "physical register table fits");
    if (physical_n == 64u) {
        return &physical[0];
    }
    physical_index[physical_n] = index;
    return &physical[physical_n++];
}

/** Read the register bank used by the production switching code. */
void kcpu_rdmsr(uint32_t index, uint32_t *lo, uint32_t *hi)
{
    uint64_t value = *reg(index);
    CHECK(!model_missing, "no unchecked access to an absent PMU");
    *lo = (uint32_t)value;
    *hi = (uint32_t)(value >> 32);
}

/** Set a physical register, or ignore it as TCG does AMD's counters. */
void kcpu_wrmsr(uint32_t index, uint32_t lo, uint32_t hi)
{
    CHECK(!model_missing, "no unchecked write to an absent PMU");
    if (!model_ignored) {
        *reg(index) = (uint64_t)lo | ((uint64_t)hi << 32);
    }
}

/** A faulting optional MSR leaves the output unchanged. */
int kcpu_rdmsr_safe(uint32_t index, uint64_t *value)
{
    if (model_missing) {
        return -1;
    }
    *value = *reg(index);
    return 0;
}

/** Optional writes can fault or succeed without retaining a value. */
int kcpu_wrmsr_safe(uint32_t index, uint64_t value)
{
    if (model_missing) {
        return -1;
    }
    if (!model_ignored) {
        *reg(index) = value;
    }
    return 0;
}

/** Find one of the independently set guest feature leaves. */
const struct kvm_cpuid_entry2 *kvm_cpuid_find(const struct kvm_vcpu *vcpu, uint32_t function,
                                              uint32_t index)
{
    uint32_t i;
    for (i = 0; i < vcpu->cpuid_nent; i++) {
        if (vcpu->cpuid[i].function == function && vcpu->cpuid[i].index == index) {
            return &vcpu->cpuid[i];
        }
    }
    return NULL;
}

/** Record invalid RDPMC's architectural exception. */
void kvm_queue_exception(struct kvm_vcpu *vcpu, uint8_t nr, int error, uint32_t code)
{
    (void)vcpu;
    CHECK(nr == 13 && error && code == 0, "RDPMC queues #GP(0)");
    fault_count++;
}

/** Record an overflow delivered under the big lock. */
void kvm_lapic_pmi(struct kvm_vcpu *vcpu)
{
    (void)vcpu;
    pmi_count++;
}

/** A replaced filter requests each vCPU to leave the old run. */
void kvm_vcpu_kick(struct kvm_vcpu *vcpu, uint32_t request)
{
    vcpu->requests |= request;
}

/** Consume a request at the next preparation. */
int kvm_vcpu_test_clear(struct kvm_vcpu *vcpu, uint32_t request)
{
    int set = (vcpu->requests & request) != 0;
    vcpu->requests &= ~request;
    return set;
}

/** Start a fresh physical PMU and two vCPUs on the same VM. */
static void setup(int intel, int ext, int v2)
{
    static const uint32_t leaves[] = {1, 7, 0xA, 0x80000001u, 0x80000022u};
    uint32_t i;
    kvm_pmu_vm_free(&vm);
    k_memset(&vm, 0, sizeof(vm));
    k_memset(&v, 0, sizeof(v));
    k_memset(&other, 0, sizeof(other));
    k_memset(cpuid, 0, sizeof(cpuid));
    k_memset(physical, 0, sizeof(physical));
    physical_n = 0;
    model_intel = intel;
    model_ext = ext;
    model_v2 = v2;
    model_missing = model_ignored = 0;
    kvm_backend = intel ? &kvm_vmx : &kvm_svm;
    kvm_pmu_start();
    for (i = 0; i < 5u; i++) {
        cpuid[i].function = leaves[i];
        kcpu_id(leaves[i], &cpuid[i].eax, &cpuid[i].ebx, &cpuid[i].ecx, &cpuid[i].edx);
        kvm_pmu_cpuid(&cpuid[i]);
    }
    v.vm = other.vm = &vm;
    v.cpuid = other.cpuid = cpuid;
    v.cpuid_nent = other.cpuid_nent = 5;
    kvm_pmu_init(&v);
    kvm_pmu_init(&other);
}

/** Check register widths, permissions, feature gating and reset. */
static void intel_registers(void)
{
    uint64_t value;
    uint32_t n, list[32];
    setup(1, 0, 0);
    CHECK(v.pmu.version == 2 && v.pmu.nr_gp == 4 && v.pmu.nr_fixed == 3,
          "Intel advertises version two, four general and three fixed counters");
    CHECK(v.pmu.global_ctrl == 15, "reset enables general counters globally");
    n = kvm_pmu_msr_list(list);
    CHECK(n == 16, "Intel migration list contains selectors, counters and controls");
    value = 0x80000001ull;
    CHECK(kvm_pmu_msr(&v, PMC0, 1, &value, 0) == 0 && v.pmu.counter[0] == 0xFFFF80000001ull,
          "guest legacy write sign extends");
    value = 0x123456789ABull;
    CHECK(kvm_pmu_msr(&v, PMC0, 1, &value, 1) == 0 && v.pmu.counter[0] == value,
          "migration preserves all counter bits");
    value = 1ull << 48;
    CHECK(kvm_pmu_msr(&v, FULL_PMC0, 1, &value, 0) == -1,
          "full-width writes reject bits outside width");
    value = 1ull << 21;
    CHECK(kvm_pmu_msr(&v, EVENTSEL0, 1, &value, 0) == -1, "anythread is reserved");
    value = EV_TXCP;
    CHECK(kvm_pmu_msr(&v, EVENTSEL0, 1, &value, 0) == -1 &&
              kvm_pmu_msr(&v, EVENTSEL0 + 2u, 1, &value, 0) == 0,
          "checkpointed transactions require counter two");
    value = 4;
    CHECK(kvm_pmu_msr(&v, FIXED_CTRL, 1, &value, 0) == -1, "fixed anythread is reserved");
    value = 0;
    CHECK(kvm_pmu_msr(&v, PERF_CAP, 1, &value, 0) == -1,
          "guest cannot write capabilities even to zero");
    CHECK(kvm_pmu_msr(&v, PERF_CAP, 1, &value, 1) == 0 &&
              kvm_pmu_msr(&v, FULL_PMC0, 0, &value, 0) == 1,
          "caller can remove full-width aliases");
    value = FW_WRITES;
    CHECK(kvm_pmu_msr(&v, PERF_CAP, 1, &value, 1) == 0, "caller can restore capabilities");
    v.has_run = 1;
    value = 0;
    CHECK(kvm_pmu_msr(&v, PERF_CAP, 1, &value, 1) == -1, "capabilities frozen after run");
    v.regs.rcx = 0;
    v.sregs.cr0 = 1;
    v.sregs.cs.selector = 3;
    CHECK(kvm_pmu_rdpmc(&v) == -1, "user RDPMC needs CR4.PCE");
    v.sregs.cr4 = 0x100;
    v.pmu.counter[0] = 0x123456789ABull;
    CHECK(kvm_pmu_rdpmc(&v) == 0 && v.regs.rax == 0x456789ABu && v.regs.rdx == 0x123,
          "RDPMC splits the full value and clears high halves");
    v.regs.rcx = (1u << 30) | 3u;
    CHECK(kvm_pmu_rdpmc(&v) == -1, "RDPMC bounds fixed counter indices");
    value = 1;
    CHECK(kvm_pmu_msr(&v, GLOBAL_STATUS, 1, &value, 0) == -1,
          "Intel global status is read only to the guest");
    CHECK(kvm_pmu_msr(&v, GLOBAL_STATUS, 1, &value, 1) == 0, "caller can restore status");
    CHECK(kvm_pmu_msr(&v, GLOBAL_OVF, 1, &value, 1) == 0 && v.pmu.global_status == 1,
          "migration overflow clear does not change status");
    CHECK(kvm_pmu_msr(&v, GLOBAL_OVF, 1, &value, 0) == 0 && !v.pmu.global_status,
          "guest overflow clear acknowledges the status");
    kvm_pmu_reset(&v);
    CHECK(!v.pmu.global_ctrl && !v.pmu.counter[0], "INIT clears the PMU");
}

/** Check the event filter independently of hardware events. */
static void filters(void)
{
    struct {
        struct kvm_pmu_event_filter head;
        uint64_t events[3];
    } f;
    struct kvm_enable_cap cap;
    setup(0, 1, 1);
    k_memset(&f, 0, sizeof(f));
    k_memset(&cap, 0, sizeof(cap));
    vm.nr_vcpus = 2;
    vm.vcpus[0] = &v;
    vm.vcpus[1] = &other;
    v.pmu.eventsel[0] = 0x2C0u | EV_ENABLE | EV_OS;
    f.head.nevents = 1;
    f.events[0] = 0x2C0u;
    CHECK(kvm_pmu_filter(&vm, (uintptr_t)&f) == 0 && live == 1 && (other.requests & KVM_REQ_PMU),
          "filter replacement kicks every vCPU");
    kvm_pmu_prepare(&v);
    CHECK(v.pmu.run_gp == 1 && !v.requests, "allow list admits an exact event");
    f.head.flags = KVM_PMU_EVENT_FLAG_MASKED_EVENTS;
    f.head.nevents = 2;
    f.events[0] = 0xC0u;
    f.events[1] = (0xFFull << 56) | FILTER_EXCLUDE | 0x2C0u;
    CHECK(kvm_pmu_filter(&vm, (uintptr_t)&f) == 0 && live == 1, "filter replace frees old copy");
    kvm_pmu_prepare(&v);
    CHECK(!v.pmu.run_gp, "masked exclusion wins over wildcard inclusion");
    f.head.action = KVM_PMU_EVENT_DENY;
    CHECK(kvm_pmu_filter(&vm, (uintptr_t)&f) == 0, "deny filter accepted");
    kvm_pmu_prepare(&v);
    CHECK(v.pmu.run_gp == 1, "deny inverts the complete match");
    f.events[0] |= 1ull << 16;
    CHECK(kvm_pmu_filter(&vm, (uintptr_t)&f) == -EINVAL && live == 1,
          "masked reserved bit leaves old filter intact");
    f.events[0] = 0xC0u;
    f.head.nevents = 301;
    CHECK(kvm_pmu_filter(&vm, (uintptr_t)&f) == -E2BIG, "filter bound is Linux's 300");
    f.head.nevents = 2;
    alloc_fail = 1;
    CHECK(kvm_pmu_filter(&vm, (uintptr_t)&f) == -ENOMEM, "allocation failure keeps old filter");
    alloc_fail = 0;
    copy_fail = 2;
    CHECK(kvm_pmu_filter(&vm, (uintptr_t)&f) == -EFAULT && live == 1,
          "event-copy fault returns the allocation");
    CHECK(kvm_pmu_enable(&vm, &cap) == -EINVAL, "PMU capability precedes all vCPUs");
    vm.nr_vcpus = 0;
    cap.args[0] = KVM_PMU_CAP_DISABLE;
    CHECK(kvm_pmu_enable(&vm, &cap) == 0, "caller disables PMU");
    kvm_pmu_refresh(&v);
    CHECK(!v.pmu.version && !v.pmu.nr_gp, "disabled VM has no guest counters");
    kvm_pmu_vm_free(&vm);
    CHECK(!live, "VM frees final filter");
}

/** Apply an architectural VMX MSR load list. */
static void load_list(const struct kvm_pmu_msr *list, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        CHECK(!list[i].reserved, "VMX MSR-list reserved field is zero");
        *reg(list[i].index) = list[i].value;
    }
}

/** Drive VMX's production list builder and collector with guest counts. */
static void intel_switch(void)
{
    struct kvm_pmu_msr guest[24], host[24];
    uint32_t ng, nh, i;
    int before = pmi_count;
    setup(1, 0, 0);
    v.pmu.eventsel[0] = 0xC0u | EV_ENABLE | EV_OS | EV_INT;
    v.pmu.counter[0] = v.pmu.gp_mask - 3;
    v.pmu.global_ctrl |= 1ull << 32;
    v.pmu.fixed_ctrl = 9;
    v.pmu.fixed[0] = v.pmu.fixed_mask - 2;
    kvm_pmu_prepare(&v);
    kvm_pmu_vmx_enter(&v, guest, host, &ng, &nh);
    CHECK(ng <= 24 && nh <= 24, "VMX lists fit caller's room");
    load_list(guest, ng);
    CHECK(!(*reg(EVENTSEL0) & EV_INT) && !(*reg(FIXED_CTRL) & 8),
          "no physical performance interrupt is armed");
    CHECK(*reg(GLOBAL_CTRL) == ((1ull << 32) | 1), "only chosen guest counters run");
    *reg(PMC0) = 5;
    *reg(FIXED0) = 7;
    for (i = 0; i < ng; i++) {
        guest[i].value = *reg(guest[i].index);
    }
    load_list(host, nh);
    CHECK(!*reg(GLOBAL_CTRL) && !*reg(EVENTSEL0) && !*reg(FIXED_CTRL),
          "host load list stops every counter");
    kvm_pmu_vmx_exit(&v, guest);
    CHECK(v.pmu.counter[0] == 1 && v.pmu.fixed[0] == 4, "hardware deltas wrap at the guest widths");
    CHECK(v.pmu.global_status == ((1ull << 32) | 1) && pmi_count == before,
          "exit queues status but does not deliver outside the big lock");
    kvm_pmu_prepare(&v);
    CHECK(pmi_count == before + 1, "next preparation delivers the pending PMI");
    kvm_pmu_prepare(&v);
    CHECK(pmi_count == before + 1, "one overflow batch sends one PMI");
    CHECK(!other.pmu.counter[0], "another vCPU has independent counters");
    kvm_pmu_vmx_enter(&other, guest, host, &ng, &nh);
    load_list(guest, ng);
    CHECK(!*reg(PMC0) && !*reg(GLOBAL_CTRL), "another vCPU starts with no physical residue");
    load_list(host, nh);
}

/** Drive AMD's legacy, extended and PerfMonV2 switching and its absence. */
static void amd_switch(void)
{
    uint64_t value;
    uint32_t mode;
    for (mode = 0; mode < 3u; mode++) {
        setup(0, mode != 0, mode == 2);
        CHECK(v.pmu.nr_gp == (mode ? 6u : 4u), "AMD counter count follows CPUID");
        value = ~0ull;
        CHECK(kvm_pmu_msr(&v, AMD_CTL0, 1, &value, 0) == 0 &&
                  !(v.pmu.eventsel[0] & 0xFFFFFFF000280000ull),
              "AMD ignores reserved selector bits");
        value = 0x123;
        CHECK(kvm_pmu_msr(&v, AMD_CTR0, 1, &value, 0) == 0, "AMD legacy counter is writable");
        if (mode) {
            value = 0;
            CHECK(kvm_pmu_msr(&v, AMD_CORE_CTR0, 0, &value, 0) == 0 && value == 0x123,
                  "AMD extended and legacy names alias one counter");
        }
        v.pmu.counter[0] = 0;
        v.pmu.eventsel[0] = 0xC0u | EV_ENABLE | EV_OS | EV_INT;
        kvm_pmu_prepare(&v);
        kvm_pmu_svm_enter(&v);
        CHECK((*reg(amd_index(0, 0)) & (EV_GUEST | EV_INT)) == EV_GUEST,
              "AMD counts guest execution without a physical interrupt");
        *reg(amd_index(0, 1)) = 17;
        kvm_pmu_svm_exit(&v);
        CHECK(v.pmu.counter[0] == 17 && !*reg(amd_index(0, 0)) && !*reg(amd_index(0, 1)),
              "AMD collects and clears physical registers");
        kvm_pmu_svm_enter(&other);
        *reg(amd_index(0, 1)) = 90;
        kvm_pmu_svm_exit(&other);
        CHECK(!other.pmu.counter[0] && v.pmu.counter[0] == 17,
              "a disabled vCPU cannot inherit another's events");
        if (mode == 2) {
            value = ~0ull;
            CHECK(kvm_pmu_msr(&v, AMD_GLOBAL, 1, &value, 0) == 0 && v.pmu.global_ctrl == 63,
                  "AMD global control drops reserved bits");
        }
    }
    model_missing = 1;
    kvm_pmu_start();
    CHECK(!kvm_pmu_enabled() && irq_flags == 0x200u, "faulting MSRs disable PMU and restore flags");
    model_missing = 0;
    model_ignored = 1;
    kvm_pmu_start();
    CHECK(!kvm_pmu_enabled(), "ignored TCG MSRs do not advertise working counters");
}

/** Run each model and leave no heap allocation. */
int main(void)
{
    intel_registers();
    filters();
    intel_switch();
    amd_switch();
    kvm_pmu_vm_free(&vm);
    CHECK(!live, "all allocations returned");
    printf("kvm-pmu-hosttest: result: %s\n", failures ? "fail" : "pass");
    return failures ? 1 : 0;
}
