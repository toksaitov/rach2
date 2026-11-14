/**
 * Check the KVM interface, /dev/kvm and its VM and vCPU descriptors,
 * against a fake backend.
 *
 * host-deps: rach2/sys/kvm/cpuid.c rach2/sys/kvm/msr.c rach2/sys/kvm/hyperv.c
 * host-deps: rach2/sys/kvm/pvclock.c rach2/sys/kvm/binstats.c
 * host-deps: rach2/sys/kvm/exitstat.c
 * host-deps: rach2/sys/kvm/pmu.c
 * host-deps: rach2/sys/kvm/dirty.c
 * host-deps: tests/host/compat/fd_flat.c tests/host/compat/atomic_host.c
 * host-deps: tests/host/compat/mutex_none.c tests/host/compat/spinlock_host.c
 * host-cflags: -DRACH2_HOST_X86_64 -DRACH2_HOST_CPU
 *
 * kvm.c and vcpu.c are the kernel's, included whole; cpuid.c, msr.c and
 * hyperv.c are linked, the last with no guest memory, APIC or eventfd under
 * it, which tests/host/kvm_hyperv_hosttest.c gives it. The processor is a
 * model of the Lenovo's Haswell: its CPUID, with VMX, AVX and XSAVE of x87,
 * SSE and AVX. The backend is a fake whose
 * entry either refuses, as the stubs do, or reports the exits a script
 * gives, and the memory slots, the interrupt routing, the in-kernel bus and
 * the local APIC are fakes that answer the capabilities their files make
 * true and act as those files do for a VM with no split controller, which
 * tests/host/kvm_irq_hosttest.c covers.
 *
 * The rows: the structures' sizes and the ioctl numbers against Linux's;
 * /dev/kvm registered as 10:232 when the backend probes; QEMU 10's
 * start-up as its sources order it, from KVM_GET_API_VERSION through the
 * required capabilities, the MSR and feature lists, with the host's
 * microcode revision, the supported CPUID taken by doubling, the vCPU and
 * its mapping, KVM_SET_CPUID2, the
 * machine-check setup, the full put and get of the registers, to KVM_RUN,
 * where the refusing backend's error comes back; each exit kind through the
 * run page, with port and MMIO reads answered on the next KVM_RUN, a HLT
 * going to the caller with no local APIC in the kernel, immediate_exit, a
 * signal, a kick from another processor and a triple fault; KVM_RUN and
 * its exits without the big lock, and interrupts off for the entry; EINTR
 * from KVM_RUN and a state ioctl whose wait for a vCPU another process
 * holds SIGKILL ends;
 * kvm/exit_stats's rows for VMX's and SVM's codes, summed over two
 * processors, with each exit's wait for the big lock, its clear, the
 * entry, run and exit counted only from marks in order, the preparation
 * only for an exit the kernel handled, less its waits for the big lock and
 * never wrapped by a lower TSC, and an exit with no entry after it
 * counted as one that ended KVM_RUN; a KVM_REQ_CLOCK_UPDATE or KVM_REQ_EVENT
 * left after the run loop's look cancelling the entry, and a
 * KVM_REQ_TLB_FLUSH left to it; the signal mask swapped for the run;
 * the refusals of the state ioctls Linux makes; EIO from another address
 * space; every allocation given back when the last reference to the VM
 * goes, the mapping's among them; and, in a VM of their own with
 * the TSC's rate known, KVM's CPUID leaves and capabilities, the vCPUs'
 * TSCs matched under the master clock, which a write of another TSC stops,
 * the kvmclock in guest memory reading as KVM_GET_CLOCK does, its stop
 * flag, the wall clock, the clock pairing, the old MSR, KVM_SET_CLOCK, the
 * Hyper-V reference TSC page written from a clock KVM_SET_CLOCK moved
 * between the run loop's two preparations, with the kvmclock off, the
 * master clock decided again when the host's clock stops and starts,
 * KVM_CAP_ENFORCE_PV_FEATURE_CPUID and KVM_CAP_X86_DISABLE_EXITS; the
 * steal time, its writes of the thread's run_delay and its preempted flag
 * across a switch at a slice's end inside KVM_RUN, none at a halted
 * vCPU's sleep at the run loop's top, which is no preemption, and never in
 * a record the guest moved into memfd_secret with the tables unchanged; the
 * vCPU attribute KVM_VCPU_TSC_OFFSET; CR0's CD and NW kept across a
 * guest's INIT; and what Linux's selftests ask for:
 * the TSC's rate set on a VM and on a vCPU, on a processor that scales
 * none and with SVM's and VMX's ratios,
 * the statistics files of a VM and a vCPU, KVM_PRE_FAULT_MEMORY, 1024
 * vCPUs with ids below 4096, the CR4 bits taken before KVM_SET_CPUID2,
 * the VM's capabilities and quirks, and the synchronized registers; and
 * under SVM, SYSCALL_MASK's bits 63:32 kept from the caller and refused
 * from the guest, and VIRT_SPEC_CTRL's SSBD carried out in IA32_SPEC_CTRL,
 * the host's VIRT_SPEC_CTRL or LS_CFG, or skipped on a host with none, and
 * cleared with SPEC_CTRL at INIT.
 */
#include "kvm/kvm.c"
#include "kvm/vcpu.c"
#include "fs/debugfs.h"
#include "lib/kstring.h"
#include "timer.h"
#include "vdso.h"

extern int printf(const char *format, ...);
extern void *malloc(unsigned long n);
extern void *aligned_alloc(unsigned long align, unsigned long n);
extern void free(void *p);
extern int strcmp(const char *a, const char *b);
extern int strncmp(const char *a, const char *b, unsigned long n);
extern char *strchr(const char *s, int c);

static int failures;

/** Count and print a failure when `condition` is false. */
#define CHECK(condition, detail)                          \
    do {                                                  \
        if (!(condition)) {                               \
            printf("kvm-hosttest: failed: %s\n", detail); \
            failures++;                                   \
        }                                                 \
    } while (0)

/* The model's processes: the caller, and a forked child in another
 * address space. */
static struct mm caller_mm, child_mm;
static process_t caller;
static fd_entry_t entries[64];
static int reserved[64];
static fd_table_t table = {.chunks = {entries}, .cap = 64, .refs = 1, .lock = SPINLOCK_INIT};
struct percpu cpu_table[NR_CPUS] = {{.current = &caller}, {.current = &caller}};
unsigned int host_cpu;

/* The heap's live allocations, which the last put must bring back. */
static int live;

/** The device bridge is exercised by kvm_vfio_hosttest. */
int32_t kvm_vfio_create(struct kvm_vm *vm, uintptr_t arg)
{
    (void)vm;
    (void)arg;
    return -ENODEV;
}

/** kmalloc over the host's allocator, counted. */
void *kmalloc(size_t size)
{
    live++;
    return malloc(size ? size : 1);
}

/** kzalloc over the host's allocator, counted. */
void *kzalloc(size_t size)
{
    void *p = kmalloc(size);

    k_memset(p, 0, size);
    return p;
}

/** kfree, counted; NULL is nothing. */
void kfree(void *p)
{
    if (p) {
        live--;
        free(p);
    }
}

/** Pages, aligned, with their address as their physical address. */
void *kalloc_pages(uint32_t count, uintptr_t *phys)
{
    void *p = aligned_alloc(4096, 4096ul * count);

    live++;
    if (phys) {
        *phys = (uintptr_t)p;
    }
    return p;
}

/** Pages whose frames are consecutive: `kalloc_pages`'s, whose address is
 * their physical address. */
void *kalloc_contig(uint32_t count, uintptr_t *phys)
{
    return kalloc_pages(count, phys);
}

/** A kernel address is its own physical address here. */
uintptr_t paging_virt_to_phys(uintptr_t va)
{
    return va;
}

/** Give pages back. */
void kfree_pages(void *p, uint32_t count)
{
    (void)count;
    live--;
    free(p);
}

/** memcpy, memset and memmove, for the kernel's sources. */
void *k_memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;

    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

/* A word no `k_memset` may cover, and how many have since it was set: an
 * INIT's reset keeps the APIC base whole, which another processor's
 * delivery reads without a lock. */
static const volatile void *memset_watch;
static int memset_watch_hits;

void *k_memset(void *dst, int c, size_t n)
{
    uint8_t *d = dst;

    if (memset_watch && (const volatile uint8_t *)memset_watch >= d &&
        (const volatile uint8_t *)memset_watch < d + n) {
        memset_watch_hits++;
    }
    while (n--) {
        *d++ = (uint8_t)c;
    }
    return dst;
}

/** The caller's memory is the host's; address 0 faults. */
int copy_to_user(void __user *dst, const void *src, size_t n)
{
    if (!dst) {
        return -EFAULT;
    }
    k_memcpy(dst, src, n);
    return 0;
}

int copy_from_user(void *dst, const void __user *src, size_t n)
{
    if (!src) {
        return -EFAULT;
    }
    k_memcpy(dst, src, n);
    return 0;
}

/** The lowest free slot of the model's table. */
int fd_alloc(fd_table_t *t)
{
    int i;

    (void)t;
    for (i = 3; i < 64; i++) {
        if (!reserved[i]) {
            reserved[i] = 1;
            return i;
        }
    }
    return -EMFILE;
}

/* The node registered, and the model's holds of the address spaces. */
static struct devfs_node *registered;
static int holds;

int32_t devfs_node_register(struct devfs_node *node)
{
    registered = node;
    return 0;
}

/* /sys/module's directories and files, which the model takes and forgets. */
static int sysfs_handles;

int sysfs_anchor(int node, int key)
{
    (void)node;
    (void)key;
    return ++sysfs_handles;
}

int sysfs_add_dir(int parent, const char *name)
{
    (void)parent;
    (void)name;
    return ++sysfs_handles;
}

int sysfs_module_params(const char *module, int *made)
{
    (void)module;
    *made = 0;
    return ++sysfs_handles;
}

void sysfs_module_params_undo(int made, const int *added, int n)
{
    (void)made;
    (void)added;
    (void)n;
}

/* exitstat.c's debug file kvm/exit_stats: its hooks and their data, which
 * exitstat_alone_rows, exitstat_part_rows and exitstat_run_rows read and
 * write through. */
static const struct debugfs_text_ops *exitstat_ops_kept;
static void *exitstat_data_kept;

int32_t debugfs_create_text(const char *path, uint32_t mode, const struct debugfs_text_ops *ops,
                            void *data)
{
    CHECK(strcmp(path, "kvm/exit_stats") == 0 && mode == 0644u,
          "the only text file is kvm/exit_stats, mode 0644");
    exitstat_ops_kept = ops;
    exitstat_data_kept = data;
    return 0;
}

int sysfs_add_attr(int parent, const struct sysfs_attr *attr, void *obj, int arg)
{
    (void)parent;
    (void)attr;
    (void)obj;
    (void)arg;
    return ++sysfs_handles;
}

void mm_hold(struct mm *mm)
{
    (void)mm;
    holds++;
}

void mm_unhold(struct mm *mm)
{
    (void)mm;
    holds--;
}

/* The capabilities the caller has given up, by number below 32. */
static uint32_t caps_lost;

/** The caller's capabilities: those of root, less `caps_lost`. */
int cap_holds(const process_t *p, uint32_t cap)
{
    return p->euid == 0 && (cap >= 32u || !(caps_lost & (1u << cap)));
}

/* The clocks: the raw clock, which the kvmclock counts, the real
 * nanoseconds, and the TSC. */
static uint64_t mono_ns = 5000000000ull, real_ns = 1800000000000000000ull, tsc = 1000000;

void rtc_raw_monotonic(int64_t *s, int64_t *ns)
{
    *s = (int64_t)(mono_ns / 1000000000ull);
    *ns = (int64_t)(mono_ns % 1000000000ull);
}

void rtc_realtime(int64_t *s, int64_t *ns)
{
    *s = (int64_t)(real_ns / 1000000000ull);
    *ns = (int64_t)(real_ns % 1000000000ull);
}

uint64_t arch_cycles(void)
{
    return tsc;
}

/* Whether the vDSO's clock runs from the TSC, and the TSC's counts in a
 * tick, 0 for a rate not known; the model's TSC runs at 2 GHz. */
static int vclock_on;
static uint32_t tick_counts;

int vclock_running(void)
{
    return vclock_on;
}

/* The count of the clock's stops and starts, which a row raises with each
 * change of `vclock_on` that KVM is to see. */
static uint32_t vclock_gen;

uint32_t vclock_generation(void)
{
    return vclock_gen;
}

/** The TSC's clock `which` and the TSC read together. The kvmclock counts
 * the raw clock, as Linux's get_kvmclock_base_ns counts ktime_get_raw
 * (arch/x86/kvm/x86.c:1870-1874), so a reading of the monotonic clock, which
 * the NTP state slews, is a failure, and answers 7 ms apart from the raw
 * clock, so that rows that read it see the difference. */
int vclock_read_cycles(int which, uint64_t *ns, uint64_t *cycles)
{
    if (!vclock_on) {
        return 0;
    }
    CHECK(which != VCLOCK_READ_MONO, "the kvmclock reads the raw clock, never the slewed one");
    *ns = which == VCLOCK_READ_REAL  ? real_ns
          : which == VCLOCK_READ_RAW ? mono_ns
                                     : mono_ns + 7000000u;
    *cycles = tsc;
    return 1;
}

uint32_t timer_calibrate(void)
{
    return tick_counts;
}

/** Let `ns` nanoseconds pass on every clock. */
static void clock_pass(uint64_t ns)
{
    mono_ns += ns;
    real_ns += ns;
    tsc += ns * 2u;
}

/* The big lock's depth on this processor, the interrupt flag, the
 * processors kicked and a pending signal. KVM_RUN is called without the big
 * lock, as the system call layer calls an unlocked file's operations. */
static int bkl_depth, irq_on = 1, kicked = -1, sig_pending;

/* The TSC cycles the next take of the big lock waits, as another holder
 * would make it wait. */
static uint64_t bkl_wait;

/** Take the big lock: one level deeper, after `bkl_wait` cycles. */
void bkl_enter(void)
{
    tsc += bkl_wait;
    bkl_wait = 0;
    bkl_depth++;
}

/** Give back one level of the big lock. */
void bkl_exit(void)
{
    CHECK(bkl_depth > 0, "the big lock is given back only when held");
    bkl_depth--;
}

/** Whether this thread holds the big lock. */
int bkl_held(void)
{
    return bkl_depth > 0;
}

/** Let go of the big lock at any depth, as KVM_RUN does with a hold the
 * system call's entry took: the depth. */
int bkl_drop(void)
{
    int depth = bkl_depth;

    CHECK(!irq_on, "the big lock is let go with interrupts off");
    bkl_depth = 0;
    return depth;
}

/** Take the big lock back at `depth`. */
void bkl_take(int depth)
{
    CHECK(!irq_on && bkl_depth == 0, "the big lock is taken back with interrupts off, once");
    bkl_depth = depth;
}

/** Nothing waits on this processor's requests in a test of one thread. */
void smp_service(void)
{}

uint32_t kirq_save(void)
{
    uint32_t f = (uint32_t)irq_on;

    irq_on = 0;
    return f;
}

void kirq_restore(uint32_t flags)
{
    irq_on = (int)flags;
}

int smp_cpu_count(void)
{
    return 4;
}

void smp_kick(int cpu)
{
    kicked = cpu;
}

/** A signal the row posts: `run_signal` calls this only once the
 * caller's signal-work flag is set, and only under the big lock, which
 * the signal code needs. */
int signal_pending(void)
{
    CHECK(bkl_depth > 0, "signal_pending runs under the big lock");
    return sig_pending;
}

/* The run loop's switch at a slice's end, which a row asks for by setting
 * the caller's need_resched: the switch calls the thread's `sched_out`, as
 * `schedule` does, the thread waits `preempt_wait` ns on a run queue and is
 * switched in again, and the steal time's `preempted` byte at
 * `preempt_seen_at` is kept as the switch found it. With `preempt_alone`
 * no other thread is queued, so the request switches to none. */
static uint64_t preempt_wait;
static const uint8_t *preempt_seen_at;
static int preempt_seen = -1;
static int preempt_alone;

/* The run loop's passes over its top, each of which takes a switch here,
 * counted while `pass_guard` is set. Past 1000 passes the loop would never
 * enter: the guard takes back that vCPU's KVM_REQ_TLB_FLUSH, so that the
 * row fails on what its entry found instead. */
static struct kvm_vcpu *pass_guard;
static int loop_passes;

void sched_take_preempt(void)
{
    if (pass_guard && ++loop_passes > 1000) {
        pass_guard->requests &= ~KVM_REQ_TLB_FLUSH;
    }
    if (caller.need_resched) {
        caller.need_resched = 0;
        if (preempt_alone) {
            return;
        }
        CHECK(caller.sched_out != NULL, "a vCPU's thread has its sched_out across KVM_RUN");
        caller.nivcsw++;
        caller.sched_out(caller.sched_out_arg, 1);
        caller.run_delay += preempt_wait;
        caller.pcount++;
        preempt_seen = preempt_seen_at ? *preempt_seen_at : -1;
    }
}

void signal_set_blocked(uint64_t mask)
{
    current_proc->sig_blocked = mask;
}

int wq_wait_event(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    (void)cond;
    (void)arg;
    return -EINTR;
}

int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    (void)wq;
    CHECK(cond(arg), "a vCPU is never held by another ioctl in the model");
    return 0;
}

void wq_wake(wait_queue_t *wq, uint32_t kinds)
{
    (void)wq;
    (void)kinds;
}

void wait_queue_init(wait_queue_t *wq)
{
    wq->head = NULL;
}

void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)level;
    (void)subsystem;
    (void)format;
}

/** A frame is its own address in the model. */
void *kmap_temp(uintptr_t phys)
{
    return (void *)phys;
}

void kunmap_temp(void *va)
{
    (void)va;
}

/* The last device mapping made: its operations and object. */
static const struct vm_device_ops *map_ops;
static void *map_obj;
static uint32_t map_cache;

int vm_device_map(vma_t *vma, const struct vm_device_ops *ops, void *obj, uint32_t cache)
{
    if (vma->type != VM_SHARED) {
        return -EINVAL;
    }
    map_ops = ops;
    map_obj = obj;
    map_cache = cache;
    return 0;
}

/* Whether the model has XSAVEC and XSAVES, which the Haswell lacks, and
 * the fake backend lets a guest run XSAVES. */
static int model_xsaves;

/* Whether the model lacks XSAVE, OSXSAVE and AVX, as a processor before
 * the Core 2 of 45 nm does. */
static int model_no_xsave;

/* The speculation controls of spec_rows' hosts, which the Haswell's model
 * otherwise lacks as MSRs: with `model_spec_bits`, IA32_SPEC_CTRL takes
 * those bits; with `model_virt`, CPUID offers VIRT_SSBD and the host has
 * its VIRT_SPEC_CTRL, as beneath another hypervisor; with `model_amd17`,
 * the processor is AMD's family 17h, two threads to a core, with LS_CFG,
 * and `model_apic` is the extended APIC ID of the processor that asks.
 * The MSRs hold `model_msr_*`, and the model logs every write that
 * succeeds in `model_wr`. */
static uint64_t model_spec_bits;
static int model_virt, model_amd17;
static uint32_t model_apic;
static uint64_t model_msr_spec, model_msr_virt, model_msr_ls;
static struct {
    uint32_t index;
    uint64_t value;
} model_wr[8];
static int model_wrs;

/* The processor: a model of the Lenovo's Haswell, i7-4xxx. */
void kcpu_id_sub(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    uint32_t r[4] = {0, 0, 0, 0};

    switch (leaf) {
    case 0:
        r[0] = 0xD;
        r[1] = 0x756E6547; /* GenuineIntel */
        r[2] = 0x6C65746E;
        r[3] = 0x49656E69;
        break;
    case 1:
        r[0] = 0x000306C3;
        r[1] = 0x00100800;
        r[2] = 0x7FFAFBBFu; /* with VMX, x2APIC, OSXSAVE */
        if (model_no_xsave) {
            r[2] &= ~0x1C000000u; /* XSAVE, OSXSAVE, AVX */
        }
        r[3] = 0xBFEBFBFFu;
        break;
    case 4:
        if (sub < 4) {
            r[0] = 0x1C004121u + sub;
        }
        break;
    case 6:
        r[0] = 0x77;
        break;
    case 7:
        if (sub == 0) {
            r[1] = 0x000027ABu; /* FSGSBASE TSC_ADJUST BMI1 AVX2 SMEP BMI2 ERMS INVPCID */
            r[3] = 0x9C000400u; /* MD_CLEAR, the speculation controls */
        }
        break;
    case 0xA:
        r[0] = 0x07300403;
        break;
    case 0xD:
        if (sub == 0) {
            r[0] = 0x7;
            r[1] = r[2] = 832;
        } else if (sub == 1) {
            r[0] = model_xsaves ? 0xF : 0x1; /* XSAVEOPT, and XSAVEC, XGETBV1, XSAVES */
        } else if (sub == 2) {
            r[0] = 256;
            r[1] = 576;
        }
        break;
    case 0x80000000u:
        r[0] = 0x80000008u;
        break;
    case 0x80000001u:
        r[2] = 0x21;
        r[3] = 0x2C100800u;
        break;
    case 0x80000007u:
        r[3] = 0x100;
        break;
    case 0x80000008u:
        r[0] = 0x3027;
        r[1] = model_virt ? 1u << 25 : 0u; /* VIRT_SSBD */
        break;
    default:
        break;
    }
    if (model_amd17) {
        if (leaf == 0) {
            r[1] = 0x68747541; /* AuthenticAMD */
            r[2] = 0x444D4163;
            r[3] = 0x69746E65;
        } else if (leaf == 1) {
            r[0] = 0x00800F11; /* family 17h, model 1 */
        } else if (leaf == 0x80000000u) {
            r[0] = 0x8000001Eu;
        } else if (leaf == 0x8000001Eu) {
            r[0] = model_apic;
            r[1] = 0x100; /* two threads to a core */
        }
    }
    *a = r[0];
    *b = r[1];
    *c = r[2];
    *d = r[3];
}

void kcpu_id(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    kcpu_id_sub(leaf, 0, a, b, c, d);
}

/** The model's MSRs: the Haswell's microcode revision 0x28 in
 * UCODE_REV's high half, and 0x5 for every other. */
void kcpu_rdmsr(uint32_t msr, uint32_t *lo, uint32_t *hi)
{
    *lo = msr == 0x8Bu ? 0 : 0x5;
    *hi = msr == 0x8Bu ? 0x28 : 0;
}

/** The host PMU driver's count of its writes, which this model's
 * constant MSRs never need to move. */
uint32_t platform_pmu_writes(void)
{
    return 0;
}

/** Physical counter writes are unused by this model's fake backend. */
void kcpu_wrmsr(uint32_t msr, uint32_t lo, uint32_t hi)
{
    (void)msr;
    (void)lo;
    (void)hi;
}

/** The speculation controls of spec_rows' hosts, `model_spec_bits`,
 * `model_virt` and `model_amd17`; the model has no other, and no AMD PMU. */
static uint64_t *model_msr(uint32_t msr)
{
    if (msr == 0x48u && model_spec_bits) {
        return &model_msr_spec;
    }
    if (msr == 0xC001011Fu && model_virt) {
        return &model_msr_virt;
    }
    if (msr == 0xC0011020u && model_amd17) {
        return &model_msr_ls;
    }
    return NULL;
}

int kcpu_rdmsr_safe(uint32_t msr, uint64_t *value)
{
    uint64_t *reg = model_msr(msr);

    if (!reg) {
        return -1;
    }
    *value = *reg;
    return 0;
}

int kcpu_wrmsr_safe(uint32_t msr, uint64_t value)
{
    uint64_t *reg = model_msr(msr);

    if (!reg || (msr == 0x48u && (value & ~model_spec_bits))) {
        return -1;
    }
    *reg = value;
    if (model_wrs < 8) {
        model_wr[model_wrs].index = msr;
        model_wr[model_wrs].value = value;
        model_wrs++;
    }
    return 0;
}

/** No virtual APIC is armed for a performance interrupt here. */
void kvm_lapic_pmi(struct kvm_vcpu *v)
{
    (void)v;
}

/* The fake backend's mode: refuse every entry, or report the exits of
 * `script` in turn, each with the processor's exit code of its slot in
 * `script_hw`; with `refuse_at_end`, refuse the entry that would run past
 * the script. With `late_request` set, an exit asks for an event, and the
 * event's injection once the script is spent leaves `late_request`'s
 * request after the run loop has looked for it, so the entry that follows
 * finds it. With `late_resched` set the injection asks for a switch in the
 * same way, and `resched_entered` is set when an entry found that switch
 * still asked for. `entry_requests` are the vCPU's requests at the last
 * entry. */
static int refusing = 1, refuse_at_end, late_resched, resched_entered;
static uint32_t late_request, entry_requests;
static struct kvm_exit script[16];
static uint32_t script_hw[16];
static int script_len, script_at, entries_made, completes, injects;
static uint64_t completed;

/** Whether the processor has the extension: the model has VMX. */
static int fake_probe(void)
{
    return 1;
}

/** Enter: with interrupts off and without the big lock, refuse, or count. */
static int fake_enter(struct kvm_vcpu *v)
{
    (void)v;
    CHECK(!irq_on && bkl_depth == 0, "the entry runs with interrupts off and no big lock");
    CHECK(v->mode == KVM_MODE_IN_GUEST, "the vCPU is in the guest during the entry");
    entries_made++;
    entry_requests = v->requests;
    resched_entered |= caller.need_resched != 0;
    if (refusing || (refuse_at_end && script_at == script_len)) {
        return -EOPNOTSUPP;
    }
    v->hw_exit = script_hw[script_at];
    return 0;
}

/* An exit the kernel takes for a host interrupt, KVM_X_AGAIN, is one at an
 * instruction boundary whose interrupt asks for a switch, when set. */
static int tick_exits;
static uint32_t ring_writes;
static int block_turn, block_seen = -1; /* the sleep of `kvm_lapic_prepare` */

/** The next exit of the script. */
static int fake_exit(struct kvm_vcpu *v)
{
    struct kvm_slot_view s;
    uint32_t i;

    CHECK(irq_on && bkl_depth == 0,
          "an exit is handled with interrupts on and without the big lock, as Linux's");
    if (script_at == script_len) {
        return -EIO;
    }
    v->exit = script[script_at++];
    k_memset(&s, 0, sizeof(s));
    s.flags = KVM_MEM_LOG_DIRTY_PAGES;
    s.id = 3;
    for (i = 0; i < ring_writes; i++) {
        (void)kvm_dirty_mark(v, &s, i);
    }
    ring_writes = 0;
    v->if_flag = 1;
    v->irq_allowed = 1;
    if (late_request || late_resched) {
        req_set(v, KVM_REQ_EVENT);
    }
    if (tick_exits && v->exit.kind == KVM_X_AGAIN) {
        v->at_boundary = 1;
        caller.need_resched = 1;
    }
    if (block_turn == 1 && v->vm->irqchip != KVM_IRQCHIP_NONE) {
        v->mp_state = KVM_MP_STATE_HALTED;
    }
    return 0;
}

/** The answer of a read: the first eight bytes of it, kept. */
static void fake_complete(struct kvm_vcpu *v)
{
    completes++;
    k_memcpy(&completed, v->exit.kind == KVM_X_IO ? v->exit.data : v->exit.mmio.data, 8);
}

/** An event queued for the entry: counted. */
static void fake_inject(struct kvm_vcpu *v)
{
    injects++;
    if (late_request && script_at == script_len) {
        req_set(v, late_request);
        late_request = 0;
    }
    if (late_resched && script_at == script_len) {
        caller.need_resched = 1;
        late_resched = 0;
    }
}

/* The calls of the backend's msr_filter_changed. */
static int filter_changes;

/** The backend's msr_filter_changed: counted. */
static void fake_msr_filter_changed(struct kvm_vcpu *v)
{
    (void)v;
    filter_changes++;
}

/* The backend lets no guest run XSAVES though the host has it, as SVM on
 * a Zen 2 with Erratum 1386 unfixed. */
static int backend_no_xsaves;

/** The backend's xsaves: `model_xsaves`, unless `backend_no_xsaves`. */
static int fake_xsaves(void)
{
    return model_xsaves && !backend_no_xsaves;
}

/** The capabilities a backend makes true: single-stepping and guest
 * debugging. */
static int32_t fake_backend_cap(long cap)
{
    return cap == KVM_CAP_X86_ROBUST_SINGLESTEP || cap == KVM_CAP_SET_GUEST_DEBUG;
}

/* The width of the fraction of the ratio the model's processor scales a
 * TSC by, SVM's 32 or VMX's 48, and 0 for a processor that scales none. */
static uint32_t scale_bits;

/** The backend's tsc_scaling: the largest ratio of `scale_bits`. */
static uint64_t fake_tsc_scaling(uint32_t *frac_bits)
{
    *frac_bits = scale_bits;
    return scale_bits == 32 ? 0xFFFFFFFFFFull : scale_bits ? ~0ull : 0;
}

const struct kvm_backend kvm_vmx = {"vmx",
                                    fake_probe,
                                    NULL,
                                    NULL,
                                    NULL,
                                    NULL,
                                    fake_enter,
                                    fake_exit,
                                    fake_complete,
                                    NULL,
                                    fake_inject,
                                    fake_backend_cap,
                                    NULL,
                                    fake_tsc_scaling,
                                    fake_msr_filter_changed,
                                    fake_xsaves};
const struct kvm_backend kvm_svm = {"svm", fake_probe, NULL, NULL, NULL, NULL, NULL,
                                    NULL,  NULL,       NULL, NULL, NULL, NULL};

/* The fakes of the later tasks: slots, routing, the bus, the tables, the
 * dirty log and the local APIC, answering their capabilities. */
static int slots_set;

int kvm_slots_init(struct kvm_vm *vm)
{
    vm->slots = NULL;
    return 0;
}

void kvm_slots_free(struct kvm_vm *vm)
{
    (void)vm;
}

int32_t kvm_slots_set(struct kvm_vm *vm, const struct kvm_userspace_memory_region *r)
{
    (void)vm;
    (void)r;
    slots_set++;
    return 0;
}

int32_t kvm_slots_check_extension(long cap)
{
    switch (cap) {
    case KVM_CAP_USER_MEMORY:
    case KVM_CAP_DESTROY_MEMORY_REGION_WORKS:
    case KVM_CAP_JOIN_MEMORY_REGIONS_WORKS:
        return 1;
    case KVM_CAP_NR_MEMSLOTS:
        return KVM_MEM_SLOTS;
    default:
        return 0;
    }
}

int kvm_tdp_init(struct kvm_vm *vm)
{
    (void)vm;
    return 0;
}

void kvm_tdp_free(struct kvm_vm *vm)
{
    (void)vm;
}

/* What the model's page tables say to KVM_PRE_FAULT_MEMORY: the first
 * `pre_fault_pages` pages of a range are mapped, and the next has no slot. */
static uint64_t pre_fault_pages;

