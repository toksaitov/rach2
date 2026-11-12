/*
 * kvm.h: the kernel's side of /dev/kvm, the VM and vCPU records, the
 * operations a processor's backend (SVM or VMX) gives, the slot lookup, and
 * the calls each file of rach2/sys/kvm/ offers the others. Readme.md has
 * the design and the lock order.
 */
#ifndef _KVM_H_
#define _KVM_H_

#include "stats.h"
#include "../lib/kint.h"
#include "kvm_abi.h"
#include "decode.h"
#include "pmu.h"
#include "../io/anon.h"
#include "../proc/wait_queue.h"

struct eventfd;
struct fd_entry;
struct kvm_vm;
struct kvm_vcpu;

/* The most vCPUs a VM has, and the bound of their ids, which
 * KVM_CAP_MAX_VCPUS and KVM_CAP_MAX_VCPU_ID report: Linux's 1024 and
 * 4096. An id past 255 is an x2APIC id, and the xAPIC id is its low byte. */
#define KVM_MAX_VCPUS    1024
#define KVM_MAX_VCPU_IDS 4096

/* A set of a VM's vCPUs by their places in `vcpus`, a bit each. */
#define KVM_VCPU_WORDS         ((KVM_MAX_VCPUS + 63) / 64)
#define KVM_VCPU_SET(map, i)   ((map)[(i) / 64u] |= 1ull << ((i) % 64u))
#define KVM_VCPU_CLEAR(map, i) ((map)[(i) / 64u] &= ~(1ull << ((i) % 64u)))
#define KVM_VCPU_TEST(map, i)  (((map)[(i) / 64u] >> ((i) % 64u)) & 1u)

/* Whether `vm` keeps the quirk `quirk` (KVM_X86_QUIRK_*), which it does
 * until KVM_CAP_DISABLE_QUIRKS2 turns it off, as Linux's
 * kvm_check_has_quirk. */
#define KVM_QUIRK(vm, quirk) (!((vm)->disabled_quirks & (quirk)))

/* An APIC timer's tick in ns before KVM_CAP_X86_APIC_BUS_CYCLES_NS sets
 * another, Linux's APIC_BUS_CYCLE_NS_DEFAULT: a bus of 1 GHz. */
#define KVM_APIC_BUS_NS_DEFAULT 1

/* The memory slots a VM has, KVM_CAP_NR_MEMSLOTS. */
#define KVM_MEM_SLOTS 256

/* The CPUID entries KVM_SET_CPUID2 takes and KVM_GET_SUPPORTED_CPUID
 * gives at most, Linux's KVM_MAX_CPUID_ENTRIES. */
#define KVM_MAX_CPUID_ENTRIES 256

/* The MSRs one KVM_GET_MSRS or KVM_SET_MSRS moves at most, Linux's
 * KVM_MAX_MSR_ENTRIES. */
#define KVM_MAX_MSR_ENTRIES 256

/* The pages of a vCPU's own in its mapping: the run page and the page of
 * port I/O data. The mapping has one more, the VM's ring of coalesced
 * writes, so KVM_GET_VCPU_MMAP_SIZE is KVM_MMAP_PAGES pages. */
#define KVM_RUN_PAGES  2u
#define KVM_MMAP_PAGES 3u

/*
 * The statistics of a VM and of a vCPU that KVM_GET_STATS_FD's file gives,
 * a 64-bit value each, in the order of Linux's struct kvm_vm_stat and
 * struct kvm_vcpu_stat; a histogram of halt times has KVM_HALT_POLL_HIST
 * values. Rach2 counts the ones the Readme names, and the rest read 0.
 */
#define KVM_HALT_POLL_HIST 32

enum kvm_vm_stat {
    KVM_VM_STAT_REMOTE_TLB_FLUSH,
    KVM_VM_STAT_REMOTE_TLB_FLUSH_REQUESTS,
    KVM_VM_STAT_MMU_SHADOW_ZAPPED,
    KVM_VM_STAT_MMU_PTE_WRITE,
    KVM_VM_STAT_MMU_PDE_ZAPPED,
    KVM_VM_STAT_MMU_FLOODED,
    KVM_VM_STAT_MMU_RECYCLED,
    KVM_VM_STAT_MMU_CACHE_MISS,
    KVM_VM_STAT_MMU_UNSYNC,
    KVM_VM_STAT_PAGES_4K,
    KVM_VM_STAT_PAGES_2M,
    KVM_VM_STAT_PAGES_1G,
    KVM_VM_STAT_NX_LPAGE_SPLITS,
    KVM_VM_STAT_MAX_MMU_PAGE_HASH_COLLISIONS,
    KVM_VM_STAT_MAX_MMU_RMAP_SIZE,
    KVM_VM_STATS
};

enum kvm_vcpu_stat {
    KVM_VCPU_STAT_HALT_SUCCESSFUL_POLL,
    KVM_VCPU_STAT_HALT_ATTEMPTED_POLL,
    KVM_VCPU_STAT_HALT_POLL_INVALID,
    KVM_VCPU_STAT_HALT_WAKEUP,
    KVM_VCPU_STAT_HALT_POLL_SUCCESS_NS,
    KVM_VCPU_STAT_HALT_POLL_FAIL_NS,
    KVM_VCPU_STAT_HALT_WAIT_NS,
    KVM_VCPU_STAT_HALT_POLL_SUCCESS_HIST,
    KVM_VCPU_STAT_HALT_POLL_FAIL_HIST = KVM_VCPU_STAT_HALT_POLL_SUCCESS_HIST + KVM_HALT_POLL_HIST,
    KVM_VCPU_STAT_HALT_WAIT_HIST = KVM_VCPU_STAT_HALT_POLL_FAIL_HIST + KVM_HALT_POLL_HIST,
    KVM_VCPU_STAT_BLOCKING = KVM_VCPU_STAT_HALT_WAIT_HIST + KVM_HALT_POLL_HIST,
    KVM_VCPU_STAT_PF_TAKEN,
    KVM_VCPU_STAT_PF_FIXED,
    KVM_VCPU_STAT_PF_EMULATE,
    KVM_VCPU_STAT_PF_SPURIOUS,
    KVM_VCPU_STAT_PF_FAST,
    KVM_VCPU_STAT_PF_MMIO_SPTE_CREATED,
    KVM_VCPU_STAT_PF_GUEST,
    KVM_VCPU_STAT_TLB_FLUSH,
    KVM_VCPU_STAT_INVLPG,
    KVM_VCPU_STAT_EXITS,
    KVM_VCPU_STAT_IO_EXITS,
    KVM_VCPU_STAT_MMIO_EXITS,
    KVM_VCPU_STAT_SIGNAL_EXITS,
    KVM_VCPU_STAT_IRQ_WINDOW_EXITS,
    KVM_VCPU_STAT_NMI_WINDOW_EXITS,
    KVM_VCPU_STAT_L1D_FLUSH,
    KVM_VCPU_STAT_HALT_EXITS,
    KVM_VCPU_STAT_REQUEST_IRQ_EXITS,
    KVM_VCPU_STAT_IRQ_EXITS,
    KVM_VCPU_STAT_HOST_STATE_RELOAD,
    KVM_VCPU_STAT_FPU_RELOAD,
    KVM_VCPU_STAT_INSN_EMULATION,
    KVM_VCPU_STAT_INSN_EMULATION_FAIL,
    KVM_VCPU_STAT_HYPERCALLS,
    KVM_VCPU_STAT_IRQ_INJECTIONS,
    KVM_VCPU_STAT_NMI_INJECTIONS,
    KVM_VCPU_STAT_REQ_EVENT,
    KVM_VCPU_STAT_NESTED_RUN,
    KVM_VCPU_STAT_DIRECTED_YIELD_ATTEMPTED,
    KVM_VCPU_STAT_DIRECTED_YIELD_SUCCESSFUL,
    KVM_VCPU_STAT_PREEMPTION_REPORTED,
    KVM_VCPU_STAT_PREEMPTION_OTHER,
    KVM_VCPU_STAT_GUEST_MODE,
    KVM_VCPU_STAT_NOTIFY_WINDOW_EXITS,
    KVM_VCPU_STATS
};

/*
 * The parts of a vCPU's state, as bits. The vCPU record keeps each part in
 * the form the ioctls give it (`struct kvm_vcpu`); the backend keeps the
 * same state in its control block while the guest runs. A set ioctl writes
 * the record and marks the part in `dirty`, and the backend loads each
 * dirty part before the next entry and clears its bit. A get ioctl first
 * asks the backend to write back the parts it reads (`save`).
 */
#define KVM_PART_REGS   0x001u
#define KVM_PART_SREGS  0x002u
#define KVM_PART_FPU    0x004u /* the XSAVE image and XCR0 */
#define KVM_PART_EVENTS 0x008u
#define KVM_PART_DEBUG  0x010u /* the debug registers and guest debugging */
#define KVM_PART_MSRS   0x020u
#define KVM_PART_MP     0x040u
#define KVM_PART_CPUID  0x080u
#define KVM_PART_TSC    0x100u /* the TSC's offset */
#define KVM_PART_CR8    0x200u /* CR8 alone, which KVM_RUN takes from the run page */
#define KVM_PART_ALL    0x3FFu

/*
 * The kinds of exit a backend's `exit` gives the run loop, in the vCPU's
 * `exit` record. The loop re-enters for KVM_X_AGAIN and answers the rest:
 * port I/O and MMIO go to the caller unless the local APIC's page or the
 * in-kernel bus takes them (vlapic.c, iobus.c), HLT goes to the caller
 * unless the in-kernel local APIC waits for an interrupt (vlapic.c), an
 * interrupt window goes to the caller only when it asked for one, and the
 * others go to the caller as their KVM_EXIT_* reasons.
 */
enum kvm_exit_kind {
    KVM_X_AGAIN = 0,    /* handled in the kernel; enter again */
    KVM_X_IO,           /* port I/O */
    KVM_X_MMIO,         /* an access to guest-physical memory with no slot */
    KVM_X_HLT,          /* the guest halted with interrupts enabled */
    KVM_X_SHUTDOWN,     /* a triple fault */
    KVM_X_IRQ_WINDOW,   /* the window the caller asked for is open */
    KVM_X_INTERNAL,     /* an exit the backend cannot handle */
    KVM_X_FAIL_ENTRY,   /* the processor refused the entry */
    KVM_X_DEBUG,        /* a debug exception under KVM_SET_GUEST_DEBUG */
    KVM_X_SYSTEM_EVENT, /* a reset or shutdown the guest asked for */
    KVM_X_HYPERV,       /* a Hyper-V hypercall the caller answers */
    KVM_X_MSR           /* an RDMSR or WRMSR the caller answers */
};

/*
 * One exit, as the backend reports it. For KVM_X_IO, `io` and, for an OUT,
 * `count` * `size` bytes in `data`, which the loop copies to the run's
 * port I/O page; for KVM_X_MMIO, `mmio`; for KVM_X_INTERNAL, `internal`
 * as Linux's kvm_run.internal; for KVM_X_FAIL_ENTRY, the processor's reason
 * in `hw_reason`; for KVM_X_DEBUG, `debug`; for KVM_X_SYSTEM_EVENT,
 * `system_event`; for KVM_X_HYPERV, `hyperv`; for KVM_X_MSR, `msr`. A
 * port or MMIO read the caller answers comes back to the backend through
 * `complete` before the next entry; an MSR's answer goes to
 * `kvm_msr_complete`.
 */
struct kvm_exit {
    enum kvm_exit_kind kind;
    struct kvm_run_io io;
    struct kvm_run_mmio mmio;
    struct kvm_run_internal internal;
    struct kvm_run_debug debug;
    struct kvm_run_system_event system_event;
    struct kvm_hyperv_exit hyperv;
    struct {
        int write;       /* WRMSR, else RDMSR */
        uint32_t index;  /* ECX */
        uint32_t reason; /* KVM_MSR_EXIT_REASON_* */
        uint32_t len;    /* the instruction's bytes, which the answer skips */
        uint64_t data;   /* EDX:EAX of a WRMSR */
    } msr;
    uint64_t hw_reason;
    uint8_t data[4096]; /* the bytes of an OUT, or of an IN once answered */
};

/*
 * A guest-physical page table entry, in the format the backend gives:
 * EPT's on VMX, the AMD64 long-mode format on SVM. The guest-physical
 * tables (tdp.c) build entries through these masks alone, so one walker
 * serves both. An entry is present when any bit of `present` is set; a
 * leaf grants a read with `read`, a write with `write`, and an execution
 * with `exec` on EPT or by leaving `nx` clear on SVM, whose entries also
 * need `user`, since the processor walks them as user accesses. `large`
 * marks a 2 MiB or 1 GiB leaf, `addr` is the mask of the frame's address,
 * `accessed` and `dirty` are the bits the processor sets, and `cache(type,
 * ignore_guest_pat)` gives the bits of the memory type KVM_MT_* in a leaf of
 * either size, with the guest's PAT ignored or in charge; they never include
 * the PAT bit of a 4 KiB leaf, which is a large leaf's `large`. A table
 * entry has every one of `read`, `write`, `exec` and `user`.
 */
