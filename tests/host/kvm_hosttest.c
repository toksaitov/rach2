/**
 * Check the KVM interface, /dev/kvm and its VM and vCPU descriptors,
 * against a fake backend.
 *
 * host-deps: rach2/sys/kvm/cpuid.c rach2/sys/kvm/msr.c rach2/sys/kvm/hyperv.c
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
 * required capabilities, the MSR and feature lists, the supported CPUID
 * taken by doubling, the vCPU and its mapping, KVM_SET_CPUID2, the
 * machine-check setup, the full put and get of the registers, to KVM_RUN,
 * where the refusing backend's error comes back; each exit kind through the
 * run page, with port and MMIO reads answered on the next KVM_RUN, a HLT
 * going to the caller with no local APIC in the kernel, immediate_exit, a
 * signal, a kick from another processor and a triple fault; the big lock
 * let go and interrupts off for the entry; the signal mask swapped for the
 * run; the refusals of the state ioctls Linux makes; EIO from another
 * address space; and every allocation given back when the last reference
 * to the VM goes, the mapping's among them.
 */
#include "kvm/kvm.c"
#include "kvm/vcpu.c"
#include "lib/kstring.h"

extern int printf(const char *format, ...);
extern void *malloc(unsigned long n);
extern void *aligned_alloc(unsigned long align, unsigned long n);
extern void free(void *p);

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
static fd_table_t table = {entries, 64, 1, 0, NULL};
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

/* The clocks: the monotonic and real nanoseconds, and the TSC. */
static uint64_t mono_ns = 5000000000ull, real_ns = 1800000000000000000ull, tsc = 1000000;

void rtc_monotonic(int64_t *s, int64_t *ns)
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

/* The big lock's depth on this processor, the interrupt flag, the
 * processors kicked and a pending signal. */
static int bkl_depth = 1, irq_on = 1, kicked = -1, sig_pending;

int bkl_drop(void)
{
    int d = bkl_depth;

    CHECK(!irq_on, "the big lock is let go with interrupts off");
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
    kicked = cpu;
}

int signal_pending(void)
{
    return sig_pending;
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
    case 0x80000007u:
        r[3] = 0x100;
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

/* The fake backend's mode: refuse every entry, or report the exits of
 * `script` in turn. */
static int refusing = 1;
static struct kvm_exit script[16];
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
    return refusing ? -EOPNOTSUPP : 0;
}

/** The next exit of the script. */
static int fake_exit(struct kvm_vcpu *v)
{
    CHECK(irq_on && bkl_depth == 1, "an exit is handled with interrupts on and the big lock");
    if (script_at == script_len) {
        return -EIO;
    }
    v->exit = script[script_at++];
    v->if_flag = 1;
    v->irq_allowed = 1;
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
    (void)v;
    injects++;
}

/** The capabilities a backend makes true: single-stepping and guest
 * debugging. */
static int32_t fake_backend_cap(long cap)
{
    return cap == KVM_CAP_X86_ROBUST_SINGLESTEP || cap == KVM_CAP_SET_GUEST_DEBUG;
}

const struct kvm_backend kvm_vmx = {"vmx",       fake_probe,       NULL,      NULL,          NULL,
                                    NULL,        fake_enter,       fake_exit, fake_complete, NULL,
                                    fake_inject, fake_backend_cap, NULL};
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