int32_t kvm_tdp_pre_fault(struct kvm_vm *vm, struct kvm_pre_fault_memory *range)
{
    uint64_t n = range->size / 4096u < pre_fault_pages ? range->size / 4096u : pre_fault_pages;

    (void)vm;
    range->gpa += n * 4096u;
    range->size -= n * 4096u;
    return n ? 0 : -ENOENT;
}

uint32_t kvm_mmu_lock(struct kvm_vm *vm)
{
    return spin_lock_irqsave(&vm->mmu_lock);
}

void kvm_mmu_unlock(struct kvm_vm *vm, uint32_t flags)
{
    spin_unlock_irqrestore(&vm->mmu_lock, flags);
}

int kvm_slot_get(struct kvm_vm *vm, uint32_t id, struct kvm_slot_view *out)
{
    (void)vm;
    (void)id;
    (void)out;
    return 0;
}

int kvm_tdp_protect(struct kvm_vm *vm, uint64_t gfn, uint64_t mask)
{
    (void)vm;
    (void)gfn;
    (void)mask;
    return 0;
}

void kvm_tdp_flush(struct kvm_vm *vm)
{
    (void)vm;
}

int32_t kvm_irq_ioctl(struct kvm_vm *vm, uint32_t request, uintptr_t arg)
{
    (void)vm;
    (void)request;
    (void)arg;
    return -ENOTTY;
}

/** The split controller once a vCPU exists, which Linux refuses. */
int32_t kvm_irq_enable_split(struct kvm_vm *vm, const struct kvm_enable_cap *cap)
{
    (void)cap;
    return vm->nr_vcpus ? -EEXIST : -EINVAL;
}

int32_t kvm_irq_check_extension(long cap)
{
    return cap == KVM_CAP_IRQ_ROUTING || cap == KVM_CAP_IRQFD || cap == KVM_CAP_SIGNAL_MSI ||
           cap == KVM_CAP_SPLIT_IRQCHIP;
}

void kvm_irq_free(struct kvm_vm *vm)
{
    (void)vm;
}

/** No I/O APIC in this model, so no pin has counts. */
int kvm_ioapic_counts(struct kvm_vm *vm, uint32_t pin, uint32_t *counts)
{
    (void)vm;
    (void)pin;
    (void)counts;
    return -1;
}

/** Legacy I/O is not part of this model. */
int kvm_irq_io(struct kvm_vcpu *v)
{
    (void)v;
    return 0;
}

int32_t kvm_iobus_ioctl(struct kvm_vm *vm, uint32_t request, uintptr_t arg)
{
    (void)vm;
    (void)request;
    (void)arg;
    return -ENOTTY;
}

/* A port the fake bus takes, as an ioeventfd would. */
#define BUS_PORT 0x510

int kvm_iobus_write(struct kvm_vcpu *v)
{
    return v->exit.kind == KVM_X_IO && v->exit.io.port == BUS_PORT;
}

int32_t kvm_iobus_check_extension(long cap)
{
    return cap == KVM_CAP_IOEVENTFD || cap == KVM_CAP_IOEVENTFD_ANY_LENGTH;
}

void kvm_iobus_free(struct kvm_vm *vm)
{
    (void)vm;
}

int kvm_iobus_has_eventfd(struct kvm_vm *vm, const struct eventfd *ev)
{
    (void)vm;
    (void)ev;
    return 0;
}

void kvm_gpamap_vm_end(struct kvm_vm *vm)
{
    (void)vm;
}

int32_t kvm_lapic_ioctl(struct kvm_vcpu *v, uint32_t request, uintptr_t arg)
{
    (void)v;
    (void)request;
    (void)arg;
    return -ENOTTY;
}

int kvm_lapic_halt(struct kvm_vcpu *v)
{
    (void)v;
    return 1;
}

void kvm_lapic_free(struct kvm_vcpu *v)
{
    (void)v;
}

int kvm_lapic_create(struct kvm_vcpu *v)
{
    (void)v;
    return 0;
}

int kvm_lapic_mmio(struct kvm_vcpu *v)
{
    (void)v;
    return 0;
}

int kvm_lapic_ready(struct kvm_vcpu *v)
{
    (void)v;
    return 0;
}

/* A halted vCPU's sleep at the run loop's top, which a row asks for with
 * `block_turn`, the pass over the loop's top it comes at, counting down:
 * the exit before that pass halts the vCPU (`fake_exit`), so that the loop
 * calls `kvm_lapic_prepare`, and the thread blocks in `wq_wait_event`,
 * whose `schedule` calls its `sched_out` with interrupts off, not
 * preempted, and is woken, switched in again and runnable; the steal
 * time's `preempted` byte at `preempt_seen_at` is kept in `block_seen` as
 * the switch found it. */
int32_t kvm_lapic_prepare(struct kvm_vcpu *v)
{
    if (block_turn && --block_turn == 0) {
        CHECK(irq_on && caller.sched_out != NULL,
              "a halted vCPU sleeps with interrupts on, its thread's sched_out set");
        irq_on = 0;
        caller.nvcsw++;
        caller.sched_out(caller.sched_out_arg, 0);
        caller.pcount++;
        block_seen = preempt_seen_at ? *preempt_seen_at : -1;
        irq_on = 1;
        v->mp_state = KVM_MP_STATE_RUNNABLE;
    }
    return 0;
}

/** vlapic.c's and i8254.c's timer checks, which KVM_REQ_TIMER runs: no
 * timer is armed here. */
void kvm_lapic_timer_check(struct kvm_vcpu *v)
{
    (void)v;
}

void kvm_pit_check(struct kvm_vm *vm)
{
    (void)vm;
}

uint64_t kvm_lapic_next_ns(struct kvm_vcpu *v)
{
    (void)v;
    return 0;
}

uint64_t kvm_lapic_cr8(struct kvm_vcpu *v)
{
    return v->sregs.cr8;
}

void kvm_lapic_set_cr8(struct kvm_vcpu *v, uint64_t cr8)
{
    v->sregs.cr8 = cr8;
}

int kvm_lapic_set_base(struct kvm_vcpu *v, uint64_t value, int host)
{
    (void)host;
    v->sregs.apic_base = value;
    return 0;
}

/** The APIC's MSRs with no APIC in the kernel, as vlapic.c answers them:
 * the TSC deadline reads 0 and takes any write; the rest are not its. */
int kvm_lapic_msr(struct kvm_vcpu *v, uint32_t index, int write, uint64_t *value, int host)
{
    (void)v;
    (void)host;
    if (index != 0x6E0) {
        return 1;
    }
    if (!write) {
        *value = 0;
    }
    return 0;
}

/* What hyperv.c calls of emulate.c, vlapic.c, eventfd.c and the clock,
 * for a VM with no guest memory, no APIC and no eventfd. */

/* Two guest pages at GUEST_PAGE, present only while guest_mem is set, for
 * the kvmclock's rows, the second in a region of memfd_secret; the Hyper-V
 * rows see no guest memory. */
#define GUEST_PAGE 0x1000u
static uint8_t guest_page[2 * 4096];
static int guest_mem;
static int low_rw; /* accesses below GUEST_PAGE, which no row maps */

int kvm_guest_phys_rw(struct kvm_vcpu *v, uint64_t gpa, void *buf, uint32_t n, int write)
{
    (void)v;
    if (gpa < GUEST_PAGE) {
        low_rw++;
    }
    if (!guest_mem || gpa < GUEST_PAGE || gpa + n > GUEST_PAGE + sizeof(guest_page)) {
        return 1;
    }
    if (write) {
        memcpy(guest_page + (gpa - GUEST_PAGE), buf, n);
    } else {
        memcpy(buf, guest_page + (gpa - GUEST_PAGE), n);
    }
    return 0;
}

/** The exchange of the steal time's `preempted`: a read and a write, since
 * no guest runs beside the test's one thread to come between them. */
int kvm_guest_phys_clear(struct kvm_vcpu *v, uint64_t gpa, uint32_t bits, uint32_t *old)
{
    uint32_t w = 0;

    *old = 0;
    if (kvm_guest_phys_rw(v, gpa, &w, 4, 0) != 0) {
        return 1;
    }
    *old = w;
    w &= ~bits;
    return kvm_guest_phys_rw(v, gpa, &w, 4, 1);
}

/* The caller's region over the first guest page, which a row marks
 * VM_PROT_SECRET, as memfd_secret's, and the one over the second, always
 * memfd_secret's. */
static vma_t guest_vma, secret_vma;

/** The VM's read side: one thread, so a section needs nothing. */
uint32_t kvm_srcu_read_lock(struct kvm_vm *vm)
{
    (void)vm;
    return 0;
}

/** The end of a section. */
void kvm_srcu_read_unlock(struct kvm_vm *vm, uint32_t idx)
{
    (void)vm;
    (void)idx;
}

/** A replacement's wait for the sections: none is under way. */
void kvm_srcu_synchronize(struct kvm_vm *vm)
{
    (void)vm;
}

/** The slot of a guest page, which the steal time's record vouches for: 1
 * with `out` filled while guest_mem is set, or 0. */
int kvm_slot_find(struct kvm_vm *vm, uint64_t gfn, struct kvm_slot_view *out)
{
    uint64_t first = GUEST_PAGE >> 12;

    (void)vm;
    if (!guest_mem || gfn < first || gfn - first >= sizeof(guest_page) / 4096u) {
        return 0;
    }
    k_memset(out, 0, sizeof(*out));
    out->base_gfn = gfn;
    out->npages = 1;
    out->hva = (uint64_t)(uintptr_t)(guest_page + (gfn - first) * 4096u);
    return 1;
}

/** The region at `addr`: the second guest page's or the first's. */
vma_t *vm_find(vm_space_t *vm, uintptr_t addr)
{
    (void)vm;
    if (addr >= (uintptr_t)guest_page + 4096u) {
        secret_vma.prot = VM_PROT_SECRET;
        return &secret_vma;
    }
    return &guest_vma;
}

/** Nothing to wait for in a test of one thread. */
void mmap_read_lock(struct mm *mm)
{
    (void)mm;
}

/** Give back `mmap_read_lock`: nothing. */
void mmap_unlock(struct mm *mm)
{
    (void)mm;
}

/* The no-fault write of the steal time's preempted byte, into the same
 * page; `nofault_refuse` stands for a page that is not present. */
static int nofault_refuse;

int kvm_tdp_write_nofault(struct kvm_vm *vm, uint64_t gpa, const void *buf, uint32_t n,
                          uint32_t seq)
{
    (void)vm;
    (void)seq;
    if (nofault_refuse || !guest_mem || gpa < GUEST_PAGE ||
        gpa + n > GUEST_PAGE + sizeof(guest_page)) {
        return 1;
    }
    memcpy(guest_page + (gpa - GUEST_PAGE), buf, n);
    return 0;
}

int kvm_lapic_set_irq(struct kvm_vcpu *v, uint32_t vector)
{
    (void)v;
    (void)vector;
    return -1;
}

int kvm_lapic_hv_msr(struct kvm_vcpu *v, uint32_t reg, int write, uint64_t *value)
{
    (void)v;
    (void)reg;
    (void)write;
    (void)value;
    return 1;
}

/* The vector of the last exception queued, -1 for none. */
static int queued = -1;

void kvm_queue_exception(struct kvm_vcpu *v, uint8_t nr, int has_error, uint32_t error)
{
    (void)v;
    (void)has_error;
    (void)error;
    queued = nr;
}

/** emulate.c's: whether the CPUID names AMD, "AuthenticAMD". */
int kvm_guest_amd(const struct kvm_vcpu *v)
{
    const struct kvm_cpuid_entry2 *e = kvm_cpuid_find(v, 0, 0);

    return e && e->ebx == 0x68747541u && e->edx == 0x69746E65u && e->ecx == 0x444D4163u;
}

int kvm_emul_cpl(const struct kvm_vcpu *v)
{
    (void)v;
    return 0;
}

int kvm_emul_mode(const struct kvm_vcpu *v)
{
    (void)v;
    return 64;
}

void kvm_emul_skip(struct kvm_vcpu *v, uint32_t len)
{
    v->regs.rip += len;
}

void kvm_emul_stepped(struct kvm_vcpu *v)
{
    v->emul.step_trap = 0;
}

struct eventfd *eventfd_get(int fd, int32_t *err)
{
    (void)fd;
    *err = -EBADF;
    return NULL;
}

void eventfd_put(struct eventfd *ev)
{
    (void)ev;
}

void eventfd_signal(struct eventfd *ev)
{
    (void)ev;
}

void arch_relax(void)
{}

/* /dev/kvm's descriptor. */
static fd_entry_t dev_entry;

/** An ioctl of /dev/kvm. */
static int32_t dev(uint32_t request, uintptr_t arg)
{
    return registered->ops->ioctl(&dev_entry, request, arg);
}

/** An ioctl of the descriptor `fd`. */
static int32_t fdio(int fd, uint32_t request, uintptr_t arg)
{
    return entries[fd].ops->ioctl(&entries[fd], request, arg);
}

/** Close the descriptor `fd`: its object is released. */
static void fdclose(int fd)
{
    entries[fd].ops->close(&entries[fd]);
    k_memset(&entries[fd], 0, sizeof(entries[fd]));
    reserved[fd] = 0;
}

/** Check the sizes and offsets of the structures, and ioctl numbers,
 * against Linux's x86-64 values. */
static void abi_rows(void)
{
    CHECK(sizeof(struct kvm_regs) == 144 && sizeof(struct kvm_sregs) == 312,
          "kvm_regs and kvm_sregs have Linux's sizes");
    CHECK(sizeof(struct kvm_fpu) == 416 && sizeof(struct kvm_vcpu_events) == 64,
          "kvm_fpu and kvm_vcpu_events have Linux's sizes");
    CHECK(sizeof(struct kvm_debugregs) == 128 && sizeof(struct kvm_xcrs) == 392,
          "kvm_debugregs and kvm_xcrs have Linux's sizes");
    CHECK(sizeof(struct kvm_clock_data) == 48 && sizeof(struct kvm_enable_cap) == 104,
          "kvm_clock_data and kvm_enable_cap have Linux's sizes");
    CHECK(sizeof(struct kvm_x86_mce) == 64 && sizeof(struct kvm_guest_debug) == 72,
          "kvm_x86_mce and kvm_guest_debug have Linux's sizes");
    CHECK(sizeof(struct kvm_cpuid_entry2) == 40 && sizeof(struct kvm_userspace_memory_region) == 32,
          "kvm_cpuid_entry2 and the memory region have Linux's sizes");
    CHECK(__builtin_offsetof(struct kvm_run, u) == 32 &&
              __builtin_offsetof(struct kvm_run, kvm_valid_regs) == 288 &&
              __builtin_offsetof(struct kvm_run, s) == 304,
          "kvm_run's union, valid registers and sync area are where Linux has them");
    CHECK(__builtin_offsetof(struct kvm_vcpu_events, exception_payload) == 56,
          "the exception payload is where Linux has it");
    CHECK(KVM_CAP_DEVICE_CTRL == 89 && kvm_check_extension(NULL, KVM_CAP_DEVICE_CTRL) == 1,
          "the VFIO device-control capability is present");
    CHECK(KVM_RUN == 0xAE80u && KVM_CREATE_VM == 0xAE01u, "KVM_RUN and KVM_CREATE_VM");
    CHECK(KVM_CAP_IMMEDIATE_EXIT == 136 && KVM_CAP_CHECK_EXTENSION_VM == 105 &&
              KVM_CAP_SPLIT_IRQCHIP == 121 && KVM_CAP_IOEVENTFD_ANY_LENGTH == 122,
          "the capabilities' numbers past 100");
    CHECK(KVM_CAP_MAX_VCPU_ID == 128 && KVM_CAP_X2APIC_API == 129 &&
              KVM_CAP_GET_MSR_FEATURES == 153 && KVM_CAP_ENABLE_CAP_VM == 98,
          "and more of them");
    CHECK(KVM_GET_REGS == 0x8090AE81u && KVM_SET_SREGS == 0x4138AE84u,
          "KVM_GET_REGS and KVM_SET_SREGS");
    CHECK(KVM_SET_CPUID2 == 0x4008AE90u && KVM_GET_SUPPORTED_CPUID == 0xC008AE05u,
          "KVM_SET_CPUID2 and KVM_GET_SUPPORTED_CPUID");
    CHECK(KVM_SET_USER_MEMORY_REGION == 0x4020AE46u && KVM_ENABLE_CAP == 0x4068AEA3u,
          "KVM_SET_USER_MEMORY_REGION and KVM_ENABLE_CAP");
    CHECK(KVM_GET_TSC_KHZ == 0xAEA3u, "KVM_GET_TSC_KHZ");
    CHECK(KVM_SET_USER_MEMORY_REGION2 == 0x40A0AE49u &&
              sizeof(struct kvm_userspace_memory_region2) == 160,
          "KVM_SET_USER_MEMORY_REGION2");
    CHECK(KVM_GET_XSAVE == 0x9000AEA4u && KVM_GET_XCRS == 0x8188AEA6u,
          "KVM_GET_XSAVE and KVM_GET_XCRS");
    CHECK(KVM_GET_XSAVE2 == 0x9000AECFu && KVM_CAP_XSAVE2 == 208,
          "KVM_GET_XSAVE2 and KVM_CAP_XSAVE2");
    CHECK(KVM_GET_VCPU_EVENTS == 0x8040AE9Fu && KVM_GET_CLOCK == 0x8030AE7Cu,
          "KVM_GET_VCPU_EVENTS and KVM_GET_CLOCK");
}

/** Check Hyper-V's structures, ioctls and capabilities against Linux's. */
static void hv_abi_rows(void)
{
    CHECK(sizeof(struct kvm_hyperv_exit) == 56 && sizeof(struct kvm_hyperv_eventfd) == 24 &&
              sizeof(struct kvm_irq_routing_hv_sint) == 8,
          "Hyper-V's exit, eventfd and SINT route have Linux's sizes");
    CHECK(KVM_GET_SUPPORTED_HV_CPUID == 0xC008AEC1u && KVM_HYPERV_EVENTFD == 0x4018AEBDu &&
              KVM_EXIT_HYPERV == 27,
          "KVM_GET_SUPPORTED_HV_CPUID, KVM_HYPERV_EVENTFD and KVM_EXIT_HYPERV");
    CHECK(KVM_CAP_HYPERV_TIME == 96 && KVM_CAP_HYPERV_SYNIC == 123 &&
              KVM_CAP_HYPERV_SYNIC2 == 148 && KVM_CAP_HYPERV_VP_INDEX == 149 &&
              KVM_CAP_HYPERV_EVENTFD == 154 && KVM_CAP_HYPERV_TLBFLUSH == 155 &&
              KVM_CAP_HYPERV_SEND_IPI == 161 && KVM_CAP_HYPERV_ENLIGHTENED_VMCS == 163 &&
              KVM_CAP_HYPERV_CPUID == 167 && KVM_CAP_HYPERV_DIRECT_TLBFLUSH == 175 &&
              KVM_CAP_SYS_HYPERV_CPUID == 191 && KVM_CAP_HYPERV_ENFORCE_CPUID == 199,
          "the Hyper-V capabilities' numbers");
}

/* The capabilities QEMU 10 requires of KVM on x86 (kvm-all.c and
 * target/i386/kvm/kvm.c). */
static const long required[] = {KVM_CAP_USER_MEMORY,
                                KVM_CAP_DESTROY_MEMORY_REGION_WORKS,
                                KVM_CAP_JOIN_MEMORY_REGIONS_WORKS,
                                KVM_CAP_INTERNAL_ERROR_DATA,
                                KVM_CAP_IOEVENTFD,
                                KVM_CAP_IOEVENTFD_ANY_LENGTH,
                                KVM_CAP_IRQFD,
                                KVM_CAP_SET_TSS_ADDR,
                                KVM_CAP_EXT_CPUID,
                                KVM_CAP_MP_STATE,
                                KVM_CAP_SIGNAL_MSI,
                                KVM_CAP_IRQ_ROUTING,
                                KVM_CAP_DEBUGREGS,
                                KVM_CAP_XSAVE,
                                KVM_CAP_VCPU_EVENTS,
                                KVM_CAP_X86_ROBUST_SINGLESTEP,
                                KVM_CAP_MCE,
                                KVM_CAP_ADJUST_CLOCK,
                                KVM_CAP_SET_IDENTITY_MAP_ADDR};

/* A buffer for the ioctls that take a header and entries. */
static union {
    struct kvm_cpuid2 cpuid;
    struct kvm_msrs msrs;
    struct kvm_msr_list list;
    uint8_t bytes[16384];
} buf;

/** The entries of `buf` after an 8-byte header. */
#define CPUID_ENTRIES ((struct kvm_cpuid_entry2 *)(buf.bytes + 8))
#define MSR_ENTRIES   ((struct kvm_msr_entry *)(buf.bytes + 8))
#define MSR_INDICES   ((uint32_t *)(buf.bytes + 4))

/* The MSRs QEMU puts at a full reset for this model: the list's, the
 * MTRRs, and the machine-check registers of its ten banks. */
static uint32_t put_msrs[256];
static int n_put;

/** Add `index` to QEMU's list of MSRs to put. */
static void put_msr(uint32_t index)
{
    CHECK(n_put < (int)(sizeof(put_msrs) / sizeof(put_msrs[0])), "QEMU's list of MSRs fits");
    if (n_put < (int)(sizeof(put_msrs) / sizeof(put_msrs[0]))) {
        put_msrs[n_put++] = index;
    }
}

/** QEMU's first steps: the API version and the VM. Returns the VM's
 * descriptor. */
static int startup_vm(void)
{
    int vm;

    CHECK(dev(KVM_GET_API_VERSION, 0) == 12, "KVM_GET_API_VERSION is 12");
    CHECK(dev(KVM_GET_API_VERSION, 1) == -EINVAL, "KVM_GET_API_VERSION takes no argument");
    CHECK(dev(KVM_CREATE_VM, 1) == -EINVAL, "KVM_CREATE_VM refuses a machine type x86 lacks");
    vm = dev(KVM_CREATE_VM, 0);
    CHECK(vm >= 3 && entries[vm].type == FD_TYPE_ANON, "KVM_CREATE_VM gives a descriptor");
    CHECK(entries[vm].flags == (FD_FLAG_READ | FD_FLAG_WRITE | FD_FLAG_CLOEXEC),
          "the VM's descriptor is read-write and close-on-exec");
    CHECK(((struct anon_obj *)entries[vm].priv)->name[0] == 'k', "the VM is anon_inode:kvm-vm");
    return vm;
}

/** The capabilities QEMU requires of the VM `vm`, and the counts of
 * /dev/kvm. */
static void startup_caps(int vm)
{
    uint32_t i;
    int missing = 0;

    for (i = 0; i < sizeof(required) / sizeof(required[0]); i++) {
        if (fdio(vm, KVM_CHECK_EXTENSION, (uintptr_t)required[i]) <= 0) {
            printf("kvm-hosttest: capability %ld missing\n", required[i]);
            missing++;
        }
    }
    CHECK(missing == 0, "the VM answers every capability QEMU requires");
    CHECK(dev(KVM_CHECK_EXTENSION, KVM_CAP_NR_VCPUS) == 4, "KVM_CAP_NR_VCPUS is the processors");
    CHECK(dev(KVM_CHECK_EXTENSION, KVM_CAP_MAX_VCPUS) == KVM_MAX_VCPUS, "KVM_CAP_MAX_VCPUS");
    CHECK(dev(KVM_CHECK_EXTENSION, KVM_CAP_NR_MEMSLOTS) == KVM_MEM_SLOTS, "KVM_CAP_NR_MEMSLOTS");
    CHECK(dev(KVM_CHECK_EXTENSION, KVM_CAP_MCE) == KVM_MAX_MCE_BANKS, "KVM_CAP_MCE is the banks");
    CHECK(dev(KVM_CHECK_EXTENSION, KVM_CAP_PV_MMU) == 0, "a capability not offered is 0");
    CHECK(dev(KVM_CHECK_EXTENSION, 100000) == 0, "an unknown capability is 0");
    CHECK(dev(KVM_CHECK_EXTENSION, KVM_CAP_SPLIT_IRQCHIP) == 1, "the split irqchip is offered");
}

/** How many of the MSRs `first` to `last` the list of `n` names holds. */
static int list_holds(const uint32_t *list, uint32_t n, uint32_t first, uint32_t last)
{
    uint32_t i;
    int found = 0;

    for (i = 0; i < n; i++) {
        found += list[i] >= first && list[i] <= last;
    }
    return found;
}

/** kvm_get_supported_msrs: the index list, its count first, which starts
 * QEMU's list of MSRs to put; then the feature MSRs. */
static void startup_msr_lists(void)
{
    const uint32_t *list;
    uint32_t i, nmsrs;
    int ucode, steal, debugctl;
    int32_t r;

    buf.list.nmsrs = 0;
    r = dev(KVM_GET_MSR_INDEX_LIST, (uintptr_t)&buf);
    nmsrs = buf.list.nmsrs;
    CHECK(r == -E2BIG && nmsrs > 10, "KVM_GET_MSR_INDEX_LIST gives its count with -E2BIG");
    r = dev(KVM_GET_MSR_INDEX_LIST, (uintptr_t)&buf);
    CHECK(r == 0 && buf.list.nmsrs == nmsrs, "KVM_GET_MSR_INDEX_LIST with room");
    n_put = 0;
    ucode = steal = debugctl = 0;
    for (i = 0; i < nmsrs; i++) {
        put_msr(MSR_INDICES[i]);
        ucode |= MSR_INDICES[i] == 0x8B ? 1 : MSR_INDICES[i] == 0x1FC ? 2 : 0;
        ucode |= MSR_INDICES[i] == 0xC0010015u ? 4 : 0;
        steal |= MSR_INDICES[i] == MSR_KVM_STEAL_TIME;
        debugctl |= MSR_INDICES[i] == 0x1D9;
    }
    CHECK(ucode == 7,
          "the index list names UCODE_REV, POWER_CTL and HWCR, as Linux's emulated_msrs");
    CHECK(steal, "and MSR_KVM_STEAL_TIME");
    CHECK(kvm_backend == &kvm_vmx && debugctl,
          "VMX's index list names IA32_DEBUGCTL, as Linux's msrs_to_save_base");
    CHECK(!list_holds(MSR_INDICES, nmsrs, 0x1DB, 0x1DE),
          "but not the last-branch MSRs, which the Intel host does not read");
    kvm_backend = &kvm_svm;
    list = kvm_msr_list(0, &nmsrs);
    kvm_backend = &kvm_vmx;
    debugctl = 0;
    for (i = 0; i < nmsrs; i++) {
        debugctl |= list[i] == 0x1D9;
    }
    CHECK(debugctl, "and so does SVM's, which answers it as svm_get_msr without lbrv");
    CHECK(list_holds(list, nmsrs, 0x1DB, 0x1DE) == 4,
          "with LASTBRANCHFROMIP to LASTINTTOIP beside it, as Linux's msrs_to_save_base");
    buf.list.nmsrs = 4;
    r = dev(KVM_GET_MSR_FEATURE_INDEX_LIST, (uintptr_t)&buf);
    CHECK(r == 0 && buf.list.nmsrs == 2 && MSR_INDICES[0] == 0x8B && MSR_INDICES[1] == 0x345,
          "the feature list names UCODE_REV and PERF_CAPABILITIES");
    buf.msrs.nmsrs = 2;
    MSR_ENTRIES[0].index = 0x8B;
    MSR_ENTRIES[1].index = 0x10A;
    r = dev(KVM_GET_MSRS, (uintptr_t)&buf);
    CHECK(r == 1 && MSR_ENTRIES[0].data == 0x2800000000ull,
          "KVM_GET_MSRS of /dev/kvm reads feature MSRs up to the first that is not, "
          "UCODE_REV as the host's");
}

/** Check the offered CPUID entry `e` against the model's processor. */
static void check_cpuid_entry(const struct kvm_cpuid_entry2 *e)
{
    if (e->function == 1) {
        CHECK(!(e->ecx & (1u << 5)), "leaf 1 has no VMX");
        CHECK(!(e->ecx & (1u << 27)) && (e->ecx & (1u << 31)), "no OSXSAVE, and HYPERVISOR");
        CHECK((e->ecx & (1u << 28)) && (e->ecx & (1u << 26)), "leaf 1 keeps AVX and XSAVE");
        CHECK(e->ecx & (1u << 21), "leaf 1 offers x2APIC, which the kernel's local APIC emulates");
    }
    if (e->function == 7 && e->index == 0) {
        CHECK((e->ebx & 0x20u) && (e->edx & (1u << 26)),
              "leaf 7 keeps AVX2 and the host's SPEC_CTRL, as Linux's kvm_set_cpu_caps");
        CHECK(e->flags == KVM_CPUID_FLAG_SIGNIFCANT_INDEX, "leaf 7 is indexed");
    }
    if (e->function == 0xD && e->index == 0) {
        CHECK(e->eax == 7 && e->ebx == 832, "leaf 0xD offers x87, SSE and AVX in 832 bytes");
    }
    if (e->function == 0xA) {
        CHECK(e->eax == 0x07300402u, "architectural performance counters capped at version two");
    }
    if (e->function == KVM_CPUID_SIGNATURE) {
        CHECK(e->eax == KVM_CPUID_FEATURES && e->ebx == 0x4B4D564Bu && e->ecx == 0x564B4D56u &&
                  e->edx == 0x4Du,
              "KVM's signature leaf");
    }
    if (e->function == KVM_CPUID_FEATURES) {
        CHECK(e->eax == ((1u << KVM_FEATURE_NOP_IO_DELAY) | (1u << KVM_FEATURE_STEAL_TIME) |
                         (1u << KVM_FEATURE_PV_UNHALT) | (1u << KVM_FEATURE_PV_TLB_FLUSH) |
                         (1u << KVM_FEATURE_PV_SEND_IPI) | (1u << KVM_FEATURE_POLL_CONTROL) |
                         (1u << KVM_FEATURE_PV_SCHED_YIELD)),
              "KVM's features, with no kvmclock while the TSC's rate is not known");
    }
    CHECK(e->function < 0x40000000u || e->function >= 0x80000000u ||
              e->function == KVM_CPUID_SIGNATURE || e->function == KVM_CPUID_FEATURES,
          "no paravirtual leaf but KVM's two");
}

/** KVM_GET_EMULATED_CPUID: Linux's three entries, for MOVBE and RDPID. */
static void emulated_cpuid(void)
{
    const struct kvm_cpuid_entry2 *e = CPUID_ENTRIES;

    buf.cpuid.nent = 2;
    CHECK(dev(KVM_GET_EMULATED_CPUID, (uintptr_t)&buf) == -E2BIG,
          "KVM_GET_EMULATED_CPUID into two entries is -E2BIG");
    buf.cpuid.nent = 8;
    CHECK(dev(KVM_GET_EMULATED_CPUID, (uintptr_t)&buf) == 0 && buf.cpuid.nent == 3,
          "KVM_GET_EMULATED_CPUID gives three entries");
    CHECK(e[0].function == 0 && e[0].eax == 7 && e[1].function == 1 && e[1].ecx == (1u << 22) &&
              e[1].eax == 0 && e[1].edx == 0,
          "leaf 0 with the highest leaf and leaf 1 with MOVBE alone");
    CHECK(e[2].function == 7 && e[2].index == 0 && e[2].flags == KVM_CPUID_FLAG_SIGNIFCANT_INDEX &&
              e[2].ecx == (1u << 22) && e[2].ebx == 0,
          "and leaf 7 with RDPID alone, on a processor with RDTSCP");
}