#define KVM_MT_UC 0
#define KVM_MT_WC 1
#define KVM_MT_WB 6
struct kvm_tdp_format {
    uint64_t present, read, write, exec, nx, user, large, addr, accessed, dirty;
    uint64_t (*cache)(int type, int ignore_guest_pat);
};

/*
 * The operations of a processor's backend, svm.c or vmx.c, which the
 * kernel picks at boot by `probe`. Each runs under the big lock with
 * interrupts on unless it says otherwise.
 *
 * `probe` answers whether the processor has the extension and the
 * firmware leaves it usable: 1 or 0. `vm_init` and `vm_free` make and free
 * the backend's share of a VM; `vcpu_init` and `vcpu_free` its share of a
 * vCPU, `vcpu_init` returning 0 or -errno. `enter` runs the guest from the
 * state the vCPU's record and its backend's block hold until the next exit:
 * it is called with interrupts off and without the big lock, takes no
 * lock and does not sleep, and returns with interrupts off, leaving the
 * host's interrupts pending until the loop turns them on, 0, or -errno for
 * an entry it cannot make. Before the entry it clears KVM_REQ_TLB_FLUSH
 * (`kvm_vcpu_test_clear`) and, when it was set, drops the processor's
 * guest-physical translations of the VM, as it does too when it enters on
 * a processor other than the one it last entered on, whose translations
 * may predate a flush; it reads the tables at `kvm_tdp_root`. `exit` then
 * turns that exit into `v->exit`, under the big lock with interrupts on,
 * and may sleep: 0, or -errno to end
 * KVM_RUN with that error. `complete` gives the backend the answer to the
 * port or MMIO read of `v->exit` before the next entry. `save` writes the
 * parts `parts` of the backend's state into the vCPU's record. `inject`
 * makes the event the record's `events` holds pending for the next entry,
 * so an interrupt, an NMI or an exception reaches the guest at its next
 * instruction boundary; with the local APIC in the kernel and no interrupt
 * in `events`, it also asks `kvm_lapic_pending` for the vector waiting,
 * injects the one `kvm_lapic_ack` gives when the guest can take an
 * interrupt at the entry, recording it in `events.interrupt`, and
 * otherwise arms the processor's interrupt-window exit, which it reports
 * as KVM_X_IRQ_WINDOW. It arms that exit, too, while the caller's run page
 * asks for the window. `inject` also runs after KVM_SET_SREGS, so that a
 * backend whose processor takes PAE's PDPTEs from its own state can read
 * them from guest memory there. A guest's RDMSR and WRMSR of the APIC
 * base, the TSC deadline and the x2APIC registers go to `kvm_lapic_msr`,
 * and a backend that lets the guest write CR8 without an exit gives the
 * value to `kvm_lapic_set_cr8` after each exit and loads `kvm_lapic_cr8`'s
 * before each entry, since CR8 is the APIC's TPR. A guest's VMCALL or
 * VMMCALL goes to `kvm_hv_hypercall` while `kvm_hv_hypercall_enabled`; the
 * flush KVM_REQ_TLB_FLUSH asks for must drop the guest's own translations
 * as well, since the Hyper-V TLB flush hypercalls ask for it to flush the
 * guest's TLB. `check_extension` answers
 * the capabilities whose truth depends on the processor, such as
 * KVM_CAP_X86_ROBUST_SINGLESTEP: the value KVM_CHECK_EXTENSION gives, 0
 * for none. `tdp` is the format of its guest-physical entries.
 * `tsc_scaling`, NULL for a backend without it, gives the largest ratio
 * the processor scales a guest's TSC by, with the width of its fraction
 * in `*frac_bits`, or 0 when the processor does not scale; a backend that
 * scales loads the vCPU's `tsc_ratio`, or 1 for none, with KVM_PART_TSC.
 * `msr_filter_changed`, which the run loop calls before an entry after
 * KVM_X86_SET_MSR_FILTER replaced the VM's filter (KVM_REQ_MSR_FILTER),
 * and which `vcpu_init` does itself, has the guest's access exit for each
 * MSR the processor lets it reach directly that the filter denies, as
 * Linux's msr_filter_changed; an exit for such an MSR the filter allows
 * came before the change, and the instruction runs again unanswered.
 * `xsaves`, NULL for a backend that cannot give it, is 1 where a guest
 * whose CPUID offers XSAVES may run XSAVES and XRSTORS: VMX's secondary
 * control, or on SVM the host's XSAVES alone, which the guest runs
 * whenever it has XSAVE, as no intercept or control stops it.
 */
struct kvm_backend {
    const char *name;
    int (*probe)(void);
    int (*vm_init)(struct kvm_vm *vm);
    void (*vm_free)(struct kvm_vm *vm);
    int (*vcpu_init)(struct kvm_vcpu *v);
    void (*vcpu_free)(struct kvm_vcpu *v);
    int (*enter)(struct kvm_vcpu *v);
    int (*exit)(struct kvm_vcpu *v);
    void (*complete)(struct kvm_vcpu *v);
    void (*save)(struct kvm_vcpu *v, uint32_t parts);
    void (*inject)(struct kvm_vcpu *v);
    int32_t (*check_extension)(long cap);
    const struct kvm_tdp_format *tdp;
    uint64_t (*tsc_scaling)(uint32_t *frac_bits);
    void (*msr_filter_changed)(struct kvm_vcpu *v);
    int (*xsaves)(void);
};

/* The two backends, svm.c's and vmx.c's. */
extern const struct kvm_backend kvm_svm;
extern const struct kvm_backend kvm_vmx;

/* The backend `kvm_start` chose, or NULL when neither probed. */
extern const struct kvm_backend *kvm_backend;

/*
 * A VM. `refs` counts the VM's descriptors, its vCPUs and the mappings of
 * their run pages; the last `kvm_vm_put` frees it. The vCPUs are in
 * `vcpus`, by the order of their creation, and `nr_vcpus` of them are
 * live; a vCPU is never removed before the VM is freed.
 */
struct kvm_vm {
    struct anon_obj anon; /* "kvm-vm", for /proc/PID/fd */
    uint32_t refs;
    struct kvm_vcpu *vcpus[KVM_MAX_VCPUS];
    uint32_t nr_vcpus;
    uint64_t tss_addr;    /* KVM_SET_TSS_ADDR */
    uint64_t ident_addr;  /* KVM_SET_IDENTITY_MAP_ADDR; 0xFEFFC000 by default */
    int64_t clock_offset; /* the guest's kvmclock less the host's monotonic ns */
    /* The kvmclock's master clock (pvclock.c): while `master_clock` is set,
     * the kvmclock at the host's TSC count t is master_ns + clock_offset
     * plus t - master_tsc at the TSC's rate, `master_mul` and
     * `master_shift` in the kvmclock's form, the same on every vCPU. */
    int master_clock;
    uint64_t master_ns;  /* the host's monotonic ns at `master_tsc` */
    uint64_t master_tsc; /* the host's TSC count */
    uint32_t master_mul;
    int8_t master_shift;
    uint64_t wall_clock; /* the last value of MSR_KVM_WALL_CLOCK(_NEW) */
    int old_kvmclock;    /* the boot vCPU's guest uses MSR_KVM_SYSTEM_TIME */
    /* The vCPUs' TSCs as Linux's kvm_synchronize_tsc matches them: the
     * generation of matched writes, its offset, the vCPUs that joined it
     * after its first, and the last write's ns, value, rate and offset. */
    int user_set_tsc;
    uint32_t tsc_gen;
    uint32_t tsc_matched;
    int64_t tsc_cur_offset;
    int64_t tsc_last_offset;
    uint64_t tsc_last_ns, tsc_last_write, tsc_last_khz;
    uint64_t tsc_cur_ns, tsc_cur_write; /* the generation's start: a moment, the TSC then */
    uint32_t disable_exits;             /* KVM_CAP_X86_DISABLE_EXITS: KVM_X86_DISABLE_EXITS_* */
    uint32_t max_vcpu_ids;       /* KVM_CAP_MAX_VCPU_ID: ids are below it; 0 until it is settled */
    uint32_t disabled_quirks;    /* KVM_X86_QUIRK_* turned off (KVM_QUIRK) */
    int exit_on_emulation_error; /* KVM_CAP_EXIT_ON_EMULATION_FAILURE */
    int platform_info_hidden;    /* KVM_CAP_MSR_PLATFORM_INFO 0: the guest's read is #GP */
    uint32_t user_space_msr;     /* KVM_CAP_X86_USER_SPACE_MSR: KVM_MSR_EXIT_REASON_* */
    void *msr_filter;            /* KVM_X86_SET_MSR_FILTER's, msr.c's; NULL for none */
    int x2apic_format;           /* KVM_X2APIC_API_USE_32BIT_IDS */
    int x2apic_no_broadcast;     /* KVM_X2APIC_API_DISABLE_BROADCAST_QUIRK */
    uint64_t apic_bus_ns;        /* KVM_CAP_X86_APIC_BUS_CYCLES_NS: an APIC timer's tick in ns */
    uint32_t default_tsc_khz;    /* KVM_SET_TSC_KHZ of the VM: the rate a new vCPU's TSC has */
    int irqchip;                 /* KVM_IRQCHIP_*: which interrupt controller is in the kernel */
    uint32_t boot_cpu_id;        /* KVM_SET_BOOT_CPU_ID */
    uint32_t dirty_manual;       /* KVM_CAP_MANUAL_DIRTY_LOG_PROTECT2's options, dirty.c's */
    uint32_t noncoherent;        /* devices assigned that do not snoop (`kvm_tdp_noncoherent`) */
    void *slots;                 /* slots.c's */
    void *tdp;                   /* tdp.c's */
    void *irq;                   /* irq.c's */
    void *pic;                   /* i8259.c's, with the full irqchip */
    void *ioapic;                /* ioapic.c's, with the full irqchip */
    void *pit;                   /* i8254.c's, after KVM_CREATE_PIT or PIT2 */
    void *iobus;                 /* iobus.c's */
    struct kvm_coalesced_mmio_ring *ring; /* the page of coalesced writes, every vCPU's third */
    void *vfio;                           /* vfio.c's bridge; holds a VM reference */
    void *hv;                             /* hyperv.c's */
    void *pmu_filter;                     /* pmu.c's event filter, read under the big lock */
    int pmu_disabled;                     /* KVM_CAP_PMU_CAPABILITY, set before vCPUs exist */
    void *backend;                        /* the backend's */
    void *gpamap;                /* gpamap.c's: the maps of the guest's pages into programs */
    struct kvm_vm *next_vm;      /* kvm.c's list of live VMs */
    uint64_t stat[KVM_VM_STATS]; /* by enum kvm_vm_stat */
    char stats_id[KVM_STATS_NAME_SIZE]; /* "kvm-PID", its maker's */
};

#define KVM_IRQCHIP_NONE   0
#define KVM_IRQCHIP_SPLIT  1
#define KVM_IRQCHIP_KERNEL 2

/* How the vCPU stands, for a kick from another processor. */
#define KVM_MODE_OUTSIDE  0 /* in the kernel, or in the caller */
#define KVM_MODE_IN_GUEST 1 /* between the check of its requests and the exit */

/* The requests another thread leaves a vCPU, in `requests`, which the run
 * loop answers before each entry. */
#define KVM_REQ_EXIT         0x1u   /* go back to the caller */
#define KVM_REQ_EVENT        0x2u   /* an event to inject */
#define KVM_REQ_TRIPLE_FAULT 0x4u   /* end in KVM_EXIT_SHUTDOWN */
#define KVM_REQ_EOI_EXIT     0x8u   /* end in KVM_EXIT_IOAPIC_EOI (vlapic.c) */
#define KVM_REQ_TLB_FLUSH    0x10u  /* the backend drops the VM's guest-physical translations */
#define KVM_REQ_HV_EXIT      0x20u  /* end in KVM_EXIT_HYPERV (hyperv.c) */
#define KVM_REQ_HV_CRASH     0x40u  /* end in KVM_SYSTEM_EVENT_CRASH (hyperv.c) */
#define KVM_REQ_HV_RESET     0x80u  /* end in KVM_SYSTEM_EVENT_RESET (hyperv.c) */
#define KVM_REQ_CLOCK_UPDATE 0x100u /* write the kvmclock again (pvclock.c) */
#define KVM_REQ_STEAL_UPDATE 0x200u /* write the steal time again (pvclock.c) */
#define KVM_REQ_MSR_FILTER   0x400u /* the backend's `msr_filter_changed` */
#define KVM_REQ_PMU          0x800u /* refresh the event filter before entry */

