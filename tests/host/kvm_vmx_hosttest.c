/**
 * Check the VMX backend, vmx.c, on a model of the Lenovo's processor.
 *
 * host-deps: rach2/sys/kvm/kvm.c rach2/sys/kvm/vcpu.c rach2/sys/kvm/cpuid.c
 * host-deps: rach2/sys/kvm/msr.c rach2/sys/kvm/emulate.c rach2/sys/kvm/decode.c
 * host-deps: rach2/sys/kvm/hyperv.c rach2/sys/kvm/pvclock.c
 * host-deps: rach2/sys/kvm/binstats.c rach2/sys/kvm/exitstat.c
 * host-deps: rach2/sys/kvm/pmu.c
 * host-deps: tests/host/compat/fd_flat.c tests/host/compat/mutex_none.c
 * host-deps: tests/host/compat/spinlock_host.c tests/host/compat/atomic_host.c
 * host-cflags: -DRACH2_HOST_X86_64 -DRACH2_HOST_CPU
 *
 * vmx.c is the kernel's, included whole, so that its table of operations
 * can point at the model; the run loop, the vCPU ioctls, the CPUID and MSR
 * tables, the instruction emulator, the decoder and Hyper-V's enlightenments
 * are linked as they are.
 * The model is the Lenovo's Haswell as its capability MSRs read, with
 * IA32_FEATURE_CONTROL locked with VMX on (0x5), and VMXON, VMXOFF,
 * VMCLEAR, VMPTRLD, VMREAD, VMWRITE, INVEPT, INVVPID, VMLAUNCH and VMRESUME
 * as the SDM gives them, failures included: a VMCS of fields kept by
 * encoding and width, its launch state, and at each entry the checks of the
 * controls, the host state and the guest state that the backend's choices
 * reach, with the failure the processor gives for each. A guest's run is a
 * row of a script: the exit it ends with and what it leaves in the exit
 * fields. Memory is one slot of 1 MiB at guest-physical 0, the host's.
 *
 * The rows run the trial's path start to finish: the probe, refusing a
 * processor without what the backend needs, leaving the controls that load
 * IA32_PERF_GLOBAL_CTRL unused on Nehalem and Westmere and choosing the
 * Lenovo's controls;
 * a VM and its vCPU; VMX on on a processor exactly while a VMCS is active
 * there, with CR4 as it was once it is off, on every path; the first entry
 * a VMLAUNCH and the next VMRESUMEs with no VMPTRLD; the VMCS built and
 * checked by the model; each exit the corpus's guests cause handled in the
 * kernel or given to the caller: CPUID, port I/O and its string form, with
 * RF across a REP cut at a page and a 64-bit address size at offset 0, EPT
 * faults on RAM and MMIO reads, writes and read-modify-writes, an instruction
 * the decoder refuses, the control and debug registers, real to long mode and
 * PAE's PDPTEs, the MSRs, XSETBV, VMCALL, KVM's and Hyper-V's once the CPUID
 * and the guest's id name it, and the instructions answered alone, the
 * interrupt and NMI windows, an event the exit found under delivery, the
 * caller's single steps and breakpoints, the guest's own #DB and #AC given
 * back, #UD exits for the instructions Linux's emulator runs after one and
 * for the other vendor's hypercall, a table of the instructions Linux's
 * emulator runs with their operand on MMIO, string, far-pointer, MMX and SSE
 * forms among them, a RIP above 4 GiB, the host's
 * interrupt, NMI and machine check, the host's FREEZE_WHILE_SMM carried in
 * the guest's IA32_DEBUGCTL, hidden from its reads and written back to the
 * host's after the exit, the preemption timer, a triple fault and
 * the two kinds of failed entry; hardware task switches by CALL, IRET, JMP and
 * task gates, to 32-bit, 16-bit and virtual-8086 tasks, and their faults; a
 * move to another processor, which clears the VMCS on the one it left and
 * launches it again; a state read between runs on another processor; VMXON
 * and VMPTRLD failing; a failed VMWRITE reported once; VPIDs; the TPR
 * shadow, its threshold and its exit, with the APIC in the kernel; two vCPUs on
 * one processor, each resumed; a panic inside a run, which clears every
 * VMCS active there and turns VMX off; the VMCSs cleared as the vCPUs are
 * freed; and what Linux's selftests ask of the guest's side:
 * AMD's hypercall instruction patched or left as its quirk says, MWAIT's
 * quirk, MSR_PLATFORM_INFO and CPUID faulting, a plain OUT's RIP at its
 * exit and after it with port 0x7E's quirk, an emulation failure above
 * CPL 0 under KVM_CAP_EXIT_ON_EMULATION_FAILURE, DR7.GD under the
 * caller's breakpoints, and the TSC multiplier on a model whose controls
 * have TSC scaling.
 *
 * Beside the trial's path: the host's state and MSRs back, the guest's
 * FPU left to the switch, at a switch between two runs, a slice's end or
 * a halted vCPU's sleep at the run loop's top, which marks no preemption,
 * and saved and loaded again at the next entry; on a host PKRU that is not
 * 0, the guest's PKRU from its image for each run and in the image at
 * each save, and the host's in between runs and at the sleep; the guest's
 * SYSCALL_MASK with a bit of 63:32 kept from the guest and the caller
 * without EFER.SCE, refused with it, and its load once EFER.SCE is on
 * caught at its #GP; under a host DR7 that enables a breakpoint, DR7 at
 * 0x400 while DR0 to DR3 change, and the host's back after each run; the
 * first entry of a new vCPU with RSP all ones; and kvm/exit_stats's ten
 * columns in order, a run's entry, run and exit as the model's TSC steps
 * them.
 */
#include "kvm/vmx.c"

#include <setjmp.h>

#include "fs/debugfs.h"
#include "fs/devfs.h"
#include "fs/fd.h"
#include "fs/file.h"
#include "fs/sysfs.h"
#include "mm/mm.h"
#include "mm/vm.h"
#include "proc/process.h"

extern int printf(const char *format, ...);
extern int memcmp(const void *a, const void *b, unsigned long n);
extern void *malloc(unsigned long n);
extern void *aligned_alloc(unsigned long align, unsigned long n);
extern void free(void *p);

/** The device bridge is exercised by kvm_vfio_hosttest. */
int32_t kvm_vfio_create(struct kvm_vm *vm, uintptr_t arg)
{
    (void)vm;
    (void)arg;
    return -ENODEV;
}

static int failures;

/** Count and print a failure when `condition` is false. */
#define CHECK(condition, detail)                              \
    do {                                                      \
        if (!(condition)) {                                   \
            printf("kvm-vmx-hosttest: failed: %s\n", detail); \
            failures++;                                       \
        }                                                     \
    } while (0)

/* ---- the kernel's environment ---- */

static struct mm caller_mm;
static process_t caller;
static fd_entry_t entries[64];
static int reserved[64];
static fd_table_t table = {{entries}, 64, 1, 0, SPINLOCK_INIT, NULL};
struct percpu cpu_table[NR_CPUS] = {{.current = &caller}, {.current = &caller}};
unsigned int host_cpu;

void (*k_panic_hook)(void);

/* The heap's live allocations, which the last put must bring back. */
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

/** A kernel address is its own physical address here. */
uintptr_t paging_virt_to_phys(uintptr_t va)
{
    return va;
}

void kfree_pages(void *p, uint32_t count)
{
    (void)count;
    live--;
    free(p);
}

void *fpu_area_alloc(void)
{
    return kzalloc(1024);
}

void fpu_area_free(void *area)
{
    kfree(area);
}

void *k_memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;

    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

void *k_memset(void *dst, int c, size_t n)
{
    uint8_t *d = dst;

    while (n--) {
        *d++ = (uint8_t)c;
    }
    return dst;
}

static void slot_access(const void *p);

/** The caller's memory is the host's; address 0 faults. A copy into the
 * slot's memory is counted, with whether the slots' readers' lock is
 * held. */
int copy_to_user(void __user *dst, const void *src, size_t n)
{
    if (!dst) {
        return -EFAULT;
    }
    slot_access(dst);
    k_memcpy(dst, src, n);
    return 0;
}

/** As `copy_to_user`, from the caller's memory. */
int copy_from_user(void *dst, const void __user *src, size_t n)
{
    if (!src) {
        return -EFAULT;
    }
    slot_access(src);
    k_memcpy(dst, src, n);
    return 0;
}

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

static struct devfs_node *registered;

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

/* exitstat.c's debug file kvm/exit_stats: its hooks and their data, which
 * exitstat_rows reads and writes through. */
static const struct debugfs_text_ops *exitstat_ops;
static void *exitstat_data;

int32_t debugfs_create_text(const char *path, uint32_t mode, const struct debugfs_text_ops *ops,
                            void *data)
{
    (void)path;
    (void)mode;
    exitstat_ops = ops;
    exitstat_data = data;
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
}

void mm_unhold(struct mm *mm)
{
    (void)mm;
}

/** The caller holds every capability, as root does. */
int cap_holds(const process_t *p, uint32_t cap)
{
    (void)p;
    (void)cap;
    return 1;
}

void rtc_raw_monotonic(int64_t *s, int64_t *ns)
{
    *s = 5;
    *ns = 0;
}

void rtc_realtime(int64_t *s, int64_t *ns)
{
    *s = 1800000000;
    *ns = 0;
}

/* The TSC that arch_cycles reads. While `tsc_steps` is set, the entry's
 * RDMSR of DEBUGCTL adds 3 to it, the guest's run 1000 and the exit's
 * VMREAD of its reason 7, so the cycles kvm/exit_stats gives a run's
 * entry, run and exit are known. */
static uint64_t model_tsc = 1000000;
static int tsc_steps;

uint64_t arch_cycles(void)
{
    return model_tsc;
}

/* The big lock's depth and the interrupt flag. KVM_RUN is called without
 * the big lock, as the system call layer calls an unlocked file's
 * operations. */
static int bkl_depth, irq_on = 1;

/** Take the big lock: one level deeper. */
void bkl_enter(void)
{
    bkl_depth++;
}

/** Give back one level of the big lock. */
void bkl_exit(void)
{
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
    (void)cpu;
}

int signal_pending(void)
{
    return 0;
}

static void model_switch(void);

