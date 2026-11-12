/*
 * kvm_abi.h: Linux's KVM interface at API version 12, the x86 structures,
 * ioctl numbers, capabilities and exit reasons of <linux/kvm.h> and
 * <asm/kvm.h>, in C89 with fixed-width members.
 */
#ifndef _KVM_ABI_H_
#define _KVM_ABI_H_

#include "../lib/kint.h"

#define KVM_API_VERSION 12

/* The ioctl number of <asm-generic/ioctl.h>: direction, size, type 0xAE and
 * the command. Read is the kernel writing the caller's buffer. */
#define KVMIO 0xAEu
#define KVM_IOC(dir, nr, sz) \
    (((uint32_t)(dir) << 30) | ((uint32_t)(sz) << 16) | (KVMIO << 8) | (nr))
#define KVM_IO(nr)       KVM_IOC(0u, nr, 0u)
#define KVM_IOW(nr, sz)  KVM_IOC(1u, nr, sz)
#define KVM_IOR(nr, sz)  KVM_IOC(2u, nr, sz)
#define KVM_IOWR(nr, sz) KVM_IOC(3u, nr, sz)

/* The ioctls of /dev/kvm. */
#define KVM_GET_API_VERSION            KVM_IO(0x00u)
#define KVM_CREATE_VM                  KVM_IO(0x01u)
#define KVM_GET_MSR_INDEX_LIST         KVM_IOWR(0x02u, 4u)
#define KVM_CHECK_EXTENSION            KVM_IO(0x03u)
#define KVM_GET_VCPU_MMAP_SIZE         KVM_IO(0x04u)
#define KVM_GET_SUPPORTED_CPUID        KVM_IOWR(0x05u, 8u)
#define KVM_GET_EMULATED_CPUID         KVM_IOWR(0x09u, 8u)
#define KVM_GET_MSR_FEATURE_INDEX_LIST KVM_IOWR(0x0Au, 4u)
#define KVM_X86_GET_MCE_CAP_SUPPORTED  KVM_IOR(0x9Du, 8u)
#define KVM_GET_SUPPORTED_HV_CPUID     KVM_IOWR(0xC1u, 8u)
/* KVM_GET_MSRS is also an ioctl of /dev/kvm, for the feature MSRs, and so
 * are KVM_GET_DEVICE_ATTR and KVM_HAS_DEVICE_ATTR, for the attributes of
 * the system (KVM_CAP_SYS_ATTRIBUTES). */

/* The ioctls of a VM. */
#define KVM_CREATE_VCPU               KVM_IO(0x41u)
#define KVM_GET_DIRTY_LOG             KVM_IOW(0x42u, 16u)
#define KVM_SET_NR_MMU_PAGES          KVM_IO(0x44u)
#define KVM_GET_NR_MMU_PAGES          KVM_IO(0x45u)
#define KVM_SET_USER_MEMORY_REGION    KVM_IOW(0x46u, 32u)
#define KVM_SET_TSS_ADDR              KVM_IO(0x47u)
#define KVM_SET_IDENTITY_MAP_ADDR     KVM_IOW(0x48u, 8u)
#define KVM_SET_USER_MEMORY_REGION2   KVM_IOW(0x49u, 160u)
#define KVM_CREATE_IRQCHIP            KVM_IO(0x60u)
#define KVM_IRQ_LINE                  KVM_IOW(0x61u, 8u)
#define KVM_GET_IRQCHIP               KVM_IOWR(0x62u, 520u)
#define KVM_SET_IRQCHIP               KVM_IOR(0x63u, 520u)
#define KVM_CREATE_PIT                KVM_IO(0x64u)
#define KVM_GET_PIT                   KVM_IOWR(0x65u, 72u)
#define KVM_SET_PIT                   KVM_IOR(0x66u, 72u)
#define KVM_IRQ_LINE_STATUS           KVM_IOWR(0x67u, 8u)
#define KVM_REGISTER_COALESCED_MMIO   KVM_IOW(0x67u, 16u)
#define KVM_UNREGISTER_COALESCED_MMIO KVM_IOW(0x68u, 16u)
#define KVM_SET_GSI_ROUTING           KVM_IOW(0x6Au, 8u)
#define KVM_REINJECT_CONTROL          KVM_IO(0x71u)
#define KVM_IRQFD                     KVM_IOW(0x76u, 32u)
#define KVM_CREATE_PIT2               KVM_IOW(0x77u, 64u)
#define KVM_SET_BOOT_CPU_ID           KVM_IO(0x78u)
#define KVM_IOEVENTFD                 KVM_IOW(0x79u, 64u)
#define KVM_SET_CLOCK                 KVM_IOW(0x7Bu, 48u)
#define KVM_GET_CLOCK                 KVM_IOR(0x7Cu, 48u)
#define KVM_GET_PIT2                  KVM_IOR(0x9Fu, 112u)
#define KVM_SET_PIT2                  KVM_IOW(0xA0u, 112u)
#define KVM_ENABLE_CAP                KVM_IOW(0xA3u, 104u)
#define KVM_SIGNAL_MSI                KVM_IOW(0xA5u, 32u)
#define KVM_SET_PMU_EVENT_FILTER      KVM_IOW(0xB2u, 32u)
#define KVM_HYPERV_EVENTFD            KVM_IOW(0xBDu, 24u)
#define KVM_X86_SET_MSR_FILTER        KVM_IOW(0xC6u, 392u)
#define KVM_CLEAR_DIRTY_LOG           KVM_IOWR(0xC0u, 24u)
#define KVM_CREATE_DEVICE             KVM_IOWR(0xE0u, 12u)
/* KVM_SET_TSC_KHZ and KVM_GET_TSC_KHZ are also ioctls of a VM, for the rate
 * its new vCPUs start at (KVM_CAP_VM_TSC_CONTROL), and KVM_GET_STATS_FD is
 * one of a VM and of a vCPU. */
#define KVM_GET_STATS_FD              KVM_IO(0xCEu)

/* The ioctls of a device created by KVM_CREATE_DEVICE. */
#define KVM_SET_DEVICE_ATTR KVM_IOW(0xE1u, 24u)
#define KVM_GET_DEVICE_ATTR KVM_IOW(0xE2u, 24u)
#define KVM_HAS_DEVICE_ATTR KVM_IOW(0xE3u, 24u)

