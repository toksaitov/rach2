/**
 * Check KVM's Hyper-V enlightenments, rach2/sys/kvm/hyperv.c, against the
 * TLFS and Linux's arch/x86/kvm/hyperv.c.
 *
 * host-deps: tests/host/compat/atomic_host.c
 * host-cflags: -DRACH2_HOST_X86_64 -DRACH2_HOST_CPU
 *
 * hyperv.c is the kernel's, included whole. The VM is a record of three
 * vCPUs made here, with the split controller. The guest's memory is 64 KiB
 * from address 0, and an access past it is one in no slot. The APIC is a
 * fake that records the vectors raised on each vCPU and the registers of
 * the APIC MSRs; the clocks are one model, the host's ns and a TSC of
 * 2.4 GHz; the eventfds are four fakes that count their signals and
 * references.
 *
 * The rows: KVM_GET_SUPPORTED_HV_CPUID's ten leaves, with and without the
 * APIC in the kernel, and its refusals; the capabilities and
 * KVM_ENABLE_CAP's answers; each MSR, from the guest and from the caller,
 * before and after the CPUID names Hyper-V and under
 * KVM_CAP_HYPERV_ENFORCE_CPUID: the guest's id and the hypercall page on VMX
 * and SVM, the VP index, the reference counter and the TSC page by Linux's
 * formula, left at sequence 0 without the master clock, the frequencies, the
 * crash and reset MSRs ending the run, the APIC MSRs, the VP assist page,
 * the runtime, the invariant TSC, the synthetic debugger and the SynIC's;
 * the SINTs raised through a route, their vectors and auto-EOI; the
 * synthetic timers one-shot, periodic, auto-enabled, direct, held by a full
 * slot and retried at EOM and at EOI; the hypercalls' #UD, statuses, 32-bit
 * results, fast and XMM input, the XMM registers read only once synced from
 * the processor to the image, the exits to the caller and their completion,
 * HvSignalEvent over KVM_HYPERV_EVENTFD, the extended and debug hypercalls;
 * the IPI and TLB flush hypercalls with simple and sparse sets; each entry
 * point that KVM_RUN calls without the big lock taking it once, around the
 * work it covers, and giving it back, checked apart; and every allocation
 * and eventfd reference given back.
 */
#include "kvm/hyperv.c"

extern int printf(const char *format, ...);
extern void *malloc(unsigned long n);
extern void free(void *p);

static int failures;

/** Count and print a failure when `condition` is false. */
#define CHECK(condition, detail)                                 \
    do {                                                         \
        if (!(condition)) {                                      \
            printf("kvm-hyperv-hosttest: failed: %s\n", detail); \
            failures++;                                          \
        }                                                        \
    } while (0)

static process_t caller;
struct percpu cpu_table[NR_CPUS] = {{.current = &caller}};
unsigned int host_cpu;

/* The entry points that take the big lock around hyperv.c's state, each
 * checked apart: a call begins and ends with the lock free and takes it at
 * most once (`ep_takes`), and the work the lock covers, the stubs that call
 * `ep_work`, runs at depth 1. `ep_bad` counts the breaches. */
enum { EP_MSR, EP_HCALL, EP_COMPLETE, EP_PREPARE, EP_STIMERS, EP_N };
static int bkl_depth, bkl_takes, ep_now = -1, ep_start;
static int ep_takes[EP_N], ep_work_runs[EP_N], ep_bad[EP_N];

/** Take the big lock for `v`'s exit: one level deeper. */
void kvm_bkl_enter(struct kvm_vcpu *v)
{
    (void)v;
    bkl_depth++;
    bkl_takes++;
}

/** Give back one level of the big lock. */
void bkl_exit(void)
{
    bkl_depth--;
}

/** Note that the entry point running, if any, has reached work its lock
 * covers. */
static void ep_work(void)
{
    if (ep_now >= 0) {
        ep_work_runs[ep_now]++;
        ep_bad[ep_now] += bkl_depth != 1;
    }
}

/** Begin a call of the entry point `ep`, with the big lock free. */
static void ep_begin(int ep)
{
    ep_bad[ep] += bkl_depth != 0;
    ep_now = ep;
    ep_start = bkl_takes;
}

/** End the call `ep_begin` began: the lock free again, taken at most once. */
static void ep_end(void)
{
    ep_bad[ep_now] += bkl_depth != 0 || bkl_takes - ep_start > 1;
    ep_takes[ep_now] += bkl_takes - ep_start;
    ep_now = -1;
}

/* The heap's live allocations, which the VM's end must bring back. */
static int live;

void *kmalloc(size_t size)
{
    live++;
    return malloc(size ? size : 1);
}

void *kzalloc(size_t size)
{
    void *p = kmalloc(size);

    k_memset(p, 0, size);
    return p;
}

void kfree(void *p)
{
    if (p) {
        live--;
        free(p);
    }
}

void *k_memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;

    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

void *k_memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;

    while (n--) {
        *d++ = (unsigned char)c;
    }
    return dst;
}

/* The caller's memory is the host's; NULL is an address not mapped. */
int copy_to_user(void __user *dst, const void *src, size_t n)
{
    if (!dst) {
        return -EFAULT;
    }
    k_memcpy((void *)dst, src, n);
    return 0;
}

int copy_from_user(void *dst, const void __user *src, size_t n)
{
    if (!src) {
        return -EFAULT;
    }
    k_memcpy(dst, (const void *)src, n);
    return 0;
}

void arch_relax(void)
{}

/* The clocks: the host's ns, and a TSC of 2.4 GHz from `tsc_base`. */
static uint64_t mono_ns = 5000000000ull;
static const uint64_t tsc_base = 777;
static uint32_t tsc_per_tick = 2400000000u / TICK_HZ;

uint64_t kvm_host_ns(void)
{
    return mono_ns;
}

/** vcpu.c's lowering of the vCPU's timer deadline, which the rows read. */
void kvm_timer_arm(struct kvm_vcpu *v, uint64_t ns)
{
    if (ns && (!v->timer_ns || ns < v->timer_ns)) {
        v->timer_ns = ns;
    }
}

uint64_t arch_cycles(void)
{
    return tsc_base + mono_ns / 10u * 24u;
}

uint32_t timer_calibrate(void)
{
    return tsc_per_tick;
}

/* pvclock.c's guest TSC and its rate, which no vCPU here has scaled. */
uint64_t kvm_tsc_read(const struct kvm_vcpu *v)
{
    return arch_cycles() + (uint64_t)v->tsc_offset;
}

uint64_t kvm_tsc_hz(const struct kvm_vcpu *v)
{
    (void)v;
    return (uint64_t)tsc_per_tick * TICK_HZ;
}

/* The kvmclock, the host's ns plus the VM's offset, and whether it runs
 * from the master clock, which the reference TSC page needs. */
static int master_on = 1;

uint64_t kvm_clock_ns(struct kvm_vm *vm)
{
    return kvm_host_ns() + (uint64_t)vm->clock_offset;
}

int kvm_clock_stable_read(struct kvm_vm *vm, uint64_t *ns, uint64_t *host_tsc)
{
    if (!master_on) {
        return 0;
    }
    *host_tsc = arch_cycles();
    *ns = kvm_clock_ns(vm);
    return 1;
}

uint64_t kvm_tsc_scale(const struct kvm_vcpu *v, uint64_t tsc)
{
    (void)v;
    return tsc;
}

/* The backends: only their addresses count. */
const struct kvm_backend kvm_svm, kvm_vmx;
const struct kvm_backend *kvm_backend = &kvm_vmx;

/* The guest's memory, 64 KiB from 0. */
#define MEM_BYTES 0x10000u
static uint8_t mem[MEM_BYTES];

int kvm_guest_phys_rw(struct kvm_vcpu *v, uint64_t gpa, void *buf, uint32_t n, int write)
{
    (void)v;
    ep_work();
    if (gpa >= MEM_BYTES || n > MEM_BYTES - gpa) {
        return 1;
    }
    if (write) {
        k_memcpy(mem + gpa, buf, n);
    } else {
        k_memcpy(buf, mem + gpa, n);
    }
    return 0;
}

/* The vCPUs' mode and the exceptions queued. */
static int cpl, mode = 64, ud_queued;

int kvm_emul_cpl(const struct kvm_vcpu *v)
{
    (void)v;
    return cpl;
}

int kvm_emul_mode(const struct kvm_vcpu *v)
{
    (void)v;
    return mode;
}

void kvm_queue_exception(struct kvm_vcpu *v, uint8_t nr, int has_error, uint32_t error)
{
    (void)v;
    (void)has_error;
    (void)error;
    ep_work();
    if (nr == 6) {
        ud_queued++;
    }
}

void kvm_emul_skip(struct kvm_vcpu *v, uint32_t len)
{
    ep_work();
    v->regs.rip += len;
}

/* The guest's XMM0 to XMM5 while they are in the processor, as between a
 * VM exit and the end of KVM_RUN: `put_xmm` puts its words there and
 * leaves the image stale, and only `kvm_vcpu_fpu_sync` copies them to the
 * image, as vcpu.c's saves them through the backend. `fpu_syncs` counts the
 * syncs that found the registers live. */
#define NV 3
static uint64_t fpu_regs[NV][12];
static int fpu_live[NV], fpu_syncs;

/** Write `v`'s XMM registers back to its image, when they are live. */
void kvm_vcpu_fpu_sync(struct kvm_vcpu *v)
{
    if (fpu_live[v->index]) {
        k_memcpy(v->xsave + 160, fpu_regs[v->index], sizeof(fpu_regs[0]));
        fpu_live[v->index] = 0;
        fpu_syncs++;
    }
}

void kvm_vcpu_kick(struct kvm_vcpu *v, uint32_t request)
{
    ep_work();
    v->requests |= request;
}

int kvm_vcpu_test_clear(struct kvm_vcpu *v, uint32_t request)
{
    ep_work();
    int set = (v->requests & request) != 0;

    v->requests &= ~request;
    return set;
}

/* The APIC: the vectors raised on each vCPU since `clear_raised`, and the
 * last of the APIC MSRs. `no_apic` makes every raise -1. */
static struct kvm_vm vm;
static struct kvm_vcpu vcpus[NV];
static struct kvm_run runs[NV];
static uint8_t xsaves[NV][1024];
static uint32_t raised[NV][8];
static int raises, no_apic;
static uint32_t apic_reg;
static int apic_write;
static uint64_t apic_value;