/** The run loop's switch at a slice's end, which a row asks for. */
void sched_take_preempt(void)
{
    model_switch();
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
    (void)cond;
    (void)arg;
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

static int warnings; /* klog's warnings */

void klog(int level, const char *subsystem, const char *format, ...)
{
    (void)subsystem;
    (void)format;
    warnings += level == KLOG_WARN;
}

void *kmap_temp(uintptr_t phys)
{
    return (void *)phys;
}

void kunmap_temp(void *va)
{
    (void)va;
}

static const struct vm_device_ops *map_ops;
static void *map_obj;

int vm_device_map(vma_t *vma, const struct vm_device_ops *ops, void *obj, uint32_t cache)
{
    (void)vma;
    (void)cache;
    map_ops = ops;
    map_obj = obj;
    return 0;
}

/* The caller's region over guest memory, which is never memfd_secret's. */
static vma_t guest_vma;

/** The region at `addr`: always the plain one. */
vma_t *vm_find(vm_space_t *vm, uintptr_t addr)
{
    (void)vm;
    (void)addr;
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

/* The CPUID of a Haswell, as tests/host/kvm_hosttest.c gives it, with leaf
 * 1's signature a desktop Haswell's, 0x306C3, model 0x3C; the Lenovo's
 * i3-4030U, 0x40651, model 0x45, replaces it only in its own row.
 * `cpuid_vmx` clears VMX for the probe's refusal, and `cpuid_sig` is leaf
 * 1's signature, which the probe's erratum rows change. */
static int cpuid_vmx = 1;
static uint32_t cpuid_sig = 0x000306C3u;

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
        r[0] = cpuid_sig;
        r[1] = 0x00100800;
        r[2] = cpuid_vmx ? 0x7FFAFBBFu : 0x7FFAFB9Fu;
        r[3] = 0xBFEBFBFFu;
        break;
    case 7:
        if (sub == 0) {
            r[1] = 0x000027ABu;
            r[3] = 0x9C000400u;
        }
        break;
    case 0xD:
        if (sub == 0) {
            r[0] = 0x7;
            r[1] = r[2] = 832;
        } else if (sub == 1) {
            r[0] = 0x1;
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
    case 0x80000008u:
        r[0] = 0x3027;
        break;
    default:
        break;
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

void kcpu_rdmsr(uint32_t msr, uint32_t *lo, uint32_t *hi)
{
    (void)msr;
    *lo = 0x5;
    *hi = 0;
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

/** The model has no AMD PMU. */
int kcpu_rdmsr_safe(uint32_t msr, uint64_t *value)
{
    (void)msr;
    (void)value;
    return -1;
}

/** The model has no AMD PMU. */
int kcpu_wrmsr_safe(uint32_t msr, uint64_t value)
{
    (void)msr;
    (void)value;
    return -1;
}

/** No virtual APIC is armed for a performance interrupt here. */
void kvm_lapic_pmi(struct kvm_vcpu *v)
{
    (void)v;
}

/* ---- the later tasks' files, faked ---- */

/* The one memory slot, and the tables' faults. */
static struct kvm_userspace_memory_region slot0;
static int tdp_faults, tdp_fault_write, dirty_marks;
static uint64_t tdp_fault_gfn;
#define TDP_ROOT 0x7000ull

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
    slot0 = *r;
    return 0;
}

int32_t kvm_slots_check_extension(long cap)
{
    return cap == KVM_CAP_USER_MEMORY;
}

/* The slots' readers' lock: the holds now, the copies through the slot's
 * memory, and the copies and dirty marks made with no hold, which a change
 * of the slot would not wait for. */
static int slot_holds, slot_copies, unheld_copies;

/** Count an access of the kernel's to the caller's memory at `p`, when it
 * is the slot's. */
static void slot_access(const void *p)
{
    uintptr_t a = (uintptr_t)p;

    if (a >= slot0.userspace_addr && a - slot0.userspace_addr < slot0.memory_size) {
        slot_copies++;
        unheld_copies += slot_holds == 0;
    }
}

/** A copy's hold of the slots, counted; nothing changes them in the
 * model. */
void kvm_slots_read_lock(struct kvm_vm *vm)
{
    (void)vm;
    slot_holds++;
}

/** The end of a copy's hold. */
void kvm_slots_read_unlock(struct kvm_vm *vm)
{
    (void)vm;
    slot_holds--;
}

int kvm_slot_find(struct kvm_vm *vm, uint64_t gfn, struct kvm_slot_view *out)
{
    uint64_t base = slot0.guest_phys_addr >> 12;

    (void)vm;
    if (gfn < base || gfn >= base + (slot0.memory_size >> 12)) {
        return 0;
    }
    k_memset(out, 0, sizeof(*out));
    out->base_gfn = base;
    out->npages = slot0.memory_size >> 12;
    out->flags = slot0.flags;
    out->hva = slot0.userspace_addr + ((gfn - base) << 12);
    return 1;
}

void kvm_dirty_mark(struct kvm_vm *vm, const struct kvm_slot_view *s, uint64_t gfn)
{
    (void)vm;
    (void)s;
    (void)gfn;
    dirty_marks++;
    unheld_copies += slot_holds == 0;
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

uint64_t kvm_tdp_root(struct kvm_vm *vm)
{
    (void)vm;
    return TDP_ROOT;
}

int32_t kvm_tdp_pre_fault(struct kvm_vm *vm, struct kvm_pre_fault_memory *range)
{
    (void)vm;
    (void)range;
    return -ENOENT;
}

/** No thread here is switched away, so the steal time is never marked. */
int kvm_tdp_write_nofault(struct kvm_vm *vm, uint64_t gpa, const void *buf, uint32_t n,
                          uint32_t seq)
{
    (void)vm;
    (void)gpa;
    (void)buf;
    (void)n;
    (void)seq;
    return 1;
}

/** A frame of the slot is mapped; any other is MMIO. */
int kvm_tdp_fault(struct kvm_vm *vm, uint64_t gfn, int write)
{
    struct kvm_slot_view s;

    tdp_faults++;
    tdp_fault_gfn = gfn;
    tdp_fault_write = write;
    return kvm_slot_find(vm, gfn, &s) ? 0 : 1;
}

int32_t kvm_dirty_ioctl(struct kvm_vm *vm, uint32_t request, uintptr_t arg)
{
    (void)vm;
    (void)request;
    (void)arg;
    return -ENOTTY;
}

int32_t kvm_dirty_enable_manual(struct kvm_vm *vm, const struct kvm_enable_cap *cap)
{
    (void)vm;
    (void)cap;
    return -EINVAL;
}

int32_t kvm_irq_ioctl(struct kvm_vm *vm, uint32_t request, uintptr_t arg)
{
    (void)vm;
    (void)request;
    (void)arg;
    return -ENOTTY;
}

/** KVM_CAP_SPLIT_IRQCHIP: the local APIC in the kernel, as irq.c sets it
 * for the vCPUs made after. */
int32_t kvm_irq_enable_split(struct kvm_vm *vm, const struct kvm_enable_cap *cap)
{
    (void)cap;
    vm->irqchip = KVM_IRQCHIP_SPLIT;
    return 0;
}

int32_t kvm_irq_check_extension(long cap)
{
    (void)cap;
    return 0;
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
    (void)cap;
    return 0;
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

static void model_block(struct kvm_vcpu *v);

/** The local APIC's work before an entry: a halted vCPU's sleep when a
 * row asks for one. */
int32_t kvm_lapic_prepare(struct kvm_vcpu *v)
{
    model_block(v);
    return 0;
}

int kvm_lapic_pending(struct kvm_vcpu *v)
{
    (void)v;
    return -1;
}

int kvm_lapic_ack(struct kvm_vcpu *v)
{
    (void)v;
    return -1;
}

/** vlapic.c's bound of a run by the APIC timer's next expiry: no timer is
 * armed here, so the run is bounded by `max` alone. */
uint64_t kvm_lapic_run_bound(struct kvm_vcpu *v, uint64_t max)
{
    (void)v;
    return max;
}

/* The APIC's register page of the vCPU of a VM with the local APIC in the
 * kernel, vlapic.c's, for the one such vCPU a row runs at a time: TPR at
 * 0x80, which the guest's MOV to CR8 writes without an exit under the TPR
 * shadow. */
static volatile uint32_t apic_page[1024] __attribute__((aligned(4096)));
static int apic_irr = -1; /* the highest vector in IRR, -1 for none */
static int ppr_updates;   /* `kvm_lapic_update_ppr`'s calls */

uint64_t kvm_lapic_cr8(struct kvm_vcpu *v)
{
    return v->vm->irqchip != KVM_IRQCHIP_NONE ? (apic_page[0x80 / 4] >> 4) & 0xFu : v->sregs.cr8;
}

void kvm_lapic_set_cr8(struct kvm_vcpu *v, uint64_t cr8)
{
    if (v->vm->irqchip != KVM_IRQCHIP_NONE) {
        apic_page[0x80 / 4] = (uint32_t)(cr8 & 0xFu) << 4;
    } else {
        v->sregs.cr8 = cr8;
    }
}

uintptr_t kvm_lapic_page(struct kvm_vcpu *v)
{
    return v->vm->irqchip != KVM_IRQCHIP_NONE ? (uintptr_t)apic_page : 0;
}

int kvm_lapic_highest_irr(struct kvm_vcpu *v)
{
    return v->vm->irqchip != KVM_IRQCHIP_NONE ? apic_irr : -1;
}

void kvm_lapic_update_ppr(struct kvm_vcpu *v)
{
    ppr_updates++;
    kvm_vcpu_kick(v, KVM_REQ_EVENT);
}

int kvm_lapic_set_base(struct kvm_vcpu *v, uint64_t value, int host)
{
    (void)host;
    v->sregs.apic_base = value;
    return 0;
}

/** The TSC deadline reads 0 and takes any write; the rest are not the
 * APIC's, as vlapic.c answers with no APIC in the kernel. */
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

/* What a KVM hypercall calls of vlapic.c: KVM_HC_KICK_CPU's wake, which
 * finds no APIC, and KVM_HC_SEND_IPI's, which the VM without the split
 * controller never reaches. */

int kvm_lapic_deliver(struct kvm_vm *vm, struct kvm_vcpu *src, const struct kvm_lapic_irq *irq)
{
    (void)vm;
    (void)src;
    (void)irq;
    return 0;
}

int64_t kvm_lapic_pv_ipi(struct kvm_vm *vm, uint64_t low, uint64_t high, uint32_t min, uint64_t icr,
                         uint32_t bits)
{
    (void)vm;
    (void)low;
    (void)high;
    (void)min;
    (void)icr;
    (void)bits;
    return 0;
}

/* What hyperv.c calls of vlapic.c, eventfd.c and the clock, with no APIC
 * in the kernel, no eventfd and no TSC rate measured. */

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

uint32_t timer_calibrate(void)
{
    return 0;
}

/* The vDSO's clock, which does not run from the TSC here. */
int vclock_running(void)
{
    return 0;
}

int vclock_read_cycles(int which, uint64_t *ns, uint64_t *cycles)
{
    (void)which;
    (void)ns;
    (void)cycles;
    return 0;
}

void arch_relax(void)
{}

/* SVM, which this processor does not have. */
static int svm_probe(void)
{
    return 0;
}

const struct kvm_backend kvm_svm = {"svm", svm_probe, NULL, NULL, NULL, NULL,    NULL,
                                    NULL,  NULL,      NULL, NULL, NULL, &vmx_tdp};

/* ---- the model of the Lenovo's VMX ---- */

/* The SDM's VM-instruction errors the model gives. */
#define ERR_VMCLEAR_VMXON      3
#define ERR_LAUNCH_NOTCLEAR    4
#define ERR_RESUME_NOTLAUNCHED 5
#define ERR_CONTROLS           7
#define ERR_HOST               8
#define ERR_VMPTRLD_VMXON      10
#define ERR_VMPTRLD_REV        11
#define ERR_FIELD              12
#define ERR_READONLY           13
#define ERR_VMXON_IN_VMX       15

/* The exceptions that always exit with EPT, as Linux's
 * vmx_update_exception_bitmap has them: #DB (1), #UD (6), #AC (17) and
 * #MC (18), written out here so that a change to vmx.c's EXC_ALWAYS fails a
 * row. */
#define EXC_LINUX 0x00060042u

#define EXIT_RIP_MODEL  0xFFFFFFFF80123450ull
#define HOST_CR4_MODEL  0x000406F0ull
#define HOST_XCR0_MODEL 0x7ull /* x87, SSE and AVX */
/* The host's PKRU, as a host with protection keys on keeps it: Linux's
 * init_pkru_value, every key but 0 denied writes. The Lenovo's Haswell has
 * no protection keys, so the model's XCR0 lacks PKRU's bit 9 and its CR4
 * and IA32_VMX_CR4_FIXED1 lack PKE, which fpu_cpu_setup turns on only with
 * that bit; the backend reaches PKRU only through `rdpkru` and `wrpkru`,
 * which never read XCR0, so a PKRU that is not 0 makes its switch visible
 * on the Lenovo's model, and `m_fpu_load` and `m_fpu_store` move PKRU with
 * the image as XSAVE and XRSTOR do under an XCR0 with the bit. */
#define HOST_PKRU_MODEL 0x55555554u

/* A VMCS the model knows: its address, launch state, the processor where
 * it is active and fields by encoding. */
struct mvmcs {
    uint64_t pa;
    int launched;
    int active; /* the processor where it is active, plus one; 0 for none */
    uint64_t f[0x7000];
};

/* A processor's VMX state. */
struct mproc {
    int on;
    uint64_t cr4;
    uint64_t vmxon_pa;
    struct mvmcs *cur;
};

/* A guest's run, a row of the script: the exit and what it leaves. */
struct row {
    uint32_t reason;
    uint64_t qual, gpa;
    uint32_t intr_info, intr_error, idt_info, idt_error, len, insn_info, intr_state;
    int panic;      /* a panic comes inside the run */
    uint64_t rip;   /* the guest's RIP at the exit, when it moved without one */
    uint32_t cs_ar; /* CS's access rights at the exit, when a far jump changed them */
    uint64_t cr4;   /* CR4's bits the guest set in the run without an exit, or 0 */
    uint64_t kgs;   /* KERNEL_GS_BASE after the guest's SWAPGS in the run, or 0 */
    /* The guest's MOV to CR8 under the TPR shadow, before the exit: the
     * processor writes `tpr` to the virtual-APIC page without one. */
    int tpr_write;
    uint32_t tpr;
};

/* The VMCSes the model knows, one for each vCPU any row has run. */
#define MVMCS_MAX 16

static struct {
    /* The capability MSRs and IA32_FEATURE_CONTROL. */
    uint64_t basic, pin, proc, exit, entry, misc, proc2, ept_vpid;
    uint64_t fc;
    int fc_writes;
    /* The running processor's state, `host_cpu`'s; `cpus` keeps the
     * others' (`on_cpu`). */
    int on;
    uint64_t cr4;
    uint64_t vmxon_pa;
    struct mvmcs *cur;
    struct mproc cpus[2];
    int calls;                    /* `smp_call`s to another processor */
    struct mvmcs vmcs[MVMCS_MAX]; /* every VMCS the rows used, never forgotten */
    int nvmcs;
    /* The debug registers; every exit sets DR7 to 0x400 (SDM 28.5.1). */
    uint64_t dr[8];
    int dr6_writes;
    /* IA32_DEBUGCTL, which every exit clears (SDM 28.5.1), its writes, and
     * the guest's as the last entry loaded it. */
    uint64_t debugctl;
    int debugctl_writes;
    uint64_t entry_debugctl;
    /* The counts of each instruction, and its failures. */
    int vmxons, vmxoffs, vmclears, vmptrlds, launches, resumes, invepts, invvpids;
    int ud, gp, bad_writes, bad_reads, misplaced;
    int nmis, mces, wbinvds;
    /* The host's state `save_host` took, until `restore_host` puts it
     * back; the guest's FPU, from `fpu_load` with the host's saved until
     * `fpu_store` loads the host's again; each counted, with the image
     * loaded again (`fpu_reloads`) and saved for a reader (`fpu_syncs`). */
    int saved, saves, restores;
    int fpu_held, fpu_loads, fpu_stores, fpu_reloads, fpu_syncs;
    /* XCR0 and PKRU, the host's values while no guest runs; the MSRs
     * `msr_slot` keeps, with the WRMSRs of them counted. `fpu_load` with
     * the host's area saves PKRU in `host_pkru` and both forms load the
     * image's, `image_pkru`; `fpu_store` saves PKRU into the image and with
     * the host's area loads `host_pkru` again, as XSAVE and XRSTOR move it
     * under an XCR0 with PKRU. */
    uint64_t xcr0, fs_base;
    uint32_t pkru, host_pkru, image_pkru;
    uint64_t msr[5];
    int msr_writes;
    int msr_faults; /* the safe WRMSRs refused with #GP */
    /* What the last entry found: the processor's STAR, SYSCALL_MASK and
     * KERNEL_GS_BASE, XCR0 and the WRMSRs so far. */
    uint64_t entry_star, entry_sfmask, entry_kgs, entry_xcr0;
    int entry_msr_writes, first_msr_writes;
    /* The turn of the run loop at which it switches away
     * (`sched_take_preempt`), counting down, and the switches taken. */
    int preempt, switches;
    /* The pass over the run loop's top at which a halted vCPU sleeps
     * (`kvm_lapic_prepare`), counting down, and the sleeps taken. */
    int block, blocks;
    int fail_vmxon, fail_vmptrld, fail_launch;
    uint32_t refuse; /* a field whose VMWRITE fails as unsupported, or 0 */
    uint64_t cr0;    /* the host's CR0 */
    /* The script, and what the last entry injected. */
    struct row script[16];
    int script_len, script_at;
    uint32_t injected, injected_error;
    uint64_t entry_cr0, entry_proc, entry_rflags, entry_dr0, entry_intr;
    const char *why; /* the last entry check that failed */
} m;

/* The MSRs the model keeps, as `msr` holds them. */
static const uint32_t kept_msrs[5] = {MSR_STAR, MSR_LSTAR, MSR_SYSCALL_MASK, MSR_TSC_AUX,
                                      MSR_KERNEL_GS_BASE};

/** The host's own value of a kept MSR, before any guest. */
#define HOST_MSR(msr) (0x1000ull + (msr))

/** The index in `m.msr` of `msr`, or -1 for one the model does not keep. */
static int msr_slot(uint32_t msr)
{
    int i;

    for (i = 0; i < 5; i++) {
        if (kept_msrs[i] == msr) {
            return i;
        }
    }
    return -1;
}

/** Whether each kept MSR holds the host's value. */
static int host_msrs(void)
{
    int i;

    for (i = 0; i < 5; i++) {
        if (m.msr[i] != HOST_MSR(kept_msrs[i])) {
            return 0;
        }
    }
    return 1;
}

static jmp_buf panic_jump;

/** The VMCS whose address is `pa`, made known on its first use. */
static struct mvmcs *mvmcs_of(uint64_t pa)
{
    int i;

    for (i = 0; i < m.nvmcs; i++) {
        if (m.vmcs[i].pa == pa) {
            return &m.vmcs[i];
        }
    }
    if (m.nvmcs == MVMCS_MAX) {
        return NULL;
    }
    k_memset(&m.vmcs[m.nvmcs], 0, sizeof(m.vmcs[0]));
    m.vmcs[m.nvmcs].pa = pa;
    return &m.vmcs[m.nvmcs++];
}

/** Whether `f` is in [`lo`, `hi`] and, with `even`, a full field's even
 * encoding. */
static int in(uint32_t f, uint32_t lo, uint32_t hi, int even)
{
    return f >= lo && f <= hi && (!even || !(f & 1u));
}

/** Whether `field` is a field of the Haswell's VMCS, or the TSC
 * multiplier on a model whose controls have TSC scaling. */
static int field_known(uint64_t field)
{
    static const uint32_t ranges[][3] = {
        {0x0000, 0x0000, 1}, {0x0800, 0x080E, 1}, {0x0C00, 0x0C0C, 1}, {0x2000, 0x202D, 0},
        {0x2400, 0x2401, 0}, {0x2800, 0x2811, 0}, {0x2C00, 0x2C05, 0}, {0x4000, 0x4022, 1},
        {0x4400, 0x440E, 1}, {0x4800, 0x482A, 1}, {0x482E, 0x482E, 1}, {0x4C00, 0x4C00, 1},
        {0x6000, 0x600E, 1}, {0x6400, 0x640A, 1}, {0x6800, 0x6826, 1}, {0x6C00, 0x6C16, 1}};
    uint32_t i;

    if (field >> 32) {
        return 0;
    }
    if (field == TSC_MULTIPLIER) {
        return ((m.proc2 >> 32) & PROC2_TSC_SCALING) != 0;
    }
    for (i = 0; i < sizeof(ranges) / sizeof(ranges[0]); i++) {
        if (in((uint32_t)field, ranges[i][0], ranges[i][1], (int)ranges[i][2])) {
            return 1;
        }
    }
    return 0;
}

/** The bits a field of `field`'s width keeps. */
static uint64_t field_mask(uint32_t field)
{
    switch ((field >> 13) & 3u) {
    case 0:
        return 0xFFFFull;
    case 2:
        return 0xFFFFFFFFull;
    default:
        return field & 1u ? 0xFFFFFFFFull : ~0ull;
    }
}

/** A VMX instruction outside VMX operation: #UD, counted. */
static int not_on(void)
{
    m.ud++;
    return VMX_FAIL_INVALID;
}

/** VMXON: #UD without CR4.VMXE, #GP unless IA32_FEATURE_CONTROL is
 * locked with VMX on, VMfailValid in VMX operation, VMfailInvalid for a
 * region misaligned or of the wrong revision. */
static int m_vmxon(uint64_t *pa)
{
    if (!(m.cr4 & CR4_VMXE)) {
        return not_on();
    }
    if ((m.fc & 5u) != 5u || (m.cr0 & 0x80000021ull) != 0x80000021ull) {
        m.gp++;
        return VMX_FAIL_INVALID;
    }
    if (m.on) {
        return VMX_FAIL_VALID;
    }
    if (m.fail_vmxon || (*pa & 0xFFFu) || *(uint32_t *)(uintptr_t)*pa != (uint32_t)m.basic) {
        return VMX_FAIL_INVALID;
    }
    m.vmxons++;
    m.on = 1;
    m.vmxon_pa = *pa;
    m.cur = NULL;
    return VMX_OK;
}

/** Whether a VMCS is active on the processor `cpu`. */
static int any_active(int cpu)
{
    int i;

    for (i = 0; i < m.nvmcs; i++) {
        if (m.vmcs[i].active == cpu + 1) {
            return 1;
        }
    }
    return 0;
}

/** VMXOFF: #UD outside VMX operation. */
static int m_vmxoff(void)
{
    if (!m.on) {
        return not_on();
    }
    CHECK(!any_active((int)host_cpu), "VMCLEAR writes each active VMCS back before VMXOFF");
    m.vmxoffs++;
    m.on = 0;
    m.cur = NULL;
    return VMX_OK;
}

/** VMCLEAR: the VMCS clear and no longer current; VMfailValid for the
 * VMXON region. */
static int m_vmclear(uint64_t *pa)
{
    struct mvmcs *c;

    if (!m.on) {
        return not_on();
    }
    m.vmclears++;
    if (*pa == m.vmxon_pa) {
        return VMX_FAIL_VALID;
    }
    c = mvmcs_of(*pa);
    if (c->active && c->active != (int)host_cpu + 1) {
        m.misplaced++;
    }
    c->launched = 0;
    c->active = 0;
    if (m.cur == c) {
        m.cur = NULL;
    }
    return VMX_OK;
}

/** VMPTRLD: VMfailValid for the VMXON region or the wrong revision. A VMCS
 * active on another processor is counted as misplaced, since the SDM leaves
 * its data undefined until VMCLEAR runs there (24.11.1). */
static int m_vmptrld(uint64_t *pa)
{
    if (!m.on) {
        return not_on();
    }
    m.vmptrlds++;
    if (m.fail_vmptrld || *pa == m.vmxon_pa || *(uint32_t *)(uintptr_t)*pa != (uint32_t)m.basic) {
        return VMX_FAIL_VALID;
    }
    m.cur = mvmcs_of(*pa);
    if (m.cur->active && m.cur->active != (int)host_cpu + 1) {
        m.misplaced++;
    }
    m.cur->active = (int)host_cpu + 1;
    return VMX_OK;
}

/** Make `cpu` the running processor, the model's state its own. */
static void on_cpu(int cpu)
{
    struct mproc *o = &m.cpus[host_cpu], *n = &m.cpus[cpu];

    o->on = m.on;
    o->cr4 = m.cr4;
    o->vmxon_pa = m.vmxon_pa;
    o->cur = m.cur;
    host_cpu = (unsigned int)cpu;
    m.on = n->on;
    m.cr4 = n->cr4;
    m.vmxon_pa = n->vmxon_pa;
    m.cur = n->cur;
}

/** Run `fn` on the processor `cpu`, as its interprocessor interrupt would,
 * with interrupts off there. */
void smp_call(int cpu, void (*fn)(void *), void *arg)
{
    int back = (int)host_cpu, irq = irq_on;

    if (cpu != back) {
        m.calls++;
        on_cpu(cpu);
    }
    irq_on = 0;
    fn(arg);
    irq_on = irq;
    if (cpu != back) {
        on_cpu(back);
    }
}

/** Whether the processor `cpu` is in VMX operation exactly while a VMCS is
 * active there, with CR4 as it was but for VMXE, and the backend agrees. */
static int cpu_ok(int cpu)
{
    int on = cpu == (int)host_cpu ? m.on : m.cpus[cpu].on;
    uint64_t cr4 = cpu == (int)host_cpu ? m.cr4 : m.cpus[cpu].cr4;

    return on == any_active(cpu) && on == vmx_live[cpu] &&
           cr4 == (HOST_CR4_MODEL | (on ? CR4_VMXE : 0u));
}

/** VMREAD: VMfailInvalid with no current VMCS, VMfailValid for a field
 * the VMCS lacks. */
static int m_vmread(uint64_t field, uint64_t *value)
{
    if (!m.on) {
        return not_on();
    }
    if (!m.cur) {
        return VMX_FAIL_INVALID;
    }
    if (!field_known(field)) {
        m.bad_reads++;
        return VMX_FAIL_VALID;
    }
    *value = m.cur->f[field];
    model_tsc += tsc_steps && field == VM_EXIT_REASON ? 7u : 0u;
    return VMX_OK;
}

/** VMWRITE, kept to the field's width: VMfailValid for a field the VMCS
 * lacks or an exit-information field, which the backend never writes. */
static int m_vmwrite(uint64_t field, uint64_t value)
{
    if (!m.on) {
        return not_on();
    }
    if (!m.cur) {
        return VMX_FAIL_INVALID;
    }
    if (m.refuse && field == m.refuse) {
        m.cur->f[VM_INSTRUCTION_ERROR] = 12;
        return VMX_FAIL_VALID;
    }
    if (!field_known(field) || ((field >> 10) & 3u) == 1u) {
        printf("kvm-vmx-hosttest: VMWRITE refused for field 0x%x\n", (unsigned int)field);
        m.bad_writes++;
        return VMX_FAIL_VALID;
    }
    m.cur->f[field] = value & field_mask((uint32_t)field);
    return VMX_OK;
}

/** INVEPT, which must name the VM's own tables. */
static int m_invept(uint64_t type, const uint64_t *desc)
{
    if (!m.on) {
        return not_on();
    }
    m.invepts++;
    CHECK(type == INV_SINGLE && (desc[0] & ~0xFFFull) == TDP_ROOT, "INVEPT names the VM's tables");
    return VMX_OK;
}

/** INVVPID, which must name a vCPU's VPID, never the host's. */
static int m_invvpid(uint64_t type, const uint64_t *desc)
{
    if (!m.on) {
        return not_on();
    }
    m.invvpids++;
    CHECK(type == INV_SINGLE && desc[0] != 0, "INVVPID names the vCPU's own VPID");
    return VMX_OK;
}

/** The control word `value` is allowed by the capability MSR `cap`. */
static int ctl_ok(uint64_t value, uint64_t cap)
{
    return (value & (uint32_t)cap) == (uint32_t)cap && (value & ~(cap >> 32)) == 0;
}

/** The access rights `ar` of a usable code or data segment are accessed,
 * and a code segment's readable. */
static int seg_accessed(uint64_t ar)
{
    return (ar & 0x10000u) || (ar & 1u);
}

/** Fail the entry check `why`: 0 for the caller to give the error. */
static int fail(const char *why)
{
    m.why = why;
    return 0;
}

/** The checks of the event the entry injects (SDM 26.2.1.3): 1 when they
 * pass. */
static int check_injection(const uint64_t *f)
{
    uint64_t info = f[VM_ENTRY_INTR_INFO_FIELD];
    uint32_t type = INTR_TYPE(info), vec = INTR_VECTOR(info);
    int soft = type == TYPE_SOFT_INTR || type == TYPE_PRIV_SOFT || type == TYPE_SOFT_EXC;

    if (!(info & INTR_VALID)) {
        return 1;
    }
    if ((type == TYPE_NMI && vec != NMI_VECTOR) || (type == TYPE_HW_EXC && vec > 31u) ||
        type == 1u || type == 7u) {
        return fail("an event of the wrong type for its vector");
    }
    if ((info & INTR_ERRCODE) &&
        (type != TYPE_HW_EXC || !(f[GUEST_CR0] & CR0_PE) || !has_error_code(vec))) {
        return fail("an error code the event does not take");
    }
    if (soft && (f[VM_ENTRY_INSTRUCTION_LEN] == 0 || f[VM_ENTRY_INSTRUCTION_LEN] > 15)) {
        return fail("a software event without its length");
    }
    return 1;
}

/** The checks of the TPR shadow without virtual-interrupt delivery (SDM
 * 26.2.1.1): 1 when they pass. A page at 0 is the backend's fault alone,
 * which would keep the guest's TPR in the host's page 0. */
static int check_tpr_shadow(const uint64_t *f)
{
    uint64_t page = f[VIRTUAL_APIC_PAGE_ADDR];

    if ((page & 0xFFFu) || !page) {
        return fail("a virtual-APIC page misaligned or at 0");
    }
    if (f[TPR_THRESHOLD] & ~0xFull) {
        return fail("a TPR threshold past bit 3");
    }
    if (f[TPR_THRESHOLD] > ((*(volatile uint32_t *)(uintptr_t)(page + 0x80u) >> 4) & 0xFu)) {
        return fail("a TPR threshold above VTPR's class");
    }
    return 1;
}

/** The checks of the controls (SDM 26.2.1): 1 when they pass. */
static int check_controls(const uint64_t *f)
{
    uint64_t pin = f[PIN_BASED_VM_EXEC_CONTROL], proc = f[CPU_BASED_VM_EXEC_CONTROL];
    uint64_t proc2 = (proc & PROC_SECONDARY) ? f[SECONDARY_VM_EXEC_CONTROL] : 0;
    uint64_t eptp = f[EPT_POINTER];

    if (!ctl_ok(pin, m.pin) || !ctl_ok(proc, m.proc) || (proc2 & ~(m.proc2 >> 32)) ||
        !ctl_ok(f[VM_EXIT_CONTROLS], m.exit) || !ctl_ok(f[VM_ENTRY_CONTROLS], m.entry)) {
        return fail("a control the capability MSRs do not allow");
    }
    if (((proc & PROC_NMI_WINDOW) && !(pin & PIN_VIRT_NMI)) ||
        ((pin & PIN_VIRT_NMI) && !(pin & PIN_NMI))) {
        return fail("the NMI window without virtual NMIs");
    }
    if ((proc2 & PROC2_UG) && !(proc2 & PROC2_EPT)) {
        return fail("the unrestricted guest without EPT");
    }
    if ((proc2 & PROC2_VPID) && f[VIRTUAL_PROCESSOR_ID] == 0) {
        return fail("VPID 0");
    }
    if ((proc2 & PROC2_EPT) && ((eptp & 7u) != 6u || ((eptp >> 3) & 7u) != 3u || (eptp & 0xF80u))) {
        return fail("an EPT pointer of the wrong type or walk");
    }
    if ((f[MSR_BITMAP] & 0xFFFu) || (f[VM_ENTRY_MSR_LOAD_ADDR] & 0xFu) ||
        (f[VM_EXIT_MSR_STORE_ADDR] & 0xFu) || (f[VM_EXIT_MSR_LOAD_ADDR] & 0xFu)) {
        return fail("an MSR area misaligned");
    }
    if ((proc & PROC_TPR_SHADOW) && !check_tpr_shadow(f)) {
        return 0;
    }
    return check_injection(f);
}

/** The checks of the host state (SDM 26.2.2 and 26.2.3): 1 when they
 * pass. */
static int check_host(const uint64_t *f)
{
    if ((f[HOST_CR0] & 0x80000021ull) != 0x80000021ull || !(f[HOST_CR4] & CR4_VMXE) ||
        !(f[HOST_CR4] & CR4_PAE)) {
        return fail("the host's CR0 or CR4");
    }
    if (!(f[VM_EXIT_CONTROLS] & EXIT_HOST_64) || (f[HOST_IA32_EFER] & 0x500u) != 0x500u) {
        return fail("a 64-bit host without long mode");
    }
    if (f[HOST_CS_SELECTOR] == 0 || f[HOST_TR_SELECTOR] == 0 || (f[HOST_CS_SELECTOR] & 7u) ||
        (f[HOST_SS_SELECTOR] & 7u) || (f[HOST_TR_SELECTOR] & 7u) || f[HOST_DS_SELECTOR] ||
        f[HOST_ES_SELECTOR] || f[HOST_FS_SELECTOR] || f[HOST_GS_SELECTOR]) {
        return fail("the host's selectors");
    }
    if (f[HOST_RIP] != EXIT_RIP_MODEL || f[HOST_TR_BASE] == 0 || f[HOST_GDTR_BASE] == 0) {
        return fail("the host's RIP or bases");
    }
    return 1;
}

/** The PAT value `pat` has a valid type in each byte. */
static int pat_ok(uint64_t pat)
{
    int i;
    uint32_t t;

    for (i = 0; i < 8; i++) {
        t = (uint32_t)(pat >> (i * 8)) & 0xFFu;
        if (t != 0 && t != 1 && t != 4 && t != 5 && t != 6 && t != 7) {
            return 0;
        }
    }
    return 1;
}

/** The limit `limit` of a segment with the access rights `ar` agrees with
 * its granularity, as the SDM's 26.3.1.2 asks of a usable segment, and of
 * TR, which `always` names: G clear when a bit of the limit's low 12 is
 * clear, G set when a bit of its high 12 is set. */
static int seg_limit_ok(uint64_t limit, uint64_t ar, int always)
{
    int g = (ar & 0x8000u) != 0;

    if ((ar & 0x10000u) && !always) {
        return 1;
    }
    return !((limit & 0xFFFu) != 0xFFFu && g) && !((limit & 0xFFF00000u) && !g);
}

/** The checks of the guest's segments in the state `f` (SDM 26.3.1.2). */
static int check_segments(const uint64_t *f)
{
    uint64_t cs = f[GUEST_ES_AR_BYTES + 2u * SEG_CS], tr = f[GUEST_ES_AR_BYTES + 2u * SEG_TR];
    uint64_t ldtr = f[GUEST_ES_AR_BYTES + 2u * SEG_LDTR], ss = f[GUEST_ES_AR_BYTES + 2u * SEG_SS];
    int n;

    if ((cs & 0x10000u) || ((cs & 15u) != 3u && (cs & 9u) != 9u)) {
        return fail("CS's type");
    }
    if ((f[VM_ENTRY_CONTROLS] & ENTRY_IA32E) && (cs & (1u << 13)) && (cs & (1u << 14))) {
        return fail("CS with both L and D");
    }
    if (!(ss & 0x10000u) && (ss & 15u) != 3u && (ss & 15u) != 7u) {
        return fail("SS's type");
    }
    for (n = SEG_ES; n <= SEG_GS; n++) {
        if (!seg_accessed(f[GUEST_ES_AR_BYTES + 2u * (uint32_t)n])) {
            return fail("a usable segment not accessed");
        }
    }
    for (n = SEG_ES; n <= SEG_TR; n++) {
        if (!seg_limit_ok(f[GUEST_ES_LIMIT + 2u * (uint32_t)n],
                          f[GUEST_ES_AR_BYTES + 2u * (uint32_t)n], n == SEG_TR)) {
            return fail("a segment's limit against its granularity");
        }
    }
    if ((tr & 0x10000u) || ((tr & 15u) != 3u && (tr & 15u) != 11u) || (tr & 0x10u) ||
        !(tr & 0x80u)) {
        return fail("TR");
    }
    if (!(ldtr & 0x10000u) && ((ldtr & 15u) != 2u || (ldtr & 0x10u) || !(ldtr & 0x80u))) {
        return fail("LDTR");
    }
    if (f[GUEST_GDTR_LIMIT] > 0xFFFFu || f[GUEST_IDTR_LIMIT] > 0xFFFFu) {
        return fail("a table's limit");
    }
    return 1;
}

/** The checks of the guest's control registers, EFER, PAT and PDPTEs. */
static int check_guest_regs(const uint64_t *f)
{
    uint64_t cr0 = f[GUEST_CR0], cr4 = f[GUEST_CR4], efer = f[GUEST_IA32_EFER];
    int ia32e = (f[VM_ENTRY_CONTROLS] & ENTRY_IA32E) != 0, i;

    if (((cr0 | CR0_PE | CR0_PG) & 0x80000021ull) != 0x80000021ull ||
        ((cr0 & CR0_PG) && !(cr0 & CR0_PE))) {
        return fail("the guest's CR0");
    }
    if ((cr4 & 0x2000u) != 0x2000u || (cr4 & ~0x1767FFull)) {
        return fail("the guest's CR4");
    }
    if (ia32e && (!(cr0 & CR0_PG) || !(cr4 & CR4_PAE))) {
        return fail("IA-32e mode without paging and PAE");
    }
    if (((efer & GEFER_LMA) != 0) != ia32e ||
        ((cr0 & CR0_PG) && ((efer & GEFER_LME) != 0) != ia32e)) {
        return fail("EFER against IA-32e mode");
    }
    if (!pat_ok(f[GUEST_IA32_PAT])) {
        return fail("the guest's PAT");
    }
    if ((cr0 & CR0_PG) && (cr4 & CR4_PAE) && !ia32e) {
        for (i = 0; i < 4; i++) {
            if ((f[GUEST_PDPTR0 + 2u * (uint32_t)i] & 1u) &&
                (f[GUEST_PDPTR0 + 2u * (uint32_t)i] & 0x1E6u)) {
                return fail("a PDPTE with a reserved bit");
            }
        }
    }
    return 1;
}

/** The checks of the guest's RFLAGS, interruptibility and event
 * (SDM 26.3.1.4 and 26.3.1.5). */
static int check_guest_events(const uint64_t *f)
{
    uint64_t rflags = f[GUEST_RFLAGS], intr = f[GUEST_INTERRUPTIBILITY_INFO];
    uint64_t info = f[VM_ENTRY_INTR_INFO_FIELD];

    if (!(rflags & 2u) || (rflags & 0xFFFFFFFFFFC08028ull)) {
        return fail("RFLAGS");
    }
    if ((intr & INTR_STI) && (intr & INTR_MOV_SS)) {
        return fail("both the STI and the MOV SS shadow");
    }
    if ((intr & INTR_STI) && !(rflags & RFLAGS_IF)) {
        return fail("the STI shadow with IF clear");
    }
    if ((info & INTR_VALID) && INTR_TYPE(info) == TYPE_EXT_INTR &&
        (!(rflags & RFLAGS_IF) || (intr & (INTR_STI | INTR_MOV_SS)))) {
        return fail("an interrupt injected that the guest blocks");
    }
    if ((info & INTR_VALID) && INTR_TYPE(info) == TYPE_NMI &&
        (intr & (INTR_MOV_SS | INTR_BLOCK_NMI))) {
        return fail("an NMI injected that the guest blocks");
    }
    if ((intr & (INTR_STI | INTR_MOV_SS)) && (rflags & RFLAGS_TF) &&
        !(f[GUEST_PENDING_DBG_EXCEPTIONS] & DR6_BS)) {
        return fail("a shadow under TF without the single-step pending");
    }
    if (f[VMCS_LINK_POINTER] != ~0ull || f[GUEST_ACTIVITY_STATE] != 0) {
        return fail("the link pointer or the activity state");
    }
    return 1;
}

/** The MSR list at `pa` of `n` entries loads no MSR the entry or exit
 * refuses (SDM 26.4): FS_BASE, GS_BASE, SMBASE and the x2APIC's. */
static int msr_list_ok(uint64_t pa, uint64_t n)
{
    const uint64_t *e = (const uint64_t *)(uintptr_t)pa;
    uint32_t idx;
    uint64_t i;

    for (i = 0; i < n; i++) {
        idx = (uint32_t)e[i * 2u];
        if (idx == 0xC0000100u || idx == 0xC0000101u || idx == 0x9Eu || (idx >> 8) == 8u ||
            (e[i * 2u] >> 32)) {
            return 0;
        }
    }
    return 1;
}

/** Leave the exit of `r` in the current VMCS, and clear the entry's
 * event, as the processor does at every exit. */
static void exit_with(const struct row *r)
{
    uint64_t *f = m.cur->f;

    f[VM_EXIT_REASON] = r->reason;
    f[EXIT_QUALIFICATION] = r->qual;
    f[GUEST_PHYSICAL_ADDRESS] = r->gpa;
    f[VM_EXIT_INTR_INFO] = r->intr_info;
    f[VM_EXIT_INTR_ERROR_CODE] = r->intr_error;
    f[IDT_VECTORING_INFO_FIELD] = r->idt_info;
    f[IDT_VECTORING_ERROR_CODE] = r->idt_error;
    f[VM_EXIT_INSTRUCTION_LEN] = r->len;
    f[VMX_INSTRUCTION_INFO] = r->insn_info;
    f[GUEST_INTERRUPTIBILITY_INFO] = r->intr_state;
    if (r->rip) {
        f[GUEST_RIP] = r->rip;
    }
    if (r->cs_ar) {
        f[GUEST_ES_AR_BYTES + 2u * SEG_CS] = r->cs_ar;
    }
    f[GUEST_CR4] |= r->cr4 & ~f[CR4_GUEST_HOST_MASK];
    f[VM_ENTRY_INTR_INFO_FIELD] &= ~(uint64_t)INTR_VALID;
}

/** Check what every entry that passes the processor's checks runs under:
 * the preemption timer, the host's state saved, the guest's FPU and PKRU
 * loaded and empty MSR lists; and note what it found, for the rows. */
static void note_entry(const uint64_t *f)
{
    CHECK((f[PIN_BASED_VM_EXEC_CONTROL] & PIN_PREEMPTION) &&
              f[VMX_PREEMPTION_TIMER_VALUE] == (VMX_RUN_BOUND_TSC >> (m.misc & 0x1Fu)),
          "every run is bounded by the preemption timer");
    CHECK(m.saved && m.fpu_held && m.pkru == m.image_pkru,
          "the guest runs with the host's state saved and the guest's FPU and PKRU loaded");
    CHECK(f[VM_ENTRY_MSR_LOAD_COUNT] == 0 && f[VM_EXIT_MSR_STORE_COUNT] == 0 &&
              f[VM_EXIT_MSR_LOAD_COUNT] == 0,
          "no MSR is switched by the entry's and the exit's lists");
    m.entry_star = m.msr[msr_slot(MSR_STAR)];
    m.entry_sfmask = m.msr[msr_slot(MSR_SYSCALL_MASK)];
    m.entry_kgs = m.msr[msr_slot(MSR_KERNEL_GS_BASE)];
    m.entry_xcr0 = m.xcr0;
    m.entry_msr_writes = m.msr_writes;
    if (m.script_at == 0) {
        m.first_msr_writes = m.msr_writes;
    }
}

/** VMLAUNCH or VMRESUME: the checks, then the script's next run. */
static int m_run(struct kvm_regs *regs, uint64_t *cr2, int launched)
{
    static const struct row none = {EXIT_HLT, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0};
    const struct row *r;
    uint64_t *f;

    (void)regs;
    (void)cr2;
    CHECK(!irq_on && bkl_depth == 0, "the guest runs with interrupts off and no big lock held");
    model_tsc += tsc_steps ? 1000u : 0u;
    if (!m.on) {
        return not_on();
    }
    if (!m.cur) {
        return VMX_FAIL_INVALID;
    }
    f = m.cur->f;
    launched ? m.resumes++ : m.launches++;
    if (launched != m.cur->launched) {
        return VMX_FAIL_VALID;
    }
    if (m.fail_launch) {
        m.fail_launch = 0;
        f[VM_INSTRUCTION_ERROR] = ERR_CONTROLS;
        return VMX_FAIL_VALID;
    }
    if (!check_controls(f)) {
        f[VM_INSTRUCTION_ERROR] = ERR_CONTROLS;
        return VMX_FAIL_VALID;
    }
    if (!check_host(f)) {
        f[VM_INSTRUCTION_ERROR] = ERR_HOST;
        return VMX_FAIL_VALID;
    }
    /* The processor reads the field only when its valid bit is set. */
    m.injected =
        (f[VM_ENTRY_INTR_INFO_FIELD] & INTR_VALID) ? (uint32_t)f[VM_ENTRY_INTR_INFO_FIELD] : 0;
    m.injected_error = (uint32_t)f[VM_ENTRY_EXCEPTION_ERROR_CODE];
    m.entry_cr0 = f[GUEST_CR0];
    m.entry_proc = f[CPU_BASED_VM_EXEC_CONTROL];
    m.entry_rflags = f[GUEST_RFLAGS];
    m.entry_intr = f[GUEST_INTERRUPTIBILITY_INFO];
    m.entry_dr0 = m.dr[0];
    m.entry_debugctl = f[GUEST_IA32_DEBUGCTL];
    if (!check_segments(f) || !check_guest_regs(f) || !check_guest_events(f)) {
        f[VM_EXIT_REASON] = EXIT_FAILED_ENTRY | EXIT_INVALID_STATE;
        f[EXIT_QUALIFICATION] = 0;
        return VMX_OK;
    }
    if (!msr_list_ok(f[VM_ENTRY_MSR_LOAD_ADDR], f[VM_ENTRY_MSR_LOAD_COUNT]) ||
        !msr_list_ok(f[VM_EXIT_MSR_LOAD_ADDR], f[VM_EXIT_MSR_LOAD_COUNT])) {
        m.why = "an MSR list";
        f[VM_EXIT_REASON] = EXIT_FAILED_ENTRY | EXIT_MSR_LOAD_FAIL;
        return VMX_OK;
    }
    note_entry(f);
    m.cur->launched = 1;
    r = m.script_at < m.script_len ? &m.script[m.script_at++] : &none;
    CHECK(r != &none, "the guest runs only as the script says");
    if (r->panic) {
        k_panic_hook();
        longjmp(panic_jump, 1);
    }
    if (r->kgs) {
        m.msr[msr_slot(MSR_KERNEL_GS_BASE)] = r->kgs;
    }
    if (r->tpr_write) {
        CHECK(f[CPU_BASED_VM_EXEC_CONTROL] & PROC_TPR_SHADOW,
              "the guest writes CR8 without an exit only under the TPR shadow");
        if (f[CPU_BASED_VM_EXEC_CONTROL] & PROC_TPR_SHADOW) {
            *(volatile uint32_t *)(uintptr_t)(f[VIRTUAL_APIC_PAGE_ADDR] + 0x80u) = r->tpr;
        }
    }
    exit_with(r);
    m.debugctl = 0;
    m.dr[7] = DR7_FIXED;
    return VMX_OK;
}

/** RDMSR of the model: the capability MSRs, the true controls and their
 * defaults, IA32_FEATURE_CONTROL, IA32_DEBUGCTL, PAT, EFER and the kept
 * MSRs; any other MSR reads as a value of its own. */
static uint64_t m_rdmsr(uint32_t msr)
{
    model_tsc += tsc_steps && msr == MSR_IA32_DEBUGCTL ? 3u : 0u;
    if (msr_slot(msr) >= 0) {
        return m.msr[msr_slot(msr)];
    }
    switch (msr) {
    case MSR_IA32_FEATURE_CONTROL:
        return m.fc;
    case MSR_IA32_VMX_BASIC:
        return m.basic;
    case MSR_IA32_VMX_PINBASED:
    case MSR_IA32_VMX_TRUE_PIN:
        return m.pin;
    case MSR_IA32_VMX_PROCBASED:
        return m.proc | 0x0001E172ull;
    case MSR_IA32_VMX_TRUE_PROC:
        return m.proc;
    case MSR_IA32_VMX_EXIT:
        return m.exit | 0x4ull;
    case MSR_IA32_VMX_TRUE_EXIT:
        return m.exit;
    case MSR_IA32_VMX_ENTRY:
        return m.entry | 0x4ull;
    case MSR_IA32_VMX_TRUE_ENTRY:
        return m.entry;
    case MSR_IA32_VMX_MISC:
        return m.misc;
    case MSR_IA32_VMX_CR0_FIXED0:
        return 0x80000021ull;
    case MSR_IA32_VMX_CR0_FIXED1:
        return 0xFFFFFFFFull;
    case MSR_IA32_VMX_CR4_FIXED0:
        return 0x2000ull;
    case MSR_IA32_VMX_CR4_FIXED1:
        return 0x1767FFull;
    case MSR_IA32_VMX_PROCBASED2:
        return m.proc2;
    case MSR_IA32_VMX_EPT_VPID:
        return m.ept_vpid;
    case MSR_IA32_CR_PAT:
        return 0x0007040600070406ull;
    case MSR_IA32_DEBUGCTL:
        return m.debugctl;
    case MSR_EFER:
        return 0xD01ull;
    default:
        return 0x1000ull + msr;
    }
}

/** Whether the processor refuses `value` for `msr` with #GP: bits 63:32
 * of SYSCALL_MASK and TSC_AUX are reserved. */
static int m_msr_refused(uint32_t msr, uint64_t value)
{
    return (msr == MSR_SYSCALL_MASK || msr == MSR_TSC_AUX) && (value >> 32) != 0;
}

/** WRMSR: IA32_FEATURE_CONTROL, written only unlocked, IA32_DEBUGCTL and
 * the kept MSRs, counted; the others do not matter. A value the processor
 * refuses would fault the host. */
static void m_wrmsr(uint32_t msr, uint64_t value)
{
    CHECK(!m_msr_refused(msr, value), "no WRMSR that can fault the host writes a refused value");
    if (msr_slot(msr) >= 0) {
        m.msr[msr_slot(msr)] = value;
        m.msr_writes++;
    }
    if (msr == MSR_IA32_DEBUGCTL) {
        m.debugctl = value;
        m.debugctl_writes++;
    }
    if (msr == MSR_IA32_FEATURE_CONTROL) {
        CHECK(!(m.fc & FC_LOCKED), "IA32_FEATURE_CONTROL is written only while unlocked");
        m.fc = value;
        m.fc_writes++;
    }
}

/** The safe WRMSR: -1, with the MSR unchanged, for a value the processor
 * refuses with #GP; else `m_wrmsr`'s write and 0. */
static int m_wrmsr_safe(uint32_t msr, uint64_t value)
{
    if (m_msr_refused(msr, value)) {
        m.msr_faults++;
        return -1;
    }
    m_wrmsr(msr, value);
    return 0;
}

/** The host's CR0, CR3 and CR4. */
static uint64_t m_read_cr(int n)
{
    if (n == 4) {
        return m.cr4;
    }
    return n == 0 ? m.cr0 : 0x1000ull;
}

/** Write CR4: clearing VMXE in VMX operation is #GP. */
static void m_write_cr4(uint64_t value)
{
    if (m.on && !(value & CR4_VMXE)) {
        m.gp++;
        return;
    }
    m.cr4 = value;
}

/** A debug register. */
static uint64_t m_read_dr(int n)
{
    return m.dr[n & 7];
}

/** Write a debug register. DR0 to DR3 change only while DR7 enables no
 * breakpoint, so that the host never traps on a guest's address, as
 * Linux's vcpu_enter_guest clears DR7 first (x86.c:10542-10548). */
static void m_write_dr(int n, uint64_t value)
{
    CHECK((n & 7) > 3 || !(m.dr[7] & DR7_ENABLES),
          "DR0 to DR3 change only while DR7 enables no breakpoint");
    m.dr[n & 7] = value;
    if ((n & 7) == 6) {
        m.dr6_writes++;
    }
}

/** Save the host's state before an entry: an IDT, an FS base and its
 * KERNEL_GS_BASE, once until it is put back. */
static void m_save_host(struct vmx_host *h)
{
    CHECK(!m.saved, "the host's state is saved once until it is put back");
    k_memset(h, 0, sizeof(*h));
    h->idt_base = 0xFFFFFFFF80200000ull;
    h->fs_base = m.fs_base;
    h->kgs_base = m.msr[msr_slot(MSR_KERNEL_GS_BASE)];
    m.saved = 1;
    m.saves++;
}

/** Put the host's state back, its KERNEL_GS_BASE among it, counted. */
static void m_restore_host(const struct vmx_host *h)
{
    CHECK(m.saved && !irq_on, "the host's state is put back once, with interrupts off");
    m.msr[msr_slot(MSR_KERNEL_GS_BASE)] = h->kgs_base;
    m.saved = 0;
    m.restores++;
}

/** Load the guest's FPU, which happens only inside VMX operation: with
 * `host`, the host's saved first, once until `m_fpu_store` gives it back;
 * without, the image again over the guest's registers. */
static void m_fpu_load(void *host, const void *guest)
{
    (void)guest;
    CHECK(m.on && !irq_on, "the guest's FPU is loaded inside the run's section");
    if (host) {
        CHECK(!m.fpu_held, "the guest's FPU is loaded once until the host's is back");
        m.fpu_held = 1;
        m.fpu_loads++;
        m.host_pkru = m.pkru;
    } else {
        CHECK(m.fpu_held, "the image is loaded again only over the guest's registers");
        m.fpu_reloads++;
    }
    m.pkru = m.image_pkru;
}

/** Store the guest's FPU into `guest` unless it is NULL, then the host's
 * from `host` unless it is NULL; a store that keeps the guest's loaded is
 * counted. */
static void m_fpu_store(void *host, void *guest)
{
    CHECK(m.fpu_held && !irq_on, "only the guest's loaded FPU is stored, with interrupts off");
    if (guest && !host) {
        m.fpu_syncs++;
    }
    if (guest) {
        CHECK(m.pkru == 0, "the image takes the guest's PKRU, never the host's");
        m.image_pkru = m.pkru;
    }
    if (host) {
        m.fpu_held = 0;
        m.fpu_stores++;
        m.pkru = m.host_pkru;
    }
}

/** XGETBV of XCR0. */
static uint64_t m_xgetbv(void)
{
    return m.xcr0;
}

/** XSETBV of XCR0, with interrupts off. */
static void m_xsetbv(uint64_t value)
{
    CHECK(!irq_on, "XCR0 changes with interrupts off");
    m.xcr0 = value;
}

/** RDPKRU. */
static uint32_t m_rdpkru(void)
{
    return m.pkru;
}

/** WRPKRU. */
static void m_wrpkru(uint32_t value)
{
    m.pkru = value;
}

/** The host's NMI, raised once the exit is read, with interrupts off and
 * the host's XCR0 back. */
static void m_nmi(void)
{
    CHECK(irq_on == 0 && m.xcr0 == HOST_XCR0_MODEL,
          "the host's NMI is raised with the host's XCR0 back, interrupts still off");
    m.nmis++;
}

/** The host's machine check, raised once the exit is read, with the
 * host's XCR0 back. */
static void m_machine_check(void)
{
    CHECK(m.xcr0 == HOST_XCR0_MODEL,
          "the host's machine check is raised with the host's XCR0 back");
    m.mces++;
}

/** WBINVD, counted. */
static void m_wbinvd(void)
{
    m.wbinvds++;
}

/** The host's address of the exit. */
static uint64_t m_exit_rip(void)
{
    return EXIT_RIP_MODEL;
}

static const struct vmx_ops model_ops = {
    m_vmxon,   m_vmxoff,   m_vmclear,   m_vmptrld,      m_vmread,        m_vmwrite,   m_invept,
    m_invvpid, m_run,      m_rdmsr,     m_wrmsr,        m_wrmsr_safe,    m_read_cr,   m_write_cr4,
    m_read_dr, m_write_dr, m_save_host, m_restore_host, m_fpu_load,      m_fpu_store, m_xgetbv,
    m_xsetbv,  m_rdpkru,   m_wrpkru,    m_nmi,          m_machine_check, m_wbinvd,    m_exit_rip};

/** The processor `cpu`'s GS base, TSS and GDT, 4 KiB apart. */
static void host_cpu_bases(int cpu, uint64_t *gs, uint64_t *tr, uint64_t *gdt)
{
    *gs = 0xFFFFFFFF81000000ull + 0x1000ull * (uint64_t)cpu;
    *tr = *gs + 0x100u;
    *gdt = *gs + 0x200u;
}

/** The model as the Lenovo's processor is: the capability MSRs its
 * Haswell reads, IA32_FEATURE_CONTROL locked with VMX on, VMX off. */
static void model_lenovo(void)
{
    int i;

    k_memset(&m, 0, sizeof(m));
    m.basic = 0x00DA040000000012ull;
    m.pin = 0x0000007F00000016ull;
    m.proc = 0xFFF9FFFE04006172ull;
    m.exit = 0x007FFFFF00036DFBull;
    m.entry = 0x0000FFFF000011FBull;
    m.misc = 0x00000000300481E5ull;
    m.proc2 = 0x000037FF00000000ull;
    m.ept_vpid = 0x00000F0106734141ull;
    m.fc = 0x5;
    m.cr4 = HOST_CR4_MODEL;
    m.cpus[0].cr4 = HOST_CR4_MODEL;
    m.cpus[1].cr4 = HOST_CR4_MODEL;
    m.cr0 = 0x80050033ull;
    m.xcr0 = HOST_XCR0_MODEL;
    m.pkru = HOST_PKRU_MODEL;
    m.fs_base = 0x700000ull;
    for (i = 0; i < 5; i++) {
        m.msr[i] = HOST_MSR(kept_msrs[i]);
    }
}

/** The switch away the run loop's `sched_take_preempt` takes when a row
 * asked for it at this turn: the thread's `sched_out`, preempted, with
 * interrupts off, as `schedule` calls it. */
static void model_switch(void)
{
    CHECK(!m.fpu_held || m.pkru == m.host_pkru,
          "between runs the host's PKRU is in, while the guest's FPU is held, as Linux's "
          "kvm_load_host_xsave_state puts it back after each run (x86.c:1027-1047)");
    if (m.preempt && --m.preempt == 0) {
        CHECK(!irq_on, "the switch is taken with interrupts off");
        if (current_proc->sched_out) {
            current_proc->sched_out(current_proc->sched_out_arg, 1);
        }
        m.switches++;
    }
}

/** A halted vCPU's sleep at the run loop's top, when a row asked for it
 * at this pass: the thread blocks, and `schedule` calls its `sched_out`
 * with interrupts off, not preempted. The host's state and MSRs must be
 * back by then and the guest's FPU left to the switch, and the steal time
 * must count no preemption, as Linux's kvm_sched_out marks only a task
 * still on its run queue. */
static void model_block(struct kvm_vcpu *v)
{
    uint64_t reported = v->stat[KVM_VCPU_STAT_PREEMPTION_REPORTED];
    uint64_t other = v->stat[KVM_VCPU_STAT_PREEMPTION_OTHER];

    CHECK(!m.fpu_held || m.pkru == m.host_pkru,
          "a halted vCPU's sleep finds the host's PKRU in while the guest's FPU is held");
    if (!m.block || --m.block != 0) {
        return;
    }
    CHECK(irq_on && current_proc->sched_out,
          "a halted vCPU sleeps with interrupts on, its thread's sched_out set");
    irq_on = 0;
    current_proc->sched_out(current_proc->sched_out_arg, 0);
    irq_on = 1;
    CHECK(!m.saved && host_msrs() && m.fpu_held,
          "a sleep's switch puts the host's state and MSRs back, the guest's FPU left to it");
    CHECK(v->stat[KVM_VCPU_STAT_PREEMPTION_REPORTED] == reported &&
              v->stat[KVM_VCPU_STAT_PREEMPTION_OTHER] == other && !v->st_preempted,
          "and is not a preemption of the vCPU");
    m.blocks++;
}

/* ---- the rows ---- */

/* The guest's memory: the slot's 1 MiB. */
static uint8_t ram[1u << 20] __attribute__((aligned(4096)));

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

/** Close the descriptor `fd`. */
static void fdclose(int fd)
{
    entries[fd].ops->close(&entries[fd]);
    k_memset(&entries[fd], 0, sizeof(entries[fd]));
    reserved[fd] = 0;
}

/** The vCPU of the descriptor `fd`. */
static struct kvm_vcpu *vcpu_of(int fd)
{
    return (struct kvm_vcpu *)entries[fd].priv;
}

/** The run page of the vCPU on `fd`. */
static struct kvm_run *run_of(int fd)
{
    return vcpu_of(fd)->run;
}

/** The fields of the VMCS of the vCPU on `fd`, as VMCLEAR left them. */
static uint64_t *fields(int fd)
{
    return mvmcs_of(vx(vcpu_of(fd))->vmcs_pa)->f;
}

/** Append a run that ends in the exit `reason` with `qual` and an
 * instruction of `len` bytes; returns the row for the rest. */
static struct row *script(uint32_t reason, uint64_t qual, uint32_t len)
{
    struct row *r = &m.script[m.script_len++];

    k_memset(r, 0, sizeof(*r));
    r->reason = reason;
    r->qual = qual;
    r->len = len;
    return r;
}

/** KVM_RUN on `fd`, with the script's rows, then the checks every exit
 * path must pass: each processor in VMX operation exactly while a VMCS is
 * active there, CR4 as it was once VMX is off, no VMCS loaded on one
 * processor while active on another, the host's state, MSRs, XCR0 and FPU
 * put back by KVM_RUN's end, and every row used. Returns KVM_RUN's
 * result. */
static int32_t run(int fd)
{
    int32_t r;
    int runs = m.launches + m.resumes;

    r = fdio(fd, KVM_RUN, 0);
    CHECK(cpu_ok(0) && cpu_ok(1), "VMX is on exactly where a VMCS is active, CR4 as it was");
    CHECK(m.misplaced == 0, "a VMCS is cleared where it is active before it is loaded elsewhere");
    CHECK(!m.saved && m.restores == m.saves && host_msrs(),
          "KVM_RUN ends with the host's state and MSRs back");
    CHECK(!m.fpu_held && m.fpu_loads == m.fpu_stores && m.xcr0 == HOST_XCR0_MODEL &&
              m.pkru == HOST_PKRU_MODEL,
          "KVM_RUN ends with the host's FPU, XCR0 and PKRU back");
    CHECK(m.script_at == m.script_len, "every run the script gives is entered");
    CHECK(irq_on && bkl_depth == 0, "KVM_RUN ends with interrupts on and no big lock held");
    if (m.why && m.launches + m.resumes > runs && run_of(fd)->exit_reason != KVM_EXIT_FAIL_ENTRY) {
        printf("kvm-vmx-hosttest: entry check: %s\n", m.why);
    }
    m.why = NULL;
    m.script_len = 0;
    m.script_at = 0;
    return r;
}

/* A buffer for KVM_GET_SUPPORTED_CPUID and KVM_SET_CPUID2. */
static union {
    struct kvm_cpuid2 cpuid;
    uint8_t bytes[16384];
} buf;

/** The registers of the vCPU on `fd`, got. */
static struct kvm_regs regs_of(int fd)
{
    struct kvm_regs r;

    CHECK(fdio(fd, KVM_GET_REGS, (uintptr_t)&r) == 0, "KVM_GET_REGS");
    return r;
}

/** Set the vCPU's RIP, RAX, RBX, RCX, RDX and RFLAGS. */
static void set_regs(int fd, uint64_t rip, uint64_t rax, uint64_t rcx, uint64_t rdx)
{
    struct kvm_regs r = regs_of(fd);

    r.rip = rip;
    r.rax = rax;
    r.rcx = rcx;
    r.rdx = rdx;
    CHECK(fdio(fd, KVM_SET_REGS, (uintptr_t)&r) == 0, "KVM_SET_REGS");
}

/** The special registers of the vCPU on `fd`, got. */
static struct kvm_sregs sregs_of(int fd)
{
    struct kvm_sregs s;

    CHECK(fdio(fd, KVM_GET_SREGS, (uintptr_t)&s) == 0, "KVM_GET_SREGS");
    return s;
}

/** A vCPU of id `id` on the VM `vm`, with the supported CPUID, its code
 * segment at 0 in real mode and RIP 0x1000, as a guest's firmware has it
 * once it runs from RAM. */
static int make_vcpu(int vm, uint32_t id)
{
    struct kvm_sregs s;
    int fd;

    fd = fdio(vm, KVM_CREATE_VCPU, id);
    CHECK(fd > vm, "KVM_CREATE_VCPU");
    buf.cpuid.nent = 256;
    CHECK(dev(KVM_GET_SUPPORTED_CPUID, (uintptr_t)&buf) == 0, "KVM_GET_SUPPORTED_CPUID");
    CHECK(fdio(fd, KVM_SET_CPUID2, (uintptr_t)&buf) == 0, "KVM_SET_CPUID2");
    s = sregs_of(fd);
    s.cs.base = 0;
    s.cs.selector = 0;
    CHECK(fdio(fd, KVM_SET_SREGS, (uintptr_t)&s) == 0, "KVM_SET_SREGS");
    set_regs(fd, 0x1000, 0, 0, 0);
    return fd;
}

/** The probe on processors the backend must refuse, then on the
 * Lenovo's, whose controls it chooses. */
static void probe_rows(void)
{
    model_lenovo();
    vmx_op = &model_ops;
    cpuid_vmx = 0;
    CHECK(!vmx_probe(), "no VMX in CPUID");
    cpuid_vmx = 1;
    m.fc = 0x1;
    CHECK(!vmx_probe(), "IA32_FEATURE_CONTROL locked with VMX off");
    model_lenovo();
    m.proc2 &= ~((uint64_t)PROC2_UG << 32);
    CHECK(!vmx_probe(), "no unrestricted guest");
    model_lenovo();
    m.ept_vpid &= ~EPT_CAP_WB;
    CHECK(!vmx_probe(), "no write-back EPT tables");
    model_lenovo();
    m.pin &= ~((uint64_t)PIN_PREEMPTION << 32);
    CHECK(!vmx_probe(), "no preemption timer");
    model_lenovo();
    m.proc |= (uint64_t)PROC_CR3_LOAD;
    CHECK(!vmx_probe(), "CR3 exits forced on");
    model_lenovo();
    m.proc &= ~((uint64_t)PROC_NMI_WINDOW << 32);
    CHECK(!vmx_probe(), "no NMI window");
    model_lenovo();
    m.ept_vpid &= ~VPID_CAP_INVVPID;
    CHECK(vmx_probe() && !(caps.proc2 & PROC2_VPID), "VPID left out without INVVPID");
    /* Linux's setup_vmcs_config (arch/x86/kvm/vmx/vmx.c:2740-2759) drops the
     * controls that load IA32_PERF_GLOBAL_CTRL on Nehalem and Westmere,
     * whose exits may clear its fixed-counter bits, though they offer them. */
    model_lenovo();
    CHECK(vmx_probe() && caps.perf_global,
          "the model's desktop Haswell, 0x306C3, offers the controls that load "
          "IA32_PERF_GLOBAL_CTRL, which are used");
    cpuid_sig = 0x000106A5u;
    CHECK(vmx_probe() && !caps.perf_global, "Nehalem EP, 0x106A5, does not use them");
    cpuid_sig = 0x000206C2u;
    CHECK(vmx_probe() && !caps.perf_global, "nor does Westmere EP, 0x206C2");
    cpuid_sig = 0x00040651u;
    CHECK(vmx_probe() && caps.perf_global, "the Lenovo's Haswell, 0x40651, does");
    cpuid_sig = 0x000306C3u;
    model_lenovo();
    m.proc &= ~((uint64_t)PROC_TPR_SHADOW << 32);
    CHECK(vmx_probe() && !caps.tpr_shadow &&
              (caps.proc & (PROC_CR8_LOAD | PROC_CR8_STORE)) == (PROC_CR8_LOAD | PROC_CR8_STORE),
          "no TPR shadow on a processor without it, and CR8's exits");
    model_lenovo();
    m.proc |= (uint64_t)PROC_CR8_STORE;
    CHECK(vmx_probe() && !caps.tpr_shadow, "no TPR shadow where CR8's store exit is forced on");
    model_lenovo();
    kvm_start();
    CHECK(kvm_backend == &kvm_vmx && registered && k_panic_hook == vmx_panic,
          "the Lenovo's processor probes, /dev/kvm is registered and the panic hook set");
    CHECK(caps.revision == 0x12 && caps.timer_shift == 5 && caps.ins_outs && caps.ad && caps.rdtscp,
          "the revision, the timer's rate, INS and OUTS information, EPT's A/D and RDTSCP");
    CHECK(caps.pin == 0x7Fu && (caps.proc2 & (PROC2_EPT | PROC2_UG | PROC2_VPID)) ==
                                   (PROC2_EPT | PROC2_UG | PROC2_VPID),
          "the pin controls and EPT, the unrestricted guest and VPID");
    CHECK(!(caps.proc & (PROC_CR3_LOAD | PROC_CR3_STORE | PROC_INVLPG)),
          "no CR3 or INVLPG exits under EPT");
    CHECK(caps.tpr_shadow && !(caps.proc & PROC_TPR_SHADOW),
          "the TPR shadow is the Lenovo's, taken for each vCPU with the APIC in the kernel");
    CHECK(vmx_cache(KVM_MT_WB, 1) == 0x70u && vmx_cache(KVM_MT_WB, 0) == 0x30u &&
              vmx_cache(KVM_MT_UC, 0) == 0,
          "EPT's memory types: write-back with IPAT, without, and uncached");
}

/** The launch state after the first KVM_RUN of `v`: one VMXON, one
 * VMPTRLD, a VMLAUNCH and then a VMRESUME, and the VMCS left current. */
static void launch_state_rows(const struct kvm_vcpu *v)
{
    CHECK(m.launches == 1 && m.resumes == 1 && m.vmxons == 1 && m.vmxoffs == 0 && m.vmclears == 1 &&
              m.vmptrlds == 1 && m.fc_writes == 0,
          "the first entry is a VMLAUNCH of a cleared VMCS, the next a VMRESUME, in one VMXON "
          "and one VMPTRLD");
    CHECK(m.on && m.cur && m.cur->pa == vx(v)->vmcs_pa && m.cur->launched,
          "VMX stays on after KVM_RUN with the VMCS current and launched");
}

/** The first entries: the VMCS built and checked, CPUID and HLT. */
static void launch_rows(int fd)
{
    struct kvm_vcpu *v = vcpu_of(fd);
    struct kvm_regs r;
    uint64_t *f;

    set_regs(fd, 0x1000, 0, 0, 0);
    script(EXIT_CPUID, 0, 2);
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && run_of(fd)->exit_reason == KVM_EXIT_HLT, "CPUID, then HLT to the caller");
    r = regs_of(fd);
    CHECK(r.rbx == 0x756E6547u && r.rdx == 0x49656E69u && r.rip == 0x1003,
          "CPUID answers from the guest's table and both instructions are stepped");
    launch_state_rows(v);
    CHECK(m.ud == 0 && m.gp == 0 && m.bad_writes == 0 && m.bad_reads == 0,
          "no VMX instruction outside VMX operation and no field the VMCS lacks");
    f = fields(fd);
    CHECK(f[VIRTUAL_PROCESSOR_ID] == 1 && f[EPT_POINTER] == (TDP_ROOT | 0x5Eu),
          "VPID 1 and the EPT pointer: four levels, write-back, A/D");
    CHECK(f[CR0_READ_SHADOW] == 0x60000010u && f[GUEST_CR0] == 0x30u && f[GUEST_CR4] == 0x2000u,
          "the reset's CR0 in the shadow, run without CD and NW, with NE and VMXE");
    CHECK(f[VM_ENTRY_MSR_LOAD_COUNT] == 0 && f[VM_EXIT_MSR_STORE_COUNT] == 0 &&
              f[VM_EXIT_MSR_LOAD_COUNT] == 0,
          "no MSR list: the guest's MSRs are written once per KVM_RUN, as Linux writes them");
    CHECK(f[HOST_GS_BASE] == 0xFFFFFFFF81000000ull && f[HOST_FS_BASE] == 0x700000u &&
              f[HOST_IDTR_BASE] == 0xFFFFFFFF80200000ull,
          "the host's bases");
    CHECK(v->sregs.cr0 == 0x60000010u, "the record keeps the guest's own CR0");
}

/** Port I/O: OUT to the caller, IN answered at the next KVM_RUN, a port
 * the in-kernel bus takes, and REP OUTSB. */
static void io_rows(int fd)
{
    struct kvm_vcpu *v = vcpu_of(fd);
    struct kvm_run *rp = run_of(fd);
    struct row *w;

    set_regs(fd, 0x1000, 0x41, 0, 0);
    script(EXIT_IO, 0x3F8ull << 16, 1);
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_IO && rp->u.io.direction == KVM_EXIT_IO_OUT &&
              rp->u.io.port == 0x3F8 && rp->u.io.size == 1 && rp->u.io.count == 1 &&
              v->pio[0] == 'A',
          "OUT DX, AL goes to the caller with its byte");
    CHECK(regs_of(fd).rip == 0x1000, "with RIP at it until the next KVM_RUN, as on Linux");
    script(EXIT_IO, 8u | (0x60ull << 16), 1);
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_IO && rp->u.io.direction == KVM_EXIT_IO_IN &&
              rp->u.io.port == 0x60,
          "IN AL, DX goes to the caller");
    v->pio[0] = 0x5A;
    script(EXIT_IO, (uint64_t)BUS_PORT << 16, 1);
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_HLT, "the bus's port stays in the kernel");
    CHECK((regs_of(fd).rax & 0xFF) == 0x5A && regs_of(fd).rip == 0x1004,
          "the caller's answer reaches AL at the next KVM_RUN");
    set_regs(fd, 0x1000, 0, 3, 0);
    v->regs.rsi = 0x2000;
    v->dirty |= KVM_PART_REGS;
    ram[0x2000] = 'x';
    ram[0x2001] = 'y';
    ram[0x2002] = 'z';
    w = script(EXIT_IO, 0x30u | (0x3F8ull << 16), 2);
    w->insn_info = 3u << 15;
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_IO && rp->u.io.count == 3 &&
              v->pio[0] == 'x' && v->pio[2] == 'z',
          "REP OUTSB moves the string at DS:SI");
    CHECK(regs_of(fd).rcx == 0 && regs_of(fd).rsi == 0x2003 && regs_of(fd).rip == 0x1002,
          "and counts CX and SI off, stepped past");
}

