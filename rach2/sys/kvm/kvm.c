/*
 * kvm.c: /dev/kvm and the VM descriptor, the system and VM ioctls of
 * Linux's KVM API 12, and the answers of KVM_CHECK_EXTENSION.
 */

#include "kvm.h"

#include "../fs/devfs.h"
#include "../fs/fd.h"
#include "../fs/file.h"
#include "../klog.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"
#include "../mm/mm.h"
#include "../mm/usercopy.h"
#include "../proc/process.h"
#include "../proc/smp.h"
#include "../rtc.h"

#define O_CLOEXEC 02000000

/* The identity map's address before KVM_SET_IDENTITY_MAP_ADDR, Linux's
 * VMX_EPT_IDENTITY_PAGETABLE_ADDR. */
#define KVM_IDENT_ADDR_DEFAULT 0xFEFFC000ull

/* The least KVM_SET_NR_MMU_PAGES takes, Linux's KVM_MIN_ALLOC_MMU_PAGES. */
#define KVM_MIN_MMU_PAGES 64u

/* The machine-check capabilities a guest may be given, Linux's
 * KVM_MCE_CAP_SUPPORTED. */
#define KVM_MCE_CAP_SUPPORTED (MCG_CTL_P | MCG_SER_P)

const struct kvm_backend *kvm_backend;

/*
 * A VM's record, the VM itself with the address space of the process that
 * made it, whose record it holds (`mm_hold`): Linux answers EIO to an ioctl
 * of a VM or its vCPUs from any other address space, a forked child's
 * among them.
 */
struct kvm_vm_rec {
    struct kvm_vm vm;
    struct mm *mm;
    uint32_t nr_mmu_pages;
};

/** The record of `vm`. */
static struct kvm_vm_rec *vm_rec(struct kvm_vm *vm)
{
    return (struct kvm_vm_rec *)vm;
}

int kvm_vm_caller_ok(struct kvm_vm *vm)
{
    return current_proc && current_proc->mm == vm_rec(vm)->mm;
}

uint64_t kvm_host_ns(void)
{
    int64_t s, ns;

    rtc_monotonic(&s, &ns);
    return (uint64_t)s * 1000000000ull + (uint64_t)ns;
}

void kvm_vm_get(struct kvm_vm *vm)
{
    vm->refs++;
}

void kvm_vm_put(struct kvm_vm *vm)
{
    uint32_t i;

    if (--vm->refs != 0) {
        return;
    }
    for (i = 0; i < vm->nr_vcpus; i++) {
        kvm_vcpu_free(vm->vcpus[i]);
    }
    kvm_iobus_free(vm);
    kvm_irq_free(vm);
    kvm_tdp_free(vm);
    kvm_slots_free(vm);
    if (kvm_backend && kvm_backend->vm_free) {
        kvm_backend->vm_free(vm);
    }
    if (vm_rec(vm)->mm) {
        mm_unhold(vm_rec(vm)->mm);
    }
    kfree(vm_rec(vm));
}