/* The most MSRs a vCPU keeps, those of the index list, the MTRRs and the
 * machine-check banks. */
#define KVM_VCPU_MSRS 192

/*
 * A port I/O instruction as the backend's exit gives it, for
 * `kvm_emul_pio`: the port, the element's bytes, IN or OUT, the string and
 * REP forms, OUTS's segment (KVM_SEG_*), the address size of the string's
 * registers, and the instruction's length. `count` is the elements the
 * exit moves, which `kvm_emul_pio` sets.
 */
struct kvm_pio {
    uint16_t port;
    uint32_t size; /* 1, 2 or 4 */
    int in;
    int string;
    int rep;
    uint32_t seg;
    uint32_t addr_size; /* 2, 4 or 8 */
    uint32_t len;
    uint32_t count;
};

/* What an instruction under way keeps: the answers of its MMIO reads,
 * its writes, and the pages those writes were translated to. */
#define KVM_EMUL_READS  40
#define KVM_EMUL_WRITES 8
#define KVM_EMUL_CHUNKS 16
#define KVM_EMUL_WBUF   4104 /* a page of INS's elements, and one across its end */
#define KVM_EMUL_FX     576  /* an XSAVE image's legacy area and header */

/* The answer of one MMIO read: `len` bytes at `gpa`, 8 at most. */
struct kvm_emul_read {
    uint64_t gpa;
    uint32_t len;
    uint8_t data[8];
};

/* One write an instruction queued: `len` bytes of `wbuf` from `off`, to
 * the linear address `la`, in pieces of `unit` bytes where it is MMIO. */
struct kvm_emul_write {
    uint64_t la;
    uint32_t off, len, unit;
};

/* The part of a queued write that lies in one page, at `gpa`: RAM, which
 * is written at once, or MMIO, of which `done` bytes have left. */
struct kvm_emul_chunk {
    uint64_t gpa;
    uint32_t off, len, unit, done;
    int mmio;
};

/*
 * The guest instruction emulate.c has under way across exits to the
 * caller: an instruction that waits for the answer of an MMIO read, after
 * which it runs again from its start with the answers kept in `rd`; one
 * that has ended and has MMIO writes left to deliver; a port read that
 * waits for its answer; or an OUT whose bytes are out and whose RIP is
 * stepped once the write is done. While `resume` is set the backend's
 * next `enter` enters no guest, and its `exit` gives `kvm_emul_resume`'s
 * exit.
 */
struct kvm_emul {
    uint32_t stage;          /* emulate.c's EMUL_* */
    int resume;              /* the next exit is the instruction's next step */
    int ud;                  /* a #UD exit brought the instruction */
    uint8_t fx[KVM_EMUL_FX]; /* the x87 and SSE state as the pass found it */
    struct kvm_insn insn;    /* the instruction */
    struct kvm_emul_read rd[KVM_EMUL_READS];
    uint32_t rcount; /* the answers kept; the read that waits is the next */
    struct kvm_emul_write w[KVM_EMUL_WRITES];
    uint32_t nw;
    struct kvm_emul_chunk chunk[KVM_EMUL_CHUNKS];
    uint32_t nchunk, wcur; /* the chunks, and the first that may have MMIO left */
    uint8_t wbuf[KVM_EMUL_WBUF];
    uint32_t wlen;
    uint64_t info[5];   /* the exit's reason and qualifications, for an internal error */
    int step_trap;      /* an instruction was stepped under KVM_GUESTDBG_SINGLESTEP */
    struct kvm_pio pio; /* the port instruction */
    uint64_t out_rip;   /* the linear RIP of an OUT whose RIP is not yet stepped */
    int fail;           /* EMUL_FAIL's: emulate.c's X_FAIL, or -EFAULT */
};

/*
 * A vCPU. Its descriptor holds a reference to the VM, and so does each
 * mapping of its run pages, through the vCPU (`maps`); the VM frees the
 * vCPU with itself. The state is in the forms of the ioctls; `dirty` and
 * `save` keep it and the backend's block in step (KVM_PART_*).
 */
struct kvm_vcpu {
    struct anon_obj anon; /* `name`, for /proc/PID/fd */
    char name[24];        /* "kvm-vcpu:ID" */
    struct kvm_vm *vm;
    uint32_t id;                /* the caller's id, the APIC id */
    uint32_t index;             /* the place in the VM's `vcpus` */
    struct kvm_run *run;        /* the run page, the first of KVM_RUN_PAGES */
    uint8_t *pio;               /* the port I/O page, the second */
    volatile uint32_t mode;     /* KVM_MODE_* */
    volatile uint32_t requests; /* KVM_REQ_* */
    volatile int32_t cpu;       /* the processor it last entered on, -1 before */
    int busy;                   /* an ioctl of the vCPU is under way, KVM_RUN among them */
    int hlt_in_guest; /* the VM's KVM_X86_DISABLE_EXITS_HLT at creation: HLT does not exit */
    int has_run;      /* KVM_RUN has entered the guest */
    wait_queue_t wq;  /* the ioctls that wait for `busy` to clear */
    int pending;      /* `exit` is a read the caller answers in the run page */
    int msr_pending;  /* `exit` is KVM_X_MSR, which the caller answers */
    int has_sigmask;
    uint64_t sigmask; /* KVM_SET_SIGNAL_MASK, less SIGKILL and SIGSTOP */
    uint32_t dirty;   /* KVM_PART_* the backend has yet to load */

    struct kvm_regs regs;
    struct kvm_sregs sregs;
    uint8_t *xsave;  /* KVM_XSAVE_SIZE bytes, 64-byte aligned, in XSAVE's standard form */
    void *xsave_mem; /* what `xsave` was carved from */
    uint64_t xcr0;
    struct kvm_vcpu_events events;
    uint32_t nmi_queued; /* NMIs waiting, at most 2, as Linux keeps them */
    struct kvm_debugregs debugregs;
    struct kvm_guest_debug guest_debug;
    uint32_t mp_state;
    struct kvm_cpuid_entry2 *cpuid; /* KVM_SET_CPUID2's, or NULL */
    uint32_t cpuid_nent;
    int cpuid_set; /* KVM_SET_CPUID2 was taken, with entries or with none */
    uint32_t msr_count;
    uint32_t msr_index[KVM_VCPU_MSRS];
    uint64_t msr_value[KVM_VCPU_MSRS];
    int64_t tsc_offset;     /* the guest's TSC less the host's */
    uint32_t tsc_gen;       /* the VM's `tsc_gen` this vCPU's TSC last joined */
    uint32_t tsc_khz;       /* its TSC's rate, KVM_SET_TSC_KHZ's; 0 when none is known */
    uint64_t tsc_ratio;     /* the processor scales the host's TSC by it; 0 at the host's rate */
    uint32_t tsc_frac_bits; /* the bits of `tsc_ratio` that are its fraction */
    int tsc_catchup;        /* a rate above the host's, unscaled: kept before each entry */
    uint64_t tsc_this_ns, tsc_this_write; /* where its TSC counts from: a moment, its value */
    uint64_t pv_time;      /* MSR_KVM_SYSTEM_TIME(_NEW): the kvmclock's address, KVM_MSR_ENABLED */
    uint32_t pv_features;  /* KVM_CPUID_FEATURES' EAX in the CPUID, KVM_FEATURE_* bits */
    int pv_enforce;        /* KVM_CAP_ENFORCE_PV_FEATURE_CPUID: the guest has those alone */
    uint64_t poll_control; /* MSR_KVM_POLL_CONTROL */
    /* The steal time (pvclock.c): MSR_KVM_STEAL_TIME, its address with
     * KVM_MSR_ENABLED; the run_delay of the thread when it was last added,
     * Linux's last_steal; the thread's pcount when it was last read, whose
     * change means the thread was switched out and in; and whether the
     * guest's record says KVM_VCPU_PREEMPTED. */
    uint64_t st_msr;
    uint64_t st_last;
    unsigned long st_pcount;
    int st_preempted;
    int pv_stopped;    /* KVM_KVMCLOCK_CTRL: the next write sets PVCLOCK_GUEST_STOPPED */
    int32_t clock_cpu; /* the processor the kvmclock was last written on, -1 for none */
    uint64_t clock_ns; /* the host's ns at that write */
    uint64_t mcg_cap, mcg_status, mcg_ctl;
    uint64_t platform_info;        /* MSR_PLATFORM_INFO, which only the caller writes */
    uint64_t misc_features;        /* MSR_MISC_FEATURES_ENABLES: bit 0, CPUID faults above CPL 0 */
    uint64_t ucode_rev;            /* MSR_IA32_UCODE_REV, which only the caller writes */
    uint64_t hwcr;                 /* MSR_K7_HWCR: McStatusWrEn and TscFreqSel alone */
    uint64_t power_ctl;            /* MSR_IA32_POWER_CTL, which nothing reads */
    uint64_t stat[KVM_VCPU_STATS]; /* by enum kvm_vcpu_stat */
    char stats_id[KVM_STATS_NAME_SIZE];        /* "kvm-PID/vcpu-ID", its maker's */
    uint64_t mce_banks[KVM_MAX_MCE_BANKS * 4]; /* CTL, STATUS, ADDR, MISC of each */
    struct kvm_pmu pmu;

    /* Set by the backend after each exit, for the run page. */
    int if_flag;     /* RFLAGS.IF */
    int irq_allowed; /* an interrupt can be injected at the next entry */
    /* Set by the backend after an exit for a host interrupt, cleared at
     * the run loop's top after its switch, before the work of the next
     * entry, Linux's at_instruction_boundary: the guest stopped between
     * two instructions, so its record may say it was preempted. */
    int at_boundary;

    /* The processor's code for the last run's exit, which the backend's
     * `enter` writes: SVM's EXITCODE, low 32 bits, or VMX's exit reason. */
    uint32_t hw_exit;
    /* exitstat.c's account of the exit the kernel handles now: its row, or
     * -1 for none, and the TSC count since which its handling is timed. */
    int32_t xs_row;
    uint64_t xs_mark;

    /* The steps each vector's interrupts reached, by enum kvm_vec_step,
     * which the VM's fdinfo shows; written by the vCPU's thread and by a
     * delivery from another processor without a lock, so a count read
     * while the guest runs may lag by the steps under way. */
    uint32_t irq_counts[256][KVM_VEC_STEPS];

    struct kvm_exit exit;
    struct kvm_emul emul; /* emulate.c's */
    void *lapic;          /* vlapic.c's */
    void *hv;             /* hyperv.c's */
    void *backend;        /* the backend's */
};

/*
 * The calls between the files of rach2/sys/kvm/. Each file's stub, in the
 * task before the one that writes it, keeps its contract with the least it
 * can: a capability it does not make true answers 0, and an ioctl of its
 * own answers -ENOTTY, as Linux's does for an ioctl it does not have.
 */

/* kvm.c */

/** Register /dev/kvm when a backend probes; at boot, once. */
void kvm_start(void);

/* exitstat.c */

/** Register the debug file `kvm/exit_stats`; from `kvm_start`, once a
 * backend has probed. */
void kvm_exitstat_start(void);

/**
 * Count the exit of the run that just ended on `v`, by `v->hw_exit`: the
 * TSC count `left`, read as the guest's run ended, and `held`, read once
 * the big lock was taken back, time the wait for the lock. Under the big
 * lock, with interrupts off.
 */
void kvm_exitstat_exit(struct kvm_vcpu *v, uint64_t left, uint64_t held);

/** End the account of `v`'s exit, if one is open: the kernel enters the
 * guest again (`to_user` 0) or KVM_RUN returns (1). Under the big lock. */
void kvm_exitstat_done(struct kvm_vcpu *v, int to_user);

/** Whether the caller runs in the address space that made `vm`; Linux
 * answers EIO to an ioctl of a VM or its vCPUs from any other. */
int kvm_vm_caller_ok(struct kvm_vm *vm);

/** The address space whose memory `vm`'s slots map, its maker's; NULL for
 * a VM made by the kernel outside any process. */
struct mm *kvm_vm_mm(struct kvm_vm *vm);

/** Take a reference to `vm`. */
void kvm_vm_get(struct kvm_vm *vm);

/** Drop a reference to `vm`, freeing it, its vCPUs, its slots and the
 * backend's share with the last one. Under the big lock. */
void kvm_vm_put(struct kvm_vm *vm);

/** The live VM one of whose ioeventfds signals `ev`, the VM that rings a
 * doorbell of a transport's (vgpu-shm(4)), or NULL. No reference is taken:
 * the answer holds until the big lock is let go. Under the big lock. */
struct kvm_vm *kvm_vm_by_eventfd(const struct eventfd *ev);