/* RFLAGS.RF, the resume flag. */
#define F_RF 0x10000u

/** REP OUTSB cut at a page: RF set while it repeats and cleared once it
 * ends, as Linux's x86_emulate_insn leaves it. */
static void outs_rf_rows(int fd)
{
    struct kvm_vcpu *v = vcpu_of(fd);
    struct kvm_run *rp = run_of(fd);
    struct kvm_regs r;
    struct row *w;

    set_regs(fd, 0x1000, 0, 3, 0);
    r = regs_of(fd);
    r.rsi = 0x2FFE;
    r.rflags &= ~(uint64_t)F_RF;
    CHECK(fdio(fd, KVM_SET_REGS, (uintptr_t)&r) == 0,
          "KVM_SET_REGS with SI two bytes short of a page");
    ram[0x2FFE] = 'p';
    ram[0x2FFF] = 'q';
    ram[0x3000] = 'r';
    w = script(EXIT_IO, 0x30u | (0x3F8ull << 16), 2);
    w->insn_info = 3u << 15;
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_IO && rp->u.io.count == 2 &&
              v->pio[0] == 'p' && v->pio[1] == 'q',
          "REP OUTSB stops at the page's end");
    r = regs_of(fd);
    CHECK(r.rcx == 1 && r.rsi == 0x3000 && r.rip == 0x1000 && (r.rflags & F_RF),
          "and repeats with RF set");
    w = script(EXIT_IO, 0x30u | (0x3F8ull << 16), 2);
    w->insn_info = 3u << 15;
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_IO && rp->u.io.count == 1 && v->pio[0] == 'r',
          "the guest's next pass moves the last byte");
    r = regs_of(fd);
    CHECK(r.rcx == 0 && r.rsi == 0x3001 && r.rip == 0x1002 && !(r.rflags & F_RF),
          "and ends the instruction with RF clear");
}

/** REP INSB cut at a page, whose elements are written at the next
 * KVM_RUN: RF set while it repeats and cleared once it ends. */
static void ins_rf_rows(int fd)
{
    struct kvm_vcpu *v = vcpu_of(fd);
    struct kvm_run *rp = run_of(fd);
    struct kvm_regs r;

    set_regs(fd, 0x1000, 0, 2, 0);
    r = regs_of(fd);
    r.rdi = 0x2FFF;
    CHECK(fdio(fd, KVM_SET_REGS, (uintptr_t)&r) == 0,
          "KVM_SET_REGS with DI a byte short of a page");
    script(EXIT_IO, 0x38u | (0x60ull << 16), 2);
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_IO && rp->u.io.direction == KVM_EXIT_IO_IN &&
              rp->u.io.count == 1,
          "REP INSB stops at the page's end");
    v->pio[0] = 'i';
    script(EXIT_IO, 0x38u | (0x60ull << 16), 2);
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_IO && rp->u.io.count == 1,
          "the answer is stored at the next KVM_RUN, and the guest's next pass reads again");
    r = regs_of(fd);
    CHECK(ram[0x2FFF] == 'i' && r.rcx == 1 && r.rdi == 0x3000 && r.rip == 0x1000 &&
              (r.rflags & F_RF),
          "with the first element stored and RF set while it repeats");
    v->pio[0] = 'j';
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_HLT, "the last answer ends it");
    r = regs_of(fd);
    CHECK(ram[0x3000] == 'j' && r.rcx == 0 && r.rdi == 0x3001 && r.rip == 0x1003 &&
              !(r.rflags & F_RF),
          "with RF clear and RIP past it");
}

/** A REP OUTSB at offset 0 with a 64-bit address size, which the model's
 * exit may give in any mode: no wrap is near, so it moves whole. */
static void outs_wrap_rows(int fd)
{
    struct kvm_vcpu *v = vcpu_of(fd);
    struct kvm_run *rp = run_of(fd);
    struct kvm_regs r;
    struct row *w;

    set_regs(fd, 0x1000, 0, 3, 0);
    r = regs_of(fd);
    r.rsi = 0;
    CHECK(fdio(fd, KVM_SET_REGS, (uintptr_t)&r) == 0, "KVM_SET_REGS with SI 0");
    ram[0] = 'a';
    ram[1] = 'b';
    ram[2] = 'c';
    w = script(EXIT_IO, 0x30u | (0x3F8ull << 16), 2);
    w->insn_info = (2u << 7) | (3u << 15);
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_IO && rp->u.io.count == 3 &&
              v->pio[0] == 'a' && v->pio[2] == 'c',
          "REP OUTSB at offset 0 with a 64-bit address size moves every byte at once");
    r = regs_of(fd);
    CHECK(r.rcx == 0 && r.rsi == 3 && r.rip == 0x1002, "and counts RCX and RSI off, stepped past");
}

/** Set RCX and RDI of the vCPU on `fd`, as a caller does between an exit
 * and the next KVM_RUN. */
static void set_cx_di(int fd, uint64_t rcx, uint64_t rdi)
{
    struct kvm_regs r = regs_of(fd);

    r.rcx = rcx;
    r.rdi = rdi;
    CHECK(fdio(fd, KVM_SET_REGS, (uintptr_t)&r) == 0, "KVM_SET_REGS of RCX and RDI");
}

/** REP INSB whose count the caller changes between the exit and the next
 * KVM_RUN, as Linux's userspace_io_test does: the elements stored are
 * those RCX now allows, at most the exit's, so RCX ends at 0 and not past
 * it, and the guest's buffer is not overrun. */
static void ins_count_rows(int fd)
{
    struct kvm_vcpu *v = vcpu_of(fd);
    struct kvm_run *rp = run_of(fd);
    struct kvm_regs r;

    k_memset(&ram[0x2000], 0, 8);
    set_regs(fd, 0x1000, 0, 2, 0);
    set_cx_di(fd, 2, 0x2000);
    script(EXIT_IO, 0x38u | (0x80ull << 16), 2);
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_IO && rp->u.io.count == 2,
          "REP INSB of two bytes asks the caller for both");
    set_cx_di(fd, 1, 0x2000);
    k_memcpy(v->pio, "xy", 2);
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_HLT, "the caller's RCX of 1 ends it");
    r = regs_of(fd);
    CHECK(ram[0x2000] == 'x' && ram[0x2001] == 0 && r.rcx == 0 && r.rdi == 0x2001 &&
              r.rip == 0x1003 && !(r.rflags & F_RF),
          "with the one byte RCX allows stored and RCX at 0, as Linux leaves them");

    set_regs(fd, 0x1000, 0, 3, 0);
    set_cx_di(fd, 3, 0x2000);
    script(EXIT_IO, 0x38u | (0x80ull << 16), 2);
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_IO && rp->u.io.count == 3,
          "REP INSB of three bytes asks for three");
    set_cx_di(fd, 5, 0x2000);
    k_memcpy(v->pio, "abc", 3);
    script(EXIT_IO, 0x38u | (0x80ull << 16), 2);
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_IO && rp->u.io.count == 2,
          "a caller's RCX of 5 stores the three answered and asks for the other two");
    r = regs_of(fd);
    CHECK(memcmp(&ram[0x2000], "abc", 3) == 0 && ram[0x2003] == 0 && r.rcx == 2 &&
              r.rdi == 0x2003 && r.rip == 0x1000 && (r.rflags & F_RF),
          "with RCX and RDI counted by three");
    set_cx_di(fd, 0, 0x2003);
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_HLT, "the caller's RCX of 0 ends it");
    r = regs_of(fd);
    CHECK(ram[0x2003] == 0 && r.rcx == 0 && r.rdi == 0x2003 && r.rip == 0x1003 &&
              !(r.rflags & F_RF),
          "with nothing stored, as Linux's first termination condition ends it");
}

/** The string I/O instructions cut at a page, one near no wrap, and a
 * count the caller changes. */
static void io_string_rows(int fd)
{
    outs_rf_rows(fd);
    ins_rf_rows(fd);
    outs_wrap_rows(fd);
    ins_count_rows(fd);
}

/** Write the instruction `bytes` of `n` bytes at `rip` and point RIP at
 * it with RAX `rax`. */
static void code(int fd, uint64_t rip, const char *bytes, uint32_t n, uint64_t rax)
{
    k_memcpy(&ram[rip], bytes, n);
    set_regs(fd, rip, rax, 0, 0);
}

/** Set DS's base on the vCPU on `fd`, and BX. */
static void set_ds_bx(int fd, uint64_t base, uint64_t bx)
{
    struct kvm_sregs s = sregs_of(fd);
    struct kvm_regs r = regs_of(fd);

    s.ds.base = base;
    CHECK(fdio(fd, KVM_SET_SREGS, (uintptr_t)&s) == 0, "KVM_SET_SREGS with DS's base");
    r.rbx = bx;
    CHECK(fdio(fd, KVM_SET_REGS, (uintptr_t)&r) == 0, "KVM_SET_REGS with BX");
}