int32_t kvm_check_extension(struct kvm_vm *vm, long cap)
{
    int32_t r;
    int cpus;

    (void)vm;
    switch (cap) {
    case KVM_CAP_EXT_CPUID:
    case KVM_CAP_SET_TSS_ADDR:
    case KVM_CAP_SET_IDENTITY_MAP_ADDR:
    case KVM_CAP_MP_STATE:
    case KVM_CAP_INTERNAL_ERROR_DATA:
    case KVM_CAP_VCPU_EVENTS:
    case KVM_CAP_DEBUGREGS:
    case KVM_CAP_INTR_SHADOW:
    case KVM_CAP_USER_NMI:
    case KVM_CAP_IMMEDIATE_EXIT:
    case KVM_CAP_ENABLE_CAP:
    case KVM_CAP_ENABLE_CAP_VM:
    case KVM_CAP_CHECK_EXTENSION_VM:
    case KVM_CAP_GET_MSR_FEATURES:
    case KVM_CAP_SET_BOOT_CPU_ID:
        return 1;
    case KVM_CAP_XSAVE:
    case KVM_CAP_XCRS:
        return kvm_xcr0_supported() != 0;
    case KVM_CAP_MCE:
        return KVM_MAX_MCE_BANKS;
    case KVM_CAP_ADJUST_CLOCK:
        return (int32_t)(KVM_CLOCK_REALTIME | KVM_CLOCK_HOST_TSC);
    case KVM_CAP_NR_VCPUS:
        cpus = smp_cpu_count();
        return cpus < KVM_MAX_VCPUS ? cpus : KVM_MAX_VCPUS;
    case KVM_CAP_MAX_VCPUS:
        return KVM_MAX_VCPUS;
    case KVM_CAP_MAX_VCPU_ID:
        return KVM_MAX_VCPU_IDS;
    default:
        break;
    }
    r = kvm_slots_check_extension(cap);
    if (r == 0) {
        r = kvm_irq_check_extension(cap);
    }
    if (r == 0) {
        r = kvm_iobus_check_extension(cap);
    }
    if (r == 0 && kvm_backend && kvm_backend->check_extension) {
        r = kvm_backend->check_extension(cap);
    }
    return r;
}

/** KVM_GET_MSR_INDEX_LIST and KVM_GET_MSR_FEATURE_INDEX_LIST, `feature`
 * naming the second: the count written back, then the indices when the
 * caller's room holds them. 0, -E2BIG or -EFAULT. */
static int32_t get_msr_list(uintptr_t arg, int feature)
{
    struct kvm_msr_list head;
    const uint32_t *list;
    uint32_t n, room;

    if (copy_from_user(&head, (const void __user *)arg, sizeof(head)) != 0) {
        return -EFAULT;
    }
    room = head.nmsrs;
    list = kvm_msr_list(feature, &n);
    head.nmsrs = n;
    if (copy_to_user((void __user *)arg, &head, sizeof(head)) != 0) {
        return -EFAULT;
    }
    if (room < n) {
        return -E2BIG;
    }
    if (copy_to_user((void __user *)(arg + sizeof(head)), list, n * sizeof(uint32_t)) != 0) {
        return -EFAULT;
    }
    return 0;
}

/** KVM_GET_MSRS of /dev/kvm: the feature MSRs the caller names, read in
 * order until one is not a feature MSR. The count read, -E2BIG for
 * KVM_MAX_MSR_ENTRIES or more, -ENOMEM or -EFAULT. */
static int32_t get_feature_msrs(uintptr_t arg)
{
    struct kvm_msrs head;
    struct kvm_msr_entry *ent;
    uint32_t i;
    int32_t r;

    if (copy_from_user(&head, (const void __user *)arg, sizeof(head)) != 0) {
        return -EFAULT;
    }
    if (head.nmsrs >= KVM_MAX_MSR_ENTRIES) {
        return -E2BIG;
    }
    ent = kmalloc(head.nmsrs * sizeof(*ent) + 1u);
    if (!ent) {
        return -ENOMEM;
    }
    r = -EFAULT;
    if (copy_from_user(ent, (const void __user *)(arg + sizeof(head)), head.nmsrs * sizeof(*ent)) !=
        0) {
        goto out;
    }
    for (i = 0; i < head.nmsrs; i++) {
        if (kvm_msr_feature(ent[i].index, &ent[i].data) != 0) {
            break;
        }
    }
    if (copy_to_user((void __user *)(arg + sizeof(head)), ent, head.nmsrs * sizeof(*ent)) != 0) {
        goto out;
    }
    r = (int32_t)i;
out:
    kfree(ent);
    return r;
}

/** KVM_GET_SUPPORTED_CPUID, or KVM_GET_EMULATED_CPUID for `emulated`,
 * which has no entries: the kernel emulates no instruction the processor
 * lacks. The entries and their count, which Linux writes back only when
 * they fit: 0, -E2BIG, -ENOMEM or -EFAULT. */