int kvm_lapic_set_irq(struct kvm_vcpu *v, uint32_t vector)
{
    ep_work();
    if (no_apic) {
        return -1;
    }
    raised[v->index][vector >> 5] |= 1u << (vector & 31u);
    raises++;
    return 1;
}

int kvm_lapic_hv_msr(struct kvm_vcpu *v, uint32_t reg, int write, uint64_t *value)
{
    (void)v;
    ep_work();
    apic_reg = reg;
    apic_write = write;
    if (write) {
        apic_value = *value;
    } else {
        *value = 0x1234;
    }
    return 0;
}

/** Whether `vector` was raised on vCPU `i`. */
static int was_raised(int i, uint32_t vector)
{
    return (raised[i][vector >> 5] >> (vector & 31u)) & 1u;
}

/** Forget the vectors raised. */
static void clear_raised(void)
{
    k_memset(raised, 0, sizeof(raised));
    raises = 0;
}

/* The eventfds: fds 0 to 3, each with its references and signals. */
struct eventfd {
    int refs;
    int signals;
};
static struct eventfd evs[4];

struct eventfd *eventfd_get(int fd, int32_t *err)
{
    if (fd < 0 || fd >= 4) {
        *err = -EBADF;
        return NULL;
    }
    evs[fd].refs++;
    return &evs[fd];
}

void eventfd_put(struct eventfd *ev)
{
    ev->refs--;
}

void eventfd_signal(struct eventfd *ev)
{
    ep_work();
    ev->signals++;
}

/* The entry points, each called through a wrapper of the records above;
 * the names below stand for the wrappers in the rows. */

/** `kvm_hv_msr`, recorded as EP_MSR. */
static int msr_entry(struct kvm_vcpu *v, uint32_t index, int write, uint64_t *value, int host)
{
    int r;

    ep_begin(EP_MSR);
    r = kvm_hv_msr(v, index, write, value, host);
    ep_end();
    return r;
}

/** `kvm_hv_hypercall`, recorded as EP_HCALL. */
static int hcall_entry(struct kvm_vcpu *v, uint32_t len)
{
    int r;

    ep_begin(EP_HCALL);
    r = kvm_hv_hypercall(v, len);
    ep_end();
    return r;
}

/** `kvm_hv_complete`, recorded as EP_COMPLETE. */
static void complete_entry(struct kvm_vcpu *v)
{
    ep_begin(EP_COMPLETE);
    kvm_hv_complete(v);
    ep_end();
}

/** `kvm_hv_prepare`, recorded as EP_PREPARE. */
static int32_t prepare_entry(struct kvm_vcpu *v)
{
    int32_t r;

    ep_begin(EP_PREPARE);
    r = kvm_hv_prepare(v);
    ep_end();
    return r;
}

/** `kvm_hv_stimers`, recorded as EP_STIMERS. */
static void stimers_entry(struct kvm_vcpu *v)
{
    ep_begin(EP_STIMERS);
    kvm_hv_stimers(v);
    ep_end();
}

#define kvm_hv_msr       msr_entry
#define kvm_hv_hypercall hcall_entry
#define kvm_hv_complete  complete_entry
#define kvm_hv_prepare   prepare_entry
#define kvm_hv_stimers   stimers_entry

/** Make the VM of NV vCPUs, with the split controller unless `none`. */
static void vm_make(int none)
{
    int i;

    k_memset(&vm, 0, sizeof(vm));
    vm.irqchip = none ? 0 : KVM_IRQCHIP_SPLIT;
    vm.apic_bus_ns = KVM_APIC_BUS_NS_DEFAULT;
    vm.clock_offset = 123456789;
    for (i = 0; i < NV; i++) {
        k_memset(&vcpus[i], 0, sizeof(vcpus[i]));
        vcpus[i].vm = &vm;
        vcpus[i].index = (uint32_t)i;
        vcpus[i].id = (uint32_t)i;
        vcpus[i].run = &runs[i];
        vcpus[i].xsave = xsaves[i];
        fpu_live[i] = 0;
        vcpus[i].sregs.cr0 = 1;
        vcpus[i].tsc_offset = 1000 * (i + 1);
        vcpus[i].tsc_khz = 2400000u;
        vm.vcpus[i] = &vcpus[i];
    }
    vm.nr_vcpus = NV;
}

/** End the VM as kvm.c does, its vCPUs first. */
static void vm_end(void)
{
    int i;

    for (i = 0; i < NV; i++) {
        kvm_hv_vcpu_free(&vcpus[i]);
    }
    kvm_hv_vm_free(&vm);
}

/* The supported Hyper-V CPUID, as KVM_GET_SUPPORTED_HV_CPUID gives it. */
static struct {
    struct kvm_cpuid2 h;
    struct kvm_cpuid_entry2 e[12];
} hvc;

/** The entry of `fn` in `hvc`, or NULL. */
static struct kvm_cpuid_entry2 *hv_leaf(uint32_t fn)
{
    uint32_t i;

    for (i = 0; i < hvc.h.nent; i++) {
        if (hvc.e[i].function == fn) {
            return &hvc.e[i];
        }
    }
    return NULL;
}

/** Fetch the supported leaves into `hvc`, for `v` or /dev/kvm's. */
static int32_t get_hv_cpuid(struct kvm_vcpu *v)
{
    k_memset(&hvc, 0, sizeof(hvc));
    hvc.h.nent = 12;
    return kvm_hv_get_cpuid(v, (uintptr_t)&hvc);
}

/** Give each vCPU the supported leaves as its CPUID. */
static void set_all_cpuid(void)
{
    int i;

    for (i = 0; i < NV; i++) {
        kvm_hv_set_cpuid(&vcpus[i], hvc.e, hvc.h.nent);
    }
}

/** A guest or caller read of `index` on `v`; `*r` is kvm_hv_msr's answer. */
static uint64_t rd(struct kvm_vcpu *v, uint32_t index, int host, int *r)
{
    uint64_t value = 0;

    *r = kvm_hv_msr(v, index, 0, &value, host);
    return value;
}

/** A guest or caller write of `value` to `index` on `v`. */
static int wr(struct kvm_vcpu *v, uint32_t index, uint64_t value, int host)
{
    return kvm_hv_msr(v, index, 1, &value, host);
}

/** The 32-bit word of guest memory at `gpa`. */
static uint32_t mem32(uint32_t gpa)
{
    uint32_t w;

    k_memcpy(&w, mem + gpa, 4);
    return w;
}

/** The 64-bit word of guest memory at `gpa`. */
static uint64_t mem64(uint32_t gpa)
{
    uint64_t w;

    k_memcpy(&w, mem + gpa, 8);
    return w;
}

/** Store `w` in the 64-bit word of guest memory at `gpa`. */
static void put64(uint32_t gpa, uint64_t w)
{
    k_memcpy(mem + gpa, &w, 8);
}

/** Set XMM half `i` of `v`'s registers, low halves even, and leave its
 * image's copy stale, ~`w`, until `kvm_vcpu_fpu_sync`. */
static void put_xmm(struct kvm_vcpu *v, int i, uint64_t w)
{
    uint64_t stale = ~w;

    if (!fpu_live[v->index]) {
        k_memcpy(fpu_regs[v->index], v->xsave + 160, sizeof(fpu_regs[0]));
        fpu_live[v->index] = 1;
    }
    fpu_regs[v->index][i] = w;
    k_memcpy(v->xsave + 160 + i * 8, &stale, 8);
}

/**
 * Make hypercall `param` on `v` from 64-bit mode, with RDX `in` and R8
 * `out`, from a three-byte VMCALL at RIP 0x1000: what the guest finds in
 * RAX, or ~0 when it went to the caller.
 */
static uint64_t hcall(struct kvm_vcpu *v, uint64_t param, uint64_t in, uint64_t out)
{
    v->regs.rip = 0x1000;
    v->regs.rcx = param;
    v->regs.rdx = in;
    v->regs.r8 = out;
    v->regs.rax = ~0ull;
    kvm_hv_hypercall(v, 3);
    return v->regs.rax;
}

#define STIMER0_COUNT  (HV_MSR_STIMER0_CONFIG + 1u)
#define STIMER3_CONFIG (HV_MSR_STIMER3_COUNT - 1u)

#define REP(n)   ((uint64_t)(n) << HC_REP_SHIFT)
#define VAR(n)   ((uint64_t)(n) << HC_VARHEAD_SHIFT)
#define START(n) ((uint64_t)(n) << HC_START_SHIFT)

/** KVM_GET_SUPPORTED_HV_CPUID's leaves. */
static void cpuid_rows(void)
{
    struct kvm_cpuid_entry2 *e;
    static const uint32_t fns[10] = {0x40000000u, 0x40000001u, 0x40000002u, 0x40000003u,
                                     0x40000004u, 0x40000005u, 0x40000080u, 0x40000081u,
                                     0x40000082u, 0x4000000Au};
    uint32_t i;
    int order = 1;

    vm_make(1);
    k_memset(&hvc, 0, sizeof(hvc));
    hvc.h.nent = 9;
    CHECK(kvm_hv_get_cpuid(NULL, (uintptr_t)&hvc) == -E2BIG, "nine entries are too few");
    CHECK(kvm_hv_get_cpuid(NULL, 0) == -EFAULT, "a bad address");
    CHECK(get_hv_cpuid(NULL) == 0 && hvc.h.nent == 10, "ten leaves");
    for (i = 0; i < 10; i++) {
        order &= hvc.e[i].function == fns[i] && hvc.e[i].index == 0 && hvc.e[i].flags == 0;
    }
    CHECK(order, "the leaves in Linux's order");
    e = hv_leaf(0x40000000u);
    CHECK(e && e->eax == 0x40000082u && e->ebx == 0x756E694Cu && e->ecx == 0x564B2078u &&
              e->edx == 0x7648204Du,
          "0x40000000: the highest leaf and \"Linux KVM Hv\"");
    CHECK(hv_leaf(0x40000001u)->eax == 0x31237648u, "0x40000001: \"Hv#1\"");
    CHECK(hv_leaf(0x40000002u)->eax == 0x3839u && hv_leaf(0x40000002u)->ebx == 0xA0000u,
          "0x40000002: Hyper-V 2016's version");
    e = hv_leaf(0x40000003u);
    CHECK(e->eax == 0xAAFFu && e->ebx == 0x100830u && e->edx == 0x84D12u,
          "0x40000003: Linux's features, with the direct timers");
    e = hv_leaf(0x40000004u);
    CHECK(e->eax == 0xE2Cu && e->ebx == 0xFFFu, "0x40000004: Linux's recommendations");
    CHECK(hv_leaf(0x40000005u)->eax == KVM_MAX_VCPUS && hv_leaf(0x40000005u)->ebx == 64,
          "0x40000005: the limits");
    CHECK(hv_leaf(0x40000081u)->eax == 0x31235356u && hv_leaf(0x40000082u)->eax == 2u,
          "the synthetic debugger's leaves");
    CHECK(hv_leaf(0x4000000Au)->eax == 0xA0000u && hv_leaf(0x4000000Au)->ebx == 1u,
          "0x4000000A: the nested features, no enlightened VMCS");
    CHECK(get_hv_cpuid(&vcpus[0]) == 0 && !(hv_leaf(0x40000003u)->edx & (1u << 19)) &&
              !(hv_leaf(0x40000004u)->eax & (1u << 10)),
          "a vCPU with no APIC in the kernel has no direct timers or cluster IPI");
    CHECK(vcpus[0].hv == NULL, "the CPUID ioctl makes no state");
    vm_end();
}