/** The value KVM_CHECK_EXTENSION gives for `cap`, of /dev/kvm when `vm` is
 * NULL and of the VM `vm` otherwise; 0 for a capability not offered. */
int32_t kvm_check_extension(struct kvm_vm *vm, long cap);

/** Create the VFIO bridge of `vm`, from the user kvm_create_device at
 * `arg`: 0 or a negative errno. Called under the big lock after checking
 * the caller's address space. The descriptor holds a VM reference. */
int32_t kvm_vfio_create(struct kvm_vm *vm, uintptr_t arg);

/* binstats.c */

/** KVM_GET_STATS_FD: a descriptor of the caller's, read-only and
 * close-on-exec, on the statistics of `v`, or of `vm` when `v` is NULL:
 * "anon_inode:kvm-vcpu-stats:ID" or "anon_inode:kvm-vm-stats". It holds a
 * reference to the VM. The descriptor, -ENOMEM or `fd_alloc`'s error. */
int32_t kvm_stats_fd(struct kvm_vm *vm, struct kvm_vcpu *v);

/** Write at `id`, of KVM_STATS_NAME_SIZE bytes, the name the statistics
 * file gives the object the caller is making: "kvm-PID" with the caller's
 * id, and "/vcpu-ID" after it for the vCPU `v`. */
void kvm_stats_id(char *id, const struct kvm_vcpu *v);

/** Write the decimal `n` at `p`, with its terminating NUL, whose address
 * is returned. */
char *kvm_decimal(char *p, uint32_t n);

/** The host's kvmclock in nanoseconds, the monotonic clock. */
uint64_t kvm_host_ns(void);

/* vcpu.c */

/** KVM_CREATE_VCPU: a vCPU of `vm` with the id `id` and a descriptor of
 * the caller's on it, "anon_inode:kvm-vcpu:ID", read-write and
 * close-on-exec. The descriptor; -EINVAL for an id past KVM_MAX_VCPU_IDS
 * or the bound KVM_CAP_MAX_VCPU_ID set, or for a VM with KVM_MAX_VCPUS
 * vCPUs, -EEXIST for an id the VM has, -ENOMEM,
 * the backend's error, or `fd_alloc`'s. */
int32_t kvm_vcpu_create(struct kvm_vm *vm, uint32_t id);

/** Free `v`, its pages and the backend's share; when the VM is freed. */
void kvm_vcpu_free(struct kvm_vcpu *v);

/** Ask `v` to leave guest mode and see its requests: an interrupt to the
 * processor it runs on when it is in the guest there, and nothing more
 * when it is not, since the loop reads `requests` before each entry. The
 * request is set atomically, so an interrupt handler or a hook under
 * another subsystem's spinlock may kick, without the big lock. */
void kvm_vcpu_kick(struct kvm_vcpu *v, uint32_t request);

/** Clear the request `request` of `v` atomically: whether it was set. */
int kvm_vcpu_test_clear(struct kvm_vcpu *v, uint32_t request);

/** Give `v` the state an INIT leaves, as Linux's kvm_vcpu_reset with
 * `init_event`: the registers, segments, control registers, events and
 * debug registers of a reset, keeping the APIC base, the FPU's state, XCR0,
 * the MSRs but the kvmclock's and the steal time's, which are turned off,
 * and the TSC. vlapic.c resets the APIC itself. */
void kvm_vcpu_init_reset(struct kvm_vcpu *v);

/** The guest's physical address bits, from leaf 0x80000008 of `v`'s CPUID,
 * 36 without it, as Linux's cpuid_query_maxphyaddr. */
uint32_t kvm_guest_maxphyaddr(const struct kvm_vcpu *v);

/** The CR4 bits `v` may not set, as Linux's __kvm_is_valid_cr4: those the
 * CPUID KVM offers reserves, and once KVM_SET_CPUID2 is taken those `v`'s
 * CPUID reserves too. VMXE and SMXE always, since no nested guest is
 * offered. */
uint64_t kvm_cr4_reserved(const struct kvm_vcpu *v);

/** The vCPU ioctls, file_ops.ioctl of a vCPU's descriptor. */
int32_t kvm_vcpu_ioctl(struct fd_entry *e, uint32_t request, uintptr_t arg);

/* cpuid.c */

/** KVM_GET_SUPPORTED_CPUID's entries: up to `max` into `out`, from the
 * host's CPUID less what KVM does not offer a guest, and KVM's own two
 * leaves at KVM_CPUID_SIGNATURE. The count, or -E2BIG when more than `max`
 * are needed. */
int32_t kvm_cpuid_supported(struct kvm_cpuid_entry2 *out, uint32_t max);

/** Whether KVM_GET_SUPPORTED_CPUID offers the feature in bit `bit` of the
 * register `reg` (0 EAX to 3 EDX) of `function`'s subleaf 0. */
int kvm_cpuid_host_has(uint32_t function, int reg, int bit);

/** KVM_GET_EMULATED_CPUID's entries, as Linux's __do_cpuid_func_emulated
 * gives them, up to `max` into `out`: leaf 0 with the highest leaf, 7;
 * leaf 1 with MOVBE; and leaf 7 with RDPID where the host has RDTSCP,
 * the instructions emulate.c runs after the #UD of a processor that lacks
 * them. The count, 3, or -E2BIG. */
int32_t kvm_cpuid_emulated(struct kvm_cpuid_entry2 *out, uint32_t max);

/** The entry of `v`'s CPUID for `function` and `index`, or NULL. */
const struct kvm_cpuid_entry2 *kvm_cpuid_find(const struct kvm_vcpu *v, uint32_t function,
                                              uint32_t index);

/** The XCR0 bits the host offers a guest: those the kernel saves. */
uint64_t kvm_xcr0_supported(void);

/** The XCR0 bits `v`'s CPUID allows: leaf 0xD's within the host's, and
 * x87 always. */
uint64_t kvm_xcr0_guest(const struct kvm_vcpu *v);

/** The bytes of an XSAVE area that holds the components `features`, as
 * the host's CPUID 0xD places them in the standard form, or with
 * `compacted` in the compacted form of XSAVEC and XSAVES; 576 at least,
 * the legacy area and the header. */
uint32_t kvm_xsave_size(uint64_t features, int compacted);

/** Set the sizes of the `n` entries `e` of a CPUID for `v` that follow
 * its XCR0, as Linux's kvm_update_cpuid_runtime: EBX of leaf 0xD's
 * subleaf 0, and of subleaf 1 where it offers XSAVEC or XSAVES. A caller
 * calls it on `v`'s own entries whenever XCR0 changes, and on entries
 * KVM_SET_CPUID2 gives before it takes or compares them. */
void kvm_cpuid_xsave_sizes(const struct kvm_vcpu *v, struct kvm_cpuid_entry2 *e, uint32_t n);

/** KVM_GET_SUPPORTED_CPUID's KVM_CPUID_FEATURES bits: the kvmclock's
 * three where the TSC's rate was measured, the I/O delay hint, STEAL_TIME,
 * PV_UNHALT, PV_SEND_IPI, POLL_CONTROL and PV_SCHED_YIELD. */
uint32_t kvm_pv_supported(void);

/** Keep the features of KVM's leaves in `v`'s CPUID, found by their
 * signature, for `kvm_pv_has`, as Linux's kvm_update_pv_runtime; after
 * KVM_SET_CPUID2, `set`, first take PV_UNHALT out of them while HLT does
 * not exit, as Linux's kvm_vcpu_after_set_cpuid. */
void kvm_pv_cpuid_update(struct kvm_vcpu *v, int set);

/** Whether `v`'s guest may use KVM's paravirtual feature `bit`
 * (KVM_FEATURE_*): always, unless KVM_CAP_ENFORCE_PV_FEATURE_CPUID holds
 * it to the CPUID's, as Linux's guest_pv_has. */
int kvm_pv_has(const struct kvm_vcpu *v, int bit);

/* msr.c */

/** The MSRs of KVM_GET_MSR_INDEX_LIST, `n` of them, or of the feature
 * list for `feature`. */
const uint32_t *kvm_msr_list(int feature, uint32_t *n);

/** The value of the feature MSR `index` into `*value`: 0, or -1 for an
 * MSR not on the feature list. */
int kvm_msr_feature(uint32_t index, uint64_t *value);

/** Give `v` the MSRs of a vCPU at power-on. */
void kvm_msr_reset(struct kvm_vcpu *v);

/** Whether `v`'s guest has MWAIT: bit 3 of its CPUID's leaf 1 ECX, or,
 * with KVM_X86_QUIRK_MISC_ENABLE_NO_MWAIT turned off, bit 18 of its
 * MISC_ENABLE, which then decides that bit, as Linux's
 * kvm_update_cpuid_runtime. */
int kvm_msr_guest_mwait(const struct kvm_vcpu *v);

/* The failures of an MSR's access, as Linux's KVM_MSR_RET_*: a value or
 * an access refused (Linux's 1), an MSR not kept (KVM_MSR_RET_UNSUPPORTED)
 * and an access KVM_X86_SET_MSR_FILTER denies (KVM_MSR_RET_FILTERED). */
#define KVM_MSR_INVAL    (-1)
#define KVM_MSR_UNKNOWN  (-2)
#define KVM_MSR_FILTERED (-3)

/** Read the MSR `index` of `v`, as the caller of KVM_GET_MSRS when `host`
 * is set and as the guest otherwise, which Linux's host_initiated tells
 * apart: 0, KVM_MSR_INVAL or KVM_MSR_UNKNOWN. */
int kvm_msr_get(struct kvm_vcpu *v, uint32_t index, uint64_t *value, int host);

/** Write the MSR `index` of `v`, as `kvm_msr_get` reads it, with the
 * checks Linux makes of a write from the host or from the guest: 0,
 * KVM_MSR_INVAL or KVM_MSR_UNKNOWN. Marks KVM_PART_MSRS dirty. */
int kvm_msr_set(struct kvm_vcpu *v, uint32_t index, uint64_t value, int host);

/** Whether the guest of `v` may access the MSR `index`, a write when
 * `write` is set, under the VM's KVM_X86_SET_MSR_FILTER, as Linux's
 * kvm_msr_allowed: 1, or 0 when the filter denies it. The x2APIC's MSRs
 * are never filtered. */
int kvm_msr_allowed(const struct kvm_vcpu *v, uint32_t index, int write);

/** The KVM_MSR_EXIT_REASON_* of the failure `r`, a KVM_MSR_* one, when
 * KVM_CAP_X86_USER_SPACE_MSR asked for it on `v`'s VM, else 0: whether
 * `kvm_msr_user_exit` would give the access to the caller. */
uint32_t kvm_msr_user_reason(const struct kvm_vcpu *v, int r);

/**
 * Give the caller the guest's RDMSR or WRMSR of `index` that failed with
 * `r`, a KVM_MSR_* failure, when KVM_CAP_X86_USER_SPACE_MSR asked for that
 * reason, as Linux's kvm_msr_user_space: `v->exit` becomes KVM_X_MSR with
 * the write's `value` and the instruction's `len` bytes, and 1 is
 * returned; else 0, and the backend raises #GP.
 */
int kvm_msr_user_exit(struct kvm_vcpu *v, uint32_t index, int write, uint64_t value, int r,
                      uint32_t len);

/** At KVM_RUN, finish the RDMSR or WRMSR the last exit gave the caller:
 * #GP for the caller's `error`, else a read's `data` in EDX:EAX and the
 * instruction skipped, as Linux's complete_fast_rdmsr and
 * complete_fast_msr_access. Nothing when no MSR exit waits. */
void kvm_msr_complete(struct kvm_vcpu *v);

/** KVM_X86_SET_MSR_FILTER of `vm`, as Linux's
 * kvm_vm_ioctl_set_msr_filter: the filter `arg` replaces the VM's, and
 * each vCPU is kicked with KVM_REQ_MSR_FILTER. 0; -EINVAL for an
 * unknown flag, a deny-all filter with no range, a range with no flag or
 * an unknown one, or a bitmap above KVM_MSR_FILTER_MAX_BITMAP_SIZE;
 * -ENOMEM; -EFAULT. */
int32_t kvm_msr_set_filter(struct kvm_vm *vm, uintptr_t arg);

/** Free the VM's MSR filter, when the VM is freed. */
void kvm_msr_free_filter(struct kvm_vm *vm);

/* pvclock.c: the kvmclock, its master clock, the wall clock and the
 * matching of the vCPUs' TSCs, as Linux's x86.c keeps them. Each runs
 * under the big lock. */

/** The host's TSC rate as measured, in kHz, which a guest's TSC runs at
 * unless KVM_SET_TSC_KHZ gave it another: 0 when none was measured. */
uint64_t kvm_tsc_khz(void);

/** Whether the processor scales a guest's TSC, which is what
 * KVM_CAP_TSC_CONTROL and KVM_CAP_VM_TSC_CONTROL say. */