static int32_t get_cpuid(uintptr_t arg, int emulated)
{
    struct kvm_cpuid2 head;
    struct kvm_cpuid_entry2 *ent;
    int32_t n, r;

    if (copy_from_user(&head, (const void __user *)arg, sizeof(head)) != 0) {
        return -EFAULT;
    }
    if (head.nent < 1) {
        return -E2BIG;
    }
    if (head.nent > KVM_MAX_CPUID_ENTRIES) {
        head.nent = KVM_MAX_CPUID_ENTRIES;
    }
    ent = kzalloc(head.nent * sizeof(*ent));
    if (!ent) {
        return -ENOMEM;
    }
    n = emulated ? 0 : kvm_cpuid_supported(ent, head.nent);
    r = n;
    if (n >= 0) {
        head.nent = (uint32_t)n;
        r = 0;
        if (copy_to_user((void __user *)arg, &head, sizeof(head)) != 0 ||
            copy_to_user((void __user *)(arg + sizeof(head)), ent, (uint32_t)n * sizeof(*ent)) !=
                0) {
            r = -EFAULT;
        }
    }
    kfree(ent);
    return r;
}

static int32_t vm_ioctl(fd_entry_t *e, uint32_t request, uintptr_t arg);
static void vm_close(fd_entry_t *e);

static const struct file_ops vm_ops = {NULL, NULL, NULL, vm_ioctl, vm_close, NULL, NULL};

/** KVM_CREATE_VM of the machine type `type`, which on x86 is 0: a VM and
 * a descriptor of the caller's on it, "anon_inode:kvm-vm", read-write and
 * close-on-exec. The descriptor, -EINVAL for another type, -ENOMEM, the
 * backend's error, or `fd_alloc`'s. */
static int32_t create_vm(uintptr_t type)
{
    struct kvm_vm_rec *rec;
    struct kvm_vm *vm;
    fd_entry_t *e;
    int32_t r;
    int fd;

    if (type != 0) {
        return -EINVAL;
    }
    rec = kzalloc(sizeof(*rec));
    if (!rec) {
        return -ENOMEM;
    }
    vm = &rec->vm;
    vm->anon.name = "kvm-vm";
    vm->refs = 1;
    vm->ident_addr = KVM_IDENT_ADDR_DEFAULT;
    vm->clock_offset = -(int64_t)kvm_host_ns();
    rec->nr_mmu_pages = KVM_MIN_MMU_PAGES;
    r = kvm_slots_init(vm);
    if (r == 0) {
        r = kvm_tdp_init(vm);
    }
    if (r == 0 && kvm_backend->vm_init) {
        r = kvm_backend->vm_init(vm);
    }
    if (r == 0) {
        rec->mm = current_proc->mm;
        if (rec->mm) {
            mm_hold(rec->mm);
        }
        fd = fd_alloc(current_proc->fds);
        r = fd;
    }
    if (r < 0) {
        kvm_vm_put(vm);
        return r;
    }
    e = &current_proc->fds->entries[fd];
    e->type = FD_TYPE_ANON;
    e->ops = &vm_ops;
    e->priv = vm;
    e->flags = FD_FLAG_READ | FD_FLAG_WRITE | FD_FLAG_CLOEXEC;
    return fd;
}