/* The ioctls of a vCPU. */
#define KVM_RUN              KVM_IO(0x80u)
#define KVM_GET_REGS         KVM_IOR(0x81u, 144u)
#define KVM_SET_REGS         KVM_IOW(0x82u, 144u)
#define KVM_GET_SREGS        KVM_IOR(0x83u, 312u)
#define KVM_SET_SREGS        KVM_IOW(0x84u, 312u)
#define KVM_TRANSLATE        KVM_IOWR(0x85u, 24u)
#define KVM_INTERRUPT        KVM_IOW(0x86u, 4u)
#define KVM_GET_MSRS         KVM_IOWR(0x88u, 8u)
#define KVM_SET_MSRS         KVM_IOW(0x89u, 8u)
#define KVM_SET_SIGNAL_MASK  KVM_IOW(0x8Bu, 4u)
#define KVM_GET_FPU          KVM_IOR(0x8Cu, 416u)
#define KVM_SET_FPU          KVM_IOW(0x8Du, 416u)
#define KVM_GET_LAPIC        KVM_IOR(0x8Eu, 1024u)
#define KVM_SET_LAPIC        KVM_IOW(0x8Fu, 1024u)
#define KVM_SET_CPUID2       KVM_IOW(0x90u, 8u)
#define KVM_GET_CPUID2       KVM_IOWR(0x91u, 8u)
#define KVM_SET_VAPIC_ADDR   KVM_IOW(0x93u, 8u)
#define KVM_GET_MP_STATE     KVM_IOR(0x98u, 4u)
#define KVM_SET_MP_STATE     KVM_IOW(0x99u, 4u)
#define KVM_NMI              KVM_IO(0x9Au)
#define KVM_SET_TSC_KHZ      KVM_IO(0xA2u)
#define KVM_GET_TSC_KHZ      KVM_IO(0xA3u)
#define KVM_SET_GUEST_DEBUG  KVM_IOW(0x9Bu, 72u)
#define KVM_X86_SETUP_MCE    KVM_IOW(0x9Cu, 8u)
#define KVM_X86_SET_MCE      KVM_IOW(0x9Eu, 64u)
#define KVM_GET_VCPU_EVENTS  KVM_IOR(0x9Fu, 64u)
#define KVM_SET_VCPU_EVENTS  KVM_IOW(0xA0u, 64u)
#define KVM_GET_DEBUGREGS    KVM_IOR(0xA1u, 128u)
#define KVM_SET_DEBUGREGS    KVM_IOW(0xA2u, 128u)
#define KVM_GET_XSAVE        KVM_IOR(0xA4u, 4096u)
#define KVM_SET_XSAVE        KVM_IOW(0xA5u, 4096u)
#define KVM_GET_XSAVE2       KVM_IOR(0xCFu, 4096u)
#define KVM_GET_XCRS         KVM_IOR(0xA6u, 392u)
#define KVM_SET_XCRS         KVM_IOW(0xA7u, 392u)
#define KVM_KVMCLOCK_CTRL    KVM_IO(0xADu)
#define KVM_PRE_FAULT_MEMORY KVM_IOWR(0xD5u, 64u)
/* KVM_GET_SUPPORTED_HV_CPUID is also an ioctl of a vCPU. */

/* The capabilities KVM_CHECK_EXTENSION names, by Linux's numbers. */
#define KVM_CAP_IRQCHIP                     0
#define KVM_CAP_HLT                         1
#define KVM_CAP_MMU_SHADOW_CACHE_CONTROL    2
#define KVM_CAP_USER_MEMORY                 3
#define KVM_CAP_SET_TSS_ADDR                4
#define KVM_CAP_VAPIC                       6
#define KVM_CAP_EXT_CPUID                   7
#define KVM_CAP_CLOCKSOURCE                 8
#define KVM_CAP_NR_VCPUS                    9
#define KVM_CAP_NR_MEMSLOTS                 10
#define KVM_CAP_PIT                         11
#define KVM_CAP_NOP_IO_DELAY                12
#define KVM_CAP_PV_MMU                      13
#define KVM_CAP_MP_STATE                    14
#define KVM_CAP_COALESCED_MMIO              15
#define KVM_CAP_SYNC_MMU                    16
#define KVM_CAP_IOMMU                       18
#define KVM_CAP_DESTROY_MEMORY_REGION_WORKS 21
#define KVM_CAP_USER_NMI                    22
#define KVM_CAP_SET_GUEST_DEBUG             23
#define KVM_CAP_REINJECT_CONTROL            24
#define KVM_CAP_IRQ_ROUTING                 25
#define KVM_CAP_IRQ_INJECT_STATUS           26
#define KVM_CAP_ASSIGN_DEV_IRQ              29
#define KVM_CAP_JOIN_MEMORY_REGIONS_WORKS   30
#define KVM_CAP_MCE                         31
#define KVM_CAP_IRQFD                       32
#define KVM_CAP_PIT2                        33
#define KVM_CAP_SET_BOOT_CPU_ID             34
#define KVM_CAP_PIT_STATE2                  35
#define KVM_CAP_IOEVENTFD                   36
#define KVM_CAP_SET_IDENTITY_MAP_ADDR       37
#define KVM_CAP_ADJUST_CLOCK                39
#define KVM_CAP_INTERNAL_ERROR_DATA         40
#define KVM_CAP_VCPU_EVENTS                 41
#define KVM_CAP_S390_PSW                    42
#define KVM_CAP_PPC_SEGSTATE                43
#define KVM_CAP_HYPERV                      44
#define KVM_CAP_HYPERV_VAPIC                45
#define KVM_CAP_HYPERV_SPIN                 46
#define KVM_CAP_PCI_SEGMENT                 47
#define KVM_CAP_PPC_PAIRED_SINGLES          48
#define KVM_CAP_INTR_SHADOW                 49
#define KVM_CAP_DEBUGREGS                   50
#define KVM_CAP_X86_ROBUST_SINGLESTEP       51
#define KVM_CAP_PPC_OSI                     52
#define KVM_CAP_PPC_UNSET_IRQ               53
#define KVM_CAP_ENABLE_CAP                  54
#define KVM_CAP_XSAVE                       55
#define KVM_CAP_XCRS                        56
#define KVM_CAP_ASYNC_PF                    59
#define KVM_CAP_TSC_CONTROL                 60
#define KVM_CAP_GET_TSC_KHZ                 61
#define KVM_CAP_TSC_DEADLINE_TIMER          72
#define KVM_CAP_MAX_VCPUS                   66
#define KVM_CAP_KVMCLOCK_CTRL               76
#define KVM_CAP_SIGNAL_MSI                  77
#define KVM_CAP_READONLY_MEM                81
#define KVM_CAP_IRQFD_RESAMPLE              82
#define KVM_CAP_DEVICE_CTRL                 89
#define KVM_CAP_ENABLE_CAP_VM               98
#define KVM_CAP_IOEVENTFD_NO_LENGTH         100
#define KVM_CAP_VM_ATTRIBUTES               101
#define KVM_CAP_VCPU_ATTRIBUTES             127
#define KVM_CAP_CHECK_EXTENSION_VM          105
#define KVM_CAP_IMMEDIATE_EXIT              136
#define KVM_CAP_SPLIT_IRQCHIP               121
#define KVM_CAP_IOEVENTFD_ANY_LENGTH        122
#define KVM_CAP_X2APIC_API                  129
#define KVM_CAP_X86_DISABLE_EXITS           143
#define KVM_CAP_MAX_VCPU_ID                 128
#define KVM_CAP_GET_MSR_FEATURES            153
#define KVM_CAP_MANUAL_DIRTY_LOG_PROTECT2   168
#define KVM_CAP_STEAL_TIME                  187
#define KVM_CAP_X86_USER_SPACE_MSR          188
#define KVM_CAP_X86_MSR_FILTER              189
#define KVM_CAP_ENFORCE_PV_FEATURE_CPUID    190
#define KVM_CAP_HYPERV_TIME                 96
#define KVM_CAP_HYPERV_SYNIC                123
#define KVM_CAP_HYPERV_SYNIC2               148
#define KVM_CAP_HYPERV_VP_INDEX             149
#define KVM_CAP_HYPERV_EVENTFD              154
#define KVM_CAP_HYPERV_TLBFLUSH             155
#define KVM_CAP_HYPERV_SEND_IPI             161
#define KVM_CAP_HYPERV_ENLIGHTENED_VMCS     163
#define KVM_CAP_HYPERV_CPUID                167
#define KVM_CAP_HYPERV_DIRECT_TLBFLUSH      175
#define KVM_CAP_SYS_HYPERV_CPUID            191
#define KVM_CAP_HYPERV_ENFORCE_CPUID        199
#define KVM_CAP_USER_MEMORY2                231
#define KVM_CAP_SYNC_REGS                   74
#define KVM_CAP_DISABLE_QUIRKS              116
#define KVM_CAP_MSR_PLATFORM_INFO           159
#define KVM_CAP_COALESCED_PIO               162
#define KVM_CAP_SET_GUEST_DEBUG2            195
#define KVM_CAP_BINARY_STATS_FD             203
#define KVM_CAP_EXIT_ON_EMULATION_FAILURE   204
#define KVM_CAP_XSAVE2                      208
#define KVM_CAP_SYS_ATTRIBUTES              209
#define KVM_CAP_DISABLE_QUIRKS2             213
#define KVM_CAP_VM_TSC_CONTROL              214
#define KVM_CAP_PRE_FAULT_MEMORY            236
#define KVM_CAP_X86_APIC_BUS_CYCLES_NS      237
#define KVM_CAP_PMU_EVENT_FILTER            173
#define KVM_CAP_PMU_CAPABILITY              212
#define KVM_CAP_PMU_EVENT_MASKED_EVENTS     226

