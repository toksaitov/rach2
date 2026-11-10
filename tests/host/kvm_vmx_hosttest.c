/**
 * Check the VMX backend, vmx.c, on a model of the Lenovo's processor.
 *
 * host-deps: rach2/sys/kvm/kvm.c rach2/sys/kvm/vcpu.c rach2/sys/kvm/cpuid.c
 * host-deps: rach2/sys/kvm/msr.c rach2/sys/kvm/emulate.c rach2/sys/kvm/decode.c
 * host-deps: rach2/sys/kvm/hyperv.c rach2/sys/kvm/pvclock.c
 * host-deps: rach2/sys/kvm/binstats.c
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
 * processor without what the backend needs and choosing the Lenovo's controls;
 * a VM and its vCPU; each entry inside VMXON and VMPTRLD and each exit followed
 * by VMCLEAR and VMXOFF with CR4 as it was, on every path; the VMCS built and
 * checked by the model; each exit the corpus's guests cause handled in the
 * kernel or given to the caller: CPUID, port I/O and its string form, EPT
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
 * interrupt, NMI and machine check, the preemption timer, a triple fault and
 * the two kinds of failed entry; hardware task switches by CALL, IRET, JMP and
 * task gates, to 32-bit, 16-bit and virtual-8086 tasks, and their faults; a
 * move to another processor; a state read between runs; VMXON and VMPTRLD
 * failing; a failed VMWRITE reported once; VPIDs; a panic inside a run,
 * which turns VMX off; and what Linux's selftests ask of the guest's side:
 * AMD's hypercall instruction patched or left as its quirk says, MWAIT's
 * quirk, MSR_PLATFORM_INFO and CPUID faulting, a plain OUT's RIP at its
 * exit and after it with port 0x7E's quirk, an emulation failure above
 * CPL 0 under KVM_CAP_EXIT_ON_EMULATION_FAILURE, DR7.GD under the
 * caller's breakpoints, and the TSC multiplier on a model whose controls
 * have TSC scaling.
 */
#include "kvm/vmx.c"

#include <setjmp.h>

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
static fd_table_t table = {entries, 64, 1, 0, NULL};
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

void rtc_monotonic(int64_t *s, int64_t *ns)
{
    *s = 5;
    *ns = 0;
}

void rtc_realtime(int64_t *s, int64_t *ns)
{
    *s = 1800000000;
    *ns = 0;
}

uint64_t arch_cycles(void)
{
    return 1000000;
}

/* The big lock's depth, the interrupt flag and a pending signal. */
static int bkl_depth = 1, irq_on = 1;

int bkl_drop(void)
{
    int d = bkl_depth;

    bkl_depth = 0;
    return d;
}

void bkl_take(int depth)
{
    bkl_depth = depth;
}

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

/** The run loop's switch at a slice's end: one thread here, no slice ends. */
void sched_take_preempt(void)
{}

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

unsigned int arch_atomic_cmpxchg(volatile unsigned int *p, unsigned int old, unsigned int value)
{
    unsigned int cur = *p;

    if (cur == old) {
        *p = value;
    }
    return cur;
}

/* The CPUID of the Lenovo's Haswell, as tests/host/kvm_hosttest.c gives
 * it; `cpuid_vmx` clears VMX for the probe's refusal. */
static int cpuid_vmx = 1;

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