/** The capabilities and KVM_ENABLE_CAP's answers. */
static void cap_rows(void)
{
    static const long caps[] = {
        KVM_CAP_HYPERV,          KVM_CAP_HYPERV_VAPIC,   KVM_CAP_HYPERV_SPIN,
        KVM_CAP_HYPERV_TIME,     KVM_CAP_HYPERV_SYNIC,   KVM_CAP_HYPERV_SYNIC2,
        KVM_CAP_HYPERV_VP_INDEX, KVM_CAP_HYPERV_EVENTFD, KVM_CAP_HYPERV_TLBFLUSH,
        KVM_CAP_HYPERV_SEND_IPI, KVM_CAP_HYPERV_CPUID,   KVM_CAP_HYPERV_ENFORCE_CPUID,
        KVM_CAP_SYS_HYPERV_CPUID};
    struct kvm_enable_cap cap;
    uint32_t i;
    int all = 1;

    vm_make(1);
    for (i = 0; i < sizeof(caps) / sizeof(caps[0]); i++) {
        all &= kvm_hv_check_extension(caps[i]) == 1;
    }
    CHECK(all, "the Hyper-V capabilities");
    CHECK(kvm_hv_check_extension(KVM_CAP_HYPERV_ENLIGHTENED_VMCS) == 0 &&
              kvm_hv_check_extension(KVM_CAP_HYPERV_DIRECT_TLBFLUSH) == 0,
          "no enlightened VMCS or direct flush, which are a nested guest's");

    k_memset(&cap, 0, sizeof(cap));
    cap.cap = KVM_CAP_HYPERV_SYNIC;
    CHECK(kvm_hv_enable_cap(&vcpus[0], &cap) == -EINVAL, "SynIC with no APIC in the kernel");
    cap.cap = KVM_CAP_HYPERV_ENLIGHTENED_VMCS;
    CHECK(kvm_hv_enable_cap(&vcpus[0], &cap) == -ENOTTY, "the enlightened VMCS refused");
    cap.cap = KVM_CAP_HYPERV_DIRECT_TLBFLUSH;
    CHECK(kvm_hv_enable_cap(&vcpus[0], &cap) == -ENOTTY, "the direct flush refused");
    cap.cap = KVM_CAP_HYPERV_ENFORCE_CPUID;
    CHECK(kvm_hv_enable_cap(&vcpus[0], &cap) == 0 && vcpus[0].hv == NULL,
          "enforcement off makes no state");
    cap.cap = KVM_CAP_HYPERV;
    CHECK(kvm_hv_enable_cap(&vcpus[0], &cap) == -EINVAL, "another capability");
    vm_end();

    vm_make(0);
    cap.cap = KVM_CAP_HYPERV_SYNIC2;
    cap.args[0] = 1;
    CHECK(kvm_hv_enable_cap(&vcpus[0], &cap) == -EINVAL, "SynIC2 with an argument");
    cap.args[0] = 0;
    CHECK(kvm_hv_enable_cap(&vcpus[0], &cap) == 0 && hv_of(&vcpus[0])->synic_active &&
              hv_of(&vcpus[0])->dont_zero && hv_of(&vcpus[0])->scontrol == 1,
          "SynIC2 on, its pages kept");
    vm_end();
    CHECK(live == 0, "the capabilities' state given back");
}

/** The VP index, read by the guest and set by the caller alone. */
static void vp_index_rows(struct kvm_vcpu *v)
{
    int r;

    CHECK(wr(v, HV_MSR_VP_INDEX, 1, 0) == -1, "#GP for the guest's write of the VP index");
    CHECK(wr(v, HV_MSR_VP_INDEX, KVM_MAX_VCPUS, 1) == -1, "a VP index past the vCPUs refused");
    CHECK(wr(&vcpus[1], HV_MSR_VP_INDEX, 7, 1) == 0 && rd(&vcpus[1], HV_MSR_VP_INDEX, 0, &r) == 7 &&
              vcpu_by_vpidx(&vm, 7) == &vcpus[1] && vcpu_by_vpidx(&vm, 1) == NULL,
          "the caller sets a VP index");
    wr(&vcpus[1], HV_MSR_VP_INDEX, 1, 1);
}

/** The TSC page of `v`'s VM, set, without the master clock: left at
 * sequence 0 and broken, as Linux's compute_tsc_page_parameters refuses a
 * kvmclock without PVCLOCK_TSC_STABLE_BIT, until the guest writes its MSR
 * again. */
static void tsc_master_rows(struct kvm_vcpu *v)
{
    struct hv_vm *h = hvm_of(&vm);
    int r;

    master_on = 0;
    kvm_hv_clock_changed(&vm);
    CHECK(kvm_hv_prepare(v) == 0 && h->tsc_status == TSC_BROKEN && mem32(0x4000) == 0,
          "without the master clock the page is left at sequence 0, as Linux's");
    CHECK(rd(v, HV_MSR_TIME_REF_COUNT, 0, &r) == (mono_ns + 123456789u) / 100u,
          "and the counter is the kvmclock's");
    master_on = 1;
    kvm_hv_clock_changed(&vm);
    CHECK(h->tsc_status == TSC_BROKEN && kvm_hv_prepare(v) == 0 && mem32(0x4000) == 0,
          "the master clock back leaves the page broken, as Linux's");
    CHECK(wr(v, HV_MSR_REFERENCE_TSC, 0x4001, 0) == 0 && kvm_hv_prepare(v) == 0 &&
              h->tsc_status == TSC_SET && mem32(0x4000) == 1,
          "until the guest writes the MSR again");
}

/** The reference counter and the TSC page. */
static void tsc_rows(struct kvm_vcpu *v)
{
    struct hv_vm *h = hvm_of(&vm);
    uint64_t scale, ref, want;
    int r;

    want = (mono_ns + 123456789u) / 100u;
    CHECK(rd(v, HV_MSR_TIME_REF_COUNT, 0, &r) == want && r == 0,
          "the reference counter is the kvmclock's in 100 ns");
    CHECK(wr(v, HV_MSR_TIME_REF_COUNT, 5, 0) == -1 && wr(v, HV_MSR_TIME_REF_COUNT, 5, 1) == 0,
          "the reference counter is read-only, the caller's write ignored");

    k_memset(mem + 0x4000, 0, 24);
    CHECK(wr(v, HV_MSR_REFERENCE_TSC, 0x4001, 0) == 0 && h->tsc_status == TSC_GUEST_CHANGED,
          "the TSC page asked for");
    CHECK(kvm_hv_prepare(v) == 0 && h->tsc_status == TSC_SET && mem32(0x4000) == 1,
          "the TSC page written before the entry, sequence 1");
    scale = mem64(0x4008);
    CHECK(scale == tsc_scale(2400000000ull) && scale == 76861433640456465ull,
          "the scale is 2^64 * 10^7 / 2.4 GHz");
    ref = mul_hi(guest_tsc(&vcpus[0]), scale) + mem64(0x4010);
    want = (mono_ns + 123456789u) / 100u;
    CHECK(ref + 1u >= want && ref <= want, "the page's formula is the kvmclock's");
    mono_ns += 1000000000u;
    ref = rd(&vcpus[2], HV_MSR_TIME_REF_COUNT, 0, &r);
    want += 10000000u;
    CHECK(ref + 2u >= want && ref <= want + 1u &&
              ref == mul_hi(guest_tsc(v), scale) + mem64(0x4010),
          "the counter follows the TSC page a second later");
    kvm_hv_clock_changed(&vm);
    CHECK(h->tsc_status == TSC_HOST_CHANGED && kvm_hv_prepare(v) == 0 && mem32(0x4000) == 2,
          "a change of the clock writes the page again, sequence 2");
    tsc_master_rows(v);
    k_memset(mem + 0x4000, 0xFF, 4);
    kvm_hv_clock_changed(&vm);
    kvm_hv_prepare(v);
    CHECK(mem32(0x4000) == 1, "the sequence skips 0xFFFFFFFF and 0");
    wr(v, HV_MSR_TSC_EMULATION_CTL, 1, 0);
    kvm_hv_clock_changed(&vm);
    CHECK(h->tsc_status == TSC_SET, "no rewrite while the guest emulates the TSC");
    wr(v, HV_MSR_TSC_EMULATION_CTL, 0, 0);
    CHECK(wr(v, HV_MSR_REFERENCE_TSC, 0x20001, 0) == 0 && kvm_hv_prepare(v) == 0 &&
              h->tsc_status == TSC_BROKEN,
          "a TSC page in no slot is broken");
    tsc_per_tick = 0;
    wr(v, HV_MSR_REFERENCE_TSC, 0x4001, 1);
    CHECK(h->tsc_status == TSC_HOST_CHANGED && kvm_hv_prepare(v) == 0 &&
              h->tsc_status == TSC_BROKEN && mem32(0x4000) == 0,
          "no TSC rate: the page left at sequence 0");
    CHECK(rd(v, HV_MSR_TIME_REF_COUNT, 0, &r) == (mono_ns + 123456789u) / 100u,
          "a broken page leaves the counter on the kvmclock");
    tsc_per_tick = 2400000000u / TICK_HZ;
    CHECK(wr(v, HV_MSR_REFERENCE_TSC, 0, 0) == 0 && h->tsc_status == TSC_UNSET, "the TSC page off");
    CHECK(tsc_scale(10000000u) == 0 && tsc_scale(1ull << 62) == 0, "scales that do not fit");
}