/** try_get_cpuid: KVM_GET_SUPPORTED_CPUID from one entry, doubling while
 * it does not fit. Returns the entries, left in `buf`. */
static uint32_t startup_cpuid(void)
{
    uint32_t i, max, n;
    int32_t r;

    emulated_cpuid();
    for (max = 1;; max *= 2) {
        buf.cpuid.nent = max;
        r = dev(KVM_GET_SUPPORTED_CPUID, (uintptr_t)&buf);
        if ((r == 0 && buf.cpuid.nent < max) || max > 256) {
            break;
        }
        CHECK(r == -E2BIG, "a small buffer is -E2BIG");
    }
    n = buf.cpuid.nent;
    CHECK(r == 0 && n > 10 && n < 64, "KVM_GET_SUPPORTED_CPUID gives the leaves");
    for (i = 0; i < n; i++) {
        check_cpuid_entry(&CPUID_ENTRIES[i]);
    }
    return n;
}

/** The VM's addresses and memory slot, before its vCPU. */
static void startup_addrs(int vm)
{
    struct kvm_userspace_memory_region region = {0, 0, 0, 0x8000000, 0x7F0000000000ull};
    struct kvm_userspace_memory_region2 region2;
    uint64_t addr = 0xFEFFC000ull;

    CHECK(fdio(vm, KVM_SET_IDENTITY_MAP_ADDR, (uintptr_t)&addr) == 0,
          "KVM_SET_IDENTITY_MAP_ADDR before the vCPUs");
    CHECK(fdio(vm, KVM_SET_TSS_ADDR, 0xFEFFD000u) == 0, "KVM_SET_TSS_ADDR");
    CHECK(fdio(vm, KVM_SET_TSS_ADDR, 0xFFFFE000u) == -EINVAL, "a TSS past 4 GiB less 3 pages");
    CHECK(fdio(vm, KVM_SET_USER_MEMORY_REGION, (uintptr_t)&region) == 0 && slots_set == 1,
          "the memory slot goes to slots.c");
    memset(&region2, 0xFF, sizeof(region2));
    region2.region = region;
    CHECK(fdio(vm, KVM_SET_USER_MEMORY_REGION2, (uintptr_t)&region2) == 0 && slots_set == 2,
          "KVM_SET_USER_MEMORY_REGION2's slot goes there too, its padding unread");
}

/** Check the mapping of the vCPU on `vcpu`: its two pages, the VM's ring,
 * the page past them, and the VM reference the mapping holds. */
static void check_mapping(int vcpu)
{
    struct kvm_vcpu *v = (struct kvm_vcpu *)entries[vcpu].priv;
    uintptr_t phys = 0;

    CHECK(map_ops->page(map_obj, 0, &phys) == 0 && phys == (uintptr_t)v->run,
          "page 0 of the mapping is the run page");
    CHECK(map_ops->page(map_obj, 1, &phys) == 0 && phys == (uintptr_t)v->pio,
          "page 1 is the port I/O page");
    CHECK(map_ops->page(map_obj, 2, &phys) == 0 && phys == (uintptr_t)v->vm->ring,
          "page 2 is the VM's ring of coalesced writes");
    CHECK(map_ops->page(map_obj, 3, &phys) == -1, "page 3 is past the mapping: SIGBUS");
    CHECK(v->vm->refs == 3, "the mapping holds the VM, beside the two descriptors");
}

/** Check the private mapping `f` of the vCPU on `vcpu`: a copy of each
 * page at the fault, none past them, and the VM's reference let go with the
 * last region. */
static void check_private(int vcpu, struct vm_file *f)
{
    static uint8_t frame[4096] __attribute__((aligned(4096)));
    struct kvm_vcpu *v = (struct kvm_vcpu *)entries[vcpu].priv;
    uint32_t refs = v->vm->refs;

    v->run->exit_reason = 0x5A;
    v->pio[7] = 0xA5;
    CHECK(f->fill(f, 0, (uintptr_t)frame) == 0 && ((struct kvm_run *)frame)->exit_reason == 0x5A,
          "a private mapping's first page is a copy of the run page");
    CHECK(f->fill(f, 1, (uintptr_t)frame) == 0 && frame[7] == 0xA5,
          "its second page is a copy of the port I/O page");
    v->vm->ring->last = 0x1234;
    CHECK(f->fill(f, 2, (uintptr_t)frame) == 0 &&
              ((struct kvm_coalesced_mmio_ring *)frame)->last == 0x1234,
          "its third page is a copy of the ring of coalesced writes");
    v->vm->ring->last = 0;
    CHECK(f->fill(f, 3, (uintptr_t)frame) == -1, "and a page past them raises SIGBUS");
    v->run->exit_reason = 0;
    v->pio[7] = 0;
    CHECK(refs == 3, "the private mapping holds the VM");
    f->path = kmalloc(8);
    f->release(f);
    CHECK(v->vm->refs == 2, "its release lets the VM go");
}

/** KVM_CREATE_VCPU on the VM `vm`, and the vCPU's mapping. Returns the
 * vCPU's descriptor. */
static int startup_vcpu(int vm)
{
    uint64_t addr = 0xFEFFC000ull;
    vma_t vma;
    int vcpu;

    vcpu = fdio(vm, KVM_CREATE_VCPU, 0);
    CHECK(vcpu > vm && entries[vcpu].ops == &vcpu_ops, "KVM_CREATE_VCPU gives a descriptor");
    CHECK(fdio(vm, KVM_CREATE_VCPU, 0) == -EEXIST, "a second vCPU of the same id");
    CHECK(fdio(vm, KVM_CREATE_VCPU, KVM_MAX_VCPU_IDS) == -EINVAL, "an id past the bound");
    CHECK(fdio(vm, KVM_SET_IDENTITY_MAP_ADDR, (uintptr_t)&addr) == -EBUSY,
          "KVM_SET_IDENTITY_MAP_ADDR after a vCPU");
    CHECK(dev(KVM_GET_VCPU_MMAP_SIZE, 0) == 12288,
          "the mapping is the run page, the I/O page and the ring of coalesced writes");
    k_memset(&vma, 0, sizeof(vma));
    vma.end = 12288;
    vma.type = VM_FILE;
    CHECK(entries[vcpu].ops->mmap(&entries[vcpu], &vma) == 0 && vma.file, "a private mapping");
    check_private(vcpu, vma.file);
    vma.file = NULL;
    vma.type = VM_SHARED;
    CHECK(entries[vcpu].ops->mmap(&entries[vcpu], &vma) == 0 && map_cache == 0,
          "a shared mapping, cached");
    check_mapping(vcpu);
    return vcpu;
}

/** kvm_arch_init_vcpu's CPUID and machine checks, then the registers got
 * and put, on the vCPU `vcpu`; the supported CPUID's `n` entries are in
 * `buf`. */
static void startup_regs(int vcpu, uint32_t n)
{
    static uint8_t xsave[4096] __attribute__((aligned(64)));
    struct kvm_regs regs;
    struct kvm_sregs sregs;
    struct kvm_xcrs xcrs;
    uint64_t mce_cap = 0;

    buf.cpuid.nent = n;
    CHECK(fdio(vcpu, KVM_SET_CPUID2, (uintptr_t)&buf) == 0, "KVM_SET_CPUID2");
    CHECK(dev(KVM_X86_GET_MCE_CAP_SUPPORTED, (uintptr_t)&mce_cap) == 0, "the MCE capabilities");
    CHECK(mce_cap == (MCG_CTL_P | MCG_SER_P), "MCG_CTL_P and MCG_SER_P are offered");
    mce_cap = MCG_CTL_P | MCG_SER_P | 10;
    CHECK(fdio(vcpu, KVM_X86_SETUP_MCE, (uintptr_t)&mce_cap) == 0, "KVM_X86_SETUP_MCE, 10 banks");
    CHECK(fdio(vcpu, KVM_GET_REGS, (uintptr_t)&regs) == 0, "KVM_GET_REGS");
    CHECK(regs.rip == 0xFFF0 && regs.rflags == 2, "a new vCPU is at the reset vector");
    CHECK(fdio(vcpu, KVM_GET_SREGS, (uintptr_t)&sregs) == 0, "KVM_GET_SREGS");
    CHECK(sregs.cs.base == 0xFFFF0000ull && sregs.cr0 == 0x60000010ull,
          "its code segment and CR0 at reset");
    CHECK(sregs.apic_base & 0x100u, "the boot vCPU's APIC base");
    CHECK(fdio(vcpu, KVM_GET_XSAVE, (uintptr_t)xsave) == 0, "KVM_GET_XSAVE");
    CHECK(xsave[0] == 0x7F && xsave[25] == 0x1F, "the image has FCW 0x37F and MXCSR 0x1F80");
    CHECK(fdio(vcpu, KVM_GET_XCRS, (uintptr_t)&xcrs) == 0, "KVM_GET_XCRS");
    CHECK(xcrs.nr_xcrs == 1 && xcrs.xcrs[0].value == 1, "XCR0 is x87 at reset");
    regs.rax = 0x1234;
    CHECK(fdio(vcpu, KVM_SET_REGS, (uintptr_t)&regs) == 0, "KVM_SET_REGS");
    CHECK(fdio(vcpu, KVM_SET_XSAVE, (uintptr_t)xsave) == 0, "KVM_SET_XSAVE");
    xcrs.xcrs[0].value = 7;
    CHECK(fdio(vcpu, KVM_SET_XCRS, (uintptr_t)&xcrs) == 0, "KVM_SET_XCRS of x87, SSE and AVX");
    CHECK(fdio(vcpu, KVM_SET_SREGS, (uintptr_t)&sregs) == 0, "KVM_SET_SREGS");
}

/** The value QEMU puts at reset in the MSR `index`: 0 in Hyper-V's, whose
 * state QEMU zeroes. */
static uint64_t reset_value(uint32_t index)
{
    if ((index >> 12) == 0x40000u) {
        return 0;
    }
    if (index == 0x277) {
        return 0x0007040600070406ull;
    }
    if (index == 0x17B || (index >= 0x400 && index % 4 == 0)) {
        return ~0ull;
    }
    return index == 0x2FF ? 0xC06 : 0;
}

/** kvm_put_msrs at a full reset on the vCPU `vcpu`: the index list's MSRs,
 * the MTRRs and the ten banks' registers, then read back. */
static void startup_msrs(int vcpu)
{
    uint32_t i;
    int32_t r;

    put_msr(0x2FF);
    put_msr(0x250);
    put_msr(0x258);
    put_msr(0x259);
    for (i = 0; i < 8; i++) {
        put_msr(0x268 + i);
    }
    for (i = 0; i < 16; i++) {
        put_msr(0x200 + i);
    }
    for (i = 0; i < 40; i++) {
        put_msr(0x400 + i);
    }
    buf.msrs.nmsrs = (uint32_t)n_put;
    for (i = 0; i < (uint32_t)n_put; i++) {
        MSR_ENTRIES[i].index = put_msrs[i];
        MSR_ENTRIES[i].reserved = 0;
        MSR_ENTRIES[i].data = reset_value(put_msrs[i]);
    }
    r = fdio(vcpu, KVM_SET_MSRS, (uintptr_t)&buf);
    CHECK(r == n_put, "KVM_SET_MSRS takes every MSR QEMU puts, which QEMU asserts");
    if (r != n_put && r >= 0) {
        printf("kvm-hosttest: KVM_SET_MSRS stopped at 0x%x\n", put_msrs[r]);
    }
    CHECK(fdio(vcpu, KVM_GET_MSRS, (uintptr_t)&buf) == n_put, "KVM_GET_MSRS reads them back");
}

/** The rest of the put on the vCPU `vcpu`, events, MP state and debug
 * registers, and KVM_RUN, which the refusing backend fails. */
static void startup_run(int vcpu)
{
    struct kvm_vcpu *v = (struct kvm_vcpu *)entries[vcpu].priv;
    struct kvm_vcpu_events ev;
    struct kvm_debugregs dbg;
    struct kvm_mp_state mp = {KVM_MP_STATE_RUNNABLE};
    int32_t r;

    CHECK(fdio(vcpu, KVM_GET_VCPU_EVENTS, (uintptr_t)&ev) == 0, "KVM_GET_VCPU_EVENTS");
    CHECK(ev.flags == (KVM_VCPUEVENT_VALID_NMI_PENDING | KVM_VCPUEVENT_VALID_SHADOW |
                       KVM_VCPUEVENT_VALID_SMM),
          "the events have Linux's flags");
    CHECK(fdio(vcpu, KVM_SET_VCPU_EVENTS, (uintptr_t)&ev) == 0, "KVM_SET_VCPU_EVENTS");
    CHECK(fdio(vcpu, KVM_SET_MP_STATE, (uintptr_t)&mp) == 0, "KVM_SET_MP_STATE runnable");
    CHECK(fdio(vcpu, KVM_GET_DEBUGREGS, (uintptr_t)&dbg) == 0, "KVM_GET_DEBUGREGS");
    CHECK(dbg.dr6 == 0xFFFF0FF0ull && dbg.dr7 == 0x400, "the debug registers at reset");
    CHECK(fdio(vcpu, KVM_SET_DEBUGREGS, (uintptr_t)&dbg) == 0, "KVM_SET_DEBUGREGS");
    entries_made = 0;
    r = fdio(vcpu, KVM_RUN, 0);
    CHECK(r == -EOPNOTSUPP && entries_made == 1,
          "KVM_RUN reaches the backend, whose refusal QEMU reports");
    CHECK(bkl_depth == 0 && irq_on, "KVM_RUN returns with interrupts on and no big lock held");
    /* The entry took the big lock for its tracer, seccomp, a held
     * descriptor or USB work: the run lets it go and takes it back. */
    bkl_depth = 2;
    entries_made = 0;
    r = fdio(vcpu, KVM_RUN, 0);
    CHECK(r == -EOPNOTSUPP && entries_made == 1 && bkl_depth == 2 && irq_on,
          "KVM_RUN entered with the big lock held runs without it and returns with its depth");
    /* Another process that shares the VM's memory and descriptors holds
     * the vCPU; the waiter's SIGKILL ends its wait, as Linux's
     * mutex_lock_killable does, and the entry's hold comes back. */
    mutex_lock(&v->mutex);
    entries_made = 0;
    r = fdio(vcpu, KVM_RUN, 0);
    CHECK(r == -EINTR && entries_made == 0 && bkl_depth == 2 && irq_on && mutex_held(&v->mutex),
          "a KVM_RUN killed in its wait for a held vCPU returns EINTR with the entry's depth");
    bkl_depth = 0;
    CHECK(fdio(vcpu, KVM_GET_DEBUGREGS, (uintptr_t)&dbg) == -EINTR && bkl_depth == 0,
          "a state ioctl killed in its wait for a held vCPU returns EINTR");
    mutex_unlock(&v->mutex);
}

/**
 * QEMU's start-up, in the order of its sources: /dev/kvm, the VM, its
 * capabilities, the MSR lists, its addresses, the supported CPUID, a
 * memory slot, the vCPU, its mapping, its CPUID and machine checks, the
 * registers got and put, and KVM_RUN. Returns the vCPU's descriptor;
 * `*vmfd` gets the VM's.
 */
static int qemu_startup(int *vmfd)
{
    uint32_t n;
    int vm, vcpu;

    vm = startup_vm();
    *vmfd = vm;
    startup_caps(vm);
    startup_msr_lists();
    n = startup_cpuid();
    startup_addrs(vm);
    vcpu = startup_vcpu(vm);
    startup_regs(vcpu, n);
    startup_msrs(vcpu);
    startup_run(vcpu);
    return vcpu;
}

/** The run page of the vCPU on descriptor `fd`. */
static struct kvm_run *run_of(int fd)
{
    return ((struct kvm_vcpu *)entries[fd].priv)->run;
}

/** Append an exit of `kind` to the script; returns it, zeroed but for its
 * kind. */
static struct kvm_exit *scripted(enum kvm_exit_kind kind)
{
    struct kvm_exit *x = &script[script_len];

    script_hw[script_len++] = 0;
    k_memset(x, 0, sizeof(*x));
    x->kind = kind;
    return x;
}

/** Append a port exit to the script: `dir`, `size` bytes, `port`,
 * `count` times. */
static struct kvm_exit *scripted_io(uint8_t dir, uint8_t size, uint16_t port, uint32_t count)
{
    struct kvm_exit *x = scripted(KVM_X_IO);

    x->io.direction = dir;
    x->io.size = size;
    x->io.port = port;
    x->io.count = count;
    return x;
}

/** The script of exit_rows: one exit of each kind. */
static void make_script(void)
{
    struct kvm_exit *x;

    refusing = 0;
    script_len = 0;
    script_at = 0;
    scripted(KVM_X_AGAIN); /* handled in the kernel, then an OUT */
    x = scripted_io(KVM_EXIT_IO_OUT, 1, 0x3F8, 3);
    k_memcpy(x->data, "abc", 3);
    scripted_io(KVM_EXIT_IO_IN, 2, 0x71, 2);
    x = scripted(KVM_X_MMIO);
    x->mmio.phys_addr = 0xFED00000ull;
    x->mmio.len = 4;
    scripted_io(KVM_EXIT_IO_OUT, 4, BUS_PORT, 1); /* the bus takes it, then a HLT */
    scripted(KVM_X_HLT);
    scripted(KVM_X_SHUTDOWN);
    x = scripted(KVM_X_INTERNAL);
    x->internal.suberror = KVM_INTERNAL_ERROR_EMULATION;
    x->internal.ndata = 40;
    x = scripted(KVM_X_FAIL_ENTRY);
    x->hw_reason = 0x21;
    scripted(KVM_X_IRQ_WINDOW);
}

/** The port and MMIO exits of the script on the vCPU `fd`, their answers
 * and immediate_exit, under a signal mask of the vCPU's. */
static void io_rows(int fd)
{
    struct kvm_vcpu *v = (struct kvm_vcpu *)entries[fd].priv;
    struct kvm_run *run = run_of(fd);
    uint64_t blocked = 0x3;

    v->has_sigmask = 1;
    v->sigmask = 0;
    caller.sig_blocked = blocked;
    host_cpu = 1;
    CHECK(fdio(fd, KVM_RUN, 0) == 0 && run->exit_reason == KVM_EXIT_IO,
          "an exit the kernel takes enters again; an OUT goes to the caller");
    CHECK(run->u.io.direction == KVM_EXIT_IO_OUT && run->u.io.port == 0x3F8 &&
              run->u.io.count == 3 && run->u.io.data_offset == 4096,
          "the OUT's port, count and data offset");
    CHECK(bytes_equal((uint8_t *)run + 4096, "abc", 3), "the OUT's bytes are in the I/O page");
    CHECK(caller.sig_blocked == blocked, "the caller's mask is back after the run");
    CHECK(v->cpu == 1 && run->if_flag == 1 && run->ready_for_interrupt_injection == 1,
          "the run page has the flags of the exit");
    CHECK(fdio(fd, KVM_RUN, 0) == 0 && run->exit_reason == KVM_EXIT_IO &&
              run->u.io.direction == KVM_EXIT_IO_IN && run->u.io.size == 2,
          "an IN goes to the caller");
    k_memcpy((uint8_t *)run + 4096, "\x11\x22\x33\x44", 4);
    completes = 0;
    CHECK(fdio(fd, KVM_RUN, 0) == 0 && run->exit_reason == KVM_EXIT_MMIO &&
              run->u.mmio.phys_addr == 0xFED00000ull && !run->u.mmio.is_write,
          "an MMIO read goes to the caller");
    CHECK(completes == 1 && (completed & 0xFFFFFFFFu) == 0x44332211u,
          "the IN's answer reaches the backend at the next KVM_RUN");
    k_memcpy(run->u.mmio.data, "\x55\x66\x77\x88", 4);
    run->immediate_exit = 1;
    entries_made = 0;
    CHECK(fdio(fd, KVM_RUN, 0) == -EINTR && entries_made == 0,
          "immediate_exit ends KVM_RUN before the entry");
    CHECK(completes == 2 && (completed & 0xFFFFFFFFu) == 0x88776655u,
          "the MMIO answer reaches the backend even under immediate_exit");
    run->immediate_exit = 0;
}

/** The other exits of the script on the vCPU `fd`. */
static void other_exit_rows(int fd)
{
    struct kvm_run *run = run_of(fd);

    CHECK(fdio(fd, KVM_RUN, 0) == 0 && run->exit_reason == KVM_EXIT_HLT,
          "a write the bus takes enters again; HLT goes to the caller with no local APIC");
    CHECK(fdio(fd, KVM_RUN, 0) == 0 && run->exit_reason == KVM_EXIT_SHUTDOWN, "a triple fault");
    CHECK(fdio(fd, KVM_RUN, 0) == 0 && run->exit_reason == KVM_EXIT_INTERNAL_ERROR,
          "an internal error");
    CHECK(run->u.internal.suberror == KVM_INTERNAL_ERROR_EMULATION && run->u.internal.ndata == 16,
          "its suberror, and its data cut to 16 words");
    CHECK(fdio(fd, KVM_RUN, 0) == 0 && run->exit_reason == KVM_EXIT_FAIL_ENTRY, "a failed entry");
    CHECK(run->u.fail_entry.hardware_entry_failure_reason == 0x21 && run->u.fail_entry.cpu == 1,
          "its reason and processor");
    CHECK(fdio(fd, KVM_RUN, 0) == 0 && run->exit_reason == KVM_EXIT_IRQ_WINDOW_OPEN,
          "the interrupt window");
}

/** A signal and a kick end KVM_RUN on the vCPU `fd` before the entry. */
static void interrupt_rows(int fd)
{
    struct kvm_vcpu *v = (struct kvm_vcpu *)entries[fd].priv;
    struct kvm_run *run = run_of(fd);

    sig_pending = 1;
    caller.sig_woken = 1;
    entries_made = 0;
    CHECK(fdio(fd, KVM_RUN, 0) == -EINTR && run->exit_reason == KVM_EXIT_INTR,
          "a signal ends KVM_RUN with KVM_EXIT_INTR");
    CHECK(entries_made == 0, "the signal is seen before the entry");
    sig_pending = 0;
    caller.sig_woken = 0;
    CHECK(fdio(fd, KVM_RUN, 1) == -EINVAL, "KVM_RUN takes no argument");

    v->mode = KVM_MODE_IN_GUEST;
    v->cpu = 1;
    host_cpu = 0;
    kicked = -1;
    kvm_vcpu_kick(v, KVM_REQ_EXIT);
    CHECK(kicked == 1, "a kick interrupts the processor the vCPU is in the guest on");
    v->mode = KVM_MODE_OUTSIDE;
    host_cpu = 1;
    entries_made = 0;
    CHECK(fdio(fd, KVM_RUN, 0) == -EINTR && run->exit_reason == KVM_EXIT_INTR,
          "the kick's request ends the next KVM_RUN");
    CHECK(entries_made == 0, "the request is seen before the entry");
    v->has_sigmask = 0;
    host_cpu = 0;
}

/** Each exit kind through the run page of the vCPU `fd`. */
static void exit_rows(int fd)
{
    make_script();
    io_rows(fd);
    other_exit_rows(fd);
    interrupt_rows(fd);
}

/** KVM_SET_SREGS's refusals and its interrupt bitmap, and KVM_INTERRUPT,
 * on the vCPU `fd`. */
static void sregs_rows(int fd)
{
    struct kvm_vcpu *v = (struct kvm_vcpu *)entries[fd].priv;
    struct kvm_interrupt irq = {0x30};
    struct kvm_sregs s;

    fdio(fd, KVM_GET_SREGS, (uintptr_t)&s);
    s.efer = 0x400; /* LMA without LME and paging */
    CHECK(fdio(fd, KVM_SET_SREGS, (uintptr_t)&s) == -EINVAL, "LMA without long mode");
    s.efer = 0;
    s.cr0 = 0x80000000ull; /* PG without PE */
    CHECK(fdio(fd, KVM_SET_SREGS, (uintptr_t)&s) == -EINVAL, "PG without PE");
    s.cr0 = 0x60000010ull;
    s.cr4 = 1ull << 13; /* VMXE */
    CHECK(fdio(fd, KVM_SET_SREGS, (uintptr_t)&s) == -EINVAL, "CR4.VMXE, with no nesting");
    s.cr4 = 1ull << 18; /* OSXSAVE, which the guest's CPUID allows */
    s.interrupt_bitmap[0] = 1ull << 0x21;
    CHECK(fdio(fd, KVM_SET_SREGS, (uintptr_t)&s) == 0, "KVM_SET_SREGS with an interrupt");
    CHECK(v->events.interrupt.injected && v->events.interrupt.nr == 0x21,
          "the bitmap's interrupt is queued");
    fdio(fd, KVM_GET_SREGS, (uintptr_t)&s);
    CHECK(s.interrupt_bitmap[0] == 1ull << 0x21, "and KVM_GET_SREGS reports it");
    CHECK(fdio(fd, KVM_INTERRUPT, (uintptr_t)&irq) == -ENXIO, "KVM_INTERRUPT while one waits");
    v->events.interrupt.injected = 0;
    CHECK(fdio(fd, KVM_INTERRUPT, (uintptr_t)&irq) == 0 && v->events.interrupt.nr == 0x30,
          "KVM_INTERRUPT queues the vector");
    irq.irq = 256;
    CHECK(fdio(fd, KVM_INTERRUPT, (uintptr_t)&irq) == -EINVAL, "a vector past 255");
}

/** The refusals of KVM_SET_XCRS, KVM_SET_XSAVE and KVM_SET_DEBUGREGS on
 * the vCPU `fd`. */
static void xstate_rows(int fd)
{
    static uint8_t xsave[4096];
    struct kvm_xcrs xcrs;
    struct kvm_debugregs dbg;
    uint64_t bv;

    fdio(fd, KVM_GET_XCRS, (uintptr_t)&xcrs);
    xcrs.xcrs[0].value = 0x4 | 0x1; /* AVX without SSE */
    CHECK(fdio(fd, KVM_SET_XCRS, (uintptr_t)&xcrs) == -EINVAL, "XCR0 with AVX and no SSE");
    xcrs.xcrs[0].value = 0xE7; /* AVX-512, which the guest's CPUID does not allow */
    CHECK(fdio(fd, KVM_SET_XCRS, (uintptr_t)&xcrs) == -EINVAL, "XCR0 past the guest's CPUID");
    xcrs.flags = 1;
    CHECK(fdio(fd, KVM_SET_XCRS, (uintptr_t)&xcrs) == -EINVAL, "KVM_SET_XCRS with flags");

    fdio(fd, KVM_GET_XSAVE, (uintptr_t)xsave);
    bv = 0x20;
    k_memcpy(xsave + 512, &bv, 8);
    CHECK(fdio(fd, KVM_SET_XSAVE, (uintptr_t)xsave) == -EINVAL, "an image with AVX-512 marked");
    bv = 0x3;
    k_memcpy(xsave + 512, &bv, 8);
    xsave[26] = 0x01; /* MXCSR bit 16 */
    CHECK(fdio(fd, KVM_SET_XSAVE, (uintptr_t)xsave) == -EINVAL, "MXCSR with a reserved bit");
    xsave[26] = 0;
    xsave[520] = 1; /* XCOMP_BV */
    CHECK(fdio(fd, KVM_SET_XSAVE, (uintptr_t)xsave) == -EINVAL, "a compacted image");
    xsave[520] = 0;
    CHECK(fdio(fd, KVM_SET_XSAVE, (uintptr_t)xsave) == 0, "the image put right");

    fdio(fd, KVM_GET_DEBUGREGS, (uintptr_t)&dbg);
    dbg.dr6 = 1ull << 32;
    CHECK(fdio(fd, KVM_SET_DEBUGREGS, (uintptr_t)&dbg) == -EINVAL, "DR6 above bit 31");
    dbg.dr6 = 0xFFFF0FF0ull;
    dbg.flags = 1;
    CHECK(fdio(fd, KVM_SET_DEBUGREGS, (uintptr_t)&dbg) == -EINVAL, "debug registers with flags");
}

/** The refusals of KVM_SET_VCPU_EVENTS and KVM_SET_MP_STATE, and the NMI
 * queue, on the vCPU `fd`. */
static void events_rows(int fd)
{
    struct kvm_vcpu *v = (struct kvm_vcpu *)entries[fd].priv;
    struct kvm_mp_state mp = {KVM_MP_STATE_HALTED};
    struct kvm_vcpu_events ev;

    fdio(fd, KVM_GET_VCPU_EVENTS, (uintptr_t)&ev);
    ev.exception.injected = 1;
    ev.exception.nr = 2;
    CHECK(fdio(fd, KVM_SET_VCPU_EVENTS, (uintptr_t)&ev) == -EINVAL, "an exception of the NMI");
    ev.exception.nr = 14;
    ev.flags = KVM_VCPUEVENT_VALID_PAYLOAD;
    CHECK(fdio(fd, KVM_SET_VCPU_EVENTS, (uintptr_t)&ev) == -EINVAL, "a payload not enabled");
    ev.flags = KVM_VCPUEVENT_VALID_SMM;
    ev.smi.smm = 1;
    CHECK(fdio(fd, KVM_SET_VCPU_EVENTS, (uintptr_t)&ev) == -EINVAL, "SMM, which is not offered");
    ev.smi.smm = 0;
    ev.flags = KVM_VCPUEVENT_VALID_NMI_PENDING;
    ev.nmi.pending = 1;
    CHECK(fdio(fd, KVM_SET_VCPU_EVENTS, (uintptr_t)&ev) == 0, "the events put right");
    CHECK(v->nmi_queued == 1 && v->events.exception.nr == 14, "the NMI and the page fault wait");
    fdio(fd, KVM_NMI, 0);
    fdio(fd, KVM_NMI, 0);
    CHECK(v->nmi_queued == 2, "NMIs queue up to two, as Linux's");
    CHECK(fdio(fd, KVM_SET_MP_STATE, (uintptr_t)&mp) == -EINVAL,
          "HALTED with no local APIC in the kernel");
}

/** KVM_GET_CPUID2, KVM_SET_CPUID2 after a run, and the MSR writes refused
 * and the TSC, on the vCPU `fd`. */