void kvm_dirty_mark(struct kvm_vm *vm, uint64_t gfn)
{
    (void)vm;
    (void)gfn;
    dirty_marks++;
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

int32_t kvm_irq_enable_split(struct kvm_vm *vm, const struct kvm_enable_cap *cap)
{
    (void)vm;
    (void)cap;
    return -EINVAL;
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

int32_t kvm_lapic_prepare(struct kvm_vcpu *v)
{
    (void)v;
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

int vclock_read_cycles(int real, uint64_t *ns, uint64_t *cycles)
{
    (void)real;
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

#define EXIT_RIP_MODEL 0xFFFFFFFF80123450ull
#define HOST_CR4_MODEL 0x000406F0ull

/* A VMCS the model knows: its address, launch state and fields by
 * encoding. */
struct mvmcs {
    uint64_t pa;
    int launched;
    uint64_t f[0x7000];
};

/* A guest's run, a row of the script: the exit and what it leaves. */
struct row {
    uint32_t reason;
    uint64_t qual, gpa;
    uint32_t intr_info, intr_error, idt_info, idt_error, len, insn_info, intr_state;
    int panic;      /* a panic comes inside the run */
    uint64_t rip;   /* the guest's RIP at the exit, when it moved without one */
    uint32_t cs_ar; /* CS's access rights at the exit, when a far jump changed them */
};

static struct {
    /* The capability MSRs and IA32_FEATURE_CONTROL. */
    uint64_t basic, pin, proc, exit, entry, misc, proc2, ept_vpid;
    uint64_t fc;
    int fc_writes;
    /* The processor's state. */
    int on;
    uint64_t cr4;
    uint64_t vmxon_pa;
    struct mvmcs *cur;
    struct mvmcs vmcs[6];
    int nvmcs;
    uint64_t dr[8];
    int dr6_writes;
    /* The counts of each instruction, and its failures. */
    int vmxons, vmxoffs, vmclears, vmptrlds, launches, resumes, invepts, invvpids;
    int ud, gp, bad_writes, bad_reads;
    int nmis, mces, wbinvds, fpu_loads, fpu_stores, restores;
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
    if (m.nvmcs == 6) {
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

/** VMXOFF: #UD outside VMX operation. */
static int m_vmxoff(void)
{
    if (!m.on) {
        return not_on();
    }
    CHECK(!m.cur, "VMCLEAR writes the VMCS back before VMXOFF");
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
    c->launched = 0;
    if (m.cur == c) {
        m.cur = NULL;
    }
    return VMX_OK;
}

/** VMPTRLD: VMfailValid for the VMXON region or the wrong revision. */
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
    return VMX_OK;
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
    f[VM_ENTRY_INTR_INFO_FIELD] &= ~(uint64_t)INTR_VALID;
}

/** VMLAUNCH or VMRESUME: the checks, then the script's next run. */
static int m_run(struct kvm_regs *regs, uint64_t *cr2, int launched)
{
    static const struct row none = {EXIT_HLT, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0};
    const struct row *r;
    uint64_t *f;

    (void)regs;
    (void)cr2;
    CHECK(!irq_on && bkl_depth == 0, "the guest runs with interrupts off and the big lock let go");
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
    m.injected = (uint32_t)f[VM_ENTRY_INTR_INFO_FIELD];
    m.injected_error = (uint32_t)f[VM_ENTRY_EXCEPTION_ERROR_CODE];
    m.entry_cr0 = f[GUEST_CR0];
    m.entry_proc = f[CPU_BASED_VM_EXEC_CONTROL];
    m.entry_rflags = f[GUEST_RFLAGS];
    m.entry_intr = f[GUEST_INTERRUPTIBILITY_INFO];
    m.entry_dr0 = m.dr[0];
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
    CHECK((f[PIN_BASED_VM_EXEC_CONTROL] & PIN_PREEMPTION) &&
              f[VMX_PREEMPTION_TIMER_VALUE] == (VMX_RUN_BOUND_TSC >> (m.misc & 0x1Fu)),
          "every run is bounded by the preemption timer");
    m.cur->launched = 1;
    r = m.script_at < m.script_len ? &m.script[m.script_at++] : &none;
    CHECK(r != &none, "the guest runs only as the script says");
    if (r->panic) {
        k_panic_hook();
        longjmp(panic_jump, 1);
    }
    exit_with(r);
    return VMX_OK;
}

/** RDMSR of the model: the capability MSRs, the true controls and their
 * defaults, IA32_FEATURE_CONTROL, PAT and EFER; any other MSR reads as a
 * value of its own. */
static uint64_t m_rdmsr(uint32_t msr)
{
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
    case MSR_EFER:
        return 0xD01ull;
    default:
        return 0x1000ull + msr;
    }
}

/** WRMSR: only IA32_FEATURE_CONTROL matters, written only unlocked. */
static void m_wrmsr(uint32_t msr, uint64_t value)
{
    if (msr == MSR_IA32_FEATURE_CONTROL) {
        CHECK(!(m.fc & FC_LOCKED), "IA32_FEATURE_CONTROL is written only while unlocked");
        m.fc = value;
        m.fc_writes++;
    }
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

/** Write a debug register. */
static void m_write_dr(int n, uint64_t value)
{
    m.dr[n & 7] = value;
    if ((n & 7) == 6) {
        m.dr6_writes++;
    }
}

/** The host's state before an entry: an IDT and an FS base. */
static void m_save_host(struct vmx_host *h)
{
    k_memset(h, 0, sizeof(*h));
    h->idt_base = 0xFFFFFFFF80200000ull;
    h->fs_base = 0x700000ull;
}

/** Put the host's state back, counted. */
static void m_restore_host(const struct vmx_host *h)
{
    (void)h;
    m.restores++;
}

/** Load the guest's FPU, which happens only inside VMX operation. */
static void m_fpu_load(void *host, const void *guest, uint64_t xcr0)
{
    (void)host;
    (void)guest;
    (void)xcr0;
    CHECK(m.on, "the guest's FPU is loaded inside the run's section");
    m.fpu_loads++;
}

/** Store the guest's FPU, counted. */
static void m_fpu_store(void *host, void *guest, uint64_t xcr0)
{
    (void)host;
    (void)guest;
    (void)xcr0;
    m.fpu_stores++;
}

/** The host's NMI, raised only after VMXOFF and with interrupts off. */
static void m_nmi(void)
{
    CHECK(!m.on && irq_on == 0, "the host's NMI is raised after VMXOFF, interrupts still off");
    m.nmis++;
}

/** The host's machine check, raised only after VMXOFF. */
static void m_machine_check(void)
{
    CHECK(!m.on, "the host's machine check is raised after VMXOFF");
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
    m_vmxon,     m_vmxoff,  m_vmclear,       m_vmptrld,   m_vmread,       m_vmwrite,
    m_invept,    m_invvpid, m_run,           m_rdmsr,     m_wrmsr,        m_read_cr,
    m_write_cr4, m_read_dr, m_write_dr,      m_save_host, m_restore_host, m_fpu_load,
    m_fpu_store, m_nmi,     m_machine_check, m_wbinvd,    m_exit_rip};

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
    m.cr0 = 0x80050033ull;
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
 * path must pass: VMX off, CR4 as it was, the VMCS clear, the host's state
 * put back after each run, and every row used. Returns KVM_RUN's result. */
static int32_t run(int fd)
{
    int32_t r;
    int runs = m.launches + m.resumes;

    r = fdio(fd, KVM_RUN, 0);
    CHECK(!m.on && m.cr4 == HOST_CR4_MODEL, "VMX is off with CR4 as it was after KVM_RUN");
    CHECK(m.vmxons == m.vmxoffs, "each VMXON has its VMXOFF");
    CHECK(m.restores == m.launches + m.resumes, "the host's state is put back after every run");
    CHECK(m.fpu_loads == m.fpu_stores, "the guest's FPU goes back after every run");
    CHECK(m.script_at == m.script_len, "every run the script gives is entered");
    CHECK(irq_on && bkl_depth == 1, "KVM_RUN ends with interrupts on and the big lock held");
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
    CHECK(vmx_cache(KVM_MT_WB, 1) == 0x70u && vmx_cache(KVM_MT_WB, 0) == 0x30u &&
              vmx_cache(KVM_MT_UC, 0) == 0,
          "EPT's memory types: write-back with IPAT, without, and uncached");
}

/** The first entries: the VMCS built and checked, CPUID and HLT. */
static void launch_rows(int fd)
{
    struct kvm_vcpu *v = vcpu_of(fd);
    struct kvm_regs r;
    const uint64_t *e;
    uint64_t *f;

    set_regs(fd, 0x1000, 0, 0, 0);
    script(EXIT_CPUID, 0, 2);
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && run_of(fd)->exit_reason == KVM_EXIT_HLT, "CPUID, then HLT to the caller");
    r = regs_of(fd);
    CHECK(r.rbx == 0x756E6547u && r.rdx == 0x49656E69u && r.rip == 0x1003,
          "CPUID answers from the guest's table and both instructions are stepped");
    CHECK(m.launches == 2 && m.resumes == 0 && m.vmxons == 2 && m.fc_writes == 0,
          "each entry is a VMLAUNCH of a cleared VMCS inside its own VMXON");
    CHECK(m.ud == 0 && m.gp == 0 && m.bad_writes == 0 && m.bad_reads == 0,
          "no VMX instruction outside VMX operation and no field the VMCS lacks");
    f = fields(fd);
    CHECK(f[VIRTUAL_PROCESSOR_ID] == 1 && f[EPT_POINTER] == (TDP_ROOT | 0x5Eu),
          "VPID 1 and the EPT pointer: four levels, write-back, A/D");
    CHECK(f[CR0_READ_SHADOW] == 0x60000010u && f[GUEST_CR0] == 0x30u && f[GUEST_CR4] == 0x2000u,
          "the reset's CR0 in the shadow, run without CD and NW, with NE and VMXE");
    CHECK(f[VM_ENTRY_MSR_LOAD_COUNT] == 6 && f[VM_EXIT_MSR_STORE_COUNT] == 6 &&
              f[VM_EXIT_MSR_LOAD_COUNT] == 5,
          "the guest's six MSRs in and out, the host's five back");
    e = (const uint64_t *)(uintptr_t)f[VM_EXIT_MSR_LOAD_ADDR];
    CHECK(e[0] == MSR_STAR && e[1] == 0x1000u + MSR_STAR && e[8] == MSR_TSC_AUX &&
              e[9] == 0x1000u + MSR_TSC_AUX,
          "the exit's MSR list is the host's, with their values");
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
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && (regs_of(fd).rax & 0xFFFF) == 0xBEEF && regs_of(fd).rip == 0x1103,
          "its answer reaches AX and the instruction is stepped");

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
    launches = m.launches;
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_MMIO && rp->u.mmio.is_write &&
              rp->u.mmio.data[0] == 8 && regs_of(fd).rip == 0x1302,
          "then writes the sum at the next KVM_RUN");
    CHECK(m.launches == launches, "with no guest entered between the read and the write");
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
    set_regs(fd, 0x1000, 0x11, MSR_STAR, 0x00230010u);
    CHECK(exits(fd, EXIT_MSR_WRITE, 0, 2) == 0 && vx(v)->msrs[1] == 0x0023001000000011ull,
          "WRMSR STAR reaches the entry's MSR list");

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

/** The caller's debugging: a single step, one over an instruction the
 * kernel runs, software and hardware breakpoints, interrupts held, and
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
    int before = warnings;

    m.refuse = HOST_FS_BASE;
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && warnings == before + 1, "a failed VMWRITE is reported after the exit");
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && warnings == before + 1, "once");
    m.refuse = 0;

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
    CHECK(run(fd) == 0 && rp->exit_reason == KVM_EXIT_FAIL_ENTRY &&
              rp->u.fail_entry.hardware_entry_failure_reason == ERR_CONTROLS,
          "VMLAUNCH's VMfailValid is KVM_EXIT_FAIL_ENTRY with its error");

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

/** Another processor, a state read between runs, and VMXON and VMPTRLD
 * failing. */
static void path_rows(int fd)
{
    int vmxons, invepts = m.invepts, invvpids = m.invvpids;

    host_cpu = 1;
    m.fc = 0;
    script(EXIT_HLT, 0, 1);
    CHECK(run(fd) == 0 && fields(fd)[HOST_GS_BASE] == 0xFFFFFFFF81001000ull,
          "on another processor the host's fields are its");
    CHECK(m.invepts == invepts + 1 && m.invvpids == invvpids + 1,
          "and the guest's translations are dropped there");
    CHECK(m.fc_writes == 1 && m.fc == 5, "an unlocked IA32_FEATURE_CONTROL is locked with VMX on");
    host_cpu = 0;
    script(EXIT_CPUID, 0, 2);
    script(EXIT_HLT, 0, 1);
    run(fd);

    vmxons = m.vmxons;
    CHECK(sregs_of(fd).efer == 0x500 && m.vmxons == vmxons + 1 && !m.on,
          "KVM_GET_SREGS reads the VMCS in a VMXON of its own");

    m.fail_vmxon = 1;
    CHECK(run(fd) == -EIO && m.cr4 == HOST_CR4_MODEL,
          "VMXON failing ends KVM_RUN with EIO, CR4 back");
    m.fail_vmxon = 0;
    m.fail_vmptrld = 1;
    CHECK(run(fd) == -EIO && !m.on, "VMPTRLD failing turns VMX off and ends KVM_RUN with EIO");
    m.fail_vmptrld = 0;
    m.cr0 = 0x80050013ull;
    CHECK(run(fd) == -EIO && m.gp == 0 && !m.on && m.cr4 == HOST_CR4_MODEL,
          "a host CR0 without NE ends KVM_RUN with EIO before VMXON could #GP");
    m.cr0 = 0x80050033ull;
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

/** A second vCPU's VPID, and a panic inside a run. */
static void vpid_panic_rows(int vm)
{
    struct kvm_vcpu *v;
    int fd = make_vcpu(vm, 1);

    v = vcpu_of(fd);
    CHECK(vx(v)->vpid == 2, "the second vCPU has VPID 2");
    script(EXIT_HLT, 0, 0)->panic = 1;
    if (setjmp(panic_jump) == 0) {
        fdio(fd, KVM_RUN, 0);
        CHECK(0, "the panic row returns through the hook");
    }
    CHECK(!m.on && m.cr4 == HOST_CR4_MODEL && !vmx_live[0] && m.ud == 0,
          "a panic in the run clears the VMCS, turns VMX off and puts CR4 back");
    irq_on = 1;
    bkl_depth = 1;
    v->mode = KVM_MODE_OUTSIDE;
    v->busy = 0;
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
    int runs;

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

/** SYSCALL outside 64-bit mode after its #UD, which Linux's emulator runs
 * for a guest whose CPUID vendor is AMD's, on a vCPU of its own on `vm`,
 * since a vCPU's CPUID is fixed once it has run. */
static void amd_syscall_rows(int vm)
{
    struct {
        struct kvm_msrs h;
        struct kvm_msr_entry e[1];
    } msrs;
    struct kvm_cpuid_entry2 *e = (struct kvm_cpuid_entry2 *)(buf.bytes + sizeof(buf.cpuid));
    struct kvm_sregs s;
    int fd = fdio(vm, KVM_CREATE_VCPU, 6);
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

    /* The panic row left its run without the host's state put back. */
    m.restores = m.launches + m.resumes;
    m.fpu_stores = m.fpu_loads;
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
    fdclose(fd);
    tsc_scale_rows(vm);
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
    ept_rows(fd);
    cr_rows(fd);
    insn_rows(fd);
    linux_rows(fd);
    event_rows(fd);
    debug_rows(fd);
    host_rows(fd);
    path_rows(fd);
    pae_rows(fd);
    vpid_panic_rows(vm);
    ts_rows(vm);
    hv_rows(vm);
    selftest_rows(vm);
    CHECK(m.ud == 0 && m.gp == 0 && m.bad_writes == 0 && m.bad_reads == 0,
          "no VMX instruction outside VMX operation and no field the VMCS lacks, over every row");
    fdclose(fd);
    fdclose(vm);
    CHECK(vpid_used[0] == 0, "the vCPUs' VPIDs are freed with the VM");
    CHECK(live == 5, "the VM's allocations are given back, but for the VMXON regions of the four "
                     "processors and the MSR bitmap, which the backend keeps");
    printf("kvm-vmx-hosttest: result: %s: %d failures\n", failures ? "fail" : "pass", failures);
    return failures ? 1 : 0;
}