/** The frequencies, and the crash and reset MSRs. */
static void crash_rows(struct kvm_vcpu *v)
{
    uint32_t i;
    int r, ok;

    CHECK(rd(v, HV_MSR_TSC_FREQUENCY, 0, &r) == 2400000000ull &&
              rd(v, HV_MSR_APIC_FREQUENCY, 0, &r) == 1000000000ull,
          "the TSC and APIC frequencies");
    CHECK(wr(v, HV_MSR_TSC_FREQUENCY, 1, 0) == -1 && wr(v, HV_MSR_APIC_FREQUENCY, 1, 1) == 0,
          "the frequencies are read-only, the caller's write ignored");
    vcpus[1].tsc_khz = 3100000u;
    vm.apic_bus_ns = 4;
    CHECK(rd(&vcpus[1], HV_MSR_TSC_FREQUENCY, 0, &r) == 3100000000ull &&
              rd(v, HV_MSR_TSC_FREQUENCY, 0, &r) == 2400000000ull &&
              rd(v, HV_MSR_APIC_FREQUENCY, 0, &r) == 250000000ull,
          "each vCPU's TSC rate, not the host's, and the VM's bus cycle");
    vcpus[1].tsc_khz = 2400000u;
    vm.apic_bus_ns = KVM_APIC_BUS_NS_DEFAULT;

    ok = 1;
    for (i = 0; i < 5; i++) {
        ok &= wr(v, HV_MSR_CRASH_P0 + i, 0x100 + i, 0) == 0;
    }
    for (i = 0; i < 5; i++) {
        ok &= rd(&vcpus[1], HV_MSR_CRASH_P0 + i, 0, &r) == 0x100 + i;
    }
    CHECK(ok, "the crash parameters, partition-wide");
    CHECK(wr(v, HV_MSR_CRASH_CTL, 1ull << 63, 1) == 0 &&
              rd(v, HV_MSR_CRASH_CTL, 0, &r) == 1ull << 63 && !(v->requests & KVM_REQ_HV_CRASH),
          "the caller offers the crash notice");
    CHECK(wr(v, HV_MSR_CRASH_CTL, 1ull << 63, 0) == 0 && (v->requests & KVM_REQ_HV_CRASH) &&
              kvm_hv_prepare(v) == 1 && v->run->exit_reason == KVM_EXIT_SYSTEM_EVENT &&
              v->run->u.system_event.type == KVM_SYSTEM_EVENT_CRASH,
          "the guest's crash notice ends the run in a crash");
    CHECK(wr(v, HV_MSR_RESET, 2, 0) == 0 && !(v->requests & KVM_REQ_HV_RESET),
          "a reset of other than 1 does nothing");
    CHECK(wr(v, HV_MSR_RESET, 1, 0) == 0 && rd(v, HV_MSR_RESET, 0, &r) == 0 &&
              kvm_hv_prepare(v) == 1 && v->run->u.system_event.type == KVM_SYSTEM_EVENT_RESET,
          "the reset MSR ends the run in a reset");
    CHECK(kvm_hv_prepare(v) == 0, "the requests taken once");
}

/** The APIC MSRs, the VP assist page and the runtime. */
static void vcpu_msr_rows(struct kvm_vcpu *v)
{
    int r;

    CHECK(wr(v, HV_MSR_EOI, 0, 0) == 0 && apic_reg == 0xB0 && apic_write,
          "the EOI MSR is the APIC's EOI");
    CHECK(wr(v, HV_MSR_ICR, 0x40031, 0) == 0 && apic_reg == 0x300 && apic_value == 0x40031,
          "the ICR MSR is the APIC's ICR");
    CHECK(rd(v, HV_MSR_TPR, 0, &r) == 0x1234 && apic_reg == 0x80 && !apic_write,
          "the TPR MSR is the APIC's TPR");
    put64(0x5000, ~0ull);
    CHECK(wr(v, HV_MSR_VP_ASSIST_PAGE, 0x5001, 0) == 0 &&
              rd(v, HV_MSR_VP_ASSIST_PAGE, 0, &r) == 0x5001 &&
              mem64(0x5000) == 0xFFFFFFFF00000000ull,
          "the VP assist page, its APIC assist word cleared");
    CHECK(wr(v, HV_MSR_VP_ASSIST_PAGE, 0x20001, 0) == -1, "#GP for an assist page in no slot");

    caller.utime = 3;
    caller.stime = 4;
    CHECK(rd(v, HV_MSR_VP_RUNTIME, 0, &r) == 7u * (TICK_NS / 100u),
          "the runtime is the thread's CPU time");
    CHECK(wr(v, HV_MSR_VP_RUNTIME, 5, 0) == -1 && wr(v, HV_MSR_VP_RUNTIME, 100000, 1) == 0,
          "the runtime is set by the caller alone");
    caller.utime = 5;
    CHECK(rd(v, HV_MSR_VP_RUNTIME, 0, &r) == 100000u + 2u * (TICK_NS / 100u),
          "the runtime counts on from the caller's");
}

/** The invariant TSC, the TSC emulation, MSRs KVM does not know and the
 * synthetic debugger. */
static void other_msr_rows(struct kvm_vcpu *v)
{
    int r;

    CHECK(kvm_hv_invtsc_suppressed(v), "the invariant TSC hidden until the guest exposes it");
    CHECK(wr(v, HV_MSR_TSC_INVARIANT_CTL, 2, 0) == -1, "#GP for a reserved bit");
    CHECK(wr(v, HV_MSR_TSC_INVARIANT_CTL, 1, 0) == 0 && !kvm_hv_invtsc_suppressed(v),
          "the guest exposes the invariant TSC");
    CHECK(wr(v, HV_MSR_TSC_INVARIANT_CTL, 0, 0) == -1 && wr(v, HV_MSR_TSC_INVARIANT_CTL, 0, 1) == 0,
          "the guest cannot hide it again; the caller can");
    CHECK(wr(v, HV_MSR_REENLIGHTENMENT, 0x77, 0) == 0 &&
              rd(v, HV_MSR_REENLIGHTENMENT, 0, &r) == 0x77,
          "the reenlightenment MSR");
    CHECK(wr(v, HV_MSR_TSC_EMULATION_STAT, 1, 0) == -1 &&
              wr(v, HV_MSR_TSC_EMULATION_STAT, 1, 1) == 0 &&
              rd(v, HV_MSR_TSC_EMULATION_STAT, 0, &r) == 1,
          "the emulation status set by the caller alone");
    CHECK(wr(v, 0x40000004u, 0, 0) == -1 && wr(v, 0x40000004u, 0, 1) == -1 &&
              wr(v, 0x400000B8u, 0, 0) == 1,
          "an MSR of Hyper-V's range that KVM does not know, and one outside it");

    CHECK(wr(v, HV_MSR_SYNDBG_OPTIONS, 4, 0) == 0 && rd(v, HV_MSR_SYNDBG_OPTIONS, 0, &r) == 4,
          "the debugger's options");
    CHECK(wr(v, HV_MSR_SYNDBG_SEND_BUFFER, 0x6000, 0) == 0 && !(v->requests & KVM_REQ_HV_EXIT),
          "the debugger's send buffer, kept in the kernel");
    CHECK(wr(v, HV_MSR_SYNDBG_CONTROL, 0x11, 0) == 0 && kvm_hv_prepare(v) == 1 &&
              v->run->exit_reason == KVM_EXIT_HYPERV &&
              v->run->u.hyperv.type == KVM_EXIT_HYPERV_SYNDBG &&
              v->run->u.hyperv.u.syndbg.msr == HV_MSR_SYNDBG_CONTROL &&
              v->run->u.hyperv.u.syndbg.control == 0x11 &&
              v->run->u.hyperv.u.syndbg.send_page == 0x6000,
          "the debugger's control goes to the caller");
    v->run->u.hyperv.u.syndbg.status = 0x55;
    kvm_hv_complete(v);
    CHECK(rd(v, HV_MSR_SYNDBG_STATUS, 0, &r) == 0x55 && hv_of(v)->done == DONE_NONE,
          "the caller's status read back");
}

/** The MSRs before and after the CPUID names Hyper-V. */
static void msr_rows(void)
{
    struct kvm_vcpu *v = &vcpus[0];
    int r;

    vm_make(0);
    CHECK(wr(v, 0x10, 5, 0) == 1 && wr(v, 0x40000200u, 5, 0) == 1, "MSRs outside Hyper-V's");
    CHECK(wr(v, HV_MSR_GUEST_OS_ID, 5, 0) == -1 && v->hv == NULL,
          "#GP for a guest whose CPUID does not name Hyper-V");
    CHECK(wr(v, HV_MSR_GUEST_OS_ID, 0, 1) == 0 && rd(v, HV_MSR_GUEST_OS_ID, 1, &r) == 0 && r == 0 &&
              v->hv != NULL,
          "the caller's access before the CPUID");
    CHECK(rd(v, HV_MSR_VP_INDEX, 1, &r) == 0 && rd(&vcpus[2], HV_MSR_VP_INDEX, 1, &r) == 2,
          "the VP index is the vCPU's place");
    CHECK(!kvm_hv_hypercall_enabled(v), "no hypercalls before the CPUID");

    get_hv_cpuid(v);
    set_all_cpuid();
    CHECK(hv_of(v)->enabled && hv_of(v)->features_eax == 0xAAFFu, "KVM_SET_CPUID2 names Hyper-V");
    CHECK(wr(v, HV_MSR_HYPERCALL, 0x2001, 0) == 0 && rd(v, HV_MSR_HYPERCALL, 0, &r) == 0,
          "the hypercall MSR ignored before the guest's id");
    CHECK(!kvm_hv_hypercall_enabled(v), "no hypercalls before the guest's id");
    CHECK(wr(v, HV_MSR_GUEST_OS_ID, 0x8100000000000000ull, 0) == 0 &&
              rd(&vcpus[1], HV_MSR_GUEST_OS_ID, 0, &r) == 0x8100000000000000ull,
          "the guest's id, partition-wide");
    CHECK(kvm_hv_hypercall_enabled(v), "hypercalls once the guest names itself");
    CHECK(wr(v, HV_MSR_HYPERCALL, 0x2001, 0) == 0 && rd(v, HV_MSR_HYPERCALL, 0, &r) == 0x2001 &&
              mem32(0x2000) == 0xC3C1010Fu,
          "the hypercall page: VMCALL and RET");
    kvm_backend = &kvm_svm;
    CHECK(wr(v, HV_MSR_HYPERCALL, 0x3001, 0) == 0 && mem32(0x3000) == 0xC3D9010Fu,
          "the hypercall page on SVM: VMMCALL and RET");
    kvm_backend = &kvm_vmx;
    CHECK(wr(v, HV_MSR_HYPERCALL, 0x20001, 0) == -1 && rd(v, HV_MSR_HYPERCALL, 0, &r) == 0x3001,
          "#GP for a hypercall page in no slot");
    CHECK(wr(v, HV_MSR_GUEST_OS_ID, 0, 0) == 0 && rd(v, HV_MSR_HYPERCALL, 0, &r) == 0x3000,
          "a guest id of 0 disables the hypercall page");
    wr(v, HV_MSR_GUEST_OS_ID, 0x8100000000000000ull, 0);
    vp_index_rows(v);
    tsc_rows(v);
    crash_rows(v);
    vcpu_msr_rows(v);
    other_msr_rows(v);
    vm_end();
    CHECK(live == 0, "the MSRs' state given back");
}

