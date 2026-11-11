/*
 * kvm.c: /dev/kvm and the VM descriptor, the system and VM ioctls of
 * Linux's KVM API 12, and the answers of KVM_CHECK_EXTENSION.
 */

#include "kvm.h"

#include "../arch/x86/kcpu.h"
#include "../fs/devfs.h"
#include "../fs/fd.h"
#include "../fs/file.h"
#include "../fs/synthfs.h"
#include "../fs/sysfs.h"
#include "../klog.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"
#include "../mm/mm.h"
#include "../mm/usercopy.h"
#include "../proc/process.h"
#include "../proc/smp.h"
#include "../rtc.h"

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

static uint32_t live_resources[KVM_COUNT_TOTAL];

/* The live VMs, newest first, linked by `next_vm`. */
static struct kvm_vm *live_vms;

/** Count a resource at allocation or final release, under the big lock. */
void kvm_count(enum kvm_resource resource, int change)
{
    live_resources[resource] += (uint32_t)change;
}

/** Read a live resource count under the big lock. */
uint32_t kvm_resource_count(enum kvm_resource resource)
{
    return live_resources[resource];
}

/** The record of `vm`. */
static struct kvm_vm_rec *vm_rec(struct kvm_vm *vm)
{
    return (struct kvm_vm_rec *)vm;
}

int kvm_vm_caller_ok(struct kvm_vm *vm)
{
    return current_proc && current_proc->mm == vm_rec(vm)->mm;
}

struct mm *kvm_vm_mm(struct kvm_vm *vm)
{
    return vm_rec(vm)->mm;
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

struct kvm_vm *kvm_vm_by_eventfd(const struct eventfd *ev)
{
    struct kvm_vm *vm;