#define KVM_PMU_CAP_DISABLE              1u
#define KVM_PMU_EVENT_ALLOW              0u
#define KVM_PMU_EVENT_DENY               1u
#define KVM_PMU_EVENT_FLAG_MASKED_EVENTS 1u

/* KVM_SET_PMU_EVENT_FILTER, followed by nevents uint64_t entries. */
struct kvm_pmu_event_filter {
    uint32_t action, nevents, fixed_counter_bitmap, flags;
    uint32_t pad[4];
};

/* The exit reasons of kvm_run.exit_reason. */
#define KVM_EXIT_UNKNOWN         0
#define KVM_EXIT_EXCEPTION       1
#define KVM_EXIT_IO              2
#define KVM_EXIT_HYPERCALL       3
#define KVM_EXIT_DEBUG           4
#define KVM_EXIT_HLT             5
#define KVM_EXIT_MMIO            6
#define KVM_EXIT_IRQ_WINDOW_OPEN 7
#define KVM_EXIT_SHUTDOWN        8
#define KVM_EXIT_FAIL_ENTRY      9
#define KVM_EXIT_INTR            10
#define KVM_EXIT_NMI             16
#define KVM_EXIT_INTERNAL_ERROR  17
#define KVM_EXIT_SYSTEM_EVENT    24
#define KVM_EXIT_IOAPIC_EOI      26
#define KVM_EXIT_HYPERV          27
#define KVM_EXIT_X86_RDMSR       29
#define KVM_EXIT_X86_WRMSR       30

/* The types of KVM_EXIT_SYSTEM_EVENT. */
#define KVM_SYSTEM_EVENT_SHUTDOWN 1
#define KVM_SYSTEM_EVENT_RESET    2
#define KVM_SYSTEM_EVENT_CRASH    5

#define KVM_EXIT_IO_IN  0
#define KVM_EXIT_IO_OUT 1

/* The suberrors of KVM_EXIT_INTERNAL_ERROR. */
#define KVM_INTERNAL_ERROR_EMULATION              1
#define KVM_INTERNAL_ERROR_SIMUL_EX               2
#define KVM_INTERNAL_ERROR_DELIVERY_EV            3
#define KVM_INTERNAL_ERROR_UNEXPECTED_EXIT_REASON 4

/* The flag of an emulation failure that carries the instruction's bytes. */
#define KVM_INTERNAL_ERROR_EMULATION_FLAG_INSTRUCTION_BYTES 1ull

/* The page offsets of a vCPU's mapping: the run structure, the data of
 * port I/O, which kvm_run.io.data_offset names, and the VM's ring of
 * coalesced MMIO and port writes, which every vCPU of the VM maps. */
#define KVM_PIO_PAGE_OFFSET            1
#define KVM_COALESCED_MMIO_PAGE_OFFSET 2

/* struct kvm_regs. */
struct kvm_regs {
    uint64_t rax, rbx, rcx, rdx;
    uint64_t rsi, rdi, rsp, rbp;
    uint64_t r8, r9, r10, r11;
    uint64_t r12, r13, r14, r15;
    uint64_t rip, rflags;
};

/* struct kvm_segment: a segment register with its hidden part. */
struct kvm_segment {
    uint64_t base;
    uint32_t limit;
    uint16_t selector;
    uint8_t type;
    uint8_t present, dpl, db, s, l, g, avl;
    uint8_t unusable;
    uint8_t padding;
};

/* struct kvm_dtable: the GDT or IDT register. */
struct kvm_dtable {
    uint64_t base;
    uint16_t limit;
    uint16_t padding[3];
};

#define KVM_NR_INTERRUPTS 256