/** KVM_CAP_HYPERV_ENFORCE_CPUID over the MSRs. */
static void enforce_rows(void)
{
    struct kvm_vcpu *v = &vcpus[0];
    struct kvm_enable_cap cap;
    struct kvm_cpuid_entry2 *f;
    int r;

    vm_make(0);
    get_hv_cpuid(NULL);
    f = hv_leaf(0x40000003u);
    f->eax = HV_MSR_HYPERCALL_AVAILABLE;
    f->ebx = 0;
    f->edx = 0;
    set_all_cpuid();
    k_memset(&cap, 0, sizeof(cap));
    cap.cap = KVM_CAP_HYPERV_ENFORCE_CPUID;
    cap.args[0] = 1;
    CHECK(kvm_hv_enable_cap(v, &cap) == 0 && hv_of(v)->enforce, "enforcement on");
    CHECK(wr(v, HV_MSR_GUEST_OS_ID, 1, 0) == 0, "the guest's id with the hypercall bit");
    CHECK(wr(v, HV_MSR_VP_INDEX, 0, 0) == -1 && wr(v, HV_MSR_TIME_REF_COUNT, 0, 0) == -1 &&
              wr(v, HV_MSR_SCONTROL, 0, 0) == -1 && wr(v, HV_MSR_STIMER0_CONFIG, 0, 0) == -1 &&
              wr(v, HV_MSR_CRASH_P0, 0, 0) == -1 && wr(v, HV_MSR_SYNDBG_OPTIONS, 0, 0) == -1 &&
              wr(v, HV_MSR_TSC_INVARIANT_CTL, 0, 0) == -1 && wr(v, HV_MSR_EOI, 0, 0) == -1,
          "#GP for each MSR its feature bit does not offer");
    CHECK(rd(v, HV_MSR_VP_INDEX, 1, &r) == 0 && r == 0, "the caller is not held to the CPUID");
    CHECK(rd(&vcpus[1], HV_MSR_VP_INDEX, 0, &r) == 1 && r == 0,
          "a vCPU not enforced reads what its CPUID does not offer");
    cap.args[0] = 0;
    kvm_hv_enable_cap(v, &cap);
    CHECK(rd(v, HV_MSR_VP_INDEX, 0, &r) == 0 && r == 0, "enforcement off");
    vm_end();
    CHECK(live == 0, "the enforcement's state given back");
}

/** Turn SynIC on for each vCPU, its message page at 0x8000 + 0x1000 * i
 * and SINT 2 at vector 0x50 + i. */
static void synic_on(void)
{
    struct kvm_enable_cap cap;
    int i;

    k_memset(&cap, 0, sizeof(cap));
    cap.cap = KVM_CAP_HYPERV_SYNIC;
    for (i = 0; i < NV; i++) {
        kvm_hv_enable_cap(&vcpus[i], &cap);
        wr(&vcpus[i], HV_MSR_SIMP, 0x8001u + 0x1000u * (uint32_t)i, 1);
        wr(&vcpus[i], HV_MSR_SINT0 + 2, 0x50u + (uint32_t)i, 1);
    }
}

/** SINT 2 of `v` raised through a route, and its refusals; then the guest's
 * write of SCONTROL. */
static void sint_rows(struct kvm_vcpu *v)
{
    clear_raised();
    wr(v, HV_MSR_SINT0 + 2, 0x51, 0);
    CHECK(kvm_hv_synic_set_irq(&vm, 0, 2) == 1 && was_raised(0, 0x51) && raises == 1,
          "a SINT raised through its route");
    CHECK(kvm_hv_synic_set_irq(&vm, 0, 4) == -1 && kvm_hv_synic_set_irq(&vm, 9, 2) == -1 &&
              kvm_hv_synic_set_irq(&vm, 1, 2) == -1 && kvm_hv_synic_set_irq(&vm, 0, 16) == -1,
          "no raise for a masked SINT, an unknown VP, a vCPU without SynIC or SINT 16");
    no_apic = 1;
    CHECK(kvm_hv_synic_set_irq(&vm, 0, 2) == -1, "a SINT no APIC took");
    no_apic = 0;
    CHECK(wr(v, HV_MSR_SCONTROL, 0, 0) == 0 && kvm_hv_prepare(v) == 1 &&
              v->run->u.hyperv.u.synic.msr == HV_MSR_SCONTROL &&
              v->run->u.hyperv.u.synic.control == 0,
          "the guest's write of SCONTROL goes to the caller");
}

/** The SynIC's MSRs, its SINTs and their raises. */
static void synic_rows(void)
{
    struct kvm_vcpu *v = &vcpus[0];
    struct kvm_enable_cap cap;
    int r;

    vm_make(0);
    get_hv_cpuid(NULL);
    set_all_cpuid();
    CHECK(rd(v, HV_MSR_SCONTROL, 0, &r) == 0 && r == -1, "#GP for SynIC's MSRs before it is on");
    CHECK(rd(v, HV_MSR_SVERSION, 1, &r) == 1 && r == 0 && rd(v, HV_MSR_SINT0, 1, &r) == 1ull << 16,
          "the caller reads version 1 and masked SINTs");
    CHECK(wr(v, HV_MSR_SIMP, 0, 1) == 0 && wr(v, HV_MSR_SIMP, 1, 1) == -1,
          "the caller writes only 0 before SynIC is on");
    CHECK(kvm_hv_synic_set_irq(&vm, 0, 2) == -1, "no SINT raised before SynIC is on");

    k_memset(&cap, 0, sizeof(cap));
    cap.cap = KVM_CAP_HYPERV_SYNIC;
    CHECK(kvm_hv_enable_cap(v, &cap) == 0 && rd(v, HV_MSR_SCONTROL, 0, &r) == 1,
          "SynIC on, enabled");
    k_memset(mem + 0x8000, 0xAA, 4096);
    CHECK(wr(v, HV_MSR_SIMP, 0x8001, 0) == 0 && mem32(0x8000) == 0 && mem32(0x8FFC) == 0,
          "the guest's message page cleared");
    CHECK(kvm_hv_prepare(v) == 1 && v->run->exit_reason == KVM_EXIT_HYPERV &&
              v->run->u.hyperv.type == KVM_EXIT_HYPERV_SYNIC &&
              v->run->u.hyperv.u.synic.msr == HV_MSR_SIMP &&
              v->run->u.hyperv.u.synic.msg_page == 0x8001 && v->run->u.hyperv.u.synic.control == 1,
          "the guest's write of SIMP goes to the caller");
    CHECK(wr(v, HV_MSR_SIEFP, 0x20001, 0) == -1, "#GP for a page in no slot");
    CHECK(wr(v, HV_MSR_SVERSION, 2, 0) == -1, "#GP for the guest's write of the version");
    CHECK(wr(v, HV_MSR_SINT0 + 3, 0x05, 0) == -1 && wr(v, HV_MSR_SINT0 + 3, 0x10005, 0) == 0,
          "#GP for a vector below 16 unless masked");
    CHECK(wr(v, HV_MSR_SINT0 + 3, 0x05, 1) == 0, "the caller may set one");
    CHECK(wr(v, HV_MSR_SINT0 + 2, 0x20060, 0) == 0 && kvm_hv_synic_auto_eoi(v, 0x60) &&
              vec_test(hv_of(v)->vec_bitmap, 0x60),
          "a SINT with auto-EOI");
    CHECK(wr(v, HV_MSR_SINT0 + 4, 0x60, 0) == 0 && kvm_hv_synic_auto_eoi(v, 0x60),
          "a second SINT of the vector keeps its auto-EOI");
    CHECK(wr(v, HV_MSR_SINT0 + 2, 0x10060, 0) == 0 && !kvm_hv_synic_auto_eoi(v, 0x60) &&
              vec_test(hv_of(v)->vec_bitmap, 0x60),
          "the auto-EOI SINT masked");
    wr(v, HV_MSR_SINT0 + 4, 0x10000, 0);
    CHECK(!vec_test(hv_of(v)->vec_bitmap, 0x60), "the vector free when no SINT has it");

    sint_rows(v);

    cap.cap = KVM_CAP_HYPERV_SYNIC2;
    kvm_hv_enable_cap(&vcpus[1], &cap);
    k_memset(mem + 0x9000, 0xAA, 4096);
    CHECK(wr(&vcpus[1], HV_MSR_SIMP, 0x9001, 0) == 0 && mem32(0x9000) == 0xAAAAAAAAu,
          "SynIC2 leaves the page as it was");
    vm_end();
    CHECK(live == 0, "SynIC's state given back");
}