static void table_rows(int fd)
{
    struct kvm_vcpu *v = (struct kvm_vcpu *)entries[fd].priv;

    buf.cpuid.nent = 0;
    CHECK(fdio(fd, KVM_GET_CPUID2, (uintptr_t)&buf) == -E2BIG, "KVM_GET_CPUID2 with no room");
    buf.cpuid.nent = 256;
    CHECK(fdio(fd, KVM_GET_CPUID2, (uintptr_t)&buf) == 0 && buf.cpuid.nent == v->cpuid_nent,
          "KVM_GET_CPUID2");
    CHECK(fdio(fd, KVM_SET_CPUID2, (uintptr_t)&buf) == 0, "the same CPUID after a run");
    CPUID_ENTRIES[0].eax ^= 1;
    CHECK(fdio(fd, KVM_SET_CPUID2, (uintptr_t)&buf) == -EINVAL, "another CPUID after a run");
    buf.cpuid.nent = 257;
    CHECK(fdio(fd, KVM_SET_CPUID2, (uintptr_t)&buf) == -E2BIG, "past 256 entries");

    buf.msrs.nmsrs = 3;
    MSR_ENTRIES[0].index = 0x174;
    MSR_ENTRIES[0].data = 8;
    MSR_ENTRIES[1].index = 0x277;
    MSR_ENTRIES[1].data = 0x02; /* a reserved type */
    MSR_ENTRIES[2].index = 0x175;
    MSR_ENTRIES[2].data = 0;
    CHECK(fdio(fd, KVM_SET_MSRS, (uintptr_t)&buf) == 1, "KVM_SET_MSRS stops at a refused value");
    MSR_ENTRIES[1].index = 0x10;
    MSR_ENTRIES[1].data = 5000;
    tsc = 2000000;
    CHECK(fdio(fd, KVM_SET_MSRS, (uintptr_t)&buf) == 3, "the TSC set");
    tsc += 100;
    fdio(fd, KVM_GET_MSRS, (uintptr_t)&buf);
    CHECK(MSR_ENTRIES[1].data == 5100, "the TSC runs from what was set");
    buf.msrs.nmsrs = 256;
    CHECK(fdio(fd, KVM_SET_MSRS, (uintptr_t)&buf) == -E2BIG, "256 MSRs at once");
    buf.msrs.nmsrs = 2;
    MSR_ENTRIES[0].index = 0xC0010010u;
    MSR_ENTRIES[1].index = 0xC001001Fu;
    CHECK(fdio(fd, KVM_GET_MSRS, (uintptr_t)&buf) == 2 && MSR_ENTRIES[0].data == 0 &&
              MSR_ENTRIES[1].data == 0,
          "AMD's SYSCFG and NB_CFG read zero as on Linux");
    buf.msrs.nmsrs = 1;
    MSR_ENTRIES[0].index = 0xC001001Fu;
    MSR_ENTRIES[0].data = 1ull << 46;
    CHECK(fdio(fd, KVM_SET_MSRS, (uintptr_t)&buf) == 1 &&
              fdio(fd, KVM_GET_MSRS, (uintptr_t)&buf) == 1 && MSR_ENTRIES[0].data == 0,
          "AMD's NB_CFG write is ignored as on Linux");
    MSR_ENTRIES[0].index = 0xC0010010u;
    CHECK(fdio(fd, KVM_SET_MSRS, (uintptr_t)&buf) == 0, "AMD's SYSCFG write is refused");
}

/** A machine check that ends the guest, and the ioctls a vCPU refuses, on
 * the vCPU `fd`. */
static void mce_rows(int fd)
{
    static uint8_t lapic[1024];
    struct kvm_x86_mce mce;
    struct kvm_enable_cap cap;

    k_memset(&mce, 0, sizeof(mce));
    mce.bank = 10;
    mce.status = 1ull << 63;
    CHECK(fdio(fd, KVM_X86_SET_MCE, (uintptr_t)&mce) == -EINVAL, "a bank past the guest's");
    mce.bank = 2;
    mce.status = (1ull << 63) | (1ull << 61); /* uncorrected, CR4.MCE clear */
    CHECK(fdio(fd, KVM_X86_SET_MCE, (uintptr_t)&mce) == 0, "an uncorrected machine check");
    refusing = 0;
    script_len = 1;
    script_at = 0;
    entries_made = 0;
    CHECK(fdio(fd, KVM_RUN, 0) == 0 && run_of(fd)->exit_reason == KVM_EXIT_SHUTDOWN,
          "with CR4.MCE clear it ends the guest in a triple fault");
    CHECK(entries_made == 0, "before the entry");

    k_memset(&cap, 0, sizeof(cap));
    cap.cap = KVM_CAP_SPLIT_IRQCHIP;
    CHECK(fdio(fd, KVM_ENABLE_CAP, (uintptr_t)&cap) == -EINVAL, "no vCPU capability to enable");
    CHECK(fdio(fd, KVM_TRANSLATE, (uintptr_t)&cap) == -EINVAL, "an x86 vCPU ioctl not offered");
    CHECK(fdio(fd, 0x1234, 0) == -EINVAL, "an ioctl not of KVM's type");
    CHECK(fdio(fd, KVM_GET_LAPIC, (uintptr_t)lapic) == -EINVAL,
          "KVM_GET_LAPIC with no local APIC in the kernel");
}

/** The refusals of the state ioctls Linux makes, on the vCPU `fd`. */
static void state_rows(int fd)
{
    sregs_rows(fd);
    xstate_rows(fd);
    events_rows(fd);
    table_rows(fd);
    mce_rows(fd);
}

/** The clock, the other address space and the release of everything. */
static void vm_rows(int vm, int vcpu)
{
    struct kvm_clock_data clk;
    struct kvm_enable_cap cap;
    int32_t before;

    k_memset(&clk, 0, sizeof(clk));
    CHECK(fdio(vm, KVM_GET_CLOCK, (uintptr_t)&clk) == 0 && clk.clock == 0 && clk.flags == 0,
          "the kvmclock starts at 0 with the VM, without the master clock");
    mono_ns += 1000;
    clk.clock = 1000000;
    clk.flags = KVM_CLOCK_REALTIME;
    clk.realtime = real_ns - 500;
    CHECK(fdio(vm, KVM_SET_CLOCK, (uintptr_t)&clk) == 0, "KVM_SET_CLOCK");
    fdio(vm, KVM_GET_CLOCK, (uintptr_t)&clk);
    CHECK(clk.clock == 1000500, "the realtime passed since the caller's is added");
    clk.flags = 1;
    CHECK(fdio(vm, KVM_SET_CLOCK, (uintptr_t)&clk) == -EINVAL, "a clock flag Linux lacks");

    k_memset(&cap, 0, sizeof(cap));
    cap.cap = KVM_CAP_SPLIT_IRQCHIP;
    CHECK(fdio(vm, KVM_ENABLE_CAP, (uintptr_t)&cap) == -EEXIST, "the split irqchip after a vCPU");
    cap.flags = 1;
    CHECK(fdio(vm, KVM_ENABLE_CAP, (uintptr_t)&cap) == -EINVAL, "KVM_ENABLE_CAP with flags");
    CHECK(fdio(vm, 0xAEFF, 0) == -ENOTTY, "an unknown VM ioctl");
    CHECK(fdio(vm, KVM_SET_BOOT_CPU_ID, 1) == -EBUSY, "the boot vCPU after a vCPU");

    caller.mm = &child_mm;
    CHECK(fdio(vm, KVM_CHECK_EXTENSION, KVM_CAP_USER_MEMORY) == -EIO,
          "a VM ioctl from another address space");
    CHECK(fdio(vcpu, KVM_GET_REGS, (uintptr_t)&clk) == -EIO,
          "a vCPU ioctl from another address space");
    caller.mm = &caller_mm;

    before = live;
    fdclose(vcpu);
    fdclose(vm);
    CHECK(live == before && holds == 1, "the mapping keeps the VM after both descriptors close");
    map_ops->put(map_obj);
    CHECK(live == 0 && holds == 0, "the mapping's put frees the VM and every allocation");
}

#define MSR_IA32_TSC 0x010u

/** KVM_SET_MSRS of one MSR, `index`, to `value`, on the vCPU `fd`;
 * returns the count set. */
static int32_t set_msr(int fd, uint32_t index, uint64_t value)
{
    buf.msrs.nmsrs = 1;
    MSR_ENTRIES[0].index = index;
    MSR_ENTRIES[0].reserved = 0;
    MSR_ENTRIES[0].data = value;
    return fdio(fd, KVM_SET_MSRS, (uintptr_t)&buf);
}

/** KVM_GET_CLOCK of the VM `vm` into `clk`. */
static void get_clock(int vm, struct kvm_clock_data *clk)
{
    k_memset(clk, 0, sizeof(*clk));
    CHECK(fdio(vm, KVM_GET_CLOCK, (uintptr_t)clk) == 0, "KVM_GET_CLOCK");
}

/** The supported CPUID with KVM's leaves, left in `buf`, and its count. */
static uint32_t pv_cpuid(void)
{
    buf.cpuid.nent = 256;
    CHECK(dev(KVM_GET_SUPPORTED_CPUID, (uintptr_t)&buf) == 0, "KVM_GET_SUPPORTED_CPUID");
    return buf.cpuid.nent;
}

/** KVM's leaves and capabilities with the TSC's rate known, and a VM's
 * vCPUs matching their TSCs under the master clock, as the kvmclock
 * selftests and kvm-unit-tests' kvmclock_test see them. */
static void pv_leaf_rows(void)
{
    uint32_t i, n, want;
    int found = 0;

    want = (1u << KVM_FEATURE_CLOCKSOURCE) | (1u << KVM_FEATURE_NOP_IO_DELAY) |
           (1u << KVM_FEATURE_CLOCKSOURCE2) | (1u << KVM_FEATURE_STEAL_TIME) |
           (1u << KVM_FEATURE_PV_UNHALT) | (1u << KVM_FEATURE_PV_TLB_FLUSH) |
           (1u << KVM_FEATURE_PV_SEND_IPI) | (1u << KVM_FEATURE_POLL_CONTROL) |
           (1u << KVM_FEATURE_PV_SCHED_YIELD) | (1u << KVM_FEATURE_CLOCKSOURCE_STABLE_BIT);
    n = pv_cpuid();
    for (i = 0; i < n; i++) {
        if (CPUID_ENTRIES[i].function == KVM_CPUID_FEATURES) {
            found = 1;
            CHECK(CPUID_ENTRIES[i].eax == want,
                  "with the TSC's rate known, the kvmclock and its stable bit are offered");
        }
    }
    CHECK(found, "KVM_CPUID_FEATURES is offered");
    CHECK(dev(KVM_CHECK_EXTENSION, KVM_CAP_KVMCLOCK_CTRL) == 1, "KVM_CAP_KVMCLOCK_CTRL");
    CHECK(dev(KVM_CHECK_EXTENSION, KVM_CAP_ENFORCE_PV_FEATURE_CPUID) == 1,
          "KVM_CAP_ENFORCE_PV_FEATURE_CPUID");
    CHECK(dev(KVM_CHECK_EXTENSION, KVM_CAP_X86_DISABLE_EXITS) ==
              (KVM_X86_DISABLE_EXITS_PAUSE | KVM_X86_DISABLE_EXITS_HLT),
          "KVM_CAP_X86_DISABLE_EXITS offers PAUSE and HLT");
    CHECK(dev(KVM_CHECK_EXTENSION, KVM_CAP_ADJUST_CLOCK) == KVM_CLOCK_VALID_FLAGS,
          "KVM_CAP_ADJUST_CLOCK names KVM_GET_CLOCK's flags");
    CHECK(dev(KVM_CHECK_EXTENSION, KVM_CAP_STEAL_TIME) == 1, "KVM_CAP_STEAL_TIME");
    CHECK(dev(KVM_CHECK_EXTENSION, KVM_CAP_VCPU_ATTRIBUTES) == 1, "KVM_CAP_VCPU_ATTRIBUTES");
}

/** The guest's kvmclock at the host's TSC count `now`, by the formula
 * the vCPU `v`'s copy at GUEST_PAGE gives, as pvclock_clocksource_read. */
static uint64_t guest_reads(const struct kvm_vcpu *v, uint64_t now)
{
    const struct pvclock_vcpu_time_info *pv = (const void *)guest_page;
    uint64_t delta = now + (uint64_t)v->tsc_offset - pv->tsc_timestamp;

    delta = pv->tsc_shift < 0 ? delta >> -pv->tsc_shift : delta << pv->tsc_shift;
    return pv->system_time + (uint64_t)(((unsigned __int128)delta * pv->tsc_to_system_mul) >> 32);
}

/* The Hyper-V reference TSC page's MSR and the page in the second guest
 * page: its sequence, scale and offset. */
#define HV_REFERENCE_TSC 0x40000021u
#define HV_TSC_GPA       (GUEST_PAGE + 0x1000u)

/** The reference TSC page's time in 100 ns at the host's TSC `now` on
 * `v`, by the TLFS's formula from the page at HV_TSC_GPA. */
static uint64_t hv_page_reads(const struct kvm_vcpu *v, uint64_t now)
{
    const uint8_t *page = guest_page + (HV_TSC_GPA - GUEST_PAGE);
    uint64_t scale, offset;

    memcpy(&scale, page + 8, 8);
    memcpy(&offset, page + 16, 8);
    return (uint64_t)(((unsigned __int128)(now + (uint64_t)v->tsc_offset) * scale) >> 64) + offset;
}

/**
 * The reference TSC page of the VM `vm` on vCPU `fd0`, whose kvmclock is
 * off: a KVM_SET_CLOCK on another processor between the run loop's
 * `kvm_hv_prepare` and its `kvm_clock_prepare`, whose request the second
 * takes, has the page written from the new clock before the entry, as
 * Linux's kvm_guest_time_update writes it (arch/x86/kvm/x86.c:2893).
 */
static void hv_clock_rows(int vm, int fd0)
{
    static const struct kvm_cpuid_entry2 hv[2] = {
        {0x40000001u, 0, 0, 0x31237648u, 0, 0, 0, {0}},
        {0x40000003u, 0, 0, 1u << 9, 0, 0, 0, {0}},
    };
    struct kvm_vcpu *v0 = entries[fd0].priv;
    const uint8_t *page = guest_page + (HV_TSC_GPA - GUEST_PAGE);
    struct kvm_clock_data clk;
    uint32_t seq;

    k_memset(guest_page + (HV_TSC_GPA - GUEST_PAGE), 0, 4096);
    CHECK(v0->vm->master_clock && !(v0->pv_time & KVM_MSR_ENABLED) &&
              kvm_hv_set_cpuid(v0, hv, 2) == 0 &&
              set_msr(fd0, HV_REFERENCE_TSC, HV_TSC_GPA | 1u) == 1 && kvm_hv_prepare(v0) == 0,
          "a reference TSC page set by the caller, with the kvmclock off");
    memcpy(&seq, page, 4);
    get_clock(vm, &clk);
    CHECK(seq == 1 && hv_page_reads(v0, tsc) == clk.clock / 100u, "is written at the next entry");
    CHECK(kvm_hv_prepare(v0) == 0, "the next pass has no page to write");
    k_memset(&clk, 0, sizeof(clk));
    clk.clock = 9000000000000ull;
    CHECK(fdio(vm, KVM_SET_CLOCK, (uintptr_t)&clk) == 0 && (v0->requests & KVM_REQ_CLOCK_UPDATE) &&
              v0->vm->hv_clock_moved,
          "KVM_SET_CLOCK just after it asks for the clock's update");
    kvm_clock_prepare(v0);
    memcpy(&seq, page, 4);
    get_clock(vm, &clk);
    CHECK(seq == 2 && hv_page_reads(v0, tsc) == clk.clock / 100u && !v0->vm->hv_clock_moved &&
              !(v0->requests & KVM_REQ_CLOCK_UPDATE),
          "the pass's kvm_clock_prepare takes the request and writes the page from the new clock");
}

/**
 * The host's clock stopping and starting under the VM `vm` on vCPU `fd0`,
 * whose reference TSC page `hv_clock_rows` set: KVM_GET_CLOCK, or the next
 * entry, decides the master clock again, as Linux's pvclock_gtod_notify
 * asks every vCPU for KVM_REQ_MASTERCLOCK_UPDATE, so the kvmclock's copy
 * loses PVCLOCK_TSC_STABLE_BIT and the reference TSC page is left at
 * sequence 0, as Linux's compute_tsc_page_parameters refuses it; the
 * clock's start brings the master clock and the bit back, and the page stays
 * broken until the guest writes its MSR again, as in Linux.
 */
static void host_clock_rows(int vm, int fd0)
{
    const struct pvclock_vcpu_time_info *pv = (const void *)guest_page;
    const uint8_t *page = guest_page + (HV_TSC_GPA - GUEST_PAGE);
    struct kvm_vcpu *v0 = entries[fd0].priv;
    struct kvm_clock_data clk;
    uint32_t seq, version;

    CHECK(set_msr(fd0, MSR_KVM_SYSTEM_TIME_NEW, GUEST_PAGE | KVM_MSR_ENABLED) == 1,
          "the kvmclock on again");
    kvm_clock_prepare(v0);
    memcpy(&seq, page, 4);
    CHECK(v0->vm->master_clock && (pv->flags & PVCLOCK_TSC_STABLE_BIT) && seq != 0,
          "under the master clock, the stable bit and the reference TSC page");
    vclock_on = 0;
    vclock_gen++;
    get_clock(vm, &clk);
    CHECK(clk.flags == 0 && !v0->vm->master_clock && (v0->requests & KVM_REQ_CLOCK_UPDATE),
          "the host's clock stopped: KVM_GET_CLOCK decides the master clock again, without it");
    kvm_clock_prepare(v0);
    memcpy(&seq, page, 4);
    CHECK(!(pv->flags & PVCLOCK_TSC_STABLE_BIT) && seq == 0 && !v0->vm->hv_clock_moved,
          "the next entry writes the kvmclock without the stable bit, and the page at sequence 0");
    vclock_on = 1;
    vclock_gen++;
    version = pv->version;
    kvm_clock_prepare(v0);
    memcpy(&seq, page, 4);
    CHECK(v0->vm->master_clock && (pv->flags & PVCLOCK_TSC_STABLE_BIT) &&
              pv->version == version + 2u && seq == 0,
          "the clock's start: the next entry decides it again, with the master clock");
    version = pv->version;
    kvm_clock_prepare(v0);
    CHECK(pv->version == version, "and the entry after it has nothing to write");
    get_clock(vm, &clk);
    CHECK(clk.flags & KVM_CLOCK_TSC_STABLE, "KVM_GET_CLOCK under the master clock again");
}

/**
 * The host's clock stopping between the run loop's preparation of vCPU
 * `fd0`, under the master clock, and its entry: the entry goes back to the
 * loop, and the fast path ends, so that the next preparation writes the
 * kvmclock without PVCLOCK_TSC_STABLE_BIT before the guest runs, as Linux's
 * KVM_REQ_MASTERCLOCK_UPDATE, which pvclock_gtod_notify posts to every
 * vCPU, cancels an entry past its preparation (arch/x86/kvm/x86.c:9166,
 * 10586). The clock's start after it brings the master clock back.
 */
static void host_clock_entry_rows(int fd0)
{
    const struct pvclock_vcpu_time_info *pv = (const void *)guest_page;
    struct kvm_vcpu *v0 = entries[fd0].priv;
    int made, was_refusing = refusing;

    v0->requests = 0;
    kvm_clock_prepare(v0);
    CHECK(v0->vm->master_clock && (pv->flags & PVCLOCK_TSC_STABLE_BIT) && !entry_waits(v0),
          "a preparation under the master clock leaves the entry nothing to wait for");
    vclock_on = 0;
    vclock_gen++;
    made = entries_made;
    CHECK(run_enter(v0) == -EAGAIN && entries_made == made && v0->mode == KVM_MODE_OUTSIDE &&
              irq_on,
          "the host's clock stopped after the preparation: the entry goes back to the run loop");
    CHECK(fast_ends(v0), "and the fast path ends at the exit the stop's kick forces");
    CHECK(v0->vm->master_clock && (pv->flags & PVCLOCK_TSC_STABLE_BIT),
          "until the loop's next preparation, the kvmclock keeps the old clock");
    kvm_clock_prepare(v0);
    CHECK(!v0->vm->master_clock && !(pv->flags & PVCLOCK_TSC_STABLE_BIT) && !entry_waits(v0),
          "which decides the master clock again and writes the kvmclock without the bit");
    refusing = 1;
    CHECK(run_enter(v0) == -EOPNOTSUPP && entries_made == made + 1,
          "the entry after it reaches the backend");
    refusing = was_refusing;
    vclock_on = 1;
    vclock_gen++;
    kvm_clock_prepare(v0);
    CHECK(v0->vm->master_clock && (pv->flags & PVCLOCK_TSC_STABLE_BIT) && !entry_waits(v0),
          "the clock's start brings the master clock back");
}

/** The kvmclock of the VM `vm` on vCPU `fd0` and its MSRs: the copy in
 * guest memory, which reads as KVM_GET_CLOCK does, the guest's stop, the
 * wall clock, the old MSR and KVM_SET_CLOCK. */
static void pv_clock_rows(int vm, int fd0)
{
    const struct pvclock_vcpu_time_info *pv = (const void *)guest_page;
    const struct pvclock_wall_clock *wc = (const void *)(guest_page + 0x100);
    struct kvm_vcpu *v0 = entries[fd0].priv;
    struct kvm_clock_data clk;
    struct kvm_clock_pairing *cp = (void *)(guest_page + 0x200);
    uint64_t value, epoch;

    CHECK(set_msr(fd0, MSR_KVM_SYSTEM_TIME_NEW, GUEST_PAGE | KVM_MSR_ENABLED) == 1,
          "MSR_KVM_SYSTEM_TIME_NEW");
    kvm_clock_prepare(v0);
    CHECK(pv->version == 2 && pv->flags == PVCLOCK_TSC_STABLE_BIT && pv->tsc_shift == 0 &&
              pv->tsc_to_system_mul == 0x80000000u,
          "the kvmclock is written, even, stable, at 2 GHz");
    clock_pass(123456789);
    get_clock(vm, &clk);
    CHECK(guest_reads(v0, tsc) == clk.clock, "the guest's kvmclock reads as KVM_GET_CLOCK's");
    kvm_clock_prepare(v0);
    CHECK(pv->version == 2, "under the master clock it is not written again");
    CHECK(fdio(fd0, KVM_KVMCLOCK_CTRL, 0) == 0, "KVM_KVMCLOCK_CTRL");
    kvm_clock_prepare(v0);
    CHECK(pv->version == 4 && pv->flags == (PVCLOCK_TSC_STABLE_BIT | PVCLOCK_GUEST_STOPPED),
          "which sets PVCLOCK_GUEST_STOPPED for the guest to clear");

    CHECK(set_msr(fd0, MSR_KVM_WALL_CLOCK_NEW, GUEST_PAGE + 0x100) == 1, "MSR_KVM_WALL_CLOCK_NEW");
    get_clock(vm, &clk);
    epoch = real_ns - clk.clock;
    CHECK(wc->version == 2 && wc->sec == epoch / 1000000000u && wc->nsec == epoch % 1000000000u,
          "the wall clock is the real time at which the kvmclock read 0");
    buf.msrs.nmsrs = 1;
    MSR_ENTRIES[0].index = MSR_KVM_WALL_CLOCK_NEW;
    CHECK(fdio(fd0, KVM_GET_MSRS, (uintptr_t)&buf) == 1 &&
              MSR_ENTRIES[0].data == GUEST_PAGE + 0x100,
          "and its MSR reads back");
    low_rw = 0;
    CHECK(set_msr(fd0, MSR_KVM_WALL_CLOCK, 0) == 1 &&
              set_msr(fd0, MSR_KVM_WALL_CLOCK_NEW, 0) == 1 && low_rw == 0,
          "a wall clock MSR of 0, as QEMU's reset writes, writes nothing at 0");

    CHECK(kvm_clock_pairing(v0, GUEST_PAGE + 0x200, KVM_CLOCK_PAIRING_WALLCLOCK) == 0 &&
              (uint64_t)cp->sec * 1000000000u + (uint64_t)cp->nsec == real_ns &&
              cp->tsc == tsc + (uint64_t)v0->tsc_offset,
          "KVM_HC_CLOCK_PAIRING pairs the real time with the guest's TSC");
    CHECK(kvm_clock_pairing(v0, GUEST_PAGE + 0x200, 1) == (uint64_t)-95, "and no other clock");

    value = GUEST_PAGE | KVM_MSR_ENABLED;
    CHECK(kvm_pv_msr(v0, MSR_KVM_SYSTEM_TIME, 1, &value, 0) == 0 && !v0->vm->master_clock,
          "the boot vCPU's guest choosing the old MSR stops the master clock");
    get_clock(vm, &clk);
    CHECK(clk.flags == 0, "and KVM_GET_CLOCK's flags with it");
    CHECK(kvm_pv_msr(v0, MSR_KVM_SYSTEM_TIME_NEW, 1, &value, 0) == 0 && v0->vm->master_clock,
          "the new MSR starts it again");

    k_memset(&clk, 0, sizeof(clk));
    clk.clock = 5000000000ull;
    CHECK(fdio(vm, KVM_SET_CLOCK, (uintptr_t)&clk) == 0, "KVM_SET_CLOCK under the master clock");
    clock_pass(1000);
    get_clock(vm, &clk);
    CHECK(clk.clock == 5000001000ull, "sets the kvmclock exactly");
    kvm_clock_prepare(v0);
    CHECK(guest_reads(v0, tsc) == clk.clock, "and the guest's copy follows it");
}

/** The vCPUs' TSCs of the VM `vm`, vCPUs `fd0` and `fd1`: a new vCPU
 * matches, a host write elsewhere starts a generation without the master
 * clock, and a write that matches it again brings the master clock back. */
static void pv_tsc_rows(int vm, int fd0, int fd1)
{
    struct kvm_vcpu *v0 = entries[fd0].priv, *v1 = entries[fd1].priv;
    struct kvm_clock_data clk;

    CHECK(v1->tsc_offset == v0->tsc_offset && v0->vm->master_clock,
          "a new vCPU takes the TSC of those before it");
    CHECK(set_msr(fd1, MSR_IA32_TSC, 5000000000000ull) == 1 && !v0->vm->master_clock,
          "a write of another TSC stops the master clock");
    get_clock(vm, &clk);
    CHECK(clk.flags == 0, "KVM_GET_CLOCK then has no flags");
    clock_pass(1000000);
    CHECK(set_msr(fd0, MSR_IA32_TSC, 5000000000000ull + 2000000u) == 1 &&
              v0->tsc_offset == v1->tsc_offset && v0->vm->master_clock,
          "a write that matches it starts the master clock again");
    get_clock(vm, &clk);
    CHECK(clk.flags == (KVM_CLOCK_TSC_STABLE | KVM_CLOCK_REALTIME | KVM_CLOCK_HOST_TSC),
          "KVM_GET_CLOCK says so");
}

static int32_t get_msr(int fd, uint32_t index, uint64_t *value);

/* The steal time's record in the guest page, 64-byte aligned. */
#define ST_GPA (GUEST_PAGE + 0x400u)

/** A halted vCPU's sleep in KVM_RUN on vCPU `fd0`, whose steal time is on
 * and whose `reported` preemptions are counted so far: the switch tells the
 * thread's `sched_out` it is not preempted, so the record is not marked and
 * no preemption is counted, as Linux's kvm_sched_out marks only a task
 * still on its run queue (virt/kvm/kvm_main.c). */
static void steal_block_row(int fd0, uint64_t reported)
{
    struct kvm_vcpu *v0 = entries[fd0].priv;
    struct kvm_steal_time *st = (void *)(guest_page + (ST_GPA - GUEST_PAGE));
    unsigned long nvcsw = caller.nvcsw;

    script_len = 0;
    script_at = 0;
    scripted(KVM_X_AGAIN);
    scripted(KVM_X_HLT);
    v0->vm->irqchip = KVM_IRQCHIP_SPLIT;
    block_turn = 2;
    block_seen = -1;
    CHECK(fdio(fd0, KVM_RUN, 0) == 0 && run_of(fd0)->exit_reason == KVM_EXIT_HLT &&
              caller.nvcsw == nvcsw + 1,
          "KVM_RUN through a halted vCPU's sleep after an exit, to a HLT");
    v0->vm->irqchip = KVM_IRQCHIP_NONE;
    CHECK(block_seen == 0 && st->preempted == 0 && st->version == 24 &&
              v0->stat[KVM_VCPU_STAT_PREEMPTION_REPORTED] == reported + 3 &&
              v0->stat[KVM_VCPU_STAT_PREEMPTION_OTHER] == 1,
          "the sleep's switch is not a preemption: the record is not marked, nor counted, and the "
          "switch in after it asks for a write, as KVM_RUN's start does");
}

/** The guest's mark to flush its TLB, KVM_FEATURE_PV_TLB_FLUSH, in `v0`'s
 * steal time record `st`, which the last write left at version 26. */
static void steal_flush_rows(struct kvm_vcpu *v0, struct kvm_steal_time *st)
{
    v0->requests &= ~KVM_REQ_TLB_FLUSH;
    st->preempted = KVM_VCPU_PREEMPTED;
    caller.pcount++;
    kvm_steal_prepare(v0);
    CHECK(st->version == 28 && st->preempted == 0 && !(v0->requests & KVM_REQ_TLB_FLUSH),
          "a record that says preempted alone is cleared with no flush of the guest's TLB");
    st->preempted = KVM_VCPU_PREEMPTED | KVM_VCPU_FLUSH_TLB;
    st->u8_pad[0] = 0x5A;
    caller.pcount++;
    kvm_steal_prepare(v0);
    CHECK(st->version == 30 && st->preempted == 0 && st->u8_pad[0] == 0x5A &&
              (v0->requests & KVM_REQ_TLB_FLUSH),
          "the guest's KVM_VCPU_FLUSH_TLB in a preempted record is taken with `preempted`, its "
          "padding kept, and the entry flushes the guest's TLB, as Linux's record_steal_time "
          "with KVM_FEATURE_PV_TLB_FLUSH");
    v0->requests &= ~KVM_REQ_TLB_FLUSH;
    st->u8_pad[0] = 0;
}

/** The steal time of vCPU `fd0`, as the selftest steal_time and Linux's
 * record_steal_time see it: MSR_KVM_STEAL_TIME refuses its reserved bits,
 * each write adds the thread's run_delay since the last and keeps the
 * version even, a switch of the thread asks for a write, and a preemption
 * at a host interrupt marks the record preempted until the next entry. */