/* struct kvm_sregs. */
struct kvm_sregs {
    struct kvm_segment cs, ds, es, fs, gs, ss;
    struct kvm_segment tr, ldt;
    struct kvm_dtable gdt, idt;
    uint64_t cr0, cr2, cr3, cr4, cr8;
    uint64_t efer;
    uint64_t apic_base;
    uint64_t interrupt_bitmap[(KVM_NR_INTERRUPTS + 63) / 64];
};

/* struct kvm_fpu: the legacy part of the FPU state. */
struct kvm_fpu {
    uint8_t fpr[8][16];
    uint16_t fcw;
    uint16_t fsw;
    uint8_t ftwx; /* the abridged tag word of fxsave */
    uint8_t pad1;
    uint16_t last_opcode;
    uint64_t last_ip;
    uint64_t last_dp;
    uint8_t xmm[16][16];
    uint32_t mxcsr;
    uint32_t pad2;
};

struct kvm_msr_entry {
    uint32_t index;
    uint32_t reserved;
    uint64_t data;
};

/* struct kvm_msrs, followed by `nmsrs` entries. */
struct kvm_msrs {
    uint32_t nmsrs;
    uint32_t pad;
};

/* struct kvm_msr_list, followed by `nmsrs` indices. */
struct kvm_msr_list {
    uint32_t nmsrs;
};

#define KVM_CPUID_FLAG_SIGNIFCANT_INDEX 1u

struct kvm_cpuid_entry2 {
    uint32_t function;
    uint32_t index;
    uint32_t flags;
    uint32_t eax, ebx, ecx, edx;
    uint32_t padding[3];
};

/* struct kvm_cpuid2, followed by `nent` entries. */
struct kvm_cpuid2 {
    uint32_t nent;
    uint32_t padding;
};

#define KVM_MP_STATE_RUNNABLE      0
#define KVM_MP_STATE_UNINITIALIZED 1
#define KVM_MP_STATE_INIT_RECEIVED 2
#define KVM_MP_STATE_HALTED        3
#define KVM_MP_STATE_SIPI_RECEIVED 4
#define KVM_MP_STATE_AP_RESET_HOLD 9

struct kvm_mp_state {
    uint32_t mp_state;
};

#define KVM_VCPUEVENT_VALID_NMI_PENDING  0x00000001u
#define KVM_VCPUEVENT_VALID_SIPI_VECTOR  0x00000002u
#define KVM_VCPUEVENT_VALID_SHADOW       0x00000004u
#define KVM_VCPUEVENT_VALID_SMM          0x00000008u
#define KVM_VCPUEVENT_VALID_PAYLOAD      0x00000010u
#define KVM_VCPUEVENT_VALID_TRIPLE_FAULT 0x00000020u

/* The interrupt shadow's kinds in kvm_vcpu_events.interrupt.shadow. */
#define KVM_X86_SHADOW_INT_MOV_SS 0x01u
#define KVM_X86_SHADOW_INT_STI    0x02u

struct kvm_vcpu_events {
    struct {
        uint8_t injected;
        uint8_t nr;
        uint8_t has_error_code;
        uint8_t pending;
        uint32_t error_code;
    } exception;
    struct {
        uint8_t injected;
        uint8_t nr;
        uint8_t soft;
        uint8_t shadow;
    } interrupt;
    struct {
        uint8_t injected;
        uint8_t pending;
        uint8_t masked;
        uint8_t pad;
    } nmi;
    uint32_t sipi_vector;
    uint32_t flags;
    struct {
        uint8_t smm;
        uint8_t pending;
        uint8_t smm_inside_nmi;
        uint8_t latched_init;
    } smi;
    struct {
        uint8_t pending;
    } triple_fault;
    uint8_t reserved[26];
    uint8_t exception_has_payload;
    uint64_t exception_payload;
};

struct kvm_debugregs {
    uint64_t db[4];
    uint64_t dr6;
    uint64_t dr7;
    uint64_t flags;
    uint64_t reserved[9];
};

#define KVM_XSAVE_SIZE 4096u

struct kvm_xsave {
    uint32_t region[KVM_XSAVE_SIZE / 4];
};

#define KVM_MAX_XCRS 16

struct kvm_xcr {
    uint32_t xcr;
    uint32_t reserved;
    uint64_t value;
};

struct kvm_xcrs {
    uint32_t nr_xcrs;
    uint32_t flags;
    struct kvm_xcr xcrs[KVM_MAX_XCRS];
    uint64_t padding[16];
};

/* The flags of struct kvm_clock_data. */
#define KVM_CLOCK_TSC_STABLE  2u
#define KVM_CLOCK_REALTIME    (1u << 2)
#define KVM_CLOCK_HOST_TSC    (1u << 3)
#define KVM_CLOCK_VALID_FLAGS (KVM_CLOCK_TSC_STABLE | KVM_CLOCK_REALTIME | KVM_CLOCK_HOST_TSC)

struct kvm_clock_data {
    uint64_t clock;
    uint32_t flags;
    uint32_t pad0;
    uint64_t realtime;
    uint64_t host_tsc;
    uint32_t pad[4];
};

/* The exits KVM_CAP_X86_DISABLE_EXITS stops: MWAIT, HLT, PAUSE and the
 * C-state residency MSRs' reads. */
#define KVM_X86_DISABLE_EXITS_MWAIT  (1u << 0)
#define KVM_X86_DISABLE_EXITS_HLT    (1u << 1)
#define KVM_X86_DISABLE_EXITS_PAUSE  (1u << 2)
#define KVM_X86_DISABLE_EXITS_CSTATE (1u << 3)
#define KVM_X86_DISABLE_VALID_EXITS  0xFu

/* KVM's own CPUID leaves, at 0x40000000 or 0x100 above it for each
 * hypervisor's leaves before them, and the feature bits of the second,
 * Linux's asm/kvm_para.h. */
#define KVM_CPUID_SIGNATURE                0x40000000u
#define KVM_CPUID_FEATURES                 0x40000001u
#define KVM_FEATURE_CLOCKSOURCE            0
#define KVM_FEATURE_NOP_IO_DELAY           1
#define KVM_FEATURE_CLOCKSOURCE2           3
#define KVM_FEATURE_STEAL_TIME             5
#define KVM_FEATURE_PV_UNHALT              7
#define KVM_FEATURE_PV_SEND_IPI            11
#define KVM_FEATURE_POLL_CONTROL           12
#define KVM_FEATURE_PV_SCHED_YIELD         13
#define KVM_FEATURE_CLOCKSOURCE_STABLE_BIT 24

/* Linux's KVM hypercalls, the number in RAX, and the errors they return
 * there, negated. */