/** Timer 0 of `v` one-shot on SINT 2, a ms on. */
static void oneshot_rows(struct kvm_vcpu *v)
{
    struct hv_stimer *s = &hv_of(v)->stimer[0];
    uint64_t now, next;
    int r;

    now = ref_counter(&vm);
    CHECK(wr(v, HV_MSR_STIMER0_CONFIG, 0x20001, 0) == 0 &&
              wr(v, STIMER0_COUNT, now + 10000u, 0) == 0 &&
              rd(v, HV_MSR_STIMER0_CONFIG, 0, &r) == 0x20001 &&
              rd(v, STIMER0_COUNT, 0, &r) == now + 10000u,
          "timer 0 one-shot on SINT 2, a ms on");
    CHECK(kvm_hv_stimer_next(v) == 1, "the timer looked at before the next entry");
    kvm_hv_stimers(v);
    next = kvm_hv_stimer_next(v);
    CHECK(next == mono_ns + 1000000u && raises == 0, "armed to fire a ms on");
    mono_ns += 999999u;
    kvm_hv_stimers(v);
    CHECK(raises == 0 && mem32(0x8000 + 2 * 256) == 0, "not yet");
    mono_ns += 1u;
    kvm_hv_stimers(v);
    CHECK(was_raised(0, 0x50) && raises == 1, "the timer fired on SINT 2's vector");
    CHECK(mem32(0x8200) == HV_MSG_TIMER_EXPIRED && mem[0x8204] == 24 && mem32(0x8210) == 0 &&
              mem64(0x8218) == now + 10000u && mem64(0x8220) == ref_counter(&vm),
          "the message: its type, size, timer, expiry and delivery");
    CHECK(!(s->config & STIMER_ENABLE) && kvm_hv_stimer_next(v) == 0,
          "the one-shot timer disabled once delivered");
}

/** Timer 0's message held by a full slot, retried at EOM and at EOI. */
static void held_rows(struct kvm_vcpu *v)
{
    struct hv_stimer *s = &hv_of(v)->stimer[0];
    uint64_t now;

    clear_raised();
    now = ref_counter(&vm);
    wr(v, HV_MSR_STIMER0_CONFIG, 0x20001, 0);
    wr(v, STIMER0_COUNT, now + 100u, 0);
    kvm_hv_stimers(v);
    mono_ns += 10000u;
    kvm_hv_stimers(v);
    CHECK(raises == 0 && s->msg_pending && (s->config & STIMER_ENABLE) &&
              (mem[0x8205] & HV_MSG_PENDING),
          "a full slot holds the message, marked pending");
    k_memset(mem + 0x8200, 0, 256);
    CHECK(wr(v, HV_MSR_EOM, 0, 0) == 0 && kvm_hv_stimer_next(v) == 1, "EOM retries the timer");
    kvm_hv_stimers(v);
    CHECK(raises == 1 && !s->msg_pending && !(s->config & STIMER_ENABLE),
          "the held message delivered");

    clear_raised();
    now = ref_counter(&vm);
    wr(v, HV_MSR_STIMER0_CONFIG, 0x20001, 0);
    wr(v, STIMER0_COUNT, now + 100u, 0);
    kvm_hv_stimers(v);
    mono_ns += 10000u;
    kvm_hv_stimers(v);
    k_memset(mem + 0x8200, 0, 256);
    kvm_hv_synic_eoi(v, 0x50);
    kvm_hv_stimers(v);
    CHECK(raises == 1 && !s->msg_pending, "an EOI of the SINT's vector retries it too");
}

/** Timer 0 periodic, its SINT 0 and auto-enable. */
static void periodic_rows(struct kvm_vcpu *v)
{
    struct hv_stimer *s = &hv_of(v)->stimer[0];
    uint64_t now;

    clear_raised();
    k_memset(mem + 0x8200, 0, 256);
    now = ref_counter(&vm);
    wr(v, HV_MSR_STIMER0_CONFIG, 0x20003, 0);
    wr(v, STIMER0_COUNT, 5000, 0);
    kvm_hv_stimers(v);
    CHECK(s->exp_time == now + 5000u && kvm_hv_stimer_next(v) == mono_ns + 500000u,
          "a periodic timer of half a ms");
    mono_ns += 500000u;
    kvm_hv_stimers(v);
    CHECK(raises == 1 && s->exp_time == now + 10000u && (s->config & STIMER_ENABLE),
          "it fires and rearms for the next period");
    mono_ns += 500000u;
    kvm_hv_stimers(v);
    CHECK(raises == 1 && !s->msg_pending && s->exp_time == now + 15000u,
          "a periodic message dropped while the slot is full");

    CHECK(wr(v, HV_MSR_STIMER0_CONFIG, 0x00001, 0) == 0 && !(s->config & STIMER_ENABLE),
          "an enabled timer moved to SINT 0 is disabled");
    CHECK(wr(v, HV_MSR_STIMER0_CONFIG, 0x20008, 0) == 0 && !(s->config & STIMER_ENABLE) &&
              wr(v, STIMER0_COUNT, 5000, 0) == 0 && (s->config & STIMER_ENABLE),
          "auto-enable: the count enables the timer");
    CHECK(wr(v, STIMER0_COUNT, 0, 0) == 0 && !(s->config & STIMER_ENABLE),
          "a count of 0 disables it");
    CHECK(wr(v, STIMER0_COUNT, 0, 1) == 0 && wr(v, HV_MSR_STIMER0_CONFIG, 0x20008, 1) == 0 &&
              wr(v, STIMER0_COUNT, 7, 1) == 0 && !(s->config & STIMER_ENABLE),
          "the caller's count does not auto-enable");
}

/** Timer 3 in direct mode, and a count already past on vCPU 1. */
static void direct_rows(struct kvm_vcpu *v)
{
    struct hv_stimer *s;
    uint64_t now;

    clear_raised();
    s = &hv_of(v)->stimer[3];
    now = ref_counter(&vm);
    CHECK(wr(v, STIMER3_CONFIG, 0x1401, 0) == 0 && wr(v, HV_MSR_STIMER3_COUNT, now + 10u, 0) == 0,
          "timer 3 in direct mode, vector 0x40");
    kvm_hv_stimers(v);
    mono_ns += 1000u;
    kvm_hv_stimers(v);
    CHECK(raises == 1 && was_raised(0, 0x40) && !(s->config & STIMER_ENABLE),
          "the direct timer raises its vector, with no message");
    wr(v, STIMER3_CONFIG, 0, 0);

    clear_raised();
    k_memset(mem + 0x9000, 0, 4096);
    now = ref_counter(&vm);
    wr(&vcpus[1], HV_MSR_STIMER0_CONFIG, 0x20001, 0);
    wr(&vcpus[1], STIMER0_COUNT, now - 1u, 0);
    kvm_hv_stimers(&vcpus[1]);
    CHECK(raises == 0 && kvm_hv_stimer_next(&vcpus[1]) == 1,
          "a count already past is looked at again before the entry");
    kvm_hv_stimers(&vcpus[1]);
    CHECK(raises == 1 && was_raised(1, 0x51) && mem32(0x9200) == HV_MSG_TIMER_EXPIRED,
          "and fires then, with no wait");
}

/** The synthetic timers. */
static void stimer_rows(void)
{
    struct kvm_vcpu *v = &vcpus[0];

    vm_make(0);
    get_hv_cpuid(NULL);
    set_all_cpuid();
    CHECK(wr(v, HV_MSR_STIMER0_CONFIG, 0x20001, 0) == -1, "#GP for a timer before SynIC is on");
    synic_on();
    k_memset(mem + 0x8000, 0, 4096);
    clear_raised();
    oneshot_rows(v);
    held_rows(v);
    periodic_rows(v);
    direct_rows(v);
    vm_end();
    CHECK(live == 0, "the timers' state given back");
}

/** KVM_HYPERV_EVENTFD and HvSignalEvent on `v`, whose SynIC is on. */
static void event_rows(struct kvm_vcpu *v)
{
    struct kvm_hyperv_eventfd ef;

    k_memset(&ef, 0, sizeof(ef));
    ef.conn_id = 7;
    ef.fd = 9;
    CHECK(kvm_hv_eventfd(&vm, (uintptr_t)&ef) == -EBADF, "an eventfd that is not one");
    ef.fd = 1;
    CHECK(kvm_hv_eventfd(&vm, (uintptr_t)&ef) == 0 && evs[1].refs == 1, "connection 7");
    CHECK(kvm_hv_eventfd(&vm, (uintptr_t)&ef) == -EEXIST && evs[1].refs == 1, "connection 7 twice");
    ef.conn_id = 1u << 24;
    CHECK(kvm_hv_eventfd(&vm, (uintptr_t)&ef) == -EINVAL, "a connection past the mask");
    ef.conn_id = 8;
    ef.flags = 2;
    CHECK(kvm_hv_eventfd(&vm, (uintptr_t)&ef) == -EINVAL, "an unknown flag");
    ef.flags = KVM_HYPERV_EVENTFD_DEASSIGN;
    CHECK(kvm_hv_eventfd(&vm, (uintptr_t)&ef) == -ENOENT, "a connection not assigned");
    CHECK(kvm_hv_eventfd(&vm, 0) == -EFAULT, "a bad address");
    ef.flags = 0;
    ef.fd = 2;
    kvm_hv_eventfd(&vm, (uintptr_t)&ef);
    CHECK(hcall(v, HVCALL_SIGNAL_EVENT | HC_FAST, 7, 0) == 0 && evs[1].signals == 1,
          "HvSignalEvent, fast, signals connection 7");
    put64(0x7200, 8);
    CHECK(hcall(v, HVCALL_SIGNAL_EVENT, 0x7200, 0) == 0 && evs[2].signals == 1,
          "HvSignalEvent from memory signals connection 8");
    CHECK(hcall(v, HVCALL_SIGNAL_EVENT, 0x7204, 0) == HV_STATUS_INVALID_ALIGNMENT &&
              hcall(v, HVCALL_SIGNAL_EVENT, 0x20000, 0) == HV_STATUS_INVALID_ALIGNMENT,
          "HvSignalEvent's input misaligned or in no slot");
    CHECK(hcall(v, HVCALL_SIGNAL_EVENT | HC_FAST, 1u << 24, 0) == HV_STATUS_INVALID_INPUT,
          "a connection past the mask");
    CHECK(hcall(v, HVCALL_SIGNAL_EVENT | HC_FAST, 9, 0) == ~0ull && v->exit.kind == KVM_X_HYPERV &&
              v->exit.hyperv.u.hcall.params[0] == 9,
          "a connection the kernel does not know goes to the caller");
    kvm_hv_complete(v);
    CHECK(hcall(v, HVCALL_SIGNAL_EVENT | HC_FAST | REP(1), 7, 0) == HV_STATUS_INVALID_INPUT,
          "HvSignalEvent with a rep");
    ef.flags = KVM_HYPERV_EVENTFD_DEASSIGN;
    ef.conn_id = 7;
    CHECK(kvm_hv_eventfd(&vm, (uintptr_t)&ef) == 0 && evs[1].refs == 0, "connection 7 deassigned");
}