static void steal_rows(int fd0)
{
    struct kvm_vcpu *v0 = entries[fd0].priv;
    struct kvm_steal_time *st = (void *)(guest_page + (ST_GPA - GUEST_PAGE));
    struct kvm_run *run = run_of(fd0);
    uint64_t value = 0, reported;

    CHECK(set_msr(fd0, MSR_KVM_STEAL_TIME, ST_GPA | KVM_STEAL_RESERVED_MASK) == 0,
          "MSR_KVM_STEAL_TIME refuses its reserved bits");
    k_memset(st, 0, sizeof(*st));
    st->version = 7; /* what the guest left there, odd */
    st->preempted = KVM_VCPU_PREEMPTED;
    caller.run_delay = 5000;
    CHECK(set_msr(fd0, MSR_KVM_STEAL_TIME, ST_GPA | KVM_MSR_ENABLED) == 1 &&
              get_msr(fd0, MSR_KVM_STEAL_TIME, &value) == 1 && value == (ST_GPA | KVM_MSR_ENABLED),
          "MSR_KVM_STEAL_TIME takes an aligned address and reads it back");
    kvm_steal_prepare(v0);
    CHECK(st->version == 10 && st->steal == 5000 && st->preempted == 0,
          "the first write evens the version, adds the thread's whole run_delay and clears "
          "preempted");
    kvm_steal_prepare(v0);
    CHECK(st->version == 10, "nothing is written again while the thread stays on its processor");
    caller.run_delay += 3000;
    caller.pcount++;
    kvm_steal_prepare(v0);
    CHECK(st->version == 12 && st->steal == 8000,
          "a switch of the thread adds the wait since the last write");

    reported = v0->stat[KVM_VCPU_STAT_PREEMPTION_REPORTED];
    v0->at_boundary = 0;
    kvm_steal_preempted(v0);
    CHECK(st->preempted == 0 && v0->stat[KVM_VCPU_STAT_PREEMPTION_OTHER] == 1,
          "a preemption away from an instruction boundary is not reported");
    v0->at_boundary = 1;
    kvm_steal_preempted(v0);
    CHECK(st->preempted == KVM_VCPU_PREEMPTED &&
              v0->stat[KVM_VCPU_STAT_PREEMPTION_REPORTED] == reported + 1,
          "one at a host interrupt marks the record preempted");
    kvm_steal_prepare(v0);
    CHECK(st->preempted == 0 && st->version == 14 && st->steal == 8000,
          "and the next entry clears it, with no switch in between");
    nofault_refuse = 1;
    kvm_steal_preempted(v0);
    CHECK(st->preempted == 0 && !v0->st_preempted &&
              v0->stat[KVM_VCPU_STAT_PREEMPTION_REPORTED] == reported + 2,
          "a record that cannot be written without a fault is not marked, and the preemption is "
          "still reported, as Linux's");
    nofault_refuse = 0;

    refusing = 0;
    script_len = 0;
    script_at = 0;
    scripted(KVM_X_AGAIN);
    scripted(KVM_X_HLT);
    tick_exits = 1;
    preempt_wait = 2000;
    preempt_seen_at = &st->preempted;
    run->immediate_exit = 0;
    CHECK(fdio(fd0, KVM_RUN, 0) == 0 && run->exit_reason == KVM_EXIT_HLT,
          "KVM_RUN through a tick that takes the processor, to a HLT");
    CHECK(preempt_seen == KVM_VCPU_PREEMPTED && st->preempted == 0,
          "the record says preempted while the thread is switched out, and not after");
    CHECK(st->steal == 10000 && st->version == 18,
          "the wait on the run queue is stolen time at the next entry");
    CHECK(v0->stat[KVM_VCPU_STAT_PREEMPTION_REPORTED] == reported + 3 && caller.sched_out == NULL,
          "and is reported, and the thread has no sched_out after KVM_RUN");
    script_len = 0;
    script_at = 0;
    scripted(KVM_X_AGAIN);
    scripted(KVM_X_HLT);
    preempt_alone = 1;
    preempt_seen = -1;
    CHECK(fdio(fd0, KVM_RUN, 0) == 0 && run->exit_reason == KVM_EXIT_HLT,
          "KVM_RUN through a tick whose request finds no other thread, to a HLT");
    CHECK(preempt_seen == -1 && st->preempted == 0 && st->version == 20 &&
              v0->stat[KVM_VCPU_STAT_PREEMPTION_REPORTED] == reported + 3 &&
              v0->stat[KVM_VCPU_STAT_PREEMPTION_OTHER] == 1,
          "the vCPU is not marked preempted, nor counted, when nothing switches it out, and the "
          "record is written once, at the entry each KVM_RUN asks for, as Linux's vcpu_load");
    preempt_alone = 0;
    tick_exits = 0;
    preempt_wait = 0;
    steal_block_row(fd0, reported);
    preempt_seen_at = NULL;
    guest_vma.prot = VM_PROT_SECRET;
    v0->vm->mmu_seq++;
    caller.pcount++;
    kvm_steal_prepare(v0);
    kvm_steal_preempted(v0);
    CHECK(st->version == 26 && !v0->st_ok && !v0->st_preempted && st->preempted == 0,
          "a record in a memfd_secret region, looked at again once the tables change, is written "
          "through the caller's address, as Linux's record_steal_time, and never marked "
          "preempted without a fault");
    guest_vma.prot = 0;
    steal_flush_rows(v0, st);
    refusing = 1;

    CHECK(set_msr(fd0, MSR_KVM_STEAL_TIME, ST_GPA) == 1, "the guest turns it off");
    caller.pcount++;
    kvm_steal_prepare(v0);
    CHECK(st->version == 30, "and nothing more is written");
}

/* A steal time record in the guest's second page, of memfd_secret. */
#define ST_SECRET_GPA (GUEST_PAGE + 0x1400u)

/** The steal time of vCPU `fd0` moved by the guest to a page of
 * memfd_secret while the tables' count stays: neither a preemption before
 * the next entry nor one after marks the new record, which was never found
 * outside memfd_secret, as Linux compares its cache's gpa with the MSR's
 * (arch/x86/kvm/x86.c:3259-3263, 4646-4650). Leaves the MSR
 * off. */
static void steal_move_rows(int fd0)
{
    struct kvm_vcpu *v0 = entries[fd0].priv;
    struct kvm_steal_time *st = (void *)(guest_page + (ST_GPA - GUEST_PAGE));
    struct kvm_steal_time *moved = (void *)(guest_page + (ST_SECRET_GPA - GUEST_PAGE));
    uint32_t seq = v0->vm->mmu_seq;

    k_memset(moved, 0, sizeof(*moved));
    CHECK(set_msr(fd0, MSR_KVM_STEAL_TIME, ST_GPA | KVM_MSR_ENABLED) == 1,
          "the guest turns the steal time on again");
    kvm_steal_prepare(v0);
    CHECK(v0->st_ok && v0->st_gpa == ST_GPA && v0->st_seq == seq,
          "its record is found outside memfd_secret at the next entry");
    CHECK(set_msr(fd0, MSR_KVM_STEAL_TIME, ST_SECRET_GPA | KVM_MSR_ENABLED) == 1,
          "the guest moves the record into a page of memfd_secret");
    v0->at_boundary = 1;
    kvm_steal_preempted(v0);
    CHECK(moved->preempted == 0 && st->preempted == 0 && !v0->st_preempted,
          "a preemption before the next entry marks neither record");
    kvm_steal_prepare(v0);
    CHECK(moved->version == 2 && !v0->st_ok && v0->vm->mmu_seq == seq,
          "the next entry writes the new record and finds it in memfd_secret, the tables' "
          "count unchanged");
    kvm_steal_preempted(v0);
    CHECK(moved->preempted == 0 && !v0->st_preempted,
          "and a preemption after it does not mark the new record");
    CHECK(set_msr(fd0, MSR_KVM_STEAL_TIME, ST_SECRET_GPA) == 1, "the guest turns it off");
    kvm_steal_prepare(v0);
}

/** A guest's INIT on vCPU `fd0`, as Linux's kvm_vcpu_reset: the steal
 * time and the kvmclock, both on, are turned off, and neither is written
 * at the next entry. */
static void init_pv_rows(int fd0)
{
    struct kvm_vcpu *v0 = entries[fd0].priv;
    struct kvm_steal_time *st = (void *)(guest_page + (ST_GPA - GUEST_PAGE));
    const struct pvclock_vcpu_time_info *pv = (const void *)guest_page;
    uint64_t value = 0;
    uint32_t pv_version, st_version = st->version;

    CHECK(set_msr(fd0, MSR_KVM_STEAL_TIME, ST_GPA | KVM_MSR_ENABLED) == 1 &&
              set_msr(fd0, MSR_KVM_SYSTEM_TIME_NEW, GUEST_PAGE | KVM_MSR_ENABLED) == 1,
          "the steal time and the kvmclock on again");
    kvm_steal_prepare(v0);
    kvm_clock_prepare(v0);
    pv_version = pv->version;
    CHECK(st->version == st_version + 2u && (v0->pv_time & KVM_MSR_ENABLED) && pv_version % 2u == 0,
          "both are written at the next entry");
    kvm_vcpu_init_reset(v0);
    CHECK(get_msr(fd0, MSR_KVM_STEAL_TIME, &value) == 1 && value == 0 &&
              get_msr(fd0, MSR_KVM_SYSTEM_TIME_NEW, &value) == 1 && value == 0,
          "a guest's INIT turns the steal time and the kvmclock off, as Linux's kvm_vcpu_reset");
    caller.pcount++;
    kvm_vcpu_kick(v0, KVM_REQ_STEAL_UPDATE);
    kvm_vcpu_kick(v0, KVM_REQ_CLOCK_UPDATE);
    kvm_steal_prepare(v0);
    kvm_clock_prepare(v0);
    st_version += 2u;
    CHECK(st->version == st_version && pv->version == pv_version,
          "and neither is written at the next entry, a switch and requests for both "
          "notwithstanding");
}

/** A guest's INIT on vCPU `fd0` leaves its APIC base as it is and never
 * clears it on the way, as Linux's kvm_lapic_reset sets the base only at a
 * reset (arch/x86/kvm/lapic.c:2814-2826): another vCPU's delivery reads it
 * without a lock, and a SIPI sent while the INIT is taken would otherwise
 * find the APIC off and be dropped. */
static void init_apic_base_row(int fd0)
{
    struct kvm_vcpu *v0 = entries[fd0].priv;
    uint64_t base = v0->sregs.apic_base;

    memset_watch = &v0->sregs.apic_base;
    memset_watch_hits = 0;
    kvm_vcpu_init_reset(v0);
    memset_watch = NULL;
    CHECK(base & 0x800u, "the vCPU's APIC is enabled before its INIT");
    CHECK(memset_watch_hits == 0 && v0->sregs.apic_base == base,
          "a guest's INIT keeps the APIC base whole, so a SIPI during it is kept");
}

/** CR0 after a guest's INIT on vCPU `fd0`, from each setting of CD and NW
 * before it: ET with CD and NW kept as they were, as Linux's
 * kvm_vcpu_reset keeps them at an INIT and sets them only at power-on
 * (arch/x86/kvm/x86.c:11752-11761). */
static void init_cr0_rows(int fd0)
{
    static const uint64_t before[3] = {0x00000011ull, 0x40000011ull, 0x60000011ull};
    struct kvm_vcpu *v0 = entries[fd0].priv;
    struct kvm_sregs sregs;
    int i, kept = 1;

    for (i = 0; i < 3; i++) {
        CHECK(fdio(fd0, KVM_GET_SREGS, (uintptr_t)&sregs) == 0, "KVM_GET_SREGS");
        sregs.cr0 = before[i];
        CHECK(fdio(fd0, KVM_SET_SREGS, (uintptr_t)&sregs) == 0, "KVM_SET_SREGS of CR0");
        kvm_vcpu_init_reset(v0);
        CHECK(fdio(fd0, KVM_GET_SREGS, (uintptr_t)&sregs) == 0, "KVM_GET_SREGS");
        kept &= sregs.cr0 == (before[i] & 0x60000000ull) + 0x10u;
    }
    CHECK(kept, "a guest's INIT leaves CR0 at ET with CD and NW as they were");
}

/** The vCPU attribute KVM_VCPU_TSC_OFFSET of vCPUs `fd0` and `fd1` of the
 * VM `vm`, as system_counter_offset_test drives it and Linux's
 * kvm_arch_tsc_set_attr matches it: the guest's TSC is the host's plus the
 * offset, a new offset on one vCPU starts a generation without the master
 * clock, and the same offset on the other joins it and brings the master
 * clock back. */
static void tsc_attr_rows(int vm, int fd0, int fd1)
{
    struct kvm_vcpu *v0 = entries[fd0].priv, *v1 = entries[fd1].priv;
    struct kvm_device_attr attr;
    struct kvm_clock_data clk;
    uint64_t value = 0, offset = (uint64_t)-180000000000ll;

    k_memset(&attr, 0, sizeof(attr));
    attr.group = KVM_VCPU_TSC_CTRL;
    attr.attr = KVM_VCPU_TSC_OFFSET;
    attr.addr = (uint64_t)(uintptr_t)&value;
    CHECK(fdio(fd0, KVM_HAS_DEVICE_ATTR, (uintptr_t)&attr) == 0, "a vCPU has KVM_VCPU_TSC_OFFSET");
    CHECK(fdio(fd0, KVM_GET_DEVICE_ATTR, (uintptr_t)&attr) == 0 &&
              value == (uint64_t)v0->tsc_offset,
          "which reads as its TSC's offset");
    value = offset;
    CHECK(fdio(fd0, KVM_SET_DEVICE_ATTR, (uintptr_t)&attr) == 0 &&
              v0->tsc_offset == (int64_t)offset && kvm_tsc_read(v0) == tsc + offset,
          "setting it makes the guest's TSC the host's plus the offset");
    CHECK(!v0->vm->master_clock, "a new offset stops the master clock");
    value = 0;
    CHECK(fdio(fd0, KVM_GET_DEVICE_ATTR, (uintptr_t)&attr) == 0 && value == offset,
          "and reads back");
    value = offset;
    CHECK(fdio(fd1, KVM_SET_DEVICE_ATTR, (uintptr_t)&attr) == 0 &&
              v1->tsc_offset == v0->tsc_offset && v0->vm->master_clock,
          "the same offset on the other vCPU matches it and starts the master clock again");
    get_clock(vm, &clk);
    CHECK(clk.flags == (KVM_CLOCK_TSC_STABLE | KVM_CLOCK_REALTIME | KVM_CLOCK_HOST_TSC),
          "KVM_GET_CLOCK says so");
    attr.group = 1;
    CHECK(fdio(fd0, KVM_HAS_DEVICE_ATTR, (uintptr_t)&attr) == -ENXIO &&
              fdio(fd0, KVM_GET_DEVICE_ATTR, (uintptr_t)&attr) == -ENXIO &&
              fdio(fd0, KVM_SET_DEVICE_ATTR, (uintptr_t)&attr) == -ENXIO,
          "another group is ENXIO");
    attr.group = KVM_VCPU_TSC_CTRL;
    attr.attr = 1;
    CHECK(fdio(fd0, KVM_HAS_DEVICE_ATTR, (uintptr_t)&attr) == -ENXIO,
          "and so is another attribute");
}

/** KVM_CAP_ENFORCE_PV_FEATURE_CPUID on vCPU `fd1` of the VM `vm`, and
 * KVM_CAP_X86_DISABLE_EXITS: PV_UNHALT dropped at KVM_SET_CPUID2 and HLT
 * left to the guests of vCPUs made after it. */
static void pv_cap_rows(int vm, int fd0, int fd1)
{
    struct kvm_vcpu *v0 = entries[fd0].priv, *v1 = entries[fd1].priv, *v2;
    struct kvm_enable_cap cap;
    uint64_t value = 0;
    uint32_t n;
    int fd2;

    k_memset(&cap, 0, sizeof(cap));
    cap.cap = KVM_CAP_ENFORCE_PV_FEATURE_CPUID;
    cap.args[0] = 1;
    CHECK(fdio(fd1, KVM_ENABLE_CAP, (uintptr_t)&cap) == 0, "KVM_CAP_ENFORCE_PV_FEATURE_CPUID");
    CHECK(kvm_pv_msr(v1, MSR_KVM_SYSTEM_TIME_NEW, 0, &value, 0) == KVM_MSR_UNKNOWN &&
              !kvm_pv_has(v1, KVM_FEATURE_PV_SEND_IPI),
          "holds a vCPU with no KVM leaf to no feature");
    value = 1;
    CHECK(kvm_pv_msr(v1, MSR_KVM_SYSTEM_TIME_NEW, 0, &value, 1) == 0 && value == 0 &&
              set_msr(fd1, MSR_KVM_SYSTEM_TIME_NEW, 0) == 1 &&
              set_msr(fd1, MSR_KVM_SYSTEM_TIME_NEW, 1) == 0,
          "but lets the caller read 0 and write 0");
    n = pv_cpuid();
    buf.cpuid.nent = n;
    CHECK(fdio(fd1, KVM_SET_CPUID2, (uintptr_t)&buf) == 0 &&
              kvm_pv_msr(v1, MSR_KVM_SYSTEM_TIME_NEW, 0, &value, 0) == 0 &&
              kvm_pv_has(v1, KVM_FEATURE_PV_SEND_IPI),
          "and to those its CPUID names");

    k_memset(&cap, 0, sizeof(cap));
    cap.cap = KVM_CAP_X86_DISABLE_EXITS;
    cap.args[0] = 0x10;
    CHECK(fdio(vm, KVM_ENABLE_CAP, (uintptr_t)&cap) == -EINVAL, "an exit Linux does not name");
    cap.args[0] = KVM_X86_DISABLE_EXITS_HLT;
    CHECK(fdio(vm, KVM_ENABLE_CAP, (uintptr_t)&cap) == 0,
          "KVM_CAP_X86_DISABLE_EXITS after the vCPUs, as Linux 6.12 takes it");
    n = pv_cpuid();
    buf.cpuid.nent = n;
    CHECK(fdio(fd0, KVM_SET_CPUID2, (uintptr_t)&buf) == 0 &&
              !(v0->pv_features & (1u << KVM_FEATURE_PV_UNHALT)) &&
              (v0->pv_features & (1u << KVM_FEATURE_CLOCKSOURCE2)),
          "KVM_SET_CPUID2 then drops PV_UNHALT");
    fd2 = fdio(vm, KVM_CREATE_VCPU, 2);
    v2 = entries[fd2].priv;
    CHECK(fd2 > fd1 && v2->hlt_in_guest && !v0->hlt_in_guest,
          "and a vCPU made after it leaves HLT to its guest");
    fdclose(fd2);
}

/** The paravirtual rows, in a VM of their own with the TSC's rate known,
 * the vDSO's clock on the TSC and a page of guest memory. */
static void pv_rows(void)
{
    struct kvm_clock_data clk;
    int vm, fd0, fd1;

    tick_counts = 2000000000u / TICK_HZ;
    vclock_on = 1;
    guest_mem = 1;
    pv_leaf_rows();
    vm = dev(KVM_CREATE_VM, 0);
    get_clock(vm, &clk);
    CHECK(clk.clock == 0 && clk.flags == 0, "no master clock before a vCPU");
    clock_pass(1000000);
    fd0 = fdio(vm, KVM_CREATE_VCPU, 0);
    get_clock(vm, &clk);
    CHECK(clk.flags == (KVM_CLOCK_TSC_STABLE | KVM_CLOCK_REALTIME | KVM_CLOCK_HOST_TSC) &&
              clk.clock == 1000000 && clk.host_tsc == tsc && clk.realtime == real_ns,
          "the master clock with the first vCPU, and its flags");
    fd1 = fdio(vm, KVM_CREATE_VCPU, 1);
    pv_tsc_rows(vm, fd0, fd1);
    pv_clock_rows(vm, fd0);
    tsc_attr_rows(vm, fd0, fd1);
    pv_cap_rows(vm, fd0, fd1);
    steal_rows(fd0);
    steal_move_rows(fd0);
    init_pv_rows(fd0);
    init_apic_base_row(fd0);
    init_cr0_rows(fd0);
    hv_clock_rows(vm, fd0);
    host_clock_rows(vm, fd0);
    host_clock_entry_rows(fd0);
    fdclose(fd1);
    fdclose(fd0);
    fdclose(vm);
    CHECK(live == 0, "the VM frees every allocation");
    tick_counts = 0;
    vclock_on = 0;
    guest_mem = 0;
}

/** KVM_ENABLE_CAP of `cap` with `arg` on the VM `vm`. */
static int32_t vm_cap(int vm, uint32_t cap, uint64_t arg)
{
    struct kvm_enable_cap c;

    k_memset(&c, 0, sizeof(c));
    c.cap = cap;
    c.args[0] = arg;
    return fdio(vm, KVM_ENABLE_CAP, (uintptr_t)&c);
}

/** KVM_GET_MSRS of the MSR `index` on the vCPU `fd` into `*value`; the
 * count read. */
static int32_t get_msr(int fd, uint32_t index, uint64_t *value)
{
    int32_t r;

    buf.msrs.nmsrs = 1;
    MSR_ENTRIES[0].index = index;
    MSR_ENTRIES[0].reserved = 0;
    MSR_ENTRIES[0].data = 0;
    r = fdio(fd, KVM_GET_MSRS, (uintptr_t)&buf);
    *value = MSR_ENTRIES[0].data;
    return r;
}

/** Linux's bounds, 1024 vCPUs with ids below 4096, and
 * KVM_CAP_MAX_VCPU_ID, as max_vcpuid_cap_test and kvm_create_max_vcpus
 * ask for them. */
static void bound_rows(void)
{
    int vm = dev(KVM_CREATE_VM, 0), fd, made = 0;
    uint32_t i;

    CHECK(dev(KVM_CHECK_EXTENSION, KVM_CAP_MAX_VCPUS) == 1024 &&
              dev(KVM_CHECK_EXTENSION, KVM_CAP_MAX_VCPU_ID) == 4096,
          "Linux's bounds: 1024 vCPUs, with ids below 4096");
    CHECK(vm_cap(vm, KVM_CAP_MAX_VCPU_ID, 4097) == -EINVAL, "a bound of ids past Linux's");
    CHECK(fdio(vm, KVM_SET_BOOT_CPU_ID, 70) == 0 && vm_cap(vm, KVM_CAP_MAX_VCPU_ID, 69) == -EINVAL,
          "a bound of ids below the boot vCPU's");
    CHECK(vm_cap(vm, KVM_CAP_MAX_VCPU_ID, 70) == 0 && vm_cap(vm, KVM_CAP_MAX_VCPU_ID, 70) == 0,
          "KVM_CAP_MAX_VCPU_ID, and the same bound again");
    CHECK(vm_cap(vm, KVM_CAP_MAX_VCPU_ID, 71) == -EINVAL, "another bound once one is set");
    CHECK(fdio(vm, KVM_SET_BOOT_CPU_ID, 71) == -EINVAL, "a boot vCPU past the bound");
    CHECK(fdio(vm, KVM_CREATE_VCPU, 70) == -EINVAL, "a vCPU's id at the bound");
    CHECK(fdio(vm, KVM_CREATE_VCPU, (uintptr_t)1 << 32) == -EINVAL,
          "an id with a bit above the low 32 set");
    fd = fdio(vm, KVM_CREATE_VCPU, 69);
    CHECK(fd > vm, "a vCPU's id below the bound");
    fdclose(fd);
    fdclose(vm);

    vm = dev(KVM_CREATE_VM, 0);
    for (i = 0; i < KVM_MAX_VCPUS; i++) {
        fd = fdio(vm, KVM_CREATE_VCPU, KVM_MAX_VCPU_IDS - 1u - i);
        if (fd > vm) {
            made++;
            fdclose(fd);
        }
    }
    CHECK(made == KVM_MAX_VCPUS, "a VM takes 1024 vCPUs, with ids up to 4095");
    CHECK(vm_cap(vm, KVM_CAP_MAX_VCPU_ID, 4096) == 0 &&
              vm_cap(vm, KVM_CAP_MAX_VCPU_ID, 512) == -EINVAL,
          "the first vCPU settles the bound of the ids at Linux's");
    CHECK(fdio(vm, KVM_CREATE_VCPU, 0) == -EINVAL, "and no vCPU past 1024");
    fdclose(vm);
    CHECK(live == 0, "the VMs free every allocation");
}

/** KVM_SET_SREGS before KVM_SET_CPUID2 takes the CR4 bits the supported
 * CPUID has, as set_sregs_test asks, and after it those the vCPU's has. */
static void cr4_rows(void)
{
    int vm = dev(KVM_CREATE_VM, 0), fd = fdio(vm, KVM_CREATE_VCPU, 0);
    struct kvm_sregs s;

    fdio(fd, KVM_GET_SREGS, (uintptr_t)&s);
    s.cr4 = 0x1707FF; /* the model's: FSGSBASE, PCIDE, OSXSAVE and SMEP */
    CHECK(fdio(fd, KVM_SET_SREGS, (uintptr_t)&s) == 0,
          "before KVM_SET_CPUID2, CR4 takes every bit the supported CPUID has");
    s.cr4 = 1ull << 21;
    CHECK(fdio(fd, KVM_SET_SREGS, (uintptr_t)&s) == -EINVAL, "but not SMAP, which it lacks");
    s.cr4 = 1ull << 13;
    CHECK(fdio(fd, KVM_SET_SREGS, (uintptr_t)&s) == -EINVAL, "nor VMXE");
    buf.cpuid.nent = 0;
    CHECK(fdio(fd, KVM_SET_CPUID2, (uintptr_t)&buf) == 0, "KVM_SET_CPUID2 with no entry");
    s.cr4 = 1ull << 18;
    CHECK(fdio(fd, KVM_SET_SREGS, (uintptr_t)&s) == -EINVAL,
          "after it, CR4 takes only what the vCPU's CPUID has");
    fdclose(fd);
    fdclose(vm);
}

/** The capabilities a VM enables: the quirks,
 * KVM_CAP_EXIT_ON_EMULATION_FAILURE, KVM_CAP_MSR_PLATFORM_INFO with the
 * MSRs of CPUID faulting, KVM_CAP_X86_APIC_BUS_CYCLES_NS and
 * KVM_CAP_VM_DISABLE_NX_HUGE_PAGES. */
static void vm_cap_rows(void)
{
    int vm = dev(KVM_CREATE_VM, 0), fd;
    struct kvm_vm *k = (struct kvm_vm *)entries[vm].priv;
    struct kvm_vcpu *v;
    uint64_t value = 0;

    CHECK(dev(KVM_CHECK_EXTENSION, KVM_CAP_DISABLE_QUIRKS2) == 0x7FF &&
              dev(KVM_CHECK_EXTENSION, KVM_CAP_DISABLE_QUIRKS) == 1,
          "KVM_CAP_DISABLE_QUIRKS2 names Linux's eleven quirks on a processor that snoops itself");
    CHECK(k->disabled_quirks == KVM_X86_QUIRK_CD_NW_CLEARED,
          "a VM on VMX starts without SVM's quirk");
    CHECK(vm_cap(vm, KVM_CAP_DISABLE_QUIRKS2, 0x800) == -EINVAL, "a quirk Linux does not name");
    CHECK(vm_cap(vm, KVM_CAP_DISABLE_QUIRKS2, KVM_X86_QUIRK_FIX_HYPERCALL_INSN) == 0 &&
              !KVM_QUIRK(k, KVM_X86_QUIRK_FIX_HYPERCALL_INSN) &&
              KVM_QUIRK(k, KVM_X86_QUIRK_LINT0_REENABLED),
          "KVM_CAP_DISABLE_QUIRKS2 turns one quirk off and leaves the others");
    CHECK(vm_cap(vm, KVM_CAP_DISABLE_QUIRKS, 0x800 | KVM_X86_QUIRK_STUFF_FEATURE_MSRS) == 0 &&
              !KVM_QUIRK(k, KVM_X86_QUIRK_STUFF_FEATURE_MSRS),
          "KVM_CAP_DISABLE_QUIRKS drops the bits it does not know");

    CHECK(dev(KVM_CHECK_EXTENSION, KVM_CAP_EXIT_ON_EMULATION_FAILURE) == 1 &&
              vm_cap(vm, KVM_CAP_EXIT_ON_EMULATION_FAILURE, 2) == -EINVAL &&
              vm_cap(vm, KVM_CAP_EXIT_ON_EMULATION_FAILURE, 1) == 0 && k->exit_on_emulation_error,
          "KVM_CAP_EXIT_ON_EMULATION_FAILURE takes 0 or 1");

    CHECK(dev(KVM_CHECK_EXTENSION, KVM_CAP_X86_APIC_BUS_CYCLES_NS) == 1 &&
              vm_cap(vm, KVM_CAP_X86_APIC_BUS_CYCLES_NS, 4) == -ENXIO,
          "the APIC's bus cycle needs the local APIC in the kernel");
    k->irqchip = KVM_IRQCHIP_SPLIT;
    CHECK(vm_cap(vm, KVM_CAP_X86_APIC_BUS_CYCLES_NS, 0) == -EINVAL &&
              vm_cap(vm, KVM_CAP_X86_APIC_BUS_CYCLES_NS, 1ull << 26) == -EINVAL,
          "a cycle of 0 ns, or one whose longest count passes 64 bits of ns");
    CHECK(vm_cap(vm, KVM_CAP_X86_APIC_BUS_CYCLES_NS, 4) == 0 && k->apic_bus_ns == 4,
          "KVM_CAP_X86_APIC_BUS_CYCLES_NS sets the cycle");
    k->irqchip = KVM_IRQCHIP_NONE;

    CHECK(dev(KVM_CHECK_EXTENSION, KVM_CAP_VM_DISABLE_NX_HUGE_PAGES) == 1,
          "KVM_CAP_VM_DISABLE_NX_HUGE_PAGES answers 1");
    caps_lost = 1u << CAP_SYS_BOOT;
    CHECK(vm_cap(vm, KVM_CAP_VM_DISABLE_NX_HUGE_PAGES, 1) == -EPERM,
          "it needs CAP_SYS_BOOT, before the argument is looked at");
    caps_lost = 0;
    caller.userns = (struct userns *)&caps_lost;
    CHECK(vm_cap(vm, KVM_CAP_VM_DISABLE_NX_HUGE_PAGES, 0) == -EPERM,
          "held in the first user namespace");
    caller.userns = NULL;
    CHECK(vm_cap(vm, KVM_CAP_VM_DISABLE_NX_HUGE_PAGES, 1) == -EINVAL,
          "it takes only the argument 0");
    CHECK(vm_cap(vm, KVM_CAP_VM_DISABLE_NX_HUGE_PAGES, 0) == 0 &&
              vm_cap(vm, KVM_CAP_VM_DISABLE_NX_HUGE_PAGES, 0) == 0,
          "and is taken again before a vCPU");

    fd = fdio(vm, KVM_CREATE_VCPU, 0);
    v = (struct kvm_vcpu *)entries[fd].priv;
    k->irqchip = KVM_IRQCHIP_SPLIT;
    CHECK(vm_cap(vm, KVM_CAP_X86_APIC_BUS_CYCLES_NS, 8) == -EINVAL, "but not after a vCPU");
    k->irqchip = KVM_IRQCHIP_NONE;
    CHECK(vm_cap(vm, KVM_CAP_VM_DISABLE_NX_HUGE_PAGES, 0) == -EINVAL,
          "nor is KVM_CAP_VM_DISABLE_NX_HUGE_PAGES");

    CHECK(get_msr(fd, 0xCE, &value) == 1 && value == 0,
          "with the feature MSRs left to the caller, PLATFORM_INFO starts at 0");
    CHECK(set_msr(fd, 0x140, 1) == 0, "and CPUID faulting cannot be turned on");
    CHECK(set_msr(fd, 0xCE, 0x80000000u | 0xFF00u) == 1 && get_msr(fd, 0xCE, &value) == 1 &&
              value == 0x8000FF00u,
          "the caller writes PLATFORM_INFO");
    CHECK(dev(KVM_CHECK_EXTENSION, KVM_CAP_MSR_PLATFORM_INFO) == 1 &&
              vm_cap(vm, KVM_CAP_MSR_PLATFORM_INFO, 0) == 0 && k->platform_info_hidden &&
              get_msr(fd, 0xCE, &value) == 1 && value == 0x8000FF00u,
          "KVM_CAP_MSR_PLATFORM_INFO with 0 hides it from the guest, not from the caller");
    CHECK(set_msr(fd, 0x140, 2) == 0 && set_msr(fd, 0x140, 1) == 1 && v->misc_features == 1 &&
              set_msr(fd, 0xCE, 0) == 0,
          "CPUID faulting on: one bit, and PLATFORM_INFO keeps offering it");
    fdclose(fd);
    fdclose(vm);
}