/** An EPT fault at an instruction the decoder refuses, at CPL 0. */
static void refused_rows(int fd)
{
    struct kvm_run *rp = run_of(fd);
    struct kvm_vcpu *v = vcpu_of(fd);
    struct row *w;

    code(fd, 0x1400, "\x0f\x0b", 2, 0);
    w = script(EXIT_EPT_VIOLATION, 0x1, 0);
    w->gpa = 0xD0000030ull;
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_INTERNAL_ERROR &&
              rp->u.internal.suberror == KVM_INTERNAL_ERROR_EMULATION,
          "an instruction the decoder refuses at CPL 0 is Linux's emulation failure");
    CHECK(v->events.exception.pending && v->events.exception.nr == UD_VECTOR,
          "with #UD queued for the guest, as Linux queues it");
    v->events.exception.pending = 0;

    code(fd, 0x1400, "\x0f\x0b", 2, 0);
    w = script(EXIT_EPT_VIOLATION, 0x1, 0);
    w->gpa = 0xD0000030ull;
    w->intr_info = 0x800u;
    w->intr_error = 0x1234;
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_INTERNAL_ERROR && rp->u.internal.ndata == 8 &&
              rp->u.internal.data[6] == 0x800u && rp->u.internal.data[7] == 0,
          "the report gives no error code for interruption information with its error bit alone");
    v->events.exception.pending = 0;
    code(fd, 0x1400, "\x0f\x0b", 2, 0);
    w = script(EXIT_EPT_VIOLATION, 0x1, 0);
    w->gpa = 0xD0000030ull;
    w->intr_info = 0x80000B0Eu;
    w->intr_error = 0x1234;
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_INTERNAL_ERROR &&
              rp->u.internal.data[6] == 0x80000B0Eu && rp->u.internal.data[7] == 0x1234,
          "and gives it with the valid bit too, as Linux's vmx_get_exit_info");
    v->events.exception.pending = 0;
}

/** An MMIO read, MOV AX, [BX], at an exit address unlike its operand's,
 * answered once under immediate_exit. */
static void read_rows(int fd)
{
    struct kvm_run *rp = run_of(fd);
    struct row *w;
    int launches;

    code(fd, 0x1100, "\x8b\x07", 2, 0);
    set_ds_bx(fd, 0xD0000000ull, 0);
    w = script(EXIT_EPT_VIOLATION, 0x1, 0);
    w->gpa = 0xD0000FF8ull;
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_MMIO &&
              rp->u.mmio.phys_addr == 0xD0000000ull && rp->u.mmio.len == 2 && !rp->u.mmio.is_write,
          "MOV AX, [BX] on MMIO is a read of two bytes for the caller, at the operand's address "
          "and not the exit's");
    rp->u.mmio.data[0] = 0xEF;
    rp->u.mmio.data[1] = 0xBE;
    rp->immediate_exit = 1;
    launches = m.launches + m.resumes;
    CHECK(run(fd) == -EINTR && (regs_of(fd).rax & 0xFFFF) == 0xBEEF && regs_of(fd).rip == 0x1102 &&
              m.launches + m.resumes == launches,
          "its answer reaches AX and the instruction is stepped before KVM_RUN returns under "
          "immediate_exit, with no guest entered");
    rp->immediate_exit = 0;
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && (regs_of(fd).rax & 0xFFFF) == 0xBEEF && regs_of(fd).rip == 0x1103,
          "and the guest runs on after it");
}

/** EPT faults: a RAM frame mapped in the kernel; MMIO reads, writes and a
 * read-modify-write carried out with the caller, at the address the
 * instruction's operand names, DS:BX with DS's base on the device's
 * frame; an instruction the decoder refuses. */
static void ept_rows(int fd)
{
    struct kvm_run *rp = run_of(fd);
    struct row *w;
    int launches;

    w = script(EXIT_EPT_VIOLATION, 0x2, 0);
    w->gpa = 0x3000;
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_HLT && tdp_fault_gfn == 3 && tdp_fault_write,
          "a write to a RAM frame is mapped in the kernel and the guest runs on");

    read_rows(fd);

    code(fd, 0x1200, "\x88\x07", 2, 0x77);
    set_ds_bx(fd, 0xD0000000ull, 0x10);
    w = script(EXIT_EPT_VIOLATION, 0x2, 0);
    w->gpa = 0xD0000010ull;
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_MMIO && rp->u.mmio.is_write &&
              rp->u.mmio.phys_addr == 0xD0000010ull && rp->u.mmio.len == 1 &&
              rp->u.mmio.data[0] == 0x77 && regs_of(fd).rip == 0x1202,
          "MOV [BX], AL on MMIO is a write of AL, stepped");

    code(fd, 0x1300, "\x00\x07", 2, 3);
    set_ds_bx(fd, 0xD0000000ull, 0x20);
    w = script(EXIT_EPT_VIOLATION, 0x3, 0);
    w->gpa = 0xD0000020ull;
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_MMIO && !rp->u.mmio.is_write &&
              rp->u.mmio.phys_addr == 0xD0000020ull,
          "ADD [BX], AL reads its operand first");
    rp->u.mmio.data[0] = 5;
    launches = m.launches + m.resumes;
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_MMIO && rp->u.mmio.is_write &&
              rp->u.mmio.data[0] == 8 && regs_of(fd).rip == 0x1302,
          "then writes the sum at the next KVM_RUN");
    CHECK(m.launches + m.resumes == launches,
          "with no guest entered between the read and the write");
    set_ds_bx(fd, 0, 0);
    refused_rows(fd);
}

/** The instruction at `rip` that exits as the script says; HLT after. */
static int32_t exits(int fd, uint32_t reason, uint64_t qual, uint32_t len)
{
    script(reason, qual, len);
    script(EXIT_HLT, 0, 1);
    return run(fd);
}

/** The control registers: CR0's PE, PAE, EFER.LME and PG to long mode,
 * CR8 both ways and its #GP. */
static void cr_rows(int fd)
{
    struct kvm_vcpu *v = vcpu_of(fd);
    struct row *w;
    uint64_t *f;

    set_regs(fd, 0x1000, 0x60000011u, 0, 0);
    CHECK(exits(fd, EXIT_CR_ACCESS, 0, 3) == 0 && regs_of(fd).rip == 0x1004,
          "MOV CR0, EAX is stepped");
    f = fields(fd);
    CHECK(sregs_of(fd).cr0 == 0x60000011u && f[CR0_READ_SHADOW] == 0x60000011u &&
              f[GUEST_CR0] == 0x31u,
          "PE set: the guest sees its CR0 and runs under PE and NE");

    set_regs(fd, 0x1000, CR4_PAE, 0, 0);
    CHECK(exits(fd, EXIT_CR_ACCESS, 4, 3) == 0 && f[GUEST_CR4] == (CR4_PAE | CR4_VMXE) &&
              f[CR4_READ_SHADOW] == CR4_PAE,
          "MOV CR4 sets PAE, and VMXE stays the host's");
    set_regs(fd, 0x1000, GEFER_LME, MSR_EFER, 0);
    CHECK(exits(fd, EXIT_MSR_WRITE, 0, 2) == 0 && sregs_of(fd).efer == GEFER_LME, "WRMSR EFER.LME");
    set_regs(fd, 0x1000, 0x80000011u, 0, 0);
    CHECK(exits(fd, EXIT_CR_ACCESS, 0, 3) == 0 && sregs_of(fd).efer == (GEFER_LME | GEFER_LMA),
          "PG with LME begins long mode");
    CHECK((f[VM_ENTRY_CONTROLS] & ENTRY_IA32E) && f[GUEST_IA32_EFER] == 0x500u &&
              f[GUEST_CR0] == 0x80000031u,
          "the entry is in IA-32e mode with EFER.LMA, which the model's checks pass");
    w = script(EXIT_CPUID, 0, 2);
    w->rip = 0xFFFFFFFF81000000ull;
    w->cs_ar = 0xA09B;
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && regs_of(fd).rip == 0xFFFFFFFF81000003ull,
          "after a far jump to 64-bit code, CPUID and HLT step a RIP above 4 GiB whole");

    set_regs(fd, 0x1000, 5, 0, 0);
    CHECK(exits(fd, EXIT_CR_ACCESS, 8, 4) == 0 && v->sregs.cr8 == 5, "MOV CR8, RAX");
    CHECK(exits(fd, EXIT_CR_ACCESS, 8u | 0x10u | (3u << 8), 4) == 0 && regs_of(fd).rbx == 5,
          "MOV RBX, CR8");
    set_regs(fd, 0x1000, 16, 0, 0);
    CHECK(exits(fd, EXIT_CR_ACCESS, 8, 4) == 0 && regs_of(fd).rip == 0x1001,
          "MOV CR8 of 16 does not complete");
    CHECK(m.injected == (GP_VECTOR | (TYPE_HW_EXC << 8) | INTR_ERRCODE | INTR_VALID) &&
              m.injected_error == 0,
          "and the next entry delivers #GP(0)");
}

/** The CR4 bits the guest owns, as Linux's set_cr4_guest_host_mask gives
 * them: written without an exit, read back by KVM_GET_SREGS, kept by the
 * next write of CR4, and a state KVM_SET_SREGS gave read back whole before
 * the next entry. */
static void cr_owned_rows(int fd)
{
    struct kvm_sregs s, was;
    uint64_t *f = fields(fd);

    CHECK(!(f[CR4_GUEST_HOST_MASK] & (CR4_PGE | CR4_TSD)) && (f[CR4_GUEST_HOST_MASK] & CR4_PAE),
          "the guest owns CR4.PGE and TSD, and not PAE");
    script(EXIT_CPUID, 0, 2)->cr4 = CR4_PGE;
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && sregs_of(fd).cr4 == (CR4_PAE | CR4_PGE),
          "KVM_GET_SREGS gives the PGE the guest set without an exit");
    set_regs(fd, 0x1000, CR4_PAE | CR4_PGE | CR4_TSD, 0, 0);
    script(EXIT_CR_ACCESS, 4, 3)->cr4 = CR4_TSD;
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && f[CR4_READ_SHADOW] == (CR4_PAE | CR4_PGE | CR4_TSD) &&
              f[GUEST_CR4] == (CR4_PAE | CR4_PGE | CR4_TSD | CR4_VMXE),
          "MOV CR4 writes the value whole, the guest's bits among it");
    set_regs(fd, 0x1000, CR4_PAE, 0, 0);
    CHECK(exits(fd, EXIT_CR_ACCESS, 4, 3) == 0 && sregs_of(fd).cr4 == CR4_PAE &&
              f[GUEST_CR4] == (CR4_PAE | CR4_VMXE),
          "and clears them");

    CHECK(exits(fd, EXIT_CPUID, 0, 2) == 0, "CPUID");
    was = s = sregs_of(fd);
    CHECK(exits(fd, EXIT_CPUID, 0, 2) == 0, "CPUID");
    s.gdt.limit = 0x7F;
    CHECK(fdio(fd, KVM_SET_SREGS, (uintptr_t)&s) == 0 && sregs_of(fd).gdt.limit == 0x7F,
          "KVM_GET_SREGS after KVM_SET_SREGS gives the state set, not the VMCS's");
    CHECK(fdio(fd, KVM_SET_SREGS, (uintptr_t)&was) == 0, "KVM_SET_SREGS");
}

/** The guest's KERNEL_GS_BASE as KVM_GET_MSRS reads it. */
static uint64_t guest_kgs(int fd)
{
    struct {
        struct kvm_msrs h;
        struct kvm_msr_entry e[1];
    } msrs;

    msrs.h.nmsrs = 1;
    msrs.h.pad = 0;
    k_memset(msrs.e, 0, sizeof(msrs.e));
    msrs.e[0].index = MSR_KERNEL_GS_BASE;
    CHECK(fdio(fd, KVM_GET_MSRS, (uintptr_t)&msrs) == 1, "KVM_GET_MSRS of KERNEL_GS_BASE");
    return msrs.e[0].data;
}

/** The guest's state that stays in the processor from one exit to the
 * next entry, as Linux's vcpu_load and vmx_prepare_switch_to_guest keep
 * it: the host's saved and the guest's MSRs and FPU loaded at KVM_RUN's
 * first entry, the host's back at its end and at each switch away, and
 * loaded again after one; the guest's XCR0 in for each run alone. The
 * guest is in long mode, with STAR written. */
static void load_rows(int fd)
{
    struct kvm_vcpu *v = vcpu_of(fd);
    int saves, loads, switches, syncs, reloads;

    set_regs(fd, 0x1000, GEFER_LME | GEFER_SCE, MSR_EFER, 0);
    CHECK(exits(fd, EXIT_MSR_WRITE, 0, 2) == 0 && (sregs_of(fd).efer & GEFER_SCE),
          "WRMSR EFER.SCE in long mode");
    syncs = m.fpu_syncs;
    reloads = m.fpu_reloads;
    set_regs(fd, 0x1000, 3, 0, 0);
    CHECK(exits(fd, EXIT_XSETBV, 0, 3) == 0 && v->xcr0 == 3 && m.fpu_syncs == syncs + 1 &&
              m.fpu_reloads == reloads + 1,
          "XSETBV of x87 and SSE saves the registers into the image, which the next run loads");

    saves = m.saves;
    loads = m.fpu_loads;
    set_regs(fd, 0x1000, 0, 0, 0);
    script(EXIT_CPUID, 0, 2);
    script(EXIT_CPUID, 0, 2)->kgs = 0xFFFF888000ABC000ull;
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && m.saves == saves + 1 && m.fpu_loads == loads + 1,
          "three runs in one KVM_RUN save the host's state and load the guest's FPU once");
    CHECK(m.entry_star == 0x0023001000000011ull && m.entry_msr_writes == m.first_msr_writes,
          "the guest's STAR is in for every run, written before the first alone");
    CHECK(!kvm_xcr0_supported() || m.entry_xcr0 == 3,
          "each run has the guest's XCR0, and KVM_RUN ends with the host's");
    CHECK(m.entry_kgs == 0xFFFF888000ABC000ull && guest_kgs(fd) == 0xFFFF888000ABC000ull,
          "the guest's SWAPGS holds to the next run and reaches KVM_GET_MSRS");

    saves = m.saves;
    loads = m.fpu_loads;
    switches = m.switches;
    m.preempt = 2;
    script(EXIT_CPUID, 0, 2)->kgs = 0xFFFF888000DEF000ull;
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && m.switches == switches + 1 && m.saves == saves + 2 &&
              m.fpu_loads == loads + 1,
          "a switch between two runs puts the host's state back, and the next entry saves it "
          "again; the guest's FPU goes with the thread");
    CHECK(m.entry_star == 0x0023001000000011ull && m.entry_kgs == 0xFFFF888000DEF000ull,
          "and loads the guest's STAR and KERNEL_GS_BASE again, as the switch found them");
    set_regs(fd, 0x1000, 7, 0, 0);
    CHECK(exits(fd, EXIT_XSETBV, 0, 3) == 0 && v->xcr0 == 7, "XSETBV of x87, SSE and AVX again");
    set_regs(fd, 0x1000, GEFER_LME, MSR_EFER, 0);
    CHECK(exits(fd, EXIT_MSR_WRITE, 0, 2) == 0 && sregs_of(fd).efer == (GEFER_LME | GEFER_LMA),
          "WRMSR EFER without SCE");
}

/** The MSRs of particular processors that Linux's KVM reads as zero or
 * whose writes it drops; `gpe` is the #GP that any other access injects. */
static void msr_family_rows(int fd, uint32_t gpe)
{
    set_regs(fd, 0x1000, 0x55, 0xC0010010u, 0x66);
    CHECK(exits(fd, EXIT_MSR_READ, 0, 2) == 0 && m.injected == 0 && regs_of(fd).rax == 0 &&
              regs_of(fd).rdx == 0 && regs_of(fd).rip == 0x1003,
          "RDMSR of AMD's SYSCFG reads zero, as Linux's KVM reads it");
    set_regs(fd, 0x1000, 0, 0xC001001Fu, 0x4000);
    CHECK(exits(fd, EXIT_MSR_WRITE, 0, 2) == 0 && m.injected == 0 && regs_of(fd).rip == 0x1003,
          "WRMSR of AMD's NB_CFG is taken and dropped");
    set_regs(fd, 0x1000, 0, 0xC001001Fu, 0);
    CHECK(exits(fd, EXIT_MSR_READ, 0, 2) == 0 && m.injected == 0 && regs_of(fd).rax == 0 &&
              regs_of(fd).rdx == 0,
          "and NB_CFG still reads zero");
    set_regs(fd, 0x1000, 1, 0xC0010010u, 0);
    CHECK(exits(fd, EXIT_MSR_WRITE, 0, 2) == 0 && m.injected == gpe && regs_of(fd).rip == 0x1001,
          "WRMSR of SYSCFG, which Linux does not drop, is #GP");
    set_regs(fd, 0x1000, 0, 0xC0010058u, 0);
    CHECK(exits(fd, EXIT_MSR_WRITE, 0, 2) == 0 && m.injected == 0 && regs_of(fd).rip == 0x1003,
          "WRMSR of zero to the MMIO configuration base is dropped");
    set_regs(fd, 0x1000, 1, 0xC0010058u, 0);
    CHECK(exits(fd, EXIT_MSR_WRITE, 0, 2) == 0 && m.injected == gpe, "and of another value is #GP");
    set_regs(fd, 0x1000, 0x55, 0x17, 0x66);
    CHECK(exits(fd, EXIT_MSR_READ, 0, 2) == 0 && m.injected == 0 && regs_of(fd).rax == 0,
          "RDMSR of the platform id reads zero");
    set_regs(fd, 0x1000, 1, 0x17, 0);
    CHECK(exits(fd, EXIT_MSR_WRITE, 0, 2) == 0 && m.injected == gpe,
          "WRMSR of the platform id is #GP");
}

/** KVM_SET_MSRS of `value` to SYSCALL_MASK on `fd`, with `set` 1 to
 * get it back instead; the number of MSRs done, and the value in `*got`
 * when getting. */
static int32_t sfmask_io(int fd, int set, uint64_t value, uint64_t *got)
{
    struct {
        struct kvm_msrs h;
        struct kvm_msr_entry e[1];
    } msrs;
    int32_t r;

    msrs.h.nmsrs = 1;
    msrs.h.pad = 0;
    k_memset(msrs.e, 0, sizeof(msrs.e));
    msrs.e[0].index = MSR_SYSCALL_MASK;
    msrs.e[0].data = value;
    r = fdio(fd, set ? KVM_SET_MSRS : KVM_GET_MSRS, (uintptr_t)&msrs);
    if (got) {
        *got = msrs.e[0].data;
    }
    return r;
}

/** SYSCALL_MASK of the vCPU on `fd`, by KVM_GET_MSRS. */
static uint64_t sfmask_of(int fd)
{
    uint64_t v = 0;

    CHECK(sfmask_io(fd, 0, 0, &v) == 1, "KVM_GET_MSRS of SYSCALL_MASK");
    return v;
}

/**
 * The guest's WRMSR of the MSRs VMX loads between exits, STAR and
 * SYSCALL_MASK, in long mode; `gpe` is the #GP. SYSCALL_MASK's bits 63:32,
 * which the processor refuses, are kept from the guest and the caller
 * without EFER.SCE, where Linux does not load the MSR, and refused with it,
 * where its WRMSR fails (vmx/vmx.c:718-731, 1916-1925); the entry's load of
 * a kept one, once EFER.SCE is on, is caught and leaves the processor's
 * value, as Linux's (vmx/vmx.c:1302-1311).
 */
static void uret_msr_rows(int fd, uint32_t gpe)
{
    int faults;

    set_regs(fd, 0x1000, 0x11, MSR_STAR, 0x00230010u);
    CHECK(exits(fd, EXIT_MSR_WRITE, 0, 2) == 0 &&
              vx(vcpu_of(fd))->gmsr[0] == 0x0023001000000011ull &&
              m.msr[msr_slot(MSR_STAR)] == HOST_MSR(MSR_STAR),
          "WRMSR STAR reaches the guest's copy, and the host's is back after KVM_RUN");
    CHECK(!(sregs_of(fd).efer & GEFER_SCE), "the rows start in long mode without EFER.SCE");
    set_regs(fd, 0x1000, 0x700, MSR_SYSCALL_MASK, 1);
    CHECK(exits(fd, EXIT_MSR_WRITE, 0, 2) == 0 && m.injected == 0 && regs_of(fd).rip == 0x1003 &&
              sfmask_of(fd) == 0x100000700ull,
          "without EFER.SCE WRMSR SYSCALL_MASK with a bit of 63:32 is kept, as Linux keeps it");
    CHECK(sfmask_io(fd, 1, 0x200000700ull, NULL) == 1 && sfmask_of(fd) == 0x200000700ull,
          "and so is the caller's KVM_SET_MSRS of one");
    faults = m.msr_faults;
    set_regs(fd, 0x1000, GEFER_LME | GEFER_SCE, MSR_EFER, 0);
    CHECK(exits(fd, EXIT_MSR_WRITE, 0, 2) == 0 && (sregs_of(fd).efer & GEFER_SCE) &&
              m.msr_faults == faults + 1 && m.entry_sfmask == HOST_MSR(MSR_SYSCALL_MASK) &&
              m.msr[msr_slot(MSR_SYSCALL_MASK)] == HOST_MSR(MSR_SYSCALL_MASK),
          "once WRMSR EFER turns SCE on, the entry's WRMSR of the kept SYSCALL_MASK faults, "
          "is caught and leaves the processor's value, as Linux's ignored failure");
    set_regs(fd, 0x1000, 0x700, MSR_SYSCALL_MASK, 1);
    CHECK(exits(fd, EXIT_MSR_WRITE, 0, 2) == 0 && m.injected == gpe && regs_of(fd).rip == 0x1001 &&
              sfmask_of(fd) == 0x200000700ull,
          "with EFER.SCE WRMSR SYSCALL_MASK with a bit of 63:32 is #GP, as Linux's failed WRMSR");
    CHECK(sfmask_io(fd, 1, 0x100000700ull, NULL) == 0 && sfmask_of(fd) == 0x200000700ull,
          "and the caller's KVM_SET_MSRS of one stops short of it");
    CHECK(sfmask_io(fd, 1, 0x700u, NULL) == 1, "KVM_SET_MSRS of one within bits 31:0");
    script(EXIT_CPUID, 0, 2);
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && m.entry_sfmask == 0x700u &&
              m.msr[msr_slot(MSR_SYSCALL_MASK)] == HOST_MSR(MSR_SYSCALL_MASK),
          "reaches the processor for the runs, and the host's is back after KVM_RUN");
    set_regs(fd, 0x1000, 0x700, MSR_SYSCALL_MASK, 0);
    CHECK(exits(fd, EXIT_MSR_WRITE, 0, 2) == 0 && m.injected == 0 &&
              vx(vcpu_of(fd))->gmsr[2] == 0x700u && m.entry_sfmask == 0x700u,
          "and so does the guest's WRMSR of one");
    set_regs(fd, 0x1000, GEFER_LME, MSR_EFER, 0);
    CHECK(exits(fd, EXIT_MSR_WRITE, 0, 2) == 0 && sregs_of(fd).efer == (GEFER_LME | GEFER_LMA),
          "WRMSR EFER turns SCE off again");
}

/** The debug registers, MSRs, XSETBV, VMCALL and the instructions the
 * kernel answers alone. */
static void insn_rows(int fd)
{
    struct kvm_vcpu *v = vcpu_of(fd);
    uint32_t ud = UD_VECTOR | (TYPE_HW_EXC << 8) | INTR_VALID;
    uint32_t gpe = GP_VECTOR | (TYPE_HW_EXC << 8) | INTR_ERRCODE | INTR_VALID;

    set_regs(fd, 0x1000, 0x401, 0, 0);
    CHECK(exits(fd, EXIT_DR_ACCESS, 7, 3) == 0 && fields(fd)[GUEST_DR7] == 0x401u,
          "MOV DR7 reaches the VMCS at the next entry");
    CHECK(exits(fd, EXIT_DR_ACCESS, 7u | 0x10u | (3u << 8), 3) == 0 && regs_of(fd).rbx == 0x401,
          "MOV RBX, DR7");

    set_regs(fd, 0x1000, 0, MSR_EFER, 0);
    CHECK(exits(fd, EXIT_MSR_READ, 0, 2) == 0 && regs_of(fd).rax == 0x500 && regs_of(fd).rdx == 0,
          "RDMSR EFER");
    set_regs(fd, 0x1000, 0, 0x12345678u, 0);
    CHECK(exits(fd, EXIT_MSR_READ, 0, 2) == 0 && m.injected == gpe && regs_of(fd).rip == 0x1001,
          "RDMSR of an MSR nobody knows is #GP");
    msr_family_rows(fd, gpe);
    set_regs(fd, 0x1000, 1, 0x6E0, 0);
    CHECK(exits(fd, EXIT_MSR_WRITE, 0, 2) == 0 && m.injected == 0 && regs_of(fd).rip == 0x1003,
          "WRMSR of the TSC deadline goes to the local APIC's MSRs");
    uret_msr_rows(fd, gpe);

    set_regs(fd, 0x1000, 7, 0, 0);
    CHECK(exits(fd, EXIT_XSETBV, 0, 3) == 0 && v->xcr0 == 7 && regs_of(fd).rip == 0x1004,
          "XSETBV of x87, SSE and AVX");
    set_regs(fd, 0x1000, 5, 0, 0);
    CHECK(exits(fd, EXIT_XSETBV, 0, 3) == 0 && v->xcr0 == 7 && m.injected == gpe,
          "AVX without SSE is #GP");

    set_regs(fd, 0x1000, 0, 0, 0);
    CHECK(exits(fd, EXIT_VMCALL, 0, 3) == 0 && (uint32_t)regs_of(fd).rax == (uint32_t)-1000,
          "VMCALL answers -KVM_ENOSYS at CPL 0");
    set_regs(fd, 0x1000, 0, 0, 0);
    CHECK(exits(fd, EXIT_RDPMC, 0, 2) == 0 && m.injected == gpe, "RDPMC is #GP");
    set_regs(fd, 0x1000, 0, 0, 0);
    script(EXIT_INVD, 0, 2);
    script(EXIT_WBINVD, 0, 2);
    script(EXIT_MONITOR, 0, 3);
    script(EXIT_MWAIT, 0, 3);
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && regs_of(fd).rip == 0x100B && m.wbinvds == 0,
          "INVD, WBINVD, MONITOR and MWAIT are stepped; no device needs the flush");
    set_regs(fd, 0x1000, 0, 0, 0);
    CHECK(exits(fd, EXIT_VMXON, 0, 4) == 0 && m.injected == ud, "a guest's VMXON is #UD");
    CHECK(exits(fd, EXIT_GETSEC, 0, 3) == 0 && m.injected == ud, "GETSEC is #UD");
    script(63, 0, 3);
    CHECK(run(fd) == 0 && run_of(fd)->exit_reason == KVM_EXIT_INTERNAL_ERROR &&
              run_of(fd)->u.internal.suberror == KVM_INTERNAL_ERROR_UNEXPECTED_EXIT_REASON,
          "an exit reason the backend does not know is Linux's internal error");
}

/** Linux's rules for a guest's own MSR accesses and KVM's hypercalls. */
static void linux_rows(int fd)
{
    uint32_t gpe = GP_VECTOR | (TYPE_HW_EXC << 8) | INTR_ERRCODE | INTR_VALID;

    set_regs(fd, 0x1000, 0, 0x12345678u, 0);
    CHECK(exits(fd, EXIT_MSR_READ, 0, 2) == 0 && m.injected == gpe, "an unknown MSR is #GP");
    CHECK((regs_of(fd).rflags & 0x10000u) != 0, "the fault's frame has RF set");
    set_regs(fd, 0x1000, 5, 0xC0010000u, 0);
    CHECK(exits(fd, EXIT_MSR_WRITE, 0, 2) == 0 && m.injected == 0 && regs_of(fd).rip == 0x1003,
          "WRMSR of a K7 event select is ignored without a PMU");
    set_regs(fd, 0x1000, 5, 0xC1u, 7);
    CHECK(exits(fd, EXIT_MSR_READ, 0, 2) == 0 && m.injected == 0 && regs_of(fd).rax == 0 &&
              regs_of(fd).rdx == 0,
          "RDMSR of a P6 counter is 0 without a PMU");
    set_regs(fd, 0x1000, 1, 0x401u, 0);
    CHECK(exits(fd, EXIT_MSR_WRITE, 0, 2) == 0 && m.injected == gpe,
          "WRMSR of 1 to a machine-check bank's STATUS is #GP");
    set_regs(fd, 0x1000, 0, 0x401u, 0);
    CHECK(exits(fd, EXIT_MSR_WRITE, 0, 2) == 0 && m.injected == 0 && regs_of(fd).rip == 0x1003,
          "WRMSR of 0 to it clears it");
    set_regs(fd, 0x1000, 11, 0, 0);
    CHECK(exits(fd, EXIT_VMCALL, 0, 3) == 0 && (uint32_t)regs_of(fd).rax == 0,
          "KVM_HC_SCHED_YIELD answers 0");
    set_regs(fd, 0x1000, 9, 0, 0);
    CHECK(exits(fd, EXIT_VMCALL, 0, 3) == 0 && (uint32_t)regs_of(fd).rax == (uint32_t)-95,
          "KVM_HC_CLOCK_PAIRING answers -KVM_EOPNOTSUPP");
    set_regs(fd, 0x1000, 10, 0, 0);
    CHECK(exits(fd, EXIT_VMCALL, 0, 3) == 0 && (uint32_t)regs_of(fd).rax == (uint32_t)-1000,
          "KVM_HC_SEND_IPI answers -KVM_ENOSYS without the local APIC in the kernel");
}

/** Set the interrupt shadow of the vCPU on `fd` to `shadow`. */
static void set_shadow(int fd, uint8_t shadow)
{
    struct kvm_vcpu_events ev;

    CHECK(fdio(fd, KVM_GET_VCPU_EVENTS, (uintptr_t)&ev) == 0, "KVM_GET_VCPU_EVENTS");
    ev.interrupt.shadow = shadow;
    ev.flags = KVM_VCPUEVENT_VALID_SHADOW;
    CHECK(fdio(fd, KVM_SET_VCPU_EVENTS, (uintptr_t)&ev) == 0, "KVM_SET_VCPU_EVENTS");
}

/** The interrupt and NMI windows, and an event the exit found under
 * delivery. */
static void event_rows(int fd)
{
    struct kvm_run *rp = run_of(fd);
    struct kvm_interrupt irq = {0x30};
    struct kvm_regs r;
    struct row *w;

    r = regs_of(fd);
    r.rflags = 2;
    fdio(fd, KVM_SET_REGS, (uintptr_t)&r);
    rp->request_interrupt_window = 1;
    script(EXIT_INTR_WINDOW, 0, 0);
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_IRQ_WINDOW_OPEN &&
              (m.entry_proc & PROC_INTR_WINDOW),
          "the caller's window arms the interrupt-window exit while IF is clear");
    rp->request_interrupt_window = 0;
    r.rflags = 0x202;
    fdio(fd, KVM_SET_REGS, (uintptr_t)&r);
    CHECK(fdio(fd, KVM_INTERRUPT, (uintptr_t)&irq) == 0, "KVM_INTERRUPT 0x30");
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && m.injected == (0x30u | INTR_VALID) && !(m.entry_proc & PROC_INTR_WINDOW),
          "the next entry injects the interrupt");

    set_shadow(fd, SHADOW_STI);
    CHECK(fdio(fd, KVM_NMI, 0) == 0, "KVM_NMI");
    script(EXIT_NMI_WINDOW, 0, 0);
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && m.injected == (NMI_VECTOR | (TYPE_NMI << 8) | INTR_VALID),
          "an NMI behind the STI shadow waits for the NMI window, then is injected");

    w = script(EXIT_EPT_VIOLATION, 0x1, 0);
    w->gpa = 0x3000;
    w->idt_info = 0x30u | INTR_VALID;
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && m.injected == (0x30u | INTR_VALID),
          "an interrupt the exit found under delivery is injected again");
}