/** The extended and debug hypercalls on `v`, then each hypercall denied
 * under the enforced CPUID. */
static void other_hcall_rows(struct kvm_vcpu *v)
{
    struct kvm_cpuid_entry2 *f;
    struct kvm_enable_cap cap;

    CHECK(hcall(v, HVCALL_EXT_QUERY, 0x7000, 0x7100) == ~0ull &&
              v->exit.hyperv.u.hcall.input == HVCALL_EXT_QUERY,
          "an extended hypercall goes to the caller");
    kvm_hv_complete(v);
    CHECK(hcall(v, HVCALL_EXT_QUERY | HC_FAST, 0, 0) == HV_STATUS_INVALID_PARAMETER,
          "a fast extended hypercall");
    CHECK(hcall(v, HVCALL_EXT_LAST + 1u, 0, 0) == HV_STATUS_INVALID_CODE,
          "past the extended hypercalls");

    CHECK(hcall(v, HVCALL_POST_DEBUG_DATA, 0x7000, 0x7100) == HV_STATUS_OPERATION_DENIED,
          "debug data without the debugger's hypercall option");
    wr(v, HV_MSR_SYNDBG_OPTIONS, HV_SYNDBG_OPTION_HCALLS, 0);
    CHECK(hcall(v, HVCALL_RETRIEVE_DEBUG_DATA, 0x7000, 0x7100) == ~0ull,
          "debug data goes to the caller");
    kvm_hv_complete(v);
    CHECK(hcall(v, HVCALL_POST_DEBUG_DATA | HC_FAST, 0, 0) == HV_STATUS_INVALID_PARAMETER,
          "fast debug data");
    f = hv_leaf(0x40000082u);
    f->eax = 0;
    set_all_cpuid();
    CHECK(hcall(v, HVCALL_RESET_DEBUG_SESSION, 0, 0) == HV_STATUS_INVALID_CODE,
          "a debug hypercall with no debugger is an unknown code");

    get_hv_cpuid(NULL);
    f = hv_leaf(0x40000003u);
    f->ebx = 0;
    f->edx = 0;
    hv_leaf(0x40000004u)->eax = 0;
    hv_leaf(0x40000004u)->ebx = 0;
    set_all_cpuid();
    k_memset(&cap, 0, sizeof(cap));
    cap.cap = KVM_CAP_HYPERV_ENFORCE_CPUID;
    cap.args[0] = 1;
    kvm_hv_enable_cap(v, &cap);
    CHECK(hcall(v, HVCALL_NOTIFY_LONG_SPIN, 0, 0) == HV_STATUS_ACCESS_DENIED &&
              hcall(v, HVCALL_POST_MESSAGE, 0, 0) == HV_STATUS_ACCESS_DENIED &&
              hcall(v, HVCALL_SIGNAL_EVENT, 0, 0) == HV_STATUS_ACCESS_DENIED &&
              hcall(v, HVCALL_FLUSH_SPACE, 0, 0) == HV_STATUS_ACCESS_DENIED &&
              hcall(v, HVCALL_FLUSH_LIST_EX, 0, 0) == HV_STATUS_ACCESS_DENIED &&
              hcall(v, HVCALL_SEND_IPI, 0, 0) == HV_STATUS_ACCESS_DENIED &&
              hcall(v, HVCALL_SEND_IPI_EX, 0, 0) == HV_STATUS_ACCESS_DENIED &&
              hcall(v, HVCALL_EXT_QUERY, 0, 0) == HV_STATUS_ACCESS_DENIED,
          "each hypercall its CPUID does not offer is denied");
    CHECK(hcall(v, 0x99, 0, 0) == HV_STATUS_INVALID_CODE, "an unknown code is not denied");
    hv_of(v)->enl_eax = HV_REMOTE_TLB_FLUSH_RECOMMENDED;
    ud_queued = 0;
    CHECK(hcall(v, HVCALL_FLUSH_SPACE | HC_FAST, 0, 0) == ~0ull && ud_queued == 1,
          "#UD for XMM input the CPUID does not offer");
}

/** The hypercalls' checks, statuses and exits to the caller. */
static void hcall_rows(void)
{
    struct kvm_vcpu *v = &vcpus[0];

    vm_make(0);
    get_hv_cpuid(NULL);
    set_all_cpuid();
    wr(v, HV_MSR_GUEST_OS_ID, 0x8100000000000000ull, 0);

    cpl = 3;
    CHECK(hcall(v, 0x99, 0, 0) == ~0ull && ud_queued == 1 && v->regs.rip == 0x1000,
          "#UD above CPL 0");
    cpl = 0;
    v->sregs.cr0 = 0;
    CHECK(hcall(v, 0x99, 0, 0) == ~0ull && ud_queued == 2, "#UD in real mode");
    v->sregs.cr0 = 1;
    CHECK(hcall(v, 0x99, 0, 0) == HV_STATUS_INVALID_CODE && v->regs.rip == 0x1003 &&
              v->exit.kind == KVM_X_AGAIN,
          "an unknown code, the VMCALL stepped");
    CHECK(hcall(v, HVCALL_NOTIFY_LONG_SPIN | (1ull << 27), 0, 0) == HV_STATUS_INVALID_INPUT,
          "a reserved bit");
    CHECK(hcall(v, HVCALL_NOTIFY_LONG_SPIN, 0, 0) == 0 &&
              hcall(v, HVCALL_NOTIFY_LONG_SPIN | REP(1), 0, 0) == HV_STATUS_INVALID_INPUT,
          "a long spin's notice, and with a rep");
    mode = 32;
    v->regs.rip = 0x1000;
    v->regs.rax = HVCALL_FLUSH_LIST;
    v->regs.rdx = 2;
    v->regs.rbx = 0;
    v->regs.rcx = 0x7000;
    put64(0x7000, 0);
    put64(0x7008, 1);
    put64(0x7010, 0);
    kvm_hv_hypercall(v, 3);
    CHECK(v->regs.rax == 0 && v->regs.rdx == 2, "a 32-bit hypercall's result in EDX:EAX");
    mode = 64;

    CHECK(hcall(v, HVCALL_POST_MESSAGE, 0x7000, 0) == HV_STATUS_INVALID_INPUT,
          "no message posted without SynIC");
    synic_on();
    CHECK(hcall(v, HVCALL_POST_MESSAGE, 0x7000, 0x7100) == ~0ull && v->exit.kind == KVM_X_HYPERV &&
              v->exit.hyperv.type == KVM_EXIT_HYPERV_HCALL &&
              v->exit.hyperv.u.hcall.input == HVCALL_POST_MESSAGE &&
              v->exit.hyperv.u.hcall.params[0] == 0x7000 &&
              v->exit.hyperv.u.hcall.params[1] == 0x7100 && v->regs.rip == 0x1000,
          "HvPostMessage goes to the caller");
    v->run->u.hyperv.u.hcall.result = 0x13;
    v->dirty = 0;
    kvm_hv_complete(v);
    CHECK(v->regs.rax == 0x13 && v->regs.rip == 0x1003 && (v->dirty & KVM_PART_REGS),
          "the caller's result completes it at the next run");
    kvm_hv_complete(v);
    CHECK(v->regs.rip == 0x1003, "completed once");
    event_rows(v);
    other_hcall_rows(v);
    vm_end();
    CHECK(live == 0 && evs[2].refs == 0, "the hypercalls' state and eventfds given back");
}

/** The IPI hypercalls. */
static void ipi_rows(void)
{
    struct kvm_vcpu *v = &vcpus[0];

    vm_make(0);
    get_hv_cpuid(NULL);
    set_all_cpuid();
    wr(v, HV_MSR_GUEST_OS_ID, 1, 0);

    clear_raised();
    put64(0x7000, 0x30);
    put64(0x7008, 0x5);
    CHECK(hcall(v, HVCALL_SEND_IPI, 0x7000, 0) == 0 && raises == 2 && was_raised(0, 0x30) &&
              was_raised(2, 0x30),
          "HvCallSendSyntheticClusterIpi to VPs 0 and 2");
    clear_raised();
    CHECK(hcall(v, HVCALL_SEND_IPI | HC_FAST, 0x31, 0x2) == 0 && raises == 1 && was_raised(1, 0x31),
          "the fast form to VP 1");
    CHECK(hcall(v, HVCALL_SEND_IPI | HC_FAST, 0x0F, 0x2) == HV_STATUS_INVALID_INPUT &&
              hcall(v, HVCALL_SEND_IPI | HC_FAST, 0x100, 0x2) == HV_STATUS_INVALID_INPUT,
          "a vector below 16 or past 255");
    CHECK(hcall(v, HVCALL_SEND_IPI | HC_FAST, 1ull << 32 | 0x31, 0x2) == HV_STATUS_INVALID_INPUT,
          "the fast form's high bits");
    CHECK(hcall(v, HVCALL_SEND_IPI | VAR(1), 0x7000, 0) == HV_STATUS_INVALID_INPUT &&
              hcall(v, HVCALL_SEND_IPI | REP(1), 0x7000, 0) == HV_STATUS_INVALID_INPUT,
          "the simple form with a variable header or a rep");
    CHECK(hcall(v, HVCALL_SEND_IPI, 0x20000, 0) == HV_STATUS_INVALID_INPUT, "input in no slot");

    clear_raised();
    put64(0x7000, 0x32);
    put64(0x7008, HV_SET_SPARSE_4K);
    put64(0x7010, 0x1);
    put64(0x7018, 0x6);
    CHECK(hcall(v, HVCALL_SEND_IPI_EX | VAR(1), 0x7000, 0) == 0 && raises == 2 &&
              was_raised(1, 0x32) && was_raised(2, 0x32),
          "the Ex form to bank 0's VPs 1 and 2");
    CHECK(hcall(v, HVCALL_SEND_IPI_EX, 0x7000, 0) == HV_STATUS_INVALID_INPUT,
          "a variable header that is not the mask's banks");
    clear_raised();
    put64(0x7010, 0x2);
    CHECK(hcall(v, HVCALL_SEND_IPI_EX | VAR(1), 0x7000, 0) == 0 && raises == 0,
          "bank 1 names no VP of the VM");
    put64(0x7010, 0);
    CHECK(hcall(v, HVCALL_SEND_IPI_EX, 0x7000, 0) == 0 && raises == 0, "an empty set");
    put64(0x7008, HV_SET_ALL);
    CHECK(hcall(v, HVCALL_SEND_IPI_EX, 0x7000, 0) == 0 && raises == 3, "the Ex form to all");
    clear_raised();
    put_xmm(v, 0, 0x1);
    put_xmm(v, 1, 0x4);
    fpu_syncs = 0;
    CHECK(hcall(v, HVCALL_SEND_IPI_EX | HC_FAST | VAR(1), 0x33, HV_SET_SPARSE_4K) == 0 &&
              raises == 1 && was_raised(2, 0x33),
          "the fast Ex form from XMM0");
    CHECK(fpu_syncs == 1 && !fpu_live[v->index],
          "the fast hypercall writes the guest's XMM registers back to the image before it "
          "reads them, as Linux's kvm_hv_hypercall_read_xmm reads them under kvm_fpu_get");
    CHECK(hcall(v, HVCALL_SEND_IPI_EX | HC_FAST | VAR(1), 0x33, HV_SET_SPARSE_4K) == 0,
          "XMM0 read again on each call");
    vm_end();

    vm_make(1);
    get_hv_cpuid(NULL);
    set_all_cpuid();
    wr(v, HV_MSR_GUEST_OS_ID, 1, 0);
    CHECK(hcall(v, HVCALL_SEND_IPI | HC_FAST, 0x31, 0x2) == HV_STATUS_INVALID_INPUT,
          "no IPI with no APIC in the kernel");
    vm_end();
    CHECK(live == 0, "the IPIs' state given back");
}