int kvm_tsc_control(void);

/** The rate below which KVM_SET_TSC_KHZ takes a value where the processor
 * scales the TSC, Linux's max_guest_tsc_khz: what the backend's largest
 * ratio makes of the host's rate, at most 2^31 - 1. */
uint32_t kvm_tsc_max_khz(void);

/** KVM_SET_TSC_KHZ's argument as Linux checks it, for a vCPU and for a VM
 * alike: the rate to set, the host's for 0, or 0 for a rate at or past
 * `kvm_tsc_max_khz`, which is -EINVAL; 0 also where no rate was measured. */
uint32_t kvm_tsc_khz_arg(uintptr_t arg);

/**
 * Give `v`'s TSC the rate `khz`, as Linux's kvm_set_tsc_khz: within 250
 * parts in a million of the host's it runs at the host's; otherwise the
 * processor scales it, by a ratio the backend loads before the next entry;
 * and without scaling a rate above the host's is kept by moving the TSC up
 * before each entry (`kvm_clock_prepare`). 0, or -1 for 0 kHz, for a rate
 * below the host's without scaling, or for a ratio the backend cannot
 * hold; the rate is recorded all the same, as Linux records it.
 */
int kvm_tsc_set_khz(struct kvm_vcpu *v, uint32_t khz);

/** The count `tsc` of the host's TSC at `v`'s rate, without its offset. */
uint64_t kvm_tsc_scale(const struct kvm_vcpu *v, uint64_t tsc);

/** `v`'s guest TSC now. */
uint64_t kvm_tsc_read(const struct kvm_vcpu *v);

/** `v`'s TSC rate in hertz, for the clocks that count by it: the scaled
 * rate where the processor scales it, and otherwise the host's. */
uint64_t kvm_tsc_hz(const struct kvm_vcpu *v);

/** Start the clock of a new VM: its kvmclock reads 0 now. */
void kvm_clock_vm_init(struct kvm_vm *vm);

/** The kvmclock of `vm` now, in ns, as the guests read it: by the master
 * clock's formula from the host's TSC while it runs, else the host's
 * monotonic clock plus the VM's offset. */
uint64_t kvm_clock_ns(const struct kvm_vm *vm);

/** KVM_GET_CLOCK, as Linux's: with the master clock, the kvmclock at a TSC
 * count read with the real time, and KVM_CLOCK_TSC_STABLE,
 * KVM_CLOCK_REALTIME and KVM_CLOCK_HOST_TSC; else the kvmclock and no
 * flag. 0 or -EFAULT. */
int32_t kvm_clock_get(struct kvm_vm *vm, uintptr_t arg);

/** KVM_SET_CLOCK: the kvmclock reads the caller's `clock` now, advanced by
 * the real time since its `realtime` with KVM_CLOCK_REALTIME, and every
 * vCPU's kvmclock is written again before it enters. 0, -EINVAL for a
 * flag Linux does not take, or -EFAULT. */
int32_t kvm_clock_set(struct kvm_vm *vm, uintptr_t arg);

/** Set `v`'s TSC as Linux's kvm_synchronize_tsc: to `*value`, the caller's
 * KVM_SET_MSRS, or for NULL to 0 at the vCPU's creation, with the offset
 * of the VM's other vCPUs where the write matches theirs, which keeps the
 * master clock. After the vCPU joined the VM's `vcpus`. */
void kvm_tsc_sync(struct kvm_vcpu *v, const uint64_t *value);

/** Give `v`'s TSC the offset `offset` from the host's, the vCPU attribute
 * KVM_VCPU_TSC_OFFSET, as Linux's kvm_arch_tsc_set_attr: the offset joins
 * the VM's generation of matched TSCs when it is the last one set and the
 * rate is the last one's, which keeps the master clock. */
void kvm_tsc_offset_set(struct kvm_vcpu *v, int64_t offset);

/** `v`'s guest moved its own TSC: its kvmclock and the reference TSC page
 * are written again before the next entry, and a TSC kept above the host's
 * rate counts on from here. */
void kvm_tsc_moved(struct kvm_vcpu *v);

/** Read or write, by `write`, the MSR `index` of `v` when it is one of
 * KVM's own: the kvmclock's four, MSR_KVM_STEAL_TIME and
 * MSR_KVM_POLL_CONTROL. `host` is 1 for
 * KVM_GET_MSRS and KVM_SET_MSRS. 0; KVM_MSR_INVAL for a value refused;
 * KVM_MSR_UNKNOWN for an MSR whose feature the CPUID lacks under
 * KVM_CAP_ENFORCE_PV_FEATURE_CPUID, as Linux's KVM_MSR_RET_UNSUPPORTED,
 * but for the caller's read and its write of 0; 1 for an MSR that is not
 * KVM's. */
int kvm_pv_msr(struct kvm_vcpu *v, uint32_t index, int write, uint64_t *value, int host);

/** Write `v`'s kvmclock into guest memory before an entry, when it was
 * asked for (KVM_REQ_CLOCK_UPDATE) or, without the master clock, when the
 * vCPU moved to another processor or KVMCLOCK_SYNC_NS passed, as Linux's
 * kvm_guest_time_update. In `v`'s thread. */
void kvm_clock_prepare(struct kvm_vcpu *v);

/** KVM_KVMCLOCK_CTRL: the guest's next kvmclock says it was stopped. 0, or
 * -EINVAL while the kvmclock is off. */
int32_t kvm_clock_ctrl(struct kvm_vcpu *v);

/** Write `v`'s steal time into guest memory before an entry, as Linux's
 * record_steal_time, when it was asked for (KVM_REQ_STEAL_UPDATE), when
 * the thread was switched out and in since the last entry, as Linux asks
 * at each vcpu_load, and while the guest's record says it was preempted:
 * the thread's run-queue wait since the last write is added to `steal`,
 * and `preempted` cleared. In `v`'s thread. */
void kvm_steal_prepare(struct kvm_vcpu *v);

/** Mark `v`'s steal time KVM_VCPU_PREEMPTED, as Linux's
 * kvm_steal_time_set_preempted, when `schedule` switches the vCPU's thread
 * away while it can still run (its `sched_out` across KVM_RUN): only when
 * the guest last stopped for a host interrupt (`at_boundary`), with the
 * steal time on, and where `kvm_tdp_write_nofault` can write the record.
 * It counts the preemption in preemption_reported, or in preemption_other
 * when the guest did not stop for an interrupt. In `v`'s thread, under the
 * run queues' lock with interrupts off; does not sleep. */
void kvm_steal_preempted(struct kvm_vcpu *v);

/** KVM_HC_CLOCK_PAIRING of `type` into the guest's `gpa`, as Linux's
 * kvm_pv_clock_pairing: the real time and the guest's TSC at one moment.
 * The hypercall's answer: 0, -KVM_EOPNOTSUPP for another type or without
 * the host's TSC clock, or -KVM_EFAULT. */
uint64_t kvm_clock_pairing(struct kvm_vcpu *v, uint64_t gpa, uint64_t type);

/* slots.c: the memory slots, each a range of the caller's memory. */

/** The slot table of a new VM into `vm->slots`: 0 or -ENOMEM. */
int kvm_slots_init(struct kvm_vm *vm);

/** Free the slot table and each slot, taking their ranges off the change
 * notice. After `kvm_tdp_free`. */
void kvm_slots_free(struct kvm_vm *vm);

/**
 * KVM_SET_USER_MEMORY_REGION, with Linux's checks and answers: a slot
 * made, moved, its flags changed or deleted, while vCPUs run. What the
 * guest-physical tables hold of a slot that moves, changes its flags or
 * goes is dropped and every vCPU out of the guest before the call returns.
 * 0; -EINVAL for an unknown flag, an address or size not page-aligned, a
 * range past user space or the guest-physical width, a slot number past
 * KVM_MEM_SLOTS, the deletion of a slot that does not exist, or a change of
 * a slot's address, size or READONLY flag; -EEXIST for a new or moved slot
 * that overlaps another; -ENOMEM.
 */
int32_t kvm_slots_set(struct kvm_vm *vm, const struct kvm_userspace_memory_region *r);

/*
 * A slot as its users see it: its first frame, its frames, its flags, its
 * number, the caller's address of the frame asked for, and its dirty log,
 * a bit for each page from `base_gfn` in 64-bit words, or NULL when it does
 * not log.
 */
struct kvm_slot_view {
    uint64_t base_gfn;
    uint64_t npages;
    uint32_t flags; /* KVM_MEM_* */
    uint32_t id;
    uint64_t hva;    /* the caller's address of the frame asked for */
    uint64_t *dirty; /* the dirty log while KVM_MEM_LOG_DIRTY_PAGES is set */
};

/** The slot that holds the guest-physical frame `gfn` of `vm`, for the
 * guest-physical tables, VFIO's mappings and the transport's buffers: 1
 * with it in `*out`, or 0 for a frame no slot holds. Under the big lock;
 * the answer holds until the lock is let go. */
int kvm_slot_find(struct kvm_vm *vm, uint64_t gfn, struct kvm_slot_view *out);

/** The slot numbered `id` of `vm`, with `hva` its first page's address: 1,
 * or 0 when there is none. As `kvm_slot_find`. */
int kvm_slot_get(struct kvm_vm *vm, uint32_t id, struct kvm_slot_view *out);

/** The capabilities of slots.c: USER_MEMORY, DESTROY_MEMORY_REGION_WORKS,
 * JOIN_MEMORY_REGIONS_WORKS, READONLY_MEM, SYNC_MMU, NR_MEMSLOTS and
 * MANUAL_DIRTY_LOG_PROTECT2. */
int32_t kvm_slots_check_extension(long cap);

/* tdp.c: the guest-physical tables. */

/** Make the tables of a new VM, an empty top table: 0 or -ENOMEM. */
int kvm_tdp_init(struct kvm_vm *vm);

/** Free them, every table. */
void kvm_tdp_free(struct kvm_vm *vm);

/** The physical address of the top table of `vm`'s guest-physical tables,
 * four levels in the backend's format, for EPTP or nCR3. */
uint64_t kvm_tdp_root(struct kvm_vm *vm);

/** The first guest frame past what the tables reach, by the host's
 * physical width and their four levels. */
uint64_t kvm_tdp_frames(void);

/**
 * Map the guest-physical frame `gfn` of `vm` after a fault of the
 * backend's, `write` 1 for a write, in the current thread, which is a vCPU's
 * and so in the VM's address space: the slot's page is brought in as the
 * caller's own touch would bring it, and mapped writable when the slot,
 * the caller's page and the dirty log allow, uncached under the guest's
 * PAT for a device's page and write-back with the guest's PAT ignored for
 * RAM, and as a 2 MiB page where the slot and the caller's pages allow.
 * May sleep. 0; 1 for a frame no slot holds or a write to a read-only
 * slot, which is MMIO; -EFAULT when the caller's page cannot be had;
 * -ENOMEM.
 */
int kvm_tdp_fault(struct kvm_vm *vm, uint64_t gfn, int write);

/**
 * Write the `n` bytes at `buf` to `vm`'s guest-physical `gpa`, within one
 * page, without a fault or a sleep, as Linux's copy_to_user_nofault through
 * a vCPU's cache of the page: only where the current thread is in the VM's
 * address space and its page is present, writable and already stored into,
 * in a slot that is not read-only. The frame is marked in the dirty log.
 * 0, or 1 with nothing written. Under the big lock; for a context that may
 * not sleep, such as a thread's `sched_out`.
 */
int kvm_tdp_write_nofault(struct kvm_vm *vm, uint64_t gpa, const void *buf, uint32_t n);

/**
 * KVM_PRE_FAULT_MEMORY's work, as Linux's kvm_vcpu_pre_fault_memory: map
 * the frames of `range` as a read fault on each would, and move the range
 * past what was mapped, a leaf at a time, until it is empty, a frame cannot
 * be mapped or a signal is pending. May sleep. 0 when any frame was mapped;
 * otherwise -EINVAL for flags, or a range not of whole pages, empty or
 * wrapping, -ENOENT for a frame no slot holds, -EINTR for the signal, or
 * `kvm_tdp_fault`'s error.
 */
int32_t kvm_tdp_pre_fault(struct kvm_vm *vm, struct kvm_pre_fault_memory *range);

/** Drop what the tables map of the `npages` frames from `gfn`, and get
 * every vCPU out of the guest with its translations to drop
 * (`kvm_tdp_flush`) when anything was mapped, before a table it emptied is
 * freed. Does not sleep. */
void kvm_tdp_zap(struct kvm_vm *vm, uint64_t gfn, uint64_t npages);