/** The feature MSRs a new vCPU starts with, by
 * KVM_X86_QUIRK_STUFF_FEATURE_MSRS, as Linux's kvm_arch_vcpu_create fills
 * them: PLATFORM_INFO and UCODE_REV. */
static void feature_msr_rows(void)
{
    int vm = dev(KVM_CREATE_VM, 0), fd = fdio(vm, KVM_CREATE_VCPU, 0);
    struct kvm_vcpu *v;
    uint64_t value = 0;

    CHECK(get_msr(fd, 0xCE, &value) == 1 && value == 0x80000000u,
          "a new vCPU's PLATFORM_INFO offers CPUID faulting");
    CHECK(get_msr(fd, 0x8B, &value) == 1 && value == 0x100000000ull,
          "and its UCODE_REV is Linux's 0x100000000 for VMX");
    v = (struct kvm_vcpu *)entries[fd].priv;
    kvm_backend = &kvm_svm;
    kvm_msr_reset(v);
    kvm_backend = &kvm_vmx;
    CHECK(get_msr(fd, 0x8B, &value) == 1 && value == 0x01000065ull,
          "and Linux's 0x01000065 for SVM");
    fdclose(fd);
    fdclose(vm);

    vm = dev(KVM_CREATE_VM, 0);
    CHECK(vm_cap(vm, KVM_CAP_DISABLE_QUIRKS2, KVM_X86_QUIRK_STUFF_FEATURE_MSRS) == 0,
          "the quirk turned off");
    fd = fdio(vm, KVM_CREATE_VCPU, 0);
    v = (struct kvm_vcpu *)entries[fd].priv;
    CHECK(get_msr(fd, 0x8B, &value) == 1 && value == 0,
          "with the feature MSRs left to the caller, UCODE_REV starts at 0");
    CHECK(set_msr(fd, 0x8B, 0x2800000000ull) == 1 && get_msr(fd, 0x8B, &value) == 1 &&
              value == 0x2800000000ull && v->ucode_rev == value,
          "the caller writes UCODE_REV");
    fdclose(fd);
    fdclose(vm);
}

/** SVM's IA32_DEBUGCTL and last-branch MSRs, as the caller reaches them:
 * Linux's SVM without lbrv reads each as 0, takes and ignores any write
 * of DEBUGCTL, and takes only the caller's 0 of the others, whose guest
 * writes are unsupported (arch/x86/kvm/svm/svm.c:2966-2980, 3223-3227 and
 * 3259-3290; x86.c:508-518 and 1590). The vCPU is VMX's fake one, read
 * with the backend SVM's and the PMU started again for it. */
static void svm_debugctl_rows(void)
{
    struct kvm_vcpu *v;
    int vm, fd, r;
    uint64_t value = 1;

    vm = dev(KVM_CREATE_VM, 0);
    fd = fdio(vm, KVM_CREATE_VCPU, 0);
    v = (struct kvm_vcpu *)entries[fd].priv;
    kvm_backend = &kvm_svm;
    kvm_pmu_start();
    CHECK(get_msr(fd, 0x1D9, &value) == 1 && value == 0, "SVM's DEBUGCTL reads 0");
    CHECK(set_msr(fd, 0x1D9, 0x3Full) == 1 && set_msr(fd, 0x1D9, 1ull << 40) == 1,
          "and takes any write, LBR, BTF, bits 5:2 and a reserved bit alike");
    value = 1;
    CHECK(get_msr(fd, 0x1D9, &value) == 1 && value == 0, "which it ignores");
    value = 1;
    CHECK(get_msr(fd, 0x1DB, &value) == 1 && value == 0 && get_msr(fd, 0x1DE, &value) == 1 &&
              value == 0,
          "LASTBRANCHFROMIP and LASTINTTOIP read 0");
    CHECK(set_msr(fd, 0x1DC, 0) == 1 && set_msr(fd, 0x1DD, 0x1000) == 0,
          "and take the caller's 0 alone");
    v->vm->user_space_msr = KVM_MSR_EXIT_REASON_UNKNOWN;
    r = kvm_msr_set(v, 0x1DB, 0, 0);
    CHECK(r == KVM_MSR_UNKNOWN && kvm_msr_user_reason(v, r) == KVM_MSR_EXIT_REASON_UNKNOWN,
          "the guest's write of one, 0 too, is unsupported: an UNKNOWN exit, as x86.c:1590");
    v->vm->user_space_msr = 0;
    kvm_backend = &kvm_vmx;
    kvm_pmu_start();
    fdclose(fd);
    fdclose(vm);
}

/** SYSCALL_MASK's bits 63:32 under SVM, in long mode with EFER.SCE: Linux
 * keeps the caller's value in the VMCB (arch/x86/kvm/svm/svm.c:3177-3178)
 * and lets the guest's WRMSR reach the processor (svm.c:99), which refuses
 * it with #GP, as msr.c refuses it here. The vCPU is VMX's fake one, read
 * with the backend SVM's. */
static void svm_sfmask_rows(void)
{
    struct kvm_vcpu *v;
    int vm, fd;
    uint64_t value = 0;

    vm = dev(KVM_CREATE_VM, 0);
    fd = fdio(vm, KVM_CREATE_VCPU, 0);
    v = (struct kvm_vcpu *)entries[fd].priv;
    kvm_backend = &kvm_svm;
    v->sregs.efer = 0x501u; /* LMA, LME and SCE */
    CHECK(set_msr(fd, 0xC0000084u, 0x100000700ull) == 1 && get_msr(fd, 0xC0000084u, &value) == 1 &&
              value == 0x100000700ull,
          "SVM keeps the caller's SYSCALL_MASK with a bit of 63:32");
    CHECK(kvm_msr_set(v, 0xC0000084u, 0x200000700ull, 0) == KVM_MSR_INVAL &&
              kvm_msr_set(v, 0xC0000084u, 0x700u, 0) == 0,
          "and refuses the guest's, as the processor its WRMSR reaches on Linux");
    kvm_backend = &kvm_vmx;
    fdclose(fd);
    fdclose(vm);
}

/** Whether the model's MSR writes since `model_wrs` was cleared are the
 * one write of `value` to `index`, or none with `index` 0. */
static int wrote(uint32_t index, uint64_t value)
{
    int r = index ? model_wrs == 1 && model_wr[0].index == index && model_wr[0].value == value
                  : model_wrs == 0;

    model_wrs = 0;
    return r;
}

/** SVM's VIRT_SPEC_CTRL on each kind of host, as Linux's svm_set_msr
 * takes it from the caller (arch/x86/kvm/svm/svm.c:3148-3156) and its
 * entry carries it out (svm.c:4418-4424, arch/x86/kernel/cpu/bugs.c:207-242):
 * on a host with no mechanism, taken and skipped, with no MSR written; with
 * SSBD in IA32_SPEC_CTRL, carried in that register; beneath a hypervisor
 * with VIRT_SSBD, in the host's VIRT_SPEC_CTRL; on family 17h, in LS_CFG,
 * set by the first thread of a core and cleared by the last
 * (arch/x86/kernel/process.c:562-599). Then INIT clears SPEC_CTRL and
 * VIRT_SPEC_CTRL, as Linux's svm_vcpu_reset (svm.c:1402-1403). The vCPU is
 * VMX's fake one, read with the backend SVM's. */
static void spec_rows(void)
{
    struct kvm_vcpu *v;
    int vm, fd;
    uint64_t value = 1;

    vm = dev(KVM_CREATE_VM, 0);
    fd = fdio(vm, KVM_CREATE_VCPU, 0);
    v = (struct kvm_vcpu *)entries[fd].priv;
    kvm_backend = &kvm_svm;
    kvm_spec_start();
    model_wrs = 0;
    CHECK(set_msr(fd, 0xC001011Fu, 4) == 1 && get_msr(fd, 0xC001011Fu, &value) == 1 && value == 4,
          "on a host with no mechanism, the caller's VIRT_SPEC_CTRL takes SSBD");
    CHECK(set_msr(fd, 0xC001011Fu, 1) == 0, "and nothing else");
    kvm_virt_ssbd_switch(v, 0, 1);
    kvm_virt_ssbd_switch(v, 0, 0);
    CHECK(kvm_spec_ctrl_guest(v) == 0 && wrote(0, 0) && !kvm_virt_ssbd_offered(),
          "which no entry carries out: SPEC_CTRL without SSBD, no MSR written, none offered");

    model_spec_bits = 7; /* IBRS, STIBP, SSBD */
    kvm_spec_start();
    model_wrs = 0;
    kvm_virt_ssbd_switch(v, 0, 1);
    CHECK(kvm_spec_ctrl_guest(v) == 4 && wrote(0, 0) && kvm_virt_ssbd_offered(),
          "with SSBD in IA32_SPEC_CTRL, the guest's SPEC_CTRL carries it; VIRT_SSBD is offered");

    model_spec_bits = 0;
    model_virt = 1;
    kvm_spec_start();
    model_wrs = 0;
    kvm_virt_ssbd_switch(v, 0, 1);
    CHECK(kvm_spec_ctrl_guest(v) == 0 && wrote(0xC001011Fu, 4),
          "beneath a hypervisor with VIRT_SSBD, the entry sets the host's VIRT_SPEC_CTRL");
    kvm_virt_ssbd_switch(v, 0, 0);
    CHECK(wrote(0xC001011Fu, 0) && !kvm_virt_ssbd_offered(),
          "and the exit clears it; VIRT_SSBD is not offered, as Linux's svm_set_cpu_caps");

    model_virt = 0;
    model_amd17 = 1;
    model_msr_ls = 0x1000;
    kvm_spec_start();
    model_wrs = 0;
    CHECK(kvm_spec_ctrl_guest(v) == 0 && kvm_virt_ssbd_offered(),
          "on family 17h without AMD_SSBD, LS_CFG carries it, and VIRT_SSBD is offered");
    model_apic = 0;
    kvm_virt_ssbd_switch(v, 0, 1);
    CHECK(wrote(0xC0011020u, 0x1400), "the first thread of a core to enter sets LS_CFG's bit 10");
    model_apic = 1;
    kvm_virt_ssbd_switch(v, 1, 1);
    CHECK(wrote(0, 0), "its sibling's entry writes nothing");
    model_apic = 0;
    kvm_virt_ssbd_switch(v, 0, 0);
    CHECK(wrote(0, 0), "nor the first one's exit, while the sibling's guest runs");
    model_apic = 1;
    kvm_virt_ssbd_switch(v, 1, 0);
    CHECK(wrote(0xC0011020u, 0x1000), "the last thread's exit puts the host's LS_CFG back");
    model_apic = 2;
    kvm_virt_ssbd_switch(v, 2, 1);
    CHECK(wrote(0xC0011020u, 0x1400), "and another core's entry sets its own");
    kvm_virt_ssbd_switch(v, 2, 0);
    CHECK(wrote(0xC0011020u, 0x1000), "and its exit clears it");
    model_amd17 = 0;

    model_spec_bits = 7;
    kvm_spec_start();
    model_wrs = 0;
    CHECK(set_msr(fd, 0x48, 5) == 1 && set_msr(fd, 0xC001011Fu, 4) == 1,
          "SPEC_CTRL and VIRT_SPEC_CTRL set");
    kvm_vcpu_init_reset(v);
    value = 1;
    CHECK(get_msr(fd, 0x48, &value) == 1 && value == 0 && get_msr(fd, 0xC001011Fu, &value) == 1 &&
              value == 0 && kvm_spec_ctrl_guest(v) == 0,
          "INIT clears both");
    model_spec_bits = 0;
    model_msr_spec = 0;
    kvm_spec_start();
    model_wrs = 0;
    kvm_backend = &kvm_vmx;
    fdclose(fd);
    fdclose(vm);
}

/** /dev/kvm's attribute KVM_X86_XCOMP_GUEST_SUPP, as amx_test reads it,
 * and the debugging capabilities. */
static void attr_rows(void)
{
    struct kvm_device_attr attr;
    uint64_t value = 0;

    k_memset(&attr, 0, sizeof(attr));
    attr.attr = KVM_X86_XCOMP_GUEST_SUPP;
    attr.addr = (uint64_t)(uintptr_t)&value;
    CHECK(dev(KVM_CHECK_EXTENSION, KVM_CAP_SYS_ATTRIBUTES) == 1 &&
              dev(KVM_HAS_DEVICE_ATTR, (uintptr_t)&attr) == 0 &&
              dev(KVM_GET_DEVICE_ATTR, (uintptr_t)&attr) == 0 && value == kvm_xcr0_supported() &&
              !(value & (3ull << 17)),
          "KVM_X86_XCOMP_GUEST_SUPP is the XCR0 a guest may have, with no AMX in it");
    attr.attr = 1;
    CHECK(dev(KVM_HAS_DEVICE_ATTR, (uintptr_t)&attr) == -ENXIO &&
              dev(KVM_GET_DEVICE_ATTR, (uintptr_t)&attr) == -ENXIO,
          "another attribute of /dev/kvm is ENXIO");
    CHECK(dev(KVM_CHECK_EXTENSION, KVM_CAP_SET_GUEST_DEBUG) == 1 &&
              dev(KVM_CHECK_EXTENSION, KVM_CAP_SET_GUEST_DEBUG2) ==
                  (int32_t)KVM_GUESTDBG_VALID_MASK,
          "KVM_CAP_SET_GUEST_DEBUG, and the flags KVM_SET_GUEST_DEBUG takes");
}

/** KVM_CAP_SYNC_REGS: the register sets KVM_RUN takes from the run page
 * and writes to it, as sync_regs_test drives them. */
static void sync_rows(void)
{
    int vm = dev(KVM_CREATE_VM, 0), fd = fdio(vm, KVM_CREATE_VCPU, 0);
    struct kvm_vcpu *v = (struct kvm_vcpu *)entries[fd].priv;
    struct kvm_run *run = run_of(fd);
    struct kvm_vcpu_events events;
    struct kvm_sregs sregs;
    struct kvm_regs regs;
    int i;

    CHECK(dev(KVM_CHECK_EXTENSION, KVM_CAP_SYNC_REGS) == 7 && offsetof(struct kvm_run, s) == 304 &&
              sizeof(struct kvm_sync_regs) == 520 && offsetof(struct kvm_sync_regs, sregs) == 144 &&
              offsetof(struct kvm_sync_regs, events) == 456,
          "KVM_CAP_SYNC_REGS names the three sets, laid out as Linux's kvm_sync_regs");
    refusing = 0;
    script_len = 0;
    script_at = 0;
    for (i = 0; i < 6; i++) {
        scripted(KVM_X_HLT);
    }
    entries_made = 0;
    run->kvm_valid_regs = 8;
    CHECK(fdio(fd, KVM_RUN, 0) == -EINVAL && entries_made == 0,
          "a set Linux does not name to write");
    run->kvm_valid_regs = 0;
    run->kvm_dirty_regs = 8 | KVM_SYNC_X86_REGS;
    CHECK(fdio(fd, KVM_RUN, 0) == -EINVAL && entries_made == 0, "or to take");
    run->kvm_dirty_regs = 0;

    v->regs.rbx = 0x1111;
    v->events.interrupt.injected = 1;
    v->events.interrupt.nr = 0x41;
    run->kvm_valid_regs = KVM_SYNC_X86_VALID_FIELDS;
    CHECK(fdio(fd, KVM_RUN, 0) == 0 && run->exit_reason == KVM_EXIT_HLT,
          "a run with the sets asked");
    fdio(fd, KVM_GET_REGS, (uintptr_t)&regs);
    fdio(fd, KVM_GET_SREGS, (uintptr_t)&sregs);
    fdio(fd, KVM_GET_VCPU_EVENTS, (uintptr_t)&events);
    CHECK(bytes_equal(&run->s.regs.regs, &regs, sizeof(regs)) && regs.rbx == 0x1111,
          "the run page has the registers as KVM_GET_REGS gives them");
    CHECK(bytes_equal(&run->s.regs.sregs, &sregs, sizeof(sregs)) && sregs.interrupt_bitmap[1] == 2,
          "the special registers as KVM_GET_SREGS, the interrupt waiting in the bitmap");
    CHECK(bytes_equal(&run->s.regs.events, &events, sizeof(events)),
          "and the events as KVM_GET_VCPU_EVENTS");

    run->s.regs.regs.rbx = 0x2222;
    run->s.regs.sregs.cr4 = 1ull << 13;
    run->kvm_dirty_regs = KVM_SYNC_X86_REGS | KVM_SYNC_X86_SREGS;
    entries_made = 0;
    CHECK(fdio(fd, KVM_RUN, 0) == -EINVAL && entries_made == 0 && v->regs.rbx == 0x2222 &&
              run->kvm_dirty_regs == KVM_SYNC_X86_SREGS,
          "special registers Linux refuses end the run, the set still marked, the registers taken");
    run->s.regs.sregs.cr4 = 0;
    run->s.regs.sregs.cr3 = 0x5000;
    run->s.regs.events.interrupt.injected = 0;
    run->s.regs.events.flags = 0;
    run->kvm_dirty_regs |= KVM_SYNC_X86_EVENTS;
    CHECK(fdio(fd, KVM_RUN, 0) == 0 && run->kvm_dirty_regs == 0 && v->sregs.cr3 == 0x5000 &&
              !v->events.interrupt.injected && run->s.regs.sregs.cr3 == 0x5000,
          "the sets marked are taken before the entry and their marks cleared");
    run->s.regs.events.flags = 0x80;
    run->kvm_dirty_regs = KVM_SYNC_X86_EVENTS;
    CHECK(fdio(fd, KVM_RUN, 0) == -EINVAL, "events Linux refuses end the run");
    run->kvm_dirty_regs = 0;

    run->kvm_valid_regs = 0;
    run->s.regs.regs.rbx = 0x3333;
    v->regs.rbx = 0x4444;
    CHECK(fdio(fd, KVM_RUN, 0) == 0 && run->s.regs.regs.rbx == 0x3333 && v->regs.rbx == 0x4444,
          "with no set asked the run page and the vCPU keep their own");
    run->kvm_dirty_regs = KVM_SYNC_X86_REGS;
    run->immediate_exit = 1;
    CHECK(fdio(fd, KVM_RUN, 0) == -EINTR && v->regs.rbx == 0x3333,
          "a set is taken even under immediate_exit");
    run->immediate_exit = 0;
    refusing = 1;
    fdclose(fd);
    fdclose(vm);
    CHECK(live == 0, "the VM frees every allocation");
}

/** KVM_PRE_FAULT_MEMORY through a vCPU's descriptor: the range moves by
 * what the page tables mapped, whatever the result. tdp.c's own rows are in
 * kvm_slots_hosttest. */
static void pre_fault_rows(void)
{
    int vm = dev(KVM_CREATE_VM, 0), vcpu = fdio(vm, KVM_CREATE_VCPU, 0);
    struct kvm_pre_fault_memory range;

    CHECK(sizeof(range) == 64 && KVM_PRE_FAULT_MEMORY == 0xC040AED5u,
          "kvm_pre_fault_memory and KVM_PRE_FAULT_MEMORY are Linux's");
    CHECK(dev(KVM_CHECK_EXTENSION, KVM_CAP_PRE_FAULT_MEMORY) == 1 &&
              fdio(vm, KVM_CHECK_EXTENSION, KVM_CAP_PRE_FAULT_MEMORY) == 1,
          "KVM_CAP_PRE_FAULT_MEMORY");
    k_memset(&range, 0, sizeof(range));
    range.gpa = 0x200000;
    range.size = 0x3000;
    pre_fault_pages = 2;
    CHECK(fdio(vcpu, KVM_PRE_FAULT_MEMORY, (uintptr_t)&range) == 0 && range.gpa == 0x202000 &&
              range.size == 0x1000,
          "a range mapped in part is 0, moved past what was mapped");
    pre_fault_pages = 0;
    CHECK(fdio(vcpu, KVM_PRE_FAULT_MEMORY, (uintptr_t)&range) == -ENOENT && range.gpa == 0x202000 &&
              range.size == 0x1000,
          "and -ENOENT, unmoved, with nothing mapped");
    CHECK(fdio(vcpu, KVM_PRE_FAULT_MEMORY, 0) == -EFAULT, "a bad range is -EFAULT");
    CHECK(fdio(vm, KVM_PRE_FAULT_MEMORY, (uintptr_t)&range) == -ENOTTY, "it is no ioctl of a VM");
    fdclose(vcpu);
    fdclose(vm);
}

/* The description a statistics descriptor is read through. */
static struct open_file stats_ofd;

/** Read `count` bytes of the statistics descriptor `fd` at `pos`, as
 * pread64 does. */
static int32_t stats_at(int fd, void *buf, uint32_t count, uint64_t pos)
{
    stats_ofd.offset = pos;
    entries[fd].ofd = &stats_ofd;
    return entries[fd].ops->read(&entries[fd], (char *)buf, count);
}

/** The first value of the statistic `name` in the file on `fd`, or all
 * ones when the file has no such descriptor. */
static uint64_t stat_of(int fd, const char *name)
{
    struct kvm_stats_header h;
    struct kvm_stats_desc d;
    uint64_t value = ~0ull;
    uint32_t i;

    stats_at(fd, &h, sizeof(h), 0);
    for (i = 0; i < h.num_desc; i++) {
        stats_at(fd, &d, sizeof(d), h.desc_offset + i * sizeof(d));
        if (strcmp(d.name, name) == 0) {
            stats_at(fd, &value, 8, h.data_offset + d.offset);
        }
    }
    return value;
}

/** Whether the `ndesc` descriptors at `descs` are ones
 * kvm_binary_stats_test takes: names that end, a known type and unit to
 * base 10, the exponent their unit has, no bucket size, and `nvalues`
 * values in all that lie in the data. */
static int stats_descs_ok(const uint8_t *descs, uint32_t ndesc, uint32_t nvalues)
{
    struct kvm_stats_desc d;
    uint32_t i, next = 0;
    int ok = 1;

    for (i = 0; ok && i < ndesc; i++) {
        uint32_t unit;

        k_memcpy(&d, descs + i * 64u, sizeof(d));
        unit = d.flags & 0xF0u;
        ok = d.name[0] != '\0' && d.name[47] == '\0' && (d.flags & 0xFu) <= 4u && unit <= 0x40u &&
             (d.flags >> 8) == 0 && d.exponent == (unit == KVM_STATS_UNIT_SECONDS ? -9 : 0) &&
             d.size != 0 && d.bucket_size == 0 && d.offset % 8u == 0 &&
             d.offset / 8u + d.size <= nvalues;
        next += d.size;
    }
    return ok && next == nvalues;
}

/** Whether the file on `fd` is what kvm_binary_stats_test requires: Linux's
 * header for `ndesc` descriptors, the id `id`, descriptors whose names end,
 * whose exponents fit their units and whose `nvalues` values lie in the
 * data, and the same bytes whether read whole, one at a time or
 * across its parts, with nothing at its end. */
static int stats_file_ok(int fd, const char *id, uint32_t ndesc, uint32_t nvalues)
{
    static uint8_t whole[8192];
    struct kvm_stats_header h;
    uint8_t piece[200];
    uint32_t size = 72u + ndesc * 64u + nvalues * 8u, pos;
    int ok;

    ok = stats_at(fd, &h, sizeof(h), 0) == (int32_t)sizeof(h) && h.flags == 0 &&
         h.name_size == 48 && h.num_desc == ndesc && h.id_offset == 24 && h.desc_offset == 72 &&
         h.data_offset == 72u + ndesc * 64u;
    ok = ok && stats_at(fd, whole, sizeof(whole), 0) == (int32_t)size &&
         strcmp((const char *)whole + 24, id) == 0;
    for (pos = 0; ok && pos < size; pos++) {
        ok = stats_at(fd, piece, 1, pos) == 1 && piece[0] == whole[pos];
    }
    for (pos = 0; ok && pos + sizeof(piece) <= size; pos += 97u) {
        ok = stats_at(fd, piece, sizeof(piece), pos) == (int32_t)sizeof(piece) &&
             bytes_equal(piece, whole + pos, sizeof(piece));
    }
    ok = ok && stats_at(fd, piece, 16, size - 4u) == 4 && stats_at(fd, piece, 1, size) == 0 &&
         stats_at(fd, piece, 1, 1ull << 40) == 0;
    return ok && stats_descs_ok(whole + 72, ndesc, nvalues);
}

/** What the vCPU on `vcpu` counts as it runs, read from its statistics
 * file on `vcpu_stats`. */
static void stats_count_rows(int vcpu, int vcpu_stats)
{
    struct kvm_vcpu *v = (struct kvm_vcpu *)entries[vcpu].priv;

    refusing = 0;
    script_len = 0;
    script_at = 0;
    scripted(KVM_X_AGAIN);
    scripted_io(KVM_EXIT_IO_OUT, 4, BUS_PORT, 1);
    scripted_io(KVM_EXIT_IO_OUT, 1, 0x3F8, 1);
    scripted(KVM_X_MMIO)->mmio.len = 4;
    scripted(KVM_X_HLT);
    CHECK(fdio(vcpu, KVM_RUN, 0) == 0 && stat_of(vcpu_stats, "exits") == 3 &&
              stat_of(vcpu_stats, "io_exits") == 2 && stat_of(vcpu_stats, "mmio_exits") == 0,
          "exits counts every exit, io_exits the bus's too");
    CHECK(fdio(vcpu, KVM_RUN, 0) == 0 && run_of(vcpu)->exit_reason == KVM_EXIT_MMIO &&
              stat_of(vcpu_stats, "mmio_exits") == 1,
          "mmio_exits counts the MMIO the caller answers");
    CHECK(fdio(vcpu, KVM_RUN, 0) == 0 && run_of(vcpu)->exit_reason == KVM_EXIT_HLT &&
              stat_of(vcpu_stats, "halt_exits") == 1 && stat_of(vcpu_stats, "exits") == 5,
          "halt_exits counts the HLT");
    sig_pending = 1;
    caller.sig_woken = 1;
    CHECK(fdio(vcpu, KVM_RUN, 0) == -EINTR && stat_of(vcpu_stats, "signal_exits") == 1,
          "signal_exits counts a run a signal ended");
    sig_pending = 0;
    caller.sig_woken = 0;
    v->stat[KVM_VCPU_STAT_HALT_WAIT_HIST + 31] = 9;
    CHECK(stat_of(vcpu_stats, "blocking") == 0 && v->stat[KVM_VCPU_STAT_BLOCKING] == 0,
          "a histogram's 32 values end before the next statistic");
    v->stat[KVM_VCPU_STAT_HALT_WAIT_HIST + 31] = 0;
}

/** The guest's TSC of the vCPU `fd`, as KVM_GET_MSRS reads it. */
static uint64_t guest_tsc(int fd)
{
    uint64_t value = 0;

    (void)get_msr(fd, MSR_IA32_TSC, &value);
    return value;
}

/** KVM_SET_TSC_KHZ on a processor that scales no TSC, the model's at 2
 * GHz: a rate within 250 parts in a million is the host's, a lower one
 * is refused, and a higher one is kept by moving the TSC up before each
 * entry, from the count and the moment its generation began at. */
static void tsc_unscaled_rows(void)
{
    int vm = dev(KVM_CREATE_VM, 0), fd;
    struct kvm_vcpu *v;

    CHECK(fdio(vm, KVM_SET_TSC_KHZ, 1000000) == 0 && fdio(vm, KVM_GET_TSC_KHZ, 0) == 1000000,
          "the VM's KVM_SET_TSC_KHZ takes a rate no vCPU could have, as Linux's");
    CHECK(fdio(vm, KVM_SET_TSC_KHZ, 0) == 0 && fdio(vm, KVM_GET_TSC_KHZ, 0) == 2000000,
          "and the host's for 0");
    fd = fdio(vm, KVM_CREATE_VCPU, 0);
    v = entries[fd].priv;
    CHECK(fdio(vm, KVM_SET_TSC_KHZ, 1000000) == 0 && fdio(vm, KVM_GET_TSC_KHZ, 0) == 1000000 &&
              fdio(fd, KVM_GET_TSC_KHZ, 0) == 2000000,
          "and one once a vCPU exists, for later vCPUs only, as Linux's x86.c takes it");
    CHECK(fdio(vm, KVM_SET_TSC_KHZ, 0) == 0, "and the host's again");
    CHECK(dev(KVM_CHECK_EXTENSION, KVM_CAP_TSC_CONTROL) == 0 &&
              fdio(vm, KVM_CHECK_EXTENSION, KVM_CAP_VM_TSC_CONTROL) == 0,
          "no KVM_CAP_TSC_CONTROL and no KVM_CAP_VM_TSC_CONTROL without scaling");
    CHECK(fdio(vm, KVM_GET_TSC_KHZ, 0) == 2000000 && fdio(fd, KVM_GET_TSC_KHZ, 0) == 2000000,
          "KVM_GET_TSC_KHZ is the host's rate, of the VM and of a vCPU");
    CHECK(fdio(fd, KVM_SET_TSC_KHZ, 2000500) == 0 && fdio(fd, KVM_SET_TSC_KHZ, 1999500) == 0 &&
              !v->tsc_catchup && v->tsc_ratio == 0 && fdio(fd, KVM_GET_TSC_KHZ, 0) == 1999500,
          "a rate within 250 parts in a million is taken, and runs at the host's");
    CHECK(fdio(fd, KVM_SET_TSC_KHZ, 1999499) == -EINVAL && fdio(fd, KVM_GET_TSC_KHZ, 0) == 1999499,
          "a lower rate is -EINVAL, and is recorded all the same, as Linux records it");
    CHECK(fdio(fd, KVM_SET_TSC_KHZ, 0) == 0 && fdio(fd, KVM_GET_TSC_KHZ, 0) == 2000000,
          "0 kHz is the host's rate");
    clock_pass(1000000);
    CHECK(guest_tsc(fd) == 2000000, "the TSC counts at the host's rate");
    CHECK(fdio(fd, KVM_SET_TSC_KHZ, 3000000) == 0 && v->tsc_catchup && v->tsc_ratio == 0,
          "a higher rate is taken");
    kvm_clock_prepare(v);
    CHECK(guest_tsc(fd) == 3000000 && (v->dirty & KVM_PART_TSC),
          "and the TSC is moved up to it before an entry");
    clock_pass(1000000);
    CHECK(guest_tsc(fd) == 5000000, "between entries it counts at the host's rate");
    kvm_clock_prepare(v);
    CHECK(guest_tsc(fd) == 6000000, "and is moved up again");
    fdclose(fd);
    fdclose(vm);
}