#define KVM_HC_VAPIC_POLL_IRQ 1u
#define KVM_HC_KICK_CPU       5u
#define KVM_HC_CLOCK_PAIRING  9u
#define KVM_HC_SEND_IPI       10u
#define KVM_HC_SCHED_YIELD    11u
#define KVM_EPERM             1
#define KVM_EFAULT            14
#define KVM_EINVAL            22
#define KVM_EOPNOTSUPP        95
#define KVM_ENOSYS            1000

/* The kvmclock's MSRs: the old pair, which the guest finds by
 * KVM_FEATURE_CLOCKSOURCE, and the new, by KVM_FEATURE_CLOCKSOURCE2; and
 * the steal time's, by KVM_FEATURE_STEAL_TIME; and the halt polling
 * control. Bit 0 of a SYSTEM_TIME or STEAL_TIME MSR enables the record at
 * the guest-physical address in the rest. */
#define MSR_KVM_WALL_CLOCK      0x11u
#define MSR_KVM_SYSTEM_TIME     0x12u
#define MSR_KVM_WALL_CLOCK_NEW  0x4B564D00u
#define MSR_KVM_SYSTEM_TIME_NEW 0x4B564D01u
#define MSR_KVM_STEAL_TIME      0x4B564D03u
#define MSR_KVM_POLL_CONTROL    0x4B564D05u
#define KVM_MSR_ENABLED         1u

/* A vCPU's kvmclock in guest memory, Linux's pvclock_vcpu_time_info: the
 * guest's clock at its TSC `tsc` is `system_time` plus (tsc -
 * tsc_timestamp) shifted by `tsc_shift`, times `tsc_to_system_mul` / 2^32,
 * in ns. `version` is odd while the host writes the rest. 32 bytes. */
struct pvclock_vcpu_time_info {
    uint32_t version;
    uint32_t pad0;
    uint64_t tsc_timestamp;
    uint64_t system_time;
    uint32_t tsc_to_system_mul;
    int8_t tsc_shift;
    uint8_t flags; /* PVCLOCK_* */
    uint8_t pad[2];
};

#define PVCLOCK_TSC_STABLE_BIT 0x01u /* every vCPU reads one clock */
#define PVCLOCK_GUEST_STOPPED  0x02u /* KVM_KVMCLOCK_CTRL: the guest was paused */

/* The wall clock in guest memory: the real time at which the kvmclock read
 * 0, its seconds modulo 2^32. 12 bytes. */
struct pvclock_wall_clock {
    uint32_t version;
    uint32_t sec;
    uint32_t nsec;
};

/* A vCPU's steal time in guest memory, Linux's kvm_steal_time: the ns its
 * thread waited for a host processor while the guest could have run,
 * `version` odd while the host writes it, and KVM_VCPU_PREEMPTED in
 * `preempted` while the host has taken the processor from it. 64 bytes,
 * at a 64-byte aligned address: MSR_KVM_STEAL_TIME's bits 1 to 5 are
 * reserved, and bits 6 up are the address. */
struct kvm_steal_time {
    uint64_t steal;
    uint32_t version;
    uint32_t flags;
    uint8_t preempted;
    uint8_t u8_pad[3];
    uint32_t pad[11];
};

#define KVM_VCPU_PREEMPTED      0x1u
#define KVM_STEAL_VALID_BITS    (~0x3Full)
#define KVM_STEAL_RESERVED_MASK 0x3Eull

/* KVM_HC_CLOCK_PAIRING's answer in guest memory, the real time at the
 * guest's TSC `tsc`. 64 bytes. */
#define KVM_CLOCK_PAIRING_WALLCLOCK 0
struct kvm_clock_pairing {
    int64_t sec;
    int64_t nsec;
    uint64_t tsc;
    uint32_t flags;
    uint32_t pad[9];
};

struct kvm_userspace_memory_region {
    uint32_t slot;
    uint32_t flags;
    uint64_t guest_phys_addr;
    uint64_t memory_size; /* bytes */
    uint64_t userspace_addr;
};

/* KVM_SET_USER_MEMORY_REGION2's argument: the same slot, then the file of
 * guest_memfd, which no VM here has, and Linux's padding, which it does
 * not read. */
struct kvm_userspace_memory_region2 {
    struct kvm_userspace_memory_region region;
    uint64_t guest_memfd_offset;
    uint32_t guest_memfd;
    uint32_t pad1;
    uint64_t pad2[14];
};

#define KVM_MEM_LOG_DIRTY_PAGES 1u
#define KVM_MEM_READONLY        2u

/* KVM_GET_DIRTY_LOG's argument: the slot, as KVM_SET_USER_MEMORY_REGION
 * numbers it, and the caller's address of a bitmap of a bit for each of the
 * slot's pages, in 64-bit words. */
struct kvm_dirty_log {
    uint32_t slot;
    uint32_t padding1;
    uint64_t dirty_bitmap;
};

/* KVM_CLEAR_DIRTY_LOG's argument: `num_pages` bits of the caller's bitmap
 * from the slot's page `first_page`, a multiple of 64. */
struct kvm_clear_dirty_log {
    uint32_t slot;
    uint32_t num_pages;
    uint64_t first_page;
    uint64_t dirty_bitmap;
};

/* The options of KVM_CAP_MANUAL_DIRTY_LOG_PROTECT2. */
#define KVM_DIRTY_LOG_MANUAL_PROTECT_ENABLE 1u
#define KVM_DIRTY_LOG_INITIALLY_SET         2u

/* The VFIO bridge is the one device type provided on x86. */
#define KVM_DEV_TYPE_VFIO      4u
#define KVM_CREATE_DEVICE_TEST 1u
#define KVM_DEV_VFIO_FILE      1u
#define KVM_DEV_VFIO_FILE_ADD  1u
#define KVM_DEV_VFIO_FILE_DEL  2u

struct kvm_create_device {
    uint32_t type;
    uint32_t fd; /* output, untouched with KVM_CREATE_DEVICE_TEST */
    uint32_t flags;
};

struct kvm_device_attr {
    uint32_t flags;
    uint32_t group;
    uint64_t attr;
    uint64_t addr; /* user address of the attribute's value */
};

struct kvm_enable_cap {
    uint32_t cap;
    uint32_t flags;
    uint64_t args[4];
    uint8_t pad[64];
};

struct kvm_interrupt {
    uint32_t irq;
};

/* The machine-check capabilities of IA32_MCG_CAP KVM offers. */
#define KVM_MAX_MCE_BANKS 32
#define MCG_CTL_P         (1ull << 8)
#define MCG_SER_P         (1ull << 24)

struct kvm_x86_mce {
    uint64_t status;
    uint64_t addr;
    uint64_t misc;
    uint64_t mcg_status;
    uint8_t bank;
    uint8_t pad1[7];
    uint64_t pad2[3];
};