/** Count a device that does not snoop the processor's caches assigned to
 * `vm` (`assigned` 1) or taken from it (0). While one is assigned, RAM is
 * mapped write-back with the guest's PAT in charge, not ignored, as Linux's
 * kvm_arch_register_noncoherent_dma makes it; the first and the last drop
 * every leaf. Under the big lock; does not sleep. */
void kvm_tdp_noncoherent(struct kvm_vm *vm, int assigned);

/** Take the write right from the 4 KiB leaves of the frames `gfn` + i for
 * each bit i of `mask`, for the dirty log, without a flush: whether any
 * leaf changed. Does not sleep. */
int kvm_tdp_protect(struct kvm_vm *vm, uint64_t gfn, uint64_t mask);

/** Ask every vCPU of `vm` to drop its guest-physical translations before
 * its next entry, and wait until none is in the guest, so that no
 * processor uses a translation the tables no longer hold. Under the big
 * lock, which no vCPU takes back while it waits; does not sleep. */
void kvm_tdp_flush(struct kvm_vm *vm);

/* dirty.c: dirty logging. */

/** KVM_GET_DIRTY_LOG and KVM_CLEAR_DIRTY_LOG, `request` naming which. */
int32_t kvm_dirty_ioctl(struct kvm_vm *vm, uint32_t request, uintptr_t arg);

/** KVM_ENABLE_CAP of KVM_CAP_MANUAL_DIRTY_LOG_PROTECT2 on `vm`: 0, or
 * -EINVAL for an option Linux does not take. */
int32_t kvm_dirty_enable_manual(struct kvm_vm *vm, const struct kvm_enable_cap *cap);

/** Mark the frame `gfn` of `vm` dirty in its slot's log, for a write of the
 * kernel's own to guest memory; nothing when its slot does not log. */
void kvm_dirty_mark(struct kvm_vm *vm, uint64_t gfn);

/* irq.c: interrupt routing, irqfd and MSIs. */

/** The VM ioctls of irq.c: KVM_CREATE_IRQCHIP, KVM_IRQ_LINE,
 * KVM_IRQ_LINE_STATUS, KVM_SET_GSI_ROUTING, KVM_IRQFD, KVM_SIGNAL_MSI. */
int32_t kvm_irq_ioctl(struct kvm_vm *vm, uint32_t request, uintptr_t arg);

/** KVM_ENABLE_CAP of KVM_CAP_SPLIT_IRQCHIP on `vm`. */
int32_t kvm_irq_enable_split(struct kvm_vm *vm, const struct kvm_enable_cap *cap);

/** The capabilities of irq.c: IRQCHIP, IRQ_ROUTING, IRQFD,
 * IRQFD_RESAMPLE, SIGNAL_MSI, SPLIT_IRQCHIP, IRQ_INJECT_STATUS,
 * TSC_DEADLINE_TIMER. */
int32_t kvm_irq_check_extension(long cap);

/** Whether an EOI of `vector` goes to the caller's I/O APIC: some route of
 * the GSIs KVM_CAP_SPLIT_IRQCHIP reserved for its pins is a level-triggered
 * MSI of that vector, as Linux's kvm_scan_ioapic_routes finds. Under the
 * big lock. */
int kvm_irq_eoi_exits(struct kvm_vm *vm, uint32_t vector);

/** Free irq.c's share of `vm`, its irqfds among them. */
void kvm_irq_free(struct kvm_vm *vm);

/* Independent sources ORed at each pin, as Linux's irq_source_id bitmap. */
#define KVM_IRQ_SRC_USER     1u
#define KVM_IRQ_SRC_RESAMPLE 2u
#define KVM_IRQ_SRC_PIT      4u

/** Set a routed GSI's source level under the big lock; -1 or deliveries.
 * `status` asks the IOAPIC to count an RTC pulse waiting for its EOI. */
int kvm_irq_set(struct kvm_vm *vm, uint32_t gsi, uint32_t source, uint32_t level, int status);

/** Notify a controller pin's acknowledgement or mask change under the
 * big lock, for PIT reinjection and irqfd resampling. */
void kvm_irq_ack(struct kvm_vm *vm, uint32_t chip, uint32_t pin);
void kvm_irq_mask(struct kvm_vm *vm, uint32_t chip, uint32_t pin, int masked);

/** Resample a pin: lower its resampling source and signal its resamplefds.
 * Returns 1 when there is a resampler, else 0; under the big lock. */
int kvm_irq_resample(struct kvm_vm *vm, uint32_t chip, uint32_t pin);

/** Process irqfds deferred from interrupt context under the big lock. */
void kvm_irq_process(struct kvm_vm *vm);

/** Whether an irqfd awaits processing; safe in a halted wait condition. */
int kvm_irq_pending(struct kvm_vm *vm);

/** Handle a PIC/PIT port or IOAPIC MMIO exit, completing reads through
 * the backend: 1 when handled, 0 for a caller-owned address. */
int kvm_irq_io(struct kvm_vcpu *v);

/** Make or free the PIC pair; create returns 0 or -ENOMEM. */
int kvm_pic_create(struct kvm_vm *vm);
void kvm_pic_free(struct kvm_vm *vm);

/** Set a PIC pin's source under the big lock: -1 masked, 0 coalesced,
 * 1 accepted. The pin is 0 to 15 and source is KVM_IRQ_SRC_*. */
int kvm_pic_set(struct kvm_vm *vm, uint32_t pin, uint32_t source, int level);

/** The PIC's next vector without acknowledgement, or -1 with no output. */
int kvm_pic_pending(struct kvm_vm *vm);

/** Acknowledge the PIC, including the cascade; returns its vector. */
int kvm_pic_ack(struct kvm_vm *vm);

/** Save or restore one PIC, chip 0 or 1; restore recomputes its output. */
void kvm_pic_state(struct kvm_vm *vm, uint32_t chip, struct kvm_pic_state *state, int set);

/** Read or write a PIC port: 1 handled, 0 not its port. */
int kvm_pic_io(struct kvm_vm *vm, uint32_t port, uint32_t size, uint32_t *value, int write);

/** Make or free the IOAPIC; create returns 0 or -ENOMEM. */
int kvm_ioapic_create(struct kvm_vm *vm);
void kvm_ioapic_free(struct kvm_vm *vm);

/** Set an IOAPIC pin's source: -1 masked, 0 coalesced, else deliveries.
 * The pin is below KVM_IOAPIC_NUM_PINS; under the big lock. */
int kvm_ioapic_set(struct kvm_vm *vm, uint32_t pin, uint32_t source, int level, int status);

/** Copy the counts of `pin`, KVM_PIN_STEPS of them by enum kvm_pin_step:
 * 0, or -1 with no IOAPIC in the kernel or no such pin. */
int kvm_ioapic_counts(struct kvm_vm *vm, uint32_t pin, uint32_t *counts);

/** An APIC's EOI broadcast, with the vector's TMR and directed-EOI bit. */
void kvm_ioapic_eoi(struct kvm_vcpu *v, uint32_t vector, int level, int directed);

/** Restore the RTC EOI count after a vCPU's local APIC is restored. */
void kvm_ioapic_restore(struct kvm_vcpu *v);

/** Save or restore the IOAPIC, reinjecting restored pending lines. */
void kvm_ioapic_state(struct kvm_vm *vm, struct kvm_ioapic_state *state, int set);

/** Read or write IOAPIC MMIO: 1 handled, 0 not its address. */
int kvm_ioapic_io(struct kvm_vm *vm, uint64_t addr, uint32_t size, uint64_t *value, int write);

/** The PIT creation, state and reinjection ioctls: 0 or -errno. */
int32_t kvm_pit_ioctl(struct kvm_vm *vm, uint32_t request, uintptr_t arg);

/** Read or write a PIT or dummy speaker port: 1 handled, 0 not its port. */
int kvm_pit_io(struct kvm_vm *vm, uint32_t port, uint32_t size, uint32_t *value, int write);

/** Inject due PIT ticks under the big lock, retaining unacknowledged ones. */
void kvm_pit_check(struct kvm_vm *vm);

/** The PIT's next host deadline in ns, or 0 while stopped. */
uint64_t kvm_pit_next(struct kvm_vm *vm);

/** Acknowledge a PIT tick, reset its queue on unmask, or free the PIT. */
void kvm_pit_ack(struct kvm_vm *vm);
void kvm_pit_unmask(struct kvm_vm *vm);
void kvm_pit_free(struct kvm_vm *vm);

/* iobus.c: the in-kernel bus, ioeventfd and the coalesced zones. */

/** KVM_IOEVENTFD, KVM_REGISTER_COALESCED_MMIO and
 * KVM_UNREGISTER_COALESCED_MMIO. */
int32_t kvm_iobus_ioctl(struct kvm_vm *vm, uint32_t request, uintptr_t arg);

/** Offer the port or MMIO write of `v->exit` to the in-kernel bus: 1 when
 * an ioeventfd took it or a coalesced zone queued it on the VM's ring, 0
 * when it goes to the caller. */
int kvm_iobus_write(struct kvm_vcpu *v);

/** The capabilities of iobus.c: IOEVENTFD, IOEVENTFD_ANY_LENGTH and
 * IOEVENTFD_NO_LENGTH, 1; COALESCED_PIO, 1; and COALESCED_MMIO, the ring's
 * page offset in a vCPU's mapping, KVM_COALESCED_MMIO_PAGE_OFFSET. */
int32_t kvm_iobus_check_extension(long cap);

/** Whether an ioeventfd of `vm` signals `ev`, for the transport that finds
 * its VM by the doorbell the VM rings. Under the big lock. */
int kvm_iobus_has_eventfd(struct kvm_vm *vm, const struct eventfd *ev);

/** Free iobus.c's share of `vm`. */
void kvm_iobus_free(struct kvm_vm *vm);

/* vlapic.c: the local APIC in the kernel. */

/*
 * One interrupt as the local APICs take it, from an MSI, an IPI or a local
 * vector table entry, as Linux's struct kvm_lapic_irq. `delivery_mode` is
 * the ICR's bits 10:8 in place (KVM_APIC_DM_*), `dest_mode` 1 for a
 * logical destination, `shorthand` the ICR's bits 19:18 as 0 to 3 (none,
 * self, all, all but self), `trig_mode` 1 for a level-triggered one, and
 * `redir_hint` an MSI's redirection hint, which makes a fixed interrupt
 * go to one of its destinations as a lowest-priority one does.
 */
#define KVM_APIC_DM_FIXED   0x000u
#define KVM_APIC_DM_LOWEST  0x100u
#define KVM_APIC_DM_SMI     0x200u
#define KVM_APIC_DM_REMRD   0x300u
#define KVM_APIC_DM_NMI     0x400u
#define KVM_APIC_DM_INIT    0x500u
#define KVM_APIC_DM_STARTUP 0x600u
#define KVM_APIC_DM_EXTINT  0x700u
struct kvm_lapic_irq {
    uint32_t vector;
    uint32_t delivery_mode;
    uint32_t dest_mode;
    uint32_t level;
    uint32_t trig_mode;
    uint32_t shorthand;
    uint32_t dest_id;
    int redir_hint;
};

/** KVM_HC_SEND_IPI, as Linux's kvm_pv_send_ipi: the interrupt of `icr`'s
 * vector, mode, level and trigger to each vCPU of `vm` whose physical
 * APIC id is `min` plus a bit of `low`, or `min` plus `bits` plus a bit of
 * `high`, `bits` being 64 or 32 by the caller's mode. How many took it, or
 * -KVM_EINVAL for a logical destination or a shorthand. */
int64_t kvm_lapic_pv_ipi(struct kvm_vm *vm, uint64_t low, uint64_t high, uint32_t min, uint64_t icr,
                         uint32_t bits);

/** Make the local APIC of `v`, a vCPU of a VM whose interrupt controller
 * is split, in the state of a reset: 0 or -ENOMEM. At KVM_CREATE_VCPU. */
int kvm_lapic_create(struct kvm_vcpu *v);

/** The ioctls of `v` that the local APIC answers: KVM_GET_LAPIC,
 * KVM_SET_LAPIC, KVM_INTERRUPT, KVM_GET_MP_STATE and KVM_SET_MP_STATE. */
int32_t kvm_lapic_ioctl(struct kvm_vcpu *v, uint32_t request, uintptr_t arg);

/** A HLT of `v`: 1 when it goes to the caller as KVM_EXIT_HLT, which it
 * does with no local APIC in the kernel; 0 when the vCPU is HALTED and
 * `kvm_lapic_prepare` waits for its wake-up before the next entry, or
 * when an exception is pending, which the next entry delivers. */
int kvm_lapic_halt(struct kvm_vcpu *v);

/** Free vlapic.c's share of `v`. */
void kvm_lapic_free(struct kvm_vcpu *v);