/** KVM_SET_GUEST_DEBUG on the vCPU on `fd` with `control` and the
 * hardware breakpoint registers DR0 and DR7. */
static void guest_debug(int fd, uint32_t control, uint64_t dr0, uint64_t dr7)
{
    struct kvm_guest_debug d;

    k_memset(&d, 0, sizeof(d));
    d.control = control;
    d.debugreg[0] = dr0;
    d.debugreg[7] = dr7;
    CHECK(fdio(fd, KVM_SET_GUEST_DEBUG, (uintptr_t)&d) == 0, "KVM_SET_GUEST_DEBUG");
}

/** The caller's single step over STI, which exits in the STI shadow, and
 * the next step, which must enter under TF with the shadow ended, as
 * Linux's vmx_vcpu_run ends it; the registers back after. */
static void sti_step_rows(int fd)
{
    struct kvm_run *rp = run_of(fd);
    struct kvm_regs r, saved;
    struct row *w;

    saved = regs_of(fd);
    r = saved;
    r.rflags |= RFLAGS_IF;
    CHECK(fdio(fd, KVM_SET_REGS, (uintptr_t)&r) == 0, "KVM_SET_REGS with IF");
    w = script(EXIT_EXCEPTION_NMI, 0x4000, 0);
    w->intr_info = DB_VECTOR | (TYPE_HW_EXC << 8) | INTR_VALID;
    w->rip = 0x1004;
    w->intr_state = INTR_STI;
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_DEBUG && rp->u.debug.pc == 0x1004,
          "a single step over STI exits in the STI shadow");
    guest_debug(fd, KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_SINGLESTEP | KVM_GUESTDBG_BLOCKIRQ, 0, 0);
    w = script(EXIT_EXCEPTION_NMI, 0x4000, 0);
    w->intr_info = DB_VECTOR | (TYPE_HW_EXC << 8) | INTR_VALID;
    w->rip = 0x1006;
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_DEBUG && rp->u.debug.pc == 0x1006 &&
              (m.entry_rflags & RFLAGS_TF) && !(m.entry_intr & (INTR_STI | INTR_MOV_SS)),
          "and the next step enters under TF with the shadow ended, as Linux's vmx_vcpu_run "
          "ends it, which the entry's check without a pending single step asks for");
    CHECK(fdio(fd, KVM_SET_REGS, (uintptr_t)&saved) == 0, "KVM_SET_REGS back");
}

/** The caller's debugging: a single step, one over an instruction the
 * kernel runs, steps over STI and on out of its shadow, software and
 * hardware breakpoints, interrupts held, and
 * the guest's own state back after. */
static void debug_rows(int fd)
{
    struct kvm_run *rp = run_of(fd);
    uint64_t *f = fields(fd);
    struct row *w;

    set_regs(fd, 0x1000, 0, 0, 0);
    guest_debug(fd, KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_SINGLESTEP, 0, 0);
    w = script(EXIT_EXCEPTION_NMI, 0x4000, 0);
    w->intr_info = DB_VECTOR | (TYPE_HW_EXC << 8) | INTR_VALID;
    w->rip = 0x1001;
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_DEBUG && rp->u.debug.exception == DB_VECTOR &&
              rp->u.debug.dr6 == (DR6_BS | DR6_FIXED) && rp->u.debug.pc == 0x1001,
          "a single step is KVM_EXIT_DEBUG with DR6.BS at the next instruction");
    CHECK((m.entry_rflags & RFLAGS_TF) && !(regs_of(fd).rflags & RFLAGS_TF) &&
              (f[EXCEPTION_BITMAP] & (1u << DB_VECTOR)),
          "the guest runs under TF and #DB exits, and its RFLAGS hide TF");
    script(EXIT_CPUID, 0, 2);
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_DEBUG && rp->u.debug.pc == 0x1003,
          "an instruction the kernel runs is a step too");

    sti_step_rows(fd);

    guest_debug(fd, KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_USE_SW_BP, 0, 0);
    w = script(EXIT_EXCEPTION_NMI, 0, 1);
    w->intr_info = BP_VECTOR | (TYPE_SOFT_EXC << 8) | INTR_VALID;
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_DEBUG && rp->u.debug.exception == BP_VECTOR &&
              rp->u.debug.pc == 0x1003 && f[EXCEPTION_BITMAP] == (EXC_LINUX | (1u << BP_VECTOR)),
          "INT3 under software breakpoints is KVM_EXIT_DEBUG at the INT3, the only time #BP exits");

    guest_debug(fd, KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_USE_HW_BP, 0x1234, 0x1);
    w = script(EXIT_EXCEPTION_NMI, 0x1, 0);
    w->intr_info = DB_VECTOR | (TYPE_HW_EXC << 8) | INTR_VALID;
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_DEBUG &&
              rp->u.debug.dr6 == (1u | DR6_FIXED) && rp->u.debug.dr7 == 1 && f[GUEST_DR7] == 1 &&
              m.entry_dr0 == 0x1234,
          "a hardware breakpoint of the caller's runs in DR0 and DR7 and reports B0");
    m.dr[0] = 0xFFFFFFFF81234000ull;
    m.dr[7] = DR7_FIXED | 0x2u;
    script(EXIT_CPUID, 0, 2);
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_HLT && m.entry_dr0 == 0x1234 &&
              m.dr[0] == 0xFFFFFFFF81234000ull && m.dr[7] == (DR7_FIXED | 0x2u),
          "under a host DR7 that enables a breakpoint, DR7 is 0x400 while DR0 takes the "
          "caller's, and the host's DR0 and DR7 are back after each run");
    m.dr[0] = 0;
    m.dr[7] = DR7_FIXED;

    guest_debug(fd, KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_BLOCKIRQ, 0, 0);
    CHECK(fdio(fd, KVM_NMI, 0) == 0, "KVM_NMI");
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && m.injected == 0, "KVM_GUESTDBG_BLOCKIRQ holds the NMI");
    guest_debug(fd, 0, 0, 0);
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && m.injected == (NMI_VECTOR | (TYPE_NMI << 8) | INTR_VALID),
          "and it is injected once debugging ends");
    CHECK(f[EXCEPTION_BITMAP] == EXC_LINUX && f[GUEST_DR7] == vcpu_of(fd)->debugregs.dr7,
          "with the bitmap of #DB, #UD, #AC and #MC alone and DR7 the guest's again");
}

/** The host's interrupt, NMI and machine check, the preemption timer, a
 * triple fault and the two kinds of failed entry. */
static void host_rows(int fd)
{
    struct kvm_run *rp = run_of(fd);
    struct row *w;
    int before = warnings, resumes;

    /* HOST_FS_BASE is written when the thread's FS base has changed. */
    m.refuse = HOST_FS_BASE;
    m.fs_base = 0x710000ull;
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && warnings == before + 1, "a failed VMWRITE is reported after the exit");
    m.fs_base = 0x720000ull;
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && warnings == before + 1, "once");
    m.refuse = 0;
    m.fs_base = 0x700000ull;

    script(EXIT_EXTERNAL_INTR, 0, 0);
    script(EXIT_PREEMPTION_TIMER, 0, 0);
    w = script(EXIT_EXCEPTION_NMI, 0, 0);
    w->intr_info = NMI_VECTOR | (TYPE_NMI << 8) | INTR_VALID;
    w = script(EXIT_EXCEPTION_NMI, 0, 0);
    w->intr_info = MC_VECTOR | (TYPE_HW_EXC << 8) | INTR_VALID;
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_HLT && m.nmis == 1 && m.mces == 1,
          "the host's interrupt, the timer, an NMI and a machine check stay in the kernel");

    script(EXIT_TRIPLE_FAULT, 0, 0);
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_SHUTDOWN,
          "a triple fault is KVM_EXIT_SHUTDOWN");

    m.fail_launch = 1;
    resumes = m.resumes;
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_FAIL_ENTRY &&
              rp->u.fail_entry.hardware_entry_failure_reason == ERR_CONTROLS &&
              m.resumes == resumes + 1,
          "VMRESUME's VMfailValid is KVM_EXIT_FAIL_ENTRY with its error");

    set_shadow(fd, SHADOW_STI);
    {
        struct kvm_regs r = regs_of(fd);

        r.rflags = 2;
        fdio(fd, KVM_SET_REGS, (uintptr_t)&r);
    }
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_FAIL_ENTRY &&
              rp->u.fail_entry.hardware_entry_failure_reason ==
                  (EXIT_FAILED_ENTRY | EXIT_INVALID_STATE),
          "a guest state the processor refuses is KVM_EXIT_FAIL_ENTRY with the exit reason");
    set_shadow(fd, 0);
}

/**
 * The host's FREEZE_WHILE_SMM across a run, as Linux keeps it in
 * HOST_OWNED_DEBUGCTL (arch/x86/kvm/vmx/main.c:45): the guest's
 * IA32_DEBUGCTL in the VMCS carries it beside the guest's own LBR
 * (arch/x86/kvm/vmx/vmx.h:441-447), the guest's DEBUGCTL read back after
 * the exit never shows it (:449-452), and the host's DEBUGCTL, which the
 * exit clears, is written back after it (arch/x86/kvm/vmx/vmx.c:7445-7446).
 * Without the bit the guest's DEBUGCTL is its own; the host's LBR alone is
 * written back too, but never reaches the guest.
 */
static void debugctl_rows(int fd)
{
    struct kvm_vcpu *v = vcpu_of(fd);
    uint32_t lbr_nr = v->pmu.lbr_nr;
    uint64_t guest = v->pmu.debugctl;
    int writes;

    v->pmu.lbr_nr = 1;
    v->pmu.debugctl = DEBUGCTL_LBR;
    m.debugctl = DEBUGCTL_FREEZE_SMM;
    writes = m.debugctl_writes;
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && m.entry_debugctl == (DEBUGCTL_LBR | DEBUGCTL_FREEZE_SMM),
          "the guest runs with the host's FREEZE_WHILE_SMM beside its own LBR");
    CHECK(v->pmu.debugctl == DEBUGCTL_LBR,
          "the guest's DEBUGCTL read back after the exit lacks the host's bit");
    CHECK(m.debugctl == DEBUGCTL_FREEZE_SMM && m.debugctl_writes == writes + 1,
          "the host's DEBUGCTL, which the exit cleared, is written back");

    m.debugctl = DEBUGCTL_LBR;
    writes = m.debugctl_writes;
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && m.entry_debugctl == DEBUGCTL_LBR && v->pmu.debugctl == DEBUGCTL_LBR,
          "without the host's FREEZE_WHILE_SMM the guest's DEBUGCTL is its own");
    CHECK(m.debugctl == DEBUGCTL_LBR && m.debugctl_writes == writes + 1,
          "and the host's LBR is written back");

    m.debugctl = 0;
    writes = m.debugctl_writes;
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && m.debugctl_writes == writes,
          "a host DEBUGCTL of 0, which the exit leaves, is not written");
    v->pmu.lbr_nr = lbr_nr;
    v->pmu.debugctl = guest;
}

/** A state read between runs on another processor, and VMXON and VMPTRLD
 * failing there. */
static void read_fail_rows(int fd)
{
    int vmxons;
    struct mvmcs *c = mvmcs_of(vx(vcpu_of(fd))->vmcs_pa);

    on_cpu(1);
    vmxons = m.vmxons;
    CHECK(sregs_of(fd).efer == 0x500 && m.vmxons == vmxons + 1 && m.on && c->active == 2 &&
              !m.cpus[0].on && m.misplaced == 0,
          "KVM_GET_SREGS on another processor makes the VMCS current there, as Linux's vcpu_load");

    on_cpu(0);
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && c->active == 1 && !m.cpus[1].on && m.cpus[1].cr4 == HOST_CR4_MODEL,
          "a run back on the first processor clears the VMCS on the second");
    on_cpu(1);
    m.fail_vmxon = 1;
    CHECK(run(fd) == -EIO && !m.on && m.cr4 == HOST_CR4_MODEL && !m.cpus[0].on && c->active == 0,
          "VMXON failing ends KVM_RUN with EIO, CR4 back, the VMCS clear and active nowhere");
    m.fail_vmxon = 0;
    m.fail_vmptrld = 1;
    CHECK(run(fd) == -EIO && !m.on, "VMPTRLD failing turns VMX off and ends KVM_RUN with EIO");
    m.fail_vmptrld = 0;
    m.cr0 = 0x80050013ull;
    CHECK(run(fd) == -EIO && m.gp == 0 && !m.on && m.cr4 == HOST_CR4_MODEL,
          "a host CR0 without NE ends KVM_RUN with EIO before VMXON could #GP");
    m.cr0 = 0x80050033ull;
    on_cpu(0);
}

/** Another processor, which the VMCS moves to and back from, a failed
 * entry on the VMLAUNCH back, then `read_fail_rows`. */
static void path_rows(int fd)
{
    int calls, launches, resumes, vmptrlds;
    int invepts = m.invepts, invvpids = m.invvpids;
    struct mvmcs *c = mvmcs_of(vx(vcpu_of(fd))->vmcs_pa);
    struct kvm_run *rp = run_of(fd);
    struct kvm_regs r;

    script(EXIT_HLT, 0, 1);
    vmptrlds = m.vmptrlds;
    resumes = m.resumes;
    CHECK(run(fd) == 0 && m.vmptrlds == vmptrlds && m.resumes == resumes + 1,
          "on the processor that holds the VMCS current the entry is a VMRESUME, with no VMPTRLD");

    on_cpu(1);
    m.fc = 0;
    calls = m.calls;
    launches = m.launches;
    resumes = m.resumes;
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && fields(fd)[HOST_GS_BASE] == 0xFFFFFFFF81001000ull,
          "on another processor the host's fields are its");
    CHECK(m.calls == calls + 1 && !m.cpus[0].on && m.cpus[0].cr4 == HOST_CR4_MODEL,
          "the VMCS is cleared on the processor it left, which leaves VMX operation");
    CHECK(m.launches == launches + 1 && m.resumes == resumes && c->active == 2,
          "and the entry on the new one is a VMLAUNCH");
    CHECK(m.invepts == invepts + 1 && m.invvpids == invvpids + 1,
          "and the guest's translations are dropped there");
    CHECK(m.fc_writes == 1 && m.fc == 5, "an unlocked IA32_FEATURE_CONTROL is locked with VMX on");
    on_cpu(0);
    launches = m.launches;
    resumes = m.resumes;
    r = regs_of(fd);
    r.rflags = 2;
    fdio(fd, KVM_SET_REGS, (uintptr_t)&r);
    set_shadow(fd, SHADOW_STI);
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_FAIL_ENTRY && m.launches == launches + 1 &&
              m.resumes == resumes && !vx(vcpu_of(fd))->launched,
          "a VMLAUNCH that fails on the guest's state, the VMCS just moved back, leaves it "
          "unlaunched");
    set_shadow(fd, 0);
    script(EXIT_CPUID, 0, 2);
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_HLT && m.launches == launches + 2 &&
              m.resumes == resumes + 1 && !m.cpus[1].on && c->active == 1,
          "so the next entry is a VMLAUNCH again, as Linux sets launched only after an exit "
          "that is not a failed entry, and the one after it a VMRESUME");
    read_fail_rows(fd);
}

/** PAE paging: the PDPTEs loaded at KVM_SET_SREGS, and one with a reserved
 * bit refused at a CR4 write. */
static void pae_rows(int fd)
{
    struct kvm_sregs s = sregs_of(fd);
    uint64_t pdpte = 0x5001, bad = 0x6003, zero = 0;

    k_memcpy(&ram[0x4000], &pdpte, 8);
    s.cs.l = 0;
    s.cr0 = 0x80000011u;
    s.cr4 = CR4_PAE;
    s.efer = 0;
    s.cr3 = 0x4000;
    CHECK(fdio(fd, KVM_SET_SREGS, (uintptr_t)&s) == 0, "KVM_SET_SREGS to PAE paging");
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && fields(fd)[GUEST_PDPTR0] == 0x5001u &&
              !(fields(fd)[VM_ENTRY_CONTROLS] & ENTRY_IA32E),
          "the PDPTEs are read from CR3 for the entry");
    k_memcpy(&ram[0x4008], &bad, 8);
    set_regs(fd, 0x1000, CR4_PAE | CR4_PGE, 0, 0);
    CHECK(exits(fd, EXIT_CR_ACCESS, 4, 3) == 0 && sregs_of(fd).cr4 == CR4_PAE &&
              m.injected == (GP_VECTOR | (TYPE_HW_EXC << 8) | INTR_ERRCODE | INTR_VALID),
          "a PDPTE with a reserved bit makes the CR4 write #GP");
    k_memcpy(&ram[0x4008], &zero, 8);
}

/** A second vCPU's VPID, two vCPUs on one processor, and a panic inside a
 * run. `first` is the first vCPU's descriptor. */
static void vpid_panic_rows(int vm, int first)
{
    struct kvm_vcpu *v;
    int fd = make_vcpu(vm, 1), launches, resumes, vmptrlds, vmxons, i;
    struct mvmcs *a = mvmcs_of(vx(vcpu_of(first))->vmcs_pa), *b;

    v = vcpu_of(fd);
    CHECK(vx(v)->vpid == 2, "the second vCPU has VPID 2");
    script(EXIT_HLT, 0, 1);
    run(first);
    script(EXIT_HLT, 0, 1);
    launches = m.launches;
    vmxons = m.vmxons;
    CHECK(run(fd) == 0 && m.launches == launches + 1 && m.vmxons == vmxons,
          "a second vCPU on the processor is launched in the same VMXON");
    b = mvmcs_of(vx(v)->vmcs_pa);
    resumes = m.resumes;
    vmptrlds = m.vmptrlds;
    script(EXIT_HLT, 0, 1);
    CHECK(run(first) == 0 && m.resumes == resumes + 1 && m.vmptrlds == vmptrlds + 1 &&
              a->active == 1 && b->active == 1 && b->launched,
          "the first vCPU's VMCS, loaded again over the second's, is resumed, and the second's "
          "stays active and launched");
    script(EXIT_HLT, 0, 0)->panic = 1;
    if (setjmp(panic_jump) == 0) {
        fdio(fd, KVM_RUN, 0);
        CHECK(0, "the panic row returns through the hook");
    }
    CHECK(!m.on && m.cr4 == HOST_CR4_MODEL && !vmx_live[0] && m.ud == 0 && !a->active &&
              !b->active && !vx(vcpu_of(first))->launched && vx(v)->cpu == -1,
          "a panic in the run clears every VMCS active there, turns VMX off and puts CR4 back");
    /* The panic left the host's state and FPU with the guest's. */
    irq_on = 1;
    bkl_depth = 0;
    v->mode = KVM_MODE_OUTSIDE;
    mutex_init(&v->mutex, "kvm_vcpu"); /* the run's hold, left by the jump */
    current_proc->sched_out = NULL;
    m.saved = 0;
    m.restores = m.saves;
    m.fpu_held = 0;
    m.fpu_stores = m.fpu_loads;
    m.xcr0 = HOST_XCR0_MODEL;
    m.pkru = HOST_PKRU_MODEL;
    for (i = 0; i < 5; i++) {
        m.msr[i] = HOST_MSR(kept_msrs[i]);
    }
    k_memset(uret_curr, 0, sizeof(uret_curr));
    k_memset(uret_read, 0, sizeof(uret_read));
    m.script_len = 0;
    m.script_at = 0;
    fdclose(fd);
}

/* ---- the task switch ---- */

/* The task-switch rows' guest: a 32-bit protected-mode guest without
 * paging, whose GDT, LDT, IDT and TSSes are in RAM from 0x80000. */
#define TS_GDT  0x80000u
#define TS_TSSA 0x81000u /* the first task's TSS, 32-bit, selector 0x18 */
#define TS_TSSB 0x81100u /* a 32-bit TSS, selector 0x20 */
#define TS_TSSC 0x81200u /* a 16-bit TSS, selector 0x30 */
#define TS_TSSF 0x81300u /* a 32-bit TSS whose DS is not present, 0x60 */
#define TS_TSSG 0x81400u /* a 32-bit TSS of a virtual-8086 task, 0x68 */
#define TS_LDT  0x82000u
#define TS_IDT  0x83000u

/** Store the 32-bit `x` in RAM at `a`. */
static void put32(uint32_t a, uint32_t x)
{
    k_memcpy(&ram[a], &x, 4);
}

/** The 32-bit value in RAM at `a`. */
static uint32_t get32(uint32_t a)
{
    uint32_t x;

    k_memcpy(&x, &ram[a], 4);
    return x;
}

/** The 16-bit value in RAM at `a`. */
static uint32_t get16(uint32_t a)
{
    return (uint32_t)ram[a] | ((uint32_t)ram[a + 1u] << 8);
}

/** Write a descriptor at `a`: `base`, the 20-bit `limit`, the access byte
 * `access` and the flags nibble `flags` (G, D/B, L, AVL). */
static void put_desc(uint32_t a, uint32_t base, uint32_t limit, uint32_t access, uint32_t flags)
{
    uint8_t *d = &ram[a];

    d[0] = (uint8_t)limit;
    d[1] = (uint8_t)(limit >> 8);
    d[2] = (uint8_t)base;
    d[3] = (uint8_t)(base >> 8);
    d[4] = (uint8_t)(base >> 16);
    d[5] = (uint8_t)access;
    d[6] = (uint8_t)(((limit >> 16) & 0xFu) | (flags << 4));
    d[7] = (uint8_t)(base >> 24);
}

/** Write a task gate to the TSS `sel` with `dpl` at IDT vector `vec`. */
static void put_gate(uint32_t vec, uint32_t sel, uint32_t dpl)
{
    put_desc(TS_IDT + vec * 8u, sel << 16, 0, 0x85u | (dpl << 5), 0);
}

/** Fill a 32-bit TSS at `a`: CR3 0, `eip`, `eflags`, EAX `eax`, ESP
 * `esp`, CS 0x08, the data selectors `ds`, FS 0x04 from the LDT, GS 0x58,
 * which is not yet accessed, and the LDT 0x28. */
static void put_tss32(uint32_t a, uint32_t eip, uint32_t eflags, uint32_t eax, uint32_t esp,
                      uint32_t ds)
{
    k_memset(&ram[a], 0, 104);
    put32(a + 32u, eip);
    put32(a + 36u, eflags);
    put32(a + 40u, eax);
    put32(a + 56u, esp);
    put32(a + 72u, 0x10);
    put32(a + 76u, 0x08);
    put32(a + 80u, 0x10);
    put32(a + 84u, ds);
    put32(a + 88u, 0x04);
    put32(a + 92u, 0x58);
    put32(a + 96u, 0x28);
}

/** Lay out the tables and TSSes of the task-switch rows. */
static void ts_tables(void)
{
    put_desc(TS_GDT + 0x08u, 0, 0xFFFFF, 0x9B, 0xC);
    put_desc(TS_GDT + 0x10u, 0, 0xFFFFF, 0x93, 0xC);
    put_desc(TS_GDT + 0x18u, TS_TSSA, 0x67, 0x8B, 0);
    put_desc(TS_GDT + 0x20u, TS_TSSB, 0x67, 0x89, 0);
    put_desc(TS_GDT + 0x28u, TS_LDT, 0x0F, 0x82, 0);
    put_desc(TS_GDT + 0x30u, TS_TSSC, 0x2B, 0x81, 0);
    put_desc(TS_GDT + 0x38u, 0x81500, 0x50, 0x89, 0);
    put_desc(TS_GDT + 0x40u, 0, 0xFFFFF, 0x13, 0xC);
    put_desc(TS_GDT + 0x48u, 0, 0xFFFF, 0x9B, 0);
    put_desc(TS_GDT + 0x50u, 0, 0xFFFF, 0x93, 0);
    put_desc(TS_GDT + 0x58u, 0, 0xFFFFF, 0x92, 0xC);
    put_desc(TS_GDT + 0x60u, TS_TSSF, 0x67, 0x89, 0);
    put_desc(TS_GDT + 0x68u, TS_TSSG, 0x67, 0x89, 0);
    put_desc(TS_GDT + 0x70u, 0x200000, 0x67, 0x89, 0);
    put_desc(TS_LDT, 0, 0xFFFFF, 0x93, 0xC);
    put_gate(0x20, 0x20, 3);
    put_gate(0x21, 0x20, 0);
    put_gate(NMI_VECTOR, 0x20, 0);
    put_gate(GP_VECTOR, 0x20, 0);
    put_tss32(TS_TSSA, 0, 0, 0, 0, 0x10);
    put_tss32(TS_TSSB, 0x5000, 0x202, 0x11, 0x7000, 0x10);
    put_tss32(TS_TSSF, 0x5800, 0x2, 0x33, 0x7400, 0x40);
    put_tss32(TS_TSSG, 0x0100, 0x20202, 0x44, 0xFFE, 0x10);
    put32(TS_TSSG + 76u, 0x1234);
    put32(TS_TSSG + 80u, 0x2000);
    k_memset(&ram[TS_TSSC], 0, 44);
    ram[TS_TSSC + 14u] = 0x00;
    ram[TS_TSSC + 15u] = 0x60;
    ram[TS_TSSC + 16u] = 0x02;
    ram[TS_TSSC + 18u] = 0x34;
    ram[TS_TSSC + 19u] = 0x12;
    ram[TS_TSSC + 27u] = 0x78;
    ram[TS_TSSC + 34u] = 0x50;
    ram[TS_TSSC + 36u] = 0x48;
    ram[TS_TSSC + 38u] = 0x50;
    ram[TS_TSSC + 40u] = 0x50;
}

/** A flat 32-bit segment of DPL 0 with the selector `sel`, code by `code`. */
static struct kvm_segment flat(uint16_t sel, int code)
{
    struct kvm_segment s;

    k_memset(&s, 0, sizeof(s));
    s.selector = sel;
    s.limit = 0xFFFFFFFFu;
    s.type = code ? 0xB : 0x3;
    s.present = 1;
    s.s = 1;
    s.db = 1;
    s.g = 1;
    return s;
}

/** Put the vCPU on `fd` in the first task, 0x18, at EIP 0x1000 with EAX
 * 0xAAAA, ESP 0x9000 and DR7's L0, G0 and LE set. */
static void ts_start(int fd)
{
    struct kvm_sregs s = sregs_of(fd);
    struct kvm_regs r = regs_of(fd);
    struct kvm_debugregs d;

    s.cr0 = 0x11;
    s.cr4 = 0;
    s.efer = 0;
    s.cs = flat(0x08, 1);
    s.ss = s.ds = s.es = s.fs = s.gs = flat(0x10, 0);
    k_memset(&s.tr, 0, sizeof(s.tr));
    s.tr.selector = 0x18;
    s.tr.base = TS_TSSA;
    s.tr.limit = 0x67;
    s.tr.type = 11;
    s.tr.present = 1;
    k_memset(&s.ldt, 0, sizeof(s.ldt));
    s.ldt.unusable = 1;
    s.gdt.base = TS_GDT;
    s.gdt.limit = 0x7F;
    s.idt.base = TS_IDT;
    s.idt.limit = 0x1FF;
    CHECK(fdio(fd, KVM_SET_SREGS, (uintptr_t)&s) == 0, "KVM_SET_SREGS to the first task");
    k_memset(&r, 0, sizeof(r));
    r.rip = 0x1000;
    r.rflags = 0x2;
    r.rax = 0xAAAA;
    r.rsp = 0x9000;
    CHECK(fdio(fd, KVM_SET_REGS, (uintptr_t)&r) == 0, "KVM_SET_REGS in the first task");
    k_memset(&d, 0, sizeof(d));
    d.dr6 = 0xFFFF0FF0u;
    d.dr7 = 0x557;
    CHECK(fdio(fd, KVM_SET_DEBUGREGS, (uintptr_t)&d) == 0, "KVM_SET_DEBUGREGS");
}

/** A task switch to `sel` from `reason` with an instruction of `len`
 * bytes, then a HLT in the new task, on `fd`: KVM_RUN's result. */
static int32_t ts_run(int fd, uint32_t sel, uint32_t reason, uint32_t len)
{
    script(EXIT_TASK_SWITCH, sel | ((uint64_t)reason << 30), len);
    script(EXIT_HLT, 0, 1);
    return run(fd);
}

/** CALL to a 32-bit TSS, then IRET back. */
static void ts_call_rows(int fd)
{
    struct kvm_sregs s;
    struct kvm_regs r;
    struct kvm_debugregs d;

    CHECK(ts_run(fd, 0x20, KVM_TS_CALL, 7) == 0, "CALL to a TSS runs the new task");
    s = sregs_of(fd);
    r = regs_of(fd);
    CHECK(get32(TS_TSSA + 32u) == 0x1007 && get32(TS_TSSA + 40u) == 0xAAAA &&
              get32(TS_TSSA + 56u) == 0x9000 && get32(TS_TSSA + 76u) == 0x08 &&
              get32(TS_TSSA + 36u) == 0x2,
          "the old task saves the EIP after the CALL, its registers and selectors");
    CHECK(get16(TS_TSSB) == 0x18 && ram[TS_GDT + 0x20u + 5u] == 0x8B &&
              ram[TS_GDT + 0x18u + 5u] == 0x8B,
          "the new TSS links back and both are busy");
    CHECK(s.tr.selector == 0x20 && s.tr.base == TS_TSSB && s.tr.type == 11 && (s.cr0 & 8u) &&
              s.ldt.selector == 0x28 && s.ldt.base == TS_LDT && s.fs.selector == 0x04 &&
              s.gs.type == 3 && ram[TS_GDT + 0x58u + 5u] == 0x93,
          "TR, CR0.TS, the LDT, a segment from it and one marked accessed");
    CHECK(r.rip == 0x5001 && r.rax == 0x11 && r.rsp == 0x7000 && r.rflags == 0x4202,
          "the new task's registers, with NT set");
    CHECK(fdio(fd, KVM_GET_DEBUGREGS, (uintptr_t)&d) == 0 && d.dr7 == 0x402,
          "DR7's local enables are cleared");
    CHECK(ts_run(fd, 0x18, KVM_TS_IRET, 1) == 0, "IRET with NT returns to the old task");
    s = sregs_of(fd);
    r = regs_of(fd);
    CHECK(s.tr.selector == 0x18 && r.rip == 0x1008 && r.rax == 0xAAAA && r.rflags == 0x2 &&
              ram[TS_GDT + 0x20u + 5u] == 0x89 && ram[TS_GDT + 0x18u + 5u] == 0x8B,
          "IRET restores the old task, frees the nested one and leaves the old one busy");
    CHECK(get32(TS_TSSB + 32u) == 0x5002 && get32(TS_TSSB + 36u) == 0x202,
          "the nested task saves its EIP after IRET and its EFLAGS without NT");
}