int kvm_guest_phys_rw(struct kvm_vcpu *v, uint64_t gpa, void *buf, uint32_t n, int write)
{
    (void)v;
    (void)gpa;
    (void)buf;
    (void)n;
    (void)write;
    return 1;
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

void kvm_queue_exception(struct kvm_vcpu *v, uint8_t nr, int has_error, uint32_t error)
{
    (void)v;
    (void)nr;
    (void)has_error;
    (void)error;
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

void arch_relax(void)
{}

/** The compare-and-exchange, on one processor. */
unsigned int arch_atomic_cmpxchg(volatile unsigned int *p, unsigned int old, unsigned int value)
{
    unsigned int cur = *p;

    if (cur == old) {
        *p = value;
    }
    return cur;
}

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
    CHECK(KVM_SET_USER_MEMORY_REGION2 == 0x40A0AE49u &&
              sizeof(struct kvm_userspace_memory_region2) == 160,
          "KVM_SET_USER_MEMORY_REGION2");
    CHECK(KVM_GET_XSAVE == 0x9000AEA4u && KVM_GET_XCRS == 0x8188AEA6u,
          "KVM_GET_XSAVE and KVM_GET_XCRS");
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
static uint32_t put_msrs[128];
static int n_put;

/** Add `index` to QEMU's list of MSRs to put. */
static void put_msr(uint32_t index)
{
    put_msrs[n_put++] = index;
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
    CHECK(dev(KVM_CHECK_EXTENSION, KVM_CAP_COALESCED_MMIO) == 0, "a capability not offered is 0");
    CHECK(dev(KVM_CHECK_EXTENSION, 100000) == 0, "an unknown capability is 0");
    CHECK(dev(KVM_CHECK_EXTENSION, KVM_CAP_SPLIT_IRQCHIP) == 1, "the split irqchip is offered");
}

/** kvm_get_supported_msrs: the index list, its count first, which starts
 * QEMU's list of MSRs to put; then the feature MSRs. */
static void startup_msr_lists(void)
{
    uint32_t i, nmsrs;
    int32_t r;

    buf.list.nmsrs = 0;
    r = dev(KVM_GET_MSR_INDEX_LIST, (uintptr_t)&buf);
    nmsrs = buf.list.nmsrs;
    CHECK(r == -E2BIG && nmsrs > 10, "KVM_GET_MSR_INDEX_LIST gives its count with -E2BIG");
    r = dev(KVM_GET_MSR_INDEX_LIST, (uintptr_t)&buf);
    CHECK(r == 0 && buf.list.nmsrs == nmsrs, "KVM_GET_MSR_INDEX_LIST with room");
    n_put = 0;
    for (i = 0; i < nmsrs; i++) {
        put_msr(MSR_INDICES[i]);
    }
    buf.list.nmsrs = 4;
    r = dev(KVM_GET_MSR_FEATURE_INDEX_LIST, (uintptr_t)&buf);
    CHECK(r == 0 && buf.list.nmsrs == 1 && MSR_INDICES[0] == 0x8B,
          "the feature list names UCODE_REV");
    buf.msrs.nmsrs = 2;
    MSR_ENTRIES[0].index = 0x8B;
    MSR_ENTRIES[1].index = 0x10A;
    r = dev(KVM_GET_MSRS, (uintptr_t)&buf);
    CHECK(r == 1 && MSR_ENTRIES[0].data == 0x100000000ull,
          "KVM_GET_MSRS of /dev/kvm reads feature MSRs up to the first that is not");
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
        CHECK((e->ebx & 0x20u) && !(e->edx & (1u << 26)), "leaf 7 keeps AVX2, drops SPEC_CTRL");
        CHECK(e->flags == KVM_CPUID_FLAG_SIGNIFCANT_INDEX, "leaf 7 is indexed");
    }
    if (e->function == 0xD && e->index == 0) {
        CHECK(e->eax == 7 && e->ebx == 832, "leaf 0xD offers x87, SSE and AVX in 832 bytes");
    }
    if (e->function == 0xA) {
        CHECK(e->eax == 0, "no performance counters");
    }
    CHECK(e->function < 0x40000000u || e->function >= 0x80000000u, "no paravirtual leaf");
}

/** try_get_cpuid: KVM_GET_SUPPORTED_CPUID from one entry, doubling while
 * it does not fit. Returns the entries, left in `buf`. */
static uint32_t startup_cpuid(void)
{
    uint32_t i, max, n;
    int32_t r;

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

/** Check the mapping of the vCPU on `vcpu`: its two pages, the page past
 * them, and the VM reference the mapping holds. */
static void check_mapping(int vcpu)
{
    struct kvm_vcpu *v = (struct kvm_vcpu *)entries[vcpu].priv;
    uintptr_t phys = 0;

    CHECK(map_ops->page(map_obj, 0, &phys) == 0 && phys == (uintptr_t)v->run,
          "page 0 of the mapping is the run page");
    CHECK(map_ops->page(map_obj, 1, &phys) == 0 && phys == (uintptr_t)v->pio,
          "page 1 is the port I/O page");
    CHECK(map_ops->page(map_obj, 2, &phys) == -1, "page 2 is past the mapping: SIGBUS");
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
    CHECK(f->fill(f, 2, (uintptr_t)frame) == -1, "and a page past them raises SIGBUS");
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
    CHECK(dev(KVM_GET_VCPU_MMAP_SIZE, 0) == 8192, "the mapping is the run page and the I/O page");
    k_memset(&vma, 0, sizeof(vma));
    vma.end = 8192;
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
    CHECK(bkl_depth == 1 && irq_on, "KVM_RUN takes the big lock back and turns interrupts on");
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
    struct kvm_exit *x = &script[script_len++];

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
    entries_made = 0;
    CHECK(fdio(fd, KVM_RUN, 0) == -EINTR && run->exit_reason == KVM_EXIT_INTR,
          "a signal ends KVM_RUN with KVM_EXIT_INTR");
    CHECK(entries_made == 0, "the signal is seen before the entry");
    sig_pending = 0;
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
    CHECK(fdio(vm, KVM_GET_CLOCK, (uintptr_t)&clk) == 0 && clk.clock == 0 &&
              clk.flags == (KVM_CLOCK_REALTIME | KVM_CLOCK_HOST_TSC),
          "the kvmclock starts at 0 with the VM");
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

/** Run the rows; 0 when every check passed, 1 otherwise. */
int main(void)
{
    int vm, vcpu;

    caller.mm = &caller_mm;
    caller.fds = &table;
    abi_rows();
    hv_abi_rows();
    kvm_start();
    CHECK(kvm_backend == &kvm_vmx && registered && registered->major == 10 &&
              registered->minor == 232 && registered->mode == 0666,
          "/dev/kvm is 10:232, read and written by everyone, on the VMX backend");
    CHECK(registered->name[0] == 'k' && registered->name[3] == '\0', "the node is named kvm");
    vcpu = qemu_startup(&vm);
    exit_rows(vcpu);
    state_rows(vcpu);
    vm_rows(vm, vcpu);
    printf("kvm-hosttest: result: %s: %d failures\n", failures ? "fail" : "pass", failures);
    return failures ? 1 : 0;
}