/** KVM_SET_TSC_KHZ on a processor that scales the TSC by a ratio with
 * `bits` of fraction, whose 2,345,678 kHz, tsc_scaling_sync's rate, is
 * the ratio `ratio`, and whose rates end below `past`: the capabilities,
 * Linux's bound on the rate, the VM's rate given to its new vCPUs, and
 * the TSC counting at it. */
static void tsc_scaled_rows(uint32_t bits, uint64_t ratio, uint32_t past)
{
    int vm, fd0, fd1;
    struct kvm_vcpu *v0, *v1;
    uint64_t t0;

    scale_bits = bits;
    vm = dev(KVM_CREATE_VM, 0);
    CHECK(dev(KVM_CHECK_EXTENSION, KVM_CAP_TSC_CONTROL) == 1 &&
              fdio(vm, KVM_CHECK_EXTENSION, KVM_CAP_VM_TSC_CONTROL) == 1,
          "KVM_CAP_TSC_CONTROL and KVM_CAP_VM_TSC_CONTROL with scaling");
    CHECK(fdio(vm, KVM_SET_TSC_KHZ, past) == -EINVAL && fdio(vm, KVM_SET_TSC_KHZ, past - 1u) == 0,
          "the VM's rate is below what the largest ratio gives, at most 2^31 - 1");
    CHECK(fdio(vm, KVM_SET_TSC_KHZ, 2345678) == 0 && fdio(vm, KVM_GET_TSC_KHZ, 0) == 2345678,
          "the VM's KVM_SET_TSC_KHZ is read back");
    fd0 = fdio(vm, KVM_CREATE_VCPU, 0);
    v0 = entries[fd0].priv;
    CHECK(fdio(fd0, KVM_GET_TSC_KHZ, 0) == 2345678 && v0->tsc_ratio == ratio &&
              v0->tsc_frac_bits == bits && kvm_tsc_hz(v0) == 2345678000ull,
          "a new vCPU has the VM's rate, and the ratio the processor is given");
    t0 = guest_tsc(fd0);
    clock_pass(1000000);
    fd1 = fdio(vm, KVM_CREATE_VCPU, 1);
    v1 = entries[fd1].priv;
    CHECK(guest_tsc(fd0) - t0 >= 2345677 && guest_tsc(fd0) - t0 <= 2345678,
          "its TSC counts at that rate");
    CHECK(v1->tsc_offset == v0->tsc_offset && v1->tsc_ratio == ratio,
          "a second vCPU takes the same TSC");
    CHECK(fdio(fd1, KVM_SET_TSC_KHZ, past) == -EINVAL && fdio(fd1, KVM_GET_TSC_KHZ, 0) == 2345678,
          "a vCPU's rate has the same bound, and one past it changes nothing");
    v1->dirty = 0;
    CHECK(fdio(fd1, KVM_SET_TSC_KHZ, 1000000) == 0 && v1->tsc_ratio == 1ull << (bits - 1u) &&
              (v1->dirty & KVM_PART_TSC) && !v1->tsc_catchup,
          "a rate below the host's is a ratio below 1, loaded before the next entry");
    CHECK(fdio(fd1, KVM_SET_TSC_KHZ, 2000100) == 0 && v1->tsc_ratio == 0 &&
              fdio(fd1, KVM_GET_TSC_KHZ, 0) == 2000100,
          "a rate within the tolerance is not scaled");
    CHECK(fdio(fd1, KVM_SET_TSC_KHZ, 0) == 0 && fdio(fd1, KVM_GET_TSC_KHZ, 0) == 2000000,
          "0 kHz is the host's rate");
    fdclose(fd1);
    fdclose(fd0);
    fdclose(vm);
    scale_bits = 0;
}

/** KVM_SET_TSC_KHZ and KVM_GET_TSC_KHZ of a VM and of a vCPU, with
 * KVM_CAP_TSC_CONTROL and KVM_CAP_VM_TSC_CONTROL, as tsc_scaling_sync
 * and Linux's kvm_set_tsc_khz have them: without scaling, with SVM's
 * ratio and with VMX's. */
static void tsc_rows(void)
{
    tick_counts = 2000000000u / TICK_HZ;
    vclock_on = 1;
    tsc_unscaled_rows();
    tsc_scaled_rows(32, 0x12c3f2d3cull, 511999999u);
    tsc_scaled_rows(48, 0x12c3f2d3c792dull, 0x7FFFFFFFu);
    CHECK(live == 0, "the VMs free every allocation");
    tick_counts = 0;
    vclock_on = 0;
}

/** KVM_GET_STATS_FD of a VM and of a vCPU, as kvm_binary_stats_test reads
 * them: several files at once, read after the VM's own descriptors closed. */
static void stats_rows(void)
{
    int before = live, vm, vcpu, vm_stats, vm_again, vcpu_stats;
    uint8_t byte[10];

    caller.pid = 321;
    vm = dev(KVM_CREATE_VM, 0);
    vcpu = fdio(vm, KVM_CREATE_VCPU, 7);
    CHECK(sizeof(struct kvm_stats_header) == 24 &&
              __builtin_offsetof(struct kvm_stats_desc, name) == 16 &&
              sizeof(struct kvm_stats_desc) == 16 + KVM_STATS_NAME_SIZE &&
              KVM_GET_STATS_FD == 0xAECE,
          "the header and KVM_GET_STATS_FD are Linux's, and a descriptor is Linux's 16 bytes "
          "and a name of 48, 64 bytes of the file");
    CHECK(dev(KVM_CHECK_EXTENSION, KVM_CAP_BINARY_STATS_FD) == 1 &&
              fdio(vm, KVM_CHECK_EXTENSION, KVM_CAP_BINARY_STATS_FD) == 1,
          "KVM_CAP_BINARY_STATS_FD");
    vm_stats = fdio(vm, KVM_GET_STATS_FD, 0);
    vm_again = fdio(vm, KVM_GET_STATS_FD, 0);
    vcpu_stats = fdio(vcpu, KVM_GET_STATS_FD, 0);
    CHECK(vm_stats > vcpu && vm_again > vm_stats && vcpu_stats > vm_again &&
              entries[vm_stats].type == FD_TYPE_ANON &&
              entries[vm_stats].flags == (FD_FLAG_READ | FD_FLAG_CLOEXEC),
          "a descriptor each, read-only and close-on-exec");
    CHECK(strcmp(((struct anon_obj *)entries[vm_stats].priv)->name, "kvm-vm-stats") == 0 &&
              strcmp(((struct anon_obj *)entries[vcpu_stats].priv)->name, "kvm-vcpu-stats:7") == 0,
          "anon_inode:kvm-vm-stats and anon_inode:kvm-vcpu-stats:ID");
    CHECK(stats_file_ok(vm_stats, "kvm-321", 15, KVM_VM_STATS) && KVM_VM_STATS == 15,
          "the VM's file: Linux's 15 statistics, read at any offset");
    CHECK(stats_file_ok(vcpu_stats, "kvm-321/vcpu-7", 45, KVM_VCPU_STATS) && KVM_VCPU_STATS == 138,
          "the vCPU's file: Linux's 45 statistics in 138 values");
    CHECK(stat_of(vm_stats, "pages_4k") == 0 && stat_of(vcpu_stats, "exits") == 0 &&
              stat_of(vcpu_stats, "no_such") == ~0ull,
          "a statistic is found by its name");
    stats_ofd.offset = 0;
    entries[vm_stats].ofd = &stats_ofd;
    CHECK(entries[vm_stats].ops->read(&entries[vm_stats], (char *)byte, 10) == 10 &&
              stats_ofd.offset == 10 &&
              entries[vm_stats].ops->llseek(&entries[vm_stats], 0, 0) == 10,
          "a read moves the offset and a seek leaves it, as Linux's noop_llseek");
    CHECK(stats_at(vm_stats, NULL, 8, 0) == -EFAULT, "a bad buffer is -EFAULT");

    stats_count_rows(vcpu, vcpu_stats);

    fdclose(vcpu);
    fdclose(vm);
    CHECK(live > before && stats_file_ok(vm_stats, "kvm-321", 15, KVM_VM_STATS) &&
              stats_file_ok(vcpu_stats, "kvm-321/vcpu-7", 45, KVM_VCPU_STATS) &&
              stat_of(vcpu_stats, "exits") == 5,
          "the files hold the VM after its own descriptors close");
    fdclose(vm_stats);
    fdclose(vcpu_stats);
    CHECK(live > before, "each file holds a reference");
    fdclose(vm_again);
    CHECK(live == before, "the last file's close frees the VM");
}

/** The rest of Linux's machine-check MSRs on the vCPU `fd`, `v`, with ten
 * banks and MCG_CTL_P: the guest's and the caller's write of MCG_CAP,
 * which Linux has no case for; P5_MC_ADDR and P5_MC_TYPE; and a bank's
 * CTL2 without MCG_CMCI_P, as Linux's get_msr_mce and set_msr_mce. */
static void mce_more_rows(struct kvm_vcpu *v, int fd)
{
    uint64_t value;

    CHECK(kvm_msr_set(v, 0x179, MCG_CTL_P | 2, 0) == KVM_MSR_UNKNOWN &&
              v->mcg_cap == (MCG_CTL_P | 10),
          "the guest's write of MCG_CAP is an unknown MSR's");
    CHECK(kvm_msr_set(v, 0x179, MCG_CTL_P | 2, 1) == KVM_MSR_UNKNOWN &&
              kvm_msr_set(v, 0x179, 0, 1) == KVM_MSR_UNKNOWN && set_msr(fd, 0x179, 0) == 0 &&
              v->mcg_cap == (MCG_CTL_P | 10),
          "the caller's write of MCG_CAP, 0 too, is an unknown MSR's");
    value = 1;
    CHECK(kvm_msr_get(v, 0, &value, 0) == 0 && value == 0 && kvm_msr_get(v, 1, &value, 1) == 0 &&
              value == 0 && kvm_msr_set(v, 0, 0, 0) == KVM_MSR_UNKNOWN,
          "P5_MC_ADDR and P5_MC_TYPE read 0 and take no write");
    value = 1;
    CHECK(kvm_msr_get(v, 0x289, &value, 1) == 0 && value == 0 && set_msr(fd, 0x289, 0) == 1 &&
              set_msr(fd, 0x289, 1) == 0 && kvm_msr_get(v, 0x289, &value, 0) == KVM_MSR_INVAL &&
              kvm_msr_set(v, 0x289, 0, 0) == KVM_MSR_INVAL &&
              kvm_msr_get(v, 0x280 + 10, &value, 1) == KVM_MSR_INVAL,
          "a bank's CTL2 without MCG_CMCI_P is the caller's 0 and the guest's #GP, and past the "
          "count is refused");
}

/**
 * The machine-check MSRs, HWCR and POWER_CTL as the caller and the guest
 * reach them, by Linux's host_initiated: MCG_CTL without MCG_CTL_P, which
 * state_test saves and restores; each bit of HWCR on an Intel model, as
 * hwcr_msr_test sets it; and a bank's STATUS, which the guest writes
 * nonzero only on AMD with McStatusWrEn.
 */
static void mce_hwcr_rows(void)
{
    const uint64_t legal = (1ull << 3) | (1ull << 6) | (1ull << 8) | (1ull << 18) | (1ull << 24);
    const uint64_t valid = (1ull << 18) | (1ull << 24);
    int vm = dev(KVM_CREATE_VM, 0), fd = fdio(vm, KVM_CREATE_VCPU, 0);
    struct kvm_vcpu *v = (struct kvm_vcpu *)entries[fd].priv;
    uint64_t value = 1, val;
    uint32_t bit;
    int32_t r;
    int ok = 1;

    CHECK(!(v->mcg_cap & MCG_CTL_P) && get_msr(fd, 0x17B, &value) == 1 && value == 0,
          "the caller reads MCG_CTL without MCG_CTL_P, as state_test saves it");
    CHECK(set_msr(fd, 0x17B, 0) == 1 && set_msr(fd, 0x17B, ~0ull) == 0,
          "and writes 0 to it, but not all ones");
    CHECK(kvm_msr_get(v, 0x17B, &value, 0) == KVM_MSR_INVAL &&
              kvm_msr_set(v, 0x17B, 0, 0) == KVM_MSR_INVAL,
          "the guest neither reads nor writes it");
    for (bit = 0; bit < 64; bit++) {
        val = 1ull << bit;
        r = set_msr(fd, 0xC0010015u, val);
        ok &= (val & ~legal) ? r == 0 : r == 1;
        ok &= get_msr(fd, 0xC0010015u, &value) == 1 && value == (val & valid);
        ok &= set_msr(fd, 0xC0010015u, 0) == 1;
    }
    CHECK(ok, "HWCR keeps McStatusWrEn and TscFreqSel, drops three bits and refuses the rest");
    CHECK(set_msr(fd, 0x1FC, 0x1234) == 1 && get_msr(fd, 0x1FC, &value) == 1 && value == 0x1234 &&
              kvm_msr_get(v, 0x1FC, &value, 0) == 0 && value == 0x1234,
          "POWER_CTL keeps what is written, and the guest reads it");

    value = MCG_CTL_P | 10;
    CHECK(fdio(fd, KVM_X86_SETUP_MCE, (uintptr_t)&value) == 0, "KVM_X86_SETUP_MCE, 10 banks");
    CHECK(kvm_msr_set(v, 0x401, 5, 0) == KVM_MSR_INVAL && kvm_msr_set(v, 0x401, 0, 0) == 0 &&
              set_msr(fd, 0x401, 5) == 1,
          "the guest only clears a bank's STATUS, which the caller writes as it likes");
    CHECK(set_msr(fd, 0xC0010015u, 1ull << 18) == 1 && kvm_msr_set(v, 0x401, 5, 0) == KVM_MSR_INVAL,
          "McStatusWrEn does not let an Intel guest write it");
    k_memset(CPUID_ENTRIES, 0, sizeof(*CPUID_ENTRIES));
    CPUID_ENTRIES[0].eax = 0xD;
    CPUID_ENTRIES[0].ebx = 0x68747541u; /* "AuthenticAMD" */
    CPUID_ENTRIES[0].edx = 0x69746E65u;
    CPUID_ENTRIES[0].ecx = 0x444D4163u;
    buf.cpuid.nent = 1;
    CHECK(fdio(fd, KVM_SET_CPUID2, (uintptr_t)&buf) == 0 && kvm_msr_set(v, 0x401, 6, 0) == 0 &&
              v->mce_banks[1] == 6,
          "an AMD guest with McStatusWrEn does, as Linux's can_set_mci_status");
    CHECK(kvm_msr_get(v, 0x400 + 4 * 10, &value, 1) == KVM_MSR_INVAL &&
              kvm_msr_get(v, 0x400 + 4 * 32, &value, 1) == KVM_MSR_UNKNOWN,
          "a bank past MCG_CAP's count is refused, one past KVM's 32 unknown");
    mce_more_rows(v, fd);
    fdclose(fd);
    fdclose(vm);
}

/* The bitmaps of user_msr_rows' filter: reads of 0 to 0x2FFF but 0x10,
 * writes of them but POWER_CTL, and one MSR at 0xDEADBEEF. */
static uint8_t read_bits[KVM_MSR_FILTER_MAX_BITMAP_SIZE],
    write_bits[KVM_MSR_FILTER_MAX_BITMAP_SIZE];
static uint64_t one_bit = 1;

/** KVM_X86_SET_MSR_FILTER of `f` on the VM `vm`. */
static int32_t set_filter(int vm, struct kvm_msr_filter *f)
{
    return fdio(vm, KVM_X86_SET_MSR_FILTER, (uintptr_t)f);
}

/** KVM_CAP_X86_USER_SPACE_MSR's and KVM_X86_SET_MSR_FILTER's flags, as
 * userspace_msr_exit_test's user_exit_msr_flags tries each bit. */
static void user_msr_flag_rows(int vm)
{
    struct kvm_msr_filter f;
    uint64_t deny = 0;
    uint32_t i;
    int32_t r;
    int ok = 1;

    CHECK(dev(KVM_CHECK_EXTENSION, KVM_CAP_X86_USER_SPACE_MSR) == 1 &&
              dev(KVM_CHECK_EXTENSION, KVM_CAP_X86_MSR_FILTER) == 1,
          "KVM_CAP_X86_USER_SPACE_MSR and KVM_CAP_X86_MSR_FILTER");
    for (i = 0; i < 64; i++) {
        r = vm_cap(vm, KVM_CAP_X86_USER_SPACE_MSR, 1ull << i);
        ok &= (1ull << i) & KVM_MSR_EXIT_REASON_VALID_MASK ? r == 0 : r == -EINVAL;
    }
    CHECK(ok, "KVM_CAP_X86_USER_SPACE_MSR takes INVAL, UNKNOWN and FILTER alone");
    k_memset(&f, 0, sizeof(f));
    f.ranges[0].flags = KVM_MSR_FILTER_READ;
    f.ranges[0].nmsrs = 1;
    f.ranges[0].bitmap = (uintptr_t)&deny;
    for (i = 0; i < 32; i++) {
        f.flags = 1u << i;
        r = set_filter(vm, &f);
        ok &= (1u << i) & KVM_MSR_FILTER_VALID_MASK ? r == 0 : r == -EINVAL;
    }
    f.flags = KVM_MSR_FILTER_DEFAULT_ALLOW;
    for (i = 0; i < 32; i++) {
        f.ranges[0].flags = 1u << i;
        r = set_filter(vm, &f);
        ok &= (1u << i) & KVM_MSR_FILTER_RANGE_VALID_MASK ? r == 0 : r == -EINVAL;
    }
    CHECK(ok, "KVM_X86_SET_MSR_FILTER takes DEFAULT_DENY, and READ and WRITE in a range");
    k_memset(&f, 0, sizeof(f));
    f.flags = KVM_MSR_FILTER_DEFAULT_DENY;
    CHECK(set_filter(vm, &f) == -EINVAL, "a filter that denies all with no range");
    f.ranges[0].flags = KVM_MSR_FILTER_READ;
    f.ranges[0].nmsrs = KVM_MSR_FILTER_MAX_BITMAP_SIZE * 8u + 1u;
    f.ranges[0].bitmap = (uintptr_t)read_bits;
    CHECK(set_filter(vm, &f) == -EINVAL, "a bitmap past KVM_MSR_FILTER_MAX_BITMAP_SIZE");
}

/**
 * The guest's RDMSR and WRMSR under KVM_X86_SET_MSR_FILTER and
 * KVM_CAP_X86_USER_SPACE_MSR, as userspace_msr_exit_test's filter_deny
 * sets them: which accesses the filter allows, the request each vCPU gets
 * when it changes, and the exits through the run page with the caller's
 * answers at the next KVM_RUN, a read's value or #GP.
 */
static void user_msr_rows(void)
{
    int vm = dev(KVM_CREATE_VM, 0), fd = fdio(vm, KVM_CREATE_VCPU, 0);
    struct kvm_vcpu *v = (struct kvm_vcpu *)entries[fd].priv;
    struct kvm_run *run = run_of(fd);
    struct kvm_msr_filter f;
    struct kvm_regs regs;

    user_msr_flag_rows(vm);
    k_memset(read_bits, 0xFF, sizeof(read_bits));
    k_memset(write_bits, 0xFF, sizeof(write_bits));
    read_bits[0x10 / 8] &= (uint8_t)~(1u << (0x10 % 8));
    write_bits[0x1FC / 8] &= (uint8_t)~(1u << (0x1FC % 8));
    k_memset(&f, 0, sizeof(f));
    f.flags = KVM_MSR_FILTER_DEFAULT_DENY;
    f.ranges[0].flags = KVM_MSR_FILTER_READ;
    f.ranges[0].nmsrs = KVM_MSR_FILTER_MAX_BITMAP_SIZE * 8u;
    f.ranges[0].bitmap = (uintptr_t)read_bits;
    f.ranges[1].flags = KVM_MSR_FILTER_WRITE;
    f.ranges[1].nmsrs = KVM_MSR_FILTER_MAX_BITMAP_SIZE * 8u;
    f.ranges[1].bitmap = (uintptr_t)write_bits;
    f.ranges[2].flags = KVM_MSR_FILTER_READ | KVM_MSR_FILTER_WRITE;
    f.ranges[2].nmsrs = 1;
    f.ranges[2].base = 0xDEADBEEFu;
    f.ranges[2].bitmap = (uintptr_t)&one_bit;
    v->requests = 0;
    CHECK(set_filter(vm, &f) == 0 && (v->requests & KVM_REQ_MSR_FILTER),
          "a new filter asks each vCPU to bring its bitmap up to date");
    CHECK(!kvm_msr_allowed(v, 0x10, 0) && kvm_msr_allowed(v, 0x11, 0) &&
              kvm_msr_allowed(v, 0x10, 1) && !kvm_msr_allowed(v, 0x1FC, 1) &&
              kvm_msr_allowed(v, 0x1FC, 0),
          "a range's bitmap decides the accesses its flags name");
    CHECK(!kvm_msr_allowed(v, 0xC0000100u, 0) && kvm_msr_allowed(v, 0xDEADBEEFu, 1) &&
              kvm_msr_allowed(v, 0x830, 1),
          "an MSR no range names is denied, but the x2APIC's are never filtered");

    CHECK(vm_cap(vm, KVM_CAP_X86_USER_SPACE_MSR,
                 KVM_MSR_EXIT_REASON_UNKNOWN | KVM_MSR_EXIT_REASON_FILTER) == 0 &&
              kvm_msr_user_exit(v, 0x10, 0, 0, KVM_MSR_INVAL, 2) == 0,
          "a failure whose reason the caller did not ask for stays #GP");
    script_len = 0;
    script_at = 0;
    CHECK(kvm_msr_user_exit(v, 0xDEADBEEFu, 0, 0, KVM_MSR_UNKNOWN, 2) == 1,
          "an unknown MSR's read goes to the caller");
    script[script_len++] = v->exit;
    CHECK(kvm_msr_user_exit(v, 0x1FC, 1, 0x1234, KVM_MSR_FILTERED, 3) == 1, "and a filtered write");
    script[script_len++] = v->exit;
    refusing = 0;
    filter_changes = 0;
    v->regs.rip = 0x1000;
    CHECK(fdio(fd, KVM_RUN, 0) == 0 && run->exit_reason == KVM_EXIT_X86_RDMSR &&
              run->u.msr.index == 0xDEADBEEFu && run->u.msr.reason == KVM_MSR_EXIT_REASON_UNKNOWN &&
              run->u.msr.error == 0 && filter_changes == 1,
          "KVM_EXIT_X86_RDMSR with its index and reason, after the bitmap was brought up to date");
    run->u.msr.data = 0x1122334455667788ull;
    CHECK(fdio(fd, KVM_RUN, 0) == 0 && run->exit_reason == KVM_EXIT_X86_WRMSR &&
              run->u.msr.index == 0x1FC && run->u.msr.data == 0x1234 &&
              run->u.msr.reason == KVM_MSR_EXIT_REASON_FILTER,
          "KVM_EXIT_X86_WRMSR with the value written");
    CHECK(fdio(fd, KVM_GET_REGS, (uintptr_t)&regs) == 0 && regs.rax == 0x55667788u &&
              regs.rdx == 0x11223344u && regs.rip == 0x1002,
          "the caller's answer to the read is in EDX:EAX, past the instruction");
    run->u.msr.error = 1;
    queued = -1;
    CHECK(fdio(fd, KVM_RUN, 0) == -EIO && queued == 13 && v->regs.rip == 0x1002,
          "the caller's error is #GP, at the instruction");
    refusing = 1;
    fdclose(fd);
    fdclose(vm);
    CHECK(live == 0, "the VM frees its filter");
}

/** The entry of `buf`'s first `n` CPUID entries for `function` and
 * `index`, or NULL. */
static struct kvm_cpuid_entry2 *buf_entry(uint32_t n, uint32_t function, uint32_t index)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        if (CPUID_ENTRIES[i].function == function && CPUID_ENTRIES[i].index == index) {
            return &CPUID_ENTRIES[i];
        }
    }
    return NULL;
}

/** Leaf 0xD subleaf `index`'s EBX in `v`'s CPUID, or ~0. */
static uint32_t vcpu_d_ebx(const struct kvm_vcpu *v, uint32_t index)
{
    const struct kvm_cpuid_entry2 *e = kvm_cpuid_find(v, 0xD, index);

    return e ? e->ebx : ~0u;
}

/** IA32_XSS on `v`, as Linux 6.12's: the caller reads 0 and writes 0
 * alone; a guest whose CPUID has XSAVES, `xsaves`, does the same and its
 * nonzero write is #GP, and to one without it every access is #GP. */
static void xss_rows(struct kvm_vcpu *v, int xsaves)
{
    uint64_t value = 1;

    CHECK(kvm_msr_get(v, 0xDA0, &value, 1) == 0 && value == 0 && kvm_msr_set(v, 0xDA0, 0, 1) == 0 &&
              kvm_msr_set(v, 0xDA0, 1, 1) == KVM_MSR_INVAL,
          "the caller reads IA32_XSS as 0 and writes 0 to it, and nothing else");
    value = 1;
    if (xsaves) {
        CHECK(kvm_msr_get(v, 0xDA0, &value, 0) == 0 && value == 0 &&
                  kvm_msr_set(v, 0xDA0, 0, 0) == 0 && kvm_msr_set(v, 0xDA0, 1, 0) == KVM_MSR_INVAL,
              "a guest whose CPUID has XSAVES reads 0 and writes 0, and a nonzero write is #GP");
    } else {
        CHECK(kvm_msr_get(v, 0xDA0, &value, 0) == KVM_MSR_INVAL &&
                  kvm_msr_set(v, 0xDA0, 0, 0) == KVM_MSR_INVAL,
              "IA32_XSS is #GP to a guest whose CPUID lacks XSAVES");
    }
}

/**
 * XSAVES as KVM offers it, as Linux 6.12: without XSAVES and XSAVEC
 * subleaf 1 offers XSAVEOPT alone with EBX 0; with both and a backend
 * that lets a guest run XSAVES it offers both, with EBX the compacted size
 * of the XCR0 KVM supports, and with a backend that does not, XSAVEC
 * alone. A vCPU's subleaf 1 EBX follows its XCR0 from
 * the reset through KVM_SET_CPUID2 and KVM_SET_XCRS; and IA32_XSS's
 * rules (`xss_rows`) before and after its CPUID has XSAVES.
 */
static void xsaves_rows(void)
{
    struct kvm_cpuid_entry2 *d1;
    struct kvm_xcrs xcrs;
    struct kvm_vcpu *v;
    int vm, fd;
    uint32_t n;

    model_xsaves = 0;
    n = pv_cpuid();
    d1 = buf_entry(n, 0xD, 1);
    CHECK(d1 && d1->eax == 0x1 && d1->ebx == 0,
          "without XSAVES subleaf 1 offers XSAVEOPT alone, with EBX 0");
    model_xsaves = 1;
    n = pv_cpuid();
    d1 = buf_entry(n, 0xD, 1);
    CHECK(d1 && d1->eax == 0xF && d1->ebx == 832,
          "with XSAVEC and XSAVES subleaf 1 offers both, with EBX 832 for x87, SSE and AVX");
    backend_no_xsaves = 1;
    n = pv_cpuid();
    d1 = buf_entry(n, 0xD, 1);
    CHECK(d1 && d1->eax == 0x7 && d1->ebx == 832,
          "a backend that lets no guest run XSAVES keeps it out of subleaf 1, and XSAVEC in");
    backend_no_xsaves = 0;
    n = pv_cpuid();
    vm = dev(KVM_CREATE_VM, 0);
    fd = fdio(vm, KVM_CREATE_VCPU, 0);
    v = (struct kvm_vcpu *)entries[fd].priv;
    xss_rows(v, 0);
    CHECK(fdio(fd, KVM_SET_CPUID2, (uintptr_t)&buf) == 0 && vcpu_d_ebx(v, 0) == 576 &&
              vcpu_d_ebx(v, 1) == 576,
          "KVM_SET_CPUID2 gives leaf 0xD the sizes of the reset's XCR0, x87's 576");
    xss_rows(v, 1);
    CHECK(fdio(fd, KVM_GET_XCRS, (uintptr_t)&xcrs) == 0, "KVM_GET_XCRS");
    xcrs.xcrs[0].value = 7;
    CHECK(fdio(fd, KVM_SET_XCRS, (uintptr_t)&xcrs) == 0 && vcpu_d_ebx(v, 0) == 832 &&
              vcpu_d_ebx(v, 1) == 832,
          "KVM_SET_XCRS of x87, SSE and AVX makes both sizes 832");
    xcrs.xcrs[0].value = 3;
    CHECK(fdio(fd, KVM_SET_XCRS, (uintptr_t)&xcrs) == 0 && vcpu_d_ebx(v, 0) == 576 &&
              vcpu_d_ebx(v, 1) == 576,
          "and of x87 and SSE 576 again");
    fdclose(fd);
    fdclose(vm);
    model_xsaves = 0;
}

/**
 * The XSAVE image x86_64/state_test moves, on the Lenovo's model, whose
 * XCR0 is x87, SSE and AVX: KVM_CAP_XSAVE2 sizes KVM_GET_XSAVE2 at 4096
 * bytes; the image of a vCPU whose CPUID allows all three loads by
 * KVM_SET_XSAVE into a vCPU given no CPUID, and then into one given an
 * empty CPUID, as Linux checks it against what KVM supports and not the
 * guest's CPUID, while AVX-512, which KVM does not support here, is
 * refused; a vCPU with no CPUID gives x87 and SSE still; and MXCSR is
 * checked with x87 alone marked and kept with none of x87, SSE and AVX.
 */