/** Clear each vCPU's requests. */
static void clear_requests(void)
{
    int i;

    for (i = 0; i < NV; i++) {
        vcpus[i].requests = 0;
    }
}

/** Which vCPUs were asked to flush, a bit each. */
static uint32_t flushed(void)
{
    uint32_t m = 0;
    int i;

    for (i = 0; i < NV; i++) {
        if (vcpus[i].requests & KVM_REQ_TLB_FLUSH) {
            m |= 1u << i;
        }
    }
    clear_requests();
    return m;
}

/** The TLB flush hypercalls. */
static void flush_rows(void)
{
    struct kvm_vcpu *v = &vcpus[0];
    int i;

    vm_make(0);
    get_hv_cpuid(NULL);
    set_all_cpuid();
    wr(v, HV_MSR_GUEST_OS_ID, 1, 0);
    clear_requests();

    put64(0x7000, 0);
    put64(0x7008, 0);
    put64(0x7010, 0x4);
    CHECK(hcall(v, HVCALL_FLUSH_SPACE, 0x7000, 0) == 0 && flushed() == 0x4,
          "HvFlushVirtualAddressSpace on VP 2");
    put64(0x7008, HV_FLUSH_ALL_PROCESSORS);
    CHECK(hcall(v, HVCALL_FLUSH_SPACE, 0x7000, 0) == 0 && flushed() == 0x7, "on all processors");
    put64(0x7008, 0);
    put64(0x7010, 0);
    CHECK(hcall(v, HVCALL_FLUSH_SPACE, 0x7000, 0) == 0 && flushed() == 0x7,
          "an empty mask is every processor, as Windows Server 2012 sends it");
    CHECK(hcall(v, HVCALL_FLUSH_SPACE | REP(1), 0x7000, 0) == HV_STATUS_INVALID_INPUT &&
              hcall(v, HVCALL_FLUSH_SPACE | VAR(1), 0x7000, 0) == HV_STATUS_INVALID_INPUT,
          "the space flush with a rep or a variable header");
    CHECK(hcall(v, HVCALL_FLUSH_SPACE, 0x20000, 0) == HV_STATUS_INVALID_INPUT && flushed() == 0,
          "input in no slot");
    put_xmm(v, 0, 0x2);
    CHECK(hcall(v, HVCALL_FLUSH_SPACE | HC_FAST, 0, 0) == 0 && flushed() == 0x2,
          "the fast form from XMM0 on VP 1");

    put64(0x7010, 0x1);
    for (i = 0; i < 3; i++) {
        put64(0x7018 + 8 * i, 0x400000u + 0x1000u * (uint32_t)i);
    }
    CHECK(hcall(v, HVCALL_FLUSH_LIST | REP(3), 0x7000, 0) == REP(3) && flushed() == 0x1,
          "HvFlushVirtualAddressList of three, every rep complete");
    CHECK(hcall(v, HVCALL_FLUSH_LIST, 0x7000, 0) == HV_STATUS_INVALID_INPUT &&
              hcall(v, HVCALL_FLUSH_LIST | REP(3) | START(1), 0x7000, 0) == HV_STATUS_INVALID_INPUT,
          "a list of none, or from a start index");
    CHECK(hcall(v, HVCALL_FLUSH_LIST | REP(3), MEM_BYTES - 32u, 0) == HV_STATUS_INVALID_INPUT,
          "a list in no slot");
    CHECK(hcall(v, HVCALL_FLUSH_LIST | REP(16), MEM_BYTES - 32u, 0) == REP(16),
          "past the fifo the list is not read, and all is flushed");
    flushed();
    put_xmm(v, 0, 0x1);
    for (i = 1; i < 12; i++) {
        put_xmm(v, i, 0x500000u);
    }
    CHECK(hcall(v, HVCALL_FLUSH_LIST | HC_FAST | REP(11), 0, 0) == REP(11) && flushed() == 0x1,
          "the fast list of eleven in XMM0 to XMM5");
    CHECK(hcall(v, HVCALL_FLUSH_LIST | HC_FAST | REP(12), 0, 0) == HV_STATUS_INVALID_INPUT,
          "a fast list past the registers");

    put64(0x7000, 0);
    put64(0x7008, 0);
    put64(0x7010, HV_SET_SPARSE_4K);
    put64(0x7018, 0x1);
    put64(0x7020, 0x6);
    CHECK(hcall(v, HVCALL_FLUSH_SPACE_EX | VAR(1), 0x7000, 0) == 0 && flushed() == 0x6,
          "HvFlushVirtualAddressSpaceEx on bank 0's VPs 1 and 2");
    CHECK(hcall(v, HVCALL_FLUSH_SPACE_EX | VAR(2), 0x7000, 0) == HV_STATUS_INVALID_INPUT,
          "a variable header that is not the mask's banks");
    put64(0x7018, 0);
    CHECK(hcall(v, HVCALL_FLUSH_SPACE_EX, 0x7000, 0) == 0 && flushed() == 0, "an empty set");
    put64(0x7010, HV_SET_ALL);
    CHECK(hcall(v, HVCALL_FLUSH_SPACE_EX, 0x7000, 0) == 0 && flushed() == 0x7, "the set of all");
    put64(0x7010, HV_SET_SPARSE_4K);
    put64(0x7018, 0x1);
    put64(0x7020, 0x1);
    put64(0x7028, 0x600000u);
    put64(0x7030, 0x601000u);
    CHECK(hcall(v, HVCALL_FLUSH_LIST_EX | VAR(1) | REP(2), 0x7000, 0) == REP(2) && flushed() == 0x1,
          "HvFlushVirtualAddressListEx, its list after the banks");
    CHECK(hcall(v, HVCALL_FLUSH_LIST_EX | VAR(1), 0x7000, 0) == HV_STATUS_INVALID_INPUT,
          "the Ex list of none");
    put_xmm(v, 0, HV_SET_SPARSE_4K);
    put_xmm(v, 1, 0x1);
    put_xmm(v, 2, 0x4);
    CHECK(hcall(v, HVCALL_FLUSH_SPACE_EX | HC_FAST | VAR(1), 0, 0) == 0 && flushed() == 0x4,
          "the fast Ex form: the set in XMM0, the bank in XMM1");
    CHECK(hcall(v, HVCALL_FLUSH_LIST_EX | HC_FAST | VAR(1) | REP(9), 0, 0) == REP(9) &&
              flushed() == 0x4 &&
              hcall(v, HVCALL_FLUSH_LIST_EX | HC_FAST | VAR(1) | REP(10), 0, 0) ==
                  HV_STATUS_INVALID_INPUT,
          "the fast Ex list fills the registers left, and no more");
    CHECK(vp_in_set(65, 0x3, (const uint64_t[]){0, 0}) == 0 && vp_in_set(64, 0x1, NULL) == 0,
          "a VP past the banks kept is in no set");
    vm_end();
    CHECK(live == 0, "the flushes' state given back");
}

int main(void)
{
    cpuid_rows();
    cap_rows();
    msr_rows();
    enforce_rows();
    synic_rows();
    stimer_rows();
    hcall_rows();
    ipi_rows();
    flush_rows();
    CHECK(ep_takes[EP_MSR] && ep_work_runs[EP_MSR] && !ep_bad[EP_MSR],
          "kvm_hv_msr takes the big lock once around its state and gives it back");
    CHECK(ep_takes[EP_HCALL] && ep_work_runs[EP_HCALL] && !ep_bad[EP_HCALL],
          "kvm_hv_hypercall takes the big lock once around its state and gives it back");
    CHECK(ep_takes[EP_COMPLETE] && ep_work_runs[EP_COMPLETE] && !ep_bad[EP_COMPLETE],
          "kvm_hv_complete takes the big lock once around its state and gives it back");
    CHECK(ep_takes[EP_PREPARE] && ep_work_runs[EP_PREPARE] && !ep_bad[EP_PREPARE],
          "kvm_hv_prepare takes the big lock once around its state and gives it back");
    CHECK(ep_takes[EP_STIMERS] && ep_work_runs[EP_STIMERS] && !ep_bad[EP_STIMERS],
          "kvm_hv_stimers takes the big lock once around its state and gives it back");
    printf("kvm-hyperv-hosttest: result: %s: %d failures\n", failures ? "fail" : "pass", failures);
    return failures ? 1 : 0;
}