#define KVM_GUESTDBG_ENABLE     0x00000001u
#define KVM_GUESTDBG_SINGLESTEP 0x00000002u
#define KVM_GUESTDBG_INJECT_DB  0x00040000u
#define KVM_GUESTDBG_INJECT_BP  0x00080000u
#define KVM_GUESTDBG_BLOCKIRQ   0x00100000u
#define KVM_GUESTDBG_USE_SW_BP  0x00010000u
#define KVM_GUESTDBG_USE_HW_BP  0x00020000u
#define KVM_GUESTDBG_VALID_MASK                                                 \
    (KVM_GUESTDBG_ENABLE | KVM_GUESTDBG_SINGLESTEP | KVM_GUESTDBG_USE_HW_BP |   \
     KVM_GUESTDBG_USE_SW_BP | KVM_GUESTDBG_INJECT_BP | KVM_GUESTDBG_INJECT_DB | \
     KVM_GUESTDBG_BLOCKIRQ)

struct kvm_guest_debug {
    uint32_t control;
    uint32_t pad;
    uint64_t debugreg[8];
};

/* struct kvm_lapic_state: the local APIC's register page, KVM_GET_LAPIC
 * and KVM_SET_LAPIC. */
#define KVM_APIC_REG_SIZE 0x400u

struct kvm_lapic_state {
    uint8_t regs[KVM_APIC_REG_SIZE];
};

/* The two PICs and the IOAPIC, in Linux's save/restore layout. */
#define KVM_IRQCHIP_PIC_MASTER 0u
#define KVM_IRQCHIP_PIC_SLAVE  1u
#define KVM_IRQCHIP_IOAPIC     2u
#define KVM_IOAPIC_NUM_PINS    24u

struct kvm_pic_state {
    uint8_t last_irr; /* the last line levels, for edge detection */
    uint8_t irr, imr, isr;
    uint8_t priority_add; /* the highest priority pin, modulo eight */
    uint8_t irq_base;
    uint8_t read_reg_select, poll, special_mask, init_state;
    uint8_t auto_eoi, rotate_on_auto_eoi, special_fully_nested_mode, init4;
    uint8_t elcr, elcr_mask; /* level-triggered pins and the writable mask */
};

struct kvm_ioapic_state {
    uint64_t base_address;
    uint32_t ioregsel, id, irr, pad;
    uint64_t redirtbl[KVM_IOAPIC_NUM_PINS];
};

struct kvm_irqchip {
    uint32_t chip_id, pad;
    union {
        uint8_t dummy[512];
        struct kvm_pic_state pic;
        struct kvm_ioapic_state ioapic;
    } chip;
};

/* The 8254's save/restore records. A zero hardware count means 65536. */
struct kvm_pit_channel_state {
    uint32_t count; /* the reload value, 1 to 65536 */
    uint16_t latched_count;
    uint8_t count_latched, status_latched, status;
    uint8_t read_state, write_state, write_latch, rw_mode;
    uint8_t mode, bcd, gate;
    int64_t count_load_time; /* host monotonic nanoseconds */
};

struct kvm_pit_state {
    struct kvm_pit_channel_state channels[3];
};

#define KVM_PIT_FLAGS_HPET_LEGACY     1u
#define KVM_PIT_FLAGS_SPEAKER_DATA_ON 2u
#define KVM_PIT_SPEAKER_DUMMY         1u

struct kvm_pit_state2 {
    struct kvm_pit_channel_state channels[3];
    uint32_t flags;
    uint32_t reserved[9];
};

struct kvm_pit_config {
    uint32_t flags;
    uint32_t pad[15];
};

struct kvm_reinject_control {
    uint8_t pit_reinject;
    uint8_t reserved[31];
};

/* struct kvm_irq_level: KVM_IRQ_LINE's line and level, and the status
 * KVM_IRQ_LINE_STATUS writes back in the same word. */
struct kvm_irq_level {
    int32_t irq; /* the GSI in; the status out */
    uint32_t level;
};

/* The routes of KVM_SET_GSI_ROUTING: one entry a GSI for an MSI or a
 * Hyper-V synthetic interrupt, as Linux's struct kvm_irq_routing_entry of
 * 48 bytes. */
#define KVM_IRQ_ROUTING_IRQCHIP    1u
#define KVM_IRQ_ROUTING_MSI        2u
#define KVM_IRQ_ROUTING_S390       3u
#define KVM_IRQ_ROUTING_HV_SINT    4u
#define KVM_IRQ_ROUTING_XEN_EVTCHN 5u

/* The most routes, and the GSIs below which they lie, Linux's
 * KVM_MAX_IRQ_ROUTES on x86. */
#define KVM_MAX_IRQ_ROUTES 4096u

#define KVM_MSI_VALID_DEVID 1u

struct kvm_irq_routing_irqchip {
    uint32_t irqchip;
    uint32_t pin;
};

struct kvm_irq_routing_msi {
    uint32_t address_lo;
    uint32_t address_hi;
    uint32_t data;
    uint32_t devid;
};

/* A route to the synthetic interrupt source `sint` of the vCPU whose
 * Hyper-V VP index is `vcpu`. */
struct kvm_irq_routing_hv_sint {
    uint32_t vcpu;
    uint32_t sint;
};

struct kvm_irq_routing_entry {
    uint32_t gsi;
    uint32_t type;
    uint32_t flags;
    uint32_t pad;
    union {
        struct kvm_irq_routing_irqchip irqchip;
        struct kvm_irq_routing_msi msi;
        struct kvm_irq_routing_hv_sint hv_sint;
        uint32_t pad[8];
    } u;
};

/* struct kvm_irq_routing, followed by `nr` entries. */
struct kvm_irq_routing {
    uint32_t nr;
    uint32_t flags;
};

/* struct kvm_irqfd: an eventfd that raises the GSI `gsi` when signalled. */
#define KVM_IRQFD_FLAG_DEASSIGN 1u
#define KVM_IRQFD_FLAG_RESAMPLE 2u

struct kvm_irqfd {
    uint32_t fd;
    uint32_t gsi;
    uint32_t flags;
    uint32_t resamplefd;
    uint8_t pad[16];
};

/* struct kvm_msi: KVM_SIGNAL_MSI's message. */
struct kvm_msi {
    uint32_t address_lo;
    uint32_t address_hi;
    uint32_t data;
    uint32_t flags;
    uint32_t devid;
    uint8_t pad[12];
};

/* struct kvm_ioeventfd: an eventfd that a guest's write to a port or an
 * MMIO address signals. */