/**
 * Deliver `irq` to the local APICs of `vm` it names, from the APIC of
 * `src` for an IPI or from a device for NULL, as Linux's
 * kvm_irq_delivery_to_apic: the count of APICs that took it, or -1 when
 * none was named. Safe without the big lock and with interrupts off,
 * from an irqfd's hook, since it only sets bits atomically, kicks and
 * wakes.
 */
int kvm_lapic_deliver(struct kvm_vm *vm, struct kvm_vcpu *src, const struct kvm_lapic_irq *irq);

/** Deliver as above and, when non-NULL, fill `destinations`, a set of
 * KVM_VCPU_WORDS words, with the vCPUs that accepted it, for the IOAPIC's
 * RTC acknowledgement count. */
int kvm_lapic_deliver_tracked(struct kvm_vm *vm, struct kvm_vcpu *src,
                              const struct kvm_lapic_irq *irq, uint64_t *destinations);

/** Whether a local APIC matches a physical or logical destination. */
int kvm_lapic_match_dest(struct kvm_vcpu *v, uint32_t id, int logical);

/** Whether a vector remains in the local APIC's IRR or ISR. */
int kvm_lapic_pending_eoi(struct kvm_vcpu *v, uint32_t vector);

/** Request event processing and wake the APIC's wait; interrupt safe. */
void kvm_lapic_wake(struct kvm_vcpu *v);

/** Wake the first vCPU that accepts PIC interrupts, under the big lock. */
void kvm_lapic_pic_wake(struct kvm_vm *vm);

/** Deliver the PIT's virtual-wire NMI to unmasked LINT0s in NMI mode. */
void kvm_lapic_pit_nmi(struct kvm_vm *vm);

/** The vector `v` takes at its next interrupt boundary: the external
 * vector KVM_INTERRUPT gave while LINT0 takes ExtINT, else the highest one
 * requested above the processor priority; -1 for none. */
int kvm_lapic_pending(struct kvm_vcpu *v);

/** Take the vector `kvm_lapic_pending` gives, as the processor's
 * acknowledgement does: the external vector is used up, or the APIC's moves
 * from IRR to ISR, or leaves IRR alone for a SynIC's auto-EOI SINT. The
 * vector, or -1 for none. */
int kvm_lapic_ack(struct kvm_vcpu *v);

/** Offer the MMIO access of `v->exit` to `v`'s APIC page: 1 when it was
 * the APIC's, a read answered to the backend through `complete`; 0 when it
 * goes on to the bus and the caller. */
int kvm_lapic_mmio(struct kvm_vcpu *v);

/**
 * Read or write, by `write`, the MSR `index` of `v` when it is the
 * APIC's: IA32_APIC_BASE, IA32_TSC_DEADLINE or an x2APIC register
 * (0x800 to 0x8FF). `host` is 1 for KVM_GET_MSRS and KVM_SET_MSRS, which
 * may move the APIC base between any two modes. 0, -1 for a fault the
 * guest takes as #GP and the caller as a refusal, or 1 for an MSR that is
 * not the APIC's.
 */
int kvm_lapic_msr(struct kvm_vcpu *v, uint32_t index, int write, uint64_t *value, int host);

/** Deliver a performance-counter overflow through the local LVTPC entry,
 * if it is enabled. Under the big lock, never from the backend's entry. */
void kvm_lapic_pmi(struct kvm_vcpu *v);

/** Set the APIC base of `v` to `value`, with the checks of Linux's
 * kvm_set_apic_base: 0, or -1 for reserved bits, an invalid mode, or, from
 * the guest (`host` 0), x2APIC back to xAPIC or disabled to x2APIC. */
int kvm_lapic_set_base(struct kvm_vcpu *v, uint64_t value, int host);

/** CR8 of `v`, the TPR's high four bits. */
uint64_t kvm_lapic_cr8(struct kvm_vcpu *v);

/** Set the TPR of `v` from CR8 `cr8`. */
void kvm_lapic_set_cr8(struct kvm_vcpu *v, uint64_t cr8);

/**
 * Ready `v` for an entry, before each one, with the local APIC in the
 * kernel: deliver an expired timer, the APIC's or a Hyper-V synthetic
 * timer (`kvm_hv_stimers`), take an INIT or a SIPI, wait while the
 * vCPU is halted or waits for a SIPI until an event wakes it, and make
 * an interrupt waiting an event to inject (KVM_REQ_EVENT). 0 to enter; 1
 * to go to the caller with the run page's exit set, KVM_EXIT_IOAPIC_EOI;
 * -EINTR for a signal or a request to leave, with KVM_EXIT_INTR. May sleep.
 */
int32_t kvm_lapic_prepare(struct kvm_vcpu *v);

/** Whether the caller of `v` may inject an external interrupt now, the
 * run page's `ready_for_interrupt_injection` with the APIC in the kernel:
 * the guest takes interrupts, none waits, and LINT0 takes ExtINT. */
int kvm_lapic_ready(struct kvm_vcpu *v);

/** Make `vector` a fixed, edge-triggered interrupt of `v`'s APIC, as
 * Linux's kvm_apic_set_irq: 1 when the APIC took it, 0 while it is
 * disabled, -1 with no APIC. Safe from any context. */
int kvm_lapic_set_irq(struct kvm_vcpu *v, uint32_t vector);

/** Read or write, by `write`, the APIC register at `reg` (APIC_EOI,
 * APIC_ICR or APIC_TASKPRI) of `v` as an x2APIC MSR does, in either mode,
 * for Hyper-V's EOI, ICR and TPR MSRs: 0, or 1 for a value or register
 * refused, or no APIC. */
int kvm_lapic_hv_msr(struct kvm_vcpu *v, uint32_t reg, int write, uint64_t *value);

/* hyperv.c: Hyper-V's enlightenments, for a vCPU whose CPUID has Hyper-V's
 * interface at leaf 0x40000001. Each runs under the big lock unless it
 * says otherwise. */

/** The capabilities of hyperv.c: HYPERV, HYPERV_VAPIC, HYPERV_SPIN,
 * HYPERV_TIME, HYPERV_SYNIC, HYPERV_SYNIC2, HYPERV_VP_INDEX,
 * HYPERV_EVENTFD, HYPERV_TLBFLUSH, HYPERV_SEND_IPI, HYPERV_CPUID,
 * HYPERV_ENFORCE_CPUID and SYS_HYPERV_CPUID. */
int32_t kvm_hv_check_extension(long cap);

/** KVM_GET_SUPPORTED_HV_CPUID, of /dev/kvm for `v` NULL or of the vCPU
 * `v`, as Linux's kvm_get_hv_cpuid: the ten leaves and their count. 0,
 * -E2BIG for room for fewer, or -EFAULT. */
int32_t kvm_hv_get_cpuid(struct kvm_vcpu *v, uintptr_t arg);

/** KVM_HYPERV_EVENTFD: an eventfd for a connection id that the guest's
 * HvSignalEvent signals, or its removal. 0; -EINVAL for a flag or an id
 * past 24 bits; -EEXIST for an id assigned; -ENOENT for one not;
 * eventfd_get's error; -ENOMEM; -EFAULT. */
int32_t kvm_hv_eventfd(struct kvm_vm *vm, uintptr_t arg);

/** KVM_ENABLE_CAP of the vCPU `v`'s Hyper-V capabilities: HYPERV_SYNIC and
 * HYPERV_SYNIC2, which need the split controller, and
 * HYPERV_ENFORCE_CPUID. 0; -ENOTTY for ENLIGHTENED_VMCS and
 * DIRECT_TLBFLUSH, a nested guest's; -EINVAL for any other; -ENOMEM. */
int32_t kvm_hv_enable_cap(struct kvm_vcpu *v, const struct kvm_enable_cap *cap);

/** Take `v`'s new CPUID, the `n` entries `e`, before KVM_SET_CPUID2 makes
 * it the vCPU's: Hyper-V is on when it names the interface, and its
 * feature leaves are kept. 0, or -ENOMEM. */
int kvm_hv_set_cpuid(struct kvm_vcpu *v, const struct kvm_cpuid_entry2 *e, uint32_t n);

/** Read or write, by `write`, the MSR `index` of `v` when it is one of
 * Hyper-V's, from the caller of KVM_GET_MSRS or KVM_SET_MSRS with `host`
 * 1: 0; -1 for a fault the guest takes as #GP and the caller as a
 * refusal; 1 for an MSR that is not Hyper-V's. */
int kvm_hv_msr(struct kvm_vcpu *v, uint32_t index, int write, uint64_t *value, int host);

/** Whether `v`'s guest has Hyper-V and has written its guest OS id, so
 * that its hypercalls are Hyper-V's. */
int kvm_hv_hypercall_enabled(const struct kvm_vcpu *v);

/** The guest's hypercall, a VMCALL or VMMCALL of `len` bytes: `v->exit`
 * KVM_X_AGAIN with the result in RAX and RIP stepped, or with #UD queued,
 * or KVM_X_HYPERV for one the caller answers, whose result
 * `kvm_hv_complete` takes at the next KVM_RUN. 0. */
int kvm_hv_hypercall(struct kvm_vcpu *v, uint32_t len);

/** At the start of KVM_RUN, take the caller's answer to the last
 * KVM_EXIT_HYPERV: a hypercall's result, or the synthetic debugger's
 * status. */
void kvm_hv_complete(struct kvm_vcpu *v);

/** Ready `v` for an entry, before each one: write the reference TSC page
 * once it changed, and end KVM_RUN for a crash, a reset or a SynIC or
 * debugger MSR the guest wrote. 0 to enter; 1 to go to the caller with the
 * run page's exit set. */
int32_t kvm_hv_prepare(struct kvm_vcpu *v);

/** Fire `v`'s synthetic timers that are due, delivering their messages or
 * vectors, in `v`'s thread with the APIC in the kernel. */
void kvm_hv_stimers(struct kvm_vcpu *v);

/** The host's ns at which `v`'s next synthetic timer is due, 1 when one
 * waits to be looked at now, or 0 for none. */
uint64_t kvm_hv_stimer_next(const struct kvm_vcpu *v);

/** Whether `vector` of `v` is a SINT's with auto-EOI, which the APIC's
 * acknowledgement takes without putting it in service. */
int kvm_hv_synic_auto_eoi(struct kvm_vcpu *v, uint32_t vector);

/** An EOI of `vector` of `v`: a timer's message waiting for its SINT's
 * slot is tried again. */
void kvm_hv_synic_eoi(struct kvm_vcpu *v, uint32_t vector);

/** Raise SINT `sint` of the vCPU of `vm` whose VP index is `vpidx`, for a
 * KVM_IRQ_ROUTING_HV_SINT route: the APICs that took it, or -1 for no
 * such vCPU, no active SynIC or a masked SINT. Safe without the big lock
 * and with interrupts off. */
int kvm_hv_synic_set_irq(struct kvm_vm *vm, uint32_t vpidx, uint32_t sint);

/** Whether `v`'s guest is to see no invariant TSC, CPUID 0x80000007 EDX
 * bit 8: Hyper-V's invariant TSC control is offered and not yet set. */
int kvm_hv_invtsc_suppressed(const struct kvm_vcpu *v);

/** The guest's clock or TSC of `vm` moved: the reference TSC page is
 * written again before the next entry. */
void kvm_hv_clock_changed(struct kvm_vm *vm);

/** Free hyperv.c's share of `v`. */
void kvm_hv_vcpu_free(struct kvm_vcpu *v);

/** Free hyperv.c's share of `vm`, its connections' eventfds among them. */
void kvm_hv_vm_free(struct kvm_vm *vm);

/* emulate.c: the guest instructions the backends' exits hand over. Each
 * reads and writes the record's registers, which the backend keeps current
 * at the exit: `regs` always, and `sregs` for an exit that calls these. */

/** The mode of `v`'s code, 16, 32 or 64, for `kvm_decode`. */
int kvm_emul_mode(const struct kvm_vcpu *v);

/** The privilege level of `v`: SS's DPL, 0 in real mode, 3 in virtual-8086
 * mode. */
int kvm_emul_cpl(const struct kvm_vcpu *v);

/** Set RFLAGS.RF in the frame of `v`'s pending exception when it is a
 * fault, at its first injection, as Linux's kvm_check_and_inject_events:
 * the handler's IRET then resumes past an instruction breakpoint. */
void kvm_exception_rf(struct kvm_vcpu *v);

/** The answer in RAX to the KVM hypercall `v`'s registers name, as
 * Linux's kvm_emulate_hypercall gives it, before the caller cuts it to 32
 * bits outside long mode: -KVM_EPERM above CPL 0; 0 for
 * KVM_HC_VAPIC_POLL_IRQ and KVM_HC_SCHED_YIELD; KVM_HC_KICK_CPU's wake of
 * the vCPU with the APIC id in RCX; KVM_HC_SEND_IPI's count;
 * KVM_HC_CLOCK_PAIRING's (`kvm_clock_pairing`); -KVM_ENOSYS for KICK_CPU,
 * SEND_IPI and SCHED_YIELD when `kvm_pv_has` refuses their features, and
 * for any other. */