/** JMP to a 16-bit TSS and back. */
static void ts_jmp_rows(int fd)
{
    struct kvm_sregs s;
    struct kvm_regs r;

    CHECK(ts_run(fd, 0x30, KVM_TS_JMP, 7) == 0, "JMP to a 16-bit TSS runs the new task");
    s = sregs_of(fd);
    r = regs_of(fd);
    CHECK(ram[TS_GDT + 0x18u + 5u] == 0x89 && ram[TS_GDT + 0x30u + 5u] == 0x83 &&
              get16(TS_TSSC) == 0,
          "JMP frees the old task, marks the new one busy and links nothing");
    CHECK(r.rip == 0x6001 && r.rax == 0x1234 && r.rsp == 0x7800 && r.rflags == 0x2 &&
              s.cs.selector == 0x48 && !s.cs.db && s.ss.selector == 0x50 && s.tr.type == 3 &&
              s.fs.selector == 0x10 && s.ldt.unusable,
          "the 16-bit task's registers, its 16-bit code, a null LDT and FS kept");
    CHECK(ts_run(fd, 0x18, KVM_TS_JMP, 5) == 0, "JMP back to the 32-bit task");
    s = sregs_of(fd);
    r = regs_of(fd);
    CHECK(get16(TS_TSSC + 14u) == 0x6006 && get16(TS_TSSC + 18u) == 0x1234 &&
              get16(TS_TSSC + 36u) == 0x48 && ram[TS_GDT + 0x30u + 5u] == 0x81,
          "the 16-bit task saves its state in its own format");
    CHECK(r.rip == 0x1010 && r.rax == 0xAAAA && s.tr.selector == 0x18 && s.cs.db,
          "the 32-bit task resumes after its JMP");
}

/** A task gate reached by INT n, by an exception with an error code, by
 * an external interrupt and by an NMI, each followed by IRET. */
static void ts_gate_rows(int fd)
{
    struct kvm_vcpu_events ev;
    struct row *w;

    w = script(EXIT_TASK_SWITCH, 0x20 | ((uint64_t)KVM_TS_GATE << 30), 2);
    w->idt_info = 0x20u | (TYPE_SOFT_INTR << 8) | INTR_VALID;
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && m.injected == 0 && get32(TS_TSSA + 32u) == 0x1012 &&
              get16(TS_TSSB) == 0x18 && (regs_of(fd).rflags & 0x4000u),
          "INT n through a task gate steps past the INT, links back and sets NT");
    CHECK(ts_run(fd, 0x18, KVM_TS_IRET, 1) == 0, "and IRET returns");
    w = script(EXIT_TASK_SWITCH, 0x20 | ((uint64_t)KVM_TS_GATE << 30), 0);
    w->idt_info = GP_VECTOR | (TYPE_HW_EXC << 8) | INTR_ERRCODE | INTR_VALID;
    w->idt_error = 0x5678;
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && m.injected == 0 && get32(TS_TSSA + 32u) == 0x1013 &&
              regs_of(fd).rsp == 0x6FFC && get32(0x6FFC) == 0x5678,
          "an exception through a task gate pushes its error code on the new task's stack");
    CHECK(ts_run(fd, 0x18, KVM_TS_IRET, 1) == 0, "and IRET returns");
    w = script(EXIT_TASK_SWITCH, 0x20 | ((uint64_t)KVM_TS_GATE << 30), 0);
    w->idt_info = 0x22u | (TYPE_EXT_INTR << 8) | INTR_VALID;
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && m.injected == 0 && get32(TS_TSSA + 32u) == 0x1014,
          "an external interrupt through a task gate is not injected again");
    CHECK(ts_run(fd, 0x18, KVM_TS_IRET, 1) == 0, "and IRET returns");
    w = script(EXIT_TASK_SWITCH, 0x20 | ((uint64_t)KVM_TS_GATE << 30), 0);
    w->idt_info = NMI_VECTOR | (TYPE_NMI << 8) | INTR_VALID;
    script(EXIT_HLT, 0, 1)->intr_state = INTR_BLOCK_NMI;
    CHECK(run(fd) == 0 && m.injected == 0 && (m.entry_intr & INTR_BLOCK_NMI),
          "an NMI through a task gate leaves NMIs blocked in the new task");
    CHECK(fdio(fd, KVM_GET_VCPU_EVENTS, (uintptr_t)&ev) == 0 && ev.nmi.masked && !ev.nmi.injected,
          "KVM_GET_VCPU_EVENTS shows the NMI taken and masked");
    ev.nmi.masked = 0;
    CHECK(fdio(fd, KVM_SET_VCPU_EVENTS, (uintptr_t)&ev) == 0, "KVM_SET_VCPU_EVENTS");
    CHECK(ts_run(fd, 0x18, KVM_TS_IRET, 1) == 0, "and IRET returns");
}

/** The faults of a task switch: the gate's DPL, a TSS too small, a
 * segment not present in the new task, and a TSS in no slot; and a
 * virtual-8086 task. */
static void ts_fault_rows(int fd)
{
    uint32_t gp = GP_VECTOR | (TYPE_HW_EXC << 8) | INTR_ERRCODE | INTR_VALID;
    uint32_t ts = TS_VECTOR | (TYPE_HW_EXC << 8) | INTR_ERRCODE | INTR_VALID;
    struct kvm_sregs s;
    struct row *w;

    w = script(EXIT_TASK_SWITCH, 0x23 | ((uint64_t)KVM_TS_GATE << 30), 2);
    w->idt_info = 0x21u | (TYPE_SOFT_INTR << 8) | INTR_VALID;
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && m.injected == gp && m.injected_error == 0x10A &&
              sregs_of(fd).tr.selector == 0x18 && regs_of(fd).rip == 0x1017,
          "INT n through a gate below the selector's RPL is #GP(vector, IDT) in the old task, "
          "at the INT, which the next row's HLT then steps");
    CHECK(ts_run(fd, 0x38, KVM_TS_JMP, 5) == 0 && m.injected == ts && m.injected_error == 0x38 &&
              sregs_of(fd).tr.selector == 0x18 && regs_of(fd).rip == 0x1018,
          "a TSS below its kind's size is #TS(selector) with nothing changed");
    script(EXIT_TASK_SWITCH, 0x70 | ((uint64_t)KVM_TS_JMP << 30), 5);
    CHECK(run(fd) == 0 && run_of(fd)->exit_reason == KVM_EXIT_INTERNAL_ERROR &&
              run_of(fd)->u.internal.suberror == KVM_INTERNAL_ERROR_EMULATION,
          "a TSS in no slot is Linux's internal error");
    CHECK(ts_run(fd, 0x68, KVM_TS_JMP, 5) == 0, "JMP to a virtual-8086 task");
    s = sregs_of(fd);
    CHECK(s.cs.selector == 0x1234 && s.cs.base == 0x12340 && s.cs.dpl == 3 &&
              s.ss.base == 0x20000 && regs_of(fd).rip == 0x101 && (regs_of(fd).rflags & 0x20000u),
          "its segments are virtual-8086 mode's");
    ts_start(fd);
    CHECK(ts_run(fd, 0x60, KVM_TS_JMP, 5) == 0, "JMP to a task whose DS is not present");
    s = sregs_of(fd);
    CHECK(m.injected == (NP_VECTOR | (TYPE_HW_EXC << 8) | INTR_ERRCODE | INTR_VALID) &&
              m.injected_error == 0x40 && s.tr.selector == 0x60 && regs_of(fd).rax == 0x33,
          "is #NP(selector) in the new task");
}

/** Put the vCPU on `fd` back in the first task, in 32-bit protected mode
 * without paging, at the instruction `bytes` of `n` bytes, with EAX, EBX
 * and ECX as given, ESI at the device's frame and EDI at RAM. */
static void emul_start(int fd, const char *bytes, uint32_t n, uint64_t rax, uint64_t rbx,
                       uint64_t rcx)
{
    struct kvm_regs r;

    ts_start(fd);
    k_memcpy(&ram[0x1000], bytes, n);
    r = regs_of(fd);
    r.rax = rax;
    r.rbx = rbx;
    r.rcx = rcx;
    r.rsi = 0xD0000100ull;
    r.rdi = 0x6000;
    CHECK(fdio(fd, KVM_SET_REGS, (uintptr_t)&r) == 0, "KVM_SET_REGS at the instruction");
}

/** A run of the vCPU on `fd` that ends in an EPT violation at `gpa`,
 * which the caller's MMIO read of `len` bytes at `gpa` must follow. */
static int mmio_read(int fd, uint64_t gpa, uint32_t len)
{
    struct kvm_run *rp = run_of(fd);
    struct row *w = script(EXIT_EPT_VIOLATION, 0x1, 0);

    w->gpa = gpa;
    return run(fd) == 0 && rp->exit_reason == KVM_EXIT_MMIO && !rp->u.mmio.is_write &&
           rp->u.mmio.phys_addr == gpa && rp->u.mmio.len == len;
}

/* The flags the table's rows name. */
#define F_CF 0x001u
#define F_ZF 0x040u
#define F_SF 0x080u
#define F_OF 0x800u

/*
 * One instruction of the table: its bytes, with [EBX] its memory operand
 * on the device's frame; the flags and EAX, ECX and EDX it starts with;
 * the bytes it reads there and the device's answer; the bytes it writes
 * and their value; and what it leaves: the three registers, the flags
 * under a mask, and EIP when it is not the next instruction's.
 */
struct erow {
    const char *name, *bytes;
    uint32_t n, fl;
    uint64_t rax, rcx, rdx;
    uint32_t rlen;
    uint64_t rval;
    uint32_t wlen;
    uint64_t wval;
    uint64_t o_rax, o_rcx, o_rdx;
    uint32_t fmask, fval;
    uint64_t rip;
};

static const struct erow erows[] = {
    {"MUL", "\xf7\x23", 2, 0, 0x10000, 0, 0x55, 4, 0x10000, 0, 0, 0, 0, 1, F_CF | F_OF, F_CF | F_OF,
     0},
    {"IMUL r/m", "\xf7\x2b", 2, 0, 0xFFFFFFFFu, 0, 0x55, 4, 5, 0, 0, 0xFFFFFFFBu, 0, 0xFFFFFFFFu,
     F_CF | F_OF, 0, 0},
    {"DIV", "\xf7\x33", 2, 0, 100, 0, 0, 4, 7, 0, 0, 14, 0, 2, 0, 0, 0},
    {"IDIV", "\xf7\x3b", 2, 0, 0xFFFFFF9Cu, 0, 0xFFFFFFFFu, 4, 7, 0, 0, 0xFFFFFFF2u, 0, 0xFFFFFFFEu,
     0, 0, 0},
    {"IMUL r, r/m", "\x0f\xaf\x03", 3, 0, 6, 0, 0, 4, 7, 0, 0, 42, 0, 0, F_CF | F_OF, 0, 0},
    {"IMUL r, r/m, imm8", "\x6b\x03\x03", 3, 0, 0, 0, 0, 4, 0x40000000u, 0, 0, 0xC0000000u, 0, 0,
     F_CF | F_OF, F_CF | F_OF, 0},
    {"SHL r/m, CL", "\xd3\x23", 2, 0, 0, 4, 0, 4, 1, 4, 0x10, 0, 4, 0, F_CF | F_ZF | F_SF, 0, 0},
    {"SAR r/m, imm8", "\xc1\x3b\x01", 3, 0, 0, 0, 0, 4, 0x80000001u, 4, 0xC0000000u, 0, 0, 0,
     F_CF | F_ZF | F_SF, F_CF | F_SF, 0},
    {"ROL r/m8, 1", "\xd0\x03", 2, 0, 0, 0, 0, 1, 0x81, 1, 0x03, 0, 0, 0, F_CF | F_OF, F_CF | F_OF,
     0},
    {"RCR r/m, 1", "\xd1\x1b", 2, F_CF, 0, 0, 0, 4, 2, 4, 0x80000001u, 0, 0, 0, F_CF | F_OF, F_OF,
     0},
    {"CMPXCHG, equal", "\x0f\xb1\x0b", 3, 0, 5, 9, 0, 4, 5, 4, 9, 5, 9, 0, F_CF | F_ZF, F_ZF, 0},
    {"CMPXCHG, different", "\x0f\xb1\x0b", 3, 0, 5, 9, 0, 4, 6, 4, 6, 6, 9, 0, F_CF | F_ZF | F_SF,
     F_CF | F_SF, 0},
    {"XADD", "\x0f\xc1\x0b", 3, 0, 0, 2, 0, 4, 1, 4, 3, 0, 1, 0, F_CF | F_ZF | F_SF, 0, 0},
    {"BT r/m, r", "\x0f\xa3\x03", 3, 0, 3, 0, 0, 4, 8, 0, 0, 3, 0, 0, F_CF, F_CF, 0},
    {"BTR r/m, imm8", "\x0f\xba\x33\x03", 4, 0, 0, 0, 0, 4, 0xF, 4, 7, 0, 0, 0, F_CF, F_CF, 0},
    {"BSF", "\x0f\xbc\x03", 3, 0, 0, 0, 0, 4, 0x50, 0, 0, 4, 0, 0, F_ZF, 0, 0},
    {"BSR", "\x0f\xbd\x03", 3, 0, 0, 0, 0, 4, 0x50, 0, 0, 6, 0, 0, F_ZF, 0, 0},
    {"SHLD r/m, r, CL", "\x0f\xa5\x13", 3, 0, 0, 4, 0x80000000u, 4, 1, 4, 0x18, 0, 4, 0x80000000u,
     F_CF | F_ZF | F_SF, 0, 0},
    {"SHRD r/m, r, imm8", "\x0f\xac\x13\x04", 4, 0, 0, 0, 1, 4, 0x10, 4, 0x10000001u, 0, 0, 1,
     F_CF | F_ZF | F_SF, 0, 0},
    {"NEG r/m8", "\xf6\x1b", 2, 0, 0, 0, 0, 1, 1, 1, 0xFF, 0, 0, 0, F_CF | F_ZF | F_SF, F_CF | F_SF,
     0},
    {"NOT", "\xf7\x13", 2, 0, 0, 0, 0, 4, 0x0F0F0F0Fu, 4, 0xF0F0F0F0u, 0, 0, 0, 0, 0, 0},
    {"INC r/m16", "\x66\xff\x03", 3, 0, 0, 0, 0, 2, 0xFFFF, 2, 0, 0, 0, 0, F_CF | F_ZF | F_SF, F_ZF,
     0},
    {"ADC r/m, r", "\x11\x0b", 2, F_CF, 0, 2, 0, 4, 1, 4, 4, 0, 2, 0, F_CF | F_ZF | F_SF, 0, 0},
    {"SBB r, r/m", "\x1b\x03", 2, F_CF, 10, 0, 0, 4, 3, 0, 0, 6, 0, 0, F_CF | F_ZF | F_SF, 0, 0},
    {"XCHG", "\x87\x0b", 2, 0, 0, 0x22222222u, 0, 4, 0x11111111u, 4, 0x22222222u, 0, 0x11111111u, 0,
     0, 0, 0},
    {"TEST", "\x85\x0b", 2, 0, 0, 0x0F, 0, 4, 0xF0, 0, 0, 0, 0x0F, 0, F_CF | F_ZF | F_SF, F_ZF, 0},
    {"CMP r/m, r", "\x39\x0b", 2, 0, 0, 2, 0, 4, 1, 0, 0, 0, 2, 0, F_CF | F_ZF | F_SF, F_CF | F_SF,
     0},
    {"OR r/m8, imm8", "\x80\x0b\x0f", 3, 0, 0, 0, 0, 1, 0xF0, 1, 0xFF, 0, 0, 0, F_CF | F_ZF | F_SF,
     F_SF, 0},
    {"LOCK ADD", "\xf0\x01\x0b", 3, 0, 0, 1, 0, 4, 0xFFFFFFFFu, 4, 0, 0, 1, 0, F_CF | F_ZF | F_SF,
     F_CF | F_ZF, 0},
    {"MOVZX r, r/m8", "\x0f\xb6\x03", 3, 0, 0xFFFFFFFFu, 0, 0, 1, 0x80, 0, 0, 0x80, 0, 0, 0, 0, 0},
    {"MOVSX r, r/m16", "\x0f\xbf\x03", 3, 0, 0, 0, 0, 2, 0x8000, 0, 0, 0xFFFF8000u, 0, 0, 0, 0, 0},
    {"MOVBE r, m", "\x0f\x38\xf0\x03", 4, 0, 0, 0, 0, 4, 0x11223344u, 0, 0, 0x44332211u, 0, 0, 0, 0,
     0},
    {"MOVBE m, r", "\x0f\x38\xf1\x03", 4, 0, 0x11223344u, 0, 0, 0, 0, 4, 0x44332211u, 0x11223344u,
     0, 0, 0, 0, 0},
    {"SETE", "\x0f\x94\x03", 3, F_ZF, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0},
    {"SETNE", "\x0f\x95\x03", 3, F_ZF, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0},
    {"CMOVE, taken", "\x0f\x44\x03", 3, F_ZF, 9, 0, 0, 4, 7, 0, 0, 7, 0, 0, 0, 0, 0},
    {"CMOVNE, not taken", "\x0f\x45\x03", 3, F_ZF, 9, 0, 0, 4, 7, 0, 0, 9, 0, 0, 0, 0, 0},
    {"CMPXCHG8B", "\x0f\xc7\x0b", 3, 0, 2, 7, 1, 8, 0x0000000100000002ull, 8, 0x00000007D0000200ull,
     2, 7, 1, F_ZF, F_ZF, 0},
    {"MOV r/m, imm32", "\xc7\x03\x78\x56\x34\x12", 6, 0, 0, 0, 0, 0, 0, 4, 0x12345678u, 0, 0, 0, 0,
     0, 0},
    {"MOV EAX, moffs", "\xa1\x00\x02\x00\xd0", 5, 0, 0, 0, 0, 4, 0xCAFEF00Du, 0, 0, 0xCAFEF00Du, 0,
     0, 0, 0, 0},
    {"MOV r/m16, Sreg", "\x8c\x1b", 2, 0, 0, 0, 0, 0, 0, 2, 0x10, 0, 0, 0, 0, 0, 0},
    {"SMSW", "\x0f\x01\x23", 3, 0, 0, 0, 0, 0, 0, 2, 0x11, 0, 0, 0, 0, 0, 0},
    {"FNSTCW", "\xd9\x3b", 2, 0, 0, 0, 0, 0, 0, 2, 0x37F, 0, 0, 0, 0, 0, 0},
    {"JMP r/m", "\xff\x23", 2, 0, 0, 0, 0, 4, 0x5000, 0, 0, 0, 0, 0, 0, 0, 0x5000},
};

/** Report a failed check of the table's row `e`. */
static void erow_failed(const struct erow *e, const char *what)
{
    printf("kvm-vmx-hosttest: failed: %s on MMIO: %s\n", e->name, what);
    failures++;
}

/** Run the row `e` on the vCPU on `fd` up to its last exit to the caller:
 * its read, answered, and its write. Whether both were as the row says. */
static int erow_io(int fd, const struct erow *e)
{
    struct kvm_run *rp = run_of(fd);
    uint64_t got = 0;
    int ok = 1, runs;

    if (e->rlen) {
        ok = mmio_read(fd, 0xD0000200ull, e->rlen);
        k_memcpy(rp->u.mmio.data, &e->rval, 8);
    } else {
        script(EXIT_EPT_VIOLATION, 0x2, 0)->gpa = 0xD0000200ull;
    }
    if (!ok) {
        erow_failed(e, "its read");
    }
    if (!e->wlen) {
        return ok;
    }
    runs = m.launches + m.resumes;
    if (run(fd) != 0 || rp->exit_reason != KVM_EXIT_MMIO || !rp->u.mmio.is_write ||
        rp->u.mmio.phys_addr != 0xD0000200ull || rp->u.mmio.len != e->wlen) {
        erow_failed(e, "its write");
        return 0;
    }
    k_memcpy(&got, rp->u.mmio.data, e->wlen);
    if (got != e->wval || (e->rlen && m.launches + m.resumes != runs)) {
        erow_failed(e, "the value written, with no guest entered after the read");
        return 0;
    }
    return ok;
}

/** Run the row `e` on the vCPU on `fd` and check what it leaves. */
static void erow_run(int fd, const struct erow *e)
{
    struct kvm_regs r;

    emul_start(fd, e->bytes, e->n, e->rax, 0xD0000200ull, e->rcx);
    r = regs_of(fd);
    r.rdx = e->rdx;
    r.rflags = 2u | e->fl;
    CHECK(fdio(fd, KVM_SET_REGS, (uintptr_t)&r) == 0, "KVM_SET_REGS with EDX and the flags");
    if (!erow_io(fd, e)) {
        return;
    }
    script(EXIT_HLT, 0, 1);
    if (run(fd) != 0 || run_of(fd)->exit_reason != KVM_EXIT_HLT) {
        erow_failed(e, "the guest runs on after it");
        return;
    }
    r = regs_of(fd);
    if ((uint32_t)r.rax != (uint32_t)e->o_rax || (uint32_t)r.rcx != (uint32_t)e->o_rcx ||
        (uint32_t)r.rdx != (uint32_t)e->o_rdx) {
        erow_failed(e, "EAX, ECX and EDX");
    }
    if ((r.rflags & e->fmask) != e->fval) {
        erow_failed(e, "the flags");
    }
    if (r.rip != (e->rip ? e->rip : 0x1000u + e->n) + 1u) {
        erow_failed(e, "EIP");
    }
}

/** String instructions on device memory, each element one exit to the
 * caller: REPE CMPSB and REP MOVSD from the device, REPNE SCASB of it. */
static void string_rows(int fd)
{
    struct kvm_run *rp = run_of(fd);
    struct kvm_regs r;

    emul_start(fd, "\xf3\xa6", 2, 0, 0, 3);
    ram[0x6000] = 'A';
    ram[0x6001] = 'B';
    CHECK(mmio_read(fd, 0xD0000100ull, 1), "REPE CMPSB with [ESI] on MMIO reads its first byte");
    rp->u.mmio.data[0] = 'A';
    CHECK(mmio_read(fd, 0xD0000101ull, 1),
          "an equal byte leaves the instruction to run again, which reads the next");
    rp->u.mmio.data[0] = 'X';
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_HLT,
          "a byte that differs ends it with an element left");
    r = regs_of(fd);
    CHECK(r.rip == 0x1003 && r.rcx == 1 && r.rsi == 0xD0000102ull && r.rdi == 0x6002 &&
              !(r.rflags & (F_ZF | F_CF)),
          "with ECX, ESI, EDI and the flags of the last compare, and RIP past it");

    emul_start(fd, "\xf3\xa5", 2, 0, 0, 2);
    CHECK(mmio_read(fd, 0xD0000100ull, 4), "REP MOVSD from MMIO reads its first element");
    put32(0, 0x11111111u);
    k_memcpy(rp->u.mmio.data, &ram[0], 4);
    CHECK(mmio_read(fd, 0xD0000104ull, 4) && get32(0x6000) == 0x11111111u,
          "stores it and reads the second");
    put32(0, 0x22222222u);
    k_memcpy(rp->u.mmio.data, &ram[0], 4);
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && get32(0x6004) == 0x22222222u && regs_of(fd).rcx == 0 &&
              regs_of(fd).rdi == 0x6008 && regs_of(fd).rip == 0x1003,
          "and ends with ECX 0 and EDI past both");

    emul_start(fd, "\xf2\xae", 2, 'Q', 0, 5);
    r = regs_of(fd);
    r.rdi = 0xD0000300ull;
    CHECK(fdio(fd, KVM_SET_REGS, (uintptr_t)&r) == 0, "KVM_SET_REGS with EDI on MMIO");
    CHECK(mmio_read(fd, 0xD0000300ull, 1), "REPNE SCASB of MMIO reads its first byte");
    rp->u.mmio.data[0] = 'x';
    CHECK(mmio_read(fd, 0xD0000301ull, 1), "and the next after one that differs");
    rp->u.mmio.data[0] = 'Q';
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && regs_of(fd).rcx == 3 && regs_of(fd).rdi == 0xD0000302ull &&
              (regs_of(fd).rflags & F_ZF),
          "the byte that matches ends it with ZF set and ECX counting the two");
}

/** A bit test past its operand, and the stack with the operand on device
 * memory. */
static void stack_rows(int fd)
{
    struct kvm_run *rp = run_of(fd);
    int runs;

    emul_start(fd, "\x0f\xab\x03", 3, 35, 0xD0000200ull, 0);
    CHECK(mmio_read(fd, 0xD0000204ull, 4),
          "BTS [EBX], EAX with bit 35 reads the doubleword after the operand's");
    k_memcpy(rp->u.mmio.data, "\x09\x00\x00\x00", 4);
    runs = m.launches + m.resumes;
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_MMIO && rp->u.mmio.is_write &&
              rp->u.mmio.phys_addr == 0xD0000204ull && rp->u.mmio.len == 4 &&
              rp->u.mmio.data[0] == 9 && m.launches + m.resumes == runs,
          "and writes it back with bit 3 set, no guest entered between");
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && regs_of(fd).rip == 0x1004 && (regs_of(fd).rflags & F_CF),
          "CF is the bit as it was, set, and RIP is past the instruction");

    emul_start(fd, "\xff\x13", 2, 0, 0xD0000200ull, 0);
    CHECK(mmio_read(fd, 0xD0000200ull, 4), "CALL [EBX] reads its target on MMIO");
    put32(0, 0x4000u);
    k_memcpy(rp->u.mmio.data, &ram[0], 4);
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && regs_of(fd).rip == 0x4001 && regs_of(fd).rsp == 0x8FFC &&
              get32(0x8FFC) == 0x1002u,
          "and goes there with the return address pushed");

    emul_start(fd, "\xff\x33", 2, 0, 0xD0000200ull, 0);
    CHECK(mmio_read(fd, 0xD0000200ull, 4), "PUSH [EBX] reads its operand on MMIO");
    put32(0, 0xAABBCCDDu);
    k_memcpy(rp->u.mmio.data, &ram[0], 4);
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && regs_of(fd).rsp == 0x8FFC && get32(0x8FFC) == 0xAABBCCDDu,
          "and pushes it");
    emul_start(fd, "\x8f\x03", 2, 0, 0xD0000200ull, 0);
    put32(0x9000, 0x55667788u);
    script(EXIT_EPT_VIOLATION, 0x2, 0)->gpa = 0xD0000200ull;
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_MMIO && rp->u.mmio.is_write &&
              rp->u.mmio.len == 4 && rp->u.mmio.data[0] == 0x88 && rp->u.mmio.data[3] == 0x55 &&
              regs_of(fd).rsp == 0x9004,
          "POP [EBX] writes the stack's top to MMIO and releases it");
}

/** Far pointers and segment loads with their operand on device memory,
 * and a divide's #DE. */
static void far_rows(int fd)
{
    struct kvm_run *rp = run_of(fd);
    struct kvm_sregs s;

    emul_start(fd, "\xff\x2b", 2, 0, 0xD0000200ull, 0);
    CHECK(mmio_read(fd, 0xD0000200ull, 6), "JMP FAR [EBX] reads its six bytes on MMIO");
    k_memcpy(rp->u.mmio.data, "\x00\x40\x00\x00\x48\x00", 6);
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_HLT, "and the guest runs on");
    s = sregs_of(fd);
    CHECK(regs_of(fd).rip == 0x4001 && s.cs.selector == 0x48 && s.cs.limit == 0xFFFF && !s.cs.db,
          "in the 16-bit code segment the pointer names, at its offset");

    ts_tables();
    emul_start(fd, "\x0f\x00\x1b", 3, 0, 0xD0000200ull, 0);
    CHECK(mmio_read(fd, 0xD0000200ull, 2), "LTR [EBX] reads its selector on MMIO");
    k_memcpy(rp->u.mmio.data, "\x20\x00", 2);
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && sregs_of(fd).tr.selector == 0x20 && sregs_of(fd).tr.base == TS_TSSB &&
              ram[TS_GDT + 0x25u] == 0x8B,
          "loads the task register and marks the TSS busy in the GDT");
    ts_tables();

    emul_start(fd, "\x0f\xb2\x03", 3, 0, 0xD0000200ull, 0);
    CHECK(mmio_read(fd, 0xD0000200ull, 6), "LSS EAX, [EBX] reads its six bytes on MMIO");
    k_memcpy(rp->u.mmio.data, "\x78\x56\x34\x12\x50\x00", 6);
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && (uint32_t)regs_of(fd).rax == 0x12345678u &&
              sregs_of(fd).ss.selector == 0x50 && sregs_of(fd).ss.limit == 0xFFFF,
          "and loads EAX and SS from them");

    emul_start(fd, "\xf7\x33", 2, 1, 0xD0000200ull, 0);
    CHECK(mmio_read(fd, 0xD0000200ull, 4), "DIV [EBX] reads its divisor on MMIO");
    k_memset(rp->u.mmio.data, 0, 4);
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && m.injected == ((TYPE_HW_EXC << 8) | INTR_VALID) &&
              regs_of(fd).rip == 0x1001 && (uint32_t)regs_of(fd).rax == 1,
          "a divisor of zero is #DE at the instruction, with the registers as they were");
}

/** Put the vCPU on `fd` in 64-bit mode at CPL 0 at the instruction
 * `bytes` of `n` bytes, as `emul_start` puts it in protected mode, with
 * RBX at the device's 0xD0000200, through identity tables at 0x10000 that
 * map RAM and the device's 2 MiB with large pages. */
static void long_start(int fd, const char *bytes, uint32_t n)
{
    struct kvm_sregs s;
    uint64_t e;

    emul_start(fd, bytes, n, 0, 0xD0000200ull, 0);
    k_memset(&ram[0x10000], 0, 0x4000);
    e = 0x11003;
    k_memcpy(&ram[0x10000], &e, 8);
    e = 0x12003;
    k_memcpy(&ram[0x11000], &e, 8);
    e = 0x13003;
    k_memcpy(&ram[0x11000 + 3 * 8], &e, 8);
    e = 0x83;
    k_memcpy(&ram[0x12000], &e, 8);
    e = 0xD0000083ull;
    k_memcpy(&ram[0x13000 + 128 * 8], &e, 8);
    s = sregs_of(fd);
    s.cr0 = 0x80000011u;
    s.cr3 = 0x10000;
    s.cr4 = CR4_PAE;
    s.efer = GEFER_LME | GEFER_LMA;
    s.cs.l = 1;
    s.cs.db = 0;
    CHECK(fdio(fd, KVM_SET_SREGS, (uintptr_t)&s) == 0, "KVM_SET_SREGS to 64-bit mode");
}

/** MOV SS from the device with the null selector in 64-bit mode at CPL 0
 * (`long_start`): SS is Linux's flat data segment, which the next entry
 * takes. */
static void null_ss_rows(int fd)
{
    struct kvm_run *rp = run_of(fd);
    struct kvm_sregs s;

    long_start(fd, "\x8e\x13\xf4", 3);
    CHECK(mmio_read(fd, 0xD0000200ull, 2), "MOV SS, [RBX] reads its selector on MMIO");
    k_memset(rp->u.mmio.data, 0, 2);
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_HLT, "and the entry after it passes");
    s = sregs_of(fd);
    CHECK(s.ss.selector == 0 && s.ss.present && !s.ss.unusable && s.ss.type == 3 && s.ss.s &&
              s.ss.dpl == 0 && s.ss.db && s.ss.g && s.ss.limit == 0xFFFu,
          "SS is a flat data segment of DPL 0 with the limit 0xFFF under G");
}

/** Instructions Linux's emulator runs on device memory that the first
 * decoder refused. */
static void emul_rows(int fd)
{
    uint32_t i;

    for (i = 0; i < sizeof(erows) / sizeof(erows[0]); i++) {
        erow_run(fd, &erows[i]);
    }
    string_rows(fd);
    stack_rows(fd);
    far_rows(fd);
    null_ss_rows(fd);
}