/** The ioctls of /dev/kvm. */
static int32_t dev_ioctl(fd_entry_t *e, uint32_t request, uintptr_t arg)
{
    uint64_t mce = KVM_MCE_CAP_SUPPORTED;

    (void)e;
    switch (request) {
    case KVM_GET_API_VERSION:
        return arg ? -EINVAL : KVM_API_VERSION;
    case KVM_CREATE_VM:
        return create_vm(arg);
    case KVM_CHECK_EXTENSION:
        return kvm_check_extension(NULL, (long)arg);
    case KVM_GET_VCPU_MMAP_SIZE:
        return arg ? -EINVAL : (int32_t)(KVM_RUN_PAGES * 4096u);
    case KVM_GET_MSR_INDEX_LIST:
        return get_msr_list(arg, 0);
    case KVM_GET_MSR_FEATURE_INDEX_LIST:
        return get_msr_list(arg, 1);
    case KVM_GET_MSRS:
        return get_feature_msrs(arg);
    case KVM_GET_SUPPORTED_CPUID:
        return get_cpuid(arg, 0);
    case KVM_GET_EMULATED_CPUID:
        return get_cpuid(arg, 1);
    case KVM_X86_GET_MCE_CAP_SUPPORTED:
        return copy_to_user((void __user *)arg, &mce, sizeof(mce)) ? -EFAULT : 0;
    default:
        return -ENOTTY;
    }
}

/** KVM_GET_CLOCK: the guest's kvmclock, the host's monotonic clock and
 * the VM's offset, with the realtime and the host's TSC read beside it. */
static int32_t get_clock(struct kvm_vm *vm, uintptr_t arg)
{
    struct kvm_clock_data d;
    int64_t s, ns;

    k_memset(&d, 0, sizeof(d));
    d.clock = kvm_host_ns() + (uint64_t)vm->clock_offset;
    rtc_realtime(&s, &ns);
    d.realtime = (uint64_t)s * 1000000000ull + (uint64_t)ns;
    d.host_tsc = arch_cycles();
    d.flags = KVM_CLOCK_REALTIME | KVM_CLOCK_HOST_TSC;
    return copy_to_user((void __user *)arg, &d, sizeof(d)) ? -EFAULT : 0;
}

/**
 * KVM_SET_CLOCK: make the guest's kvmclock read `clock` now, or with
 * KVM_CLOCK_REALTIME, `clock` advanced by the realtime passed since the
 * caller's `realtime`, as Linux advances it when that is not in the
 * future. 0, -EINVAL for a flag Linux does not take, or -EFAULT.
 */
static int32_t set_clock(struct kvm_vm *vm, uintptr_t arg)
{
    struct kvm_clock_data d;
    uint64_t now, clock;
    int64_t s, ns;

    if (copy_from_user(&d, (const void __user *)arg, sizeof(d)) != 0) {
        return -EFAULT;
    }
    if (d.flags & ~KVM_CLOCK_VALID_FLAGS) {
        return -EINVAL;
    }
    clock = d.clock;
    now = kvm_host_ns();
    if (d.flags & KVM_CLOCK_REALTIME) {
        rtc_realtime(&s, &ns);
        if ((uint64_t)s * 1000000000ull + (uint64_t)ns > d.realtime) {
            clock += (uint64_t)s * 1000000000ull + (uint64_t)ns - d.realtime;
        }
    }
    vm->clock_offset = (int64_t)(clock - now);
    return 0;
}

/** KVM_ENABLE_CAP of a VM: KVM_CAP_SPLIT_IRQCHIP, irq.c's; -EINVAL for a
 * flag or any other capability, as Linux answers one it does not enable. */
static int32_t vm_enable_cap(struct kvm_vm *vm, uintptr_t arg)
{
    struct kvm_enable_cap cap;

    if (copy_from_user(&cap, (const void __user *)arg, sizeof(cap)) != 0) {
        return -EFAULT;
    }
    if (cap.flags) {
        return -EINVAL;
    }
    if (cap.cap == KVM_CAP_SPLIT_IRQCHIP) {
        return kvm_irq_enable_split(vm, &cap);
    }
    return -EINVAL;
}