#define KVM_IOEVENTFD_FLAG_DATAMATCH         1u
#define KVM_IOEVENTFD_FLAG_PIO               2u
#define KVM_IOEVENTFD_FLAG_DEASSIGN          4u
#define KVM_IOEVENTFD_FLAG_VIRTIO_CCW_NOTIFY 8u
#define KVM_IOEVENTFD_VALID_FLAG_MASK        15u

struct kvm_ioeventfd {
    uint64_t datamatch;
    uint64_t addr; /* a port with KVM_IOEVENTFD_FLAG_PIO, else guest-physical */
    uint32_t len;  /* 0, 1, 2, 4 or 8 bytes; 0 matches any write */
    int32_t fd;
    uint32_t flags;
    uint8_t pad[36];
};

/* struct kvm_hyperv_eventfd: an eventfd that the guest's HvSignalEvent
 * of the connection `conn_id` signals, KVM_HYPERV_EVENTFD. */
#define KVM_HYPERV_CONN_ID_MASK     0x00ffffffu
#define KVM_HYPERV_EVENTFD_DEASSIGN 1u

struct kvm_hyperv_eventfd {
    uint32_t conn_id;
    int32_t fd;
    uint32_t flags;
    uint32_t padding[3];
};

/* The quirks KVM_CAP_DISABLE_QUIRKS2 turns off, each a behaviour of old
 * KVM that Linux keeps until the caller asks otherwise. */
#define KVM_X86_QUIRK_LINT0_REENABLED            (1u << 0)
#define KVM_X86_QUIRK_CD_NW_CLEARED              (1u << 1)
#define KVM_X86_QUIRK_LAPIC_MMIO_HOLE            (1u << 2)
#define KVM_X86_QUIRK_OUT_7E_INC_RIP             (1u << 3)
#define KVM_X86_QUIRK_MISC_ENABLE_NO_MWAIT       (1u << 4)
#define KVM_X86_QUIRK_FIX_HYPERCALL_INSN         (1u << 5)
#define KVM_X86_QUIRK_MWAIT_NEVER_UD_FAULTS      (1u << 6)
#define KVM_X86_QUIRK_SLOT_ZAP_ALL               (1u << 7)
#define KVM_X86_QUIRK_STUFF_FEATURE_MSRS         (1u << 8)
#define KVM_X86_QUIRK_IGNORE_GUEST_PAT           (1u << 9)
#define KVM_X86_QUIRK_VMCS12_ALLOW_FREEZE_IN_SMM (1u << 10)
#define KVM_X86_VALID_QUIRKS                     0x7FFu
/* Those that hold on one backend alone and start turned off on the other,
 * Linux's KVM_X86_CONDITIONAL_QUIRKS. */
#define KVM_X86_CONDITIONAL_QUIRKS               (KVM_X86_QUIRK_CD_NW_CLEARED | KVM_X86_QUIRK_IGNORE_GUEST_PAT)

/* The attribute of /dev/kvm in group 0: the XCR0 bits a guest may be given,
 * with those that need a request by arch_prctl first. */
#define KVM_X86_XCOMP_GUEST_SUPP 0u

/* The attribute of a vCPU in group KVM_VCPU_TSC_CTRL
 * (KVM_CAP_VCPU_ATTRIBUTES): the offset of its TSC from the host's, the
 * u64 at the attribute's `addr`. */
#define KVM_VCPU_TSC_CTRL   0u
#define KVM_VCPU_TSC_OFFSET 0u

/* KVM_REGISTER_COALESCED_MMIO and KVM_UNREGISTER_COALESCED_MMIO: a zone of
 * guest-physical addresses, or of ports when `pio` is 1. */
struct kvm_coalesced_mmio_zone {
    uint64_t addr;
    uint32_t size;
    uint32_t pio;
};

/* One write the ring holds: its address or port, length and bytes. */
struct kvm_coalesced_mmio {
    uint64_t phys_addr;
    uint32_t len;
    uint32_t pio;
    uint8_t data[8];
};

/* The ring's page: the kernel writes at `last`, the caller reads from
 * `first`, and the ring is full when last + 1 is first. */
#define KVM_COALESCED_MMIO_MAX ((4096u - 8u) / 24u)
struct kvm_coalesced_mmio_ring {
    uint32_t first, last;
    struct kvm_coalesced_mmio coalesced_mmio[KVM_COALESCED_MMIO_MAX];
};

/* The file KVM_GET_STATS_FD gives: this header, the file's name, the
 * descriptors, each with a name of `name_size` bytes after it, then the
 * values, 64 bits each. */
struct kvm_stats_header {
    uint32_t flags;
    uint32_t name_size;
    uint32_t num_desc;
    uint32_t id_offset;
    uint32_t desc_offset;
    uint32_t data_offset;
};

#define KVM_STATS_NAME_SIZE 48u

struct kvm_stats_desc {
    uint32_t flags;   /* KVM_STATS_TYPE_*, KVM_STATS_UNIT_* and KVM_STATS_BASE_* */
    int16_t exponent; /* the unit's scale, as a power of the base */
    uint16_t size;    /* the 64-bit values the statistic has */
    uint32_t offset;  /* of its first value from the header's data_offset */
    uint32_t bucket_size;
    char name[KVM_STATS_NAME_SIZE];
};

#define KVM_STATS_TYPE_CUMULATIVE  0x0u
#define KVM_STATS_TYPE_INSTANT     0x1u
#define KVM_STATS_TYPE_PEAK        0x2u
#define KVM_STATS_TYPE_LINEAR_HIST 0x3u
#define KVM_STATS_TYPE_LOG_HIST    0x4u
#define KVM_STATS_UNIT_NONE        0x00u
#define KVM_STATS_UNIT_BYTES       0x10u
#define KVM_STATS_UNIT_SECONDS     0x20u
#define KVM_STATS_UNIT_CYCLES      0x30u
#define KVM_STATS_UNIT_BOOLEAN     0x40u
#define KVM_STATS_BASE_POW10       0x000u
#define KVM_STATS_BASE_POW2        0x100u

/* KVM_PRE_FAULT_MEMORY's argument: the guest-physical range to map, which
 * the call moves forward by what it mapped. */
struct kvm_pre_fault_memory {
    uint64_t gpa;
    uint64_t size;
    uint64_t flags;
    uint64_t padding[5];
};

/* The register sets of KVM_CAP_SYNC_REGS, the bits of kvm_run's
 * kvm_valid_regs and kvm_dirty_regs, and the run page's copy of them. */