/** The #UD exit `fd`'s next run ends with, then the runs `after` gives
 * and a HLT: KVM_RUN's result. */
static int32_t ud_run(int fd, uint32_t after, uint32_t len)
{
    struct row *w = script(EXIT_EXCEPTION_NMI, 0, 0);

    w->intr_info = UD_VECTOR | (TYPE_HW_EXC << 8) | INTR_VALID;
    if (after) {
        script(after, 0, len);
    }
    script(EXIT_HLT, 0, 1);
    return run(fd);
}

/** #UD exits: the instructions Linux's emulator runs after one are
 * carried out, the other vendor's hypercall is rewritten, and any other
 * instruction's #UD goes back to the guest. */
static void ud_rows(int fd)
{
    struct {
        struct kvm_msrs h;
        struct kvm_msr_entry e[3];
    } msrs;
    uint32_t ud = UD_VECTOR | (TYPE_HW_EXC << 8) | INTR_VALID;
    struct kvm_sregs s;

    emul_start(fd, "\x0f\x0b", 2, 0, 0, 0);
    CHECK(ud_run(fd, 0, 0) == 0 && run_of(fd)->exit_reason == KVM_EXIT_HLT && m.injected == ud &&
              regs_of(fd).rip == 0x1001,
          "UD2's #UD goes back to the guest at CPL 0, with no internal error");

    k_memset(&msrs, 0, sizeof(msrs));
    msrs.h.nmsrs = 3;
    msrs.e[0].index = MSR_IA32_SYSENTER_CS;
    msrs.e[0].data = 0x08;
    msrs.e[1].index = MSR_IA32_SYSENTER_ESP;
    msrs.e[1].data = 0x8000;
    msrs.e[2].index = MSR_IA32_SYSENTER_EIP;
    msrs.e[2].data = 0x4000;
    CHECK(fdio(fd, KVM_SET_MSRS, (uintptr_t)&msrs) == 3, "the SYSENTER MSRs set");
    emul_start(fd, "\x0f\x34", 2, 0, 0, 0);
    CHECK(ud_run(fd, 0, 0) == 0 && run_of(fd)->exit_reason == KVM_EXIT_HLT && m.injected == 0,
          "SYSENTER that raised #UD is carried out, and the entry's checks pass its segments");
    s = sregs_of(fd);
    CHECK(regs_of(fd).rip == 0x4001 && regs_of(fd).rsp == 0x8000 && s.cs.selector == 0x08 &&
              s.cs.base == 0 && s.cs.limit == 0xFFFFFFFFu && s.cs.db && !s.cs.l &&
              s.ss.selector == 0x10 && s.ss.dpl == 0,
          "to the EIP, ESP and flat segments the SYSENTER MSRs name");

    emul_start(fd, "\x0f\x05", 2, 0, 0, 0);
    s = sregs_of(fd);
    s.efer = GEFER_SCE;
    CHECK(fdio(fd, KVM_SET_SREGS, (uintptr_t)&s) == 0, "KVM_SET_SREGS with EFER.SCE");
    CHECK(ud_run(fd, 0, 0) == 0 && m.injected == ud && regs_of(fd).rip == 0x1001,
          "SYSCALL outside 64-bit mode is #UD for a guest whose vendor is Intel's, SCE set");

    emul_start(fd, "\xf3\x0f\xc7\xf9", 4, 0, 0, 7);
    CHECK(ud_run(fd, 0, 0) == 0 && m.injected == ud && regs_of(fd).rcx == 7,
          "RDPID is #UD for a guest whose CPUID lacks it");

    emul_start(fd, "\x0f\x01\xd9", 3, 0, 0, 0);
    CHECK(ud_run(fd, EXIT_VMCALL, 3) == 0 && ram[0x1000] == 0x0F && ram[0x1001] == 0x01 &&
              ram[0x1002] == 0xC1 && m.injected == 0,
          "VMMCALL that raised #UD is rewritten as VMCALL in the guest's memory");
    CHECK(regs_of(fd).rip == 0x1004 && (uint32_t)regs_of(fd).rax == (uint32_t)-1000,
          "which then runs at the same RIP and is answered");
}

/** MMX and SSE moves with their memory operand on device memory. */
static void simd_rows(int fd)
{
    struct kvm_run *rp = run_of(fd);
    struct kvm_vcpu *v = vcpu_of(fd);
    struct kvm_sregs s;
    int runs, syncs = m.fpu_syncs;

    emul_start(fd, "\x0f\x6f\x03", 3, 0, 0xD0000200ull, 0);
    CHECK(mmio_read(fd, 0xD0000200ull, 8) && v->xsave[4] == 0,
          "MOVQ MM0, [EBX] reads eight bytes on MMIO, the x87 tags as they were until it ends");
    k_memcpy(rp->u.mmio.data, "\x01\x02\x03\x04\x05\x06\x07\x08", 8);
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && memcmp(v->xsave + 32, "\x01\x02\x03\x04\x05\x06\x07\x08", 8) == 0 &&
              v->xsave[40] == 0xFF && v->xsave[41] == 0xFF && v->xsave[4] == 0xFF,
          "into the x87 register MM0 names, with its exponent all ones and every tag valid");
    emul_start(fd, "\x0f\x7f\x03", 3, 0, 0xD0000200ull, 0);
    script(EXIT_EPT_VIOLATION, 0x2, 0)->gpa = 0xD0000200ull;
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_MMIO && rp->u.mmio.is_write &&
              rp->u.mmio.len == 8 &&
              memcmp(rp->u.mmio.data, "\x01\x02\x03\x04\x05\x06\x07\x08", 8) == 0,
          "MOVQ [EBX], MM0 writes them back");

    emul_start(fd, "\xf3\x0f\x6f\x0b", 4, 0, 0xD0000200ull, 0);
    script(EXIT_EPT_VIOLATION, 0x1, 0)->gpa = 0xD0000200ull;
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_HLT && regs_of(fd).rip == 0x1001 &&
              m.injected == (UD_VECTOR | (TYPE_HW_EXC << 8) | INTR_VALID),
          "MOVDQU on MMIO without CR4.OSFXSR is #UD, with no read for the caller");
    emul_start(fd, "\xf3\x0f\x6f\x0b", 4, 0, 0xD0000200ull, 0);
    s = sregs_of(fd);
    s.cr4 = 0x200;
    CHECK(fdio(fd, KVM_SET_SREGS, (uintptr_t)&s) == 0, "KVM_SET_SREGS with CR4.OSFXSR");
    CHECK(mmio_read(fd, 0xD0000200ull, 8), "MOVDQU XMM1, [EBX] reads its low half on MMIO");
    k_memcpy(rp->u.mmio.data, "ABCDEFGH", 8);
    runs = m.launches + m.resumes;
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_MMIO && !rp->u.mmio.is_write &&
              rp->u.mmio.phys_addr == 0xD0000208ull && rp->u.mmio.len == 8 &&
              m.launches + m.resumes == runs,
          "then its high half, no guest entered between");
    k_memcpy(rp->u.mmio.data, "IJKLMNOP", 8);
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && memcmp(v->xsave + 176, "ABCDEFGHIJKLMNOP", 16) == 0,
          "into XMM1 of the vCPU's image");
    CHECK(m.fpu_syncs > syncs,
          "the emulator reads the image only after the guest's registers are saved into it");
}

/** The guest's own #DB and #AC, which always exit: each is given back,
 * #DB with its bits in the guest's DR6 and DR7.GD cleared. */
static void db_rows(int fd)
{
    uint32_t db = DB_VECTOR | (TYPE_HW_EXC << 8) | INTR_VALID;
    uint32_t ac = AC_VECTOR | (TYPE_HW_EXC << 8) | INTR_ERRCODE | INTR_VALID;
    struct kvm_debugregs d;
    struct kvm_regs r;
    uint64_t *f = fields(fd);
    struct row *w;

    ts_start(fd);
    k_memset(&d, 0, sizeof(d));
    d.db[0] = 0x4321;
    d.dr6 = 0xFFFF0FF0u;
    d.dr7 = 0x2557;
    CHECK(fdio(fd, KVM_SET_DEBUGREGS, (uintptr_t)&d) == 0, "KVM_SET_DEBUGREGS with DR7.GD");
    w = script(EXIT_EXCEPTION_NMI, 0x4001, 0);
    w->intr_info = db;
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && m.injected == db && f[EXCEPTION_BITMAP] == EXC_LINUX &&
              m.entry_dr0 == 0x4321,
          "the guest's #DB, from a breakpoint that ran in DR0, exits and is injected again");
    CHECK(fdio(fd, KVM_GET_DEBUGREGS, (uintptr_t)&d) == 0 && d.dr6 == 0xFFFF4FF1u &&
              d.dr7 == 0x557 && f[GUEST_DR7] == 0x557,
          "with the qualification's BS and B0 in the guest's DR6 and DR7.GD cleared");

    r = regs_of(fd);
    r.rip = 0x1000;
    r.rflags = 0x302;
    CHECK(fdio(fd, KVM_SET_REGS, (uintptr_t)&r) == 0, "KVM_SET_REGS with TF and IF");
    w = script(EXIT_EXCEPTION_NMI, 0x4000, 0);
    w->intr_info = db;
    w->intr_state = INTR_STI;
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && run_of(fd)->exit_reason == KVM_EXIT_HLT && m.injected == db &&
              (f[GUEST_PENDING_DBG_EXCEPTIONS] & DR6_BS),
          "a single step after STI is injected with the single-step trap pending, which the "
          "entry's check under the STI shadow asks for");

    ts_start(fd);
    w = script(EXIT_EXCEPTION_NMI, 0, 1);
    w->intr_info = DB_VECTOR | (TYPE_PRIV_SOFT << 8) | INTR_VALID;
    w->intr_state = INTR_MOV_SS;
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && m.injected == db && m.entry_intr == 0 && regs_of(fd).rip == 0x1002,
          "INT1 is stepped past, ends the shadow it ran under and is injected as #DB");
    CHECK(fdio(fd, KVM_GET_DEBUGREGS, (uintptr_t)&d) == 0 && d.dr6 == 0xFFFF0FF0u,
          "with no bit of DR6 set");

    w = script(EXIT_EXCEPTION_NMI, 0, 0);
    w->intr_info = ac;
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && m.injected == ac && m.injected_error == 0,
          "the guest's #AC exits and is injected again with its error code");
    CHECK(m.dr6_writes == 0,
          "the processor's DR6 is never written, over every row: the guest's is the record's");
}

/** A vCPU of id `id` on `vm` with the supported CPUID but for leaf 0's
 * vendor, AMD's. */
static int amd_vcpu(int vm, uint32_t id)
{
    struct kvm_cpuid_entry2 *e = (struct kvm_cpuid_entry2 *)(buf.bytes + sizeof(buf.cpuid));
    int fd = fdio(vm, KVM_CREATE_VCPU, id);
    uint32_t i;

    CHECK(fd > vm, "KVM_CREATE_VCPU");
    buf.cpuid.nent = 256;
    CHECK(dev(KVM_GET_SUPPORTED_CPUID, (uintptr_t)&buf) == 0, "KVM_GET_SUPPORTED_CPUID");
    for (i = 0; i < buf.cpuid.nent; i++) {
        if (e[i].function == 0) {
            e[i].ebx = 0x68747541u; /* "AuthenticAMD" */
            e[i].edx = 0x69746E65u;
            e[i].ecx = 0x444D4163u;
        }
    }
    CHECK(fdio(fd, KVM_SET_CPUID2, (uintptr_t)&buf) == 0, "KVM_SET_CPUID2 with AMD's vendor");
    return fd;
}

/* RCX with its low half 0 and RSI and RDI with their upper halves set, for
 * a REP string instruction at address size 4 that moves nothing. */
#define NONE_RCX 0xFFFFFFFF00000000ull
#define NONE_RSI 0x1111111100002000ull
#define NONE_RDI 0x2222222200003000ull

/** Run the REP string instruction `bytes` of 3 bytes in 64-bit mode on
 * `fd` with RCX, RSI and RDI at NONE_RCX, NONE_RSI and NONE_RDI, through
 * an EPT violation on the device, or a string IN's exit at address size 4
 * when `in`, then a HLT; 1 when the HLT is reached past it. */
static int none_run(int fd, const char *bytes, int in)
{
    struct kvm_regs r;

    long_start(fd, bytes, 3);
    r = regs_of(fd);
    r.rcx = NONE_RCX;
    r.rsi = NONE_RSI;
    r.rdi = NONE_RDI;
    CHECK(fdio(fd, KVM_SET_REGS, (uintptr_t)&r) == 0, "KVM_SET_REGS of RCX, RSI and RDI");
    if (in) {
        script(EXIT_IO, 0x38u | (0x80ull << 16), 3)->insn_info = 1u << 7;
    } else {
        script(EXIT_EPT_VIOLATION, 0x2, 0)->gpa = 0xD0000200ull;
    }
    script(EXIT_HLT, 0, 1);
    return run(fd) == 0 && run_of(fd)->exit_reason == KVM_EXIT_HLT && regs_of(fd).rip == 0x1004;
}

/** REP MOVSB, STOSB and INSB at address size 4 in 64-bit mode with ECX 0,
 * on `fd`, whose vendor is Intel's when `intel`: each moves nothing, and
 * for Intel's vendor RCX ends 0 whole, with RSI's and RDI's upper halves
 * cleared for MOVSB and RDI's for STOSB, as Linux's
 * string_registers_quirk; for another vendor every register stays. */
static void none_rows(int fd, int intel)
{
    struct kvm_regs r;

    CHECK(none_run(fd, "\xf3\x67\xa4", 0), "REP MOVSB with ECX 0 moves nothing, stepped");
    r = regs_of(fd);
    CHECK(intel ? r.rcx == 0 && r.rsi == (uint32_t)NONE_RSI && r.rdi == (uint32_t)NONE_RDI
                : r.rcx == NONE_RCX && r.rsi == NONE_RSI && r.rdi == NONE_RDI,
          intel ? "and for Intel's vendor clears RCX and RSI's and RDI's upper halves"
                : "and for AMD's vendor leaves RCX, RSI and RDI");
    CHECK(none_run(fd, "\xf3\x67\xaa", 0), "REP STOSB with ECX 0 moves nothing, stepped");
    r = regs_of(fd);
    CHECK(intel ? r.rcx == 0 && r.rsi == NONE_RSI && r.rdi == (uint32_t)NONE_RDI
                : r.rcx == NONE_RCX && r.rsi == NONE_RSI && r.rdi == NONE_RDI,
          intel ? "and for Intel's vendor clears RCX and RDI's upper half"
                : "and for AMD's vendor leaves RCX, RSI and RDI");
    CHECK(none_run(fd, "\xf3\x67\x6c", 1), "REP INSB with ECX 0 moves nothing, stepped");
    r = regs_of(fd);
    CHECK(intel ? r.rcx == 0 && r.rsi == NONE_RSI && r.rdi == NONE_RDI
                : r.rcx == NONE_RCX && r.rsi == NONE_RSI && r.rdi == NONE_RDI,
          intel ? "and for Intel's vendor clears RCX alone"
                : "and for AMD's vendor leaves RCX, RSI and RDI");
}

/** The REP string instructions with ECX 0 at address size 4 (`none_rows`)
 * on two vCPUs of their own on `vm`, Intel's vendor and AMD's. */
static void string_none_rows(int vm)
{
    int fd = make_vcpu(vm, 7);

    none_rows(fd, 1);
    fdclose(fd);
    fd = amd_vcpu(vm, 8);
    none_rows(fd, 0);
    fdclose(fd);
}

/** SYSCALL outside 64-bit mode after its #UD, which Linux's emulator runs
 * for a guest whose CPUID vendor is AMD's, on a vCPU of its own on `vm`,
 * since a vCPU's CPUID is fixed once it has run. */
static void amd_syscall_rows(int vm)
{
    struct {
        struct kvm_msrs h;
        struct kvm_msr_entry e[1];
    } msrs;
    struct kvm_sregs s;
    int fd = amd_vcpu(vm, 6);

    msrs.h.nmsrs = 1;
    msrs.h.pad = 0;
    k_memset(msrs.e, 0, sizeof(msrs.e));
    msrs.e[0].index = MSR_STAR;
    msrs.e[0].data = 0x0000000800004000ull;
    CHECK(fdio(fd, KVM_SET_MSRS, (uintptr_t)&msrs) == 1, "STAR set");
    emul_start(fd, "\x0f\x05", 2, 0, 0, 0);
    s = sregs_of(fd);
    s.efer = GEFER_SCE;
    CHECK(fdio(fd, KVM_SET_SREGS, (uintptr_t)&s) == 0, "KVM_SET_SREGS with EFER.SCE");
    CHECK(ud_run(fd, 0, 0) == 0 && run_of(fd)->exit_reason == KVM_EXIT_HLT && m.injected == 0,
          "SYSCALL outside 64-bit mode that raised #UD runs for a guest whose vendor is AMD's");
    s = sregs_of(fd);
    CHECK(regs_of(fd).rip == 0x4001 && (uint32_t)regs_of(fd).rcx == 0x1002u &&
              s.cs.selector == 0x08 && s.ss.selector == 0x10,
          "to STAR's EIP and selectors, with ECX the next instruction's address");
    fdclose(fd);
}

/** A hardware task switch from each of its sources, on a vCPU of its own
 * on `vm`; then, in the same protected mode, the instructions the
 * emulator runs on device memory and after #UD, and the guest's own #DB
 * and #AC; and SYSCALL under AMD's vendor on another vCPU. */
static void ts_rows(int vm)
{
    int fd = make_vcpu(vm, 2);

    ts_tables();
    ts_start(fd);
    ts_call_rows(fd);
    ts_jmp_rows(fd);
    ts_gate_rows(fd);
    ts_fault_rows(fd);
    emul_rows(fd);
    ud_rows(fd);
    simd_rows(fd);
    db_rows(fd);
    fdclose(fd);
    amd_syscall_rows(vm);
    string_none_rows(vm);
}

/** The caller's single step over a Hyper-V hypercall the caller answers,
 * on the vCPU `fd` whose guest named itself: an extended query, 0x8001,
 * from 32-bit protected mode at CPL 0 is KVM_EXIT_HYPERV, and the KVM_RUN
 * that takes the answer is KVM_EXIT_DEBUG past the VMCALL with no guest
 * entered, as Linux's kvm_hv_hypercall_complete_userspace steps it. Real
 * mode and the debug registers are put back after. */
static void hv_step_rows(int fd)
{
    struct kvm_run *rp = run_of(fd);
    struct kvm_sregs s = sregs_of(fd);
    struct kvm_debugregs d;
    int runs;

    CHECK(fdio(fd, KVM_GET_DEBUGREGS, (uintptr_t)&d) == 0, "KVM_GET_DEBUGREGS");
    ts_start(fd);
    guest_debug(fd, KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_SINGLESTEP, 0, 0);
    set_regs(fd, 0x1000, 0x8001u, 0, 0);
    script(EXIT_VMCALL, 0, 3);
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_HYPERV &&
              rp->u.hyperv.type == KVM_EXIT_HYPERV_HCALL && rp->u.hyperv.u.hcall.input == 0x8001u &&
              regs_of(fd).rip == 0x1000,
          "an extended hypercall under the caller's single step is the caller's");
    rp->u.hyperv.u.hcall.result = 0;
    runs = m.launches + m.resumes;
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_DEBUG && (rp->u.debug.dr6 & 0x4000u) &&
              rp->u.debug.pc == 0x1003 && regs_of(fd).rip == 0x1003 && regs_of(fd).rax == 0 &&
              m.launches + m.resumes == runs,
          "and its answer's KVM_RUN is KVM_EXIT_DEBUG past the VMCALL, with no guest entered");
    guest_debug(fd, 0, 0, 0);
    CHECK(fdio(fd, KVM_SET_SREGS, (uintptr_t)&s) == 0 &&
              fdio(fd, KVM_SET_DEBUGREGS, (uintptr_t)&d) == 0,
          "KVM_SET_SREGS and KVM_SET_DEBUGREGS back to real mode's");
}

/** A VMCALL goes to Hyper-V's hypercalls once the CPUID names Hyper-V and
 * the guest has written its id, on a vCPU of its own on `vm`. */
static void hv_rows(int vm)
{
    struct {
        struct kvm_msrs h;
        struct kvm_msr_entry e[1];
    } msrs;
    struct kvm_cpuid_entry2 *e = (struct kvm_cpuid_entry2 *)(buf.bytes + sizeof(buf.cpuid));
    struct {
        struct kvm_cpuid2 h;
        struct kvm_cpuid_entry2 e[10];
    } hv;
    uint32_t ud = UD_VECTOR | (TYPE_HW_EXC << 8) | INTR_VALID;
    struct kvm_sregs s;
    int fd = fdio(vm, KVM_CREATE_VCPU, 3);
    uint32_t i, n = 0;

    CHECK(fd > vm, "KVM_CREATE_VCPU");
    buf.cpuid.nent = 256;
    hv.h.nent = 10;
    CHECK(dev(KVM_GET_SUPPORTED_CPUID, (uintptr_t)&buf) == 0 &&
              dev(KVM_GET_SUPPORTED_HV_CPUID, (uintptr_t)&hv) == 0 && hv.h.nent == 10,
          "the supported CPUID and Hyper-V's leaves");
    /* Hyper-V's leaves take the place of KVM's own at 0x40000000, as a
     * caller that names Hyper-V gives them. */
    for (i = 0; i < buf.cpuid.nent; i++) {
        if ((e[i].function & 0xFFFFFF00u) != 0x40000000u) {
            e[n++] = e[i];
        }
    }
    buf.cpuid.nent = n;
    for (i = 0; i < n; i++) {
        if (e[i].function == 7 && e[i].index == 0) {
            e[i].ecx |= 1u << 22;
        }
    }
    k_memcpy(e + buf.cpuid.nent, hv.e, sizeof(hv.e));
    buf.cpuid.nent += 10;
    CHECK(fdio(fd, KVM_SET_CPUID2, (uintptr_t)&buf) == 0, "KVM_SET_CPUID2 with Hyper-V's leaves");
    s = sregs_of(fd);
    s.cs.base = 0;
    s.cs.selector = 0;
    CHECK(fdio(fd, KVM_SET_SREGS, (uintptr_t)&s) == 0, "KVM_SET_SREGS");
    set_regs(fd, 0x1000, 0, 0, 0);
    CHECK(exits(fd, EXIT_VMCALL, 0, 3) == 0 && (uint32_t)regs_of(fd).rax == (uint32_t)-1000,
          "VMCALL is KVM's before the guest names itself");
    msrs.h.nmsrs = 1;
    msrs.h.pad = 0;
    k_memset(msrs.e, 0, sizeof(msrs.e));
    msrs.e[0].index = 0x40000000u;
    msrs.e[0].data = 0x8100000000000000ull;
    CHECK(fdio(fd, KVM_SET_MSRS, (uintptr_t)&msrs) == 1, "the guest's id set");
    set_regs(fd, 0x1000, 0, 0, 0);
    CHECK(exits(fd, EXIT_VMCALL, 0, 3) == 0 && m.injected == ud && regs_of(fd).rip == 0x1001,
          "VMCALL is Hyper-V's, #UD in real mode and not stepped, before the HLT");
    hv_step_rows(fd);
    msrs.e[0].index = MSR_TSC_AUX;
    msrs.e[0].data = 0x1234;
    CHECK(fdio(fd, KVM_SET_MSRS, (uintptr_t)&msrs) == 1, "IA32_TSC_AUX set");
    code(fd, 0x1000, "\xf3\x0f\xc7\xf9", 4, 0);
    CHECK(ud_run(fd, 0, 0) == 0 && m.injected == 0 && regs_of(fd).rcx == 0x1234 &&
              regs_of(fd).rip == 0x1005,
          "RDPID that raised #UD reads IA32_TSC_AUX for a guest whose CPUID has it");
    fdclose(fd);
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

/** The hypercall instruction of the other vendor and MWAIT, with their
 * quirks on and then off, on the vCPU `fd` in protected mode. */
static void quirk_rows(int vm, int fd)
{
    uint32_t ud = UD_VECTOR | (TYPE_HW_EXC << 8) | INTR_VALID;
    struct row *w;

    code(fd, 0x1000, "\x0f\x01\xd9", 3, 0);
    w = script(EXIT_EXCEPTION_NMI, 0, 0);
    w->intr_info = ud;
    script(EXIT_VMCALL, 0, 3);
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && m.injected == 0 && ram[0x1002] == 0xC1 && regs_of(fd).rip == 0x1004 &&
              (uint32_t)regs_of(fd).rax == (uint32_t)-1000,
          "AMD's VMMCALL is #UD on Intel: KVM writes VMCALL in its place and the guest runs it");
    CHECK(fields(fd)[EXCEPTION_BITMAP] & (1u << UD_VECTOR), "#UD always exits");
    code(fd, 0x1000, "\x0f\x0b", 2, 0);
    w = script(EXIT_EXCEPTION_NMI, 0, 0);
    w->intr_info = ud;
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && m.injected == ud && ram[0x1000] == 0x0F && ram[0x1001] == 0x0B,
          "the #UD of another instruction goes back to the guest");
    set_regs(fd, 0x1000, 0, 0, 0);
    CHECK(exits(fd, EXIT_MWAIT, 0, 3) == 0 && m.injected == 0 && regs_of(fd).rip == 0x1004,
          "MWAIT is a no-op for a guest whose CPUID lacks it");

    CHECK(vm_cap(vm, KVM_CAP_DISABLE_QUIRKS2,
                 KVM_X86_QUIRK_FIX_HYPERCALL_INSN | KVM_X86_QUIRK_MWAIT_NEVER_UD_FAULTS) == 0,
          "KVM_CAP_DISABLE_QUIRKS2 of the hypercall's and MWAIT's quirks");
    code(fd, 0x1000, "\x0f\x01\xd9", 3, 0);
    w = script(EXIT_EXCEPTION_NMI, 0, 0);
    w->intr_info = ud;
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && m.injected == ud && ram[0x1002] == 0xD9 && regs_of(fd).rip == 0x1001,
          "with its quirk off VMMCALL stays, and is the guest's #UD");
    set_regs(fd, 0x1000, 0, 0, 0);
    CHECK(exits(fd, EXIT_MWAIT, 0, 3) == 0 && m.injected == ud && regs_of(fd).rip == 0x1001,
          "and with its quirk off MWAIT is #UD for that guest");
}

/** A plain OUT's RIP on the vCPU `fd`: at the instruction when the caller
 * sees the exit and stepped at the next KVM_RUN, as Linux's
 * complete_fast_pio_out steps it, unless the caller moved it meanwhile;
 * the caller's single step over an OUT and an IN, reported once each has
 * ended; and port 0x7E's OUT, stepped before the exit until its quirk is
 * off. */
static void out_rows(int vm, int fd)
{
    struct kvm_run *rp = run_of(fd);

    set_regs(fd, 0x1000, 0x41, 0, 0);
    script(EXIT_IO, 0x3F8ull << 16, 1);
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_IO && regs_of(fd).rip == 0x1000,
          "the caller sees an OUT with RIP at it");
    set_regs(fd, 0x1800, 0, 0, 0);
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && regs_of(fd).rip == 0x1801, "a caller that moved RIP keeps its RIP");
    guest_debug(fd, KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_SINGLESTEP, 0, 0);
    set_regs(fd, 0x1000, 0x41, 0, 0);
    script(EXIT_IO, 0x3F8ull << 16, 1);
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_IO, "an OUT under the caller's single step");
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_DEBUG && regs_of(fd).rip == 0x1001 &&
              (rp->u.debug.dr6 & 0x4000u),
          "is KVM_EXIT_DEBUG at the next KVM_RUN, past it, with no guest entered");
    set_regs(fd, 0x1000, 0, 0, 0);
    script(EXIT_IO, 8u | (0x60ull << 16), 1);
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_IO, "an IN under the caller's single step");
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_DEBUG && regs_of(fd).rip == 0x1001,
          "is KVM_EXIT_DEBUG once its answer is in");
    guest_debug(fd, 0, 0, 0);
    set_regs(fd, 0x1000, 0x41, 0, 0);
    script(EXIT_IO, 0x7Eull << 16, 1);
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_IO && rp->u.io.port == 0x7E &&
              regs_of(fd).rip == 0x1001,
          "an OUT to port 0x7E is stepped before the caller sees it");
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && regs_of(fd).rip == 0x1002, "and not again at the next KVM_RUN");
    CHECK(vm_cap(vm, KVM_CAP_DISABLE_QUIRKS2, KVM_X86_QUIRK_OUT_7E_INC_RIP) == 0,
          "KVM_CAP_DISABLE_QUIRKS2 of port 0x7E's quirk");
    set_regs(fd, 0x1000, 0x41, 0, 0);
    script(EXIT_IO, 0x7Eull << 16, 1);
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_IO && regs_of(fd).rip == 0x1000,
          "with its quirk off port 0x7E is as any other");
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && regs_of(fd).rip == 0x1002, "stepped at the next KVM_RUN");
}

/** MSR_PLATFORM_INFO and CPUID faulting on the vCPU `fd`. */
static void platform_rows(int vm, int fd)
{
    uint32_t gpe = GP_VECTOR | (TYPE_HW_EXC << 8) | INTR_ERRCODE | INTR_VALID;
    struct kvm_vcpu *v = vcpu_of(fd);
    struct {
        struct kvm_msrs h;
        struct kvm_msr_entry e[1];
    } msrs;

    k_memset(&msrs, 0, sizeof(msrs));
    msrs.h.nmsrs = 1;
    msrs.e[0].index = 0xCE;
    msrs.e[0].data = 0x80001100u;
    CHECK(fdio(fd, KVM_SET_MSRS, (uintptr_t)&msrs) == 1, "the caller writes MSR_PLATFORM_INFO");
    set_regs(fd, 0x1000, 0, 0xCE, 0);
    CHECK(exits(fd, EXIT_MSR_READ, 0, 2) == 0 && m.injected == 0 && regs_of(fd).rax == 0x80001100u,
          "the guest reads MSR_PLATFORM_INFO as the caller wrote it");
    set_regs(fd, 0x1000, 0, 0xCE, 0);
    CHECK(exits(fd, EXIT_MSR_WRITE, 0, 2) == 0 && m.injected == gpe, "its write is #GP");
    CHECK(vm_cap(vm, KVM_CAP_MSR_PLATFORM_INFO, 0) == 0, "KVM_CAP_MSR_PLATFORM_INFO with 0");
    set_regs(fd, 0x1000, 0, 0xCE, 0);
    CHECK(exits(fd, EXIT_MSR_READ, 0, 2) == 0 && m.injected == gpe, "and then its read is #GP");

    set_regs(fd, 0x1000, 1, 0x140, 0);
    CHECK(exits(fd, EXIT_MSR_WRITE, 0, 2) == 0 && m.injected == 0 && v->misc_features == 1,
          "the guest turns CPUID faulting on in MSR_MISC_FEATURES_ENABLES");
    set_regs(fd, 0x1000, 0, 0, 0);
    CHECK(exits(fd, EXIT_CPUID, 0, 2) == 0 && m.injected == 0 && regs_of(fd).rip == 0x1003,
          "CPUID at CPL 0 is still answered");
    (void)sregs_of(fd);
    v->sregs.ss.dpl = 3;
    CHECK(kvm_emul_cpuid(v) == -1 && v->events.exception.pending &&
              v->events.exception.nr == GP_VECTOR,
          "and above CPL 0 it is #GP");
    v->events.exception.pending = 0;
    v->sregs.ss.dpl = 0;
}