static void xsave_move_rows(void)
{
    static uint8_t img[4096] __attribute__((aligned(64)));
    struct kvm_xcrs xcrs;
    uint64_t bv;
    uint32_t mxcsr, n = pv_cpuid();
    int vm = dev(KVM_CREATE_VM, 0), a = fdio(vm, KVM_CREATE_VCPU, 0);
    int b = fdio(vm, KVM_CREATE_VCPU, 1);

    CHECK(dev(KVM_CHECK_EXTENSION, KVM_CAP_XSAVE2) == 4096 &&
              fdio(vm, KVM_CHECK_EXTENSION, KVM_CAP_XSAVE2) == 4096,
          "KVM_CAP_XSAVE2 is 4096 bytes on /dev/kvm and on a VM");
    buf.cpuid.nent = n;
    CHECK(fdio(a, KVM_SET_CPUID2, (uintptr_t)&buf) == 0, "KVM_SET_CPUID2 of the supported CPUID");
    fdio(a, KVM_GET_XCRS, (uintptr_t)&xcrs);
    xcrs.xcrs[0].value = 7;
    CHECK(fdio(a, KVM_SET_XCRS, (uintptr_t)&xcrs) == 0, "XCR0 of x87, SSE and AVX");
    fdio(a, KVM_GET_XSAVE2, (uintptr_t)img);
    bv = 0x7;
    k_memcpy(img + 512, &bv, 8);
    CHECK(fdio(a, KVM_SET_XSAVE, (uintptr_t)img) == 0,
          "AVX marked in use, as the guest's code has");
    k_memset(img, 0, sizeof(img));
    CHECK(fdio(a, KVM_GET_XSAVE2, (uintptr_t)img) == 0, "KVM_GET_XSAVE2");
    k_memcpy(&bv, img + 512, 8);
    CHECK(bv == 0x7 && img[25] == 0x1F, "it gives x87, SSE and AVX, and MXCSR 0x1F80");
    CHECK(fdio(b, KVM_SET_XSAVE, (uintptr_t)img) == 0, "a vCPU given no CPUID takes the image");
    bv = 0x27;
    k_memcpy(img + 512, &bv, 8);
    CHECK(fdio(b, KVM_SET_XSAVE, (uintptr_t)img) == -EINVAL,
          "but not with AVX-512 marked, which KVM does not support here");
    buf.cpuid.nent = 0;
    CHECK(fdio(b, KVM_SET_CPUID2, (uintptr_t)&buf) == 0, "KVM_SET_CPUID2 with no entry");
    bv = 0x7;
    k_memcpy(img + 512, &bv, 8);
    CHECK(fdio(b, KVM_SET_XSAVE, (uintptr_t)img) == 0, "a vCPU given an empty CPUID takes it too");
    CHECK(fdio(b, KVM_GET_XSAVE, (uintptr_t)img) == 0, "KVM_GET_XSAVE of that vCPU");
    k_memcpy(&bv, img + 512, 8);
    CHECK(bv == 0x3, "it gives x87 and SSE, without the AVX its CPUID lacks");
    bv = 0x1;
    k_memcpy(img + 512, &bv, 8);
    mxcsr = 0x10000; /* a reserved bit */
    k_memcpy(img + 24, &mxcsr, 4);
    CHECK(fdio(b, KVM_SET_XSAVE, (uintptr_t)img) == -EINVAL,
          "MXCSR is checked with x87 alone marked");
    bv = 0;
    k_memcpy(img + 512, &bv, 8);
    mxcsr = 0xFFFFFFFFu;
    k_memcpy(img + 24, &mxcsr, 4);
    CHECK(fdio(b, KVM_SET_XSAVE, (uintptr_t)img) == 0, "with none of x87, SSE and AVX it is not");
    fdio(b, KVM_GET_XSAVE, (uintptr_t)img);
    k_memcpy(&mxcsr, img + 24, 4);
    CHECK(mxcsr == 0x1F80, "and the vCPU keeps its own MXCSR");
    fdclose(b);
    fdclose(a);
    fdclose(vm);
}

/**
 * KVM_GET_XSAVE and KVM_SET_XSAVE on a model without XSAVE, where Linux
 * keeps them in FXSAVE's form: KVM_CAP_XSAVE answers 1 on either model and
 * KVM_CAP_XCRS only with XSAVE; KVM_GET_XSAVE gives FXSAVE's 512 bytes with
 * x87 and SSE marked and nothing after the header's first word, from a
 * vCPU whose image marks AVX alone and has marked bytes after the 512,
 * where the ordinary mask would mark nothing and keep those bytes; and
 * KVM_SET_XSAVE refuses AVX marked or a reserved MXCSR bit and loads the
 * 512 bytes, MXCSR among them.
 */
static void xsave_none_rows(void)
{
    static uint8_t img[4096] __attribute__((aligned(64)));
    struct kvm_vcpu *v;
    uint64_t bv;
    uint32_t mxcsr, i;
    uint8_t rest = 0;
    int vm, a;

    CHECK(dev(KVM_CHECK_EXTENSION, KVM_CAP_XSAVE) == 1 &&
              dev(KVM_CHECK_EXTENSION, KVM_CAP_XCRS) == 1,
          "with XSAVE, KVM_CAP_XSAVE and KVM_CAP_XCRS are 1");
    model_no_xsave = 1;
    kvm_xcr0_forget();
    CHECK(kvm_xcr0_supported() == 0, "the model without XSAVE supports no XCR0");
    CHECK(dev(KVM_CHECK_EXTENSION, KVM_CAP_XSAVE) == 1 &&
              dev(KVM_CHECK_EXTENSION, KVM_CAP_XCRS) == 0,
          "without it KVM_CAP_XSAVE is 1 still and KVM_CAP_XCRS 0");
    vm = dev(KVM_CREATE_VM, 0);
    a = fdio(vm, KVM_CREATE_VCPU, 0);
    v = (struct kvm_vcpu *)entries[a].priv;
    k_memset(v->xsave + 512, 0x5A, KVM_XSAVE_SIZE - 512);
    bv = 0x4; /* AVX alone */
    k_memcpy(v->xsave + 512, &bv, 8);
    k_memset(img, 0xA5, sizeof(img));
    CHECK(fdio(a, KVM_GET_XSAVE, (uintptr_t)img) == 0, "KVM_GET_XSAVE without XSAVE");
    k_memcpy(&bv, img + 512, 8);
    for (i = 520; i < sizeof(img); i++) {
        rest |= img[i];
    }
    CHECK(bv == 0x3 && rest == 0 && img[24] == 0x80 && img[25] == 0x1F,
          "it gives FXSAVE's image, x87 and SSE marked, MXCSR 0x1F80, the header and tail cleared");
    bv = 0x7;
    k_memcpy(img + 512, &bv, 8);
    CHECK(fdio(a, KVM_SET_XSAVE, (uintptr_t)img) == -EINVAL, "KVM_SET_XSAVE refuses AVX marked");
    bv = 0x3;
    k_memcpy(img + 512, &bv, 8);
    mxcsr = 0x10000; /* a reserved bit */
    k_memcpy(img + 24, &mxcsr, 4);
    CHECK(fdio(a, KVM_SET_XSAVE, (uintptr_t)img) == -EINVAL, "and a reserved MXCSR bit");
    mxcsr = 0x9F80; /* FZ */
    k_memcpy(img + 24, &mxcsr, 4);
    CHECK(fdio(a, KVM_SET_XSAVE, (uintptr_t)img) == 0, "it takes x87 and SSE");
    k_memset(img, 0, sizeof(img));
    fdio(a, KVM_GET_XSAVE, (uintptr_t)img);
    k_memcpy(&mxcsr, img + 24, 4);
    CHECK(mxcsr == 0x9F80, "and loads FXSAVE's image, MXCSR among it");
    fdclose(a);
    fdclose(vm);
    model_no_xsave = 0;
    kvm_xcr0_forget();
    CHECK(kvm_xcr0_supported() == 0x7, "the Haswell's model has x87, SSE and AVX again");
}

/** The capabilities and bounds Linux's selftests ask for, each in VMs of
 * its own. */
static void selftest_rows(void)
{
    tsc_rows();
    stats_rows();
    pre_fault_rows();
    bound_rows();
    cr4_rows();
    vm_cap_rows();
    feature_msr_rows();
    svm_debugctl_rows();
    svm_sfmask_rows();
    spec_rows();
    mce_hwcr_rows();
    xsaves_rows();
    xsave_move_rows();
    xsave_none_rows();
    user_msr_rows();
    attr_rows();
    sync_rows();
}

/** kvm/exit_stats's lines of counts, after its two headings, read into
 * `buf` of `cap` bytes and ended with a NUL; NULL when the text does not
 * fit or its headings are not those of the backend named `backend`. */
static const char *exit_stats(char *buf, int32_t cap, const char *backend)
{
    static const char columns[] = "code exits kernel user wait_cycles handle_cycles "
                                  "prepare_cycles entry_cycles run_cycles exit_cycles fast name\n";
    int32_t n = exitstat_ops_kept->show(exitstat_data_kept, buf, (uint32_t)cap - 1u);
    char *p;
    unsigned long len = 0;

    if (n < 0 || n >= cap) {
        return NULL;
    }
    buf[n] = '\0';
    while (backend[len]) {
        len++;
    }
    if (strncmp(buf, "backend ", 8) != 0 || strncmp(buf + 8, backend, len) != 0 ||
        strncmp(buf + 8 + len, " tsc_khz ", 9) != 0) {
        return NULL;
    }
    p = strchr(buf, '\n');
    if (!p || strncmp(p + 1, columns, sizeof(columns) - 1) != 0) {
        return NULL;
    }
    return p + 1 + sizeof(columns) - 1;
}

/** Write `text` to kvm/exit_stats: what the store returns. */
static int32_t exit_stats_write(const char *text)
{
    uint32_t len = 0;

    while (text[len]) {
        len++;
    }
    return exitstat_ops_kept->store(exitstat_data_kept, (const char __user *)text, len);
}

/** Count `x`'s exit, by `x->hw_exit`, closing the account before it at
 * `entry` and opening its own at `left`, whose handling then waits for the
 * big lock through kvm_bkl_enter until `held`. */
static void exit_with_wait(struct kvm_vcpu *x, uint64_t entry, uint64_t left, uint64_t held)
{
    kvm_exitstat_exit(x, entry, left);
    tsc = left;
    bkl_wait = held - left;
    kvm_bkl_enter(x);
    bkl_exit();
}

/** kvm/exit_stats on its own: the refused writes, the clear, VMX's rows,
 * its failed entries and its `other` row, summed over two processors,
 * and SVM's rows, its high codes from 0x400 and its failed entries, with
 * the cycles each exit's account gives each column. */
static void exitstat_alone_rows(void)
{
    static char buf[2048];
    const struct kvm_backend *was = kvm_backend;
    uint64_t tsc_was = tsc;
    struct kvm_vcpu x;
    const char *t;

    CHECK(exitstat_ops_kept && exitstat_ops_kept->store, "kvm_start registers kvm/exit_stats");
    CHECK(exit_stats_write("1") == -EINVAL && exit_stats_write("0x") == -EINVAL &&
              exit_stats_write("00\n") == -EINVAL,
          "a write to kvm/exit_stats other than 0 is refused");
    CHECK(exit_stats_write("0\n") == 2, "a write of 0 clears the counts");
    t = exit_stats(buf, sizeof(buf), "vmx");
    CHECK(t && *t == '\0', "with no exits the file has only its headings");

    k_memset(&x, 0, sizeof(x));
    x.xs_row = -1;
    host_cpu = 0;
    x.hw_exit = 30;
    exit_with_wait(&x, 100, 200, 250);
    host_cpu = 1;
    x.hw_exit = 0x80000021u;
    exit_with_wait(&x, 400, 500, 520);
    x.hw_exit = 0x1234;
    exit_with_wait(&x, 600, 700, 710);
    tsc = 1000;
    kvm_exitstat_done(&x);
    tsc = 2000;
    kvm_exitstat_done(&x);
    t = exit_stats(buf, sizeof(buf), "vmx");
    CHECK(t && strcmp(t, "0x1e 1 1 0 50 150 0 0 0 0 0 IO_INSTRUCTION\n"
                         "0x0 1 0 1 10 290 0 0 0 0 0 other\n"
                         "0x80000000 1 1 0 20 80 0 0 0 0 0 FAILED_VMENTRY\n") == 0,
          "VMX's rows: each exit's wait for the big lock, its handling to the next entry or to "
          "KVM_RUN's "
          "return, a failed entry under bit 31 and a reason past the table in other, "
          "summed over the processors that opened and closed the accounts");
    host_cpu = 2;
    x.hw_exit = 10;
    exit_with_wait(&x, 3000, 3100, 3200);
    CHECK(x.xs_row == -1, "an exit on a processor with no rows opens no account");
    CHECK(exit_stats_write("0") == 1, "a write of 0 without a newline");
    t = exit_stats(buf, sizeof(buf), "vmx");
    CHECK(t && *t == '\0', "clears every processor's counts");

    kvm_backend = &kvm_svm;
    host_cpu = 0;
    x.hw_exit = 0x400;
    exit_with_wait(&x, 0, 10, 15);
    x.hw_exit = 0x403;
    exit_with_wait(&x, 20, 30, 31);
    x.hw_exit = 0xFFFFFFFFu;
    exit_with_wait(&x, 40, 50, 52);
    x.hw_exit = 0xB0;
    exit_with_wait(&x, 60, 70, 73);
    x.hw_exit = 0x410;
    exit_with_wait(&x, 80, 90, 94);
    x.hw_exit = 0xAF;
    exit_with_wait(&x, 100, 110, 115);
    tsc = 125;
    kvm_exitstat_done(&x);
    t = exit_stats(buf, sizeof(buf), "svm");
    CHECK(t && strcmp(t, "0xaf 1 0 1 5 10 0 0 0 0 0 0xaf\n"
                         "0x400 1 1 0 5 5 0 0 0 0 0 npf\n"
                         "0x403 1 1 0 1 9 0 0 0 0 0 vmgexit\n"
                         "0x0 2 2 0 7 13 0 0 0 0 0 other\n"
                         "0xffffffff 1 1 0 2 8 0 0 0 0 0 invalid_guest_state\n") == 0,
          "SVM's rows: a code Linux does not name, NPF and VMGEXIT from 0x400, 0xb0 and "
          "0x410 in other, and a failed entry");
    exit_stats_write("0");
    kvm_backend = was;
    tsc = tsc_was;
}

/** kvm/exit_stats's columns of an exit's parts, on one processor: the
 * entry, run and exit the backend timed, counted only when the marks run
 * in order, and the preparation, from the run loop's first pass over its
 * top after an exit to the next entry, counted only for an exit the kernel
 * handled, and the exits the fast path took. The marks of three CPUID
 * exits, the second taken by the fast path, and KVM_RUN's return give the
 * line's eleven numbers. */
static void exitstat_part_rows(void)
{
    static char buf[2048];
    uint64_t tsc_was = tsc;
    struct kvm_vcpu x;
    const char *t;

    exit_stats_write("0");
    k_memset(&x, 0, sizeof(x));
    x.xs_row = -1;
    host_cpu = 0;
    x.hw_exit = 10;
    x.xs_in = 120;
    x.xs_out = 180;
    exit_with_wait(&x, 100, 200, 250);
    CHECK(x.xs_in == 0 && x.xs_out == 0, "an exit's count takes the backend's marks");
    tsc = 300;
    kvm_exitstat_prepare(&x);
    tsc = 350;
    kvm_exitstat_prepare(&x);
    CHECK(x.xs_prep == 300, "the preparation starts at the loop's first pass after the exit");
    x.xs_in = 420;
    x.xs_out = 470;
    exit_with_wait(&x, 400, 500, 530);
    kvm_exitstat_fast(&x);
    x.xs_in = 650;
    x.xs_out = 640;
    exit_with_wait(&x, 600, 700, 710);
    tsc = 750;
    kvm_exitstat_prepare(&x);
    tsc = 800;
    kvm_exitstat_done(&x);
    CHECK(x.xs_prep == 0 && x.xs_row == -1,
          "KVM_RUN's return ends the account and its preparation");
    t = exit_stats(buf, sizeof(buf), "vmx");
    CHECK(t && strcmp(t, "0xa 3 2 1 90 310 100 40 110 50 1 CPUID\n") == 0,
          "CPUID's line: three exits, two handled in the kernel and one that ended KVM_RUN, "
          "the waits, the handling, the one preparation that ran to an entry, the entry, "
          "run and exit of the two runs whose marks are in order, and the one fast exit");
    exit_stats_write("0");
    tsc = tsc_was;
}

/** kvm/exit_stats's PREPARE, the part of HANDLE from the run loop's first
 * pass after an exit: without a wait for the big lock inside it, which
 * counts in WAIT as HANDLE's does, and 0, never a wrap, for a preparation
 * that ends on a processor whose TSC reads lower than the one where it
 * began. */
static void exitstat_prep_rows(void)
{
    static char buf[2048];
    uint64_t tsc_was = tsc;
    struct kvm_vcpu x;
    const char *t;

    exit_stats_write("0");
    k_memset(&x, 0, sizeof(x));
    x.xs_row = -1;
    host_cpu = 0;
    x.hw_exit = 10;
    exit_with_wait(&x, 0, 100, 100);
    tsc = 120;
    kvm_exitstat_prepare(&x);
    tsc = 130;
    bkl_wait = 100;
    kvm_bkl_enter(&x);
    bkl_exit();
    x.hw_exit = 12;
    exit_with_wait(&x, 240, 300, 300);
    tsc = 400;
    kvm_exitstat_prepare(&x);
    host_cpu = 1;
    x.hw_exit = 1;
    exit_with_wait(&x, 350, 360, 360);
    tsc = 370;
    kvm_exitstat_done(&x);
    t = exit_stats(buf, sizeof(buf), "vmx");
    CHECK(t && strcmp(t, "0x1 1 0 1 0 10 0 0 0 0 0 EXTERNAL_INTERRUPT\n"
                         "0xa 1 1 0 100 40 20 0 0 0 0 CPUID\n"
                         "0xc 1 1 0 0 50 0 0 0 0 0 HLT\n") == 0,
          "PREPARE leaves out a wait inside it, as HANDLE does, and counts 0 for a span "
          "that ends on a processor whose TSC reads lower");
    exit_stats_write("0");
    host_cpu = 0;
    tsc = tsc_was;
}

/** kvm/exit_stats through KVM_RUN on the vCPU `fd`: an exit the kernel
 * handles closes as one when the guest runs again, and as one that ended
 * KVM_RUN when KVM_RUN returns first, at a request that comes after the
 * run loop has looked for one, or at the backend's refusal of the
 * entry. */
static void exitstat_run_rows(int fd)
{
    static char buf[2048];
    struct kvm_run *run = run_of(fd);
    const char *t;

    host_cpu = 0;
    refusing = 0;
    exit_stats_write("0");
    script_len = 0;
    script_at = 0;
    scripted(KVM_X_AGAIN);
    script_hw[0] = 1;
    scripted(KVM_X_HLT);
    script_hw[1] = 12;
    CHECK(fdio(fd, KVM_RUN, 0) == 0 && run->exit_reason == KVM_EXIT_HLT,
          "an interrupt's exit, then a HLT for the caller");
    t = exit_stats(buf, sizeof(buf), "vmx");
    CHECK(t && strcmp(t, "0x1 1 1 0 0 0 0 0 0 0 0 EXTERNAL_INTERRUPT\n"
                         "0xc 1 0 1 0 0 0 0 0 0 0 HLT\n") == 0,
          "the interrupt's exit counts in kernel, the HLT's in user");

    exit_stats_write("0");
    script_len = 0;
    script_at = 0;
    scripted(KVM_X_AGAIN);
    script_hw[0] = 1;
    late_request = KVM_REQ_EXIT;
    entries_made = 0;
    CHECK(fdio(fd, KVM_RUN, 0) == -EINTR && run->exit_reason == KVM_EXIT_INTR &&
              entries_made == 1 && !late_request,
          "a request that comes after the loop's look ends KVM_RUN at the entry");
    t = exit_stats(buf, sizeof(buf), "vmx");
    CHECK(t && strcmp(t, "0x1 1 0 1 0 0 0 0 0 0 0 EXTERNAL_INTERRUPT\n") == 0,
          "and the exit before it counts in user, with no entry after it");

    exit_stats_write("0");
    script_len = 0;
    script_at = 0;
    scripted(KVM_X_AGAIN);
    script_hw[0] = 1;
    refuse_at_end = 1;
    entries_made = 0;
    CHECK(fdio(fd, KVM_RUN, 0) == -EOPNOTSUPP && entries_made == 2,
          "the backend refuses the entry after an exit the kernel handled");
    t = exit_stats(buf, sizeof(buf), "vmx");
    CHECK(t && strcmp(t, "0x1 1 0 1 0 0 0 0 0 0 0 EXTERNAL_INTERRUPT\n") == 0,
          "and the exit counts in user");
    refuse_at_end = 0;
    exit_stats_write("0");
    script_len = 0;
    script_at = 0;
    script_hw[0] = 0;
    script_hw[1] = 0;
}

/** Run the vCPU `fd` through one exit the kernel handles, after which the
 * event's injection leaves `request`, to the backend's refusal of the next
 * entry: whether it reached that entry, the second, after `injected`
 * injections. `request` is taken back after, so that no row leaves it to
 * the next. */
static int late_run(int fd, uint32_t request, int injected)
{
    int was = injects, ok;

    script_len = 0;
    script_at = 0;
    scripted(KVM_X_AGAIN);
    script_hw[0] = 1;
    late_request = request;
    entries_made = 0;
    entry_requests = 0;
    loop_passes = 0;
    pass_guard = (struct kvm_vcpu *)entries[fd].priv;
    ok = fdio(fd, KVM_RUN, 0) == -EOPNOTSUPP && entries_made == 2 && !late_request &&
         injects == was + injected && loop_passes <= 1000;
    pass_guard->requests &= ~request;
    return ok;
}

/**
 * A request that comes after the run loop's look and before the entry,
 * once the vCPU is in the guest, cancels the entry (KVM_REQ_NOENTER), so
 * the loop takes it before it enters, as Linux's kvm_vcpu_exit_request
 * cancels one for any pending request (arch/x86/kvm/x86.c:1715-1720,
 * 10519-10527): a KVM_REQ_CLOCK_UPDATE is written, and a KVM_REQ_EVENT
 * injected, before the entry that follows. A KVM_REQ_TLB_FLUSH does not
 * cancel it, since the backend's entry takes that request itself and drops
 * the translations. A switch asked for at the same point cancels it too,
 * as Linux's check reads TIF_NEED_RESCHED there, and the loop switches
 * before the next entry.
 */
static void entry_cancel_rows(int fd)
{
    unsigned long switched;

    host_cpu = 0;
    refusing = 0;
    refuse_at_end = 1;
    CHECK(late_run(fd, KVM_REQ_CLOCK_UPDATE, 1) && !(entry_requests & KVM_REQ_CLOCK_UPDATE),
          "a KVM_REQ_CLOCK_UPDATE after the loop's look cancels the entry, and the loop takes it "
          "before the next");
    CHECK(late_run(fd, KVM_REQ_EVENT, 2) && !(entry_requests & KVM_REQ_EVENT),
          "a KVM_REQ_EVENT after the loop's look cancels the entry, and the event is injected "
          "before the next");
    CHECK(late_run(fd, KVM_REQ_TLB_FLUSH, 1) && (entry_requests & KVM_REQ_TLB_FLUSH),
          "a KVM_REQ_TLB_FLUSH after the loop's look leaves the entry to drop the translations");
    script_len = 0;
    script_at = 0;
    scripted(KVM_X_AGAIN);
    script_hw[0] = 1;
    late_resched = 1;
    resched_entered = 0;
    entries_made = 0;
    switched = caller.nivcsw;
    CHECK(fdio(fd, KVM_RUN, 0) == -EOPNOTSUPP && entries_made == 2 && !late_resched &&
              !resched_entered && caller.nivcsw == switched + 1,
          "a switch asked for after the loop's look cancels the entry, and the loop switches "
          "before the next");
    caller.need_resched = 0;
    late_resched = 0;
    pass_guard = NULL;
    refuse_at_end = 0;
    exit_stats_write("0");
    script_len = 0;
    script_at = 0;
    script_hw[0] = 0;
}

/** Check the ring's separate shared mapping and its permissions. */
static void dirty_ring_map_rows(int fd)
{
    struct kvm_vcpu *v = (struct kvm_vcpu *)entries[fd].priv;
    vma_t vma;
    uintptr_t phys;

    k_memset(&vma, 0, sizeof(vma));
    vma.pgoff = KVM_DIRTY_LOG_PAGE_OFFSET;
    vma.end = 4096;
    vma.type = VM_FILE;
    CHECK(entries[fd].ops->mmap(&entries[fd], &vma) == -EINVAL,
          "a private dirty-ring mapping is refused");
    vma.type = VM_SHARED;
    vma.prot = VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXEC;
    CHECK(entries[fd].ops->mmap(&entries[fd], &vma) == -EINVAL,
          "an executable dirty-ring mapping is refused");
    vma.prot &= ~VM_PROT_EXEC;
    CHECK(entries[fd].ops->mmap(&entries[fd], &vma) == 0 &&
              map_ops->page(map_obj, 64, &phys) == 0 && phys == (uintptr_t)v->dirty_ring &&
              map_ops->page(map_obj, 63, &phys) == -1 && map_ops->page(map_obj, 65, &phys) == -1,
          "the shared ring occupies exactly its configured pages at offset 64");
    CHECK(dev(KVM_GET_VCPU_MMAP_SIZE, 0) == 12288, "the base mapping's size is unchanged");
}

/** Check the dirty-ring ABI, capabilities and VM enable rules. */
static void dirty_ring_enable_rows(int vm)
{
    struct kvm_enable_cap cap;

    CHECK(sizeof(struct kvm_dirty_gfn) == 16 && KVM_RESET_DIRTY_RINGS == 0xAEC7u &&
              KVM_EXIT_DIRTY_RING_FULL == 31 && KVM_DIRTY_LOG_PAGE_OFFSET == 64 &&
              KVM_CAP_DIRTY_LOG_RING == 192 && KVM_CAP_DIRTY_LOG_RING_ACQ_REL == 223,
          "the dirty-ring ABI matches Linux's");
    CHECK(dev(KVM_CHECK_EXTENSION, KVM_CAP_DIRTY_LOG_RING) == 1048576 &&
              fdio(vm, KVM_CHECK_EXTENSION, KVM_CAP_DIRTY_LOG_RING_ACQ_REL) == 1048576,
          "both dirty-ring capabilities give Linux's maximum byte size");
    CHECK(fdio(vm, KVM_RESET_DIRTY_RINGS, 0) == -EINVAL &&
              vm_cap(vm, KVM_CAP_DIRTY_LOG_RING, 0) == -EINVAL &&
              vm_cap(vm, KVM_CAP_DIRTY_LOG_RING, 2048) == -EINVAL &&
              vm_cap(vm, KVM_CAP_DIRTY_LOG_RING, 4097) == -EINVAL &&
              vm_cap(vm, KVM_CAP_DIRTY_LOG_RING, 2097152) == -E2BIG,
          "a ring needs a power of two between one page and the maximum");
    k_memset(&cap, 0, sizeof(cap));
    cap.cap = KVM_CAP_DIRTY_LOG_RING;
    cap.args[0] = 4096;
    cap.flags = 1;
    CHECK(fdio(vm, KVM_ENABLE_CAP, (uintptr_t)&cap) == -EINVAL, "ring flags are refused");
    CHECK(vm_cap(vm, KVM_CAP_DIRTY_LOG_RING_ACQ_REL, 4096) == 0 &&
              vm_cap(vm, KVM_CAP_DIRTY_LOG_RING, 4096) == -EINVAL,
          "the acquire/release ring is enabled once");
}

/** The dirty ring's capabilities, enable rules, mappings, full-ring exit,
 * continued refusal until reset, and mapping reference after close.
 * The fake backend marks pages during an exit handled inside KVM_RUN. */
static void dirty_ring_run_rows(void)
{
    int live_before = live;
    int vm = dev(KVM_CREATE_VM, 0), fd, b, before, refs;
    struct kvm_vcpu *v;
    uintptr_t phys;
    uint32_t i;

    dirty_ring_enable_rows(vm);
    fd = fdio(vm, KVM_CREATE_VCPU, 0);
    b = fdio(vm, KVM_CREATE_VCPU, 1);
    v = (struct kvm_vcpu *)entries[fd].priv;
    CHECK(v->dirty_ring && v->dirty_ring != ((struct kvm_vcpu *)entries[b].priv)->dirty_ring &&
              v->dirty_ring[0].flags == 0,
          "each vCPU gets zeroed pages of its own");
    dirty_ring_map_rows(fd);
    refusing = 0;
    script_len = script_at = 0;
    scripted(KVM_X_AGAIN);
    ring_writes = 256u - KVM_DIRTY_RING_RESERVED;
    before = entries_made;
    CHECK(fdio(fd, KVM_RUN, 0) == 0 && v->run->exit_reason == KVM_EXIT_DIRTY_RING_FULL &&
              entries_made == before + 1 && v->dirty_ring_put == 192 &&
              v->dirty_ring[191].slot == 3 && v->dirty_ring[191].offset == 191 &&
              !kvm_running_vcpu(v->vm),
          "an exit's writes fill the ring, and the run returns before another entry");
    CHECK(fdio(fd, KVM_RUN, 0) == 0 && v->run->exit_reason == KVM_EXIT_DIRTY_RING_FULL &&
              entries_made == before + 1,
          "an unreset full ring refuses another entry");
    for (i = 0; i < 192; i++) {
        v->dirty_ring[i].flags = KVM_DIRTY_GFN_F_RESET;
    }
    CHECK(fdio(vm, KVM_RESET_DIRTY_RINGS, 0) == 192 && !kvm_dirty_ring_full(v),
          "reset reports the collected count and allows entry again");
    script_len = script_at = 0;
    scripted(KVM_X_HLT);
    CHECK(fdio(fd, KVM_RUN, 0) == 0 && v->run->exit_reason == KVM_EXIT_HLT &&
              entries_made == before + 2,
          "the guest enters after the reset");
    refs = v->vm->refs;
    fdclose(b);
    fdclose(fd);
    fdclose(vm);
    CHECK(refs == 4 && v->vm->refs == 1 && map_ops->page(map_obj, 64, &phys) == 0,
          "the ring mapping keeps its VM and pages after every descriptor closes");
    map_ops->put(map_obj);
    vm = dev(KVM_CREATE_VM, 0);
    fd = fdio(vm, KVM_CREATE_VCPU, 0);
    CHECK(vm_cap(vm, KVM_CAP_DIRTY_LOG_RING, 4096) == -EINVAL,
          "a ring cannot be enabled after a vCPU was made");
    fdclose(fd);
    fdclose(vm);
    CHECK(live == live_before, "the dirty rings and their VMs return every allocation");
}

/** Run the rows; 0 when every check passed, 1 otherwise. */
int main(void)
{
    int vm, vcpu;

    caller.mm = &caller_mm;
    caller.fds = &table;
    abi_rows();
    hv_abi_rows();
    /* A second processor started at boot, so kvm_start makes exit_stats'
     * rows for two; those rows last as long as the kernel, so the count of
     * live allocations starts after them. */
    cpu_table[1].online = 1;
    kvm_start();
    cpu_table[1].online = 0;
    live = 0;
    CHECK(kvm_backend == &kvm_vmx && registered && registered->major == 10 &&
              registered->minor == 232 && registered->mode == 0666,
          "/dev/kvm is 10:232, read and written by everyone, on the VMX backend");
    CHECK(registered->name[0] == 'k' && registered->name[3] == '\0', "the node is named kvm");
    vcpu = qemu_startup(&vm);
    exit_rows(vcpu);
    exitstat_alone_rows();
    exitstat_part_rows();
    exitstat_prep_rows();
    exitstat_run_rows(vcpu);
    entry_cancel_rows(vcpu);
    state_rows(vcpu);
    vm_rows(vm, vcpu);
    pv_rows();
    selftest_rows();
    dirty_ring_run_rows();
    printf("kvm-hosttest: result: %s: %d failures\n", failures ? "fail" : "pass", failures);
    return failures ? 1 : 0;
}