    for (vm = live_vms; vm; vm = vm->next_vm) {
        if (kvm_iobus_has_eventfd(vm, ev)) {
            return vm;
        }
    }
    return NULL;
}

void kvm_vm_put(struct kvm_vm *vm)
{
    struct kvm_vm **p;
    uint32_t i;

    if (--vm->refs != 0) {
        return;
    }
    for (p = &live_vms; *p; p = &(*p)->next_vm) {
        if (*p == vm) {
            *p = vm->next_vm;
            break;
        }
    }
    /* The maps of guest pages into programs first, which reach the slots. */
    kvm_gpamap_vm_end(vm);
    /* The irqfds first, whose hooks reach the vCPUs' APICs. */
    kvm_irq_free(vm);
    kvm_iobus_free(vm);
    for (i = 0; i < vm->nr_vcpus; i++) {
        kvm_vcpu_free(vm->vcpus[i]);
    }
    if (vm->ring) {
        kfree_pages(vm->ring, 1);
    }
    kvm_hv_vm_free(vm);
    kvm_msr_free_filter(vm);
    kvm_pmu_vm_free(vm);
    kvm_tdp_free(vm);
    kvm_slots_free(vm);
    if (kvm_backend && kvm_backend->vm_free) {
        kvm_backend->vm_free(vm);
    }
    if (vm_rec(vm)->mm) {
        mm_unhold(vm_rec(vm)->mm);
    }
    kvm_count(KVM_COUNT_VM, -1);
    kfree(vm_rec(vm));
}

/** Whether the host's processor snoops its own caches, CPUID's SS, which
 * lets a guest's memory type stand without a flush, as Linux requires
 * before it offers KVM_X86_QUIRK_IGNORE_GUEST_PAT. */
static int host_self_snoop(void)
{
    uint32_t a = 0, b = 0, c = 0, d = 0;

    kcpu_id(1, &a, &b, &c, &d);
    return (d >> 27) & 1u;
}

/** The quirks KVM_CAP_DISABLE_QUIRKS2 may turn off, Linux's
 * kvm_caps.supported_quirks: all of them, but on VMX the ignored guest PAT
 * needs a processor that snoops itself. */
static uint32_t quirks_supported(void)
{
    uint32_t q = KVM_X86_VALID_QUIRKS;

    if (kvm_backend == &kvm_vmx && !host_self_snoop()) {
        q &= ~KVM_X86_QUIRK_IGNORE_GUEST_PAT;
    }
    return q;
}

/** The quirks a new VM has turned off, Linux's inapplicable quirks: of the
 * two that hold on one backend alone, the one that is the other's, the
 * cleared CD and NW being SVM's and the ignored guest PAT VMX's. */
static uint32_t quirks_inapplicable(void)
{
    uint32_t mine =
        kvm_backend == &kvm_vmx ? KVM_X86_QUIRK_IGNORE_GUEST_PAT : KVM_X86_QUIRK_CD_NW_CLEARED;

    return KVM_X86_CONDITIONAL_QUIRKS & ~mine & quirks_supported();
}

int32_t kvm_check_extension(struct kvm_vm *vm, long cap)
{
    int32_t r;
    int cpus;

    (void)vm;
    switch (cap) {
    case KVM_CAP_SET_GUEST_DEBUG:
    case KVM_CAP_EXIT_ON_EMULATION_FAILURE:
    case KVM_CAP_MSR_PLATFORM_INFO:
    case KVM_CAP_SYS_ATTRIBUTES:
    case KVM_CAP_VCPU_ATTRIBUTES:
    case KVM_CAP_STEAL_TIME:
    case KVM_CAP_DISABLE_QUIRKS:
    case KVM_CAP_BINARY_STATS_FD:
    case KVM_CAP_PRE_FAULT_MEMORY:
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
    case KVM_CAP_DEVICE_CTRL:
    case KVM_CAP_CHECK_EXTENSION_VM:
    case KVM_CAP_GET_MSR_FEATURES:
    case KVM_CAP_SET_BOOT_CPU_ID:
    case KVM_CAP_PMU_EVENT_FILTER:
    case KVM_CAP_PMU_EVENT_MASKED_EVENTS:
    case KVM_CAP_GET_TSC_KHZ:
    case KVM_CAP_KVMCLOCK_CTRL:
    case KVM_CAP_ENFORCE_PV_FEATURE_CPUID:
    case KVM_CAP_X86_USER_SPACE_MSR:
    case KVM_CAP_X86_MSR_FILTER:
        return 1;
    case KVM_CAP_X2APIC_API:
        return (int32_t)KVM_X2APIC_API_VALID_FLAGS;
    case KVM_CAP_X86_DISABLE_EXITS:
        /* PAUSE never exits here, and HLT need not; MWAIT is not offered,
         * and the C-state residency MSRs are not passed through. */
        return (int32_t)(KVM_X86_DISABLE_EXITS_PAUSE | KVM_X86_DISABLE_EXITS_HLT);
    case KVM_CAP_PMU_CAPABILITY:
        return KVM_PMU_CAP_DISABLE;
    case KVM_CAP_XSAVE:
    case KVM_CAP_XCRS:
        return kvm_xcr0_supported() != 0;
    case KVM_CAP_MCE:
        return KVM_MAX_MCE_BANKS;
    case KVM_CAP_ADJUST_CLOCK:
        return (int32_t)KVM_CLOCK_VALID_FLAGS;
    case KVM_CAP_NR_VCPUS:
        cpus = smp_cpu_count();
        return cpus < KVM_MAX_VCPUS ? cpus : KVM_MAX_VCPUS;
    case KVM_CAP_MAX_VCPUS:
        return KVM_MAX_VCPUS;
    case KVM_CAP_MAX_VCPU_ID:
        return KVM_MAX_VCPU_IDS;
    case KVM_CAP_SET_GUEST_DEBUG2:
        return (int32_t)KVM_GUESTDBG_VALID_MASK;
    case KVM_CAP_DISABLE_QUIRKS2:
        return (int32_t)quirks_supported();
    case KVM_CAP_SYNC_REGS:
        return (int32_t)KVM_SYNC_X86_VALID_FIELDS;
    case KVM_CAP_X86_APIC_BUS_CYCLES_NS:
        return KVM_APIC_BUS_NS_DEFAULT;
    case KVM_CAP_TSC_CONTROL:
    case KVM_CAP_VM_TSC_CONTROL:
        return kvm_tsc_control();
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
    if (r == 0) {
        r = kvm_hv_check_extension(cap);
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
 * the features whose instructions the kernel runs for a processor that
 * lacks them. The entries and their count, which Linux writes back only
 * when they fit: 0, -E2BIG, -ENOMEM or -EFAULT. */
static int32_t get_supported_cpuid(uintptr_t arg, int emulated)
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
    n = emulated ? kvm_cpuid_emulated(ent, head.nent) : kvm_cpuid_supported(ent, head.nent);
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

static const struct file_ops vm_ops = {NULL, NULL, NULL, vm_ioctl, vm_close, NULL, NULL, 0};

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
    kvm_count(KVM_COUNT_VM, 1);
    vm = &rec->vm;
    vm->anon.name = "kvm-vm";
    kvm_stats_id(vm->stats_id, NULL);
    vm->refs = 1;
    vm->ident_addr = KVM_IDENT_ADDR_DEFAULT;
    vm->apic_bus_ns = KVM_APIC_BUS_NS_DEFAULT;
    vm->disabled_quirks = quirks_inapplicable();
    vm->default_tsc_khz = (uint32_t)kvm_tsc_khz();
    kvm_clock_vm_init(vm);
    rec->nr_mmu_pages = KVM_MIN_MMU_PAGES;
    vm->ring = kalloc_pages(1, NULL);
    if (vm->ring) {
        k_memset(vm->ring, 0, 4096u);
    }
    r = vm->ring ? kvm_slots_init(vm) : -ENOMEM;
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
    vm->next_vm = live_vms;
    live_vms = vm;
    e = fd_slot(current_proc->fds, fd);
    e->type = FD_TYPE_ANON;
    e->ops = &vm_ops;
    e->priv = vm;
    e->flags = FD_FLAG_READ | FD_FLAG_WRITE | FD_FLAG_CLOEXEC;
    fd_install(current_proc->fds, fd);
    return fd;
}

/** KVM_GET_DEVICE_ATTR and KVM_HAS_DEVICE_ATTR of /dev/kvm, `get` naming
 * the first, as Linux's kvm_x86_dev_get_attr: group 0 has
 * KVM_X86_XCOMP_GUEST_SUPP, the XCR0 bits a guest may have. 0, -ENXIO for
 * another group or attribute, or -EFAULT. */
static int32_t dev_attr(uintptr_t arg, int get)
{
    struct kvm_device_attr attr;
    uint64_t value = kvm_xcr0_supported();

    if (copy_from_user(&attr, (const void __user *)arg, sizeof(attr)) != 0) {
        return -EFAULT;
    }
    if (attr.group != 0 || attr.attr != KVM_X86_XCOMP_GUEST_SUPP) {
        return -ENXIO;
    }
    if (get && copy_to_user((void __user *)(uintptr_t)attr.addr, &value, sizeof(value)) != 0) {
        return -EFAULT;
    }
    return 0;
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
        return arg ? -EINVAL : (int32_t)(KVM_MMAP_PAGES * 4096u);
    case KVM_GET_MSR_INDEX_LIST:
        return get_msr_list(arg, 0);
    case KVM_GET_MSR_FEATURE_INDEX_LIST:
        return get_msr_list(arg, 1);
    case KVM_GET_MSRS:
        return get_feature_msrs(arg);
    case KVM_GET_SUPPORTED_CPUID:
        return get_supported_cpuid(arg, 0);
    case KVM_GET_EMULATED_CPUID:
        return get_supported_cpuid(arg, 1);
    case KVM_X86_GET_MCE_CAP_SUPPORTED:
        return copy_to_user((void __user *)arg, &mce, sizeof(mce)) ? -EFAULT : 0;
    case KVM_GET_SUPPORTED_HV_CPUID:
        return kvm_hv_get_cpuid(NULL, arg);
    case KVM_GET_DEVICE_ATTR:
        return dev_attr(arg, 1);
    case KVM_HAS_DEVICE_ATTR:
        return dev_attr(arg, 0);
    default:
        return -ENOTTY;
    }
}

/**
 * KVM_ENABLE_CAP of a VM, as Linux 6.12's kvm_vm_ioctl_enable_cap:
 * KVM_CAP_SPLIT_IRQCHIP, irq.c's; KVM_CAP_MANUAL_DIRTY_LOG_PROTECT2,
 * dirty.c's; KVM_CAP_X86_DISABLE_EXITS, which may come after vCPUs are
 * made and stops HLT's exits for those made after it, -EINVAL for an exit
 * Linux does not name; KVM_CAP_DISABLE_QUIRKS, and KVM_CAP_DISABLE_QUIRKS2,
 * -EINVAL for a quirk it does not name; KVM_CAP_MAX_VCPU_ID, -EINVAL for a
 * bound above KVM_MAX_VCPU_IDS, below the boot vCPU's id or other than one
 * set before; KVM_CAP_EXIT_ON_EMULATION_FAILURE, -EINVAL for a bit other
 * than 0; KVM_CAP_MSR_PLATFORM_INFO; KVM_CAP_X86_USER_SPACE_MSR, the
 * reasons of a guest's failed RDMSR or WRMSR that go to the caller,
 * -EINVAL for an unknown one; KVM_CAP_X2APIC_API, whose flags stay set
 * once set, -EINVAL for an unknown one; and KVM_CAP_X86_APIC_BUS_CYCLES_NS,
 * -EINVAL for 0, a length whose longest timer count overflows 64 bits of
 * ns, or a VM with a vCPU, and -ENXIO without the local APIC in the kernel.
 * -EINVAL for a flag or any other capability, as Linux answers one it does
 * not enable; -EFAULT for a bad pointer.
 */
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
    if (cap.cap == KVM_CAP_MANUAL_DIRTY_LOG_PROTECT2) {
        return kvm_dirty_enable_manual(vm, &cap);
    }
    if (cap.cap == KVM_CAP_PMU_CAPABILITY) {
        return kvm_pmu_enable(vm, &cap);
    }
    if (cap.cap == KVM_CAP_X86_DISABLE_EXITS) {
        if (cap.args[0] & ~(uint64_t)KVM_X86_DISABLE_VALID_EXITS) {
            return -EINVAL;
        }
        vm->disable_exits |= (uint32_t)cap.args[0];
        return 0;
    }
    switch (cap.cap) {
    case KVM_CAP_DISABLE_QUIRKS2:
        if (cap.args[0] & ~(uint64_t)quirks_supported()) {
            return -EINVAL;
        }
        /* fall through */
    case KVM_CAP_DISABLE_QUIRKS:
        vm->disabled_quirks |= (uint32_t)cap.args[0] & quirks_supported();
        return 0;
    case KVM_CAP_MAX_VCPU_ID:
        /* Once, before the first vCPU, and never below the boot vCPU's
         * id; the same bound again is taken. */
        if (cap.args[0] > KVM_MAX_VCPU_IDS || vm->boot_cpu_id > cap.args[0]) {
            return -EINVAL;
        }
        if (vm->max_vcpu_ids == cap.args[0]) {
            return 0;
        }
        if (vm->max_vcpu_ids) {
            return -EINVAL;
        }
        vm->max_vcpu_ids = (uint32_t)cap.args[0];
        return 0;
    case KVM_CAP_EXIT_ON_EMULATION_FAILURE:
        if (cap.args[0] & ~1ull) {
            return -EINVAL;
        }
        vm->exit_on_emulation_error = (int)cap.args[0];
        return 0;
    case KVM_CAP_MSR_PLATFORM_INFO:
        vm->platform_info_hidden = cap.args[0] == 0;
        return 0;
    case KVM_CAP_X86_USER_SPACE_MSR:
        if (cap.args[0] & ~(uint64_t)KVM_MSR_EXIT_REASON_VALID_MASK) {
            return -EINVAL;
        }
        vm->user_space_msr = (uint32_t)cap.args[0];
        return 0;
    case KVM_CAP_X2APIC_API:
        if (cap.args[0] & ~(uint64_t)KVM_X2APIC_API_VALID_FLAGS) {
            return -EINVAL;
        }
        if (cap.args[0] & KVM_X2APIC_API_USE_32BIT_IDS) {
            vm->x2apic_format = 1;
        }
        if (cap.args[0] & KVM_X2APIC_API_DISABLE_BROADCAST_QUIRK) {
            vm->x2apic_no_broadcast = 1;
        }
        return 0;
    case KVM_CAP_X86_APIC_BUS_CYCLES_NS:
        /* A count of 2^32 - 1 at TDCR's divisor 128 must fit 64 bits of
         * ns, as Linux guards its tmict_to_ns. */
        if (cap.args[0] == 0 || cap.args[0] > ~0ull / (0xFFFFFFFFull * 128u)) {
            return -EINVAL;
        }
        if (vm->irqchip == KVM_IRQCHIP_NONE) {
            return -ENXIO;
        }
        if (vm->nr_vcpus) {
            return -EINVAL;
        }
        vm->apic_bus_ns = cap.args[0];
        return 0;
    default:
        return -EINVAL;
    }
}

/** KVM_SET_TSC_KHZ of a VM, as Linux's: the rate its later vCPUs start
 * at, which is not tried on a vCPU here, and which a VM with vCPUs takes
 * too. 0, or -EINVAL for a rate `kvm_tsc_khz_arg` refuses. */
static int32_t vm_set_tsc_khz(struct kvm_vm *vm, uintptr_t arg)
{
    uint32_t khz = kvm_tsc_khz_arg(arg);

    if (khz == 0) {
        return -EINVAL;
    }
    vm->default_tsc_khz = khz;
    return 0;
}

/** KVM_SET_USER_MEMORY_REGION and KVM_SET_USER_MEMORY_REGION2, by
 * `request`, which set the same slot: KVM_MEM_GUEST_MEMFD is a flag no VM
 * here takes, as on Linux for a VM without private memory. `kvm_slots_set`'s
 * result, or -EFAULT. */
static int32_t vm_set_region(struct kvm_vm *vm, uint32_t request, uintptr_t arg)
{
    struct kvm_userspace_memory_region2 region2;
    uint32_t size = sizeof(region2);

    if (request == KVM_SET_USER_MEMORY_REGION) {
        size = sizeof(region2.region);
    }
    if (copy_from_user(&region2, (const void __user *)arg, size) != 0) {
        return -EFAULT;
    }
    return kvm_slots_set(vm, &region2.region);
}

/** The ioctls of a VM; -EIO from an address space other than its maker's. */
static int32_t vm_ioctl(fd_entry_t *e, uint32_t request, uintptr_t arg)
{
    struct kvm_vm *vm = (struct kvm_vm *)e->priv;
    uint64_t addr;

    if (!kvm_vm_caller_ok(vm)) {
        return -EIO;
    }
    switch (request) {
    case KVM_CREATE_DEVICE:
        return kvm_vfio_create(vm, arg);
    case KVM_GET_STATS_FD:
        return kvm_stats_fd(vm, NULL);
    case KVM_SET_TSC_KHZ:
        return vm_set_tsc_khz(vm, arg);
    case KVM_GET_TSC_KHZ:
        return (int32_t)vm->default_tsc_khz;
    case KVM_CREATE_VCPU:
        return kvm_vcpu_create(vm, (uint32_t)arg == arg ? (uint32_t)arg : KVM_MAX_VCPU_IDS);
    case KVM_CHECK_EXTENSION:
        return kvm_check_extension(vm, (long)arg);
    case KVM_SET_USER_MEMORY_REGION:
    case KVM_SET_USER_MEMORY_REGION2:
        return vm_set_region(vm, request, arg);
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
        if (arg < KVM_MIN_MMU_PAGES) {
            return -EINVAL;
        }
#if defined(__x86_64__)
        if (arg > 0xFFFFFFFFu) {
            return -EINVAL;
        }
#endif
        vm_rec(vm)->nr_mmu_pages = (uint32_t)arg;
        return 0;
    case KVM_GET_NR_MMU_PAGES:
        return (int32_t)vm_rec(vm)->nr_mmu_pages;
    case KVM_GET_CLOCK:
        return kvm_clock_get(vm, arg);
    case KVM_SET_CLOCK:
        return kvm_clock_set(vm, arg);
    case KVM_SET_BOOT_CPU_ID:
        if (vm->nr_vcpus) {
            return -EBUSY;
        }
        if (arg > KVM_MAX_VCPU_IDS || (vm->max_vcpu_ids && arg > vm->max_vcpu_ids)) {
            return -EINVAL;
        }
        vm->boot_cpu_id = (uint32_t)arg;
        return 0;
    case KVM_ENABLE_CAP:
        return vm_enable_cap(vm, arg);
    case KVM_SET_PMU_EVENT_FILTER:
        return kvm_pmu_filter(vm, arg);
    case KVM_CREATE_IRQCHIP:
    case KVM_GET_IRQCHIP:
    case KVM_SET_IRQCHIP:
    case KVM_CREATE_PIT:
    case KVM_CREATE_PIT2:
    case KVM_GET_PIT:
    case KVM_SET_PIT:
    case KVM_GET_PIT2:
    case KVM_SET_PIT2:
    case KVM_REINJECT_CONTROL:
    case KVM_IRQ_LINE:
    case KVM_IRQ_LINE_STATUS:
    case KVM_SET_GSI_ROUTING:
    case KVM_IRQFD:
    case KVM_SIGNAL_MSI:
        return kvm_irq_ioctl(vm, request, arg);
    case KVM_IOEVENTFD:
    case KVM_REGISTER_COALESCED_MMIO:
    case KVM_UNREGISTER_COALESCED_MMIO:
        return kvm_iobus_ioctl(vm, request, arg);
    case KVM_HYPERV_EVENTFD:
        return kvm_hv_eventfd(vm, arg);
    case KVM_X86_SET_MSR_FILTER:
        return kvm_msr_set_filter(vm, arg);
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

void *kvm_vm_irq_hold(const struct fd_entry *e)
{
    struct kvm_vm *vm;

    if (e->type != FD_TYPE_ANON || e->ops != &vm_ops || !e->priv) {
        return NULL;
    }
    vm = (struct kvm_vm *)e->priv;
    kvm_vm_get(vm);
    return vm;
}

/** Whether `counts`, `n` of them, holds a nonzero count. */
static int any_count(const uint32_t *counts, uint32_t n)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        if (counts[i]) {
            return 1;
        }
    }
    return 0;
}

int kvm_vm_irq_row(void *arg, uint32_t *pos, struct kvm_irq_row *row)
{
    struct kvm_vm *vm = (struct kvm_vm *)arg;
    uint32_t at, i, vec;

    k_memset(row, 0, sizeof(*row));
    row->pin = 1;
    for (at = *pos; at < KVM_IOAPIC_NUM_PINS; at++) {
        if (kvm_ioapic_counts(vm, at, row->counts) != 0) {
            at = KVM_IOAPIC_NUM_PINS;
            break;
        }
        if (any_count(row->counts, KVM_PIN_STEPS)) {
            row->index = at;
            *pos = at + 1u;
            return 1;
        }
    }
    row->pin = 0;
    for (at -= KVM_IOAPIC_NUM_PINS; at / 256u < vm->nr_vcpus; at++) {
        i = at / 256u;
        vec = at % 256u;
        if (any_count(vm->vcpus[i]->irq_counts[vec], KVM_VEC_STEPS)) {
            row->vcpu = i;
            row->index = vec;
            k_memcpy(row->counts, vm->vcpus[i]->irq_counts[vec], sizeof(row->counts));
            *pos = KVM_IOAPIC_NUM_PINS + at + 1u;
            return 1;
        }
    }
    *pos = KVM_IOAPIC_NUM_PINS + at;
    return 0;
}

void kvm_vm_irq_done(void *vm)
{
    kvm_vm_put((struct kvm_vm *)vm);
}

const char *kvm_vm_irqchip_name(void *arg)
{
    struct kvm_vm *vm = (struct kvm_vm *)arg;

    if (vm->irqchip == KVM_IRQCHIP_KERNEL) {
        return "kernel";
    }
    return vm->irqchip == KVM_IRQCHIP_SPLIT ? "split" : "none";
}

static const struct file_ops dev_ops = {NULL, NULL, NULL, dev_ioctl, NULL, NULL, NULL, 0};

/* /dev/kvm, the misc device 10:232, read and written by everyone, as
 * udev's rule for the kvm group gives it on a machine with one user. */
static struct devfs_node kvm_node = {"kvm", 10, 232, 0666, &dev_ops, NULL, NULL};

/*
 * The module parameters Linux's KVM selftests read, as Linux's files under
 * /sys/module give them, with what Rach2's KVM does: no forced emulation
 * prefix, no performance counters for guests, the guest-physical tables
 * always (EPT or nested paging, with the unrestricted guest on VMX), and no
 * eager splitting of huge leaves when a dirty log starts. Every selftest
 * reads `force_emulation_prefix` before its main and skips without it.
 */
static const char *const kvm_modules[] = {"kvm", "kvm_intel", "kvm_amd"};

static const struct kvm_param {
    int module; /* of kvm_modules: kvm, or the backend's kvm_intel or kvm_amd */
    const char *name;
    char value; /* the file's character; zero means the host PMU's Y or N */
} kvm_params[] = {{0, "force_emulation_prefix", '0'}, {0, "enable_pmu", 0},    {0, "tdp_mmu", 'Y'},
                  {0, "eager_page_split", 'N'},       {0, "ignore_msrs", 'N'}, {1, "ept", 'Y'},
                  {1, "unrestricted_guest", 'Y'},     {2, "npt", 'Y'},         {2, "avic", 'N'}};

#define KVM_NR_PARAMS (sizeof(kvm_params) / sizeof(kvm_params[0]))

/** A parameter's file: the value of `kvm_params[arg]` and a newline. */
static int32_t param_show(void *obj, int arg, char *buf, uint32_t cap)
{
    (void)obj;
    if (cap >= 2u) {
        buf[0] = kvm_params[arg].value;
        buf[1] = '\n';
    }
    return 2;
}

/**
 * Make /sys/module/MODULE/parameters for `kvm` and for the backend's
 * module, `kvm_intel` (`backend` 1) or `kvm_amd` (2), as Linux's modules
 * show theirs, each file read-only, and kvm_intel's pmu_steps, which root
 * writes. 0, or the first error of sysfs.h, which
 * leaves what was made; /dev/kvm works without the files.
 */
static int params_start(int backend)
{
    struct sysfs_attr attr;
    int module_dir, dir = 0, h, made = -1;
    uint32_t i;

    module_dir = sysfs_add_dir(sysfs_anchor(SYSFS_ROOT, 0), "module");
    if (module_dir < 0) {
        return module_dir;
    }
    attr.mode = 0444u;
    attr.show = param_show;
    attr.store = NULL;
    for (i = 0; i < KVM_NR_PARAMS; i++) {
        if (kvm_params[i].module != 0 && kvm_params[i].module != backend) {
            continue;
        }
        if (kvm_params[i].module != made) {
            h = sysfs_add_dir(module_dir, kvm_modules[kvm_params[i].module]);
            dir = h < 0 ? h : sysfs_add_dir(h, "parameters");
            if (dir < 0) {
                return dir;
            }
            made = kvm_params[i].module;
        }
        attr.name = kvm_params[i].name;
        h = sysfs_add_attr(dir, &attr, NULL, (int)i);
        if (h < 0) {
            return h;
        }
    }
    if (made == 1) {
        /* Rach2's own: the measuring knob of pmu.h's KVM_PMU_STEP_*. */
        attr.name = "pmu_steps";
        attr.mode = 0644u;
        attr.show = kvm_pmu_steps_show;
        attr.store = kvm_pmu_steps_store;
        h = sysfs_add_attr(dir, &attr, NULL, 0);
        if (h < 0) {
            return h;
        }
    }
    return 0;
}

void kvm_start(void)
{
    int r;

    if (kvm_vmx.probe()) {
        kvm_backend = &kvm_vmx;
    } else if (kvm_svm.probe()) {
        kvm_backend = &kvm_svm;
    } else {
        klog(KLOG_INFO, "kvm", "none: the processor has neither VMX nor SVM");
        return;
    }
    kvm_pmu_start();
    if (devfs_node_register(&kvm_node) != 0) {
        klog(KLOG_WARN, "kvm", "failed: /dev/kvm not registered");
        kvm_backend = NULL;
        return;
    }
    r = params_start(kvm_backend == &kvm_vmx ? 1 : 2);
    if (r < 0) {
        klog(KLOG_WARN, "kvm", "failed: /sys/module/kvm not made: %d", r);
    }
    kvm_exitstat_start();
    klog(KLOG_INFO, "kvm", "ready: %s", kvm_backend->name);
}