uint64_t kvm_emul_hypercall(struct kvm_vcpu *v);

/** Queue the exception `nr`, with `error` when `has_error`, for the next
 * entry, replacing one queued, and ask for its injection. */
void kvm_queue_exception(struct kvm_vcpu *v, uint8_t nr, int has_error, uint32_t error);

/**
 * Queue the #DB the processor raised in the guest and the backend took,
 * with `payload` its bits as DR6 names them, set for each event: fold them
 * into the guest's DR6, as Linux's kvm_deliver_exception_payload, the
 * breakpoint bits replaced, the bits active low cleared where the event
 * cleared them, and bit 12 never set; and clear DR7.GD, which the
 * processor clears when it delivers a #DB and an injection does not.
 */
void kvm_queue_db(struct kvm_vcpu *v, uint64_t payload);

/** Step `v` past an instruction of `len` bytes it ran in the kernel, as
 * Linux's kvm_skip_emulated_instruction: RIP, the interrupt shadow cleared,
 * and #DB queued when RFLAGS.TF was set, or `emul.step_trap` set under the
 * caller's single step, for the backend to end the exit as a debug one. */
void kvm_emul_skip(struct kvm_vcpu *v, uint32_t len);

/** Translate the linear address `la` of `v` through its page tables for an
 * access whose #PF error code bits are `access` (W 0x2, U 0x4, I/D 0x10):
 * 0 with `*gpa`; -1 with #PF queued; 1 for a table in no slot; -EFAULT. */
int kvm_guest_translate(struct kvm_vcpu *v, uint64_t la, uint32_t access, uint64_t *gpa);

/** Read or write `n` bytes of `v`'s guest-physical memory at `gpa`: 0; 1
 * for a byte in no slot or a write to a read-only one; -EFAULT. */
int kvm_guest_phys_rw(struct kvm_vcpu *v, uint64_t gpa, void *buf, uint32_t n, int write);

/** Read or write `n` bytes at `seg`:`ea` (KVM_SEG_*) of `v`, through its
 * page tables: 0; -1 with #PF queued; 1 for memory in no slot; -EFAULT. */
int kvm_guest_rw(struct kvm_vcpu *v, uint32_t seg, uint64_t ea, void *buf, uint32_t n, int write);

/** The bytes of the instruction at `v`'s CS:RIP, up to KVM_INSN_MAX, into
 * `buf`, for a backend that decodes an instruction its exit does not
 * describe: the count read, fewer where the next page cannot be read; -1
 * with #PF queued when the first byte cannot be; 0 when it is in no slot. */
int kvm_emul_fetch(struct kvm_vcpu *v, uint8_t *buf);

/** CPUID from `v`'s KVM_SET_CPUID2 table for EAX and ECX, into EAX to EDX,
 * with the bits Linux keeps in step with CR4, the APIC base, MISC_ENABLE
 * and Hyper-V's invariant TSC control: 0, and the caller steps RIP, or -1
 * with #GP queued, for a guest above CPL 0 that turned CPUID faulting on
 * in MSR_MISC_FEATURES_ENABLES. */
int kvm_emul_cpuid(struct kvm_vcpu *v);

/** XSETBV of EDX:EAX into XCR ECX, with Linux's checks: 0, or -1 with #GP
 * queued. The caller steps RIP on 0. */
int kvm_emul_xsetbv(struct kvm_vcpu *v);

/** The guest's RDMSR or WRMSR, by `write`, of `index`, for the MSRs every
 * backend answers the same way: the local APIC's, Hyper-V's, the TSC, the
 * identity MSRs, those of particular processors that Linux's KVM reads as
 * zero or whose writes it drops, and those msr.c keeps, of which
 * MSR_PLATFORM_INFO is the caller's to write and, after
 * KVM_CAP_MSR_PLATFORM_INFO with 0, to read. 0, or a KVM_MSR_* failure,
 * which is #GP unless `kvm_msr_user_exit` gives it to the caller. */
int kvm_emul_msr(struct kvm_vcpu *v, uint32_t index, int write, uint64_t *value);

/** Whether `v`'s CPUID names AMD or Hygon as its vendor, as Linux's
 * guest_cpuid_is_amd_compatible. */
int kvm_guest_amd(const struct kvm_vcpu *v);

/**
 * Carry out the instruction of `v` whose access to memory in no slot
 * exited, as Linux's emulator does: fetch it at CS:RIP, decode it, and run
 * it on the record's registers, with its operands found through the
 * guest's own tables. `v->exit` becomes its first MMIO read, whose answer
 * `kvm_emul_complete` takes before the instruction runs again; or, the
 * instruction done, its first MMIO write, with `emul.resume` set while more
 * follow; or KVM_X_AGAIN, with an exception queued when it faulted; or,
 * for an instruction the emulator does not know or cannot finish, #UD
 * queued and, at CPL 0, KVM_X_INTERNAL, whose report carries `info`, the
 * exit's reason, qualifications, interrupt information and error code;
 * under KVM_CAP_EXIT_ON_EMULATION_FAILURE it is KVM_X_INTERNAL at every
 * level, with no #UD. 0, or -EFAULT.
 */
int kvm_emul_mmio(struct kvm_vcpu *v, const uint64_t info[5]);

/**
 * Carry out the instruction at CS:RIP of `v` that raised #UD on the
 * host's processor, when it is one Linux's emulator runs after a #UD:
 * SYSCALL, SYSENTER and SYSEXIT, which a processor of the other vendor
 * refuses in some modes; MOVBE and RDPID, for a guest whose CPUID has
 * them on a processor without; and VMCALL or VMMCALL, whichever the
 * host's processor lacks, which is rewritten in the guest's memory as the
 * one it has and left to run again. Any other is given back to the guest
 * as the #UD it raised. `v->exit` and `info` as `kvm_emul_mmio`. 0, or
 * -EFAULT.
 */
int kvm_emul_ud(struct kvm_vcpu *v, const uint64_t info[5]);

/**
 * `count` pages whose frames are consecutive, set to all ones, for a
 * permission map, which the processor reads by its physical address and
 * across its pages; `*pa` receives the first frame's address. The heap's
 * pages are contiguous only in virtual memory, so a run whose frames are
 * not consecutive is held until one is found, so that the allocator gives
 * other frames, and then freed. NULL after 16 such runs or without memory.
 */
uint8_t *kvm_map_pages(uint32_t count, uintptr_t *pa);

/** Begin the port I/O instruction `p` of `v`: `v->exit` its KVM_X_IO, an
 * OUT's bytes from RAX or the string in guest memory; KVM_X_AGAIN with an
 * exception queued; or the MMIO read of a string OUT's element that is in
 * device memory, after whose answer the instruction runs again. RIP is
 * stepped for a string OUT, and for an OUT to port 0x7E under
 * KVM_X86_QUIRK_OUT_7E_INC_RIP; any other OUT steps it in
 * `kvm_emul_complete`, as Linux does, so that the caller sees the exit
 * with RIP at the instruction. 0, or -EFAULT. */
int kvm_emul_pio(struct kvm_vcpu *v, const struct kvm_pio *p);

/** Give the instruction under way the answer of its read, in `v->exit`, from
 * the backend's `complete`: a port read finishes, with `v->emul.resume` set
 * when its string has elements to write to device memory; an MMIO read's
 * answer is kept and `v->emul.resume` set for the instruction to run
 * again; and an OUT that was written, in the kernel or by the caller,
 * steps RIP unless the caller moved it. A port instruction that ends
 * here under the caller's single step sets `v->emul.resume` for
 * `kvm_emul_resume` to report the step, and so does a string IN whose
 * bytes cannot be written to guest memory, for it to report the internal
 * error or -EFAULT, as `kvm_emul_pio` reports a string OUT's. 0. */
int kvm_emul_complete(struct kvm_vcpu *v);

/** An instruction ended outside the backend's `exit`, which reads
 * `v->emul.step_trap`: a port instruction in `kvm_emul_complete`, or an
 * RDMSR, WRMSR or Hyper-V hypercall the caller answered. Under the
 * caller's single step, set `v->emul.resume` for `kvm_emul_resume` to
 * report the step at this KVM_RUN, as Linux's
 * kvm_skip_emulated_instruction does from a completion; clear
 * `step_trap` either way. */
void kvm_emul_stepped(struct kvm_vcpu *v);

/** The exit of the step `v->emul.resume` asked for, into `v->exit`, from
 * the backend's `exit` with no guest entered: the instruction run again,
 * its next MMIO write, `v->emul.step_trap` set for a port instruction
 * stepped in `kvm_emul_complete`, or a string IN's failure. 0, or
 * -EFAULT. */
int kvm_emul_resume(struct kvm_vcpu *v);

/** Read the four PAE PDPTEs at `cr3` of `v` into `pdpte`, as Linux's
 * load_pdptrs: 0, or -1 for a table in no slot or a present PDPTE with a
 * reserved bit, which a write of CR3 or CR4 takes as #GP. */
int kvm_guest_pdptes(struct kvm_vcpu *v, uint64_t cr3, uint64_t pdpte[4]);

/* The source of a task switch, as VMX's exit qualification gives it in
 * bits 31:30: CALL, IRET, JMP, or a task gate in the IDT. */
#define KVM_TS_CALL 0
#define KVM_TS_IRET 1
#define KVM_TS_JMP  2
#define KVM_TS_GATE 3

/**
 * Carry out `v`'s hardware task switch to the TSS `sel`, from `reason`
 * (KVM_TS_*), as Linux's kvm_task_switch, with the 16-bit and 32-bit TSS,
 * the busy bits, NT and the back link, CR3, the LDT and the segments.
 * `idt_index` is the vector of an INT n whose task gate the switch goes
 * through, for the gate's DPL check, else -1; `next_rip` the EIP the old
 * task saves; `error` the error code an exception through a task gate
 * pushes, or -1 for none. The faults are the processor's: 0 with the
 * switch made, and a fault in loading the new task's state queued in its
 * context; -1 with a fault queued in the old task, whose registers have
 * not changed; 1 when a table or TSS is in no slot, which the backend
 * reports as Linux's internal error; -EFAULT.
 */
int kvm_task_switch(struct kvm_vcpu *v, uint16_t sel, int reason, int idt_index, uint64_t next_rip,
                    int64_t error);

/* gpamap.c: a guest's pages mapped into a program of the host. */

struct kvm_gpamap;

/* A run of guest-physical pages, as a guest's driver reports a buffer of
 * its own: the first page's address, page-aligned, and the pages. */
struct kvm_gpa_run {
    uint64_t gpa;
    uint64_t pages;
};

/**
 * Hold the frames behind `n` runs of `vm`'s guest-physical pages, in the
 * order the runs give, at most 2^31 - 1 pages in all: each page found
 * through the slot that holds it in the caller's memory, the VM's maker's,
 * as a RAM page that memory maps writable outside an I/O, guard or
 * memfd_secret region (`paging_user_ram_frame`), and held by a reference.
 * The map is taken back, its frames let go of after `revoke(owner)` has
 * stopped every use of them, when the caller's memory behind any of them
 * changes or ends, when a slot over them moves or goes, and when the VM
 * ends; `kvm_gpamap_frame` then answers 0. Under the big lock; does not
 * sleep, so that no change can come between the slots' answer and the
 * frames. 0 with the map in `*out`; -EINVAL for a run that is empty, not
 * page-aligned or past the guest-physical width, or for too many pages;
 * -EFAULT for a page in no slot, in a read-only one, not present, not
 * ordinary RAM or in a memfd_secret region, which cannot be shared, and
 * for every page once the maker's address space has ended, which leaves no
 * range on its notice; -ENOMEM.
 */
int kvm_gpamap_new(struct kvm_vm *vm, const struct kvm_gpa_run *runs, uint32_t n,
                   void (*revoke)(void *owner), void *owner, struct kvm_gpamap **out);

/** The frame of page `i` of `m`, or 0 past its end or once it is taken
 * back. */
uintptr_t kvm_gpamap_frame(const struct kvm_gpamap *m, uint64_t i);

/** Let go of `m`'s frames, if it still holds them, without its `revoke`,
 * and free it: its owner's call, once nothing uses the frames. NULL is
 * ignored. Under the big lock. */
void kvm_gpamap_free(struct kvm_gpamap *m);

/** Take back every map of `vm` with a page in the `npages` frames from
 * `gfn`, for a slot over them that moves or goes. Under the big lock. */
void kvm_gpamap_zap(struct kvm_vm *vm, uint64_t gfn, uint64_t npages);

/** Take back every map of `vm`, at the VM's end. */
void kvm_gpamap_vm_end(struct kvm_vm *vm);

#endif /* _KVM_H_ */