/** The ioctls of a VM; -EIO from an address space other than its maker's. */
static int32_t vm_ioctl(fd_entry_t *e, uint32_t request, uintptr_t arg)
{
    struct kvm_vm *vm = (struct kvm_vm *)e->priv;
    struct kvm_userspace_memory_region region;
    uint64_t addr;

    if (!kvm_vm_caller_ok(vm)) {
        return -EIO;
    }
    switch (request) {
    case KVM_CREATE_VCPU:
        return kvm_vcpu_create(vm, (uint32_t)arg == arg ? (uint32_t)arg : KVM_MAX_VCPU_IDS);
    case KVM_CHECK_EXTENSION:
        return kvm_check_extension(vm, (long)arg);
    case KVM_SET_USER_MEMORY_REGION:
        if (copy_from_user(&region, (const void __user *)arg, sizeof(region)) != 0) {
            return -EFAULT;
        }
        return kvm_slots_set(vm, &region);
    case KVM_GET_DIRTY_LOG:
    case KVM_CLEAR_DIRTY_LOG:
        return kvm_dirty_ioctl(vm, request, arg);
    case KVM_SET_TSS_ADDR:
        /* The TSS takes three pages below 4 GiB. */
        if (arg > 0xFFFFFFFFull - 3u * 4096u + 1u) {
            return -EINVAL;
        }
        vm->tss_addr = arg;
        return 0;
    case KVM_SET_IDENTITY_MAP_ADDR:
        if (copy_from_user(&addr, (const void __user *)arg, sizeof(addr)) != 0) {
            return -EFAULT;
        }
        if (vm->nr_vcpus) {
            return -EBUSY;
        }
        vm->ident_addr = addr;
        return 0;
    case KVM_SET_NR_MMU_PAGES:
        if (arg < KVM_MIN_MMU_PAGES || arg > 0xFFFFFFFFu) {
            return -EINVAL;
        }
        vm_rec(vm)->nr_mmu_pages = (uint32_t)arg;
        return 0;
    case KVM_GET_NR_MMU_PAGES:
        return (int32_t)vm_rec(vm)->nr_mmu_pages;
    case KVM_GET_CLOCK:
        return get_clock(vm, arg);
    case KVM_SET_CLOCK:
        return set_clock(vm, arg);
    case KVM_SET_BOOT_CPU_ID:
        if (vm->nr_vcpus) {
            return -EBUSY;
        }
        if (arg > KVM_MAX_VCPU_IDS) {
            return -EINVAL;
        }
        vm->boot_cpu_id = (uint32_t)arg;
        return 0;
    case KVM_ENABLE_CAP:
        return vm_enable_cap(vm, arg);
    case KVM_CREATE_IRQCHIP:
    case KVM_IRQ_LINE:
    case KVM_IRQ_LINE_STATUS:
    case KVM_SET_GSI_ROUTING:
    case KVM_IRQFD:
    case KVM_SIGNAL_MSI:
        return kvm_irq_ioctl(vm, request, arg);
    case KVM_IOEVENTFD:
        return kvm_iobus_ioctl(vm, request, arg);
    default:
        return -ENOTTY;
    }
}

/** The last close of a VM's descriptor. */
static void vm_close(fd_entry_t *e)
{
    kvm_vm_put((struct kvm_vm *)e->priv);
    e->priv = NULL;
}

static const struct file_ops dev_ops = {NULL, NULL, NULL, dev_ioctl, NULL, NULL, NULL};

/* /dev/kvm, the misc device 10:232, read and written by everyone, as
 * udev's rule for the kvm group gives it on a machine with one user. */
static struct devfs_node kvm_node = {"kvm", 10, 232, 0666, &dev_ops, NULL, NULL};

void kvm_start(void)
{
    if (kvm_vmx.probe()) {
        kvm_backend = &kvm_vmx;
    } else if (kvm_svm.probe()) {
        kvm_backend = &kvm_svm;
    } else {
        klog(KLOG_INFO, "kvm", "none: the processor has neither VMX nor SVM");
        return;
    }
    if (devfs_node_register(&kvm_node) != 0) {
        klog(KLOG_WARN, "kvm", "failed: /dev/kvm not registered");
        kvm_backend = NULL;
        return;
    }
    klog(KLOG_INFO, "kvm", "ready: %s", kvm_backend->name);
}