/** An instruction the decoder refuses above CPL 0, without and with
 * KVM_CAP_EXIT_ON_EMULATION_FAILURE, and DR7.GD under the caller's
 * hardware breakpoints, on the vCPU `fd`. */
static void failure_rows(int vm, int fd)
{
    struct kvm_vcpu *v = vcpu_of(fd);
    struct kvm_run *rp = run_of(fd);
    uint64_t info[5] = {0, 0, 0, 0, 0};

    code(fd, 0x1400, "\x0f\x0b", 2, 0);
    (void)sregs_of(fd);
    v->sregs.ss.dpl = 3;
    CHECK(kvm_emul_mmio(v, info) == 0 && v->exit.kind == KVM_X_AGAIN &&
              v->events.exception.pending && v->events.exception.nr == UD_VECTOR,
          "an instruction the decoder refuses above CPL 0 is the guest's #UD");
    v->events.exception.pending = 0;
    CHECK(vm_cap(vm, KVM_CAP_EXIT_ON_EMULATION_FAILURE, 1) == 0,
          "KVM_CAP_EXIT_ON_EMULATION_FAILURE");
    CHECK(kvm_emul_mmio(v, info) == 0 && v->exit.kind == KVM_X_INTERNAL &&
              v->exit.internal.suberror == KVM_INTERNAL_ERROR_EMULATION &&
              !v->events.exception.pending,
          "and under the capability the caller's emulation failure, with no #UD");
    v->sregs.ss.dpl = 0;

    guest_debug(fd, KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_USE_HW_BP, 0, 0x2400);
    set_regs(fd, 0x1000, 0, 0, 0);
    script(EXIT_DR_ACCESS, 0x10u, 3);
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_DEBUG && rp->u.debug.exception == DB_VECTOR &&
              rp->u.debug.dr6 == 0xFFFF2FF0u && rp->u.debug.dr7 == 0x2400 &&
              rp->u.debug.pc == 0x1000 && regs_of(fd).rip == 0x1000,
          "a debug register's access under the caller's DR7.GD is KVM_EXIT_DEBUG with DR6.BD");
    guest_debug(fd, 0, 0, 0);
}

/** KVM_X86_SET_MSR_FILTER with the ranges `deny` of one MSR each, whose
 * accesses of `flags` it refuses; 0 or the ioctl's error. */
static int32_t msr_filter(int vm, const uint32_t *deny, uint32_t n, uint32_t flags)
{
    static uint64_t none;
    struct kvm_msr_filter f;
    uint32_t i;

    k_memset(&f, 0, sizeof(f));
    for (i = 0; i < n; i++) {
        f.ranges[i].flags = flags;
        f.ranges[i].nmsrs = 1;
        f.ranges[i].base = deny[i];
        f.ranges[i].bitmap = (uintptr_t)&none;
    }
    return fdio(vm, KVM_X86_SET_MSR_FILTER, (uintptr_t)&f);
}

/** The caller's single step over a filtered RDMSR and WRMSR the caller
 * answers, on the vCPU `fd`: KVM_EXIT_DEBUG past the instruction at the
 * KVM_RUN that takes the answer, with no guest entered, as Linux's
 * kvm_skip_emulated_instruction reports it from complete_fast_rdmsr and
 * complete_fast_msr_access. */
static void msr_step_rows(int fd)
{
    struct kvm_run *rp = run_of(fd);
    int runs;

    guest_debug(fd, KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_SINGLESTEP, 0, 0);
    set_regs(fd, 0x1000, 0, 0xDA0u, 0);
    script(EXIT_MSR_READ, 0, 2);
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_X86_RDMSR && regs_of(fd).rip == 0x1000,
          "a filtered RDMSR under the caller's single step is the caller's");
    rp->u.msr.error = 0;
    rp->u.msr.data = 0x1122334455667788ull;
    runs = m.launches + m.resumes;
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_DEBUG && (rp->u.debug.dr6 & 0x4000u) &&
              rp->u.debug.pc == sregs_of(fd).cs.base + 0x1002 && regs_of(fd).rip == 0x1002 &&
              regs_of(fd).rax == 0x55667788u && m.launches + m.resumes == runs,
          "and its answer's KVM_RUN is KVM_EXIT_DEBUG past it, with no guest entered");
    set_regs(fd, 0x1000, 0x9u, 0xDA0u, 0x1u);
    script(EXIT_MSR_WRITE, 0, 2);
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_X86_WRMSR && regs_of(fd).rip == 0x1000,
          "a filtered WRMSR under the caller's single step is the caller's");
    rp->u.msr.error = 0;
    runs = m.launches + m.resumes;
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_DEBUG && (rp->u.debug.dr6 & 0x4000u) &&
              rp->u.debug.pc == sregs_of(fd).cs.base + 0x1002 && regs_of(fd).rip == 0x1002 &&
              m.launches + m.resumes == runs,
          "and its answer's KVM_RUN is KVM_EXIT_DEBUG past it, with no guest entered");
    guest_debug(fd, 0, 0, 0);
}

/** The caller's MSR exits through the backend, as Linux's
 * userspace_msr_exit_test: a filtered access and an unknown MSR are
 * KVM_EXIT_X86_RDMSR or WRMSR, the caller's answer completes the
 * instruction or is #GP, and the bitmap follows the filter for the MSRs
 * it passes to the guest, on the vCPU `fd` of `vm`. */
static void user_msr_rows(int vm, int fd)
{
    uint32_t gpe = GP_VECTOR | (TYPE_HW_EXC << 8) | INTR_ERRCODE | INTR_VALID;
    uint32_t deny[2] = {0xDA0u, 0xC0000100u};
    struct kvm_vcpu *v = vcpu_of(fd);
    struct kvm_run *rp = run_of(fd);

    CHECK(msr_filter(vm, deny, 2, KVM_MSR_FILTER_READ | KVM_MSR_FILTER_WRITE) == 0 &&
              vm_cap(vm, KVM_CAP_X86_USER_SPACE_MSR,
                     KVM_MSR_EXIT_REASON_FILTER | KVM_MSR_EXIT_REASON_UNKNOWN) == 0,
          "a filter of XSS and FS_BASE, with the caller's exits for both reasons");
    set_regs(fd, 0x1000, 0, 0xDA0u, 0);
    script(EXIT_MSR_READ, 0, 2);
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_X86_RDMSR &&
              rp->u.msr.reason == KVM_MSR_EXIT_REASON_FILTER && rp->u.msr.index == 0xDA0u &&
              regs_of(fd).rip == 0x1000,
          "a filtered RDMSR is KVM_EXIT_X86_RDMSR for the filter");
    CHECK((vx(v)->msr_bitmap[1024 + 0x20] & 1u) && (vx(v)->msr_bitmap[3072 + 0x20] & 1u),
          "the run took the filter: FS_BASE's accesses exit");
    rp->u.msr.data = 0x1122334455667788ull;
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && m.injected == 0 && regs_of(fd).rax == 0x55667788u &&
              regs_of(fd).rdx == 0x11223344u && regs_of(fd).rip == 0x1003,
          "the caller's answer is EDX:EAX, past the RDMSR");

    set_regs(fd, 0x1000, 0x9u, 0xDA0u, 0x1u);
    script(EXIT_MSR_WRITE, 0, 2);
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_X86_WRMSR &&
              rp->u.msr.data == 0x100000009ull && rp->u.msr.index == 0xDA0u,
          "a filtered WRMSR is KVM_EXIT_X86_WRMSR with EDX:EAX");
    rp->u.msr.error = 1;
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && m.injected == gpe && regs_of(fd).rip == 0x1001,
          "and the caller's error is #GP at it");
    msr_step_rows(fd);

    set_regs(fd, 0x1000, 0, 0xDEADBEEFu, 0);
    script(EXIT_MSR_READ, 0, 2);
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_X86_RDMSR &&
              rp->u.msr.reason == KVM_MSR_EXIT_REASON_UNKNOWN,
          "an unknown MSR is the caller's, for KVM_MSR_EXIT_REASON_UNKNOWN");
    rp->u.msr.error = 1;
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && m.injected == gpe, "and its error is #GP");
    CHECK(vm_cap(vm, KVM_CAP_X86_USER_SPACE_MSR, KVM_MSR_EXIT_REASON_FILTER) == 0,
          "the caller's exits for the filter alone");
    set_regs(fd, 0x1000, 0, 0xDEADBEEFu, 0);
    CHECK(exits(fd, EXIT_MSR_READ, 0, 2) == 0 && m.injected == gpe && regs_of(fd).rip == 0x1001,
          "and the unknown MSR is the guest's #GP");

    CHECK(msr_filter(vm, deny, 0, 0) == 0 && vm_cap(vm, KVM_CAP_X86_USER_SPACE_MSR, 0) == 0,
          "a filter that allows all, without the caller's exits");
    set_regs(fd, 0x1000, 0, 0xC0000100u, 0);
    CHECK(exits(fd, EXIT_MSR_READ, 0, 2) == 0 && m.injected == 0 && regs_of(fd).rip == 0x1001,
          "an exit of FS_BASE under the old bitmap enters again, not stepped");
    CHECK(!(vx(v)->msr_bitmap[1024 + 0x20] & 7u) && !(vx(v)->msr_bitmap[3072 + 0x20] & 7u),
          "and FS_BASE, GS_BASE and KERNEL_GS_BASE pass to the guest again");
}

/** TSC scaling: the Haswell has none, and on a model with the control a
 * vCPU of `vm` runs under it with the multiplier of its ratio, 1 for a
 * TSC at the host's rate. */
static void tsc_scale_rows(int vm)
{
    uint32_t frac = 0, had = caps.proc2;
    uint64_t model = m.proc2;
    int fd;

    CHECK(vmx_tsc_scaling(&frac) == 0 && fdio(vm, KVM_CHECK_EXTENSION, KVM_CAP_VM_TSC_CONTROL) == 0,
          "the Haswell scales no TSC, and KVM_CAP_VM_TSC_CONTROL says so");
    m.proc2 |= (uint64_t)PROC2_TSC_SCALING << 32;
    caps.proc2 |= PROC2_TSC_SCALING;
    CHECK(vmx_tsc_scaling(&frac) == ~0ull && frac == 48 &&
              fdio(vm, KVM_CHECK_EXTENSION, KVM_CAP_VM_TSC_CONTROL) == 1,
          "with the control, a multiplier of 48 bits of fraction and the capability");
    fd = make_vcpu(vm, 5);
    CHECK(exits(fd, EXIT_CPUID, 0, 2) == 0 &&
              (fields(fd)[SECONDARY_VM_EXEC_CONTROL] & PROC2_TSC_SCALING) &&
              fields(fd)[TSC_MULTIPLIER] == 1ull << 48,
          "a vCPU runs under TSC scaling with a multiplier of 1");
    vcpu_of(fd)->tsc_ratio = 3ull << 47;
    vcpu_of(fd)->dirty |= KVM_PART_TSC;
    set_regs(fd, 0x1000, 0, 0, 0);
    CHECK(exits(fd, EXIT_CPUID, 0, 2) == 0 && fields(fd)[TSC_MULTIPLIER] == 3ull << 47,
          "and with its ratio once it has one");
    fdclose(fd);
    m.proc2 = model;
    caps.proc2 = had;
}

/** A vCPU of id `id` on `vm` as `make_vcpu` makes it, with XSAVE in its
 * CPUID's leaf 1 only when `xsave`, and XSAVES in leaf 0xD's subleaf 1
 * only when `xsaves`. */
static int xsaves_vcpu(int vm, uint32_t id, int xsave, int xsaves)
{
    struct kvm_cpuid_entry2 *e = (struct kvm_cpuid_entry2 *)(buf.bytes + sizeof(buf.cpuid));
    struct kvm_sregs s;
    uint32_t i;
    int fd = fdio(vm, KVM_CREATE_VCPU, id);

    CHECK(fd > vm, "KVM_CREATE_VCPU");
    buf.cpuid.nent = 256;
    CHECK(dev(KVM_GET_SUPPORTED_CPUID, (uintptr_t)&buf) == 0, "KVM_GET_SUPPORTED_CPUID");
    for (i = 0; i < buf.cpuid.nent; i++) {
        if (e[i].function == 1) {
            e[i].ecx = xsave ? e[i].ecx | (1u << 26) : e[i].ecx & ~(1u << 26);
        } else if (e[i].function == 0xD && e[i].index == 1) {
            e[i].eax = xsaves ? e[i].eax | (1u << 3) : e[i].eax & ~(1u << 3);
        }
    }
    CHECK(fdio(fd, KVM_SET_CPUID2, (uintptr_t)&buf) == 0, "KVM_SET_CPUID2 of XSAVE and XSAVES");
    s = sregs_of(fd);
    s.cs.base = 0;
    s.cs.selector = 0;
    CHECK(fdio(fd, KVM_SET_SREGS, (uintptr_t)&s) == 0, "KVM_SET_SREGS");
    set_regs(fd, 0x1000, 0, 0, 0);
    return fd;
}

/** Leaf 0xD subleaf 1's EBX in the CPUID of the vCPU on `fd`, or ~0. */
static uint32_t d1_ebx(int fd)
{
    const struct kvm_cpuid_entry2 *e = kvm_cpuid_find(vcpu_of(fd), 0xD, 1);

    return e ? e->ebx : ~0u;
}

/** XSAVES on a model whose secondary controls have it, on vCPUs of their
 * own on `vm`: KVM offers it; the control is on only for a vCPU whose
 * CPUID has XSAVE and XSAVES, as Linux's vmx_secondary_exec_control; and
 * subleaf 1's compacted size follows the guest's XCR0 through XSETBV. */
static void xsaves_rows(int vm)
{
    uint32_t had = caps.proc2;
    uint64_t model = m.proc2;
    int fd;

    m.proc2 |= (uint64_t)PROC2_XSAVES << 32;
    caps.proc2 |= PROC2_XSAVES;
    CHECK(vmx_xsaves(), "with the control the backend lets a guest run XSAVES");
    fd = xsaves_vcpu(vm, 9, 1, 1);
    CHECK(d1_ebx(fd) == 576, "subleaf 1's compacted size is x87's and SSE's at the reset's XCR0");
    CHECK(exits(fd, EXIT_CPUID, 0, 2) == 0 &&
              (fields(fd)[SECONDARY_VM_EXEC_CONTROL] & PROC2_XSAVES) &&
              fields(fd)[XSS_EXIT_BITMAP] == 0,
          "a vCPU whose CPUID has XSAVE and XSAVES runs with the control and no XSS exit");
    set_regs(fd, 0x1000, 7, 0, 0);
    CHECK(exits(fd, EXIT_XSETBV, 0, 3) == 0 && vcpu_of(fd)->xcr0 == 7 && d1_ebx(fd) == 832,
          "XSETBV of AVX makes subleaf 1's compacted size 832");
    set_regs(fd, 0x1000, 3, 0, 0);
    CHECK(exits(fd, EXIT_XSETBV, 0, 3) == 0 && vcpu_of(fd)->xcr0 == 3 && d1_ebx(fd) == 576,
          "and XSETBV of x87 and SSE 576 again");
    fdclose(fd);
    fd = xsaves_vcpu(vm, 10, 1, 0);
    CHECK(exits(fd, EXIT_CPUID, 0, 2) == 0 &&
              !(fields(fd)[SECONDARY_VM_EXEC_CONTROL] & PROC2_XSAVES),
          "a vCPU whose CPUID lacks XSAVES runs without the control");
    fdclose(fd);
    fd = xsaves_vcpu(vm, 11, 0, 1);
    CHECK(exits(fd, EXIT_CPUID, 0, 2) == 0 &&
              !(fields(fd)[SECONDARY_VM_EXEC_CONTROL] & PROC2_XSAVES),
          "and one whose CPUID has XSAVES without XSAVE");
    fdclose(fd);
    m.proc2 = model;
    caps.proc2 = had;
}

/** Whether the `len` bytes at `a` and `b` are the same. */
static int same(const char *a, const char *b, size_t len)
{
    while (len && *a == *b) {
        a++;
        b++;
        len--;
    }
    return len == 0;
}

/** The length of the string `s`. */
static size_t length(const char *s)
{
    size_t len = 0;

    while (s[len]) {
        len++;
    }
    return len;
}

/** The line of kvm/exit_stats in `text` whose code is `code`, its nine
 * counts after the code read into `n`: 1 when it is there whole, with the
 * name `name` last, and 0 otherwise. */
static int stat_line(const char *text, const char *code, const char *name, uint64_t n[9])
{
    const char *p = text;
    size_t len = length(code), i;

    while (*p && !(p > text && p[-1] == '\n' && same(p, code, len) && p[len] == ' ')) {
        p++;
    }
    if (!*p) {
        return 0;
    }
    p += len;
    for (i = 0; i < 9; i++) {
        if (*p++ != ' ' || *p < '0' || *p > '9') {
            return 0;
        }
        for (n[i] = 0; *p >= '0' && *p <= '9'; p++) {
            n[i] = n[i] * 10u + (uint64_t)(*p - '0');
        }
    }
    len = length(name);
    return *p == ' ' && same(p + 1, name, len) && p[1 + len] == '\n';
}

/**
 * kvm/exit_stats through the VMX backend: a CPUID exit the kernel handles,
 * then a HLT for the caller. Each line gives, in this order, the code, the
 * exits, those handled in the kernel, those that ended KVM_RUN, the wait,
 * the handling and its preparation, and the entry, run and exit the backend
 * timed around VMLAUNCH and VMRESUME, which the model's TSC steps make 3,
 * 1000 and 7 cycles.
 */
static void exitstat_rows(void)
{
    static const char columns[] = "\ncode exits kernel user wait_cycles handle_cycles "
                                  "prepare_cycles entry_cycles run_cycles exit_cycles name\n";
    static char buf[2048];
    struct kvm_userspace_memory_region region = {0, 0, 0, sizeof(ram), 0};
    unsigned int was_cpu = host_cpu;
    const char *nl;
    uint64_t n[9];
    int32_t len;
    int vm, fd;

    on_cpu(0);
    vm = dev(KVM_CREATE_VM, 0);
    region.userspace_addr = (uint64_t)(uintptr_t)ram;
    CHECK(fdio(vm, KVM_SET_USER_MEMORY_REGION, (uintptr_t)&region) == 0, "a VM of a slot");
    fd = make_vcpu(vm, 0);
    CHECK(exitstat_ops && exitstat_ops->store(exitstat_data, (const char __user *)"0", 1) == 1,
          "kvm/exit_stats is registered and cleared");
    set_regs(fd, 0x1000, 0, 0, 0);
    script(EXIT_CPUID, 0, 2);
    script(EXIT_HLT, 0, 1);
    tsc_steps = 1;
    CHECK(run(fd) == 0, "a CPUID exit handled in the kernel, then a HLT for the caller");
    tsc_steps = 0;
    len = exitstat_ops->show(exitstat_data, buf, sizeof(buf) - 1u);
    CHECK(len > 0 && len < (int32_t)sizeof(buf), "kvm/exit_stats fits its buffer");
    buf[len > 0 && len < (int32_t)sizeof(buf) ? len : 0] = '\0';
    for (nl = buf; *nl && *nl != '\n'; nl++) {
    }
    CHECK(same(buf, "backend vmx tsc_khz ", 20) && same(nl, columns, sizeof(columns) - 1u),
          "kvm/exit_stats names its backend and its ten columns, in order");
    CHECK(stat_line(buf, "0xa", "CPUID", n) && n[0] == 1 && n[1] == 1 && n[2] == 0 && n[6] == 3 &&
              n[7] == 1000 && n[8] == 7,
          "CPUID's line: one exit, handled in the kernel, whose run's entry, run and exit "
          "took 3, 1000 and 7 cycles");
    CHECK(stat_line(buf, "0xc", "HLT", n) && n[0] == 1 && n[1] == 0 && n[2] == 1 && n[5] == 0 &&
              n[6] == 3 && n[7] == 1000 && n[8] == 7,
          "HLT's line: one exit that ended KVM_RUN, with no preparation, and the same three");
    exitstat_ops->store(exitstat_data, (const char __user *)"0", 1);
    fdclose(fd);
    fdclose(vm);
    model_tsc = 1000000;
    on_cpu((int)was_cpu);
}

/** A halted vCPU's sleep between two runs of one KVM_RUN, with the local
 * APIC in the kernel: the switch away puts the host's state back, the next
 * entry saves and loads it again, and the guest's FPU stays loaded. */
static void block_rows(void)
{
    struct kvm_userspace_memory_region region = {0, 0, 0, sizeof(ram), 0};
    int vm, fd, saves, loads, blocks;

    vm = dev(KVM_CREATE_VM, 0);
    region.userspace_addr = (uint64_t)(uintptr_t)ram;
    CHECK(fdio(vm, KVM_SET_USER_MEMORY_REGION, (uintptr_t)&region) == 0 &&
              vm_cap(vm, KVM_CAP_SPLIT_IRQCHIP, 24) == 0,
          "a VM of a slot and the local APIC in the kernel");
    k_memset((void *)apic_page, 0, sizeof(apic_page));
    apic_irr = -1;
    fd = make_vcpu(vm, 0);
    saves = m.saves;
    loads = m.fpu_loads;
    blocks = m.blocks;
    m.block = 2;
    script(EXIT_CPUID, 0, 2);
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && m.blocks == blocks + 1 && m.saves == saves + 2 &&
              m.fpu_loads == loads + 1,
          "a halted vCPU's sleep between two runs puts the host's state back, and the next "
          "entry saves it again; the guest's FPU goes with the thread");
    fdclose(fd);
    fdclose(vm);
}

/**
 * The TPR shadow, as Linux's vmx.c keeps it, on a VM with the local APIC in
 * the kernel: CR8's exits off and the APIC's page the virtual-APIC page;
 * the guest's MOV to CR8, which writes the page's TPR without an exit,
 * read back by KVM_GET_SREGS and the run page; the TPR threshold chosen at
 * every entry, the class of a vector TPR holds back and 0 once TPR lets it
 * through, never above TPR, which the model's entry checks, after TPR
 * changed by the guest, by the APIC with no request for an injection and
 * by KVM_SET_SREGS; the TPR-below-threshold
 * exit, which brings PPR up to date and asks for an injection, the MOV
 * done; and CR8's exits kept for a processor without the shadow.
 */
static void tpr_rows(void)
{
    struct kvm_userspace_memory_region region = {0, 0, 0, sizeof(ram), 0};
    struct kvm_sregs sr;
    struct row *w;
    uint64_t *f, rip;
    int vm, fd;

    vm = dev(KVM_CREATE_VM, 0);
    region.userspace_addr = (uint64_t)(uintptr_t)ram;
    CHECK(fdio(vm, KVM_SET_USER_MEMORY_REGION, (uintptr_t)&region) == 0 &&
              vm_cap(vm, KVM_CAP_SPLIT_IRQCHIP, 24) == 0,
          "a VM of a slot and the local APIC in the kernel");
    k_memset((void *)apic_page, 0, sizeof(apic_page));
    apic_irr = -1;
    ppr_updates = 0;
    fd = make_vcpu(vm, 0);
    CHECK(exits(fd, EXIT_CPUID, 0, 2) == 0, "a vCPU with the APIC in the kernel runs");
    f = fields(fd);
    CHECK((m.entry_proc & PROC_TPR_SHADOW) && !(m.entry_proc & (PROC_CR8_LOAD | PROC_CR8_STORE)),
          "the guest runs under the TPR shadow, and CR8 does not exit");
    CHECK(f[VIRTUAL_APIC_PAGE_ADDR] == (uintptr_t)apic_page && f[TPR_THRESHOLD] == 0,
          "the APIC's page is the virtual-APIC page, with no threshold while nothing waits");

    w = script(EXIT_CPUID, 0, 2);
    w->tpr_write = 1;
    w->tpr = 0x60;
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && sregs_of(fd).cr8 == 6 && run_of(fd)->cr8 == 6,
          "the guest's MOV CR8, 6, done without an exit, is KVM_GET_SREGS's and the run page's");
    apic_irr = 0x51;
    CHECK(exits(fd, EXIT_CPUID, 0, 2) == 0 && f[TPR_THRESHOLD] == 5,
          "with vector 0x51 waiting under TPR 0x60, the threshold is its class, 5");

    rip = regs_of(fd).rip;
    /* 4, the length of MOV CR8, RAX (44 0F 22 C0), which a step would add. */
    w = script(EXIT_TPR_BELOW, 0, 4);
    w->tpr_write = 1;
    w->tpr = 0x40;
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && run_of(fd)->exit_reason == KVM_EXIT_HLT && ppr_updates == 1 &&
              regs_of(fd).rip == rip + 1,
          "the guest's MOV CR8, 4, below the threshold exits: PPR is brought up to date, and "
          "only HLT steps RIP");
    CHECK(f[TPR_THRESHOLD] == 0 && sregs_of(fd).cr8 == 4,
          "the next entry's threshold is 0, since TPR 0x40 lets vector 0x51 through");

    CHECK(exits(fd, EXIT_CPUID, 0, 2) == 0 && f[TPR_THRESHOLD] == 0, "the threshold stays 0");
    apic_page[0x80 / 4] = 0x60;
    CHECK(exits(fd, EXIT_CPUID, 0, 2) == 0 && f[TPR_THRESHOLD] == 5,
          "TPR raised over the vector again, the threshold is 5");
    apic_page[0x80 / 4] = 0x00;
    CHECK(exits(fd, EXIT_CPUID, 0, 2) == 0 && run_of(fd)->exit_reason == KVM_EXIT_HLT &&
              f[TPR_THRESHOLD] == 0,
          "TPR lowered with no request for an injection, as an MMIO write does while a vector "
          "in service holds PPR, the next entry's threshold is 0, which its check of VTPR passes");
    apic_page[0x80 / 4] = 0x60;
    CHECK(exits(fd, EXIT_CPUID, 0, 2) == 0 && f[TPR_THRESHOLD] == 5, "the threshold is 5 again");
    sr = sregs_of(fd);
    sr.cr8 = 0;
    CHECK(fdio(fd, KVM_SET_SREGS, (uintptr_t)&sr) == 0 && apic_page[0x80 / 4] == 0 &&
              exits(fd, EXIT_CPUID, 0, 2) == 0 && f[TPR_THRESHOLD] == 0,
          "KVM_SET_SREGS's CR8 of 0 writes the page's TPR, and the next entry's threshold is 0");
    apic_irr = -1;
    fdclose(fd);

    caps.tpr_shadow = 0;
    fd = make_vcpu(vm, 1);
    CHECK(exits(fd, EXIT_CPUID, 0, 2) == 0 && !(m.entry_proc & PROC_TPR_SHADOW) &&
              (m.entry_proc & (PROC_CR8_LOAD | PROC_CR8_STORE)) ==
                  (PROC_CR8_LOAD | PROC_CR8_STORE) &&
              fields(fd)[VIRTUAL_APIC_PAGE_ADDR] == 0,
          "without the shadow CR8 exits, and no virtual-APIC page is given");
    set_regs(fd, 0x1000, 3, 0, 0);
    CHECK(exits(fd, EXIT_CR_ACCESS, 8, 4) == 0 && apic_page[0x80 / 4] == 0x30,
          "and its MOV CR8, 3, exits to set the APIC's TPR");
    caps.tpr_shadow = 1;
    fdclose(fd);
    fdclose(vm);
}

/** The first entry of a new vCPU in 64-bit mode with RSP all ones, a
 * canonical stack pointer that KVM_SET_REGS takes: the VMCS gets it though
 * the entry's cache fills its fields with all ones at the build, since its
 * RSP, RIP and RFLAGS are known only once written, as Linux's regs_dirty
 * makes vmx_vcpu_run write them at the first entry (x86.c:11505-11506,
 * vmx.c:7379-7383). */
static void first_entry_rows(int vm)
{
    int fd = make_vcpu(vm, 12);
    struct kvm_regs r;

    long_start(fd, "\x0F\xA2\xF4", 3);
    r = regs_of(fd);
    r.rsp = ~0ull;
    CHECK(fdio(fd, KVM_SET_REGS, (uintptr_t)&r) == 0, "KVM_SET_REGS with RSP all ones");
    script(EXIT_CPUID, 0, 2);
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && run_of(fd)->exit_reason == KVM_EXIT_HLT,
          "a new vCPU with RSP all ones runs CPUID, then HLT to the caller");
    r = regs_of(fd);
    CHECK(r.rsp == ~0ull && r.rip == 0x1003,
          "the first entry writes RSP all ones to the VMCS, and the exit reads it back");
    fdclose(fd);
}

/** What Linux's selftests ask of the guest's side, on a vCPU of its own
 * on `vm` in protected mode. The rows change the VM's capabilities, so
 * they come last. */
static void selftest_rows(int vm)
{
    int fd = make_vcpu(vm, 4);

    ts_start(fd);
    quirk_rows(vm, fd);
    platform_rows(vm, fd);
    out_rows(vm, fd);
    failure_rows(vm, fd);
    user_msr_rows(vm, fd);
    fdclose(fd);
    tsc_scale_rows(vm);
    xsaves_rows(vm);
}

/** Run the rows; 0 when every check passed, 1 otherwise. */
int main(void)
{
    struct kvm_userspace_memory_region region = {0, 0, 0, sizeof(ram), 0};
    int vm, fd;

    caller.mm = &caller_mm;
    caller.fds = &table;
    probe_rows();
    vm = dev(KVM_CREATE_VM, 0);
    CHECK(vm >= 3, "KVM_CREATE_VM");
    region.userspace_addr = (uint64_t)(uintptr_t)ram;
    CHECK(fdio(vm, KVM_SET_USER_MEMORY_REGION, (uintptr_t)&region) == 0, "a slot of 1 MiB at 0");
    fd = make_vcpu(vm, 0);
    launch_rows(fd);
    io_rows(fd);
    io_string_rows(fd);
    ept_rows(fd);
    cr_rows(fd);
    cr_owned_rows(fd);
    insn_rows(fd);
    load_rows(fd);
    linux_rows(fd);
    event_rows(fd);
    debug_rows(fd);
    host_rows(fd);
    debugctl_rows(fd);
    path_rows(fd);
    pae_rows(fd);
    vpid_panic_rows(vm, fd);
    ts_rows(vm);
    hv_rows(vm);
    selftest_rows(vm);
    first_entry_rows(vm);
    tpr_rows();
    block_rows();
    exitstat_rows();
    CHECK(m.ud == 0 && m.gp == 0 && m.bad_writes == 0 && m.bad_reads == 0,
          "no VMX instruction outside VMX operation and no field the VMCS lacks, over every row");
    CHECK(slot_copies > 0 && dirty_marks > 0 && unheld_copies == 0 && slot_holds == 0,
          "every copy through the slot and every dirty mark under the slots' readers' lock, "
          "over every row, and each hold given back");
    fdclose(fd);
    fdclose(vm);
    CHECK(vpid_used[0] == 0, "the vCPUs' VPIDs are freed with the VM");
    CHECK(cpu_ok(0) && cpu_ok(1) && !m.on && !m.cpus[1].on && m.vmxons == m.vmxoffs,
          "the freed vCPUs' VMCSs are cleared, and VMX is off with the last");
    CHECK(live == 5, "the VM's allocations are given back, but for the VMXON regions of the four "
                     "processors, which the backend keeps, and the exit counts of the processor "
                     "that started KVM");
    printf("kvm-vmx-hosttest: result: %s: %d failures\n", failures ? "fail" : "pass", failures);
    return failures ? 1 : 0;
}
