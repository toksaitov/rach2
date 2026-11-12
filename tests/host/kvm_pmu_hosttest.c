/* Check guest PMUs against Intel and AMD register models.
 * host-cflags: -DRACH2_HOST_X86_64 -DRACH2_HOST_CPU
 */
#include "kvm/pmu.c"

extern int printf(const char *format, ...);
extern void *malloc(unsigned long n);
extern void free(void *p);

static int failures, live, alloc_fail, copy_fail, pmi_count, fault_count;
static int model_intel, model_ext, model_v2, model_missing, model_ignored, model_lbr, model_notsx;
static int model_hybrid, model_bld;
static uint32_t model_amd_sig; /* AMD's leaf 1 signature, 0 for family 0 */
static uint32_t irq_flags = 0x200u;
static uint64_t physical[160];
static uint32_t physical_index[160], physical_n;
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
        *a = 0x40651u; /* the Lenovo's Haswell: family 6, model 0x45 */
        *c = PDCM;
    }
    if (leaf == 1 && !model_intel) {
        *a = model_amd_sig;
    }
    if (leaf == 0xA && model_intel) {
        *a = 0x07300403u;
        *d = 3u | (48u << 5);
    }
    if (leaf == 7 && model_intel) {
        *b = model_notsx ? 0 : 1u << 11;
        *c = model_bld ? L7_BLD : 0;
        *d = model_hybrid ? L7_HYBRID : 0;
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
    CHECK(physical_n < 160u, "physical register table fits");
    if (physical_n == 160u) {
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

/** Optional writes can fault or succeed without retaining a value. An
 * LBR_FROM of format 4 without TSX faults unless bits 62:61 repeat bits
 * 60:59, as Linux's lbr_from_signext_quirk_wr allows for. */
int kcpu_wrmsr_safe(uint32_t index, uint64_t value)
{
    if (model_missing) {
        return -1;
    }
    if (model_lbr && model_notsx && index - LBR_FROM < 16u &&
        ((value >> 61) & 3u) != ((value >> 59) & 3u)) {
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
    if (intel && model_lbr) {
        *reg(PERF_CAP) = FW_WRITES | 4u;
    }
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

/** Find an MSR in a VMX list, or -1. */
static int list_find(const struct kvm_pmu_msr *list, uint32_t n, uint32_t index)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        if (list[i].index == index) {
            return (int)i;
        }
    }
    return -1;
}

/** Drive VMX's switching beside the host's watchdog on fixed counter 1:
 * the VMCS's global control, no counter written, deltas read after the
 * exit, and each guest counter on a physical counter its event may use. */
static void intel_switch(void)
{
    struct kvm_pmu_msr guest[KVM_PMU_LIST_MAX], host[KVM_PMU_LIST_MAX];
    uint64_t global[2], watch_global = FIXED_BIT(1);
    uint32_t ng, nh;
    int before = pmi_count;
    setup(1, 0, 0);
    CHECK(pmu_host.table == c_hsw && pmu_host.cp && pmu_host.fixed0_last,
          "Haswell's model takes Haswell's table, with fixed counter 0 placed last");
    *reg(GLOBAL_CTRL) = watch_global;
    *reg(FIXED_CTRL) = 0xB0u;
    *reg(FIXED0 + 1u) = 1000;
    *reg(PMC0) = 300;
    *reg(PMC0 + 1u) = 200;
    *reg(PMC0 + 2u) = 100;
    /* L1D_PEND_MISS.PENDING counts only on counter 2. */
    v.pmu.eventsel[0] = 0x148u | EV_ENABLE | EV_OS | EV_INT;
    v.pmu.counter[0] = v.pmu.gp_mask - 3;
    v.pmu.global_ctrl |= FIXED_BIT(0) | FIXED_BIT(1);
    v.pmu.fixed_ctrl = 9u | (3u << 4);
    v.pmu.fixed[0] = v.pmu.fixed_mask - 2;
    kvm_pmu_prepare(&v);
    kvm_pmu_vmx_enter(&v, guest, host, &ng, &nh, global);
    CHECK(!ng && !nh, "the VMCS switches the global control without lists");
    CHECK(v.pmu.phys[0] == 2, "a constrained event runs on the counter it may use");
    CHECK(v.pmu.phys[KVM_PMU_GP_MAX + 1u] == 0,
          "fixed counter 1, which the host holds, runs on a general counter");
    CHECK(v.pmu.phys[KVM_PMU_GP_MAX] == 1,
          "and fixed counter 0, on Haswell, on a free general counter before its own");
    CHECK(global[0] == (watch_global | 7u) && global[1] == watch_global,
          "the guest's counters run beside the host's fixed counter 1");
    CHECK(*reg(FIXED0 + 1u) == 1000, "the host's counter is left alone");
    CHECK(*reg(EVENTSEL0 + 2u) == (0x148u | EV_ENABLE | EV_OS) &&
              *reg(EVENTSEL0) == (0x3Cu | EV_ENABLE | EV_OS | EV_USR) &&
              *reg(EVENTSEL0 + 1u) == (0xC0u | EV_ENABLE | EV_OS) && *reg(FIXED_CTRL) == 0xB0u,
          "selectors are set without a physical performance interrupt, and the host's field kept");
    CHECK(*reg(PMC0 + 2u) == 100 && *reg(PMC0) == 300 && *reg(PMC0 + 1u) == 200,
          "no counter is written at an entry");
    *reg(PMC0 + 2u) += 5;
    *reg(PMC0 + 1u) += 7;
    *reg(PMC0) += 11;
    kvm_pmu_vmx_exit(&v, guest, 1);
    CHECK(v.pmu.counter[0] == 1 && v.pmu.fixed[0] == 4 && v.pmu.fixed[1] == 11,
          "deltas wrap at the guest widths");
    CHECK(!*reg(EVENTSEL0 + 2u) && !*reg(EVENTSEL0) && !*reg(EVENTSEL0 + 1u) &&
              (*reg(FIXED_CTRL) & 0xF0u) == 0xB0u,
          "the exit releases KVM's selectors and keeps the host's field");
    CHECK(v.pmu.global_status == (FIXED_BIT(0) | 1) && pmi_count == before,
          "exit queues status but does not deliver outside the big lock");
    kvm_pmu_prepare(&v);
    CHECK(pmi_count == before + 1, "next preparation delivers the pending PMI");
    kvm_pmu_prepare(&v);
    CHECK(pmi_count == before + 1, "one overflow batch sends one PMI");
    kvm_pmu_vmx_enter(&v, guest, host, &ng, &nh, NULL);
    CHECK(list_find(guest, ng, GLOBAL_CTRL) >= 0 && list_find(host, nh, GLOBAL_CTRL) >= 0 &&
              guest[list_find(guest, ng, GLOBAL_CTRL)].value == (watch_global | 7u) &&
              host[list_find(host, nh, GLOBAL_CTRL)].value == watch_global,
          "without the VMCS's controls the lists switch the global control");
    *reg(PMC0 + 2u) += 9;
    kvm_pmu_vmx_exit(&v, guest, 0);
    CHECK(v.pmu.counter[0] == 1 && !*reg(EVENTSEL0 + 2u),
          "a failed entry counts nothing and still releases the selector");
    model_hybrid = 1;
    setup(1, 0, 0);
    CHECK(!kvm_pmu_enabled() && !v.pmu.version, "a hybrid processor's guests have no PMU");
    model_hybrid = 0;
}

/** Drive the placement beside the host's fixed counter 1: a constrained
 * event that finds its counter taken, instructions on Haswell's general
 * counters before fixed counter 0 and on fixed counter 0 first elsewhere,
 * an inverted count kept off it, and counters the host does not hold. */
static void intel_placement(void)
{
    struct kvm_pmu_msr guest[KVM_PMU_LIST_MAX], host[KVM_PMU_LIST_MAX];
    uint64_t global[2], watch_global = FIXED_BIT(1);
    uint32_t ng, nh;
    setup(1, 0, 0);
    *reg(GLOBAL_CTRL) = watch_global;
    *reg(FIXED_CTRL) = 0xB0u;
    v.pmu.eventsel[0] = 0x148u | EV_ENABLE | EV_OS | EV_INT;
    v.pmu.eventsel[1] = v.pmu.eventsel[2] = v.pmu.eventsel[3] = v.pmu.eventsel[0];
    v.pmu.global_ctrl = 15;
    kvm_pmu_prepare(&v);
    kvm_pmu_vmx_enter(&v, guest, host, &ng, &nh, global);
    CHECK(v.pmu.run_gp == 1 && global[0] == (watch_global | 4u),
          "a counter left without a counter its event may use does not run");
    kvm_pmu_vmx_exit(&v, guest, 1);
    v.pmu.eventsel[0] = v.pmu.eventsel[1] = 0xC0u | EV_ENABLE | EV_USR;
    v.pmu.eventsel[2] = 0xC0u | EV_ENABLE | EV_USR | EV_INVERT;
    v.pmu.eventsel[3] = 0xC0u | EV_ENABLE | EV_USR;
    v.pmu.global_ctrl = 15u | FIXED_BIT(0);
    v.pmu.fixed_ctrl = 2;
    kvm_pmu_prepare(&v);
    kvm_pmu_vmx_enter(&v, guest, host, &ng, &nh, global);
    CHECK(v.pmu.run_gp == 15 && v.pmu.run_fixed == 1 && v.pmu.phys[2] == 0 && v.pmu.phys[0] == 1 &&
              v.pmu.phys[1] == 2 && v.pmu.phys[3] == 3 && v.pmu.phys[KVM_PMU_GP_MAX] == 32u,
          "on Haswell instructions take the general counters first and fixed counter 0 last, "
          "and an inverted count a general one");
    CHECK(*reg(FIXED_CTRL) == 0xB2u, "the guest's fixed field counts in its modes");
    kvm_pmu_vmx_exit(&v, guest, 1);
    pmu_host.fixed0_last = 0;
    v.pmu.global_ctrl = 7u | FIXED_BIT(0);
    kvm_pmu_prepare(&v);
    kvm_pmu_vmx_enter(&v, guest, host, &ng, &nh, global);
    CHECK(v.pmu.run_gp == 7 && v.pmu.run_fixed == 1 && v.pmu.phys[2] == 0 && v.pmu.phys[0] == 32u &&
              v.pmu.phys[1] == 1 && v.pmu.phys[KVM_PMU_GP_MAX] == 2,
          "elsewhere instructions take fixed counter 0 first, as Linux's perf places them");
    CHECK(*reg(FIXED_CTRL) == 0xB2u,
          "a general counter's event counts in its modes on a fixed one");
    kvm_pmu_vmx_exit(&v, guest, 1);
    pmu_host.fixed0_last = 1;
    v.pmu.eventsel[3] = 0;
    *reg(GLOBAL_CTRL) = watch_global | 15u | FIXED_BIT(0);
    *reg(FIXED_CTRL) = 0;
    v.pmu.eventsel[0] = 0xC4u | EV_ENABLE | EV_OS;
    v.pmu.global_ctrl = 1;
    kvm_pmu_prepare(&v);
    kvm_pmu_vmx_enter(&v, guest, host, &ng, &nh, global);
    CHECK(!*reg(GLOBAL_CTRL) && global[1] == 0 && global[0] == 1 && v.pmu.phys[0] == 0,
          "counters the host does not hold stop in the host and are the guest's");
    kvm_pmu_vmx_exit(&v, guest, 1);
    CHECK(!other.pmu.counter[0], "another vCPU has independent counters");
    *reg(GLOBAL_CTRL) = watch_global;
    *reg(FIXED_CTRL) = 0xB0u;
    v.pmu.eventsel[0] = 0;
    v.pmu.eventsel[2] = 0x3Cu | EV_TXCP | EV_ENABLE | EV_OS;
    v.pmu.global_ctrl = 4;
    kvm_pmu_prepare(&v);
    kvm_pmu_vmx_enter(&v, guest, host, &ng, &nh, global);
    CHECK(v.pmu.run_gp == 4 && v.pmu.phys[2] == 2 && global[0] == (watch_global | 4u),
          "a TSX-checkpointed event runs on counter 2 alone");
    kvm_pmu_vmx_exit(&v, guest, 1);
    *reg(GLOBAL_CTRL) = watch_global | 4u;
    *reg(EVENTSEL0 + 2u) = 0x3Cu | EV_ENABLE | EV_OS;
    kvm_pmu_prepare(&v);
    kvm_pmu_vmx_enter(&v, guest, host, &ng, &nh, global);
    CHECK(!v.pmu.run_gp && global[0] == (watch_global | 4u),
          "and does not run while the host holds counter 2");
    kvm_pmu_vmx_exit(&v, guest, 1);
}

/** Drive the guest's LBR stack: PERF_CAPABILITIES, DEBUGCTL, its MSRs,
 * the lists while DEBUGCTL.LBR is set, and the freeze on a PMI. */
static void intel_lbr(void)
{
    struct kvm_pmu_msr guest[KVM_PMU_LIST_MAX], host[KVM_PMU_LIST_MAX];
    uint64_t value, global[2];
    uint32_t ng, nh;
    int k;
    model_lbr = 1;
    model_notsx = 1;
    setup(1, 0, 0);
    CHECK(kvm_pmu_feature(PERF_CAP, &value) == 0 && value == (FW_WRITES | 4u),
          "PERF_CAPABILITIES offers the host's LBR format");
    CHECK(v.pmu.lbr_nr == 16 && pmu_host.lbr_quirk, "Haswell's 16 records, without TSX");
    value = FW_WRITES | 5u;
    CHECK(kvm_pmu_msr(&v, PERF_CAP, 1, &value, 1) == -1, "another LBR format is refused");
    value = FW_WRITES;
    CHECK(kvm_pmu_msr(&v, PERF_CAP, 1, &value, 1) == 0 && !v.pmu.lbr_nr &&
              kvm_pmu_msr(&v, LBR_TOS, 0, &value, 0) == 1,
          "format 0 removes the stack");
    value = DEBUGCTL_FRZ;
    CHECK(kvm_pmu_msr(&v, DEBUGCTL, 1, &value, 0) == -1, "FREEZE needs the stack");
    value = DEBUGCTL_LBR | DEBUGCTL_BTF;
    CHECK(kvm_pmu_msr(&v, DEBUGCTL, 1, &value, 0) == 0 && !v.pmu.debugctl,
          "LBR and BTF without the stack are dropped");
    cpuid[0].eax = 0x306C3u;
    value = FW_WRITES | 4u;
    CHECK(kvm_pmu_msr(&v, PERF_CAP, 1, &value, 1) == -1, "the format needs the host's model");
    cpuid[0].eax = 0x40651u;
    CHECK(kvm_pmu_msr(&v, PERF_CAP, 1, &value, 1) == 0 && v.pmu.lbr_nr == 16,
          "the host's format restores the stack");
    value = DEBUGCTL_LBR | DEBUGCTL_FRZ | DEBUGCTL_BTF;
    CHECK(kvm_pmu_msr(&v, DEBUGCTL, 1, &value, 0) == 0 &&
              v.pmu.debugctl == (DEBUGCTL_LBR | DEBUGCTL_FRZ),
          "the guest sets LBR and FREEZE_LBRS_ON_PMI");
    value = DEBUGCTL_BLD;
    CHECK(kvm_pmu_msr(&v, DEBUGCTL, 1, &value, 1) == -1,
          "BUS_LOCK_DETECT without the host's is #GP, the caller's too");
    value = DEBUGCTL_RTM;
    CHECK(kvm_pmu_msr(&v, DEBUGCTL, 1, &value, 1) == -1, "and RTM_DEBUG without TSX");
    value = 1ull << 3;
    CHECK(kvm_pmu_msr(&v, DEBUGCTL, 1, &value, 0) == -1, "another DEBUGCTL bit is #GP");
    value = 7;
    CHECK(kvm_pmu_msr(&v, LBR_TOS, 1, &value, 1) == 0 && *reg(LBR_TOS) == 7 && v.pmu.lbr_tos == 7,
          "the caller writes LBR_TOS through the processor");
    value = 0x1FFFFFFF81000000ull;
    CHECK(kvm_pmu_msr(&v, LBR_FROM + 15u, 1, &value, 0) == -1,
          "the guest's LBR_FROM without its sign bits faults, as on the processor");
    CHECK(kvm_pmu_msr(&v, LBR_FROM + 15u, 1, &value, 1) == 0 &&
              v.pmu.lbr_from[15] == 0x1FFFFFFF81000000ull,
          "the caller's write of a value a read returned is kept as read");
    CHECK(kvm_pmu_msr(&v, LBR_FROM + 16u, 0, &value, 0) == 1, "the stack has exactly 16 records");
    kvm_pmu_prepare(&v);
    kvm_pmu_vmx_enter(&v, guest, host, &ng, &nh, global);
    k = list_find(guest, ng, LBR_FROM + 15u);
    CHECK(ng == 34 && !nh && v.pmu.lbr_list == 0 && k >= 0 &&
              guest[k].value == 0x7FFFFFFF81000000ull,
          "the lists carry the stack, LBR_FROM sign-extended into bits 62:61");
    guest[list_find(guest, ng, LBR_TO + 2u)].value = 0x401000;
    guest[list_find(guest, ng, LBR_TOS)].value = 2;
    kvm_pmu_vmx_exit(&v, guest, 1);
    CHECK(kvm_pmu_msr(&v, LBR_TO + 2u, 0, &value, 0) == 0 && value == 0x401000 &&
              v.pmu.lbr_tos == 2 && v.pmu.lbr_list == ~0u,
          "the exit's stored stack is the guest's");
    v.pmu.pmi = 1;
    kvm_pmu_prepare(&v);
    CHECK(v.pmu.debugctl == DEBUGCTL_FRZ, "a PMI under FREEZE_LBRS_ON_PMI clears DEBUGCTL.LBR");
    kvm_pmu_vmx_enter(&v, guest, host, &ng, &nh, global);
    CHECK(!ng && v.pmu.lbr_list == ~0u, "a stopped stack stays out of the lists");
    kvm_pmu_vmx_exit(&v, guest, 1);
    model_lbr = model_notsx = 0;
    model_bld = 1;
    setup(1, 0, 0);
    value = DEBUGCTL_BLD | DEBUGCTL_RTM;
    CHECK(
        kvm_pmu_msr(&v, DEBUGCTL, 1, &value, 0) == 0 &&
            v.pmu.debugctl == (DEBUGCTL_BLD | DEBUGCTL_RTM) &&
            kvm_pmu_vmcs_debugctl(&v) == (DEBUGCTL_BLD | DEBUGCTL_RTM),
        "BUS_LOCK_DETECT and RTM_DEBUG where the host has them and the guest's CPUID offers them");
    cpuid[1].ebx &= ~L7_RTM;
    cpuid[1].ecx &= ~L7_BLD;
    value = DEBUGCTL_BLD;
    CHECK(kvm_pmu_msr(&v, DEBUGCTL, 1, &value, 0) == -1, "BUS_LOCK_DETECT needs the guest's CPUID");
    value = DEBUGCTL_RTM;
    CHECK(kvm_pmu_msr(&v, DEBUGCTL, 1, &value, 0) == -1, "and RTM_DEBUG");
    value = DEBUGCTL_BLD | DEBUGCTL_RTM;
    CHECK(kvm_pmu_msr(&v, DEBUGCTL, 1, &value, 1) == 0 &&
              v.pmu.debugctl == (DEBUGCTL_BLD | DEBUGCTL_RTM),
          "the caller sets both without the guest's CPUID");
    model_bld = 0;
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
        CHECK(v.pmu.counter[0] == 17 && !*reg(amd_index(0, 0)),
              "AMD collects the delta and releases the selector");
        if (mode) {
            *reg(amd_index(0, 0)) = 0x76u | EV_ENABLE | EV_INT;
            kvm_pmu_svm_enter(&v);
            CHECK(*reg(amd_index(0, 0)) == (0x76u | EV_ENABLE | EV_INT) && v.pmu.phys[0] == 1 &&
                      (*reg(amd_index(1, 0)) & EV_GUEST),
                  "AMD leaves the host's counter and runs the guest's on the next");
            *reg(amd_index(1, 1)) += 3;
            kvm_pmu_svm_exit(&v);
            CHECK(v.pmu.counter[0] == 20 && !*reg(amd_index(1, 0)) &&
                      *reg(amd_index(0, 0)) == (0x76u | EV_ENABLE | EV_INT),
                  "AMD counts on the spare counter and leaves the host's running");
            *reg(amd_index(0, 0)) = 0;
            v.pmu.counter[0] = 17;
        }
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

/** Drive AMD's event constraints: family 15h's counters beside the
 * detector's core counter 2, and family 17h's merged pair beside its
 * counter 0. */
static void amd_constraints(void)
{
    uint64_t watch = 0x76u | EV_ENABLE | EV_INT;
    model_amd_sig = 0x00600F12u; /* family 15h */
    setup(0, 1, 0);
    CHECK(pmu_host.family == 0x15u, "family 15h is read from leaf 1");
    *reg(amd_index(2, 0)) = watch;
    v.pmu.eventsel[0] = 0x02Du | EV_ENABLE | EV_OS;
    v.pmu.eventsel[1] = 0x87u | (1ull << 32) | EV_ENABLE | EV_OS; /* event 0x187 */
    v.pmu.eventsel[2] = 0x076u | EV_ENABLE | EV_OS;
    kvm_pmu_prepare(&v);
    kvm_pmu_svm_enter(&v);
    CHECK(v.pmu.run_gp == 7 && v.pmu.phys[0] == 3 && v.pmu.phys[1] == 0 && v.pmu.phys[2] == 1,
          "family 15h's events run on their counters, counter 0's beside the detector");
    kvm_pmu_svm_exit(&v);
    CHECK(*reg(amd_index(2, 0)) == watch && !*reg(amd_index(3, 0)) && !*reg(amd_index(0, 0)),
          "the host's counter is kept");
    model_amd_sig = 0x00800F11u; /* family 17h */
    setup(0, 1, 0);
    *reg(amd_index(0, 0)) = watch;
    v.pmu.eventsel[0] = 0x0C0u | EV_ENABLE | EV_OS;
    v.pmu.eventsel[1] = 0x003u | EV_ENABLE | EV_OS;
    kvm_pmu_prepare(&v);
    kvm_pmu_svm_enter(&v);
    CHECK(v.pmu.phys[1] == 2 && v.pmu.pair == 2 && *reg(amd_index(3, 0)) == AMD_MERGE &&
              v.pmu.phys[0] == 1,
          "a large-increment event takes an even counter merged with the next");
    *reg(amd_index(2, 1)) += 40;
    kvm_pmu_svm_exit(&v);
    CHECK(v.pmu.counter[1] == 40 && !*reg(amd_index(3, 0)) && !*reg(amd_index(2, 0)),
          "the pair's even counter is read and both are released");
    model_amd_sig = 0;
}

/** Run each model and leave no heap allocation. */
int main(void)
{
    intel_registers();
    filters();
    intel_switch();
    intel_placement();
    intel_lbr();
    amd_switch();
    amd_constraints();
    kvm_pmu_vm_free(&vm);
    CHECK(!live, "all allocations returned");
    printf("kvm-pmu-hosttest: result: %s\n", failures ? "fail" : "pass");
    return failures ? 1 : 0;
}