#define KVM_SYNC_X86_REGS         (1u << 0)
#define KVM_SYNC_X86_SREGS        (1u << 1)
#define KVM_SYNC_X86_EVENTS       (1u << 2)
#define KVM_SYNC_X86_VALID_FIELDS 0x7u

struct kvm_sync_regs {
    struct kvm_regs regs;
    struct kvm_sregs sregs;
    struct kvm_vcpu_events events;
};

/* The exit-specific part of struct kvm_run, a union of 256 bytes. */
struct kvm_run_io {
    uint8_t direction; /* KVM_EXIT_IO_IN or KVM_EXIT_IO_OUT */
    uint8_t size;      /* 1, 2 or 4 */
    uint16_t port;
    uint32_t count;
    uint64_t data_offset; /* from the start of the run structure */
};

struct kvm_run_mmio {
    uint64_t phys_addr;
    uint8_t data[8];
    uint32_t len;
    uint8_t is_write;
};

struct kvm_run_internal {
    uint32_t suberror;
    uint32_t ndata;
    uint64_t data[16];
};

struct kvm_run_fail_entry {
    uint64_t hardware_entry_failure_reason;
    uint32_t cpu;
};

struct kvm_run_debug {
    uint32_t exception;
    uint32_t pad;
    uint64_t pc;
    uint64_t dr6;
    uint64_t dr7;
};

/* KVM_EXIT_IOAPIC_EOI: the vector of a level-triggered interrupt the
 * guest ended, for the caller's I/O APIC. */
struct kvm_run_eoi {
    uint8_t vector;
};

struct kvm_run_system_event {
    uint32_t type;
    uint32_t ndata;
    uint64_t data[16];
};

/* KVM_EXIT_X86_RDMSR and KVM_EXIT_X86_WRMSR: a guest's access to the MSR
 * `index` that the caller asked to see (KVM_CAP_X86_USER_SPACE_MSR) for
 * the `reason` KVM_MSR_EXIT_REASON_*; the caller answers a read in `data`,
 * and either with `error` set for a #GP, at the next KVM_RUN. */
#define KVM_MSR_EXIT_REASON_INVAL      (1u << 0)
#define KVM_MSR_EXIT_REASON_UNKNOWN    (1u << 1)
#define KVM_MSR_EXIT_REASON_FILTER     (1u << 2)
#define KVM_MSR_EXIT_REASON_VALID_MASK 0x7u

struct kvm_run_msr {
    uint8_t error;
    uint8_t pad[7];
    uint32_t reason;
    uint32_t index;
    uint64_t data;
};

/* KVM_X86_SET_MSR_FILTER: up to 16 ranges, each `nmsrs` MSRs from `base`
 * with a bit each in `bitmap`, 1 to allow the accesses `flags` names and
 * 0 to deny them; an access no range names is allowed unless the filter
 * is KVM_MSR_FILTER_DEFAULT_DENY. */
#define KVM_MSR_FILTER_MAX_BITMAP_SIZE  0x600u
#define KVM_MSR_FILTER_MAX_RANGES       16u
#define KVM_MSR_FILTER_READ             (1u << 0)
#define KVM_MSR_FILTER_WRITE            (1u << 1)
#define KVM_MSR_FILTER_RANGE_VALID_MASK 0x3u
#define KVM_MSR_FILTER_DEFAULT_ALLOW    0u
#define KVM_MSR_FILTER_DEFAULT_DENY     (1u << 0)
#define KVM_MSR_FILTER_VALID_MASK       0x1u

struct kvm_msr_filter_range {
    uint32_t flags;
    uint32_t nmsrs;
    uint32_t base;
    uint32_t pad;
    uint64_t bitmap; /* the caller's pointer */
};

struct kvm_msr_filter {
    uint32_t flags;
    uint32_t pad;
    struct kvm_msr_filter_range ranges[KVM_MSR_FILTER_MAX_RANGES];
};

/* KVM_CAP_X2APIC_API's flags: x2APIC ids of 32 bits in KVM_GET_LAPIC,
 * KVM_SET_LAPIC and an MSI's address, and no broadcast of a device's
 * interrupt to 0xFF in x2APIC mode. */
#define KVM_X2APIC_API_USE_32BIT_IDS           (1u << 0)
#define KVM_X2APIC_API_DISABLE_BROADCAST_QUIRK (1u << 1)
#define KVM_X2APIC_API_VALID_FLAGS             0x3u

/* KVM_EXIT_HYPERV: a write of the guest's to a SynIC MSR (`synic`), a
 * hypercall the kernel leaves to the caller, which writes its `result`
 * (`hcall`), or a write to a synthetic debugger MSR (`syndbg`). */
#define KVM_EXIT_HYPERV_SYNIC  1
#define KVM_EXIT_HYPERV_HCALL  2
#define KVM_EXIT_HYPERV_SYNDBG 3

struct kvm_hyperv_exit {
    uint32_t type;
    uint32_t pad1;
    union {
        struct {
            uint32_t msr;
            uint32_t pad2;
            uint64_t control;
            uint64_t evt_page;
            uint64_t msg_page;
        } synic;
        struct {
            uint64_t input;
            uint64_t result;
            uint64_t params[2];
        } hcall;
        struct {
            uint32_t msr;
            uint32_t pad2;
            uint64_t control;
            uint64_t status;
            uint64_t send_page;
            uint64_t recv_page;
            uint64_t pending_page;
        } syndbg;
    } u;
};

#define KVM_RUN_EXIT_BYTES 256u

/* struct kvm_run, the first page of a vCPU's mapping. */
struct kvm_run {
    uint8_t request_interrupt_window;
    uint8_t immediate_exit;
    uint8_t padding1[6];
    uint32_t exit_reason;
    uint8_t ready_for_interrupt_injection;
    uint8_t if_flag;
    uint16_t flags;
    uint64_t cr8;
    uint64_t apic_base;
    union {
        uint64_t hardware_exit_reason;
        struct kvm_run_fail_entry fail_entry;
        struct kvm_run_io io;
        struct kvm_run_mmio mmio;
        struct kvm_run_internal internal;
        struct kvm_run_debug debug;
        struct kvm_run_system_event system_event;
        struct kvm_run_eoi eoi;
        struct kvm_hyperv_exit hyperv;
        struct kvm_run_msr msr;
        char padding[KVM_RUN_EXIT_BYTES];
    } u;
    uint64_t kvm_valid_regs; /* KVM_SYNC_X86_*: the sets KVM_RUN writes to `s` */
    uint64_t kvm_dirty_regs; /* KVM_SYNC_X86_*: the sets KVM_RUN takes from `s` */
    union {
        struct kvm_sync_regs regs;
        char padding[2048];
    } s;
};

#endif /* _KVM_ABI_H_ */
