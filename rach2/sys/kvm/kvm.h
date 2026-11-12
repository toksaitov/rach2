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
#include "../lib/spinlock.h"
#include "../proc/mutex.h"
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

/* The memory slots a VM has, KVM_CAP_NR_MEMSLOTS: Linux's x86
 * KVM_USER_MEM_SLOTS, SHRT_MAX less the 3 slots its KVM keeps for itself,
 * so that a caller numbers its slots below it as on Linux. */
#define KVM_MEM_SLOTS 32764

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

/* Linux's ring bound and reserve for writes made while handling an exit.
 * Neither backend buffers dirty pages in hardware. */
#define KVM_DIRTY_RING_MAX_BYTES (65536u * 16u)
#define KVM_DIRTY_RING_RESERVED  64u

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
 * kernel picks at boot by `probe`. Each runs with interrupts on unless it
 * says otherwise: `probe`, `vm_init`, `vm_free`, `vcpu_init`, `vcpu_free`,
 * `check_extension` and `tsc_scaling` under the big lock, and the rest in
 * the thread that holds the vCPU (`mutex`), KVM_RUN's without the big
 * lock.
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
 * turns that exit into `v->exit`, with interrupts on and without the big
 * lock, and may sleep: 0, or -errno to end
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
 * before each entry, since CR8 is the APIC's TPR, or has the processor
 * keep TPR in the APIC's register page itself (`kvm_lapic_page`) and
 * calls `kvm_lapic_update_ppr` when the guest lowers TPR below the
 * priority of the highest vector waiting (`kvm_lapic_highest_irr`). A
 * guest's VMCALL or VMMCALL goes to `kvm_hv_hypercall` while
 * `kvm_hv_hypercall_enabled`; the flush KVM_REQ_TLB_FLUSH asks for must
 * drop the guest's own translations as well, since the Hyper-V TLB flush
 * hypercalls ask for it to flush the guest's TLB. `check_extension`
 * answers the capabilities whose truth depends on the processor, such as
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
 * `put`, NULL for a backend that gives the processor back after each run,
 * gives the host back what `enter` left the guest's from one run to the
 * next, as Linux's kvm_arch_vcpu_put: the run loop calls it with interrupts
 * off at each switch of the vCPU's thread away from its processor, with
 * `fpu` 0, and at KVM_RUN's end, with `fpu` 1, when the floating-point
 * registers go back to the host as well. While they hold the guest's,
 * `save` of KVM_PART_FPU writes them into the image, and a change of the
 * image marks KVM_PART_FPU, so that the next entry loads it.
 * `fast`, NULL for a backend without it, is the exit path's fast path, as
 * Linux's vmx_exit_handlers_fastpath and svm_exit_handlers_fastpath
 * (arch/x86/kvm/vmx/vmx.c:7258-7279, svm/svm.c:4283-4310): called after
 * each `enter` that returned 0, still with interrupts off, it handles the
 * exit `enter` read when it is one of the few it takes, a WRMSR of the TSC
 * deadline and on VMX the preemption timer's expiry, with the local APIC
 * in the kernel, in the way `exit` would, with `v->exit` KVM_X_AGAIN and
 * the event to inject chosen when it delivered one. 1 when it handled the
 * exit, which `exit` must then not see, and 0, with nothing changed, for
 * every other exit. It does not sleep and takes only spinlocks taken with
 * interrupts off: the MSR filter's and those of the APIC's delivery.
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
    void (*put)(struct kvm_vcpu *v, int fpu);
    int (*fast)(struct kvm_vcpu *v);
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
    /* Over the slots' trees, their dirty logs, the guest-physical tables
     * and the VM's page counts (slots.c, tdp.c, dirty.c): Linux's mmu_lock.
     * `mmu_seq` counts the zaps, each under the lock, as Linux's
     * mmu_invalidate_seq: a fault reads it before it looks at the caller's
     * page and maps nothing when it moved. */
    struct spinlock mmu_lock;
    volatile uint32_t mmu_seq;
    /* Over the master clock, the TSCs' matching, `clock_offset` and
     * `wall_clock` (pvclock.c), as Linux's tsc_write_lock with its
     * pvclock_sc: a reader copies the fields out under it. */
    struct spinlock clock_lock;
    /* The kvmclock moved since hyperv.c last looked: its reference TSC
     * page is written again (`kvm_hv_clock_changed`). Set by pvclock.c,
     * taken by an exchange under the big lock (`kvm_hv_prepare`,
     * `kvm_hv_clock_update`). */
    volatile uint32_t hv_clock_moved;
    /* The VM's read side, Linux's kvm->srcu (`kvm_srcu_read_lock`), over
     * the slots, `msr_filter` and `pmu_filter`, which a guest's
     * exits read with no lock: a reader joins `srcu_readers[srcu_gen]`;
     * `srcu_waiting` counts the replacements waiting on `srcu_wq` for a
     * count to reach 0. Zero is idle. `srcu_gp_lock`, Linux's
     * srcu_gp_mutex, is held by one replacement from its first change of
     * `srcu_gen` to the end of its second wait (`kvm_srcu_synchronize`). */
    volatile uint32_t srcu_gen;
    volatile uint32_t srcu_readers[2];
    volatile uint32_t srcu_waiting;
    wait_queue_t srcu_wq;
    struct mutex srcu_gp_lock;
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
    /* The host clock's generation (`vclock_generation`) at which the master
     * clock was last decided; written under `clock_lock`, compared without
     * it before each entry (`kvm_clock_prepare`) and at the entry's gates
     * (`kvm_clock_host_moved`). */
    volatile uint32_t host_clock_gen;
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
    void *msr_filter;            /* KVM_X86_SET_MSR_FILTER's, msr.c's, read under `srcu_gen` */
    int x2apic_format;           /* KVM_X2APIC_API_USE_32BIT_IDS */
    int x2apic_no_broadcast;     /* KVM_X2APIC_API_DISABLE_BROADCAST_QUIRK */
    uint64_t apic_bus_ns;        /* KVM_CAP_X86_APIC_BUS_CYCLES_NS: an APIC timer's tick in ns */
    uint32_t default_tsc_khz;    /* KVM_SET_TSC_KHZ of the VM: the rate a new vCPU's TSC has */
    int irqchip;                 /* KVM_IRQCHIP_*: which interrupt controller is in the kernel */
    uint32_t boot_cpu_id;        /* KVM_SET_BOOT_CPU_ID */
    uint32_t dirty_manual;       /* KVM_CAP_MANUAL_DIRTY_LOG_PROTECT2's options, dirty.c's */
    uint32_t dirty_ring_size;    /* bytes per vCPU; fixed before the first vCPU */
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
    void *pmu_filter;                     /* pmu.c's event filter, read under `srcu_gen` */
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

/* The requests left for a vCPU, in `requests`, by its own thread or
 * another, as Linux's vcpu->requests: whoever changes state that the vCPU
 * must act on before it enters sets the request's bit atomically and kicks
 * the vCPU (`kvm_vcpu_kick`), and the run loop does each source's work only
 * behind its bit, so that an entry with no request tests the one word, as
 * Linux's vcpu_enter_guest tests kvm_request_pending
 * (arch/x86/kvm/x86.c:10288). The timers, which have no host timer of
 * their own to raise a request, keep their earliest deadline in the vCPU
 * (`timer_ns`), which the loop compares with the clock while one is armed
 * and turns into KVM_REQ_TIMER when it passes. */
#define KVM_REQ_EXIT               0x1u  /* go back to the caller */
#define KVM_REQ_EVENT              0x2u  /* an event to inject */
#define KVM_REQ_TRIPLE_FAULT       0x4u  /* end in KVM_EXIT_SHUTDOWN */
#define KVM_REQ_EOI_EXIT           0x8u  /* end in KVM_EXIT_IOAPIC_EOI (vlapic.c) */
#define KVM_REQ_TLB_FLUSH          0x10u /* the backend drops the VM's guest-physical translations */
#define KVM_REQ_HV_EXIT            0x20u   /* end in KVM_EXIT_HYPERV (hyperv.c) */
#define KVM_REQ_HV_CRASH           0x40u   /* end in KVM_SYSTEM_EVENT_CRASH (hyperv.c) */
#define KVM_REQ_HV_RESET           0x80u   /* end in KVM_SYSTEM_EVENT_RESET (hyperv.c) */
#define KVM_REQ_CLOCK_UPDATE       0x100u  /* write the kvmclock and the reference TSC page again */
#define KVM_REQ_STEAL_UPDATE       0x200u  /* write the steal time again (pvclock.c) */
#define KVM_REQ_MSR_FILTER         0x400u  /* the backend's `msr_filter_changed` */
#define KVM_REQ_PMU                0x800u  /* find the counters that run again (pmu.c) */
#define KVM_REQ_SCAN_IOAPIC        0x1000u /* find the EOI exits again (vlapic.c) */
/* A timer may be due or was changed: the local APIC's, the PIT's, Hyper-V's
 * synthetic timers and the kvmclock's resync are looked at, and `timer_ns`
 * found again (vcpu.c), as Linux's hrtimers raise their own requests. */
#define KVM_REQ_TIMER              0x2000u
#define KVM_REQ_NMI                0x4000u  /* NMIs delivered, to queue (vlapic.c) */
#define KVM_REQ_INIT_SIPI          0x8000u  /* an INIT or a SIPI delivered, to take (vlapic.c) */
#define KVM_REQ_IRQFD              0x10000u /* an irqfd left for the run loop rose (irq.c) */
#define KVM_REQ_PMI                0x20000u /* a counter overflowed: deliver its PMI (pmu.c) */
#define KVM_REQ_DIRTY_RING_FULL    0x40000u /* the dirty ring has its reserve left (dirty.c) */
#define KVM_REQ_MASTERCLOCK_UPDATE 0x80000u /* decide the master clock again (pvclock.c) */

/* The requests that cancel an entry they find set once the vCPU is
 * IN_GUEST, as Linux's kvm_vcpu_exit_request cancels one for any pending
 * request (arch/x86/kvm/x86.c:1715-1720): every request but the flush, which
 * the backend's `enter` answers itself. A request a kick sets between the
 * loop's answer and the entry, before the kick could see IN_GUEST and
 * interrupt the processor, is answered before the guest runs: an event
 * the kick brings, or the kvmclock's, so that no vCPU runs on the master
 * clock's old formula while another runs on its new. Each is cleared by
 * the loop's next pass, so the loop does not come back here forever. */
#define KVM_REQ_NOENTER (~KVM_REQ_TLB_FLUSH)

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
    int fx_held;             /* `fx` was taken in this pass, at its first use */
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
    uint32_t id;                      /* the caller's id, the APIC id */
    uint32_t index;                   /* the place in the VM's `vcpus` */
    struct kvm_run *run;              /* the run page, the first of KVM_RUN_PAGES */
    uint8_t *pio;                     /* the port I/O page, the second */
    struct kvm_dirty_gfn *dirty_ring; /* shared pages at KVM_DIRTY_LOG_PAGE_OFFSET */
    /* Free-running entry counts under mmu_lock, read by the run loop.
     * The caller can change entries, but never these indices. */
    volatile uint32_t dirty_ring_put, dirty_ring_reset;
    volatile uint32_t mode;     /* KVM_MODE_* */
    volatile uint32_t requests; /* KVM_REQ_* */
    volatile int32_t cpu;       /* the processor it last entered on, -1 before */
    /* The host's ns from which a timer of the vCPU may be due, 0 for none:
     * the earliest deadline of the local APIC's timer, Hyper-V's synthetic
     * timers, the PIT's and the kvmclock's resync, or 1 for work waiting
     * now. The vCPU's thread alone writes it, finding it again at
     * KVM_REQ_TIMER and lowering it when it arms a timer itself
     * (`kvm_timer_arm`); another thread changing a timer asks with
     * KVM_REQ_TIMER. It may be early, which costs a look, never late. */
    uint64_t timer_ns;
    /* Held for each ioctl of the vCPU, KVM_RUN among them, as Linux's
     * vcpu->mutex: the state below is its holder's. */
    struct mutex mutex;
    int hlt_in_guest; /* the VM's KVM_X86_DISABLE_EXITS_HLT at creation: HLT does not exit */
    int has_run;      /* KVM_RUN has entered the guest */
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
    int cpuid_set;         /* KVM_SET_CPUID2 was taken, with entries or with none */
    uint64_t cr4_reserved; /* `kvm_cr4_reserved`'s, kept from creation and KVM_SET_CPUID2 */
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
    /* The record at the guest-physical `st_gpa` was last written whole,
     * and found in no region of memfd_secret, when the VM's `mmu_seq` was
     * `st_seq` (`st_ok`), so that `kvm_steal_preempted`, which cannot look
     * at the caller's regions, writes only while no zap has come since and
     * the MSR still names that address, as Linux's
     * kvm_steal_time_set_preempted checks its cache's slots generation and
     * gpa (arch/x86/kvm/x86.c:4647-4650). */
    int st_ok;
    uint32_t st_seq;
    uint64_t st_gpa;
    int pv_stopped;    /* KVM_KVMCLOCK_CTRL: the next write sets PVCLOCK_GUEST_STOPPED */
    int32_t clock_cpu; /* the processor the kvmclock was last written on, -1 for none */
    uint64_t clock_ns; /* the host's ns at that write */
    uint64_t mcg_cap, mcg_status, mcg_ctl;
    uint64_t platform_info;        /* MSR_PLATFORM_INFO, which only the caller writes */
    uint64_t misc_features;        /* MSR_MISC_FEATURES_ENABLES: bit 0, CPUID faults above CPL 0 */
    uint64_t ucode_rev;            /* MSR_IA32_UCODE_REV, which only the caller writes */
    uint64_t hwcr;                 /* MSR_K7_HWCR: McStatusWrEn and TscFreqSel alone */
    uint64_t power_ctl;            /* MSR_IA32_POWER_CTL, which nothing reads */
    uint64_t spec_ctrl;            /* IA32_SPEC_CTRL as the guest wrote it */
    uint64_t virt_spec_ctrl;       /* AMD's VIRT_SPEC_CTRL: SSBD alone */
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
     * -1 for none, the TSC count since which its handling is timed, and
     * the cycles it has waited for the big lock (`kvm_bkl_enter`). */
    int32_t xs_row;
    uint64_t xs_mark;
    uint64_t xs_wait;
    /* TSC counts, 0 when not taken: the run loop's at its first pass over
     * its top once the last exit is handled (`xs_prep`); the backend's just
     * before the entry instruction of the run that ends in the next exit
     * (`xs_in`) and just after that exit (`xs_out`). */
    uint64_t xs_prep, xs_in, xs_out;
    /* `xs_wait` when `xs_prep` was taken: the waits after it are the
     * preparation's, which it does not count, as the handling does not. */
    uint64_t xs_prep_wait;

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

/** Make the exit counts of each processor started since; from each VM's
 * creation, with the big lock held. A processor left without them for want
 * of memory is logged, and its exits go uncounted. */
void kvm_exitstat_cpus(void);

/**
 * Count the exit of the run that just ended on `v`, by `v->hw_exit`, among
 * the entries since boot (`kvm_exit_count`) and in this processor's row,
 * and close the account of the exit before it, if one is open, as handled
 * in the kernel: `entry`, read just before the backend's entry, ends that
 * exit's handling and its preparation, and `left`, read as the backend's
 * entry returned, starts the new one's; `entry` and the run's `xs_in` and
 * `xs_out`, where taken, time the entry, the run and the exit before
 * `left`. Only after a run that entered the guest; in `v`'s thread, with
 * interrupts off.
 */
void kvm_exitstat_exit(struct kvm_vcpu *v, uint64_t entry, uint64_t left);

/** Mark the TSC count at which the run loop, the exit of `v` handled, begins
 * to prepare the next entry, if an account is open and not yet marked.
 * In `v`'s thread. */
void kvm_exitstat_prepare(struct kvm_vcpu *v);

/** Count the exit of `v` whose account is open as one the backend's
 * `fast` handled, after which the guest entered again with the run loop
 * not run. In `v`'s thread, with interrupts off. */
void kvm_exitstat_fast(struct kvm_vcpu *v);

/** End the account of `v`'s exit, if one is open, as one that ended
 * KVM_RUN. The exit's waits for the big lock go to its row's wait, the
 * rest of its time to its handling. When KVM_RUN returns, in `v`'s
 * thread. */
void kvm_exitstat_done(struct kvm_vcpu *v);

/** Take the big lock for a part of KVM_RUN that still needs it, counting
 * the cycles the take waits in the account of `v`'s exit; `v` may be NULL,
 * for a caller outside KVM_RUN. Never under a spinlock. */
void kvm_bkl_enter(struct kvm_vcpu *v);

/** Whether the caller runs in the address space that made `vm`; Linux
 * answers EIO to an ioctl of a VM or its vCPUs from any other. */
int kvm_vm_caller_ok(struct kvm_vm *vm);

/** The address space whose memory `vm`'s slots map, its maker's; NULL for
 * a VM made by the kernel outside any process. */
struct mm *kvm_vm_mm(struct kvm_vm *vm);

/** Take a reference to `vm`. */
void kvm_vm_get(struct kvm_vm *vm);

/** Drop a reference to `vm`, freeing it, its vCPUs, its slots and the
 * backend's share with the last one. Under the big lock; no vCPU is in
 * KVM_RUN once the last reference goes, since its descriptor holds one. */
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

/** The host's kvmclock in nanoseconds, the raw clock, which the NTP state
 * does not slew, as Linux's get_kvmclock_base_ns counts from boot at the
 * raw clock's rate (arch/x86/kvm/x86.c). */
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
 * when it is not, since the loop reads `requests` once the vCPU is
 * IN_GUEST, before each entry. The request is set atomically, so an
 * interrupt handler or a holder of any spinlock may kick, without the big
 * lock. */
void kvm_vcpu_kick(struct kvm_vcpu *v, uint32_t request);

/** Clear the request `request` of `v` atomically: whether it was set. */
int kvm_vcpu_test_clear(struct kvm_vcpu *v, uint32_t request);

/** Lower `v`'s `timer_ns` to `ns`, the host's ns of a timer the caller just
 * armed, or 1 for timer work waiting now; 0 changes nothing. By the holder
 * of `v`'s mutex, in any context; another thread kicks KVM_REQ_TIMER. */
void kvm_timer_arm(struct kvm_vcpu *v, uint64_t ns);

/**
 * Look at `v`'s timers once one may be due: when `timer_ns` has passed,
 * which reads the clock only while it is set, or at KVM_REQ_TIMER. With
 * the local APIC in the kernel, deliver the APIC timer's expiry
 * (`kvm_lapic_timer_check`), the PIT's (`kvm_pit_check`) and Hyper-V's
 * synthetic timers' (`kvm_hv_stimers`); ask for the kvmclock once its
 * resync is due (`kvm_clock_next`); and find `timer_ns` again. The run
 * loop's, before each entry and after each halted wait, in `v`'s thread
 * without the big lock.
 */
void kvm_vcpu_timers(struct kvm_vcpu *v);

/** Make `v`'s floating-point image hold the guest's state before it is
 * read during KVM_RUN, while the registers may hold it (the backend's
 * `put`); a writer of the image marks KVM_PART_FPU after. Any context. */
void kvm_vcpu_fpu_sync(struct kvm_vcpu *v);

/** Give `v` the state an INIT leaves, as Linux's kvm_vcpu_reset with
 * `init_event`: the registers, segments, control registers, events and
 * debug registers of a reset, keeping CR0's CD and NW, the APIC base, the
 * FPU's state, XCR0,
 * the MSRs but the kvmclock's and the steal time's, which are turned off,
 * and SPEC_CTRL and VIRT_SPEC_CTRL, which are cleared, and the TSC.
 * vlapic.c resets the APIC itself. */
void kvm_vcpu_init_reset(struct kvm_vcpu *v);

/** The guest's physical address bits, from leaf 0x80000008 of `v`'s CPUID,
 * 36 without it, as Linux's cpuid_query_maxphyaddr. */
uint32_t kvm_guest_maxphyaddr(const struct kvm_vcpu *v);

/** The CR4 bits `v` may not set, as Linux's __kvm_is_valid_cr4: those the
 * CPUID KVM offers reserves, and once KVM_SET_CPUID2 is taken those `v`'s
 * CPUID reserves too. VMXE and SMXE always, since no nested guest is
 * offered. Kept in `v` from its creation and each KVM_SET_CPUID2, as Linux
 * keeps cr4_reserved_bits and cr4_guest_rsvd_bits (arch/x86/kvm/x86.c:9382,
 * cpuid.c:417-418), since each of the host's CPUIDs that make it is an exit
 * to the hypervisor beneath a nested Rach2. */
uint64_t kvm_cr4_reserved(const struct kvm_vcpu *v);

/** The vCPU ioctls, file_ops.ioctl of a vCPU's descriptor. */
int32_t kvm_vcpu_ioctl(struct fd_entry *e, uint32_t request, uintptr_t arg);

/* cpuid.c */

/* The speculation controls' bits: leaf 7's EDX, Intel's, and leaf
 * 0x80000008's EBX, AMD's. Intel's SPEC_CTRL names IBRS and IBPB at once. */
#define CPUID_7_SPEC_CTRL  (1u << 26)
#define CPUID_7_STIBP      (1u << 27)
#define CPUID_7_FLUSH_L1D  (1u << 28)
#define CPUID_7_SSBD       (1u << 31)
#define CPUID_X8_IBPB      (1u << 12)
#define CPUID_X8_IBRS      (1u << 14)
#define CPUID_X8_STIBP     (1u << 15)
#define CPUID_X8_SSBD      (1u << 24)
#define CPUID_X8_VIRT_SSBD (1u << 25)

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

#ifdef RACH2_HOSTTEST
/** Forget the answer `kvm_xcr0_supported` keeps, so that a host test's
 * model processor may lose XSAVE and gain it back between rows. */
void kvm_xcr0_forget(void);
#endif

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

#define MSR_IA32_SPEC_CTRL 0x048u

/** Choose how the host carries out the SSBD a guest asks for in SVM's
 * VIRT_SPEC_CTRL: in IA32_SPEC_CTRL where that register takes SSBD, else
 * in the host's own VIRT_SPEC_CTRL where CPUID offers it beneath another
 * hypervisor, else in LS_CFG on AMD's families 15h to 17h, else not at all,
 * as Linux's x86_virt_spec_ctrl (arch/x86/kernel/cpu/bugs.c:207-242) skips
 * it. From `kvm_start`, before any guest runs. */
void kvm_spec_start(void);

/** Whether SVM offers VIRT_SSBD in CPUID: where the host carries it out in
 * IA32_SPEC_CTRL or in LS_CFG, as Linux's svm_set_cpu_caps offers it where
 * the host has AMD_SSBD or LS_CFG_SSBD (arch/x86/kvm/svm/svm.c:5423-5425). */
int kvm_virt_ssbd_offered(void);

/** The IA32_SPEC_CTRL the processor holds while `v` runs: the guest's,
 * with SSBD as well where its VIRT_SPEC_CTRL asks for it and the host
 * carries that out in IA32_SPEC_CTRL. */
uint64_t kvm_spec_ctrl_guest(const struct kvm_vcpu *v);

/** Put the SSBD of `v`'s VIRT_SPEC_CTRL in force on processor `cpu` before
 * an entry with `in`, or the host's back after the exit without, where the
 * host carries it out in its own VIRT_SPEC_CTRL or in LS_CFG and the two
 * differ, as Linux's svm_vcpu_run around the entry
 * (arch/x86/kvm/svm/svm.c:4418-4424). Every access of the guest's exits, so
 * the two calls of a run see one value. Interrupts off. */
void kvm_virt_ssbd_switch(const struct kvm_vcpu *v, int cpu, int in);

/** The host's IA32_SPEC_CTRL on processor `cpu`, 0 where it has none, read
 * at the first call there: the value an exit puts back. Interrupts off. */
uint64_t kvm_spec_ctrl_host(int cpu);

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
 * matching of the vCPUs' TSCs, as Linux's x86.c keeps them. Each takes the
 * VM's `clock_lock` for the VM's fields it reads or changes, and runs with
 * or without the big lock. */

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
uint64_t kvm_clock_ns(struct kvm_vm *vm);

/** Read the kvmclock of `vm` with the host's TSC count it is read at, into
 * `*ns` and `*host_tsc`, while the master clock runs, as the guests read it
 * with PVCLOCK_TSC_STABLE_BIT: 1, or 0 with nothing read without it. For
 * the reference TSC page, which Linux publishes only from a kvmclock with
 * that bit (compute_tsc_page_parameters, arch/x86/kvm/hyperv.c:1125). */
int kvm_clock_stable_read(struct kvm_vm *vm, uint64_t *ns, uint64_t *host_tsc);

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

/** Write `v`'s kvmclock into guest memory before an entry, which the run
 * loop calls at KVM_REQ_CLOCK_UPDATE or KVM_REQ_MASTERCLOCK_UPDATE and which
 * clears both: decide the master clock again once the host's clock has
 * stopped or started since it was decided (`vclock_generation`), and write
 * the kvmclock when it was asked for, as it is for every vCPU after such a
 * decision, or, without the master clock, when the vCPU moved to another
 * processor or KVMCLOCK_SYNC_NS passed, as Linux's kvm_guest_time_update,
 * and with it the reference TSC page, kvmclock or not
 * (`kvm_hv_clock_update`), so that a request taken here leaves no page of
 * the old clock to the entry. In `v`'s thread. */
void kvm_clock_prepare(struct kvm_vcpu *v);

/** Whether the host's clock has stopped or started (`vclock_generation`)
 * since `vm`'s master clock was last decided, which the run loop must decide
 * again before an entry (`kvm_clock_prepare`). Without a lock; the entry's
 * gates test it after the vCPU is IN_GUEST and a barrier, so that a stop
 * after a vCPU's preparation cancels its entry, or ends its fast path at the
 * exit the stop's kick (`vclock_kick`) forces, as Linux's
 * KVM_REQ_MASTERCLOCK_UPDATE does. */
int kvm_clock_host_moved(const struct kvm_vm *vm);

/** The host's ns at which `v`'s kvmclock is due to be written again
 * without the master clock, KVMCLOCK_SYNC_NS after its last write, as
 * Linux's kvmclock_sync_work; 0 while the kvmclock is off or the master
 * clock on. For `timer_ns`. */
uint64_t kvm_clock_next(const struct kvm_vcpu *v);

/** `v` enters on another processor than the last: without the master
 * clock its kvmclock is written again first (KVM_REQ_CLOCK_UPDATE), as
 * Linux's kvm_arch_vcpu_load asks when the vCPU moved. With interrupts
 * off. */
void kvm_clock_moved_cpu(struct kvm_vcpu *v);

/** KVM_KVMCLOCK_CTRL: the guest's next kvmclock says it was stopped. 0, or
 * -EINVAL while the kvmclock is off. */
int32_t kvm_clock_ctrl(struct kvm_vcpu *v);

/** Write `v`'s steal time into guest memory before an entry, as Linux's
 * record_steal_time, which the run loop calls at KVM_REQ_STEAL_UPDATE, as
 * each KVM_RUN and each switch of the vCPU's thread ask, and which clears
 * it: when it was asked for, when the thread was switched out and in since
 * the last write, and while the guest's record says it was preempted, the
 * thread's run-queue wait since the last write is added to `steal`, and
 * `preempted` cleared. In `v`'s thread. */
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
 * Begin a section of `vm`'s readers, Linux's srcu_read_lock(&kvm->srcu):
 * a slot the section finds, the MSR filter or the PMU's event filter it
 * loads, and a copy through a slot's address it found, stays until
 * the section ends (`kvm_srcu_read_unlock`), since a replacement waits for
 * it before it frees the old one or returns (`kvm_srcu_synchronize`), as
 * Linux's readers hold kvm->srcu (arch/x86/kvm/x86.c:10673). The section
 * takes no lock and turns no interrupts off, and nothing waits for it to
 * begin; sections nest, and one may begin with interrupts off. The number
 * to give `kvm_srcu_read_unlock`. The section may sleep, as a copy that
 * faults does.
 */
uint32_t kvm_srcu_read_lock(struct kvm_vm *vm);

/** End the section `kvm_srcu_read_lock` began, which gave `idx`. */
void kvm_srcu_read_unlock(struct kvm_vm *vm, uint32_t idx);

/** Wait until no section of `vm`'s readers that began before the call is
 * under way, as Linux's synchronize_srcu_expedited: the caller has
 * published the replacement of what it will free. Replacements wait one
 * at a time, under `srcu_gp_lock`, so none waits for a count new readers
 * join. Sleeps, letting the big lock go; not under a spinlock. */
void kvm_srcu_synchronize(struct kvm_vm *vm);

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
 *
 * Under the big lock. A move, a change of flags and a deletion sleep: they
 * wait for the sections that may have found the slot as it was
 * (`kvm_srcu_synchronize`), letting the big lock go meanwhile, so the VM's
 * other ioctls, another KVM_SET_USER_MEMORY_REGION among them, may run in
 * the middle of the call. The table is whole each time the lock is let go,
 * and a slot or log is freed only after the wait.
 */
int32_t kvm_slots_set(struct kvm_vm *vm, const struct kvm_userspace_memory_region *r);

/*
 * A slot as its users see it: its first frame, its frames, its flags, its
 * number, the caller's address of the frame asked for, and its dirty log,
 * a bit for each page from `base_gfn` in 64-bit words, or NULL when it does
 * not log or uses a dirty ring. A change frees the bitmap, so it is used in
 * one of two ways: under `mmu_lock` held from the lookup to the last use;
 * or inside a `kvm_srcu_read_lock` section from before the lookup to the
 * last use, which the change waits for, with `mmu_lock` held at each read
 * or write of the log, as `kvm_dirty_mark` does.
 */
struct kvm_slot_view {
    uint64_t base_gfn;
    uint64_t npages;
    uint32_t flags; /* KVM_MEM_* */
    uint32_t id;
    uint64_t hva;    /* the caller's address of the frame asked for */
    uint64_t *dirty; /* the bitmap log, or NULL with no log or a dirty ring */
};

/** The slot that holds the guest-physical frame `gfn` of `vm`, for the
 * guest-physical tables, VFIO's mappings and the transport's buffers: 1
 * with it in `*out`, or 0 for a frame no slot holds. Walks the tree in a
 * section of its own, with no lock; the copy may be out of date
 * once it returns, as a reader of Linux's slots under kvm->srcu may see the
 * slots of before a change. A caller that copies through `out->hva` or
 * marks `out->dirty` after the call holds a `kvm_srcu_read_lock` section
 * from before the call to its last use, so that a change of the slot waits
 * for it before it frees the log or returns to a caller that unmaps the
 * memory. */
int kvm_slot_find(struct kvm_vm *vm, uint64_t gfn, struct kvm_slot_view *out);

/** `kvm_slot_find` for a caller inside a `kvm_srcu_read_lock` section,
 * which may have interrupts off; it walks again while a change is under
 * way or came during the walk. */
int kvm_slot_find_read(struct kvm_vm *vm, uint64_t gfn, struct kvm_slot_view *out);

/** `kvm_slot_find` for a caller that holds `mmu_lock`. */
int kvm_slot_find_locked(struct kvm_vm *vm, uint64_t gfn, struct kvm_slot_view *out);

/** The slot numbered `id` of `vm`, with `hva` its first page's address: 1,
 * or 0 when there is none. Under `mmu_lock`, which the copy's log needs. */
int kvm_slot_get(struct kvm_vm *vm, uint32_t id, struct kvm_slot_view *out);

/** Take `vm`'s `mmu_lock` with interrupts off: the flags to restore. */
uint32_t kvm_mmu_lock(struct kvm_vm *vm);

/** Give back what `kvm_mmu_lock` took. */
void kvm_mmu_unlock(struct kvm_vm *vm, uint32_t flags);

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
 * It takes the address space's regions' lock to read and `mmu_lock`
 * itself, and may be called with or without the big lock: the backends'
 * exits call it without, KVM_PRE_FAULT_MEMORY (`kvm_tdp_pre_fault`) under
 * it. May sleep. 0; 1 for a frame no slot holds or a write to a
 * read-only slot, which is MMIO; -EFAULT when the caller's page cannot be
 * had; -ENOMEM.
 */
int kvm_tdp_fault(struct kvm_vm *vm, uint64_t gfn, int write);

/**
 * Write the `n` bytes at `buf` to `vm`'s guest-physical `gpa`, within one
 * page, without a fault or a sleep, as Linux's copy_to_user_nofault through
 * a vCPU's cache of the page, checked by the slots' generation: only while
 * `mmu_seq` is still `seq`, where the current thread is in the VM's
 * address space and its page is present, writable and already stored into,
 * in a slot that is not read-only. The frame is marked in the dirty log.
 * 0, or 1 with nothing written. Takes `mmu_lock`, so interrupts are off
 * from the read of the caller's entry to the write and no frame the
 * caller's tables name is freed before this processor drops its
 * translations (`smp_tlb_shootdown`); takes no regions' lock: the caller
 * found the page in no region of memfd_secret when `mmu_seq` was `seq`,
 * and any change of a region there has moved it since. For a context that
 * may not sleep, such as a thread's `sched_out`.
 */
int kvm_tdp_write_nofault(struct kvm_vm *vm, uint64_t gpa, const void *buf, uint32_t n,
                          uint32_t seq);

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
 * freed. Takes `mmu_lock` and moves `mmu_seq`. Does not sleep. */
void kvm_tdp_zap(struct kvm_vm *vm, uint64_t gfn, uint64_t npages);

/**
 * `kvm_tdp_zap` for a caller that holds `mmu_lock`, taken with the flags
 * `*flags`. A zap that visits more than a batch of entries, TDP_ZAP_BATCH
 * of them at every level with an absent subtree passed as one, goes a
 * batch at a time, each flushed and its tables freed before `mmu_lock` is
 * let go and taken again, `*flags` with it, and `mmu_seq` moved again, as
 * Linux's tdp_mmu_zap_leafs yields (arch/x86/kvm/mmu/tdp_mmu.c:705-735,
 * 848-861): the processor takes its interrupts and the other vCPUs their
 * faults between batches, and a fault that spans a batch's end maps
 * nothing. Neither sleeps nor gives way, so a change notice may call it.
 * The caller makes its change to the slots before the zap, so a fault
 * between batches maps by the new rules.
 */
void kvm_tdp_zap_locked(struct kvm_vm *vm, uint64_t gfn, uint64_t npages, uint32_t *flags);

/** Count a device that does not snoop the processor's caches assigned to
 * `vm` (`assigned` 1) or taken from it (0). While one is assigned, RAM is
 * mapped write-back with the guest's PAT in charge, not ignored, as Linux's
 * kvm_arch_register_noncoherent_dma makes it; the first and the last drop
 * every leaf. Under the big lock; takes `mmu_lock`. */
void kvm_tdp_noncoherent(struct kvm_vm *vm, int assigned);

/** Take the write right from the 4 KiB leaves of the frames `gfn` + i for
 * each bit i of `mask`, for the dirty log, without a flush: whether any
 * leaf changed. Under `mmu_lock`; does not sleep. */
int kvm_tdp_protect(struct kvm_vm *vm, uint64_t gfn, uint64_t mask);

/** Ask every vCPU of `vm` to drop its guest-physical translations before
 * its next entry, and wait until none is in the guest, so that no
 * processor uses a translation the tables no longer hold: a vCPU that
 * enters after the kick finds the request once it is IN_GUEST, before the
 * entry. Under `mmu_lock`; does not sleep. */
void kvm_tdp_flush(struct kvm_vm *vm);

/* dirty.c: dirty logging. */

/** KVM_GET_DIRTY_LOG, KVM_CLEAR_DIRTY_LOG or KVM_RESET_DIRTY_RINGS.
 * A bitmap ioctl is -ENXIO in ring mode. RESET returns the entry count,
 * or -EINVAL without a ring. The VM ioctl holds the big lock. */
int32_t kvm_dirty_ioctl(struct kvm_vm *vm, uint32_t request, uintptr_t arg);

/** KVM_ENABLE_CAP of KVM_CAP_MANUAL_DIRTY_LOG_PROTECT2 on `vm`: 0, or
 * -EINVAL for an option Linux does not take. */
int32_t kvm_dirty_enable_manual(struct kvm_vm *vm, const struct kvm_enable_cap *cap);

/** Enable a dirty ring once, before vCPUs exist: 0, -EINVAL for its
 * flags, size or timing, or -E2BIG above Linux's bound. Under the big lock. */
int32_t kvm_dirty_enable_ring(struct kvm_vm *vm, const struct kvm_enable_cap *cap);

/** The vCPU this thread runs in KVM_RUN for `vm`, or NULL outside a run
 * of that VM. The thread's scheduler callback identifies the run. */
struct kvm_vcpu *kvm_running_vcpu(struct kvm_vm *vm);

/** Mark `gfn` in `s`'s log under mmu_lock, already held by the caller.
 * A ring records the running vCPU's writes; a bitmap sets its page bit.
 * 0, or -ENOSPC for a full ring; the caller must not write on failure. */
int kvm_dirty_mark_locked(struct kvm_vm *vm, const struct kvm_slot_view *s, uint64_t gfn);

/** Reserve a ring entry before a software copy to a page in `s`: 0 and
 * *entry NULL without a ring, 0 with a reserved entry, or -ENOSPC before
 * a copy that would overrun the ring. Holds mmu_lock only for the reserve;
 * the caller holds the vCPU's mutex and a `kvm_srcu_read_lock` section. */
int kvm_dirty_reserve(struct kvm_vcpu *v, const struct kvm_slot_view *s, uint64_t gfn,
                      struct kvm_dirty_gfn **entry);

/** Reserve for the running vCPU with mmu_lock already held, for a
 * no-fault software write. The same results as kvm_dirty_reserve. */
int kvm_dirty_reserve_locked(struct kvm_vm *vm, const struct kvm_slot_view *s, uint64_t gfn,
                             struct kvm_dirty_gfn **entry);

/** Publish a reserved entry after its software copy, even a failed one
 * that may have stored a prefix. Until then RESET stops at this entry. */
void kvm_dirty_publish(struct kvm_dirty_gfn *entry);

/** Whether `v`'s dirty ring is soft-full: its uncollected entries reach
 * the ring's size less KVM_DIRTY_RING_RESERVED, which covers software
 * writes while an exit completes, as Linux's kvm_dirty_ring_soft_full
 * (virt/kvm/dirty_ring.c). The entry that makes it so asks for
 * KVM_REQ_DIRTY_RING_FULL, and KVM_RUN then ends with
 * KVM_EXIT_DIRTY_RING_FULL before any entry or halted wait, until
 * KVM_RESET_DIRTY_RINGS collects them; neither backend has a hardware
 * dirty-page buffer. 0 without a ring. Reads the counters without a lock,
 * in `v`'s thread. */
int kvm_dirty_ring_full(const struct kvm_vcpu *v);

/** Mark the frame `gfn` of `v`'s VM dirty in the log of `s`, the view of
 * the slot a kernel write to guest memory found, as Linux's
 * mark_page_dirty_in_slot (virt/kvm/kvm_main.c:3624):
 * nothing when that slot did not log. The caller holds the slots' readers'
 * lock from the lookup, so the log is not yet freed, and a move of the slot
 * meanwhile keeps the log and the frame's place in it, as Linux reuses the
 * bitmap (kvm_main.c:1671-1675). Takes `mmu_lock`. 0, or -ENOSPC for a full
 * ring, before which the caller must not write. The explicit vCPU also
 * logs writes by state ioctls, outside KVM_RUN. */
int kvm_dirty_mark(struct kvm_vcpu *v, const struct kvm_slot_view *s, uint64_t gfn);

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

/**
 * Fill `map`, 256 bits, with the vectors whose EOI on `v` goes to the
 * caller's I/O APIC, as Linux's kvm_scan_ioapic_routes
 * (arch/x86/kvm/irq_comm.c:405-434): each level-triggered MSI among the
 * GSIs KVM_CAP_SPLIT_IRQCHIP reserved for its pins whose destination is
 * `v`, or whose vector `v` has in IRR or ISR. All clear without the split
 * controller. In `v`'s thread; takes the controllers' lock.
 */
void kvm_irq_scan_eoi_exits(struct kvm_vcpu *v, uint32_t map[8]);

/** Free irq.c's share of `vm`, its irqfds among them. */
void kvm_irq_free(struct kvm_vm *vm);

/** Take the controllers' lock of `vm`, which has irq.c's share, with
 * interrupts off: the flags to restore. The functions below that say they
 * run under it are called with it held; the others take it. */
uint32_t kvm_chip_lock(struct kvm_vm *vm);

/** Give back what `kvm_chip_lock` took. */
void kvm_chip_unlock(struct kvm_vm *vm, uint32_t flags);

/* Independent sources ORed at each pin, as Linux's irq_source_id bitmap. */
#define KVM_IRQ_SRC_USER     1u
#define KVM_IRQ_SRC_RESAMPLE 2u
#define KVM_IRQ_SRC_PIT      4u

/** Set a routed GSI's source level under the controllers' lock; -1 or
 * deliveries.
 * `status` asks the IOAPIC to count an RTC pulse waiting for its EOI. */
int kvm_irq_set(struct kvm_vm *vm, uint32_t gsi, uint32_t source, uint32_t level, int status);

/** Notify a controller pin's acknowledgement or mask change under the
 * controllers' lock, for PIT reinjection and irqfd resampling. */
void kvm_irq_ack(struct kvm_vm *vm, uint32_t chip, uint32_t pin);
void kvm_irq_mask(struct kvm_vm *vm, uint32_t chip, uint32_t pin, int masked);

/** Resample a pin: lower its resampling source and signal its resamplefds.
 * Returns 1 when there is a resampler, else 0; under the controllers'
 * lock. */
int kvm_irq_resample(struct kvm_vm *vm, uint32_t chip, uint32_t pin);

/** Process irqfds deferred from interrupt context, at KVM_REQ_IRQFD. */
void kvm_irq_process(struct kvm_vm *vm);

/** Whether an irqfd awaits processing; safe in a halted wait condition. */
int kvm_irq_pending(struct kvm_vm *vm);

/** Handle a PIC/PIT port or IOAPIC MMIO exit, completing reads through
 * the backend: 1 when handled, 0 for a caller-owned address. */
int kvm_irq_io(struct kvm_vcpu *v);

/** Make or free the PIC pair; create returns 0 or -ENOMEM. */
int kvm_pic_create(struct kvm_vm *vm);
void kvm_pic_free(struct kvm_vm *vm);

/** Set a PIC pin's source under the controllers' lock: -1 masked, 0
 * coalesced, 1 accepted. The pin is 0 to 15 and source is KVM_IRQ_SRC_*. */
int kvm_pic_set(struct kvm_vm *vm, uint32_t pin, uint32_t source, int level);

/** The PIC's next vector without acknowledgement, or -1 with no output.
 * Takes the controllers' lock; any context but an interrupt. */
int kvm_pic_pending(struct kvm_vm *vm);

/** Acknowledge the PIC, including the cascade; returns its vector. Takes
 * the controllers' lock. */
int kvm_pic_ack(struct kvm_vm *vm);

/** Save or restore one PIC, chip 0 or 1; restore recomputes its output.
 * Under the controllers' lock. */
void kvm_pic_state(struct kvm_vm *vm, uint32_t chip, struct kvm_pic_state *state, int set);

/** Read or write a PIC port: 1 handled, 0 not its port. Under the
 * controllers' lock. */
int kvm_pic_io(struct kvm_vm *vm, uint32_t port, uint32_t size, uint32_t *value, int write);

/** Make or free the IOAPIC; create returns 0 or -ENOMEM. */
int kvm_ioapic_create(struct kvm_vm *vm);
void kvm_ioapic_free(struct kvm_vm *vm);

/** Set an IOAPIC pin's source: -1 masked, 0 coalesced, else deliveries.
 * The pin is below KVM_IOAPIC_NUM_PINS; under the controllers' lock. */
int kvm_ioapic_set(struct kvm_vm *vm, uint32_t pin, uint32_t source, int level, int status);

/** Copy the counts of `pin`, KVM_PIN_STEPS of them by enum kvm_pin_step:
 * 0, or -1 with no IOAPIC in the kernel or no such pin. Takes the
 * controllers' lock. */
int kvm_ioapic_counts(struct kvm_vm *vm, uint32_t pin, uint32_t *counts);

/** An APIC's EOI broadcast, with the vector's TMR and directed-EOI bit.
 * Takes the controllers' lock only when a pin or the RTC's count has the
 * vector, which it first looks for without the lock, as Linux's
 * kvm_ioapic_handles_vector. */
void kvm_ioapic_eoi(struct kvm_vcpu *v, uint32_t vector, int level, int directed);

/** Restore the RTC EOI count after a vCPU's local APIC is restored. Takes
 * the controllers' lock. */
void kvm_ioapic_restore(struct kvm_vcpu *v);

/** Save or restore the IOAPIC, reinjecting restored pending lines. Under
 * the controllers' lock. */
void kvm_ioapic_state(struct kvm_vm *vm, struct kvm_ioapic_state *state, int set);

/** Read or write IOAPIC MMIO: 1 handled, 0 not its address. Under the
 * controllers' lock. */
int kvm_ioapic_io(struct kvm_vm *vm, uint64_t addr, uint32_t size, uint64_t *value, int write);

/** The PIT creation, state and reinjection ioctls: 0 or -errno. Under the
 * big lock; each takes the controllers' lock for its change. */
int32_t kvm_pit_ioctl(struct kvm_vm *vm, uint32_t request, uintptr_t arg);

/** Read or write a PIT or dummy speaker port: 1 handled, 0 not its port.
 * Under the controllers' lock. */
int kvm_pit_io(struct kvm_vm *vm, uint32_t port, uint32_t size, uint32_t *value, int write);

/** Inject due PIT ticks, retaining unacknowledged ones, at KVM_REQ_TIMER
 * (`kvm_vcpu_timers`), which a change of the PIT asks of every vCPU. Takes
 * the controllers' lock only when a tick may be due, which it first asks
 * without the lock. */
void kvm_pit_check(struct kvm_vm *vm);

/** The PIT's next host deadline in ns, or 0 while stopped. Takes the
 * controllers' lock; any context but an interrupt. */
uint64_t kvm_pit_next(struct kvm_vm *vm);

/** Acknowledge a PIT tick or reset its queue on unmask, under the
 * controllers' lock; or free the PIT. */
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

/** Whether `v`'s local APIC is a destination of a device's `irq`, with
 * no shorthand, as Linux's kvm_apic_match_dest with no source; 0 with no
 * local APIC in the kernel. */
int kvm_lapic_match_irq(struct kvm_vcpu *v, const struct kvm_lapic_irq *irq);

/** Whether a vector remains in the local APIC's IRR or ISR. */
int kvm_lapic_pending_eoi(struct kvm_vcpu *v, uint32_t vector);

/** Leave `v` the request `request` and wake its halted wait, for a
 * source the run loop takes for every vCPU, the PIT (KVM_REQ_TIMER) and the
 * irqfds left to it (KVM_REQ_IRQFD); interrupt safe. */
void kvm_lapic_wake(struct kvm_vcpu *v, uint32_t request);

/** Deliver the local APIC timer's expiry to `v` when it is due, at
 * KVM_REQ_TIMER (`kvm_vcpu_timers`). In `v`'s thread. */
void kvm_lapic_timer_check(struct kvm_vcpu *v);

/** The host's ns at which `v` next has a timer due with its local APIC in
 * the kernel, the APIC's, a Hyper-V synthetic timer or the PIT, 0 for
 * none, for `timer_ns`. In `v`'s thread; takes the controllers' lock. */
uint64_t kvm_lapic_next_ns(struct kvm_vcpu *v);

/** Wake the first vCPU that accepts PIC interrupts, under the controllers'
 * lock. */
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
 * if it is enabled. Without the big lock, in `v`'s thread
 * (`kvm_pmu_prepare`), never from the backend's entry. */
void kvm_lapic_pmi(struct kvm_vcpu *v);

/** Set the APIC base of `v` to `value`, with the checks of Linux's
 * kvm_set_apic_base: 0, or -1 for reserved bits, an invalid mode, or, from
 * the guest (`host` 0), x2APIC back to xAPIC or disabled to x2APIC. */
int kvm_lapic_set_base(struct kvm_vcpu *v, uint64_t value, int host);

/** CR8 of `v`, the TPR's high four bits. */
uint64_t kvm_lapic_cr8(struct kvm_vcpu *v);

/** Set the TPR of `v` from CR8 `cr8`. */
void kvm_lapic_set_cr8(struct kvm_vcpu *v, uint64_t cr8);

/** The physical address of `v`'s APIC register page, which a backend may
 * give the processor to keep TPR in, as VMX's virtual-APIC page; 0 with
 * no local APIC in the kernel. The page lasts as long as the vCPU. */
uintptr_t kvm_lapic_page(struct kvm_vcpu *v);

/** The highest vector in `v`'s IRR, or -1 with none, with no local APIC
 * in the kernel, or with the APIC off in IA32_APIC_BASE, where no vector
 * in IRR can be delivered. SPIV's enable bit does not hide IRR, since
 * `kvm_lapic_pending` still delivers from it: Linux's
 * kvm_lapic_find_highest_irr (lapic.c:830-838) checks no enable, and
 * kvm_apic_has_interrupt (lapic.c:3021-3031) asks only kvm_apic_present
 * (lapic.h:208-211). */
int kvm_lapic_highest_irr(struct kvm_vcpu *v);

/** Bring PPR up to date after the guest changed TPR without an exit, and
 * ask for an injection, which a lower TPR may let through: VMX's
 * TPR-below-threshold exit, as Linux's handle_tpr_below_threshold calls
 * kvm_apic_update_ppr. */
void kvm_lapic_update_ppr(struct kvm_vcpu *v);

/**
 * Ready `v` for an entry with the local APIC in the kernel, which the run
 * loop calls before an entry with a request or with the vCPU not running:
 * take the irqfds left to it (KVM_REQ_IRQFD), the NMIs (KVM_REQ_NMI), an
 * INIT or a SIPI (KVM_REQ_INIT_SIPI), the I/O APIC's EOI exits, and wait
 * while the vCPU is halted or waits for a SIPI until an event wakes it,
 * looking at its timers after each wake (`kvm_vcpu_timers`); a halted vCPU
 * an event wakes asks for its injection (KVM_REQ_EVENT). 0 to enter; 1 to
 * go to the caller with the run page's exit set, KVM_EXIT_IOAPIC_EOI, or
 * KVM_EXIT_DIRTY_RING_FULL in place of a wait while the dirty ring is
 * full, the vCPU left halted; -EINTR for a signal or a request to leave,
 * with KVM_EXIT_INTR. May sleep.
 */
int32_t kvm_lapic_prepare(struct kvm_vcpu *v);

/**
 * Deliver the local APIC timer's expiry to `v` when it is due, as
 * `kvm_lapic_prepare` does before each entry, for the backend's `fast`,
 * as Linux's kvm_lapic_expired_hv_timer and start_hv_timer deliver it from
 * the fast path (arch/x86/kvm/lapic.c:2215-2232, 2167-2178) and, with
 * APICv, which Rach2 does not use, inject it there too (1928-1932): 1 when
 * its vector went into IRR, which the caller then injects itself with the
 * backend's `inject`, with no KVM_REQ_EVENT set, so that the requests stay
 * those of other threads, which the run loop answers; else 0, an expiry in
 * a delivery mode other than fixed going the way of every other delivery.
 * With interrupts off.
 */
int kvm_lapic_timer_expire(struct kvm_vcpu *v);

/**
 * The host's TSC cycles `v`'s guest may run before its local APIC's timer
 * is due, at most `max`, 0 when it is due now: the bound of the next run,
 * as Linux arms the VMX preemption timer from the APIC timer's deadline
 * (`vmx_set_hv_timer` and `vmx_update_hv_timer`, arch/x86/kvm/vmx/vmx.c:
 * 8123-8160 and 7199-7222), so that a guest spinning on the timer takes
 * it at its expiry, not at the host's next tick. `max` for no local APIC
 * in the kernel or no timer armed. Reads the clock only while one is.
 */
uint64_t kvm_lapic_run_bound(struct kvm_vcpu *v, uint64_t max);

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
 * says otherwise; those KVM_RUN calls, `kvm_hv_prepare`, `kvm_hv_stimers`,
 * `kvm_hv_msr`, `kvm_hv_hypercall` and `kvm_hv_complete`, take it
 * themselves (`kvm_bkl_enter`) when there is work. */

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
 * that its hypercalls are Hyper-V's. Without the big lock, in `v`'s
 * thread, from the backends' hypercall exits. */
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

/** Ready `v` for an entry, which the run loop calls at KVM_REQ_HV_CRASH,
 * KVM_REQ_HV_RESET or KVM_REQ_HV_EXIT: write the reference TSC page once
 * it changed, and end KVM_RUN for a crash, a reset or a SynIC or debugger
 * MSR the guest wrote. 0 to enter; 1 to go to the caller with the run
 * page's exit set. */
int32_t kvm_hv_prepare(struct kvm_vcpu *v);

/** Write the reference TSC page of `v`'s VM if it waits to be written, as
 * Linux's kvm_guest_time_update does with kvm_hv_setup_tsc_page
 * (arch/x86/kvm/x86.c:2893), for `kvm_clock_prepare` once it takes
 * KVM_REQ_CLOCK_UPDATE. In `v`'s thread; takes the big lock only for the
 * work. */
void kvm_hv_clock_update(struct kvm_vcpu *v);

/** Fire `v`'s synthetic timers that are due, delivering their messages or
 * vectors, at KVM_REQ_TIMER (`kvm_vcpu_timers`), in `v`'s thread with the
 * APIC in the kernel. A timer armed or marked lowers `timer_ns`. */
void kvm_hv_stimers(struct kvm_vcpu *v);

/** The host's ns at which `v`'s next synthetic timer is due, 1 when one
 * waits to be looked at now, or 0 for none. Without the big lock, in
 * `v`'s thread. */
uint64_t kvm_hv_stimer_next(const struct kvm_vcpu *v);

/** Whether `vector` of `v` is a SINT's with auto-EOI, which the APIC's
 * acknowledgement takes without putting it in service. Without the big
 * lock, in `v`'s thread. */
int kvm_hv_synic_auto_eoi(struct kvm_vcpu *v, uint32_t vector);

/** An EOI of `vector` of `v`: a timer's message waiting for its SINT's
 * slot is tried again. Without the big lock, in `v`'s thread, whose own
 * `stimer_pending` it changes. */
void kvm_hv_synic_eoi(struct kvm_vcpu *v, uint32_t vector);

/** Raise SINT `sint` of the vCPU of `vm` whose VP index is `vpidx`, for a
 * KVM_IRQ_ROUTING_HV_SINT route: the APICs that took it, or -1 for no
 * such vCPU, no active SynIC or a masked SINT. Safe without the big lock
 * and with interrupts off. */
int kvm_hv_synic_set_irq(struct kvm_vm *vm, uint32_t vpidx, uint32_t sint);

/** Whether `v`'s guest is to see no invariant TSC, CPUID 0x80000007 EDX
 * bit 8: Hyper-V's invariant TSC control is offered and not yet set.
 * Without the big lock, in `v`'s thread, from the CPUID exit. */
int kvm_hv_invtsc_suppressed(const struct kvm_vcpu *v);

/** The guest's clock or TSC of `vm` moved: the reference TSC page is
 * written again before the next entry. pvclock.c, which runs without the
 * big lock, sets `hv_clock_moved` instead before it asks for
 * KVM_REQ_CLOCK_UPDATE, which `kvm_hv_prepare` and `kvm_hv_clock_update`
 * take. */
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
 * across its pages; `*pa` receives the first frame's address. The frames
 * come from `kalloc_contig`, as Linux's svm_vcpu_alloc_msrpm takes an
 * order-1 block from alloc_pages. NULL without memory. Freed with
 * `kfree_pages`.
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
 * The map is taken back, with a call of `revoke(owner)`, which must not
 * sleep and stops every new use of the frames, when the caller's memory
 * behind any of them changes or ends, when a slot over them moves or goes,
 * and when the VM ends; `kvm_gpamap_frame` then answers 0, and the frames
 * stay held until `kvm_gpamap_free`, so that a use begun before the revoke
 * never reaches a frame reused since. Under the big lock. The slots, the
 * walk of the maker's regions and tables and the registration on its
 * change notice run under the maker's regions' lock held to read, with
 * nothing between that sleeps, so that no change of the slots or of the
 * regions comes between them unheard; a lock that another thread holds to
 * write is waited for with the VM held and the big lock let go, and the
 * answer is then -EAGAIN, for the caller to look the VM up again and call
 * again. 0 with the map in `*out`; -EINVAL for a run that is empty, not
 * page-aligned or past the guest-physical width, or for too many pages;
 * -EFAULT for a page in no slot, in a read-only one, not present, not
 * ordinary RAM or in a memfd_secret region, which cannot be shared, and
 * for every page once the maker's address space has ended, which leaves no
 * range on its notice; -ENOMEM; -EAGAIN.
 */
int kvm_gpamap_new(struct kvm_vm *vm, const struct kvm_gpa_run *runs, uint32_t n,
                   void (*revoke)(void *owner), void *owner, struct kvm_gpamap **out);

/** The frame of page `i` of `m`, or 0 past its end or once it is taken
 * back. */
uintptr_t kvm_gpamap_frame(const struct kvm_gpamap *m, uint64_t i);

/** Let go of `m`'s frames without its `revoke`, and free it: its owner's
 * call, once nothing uses the frames, whether or not the map was taken
 * back. NULL is ignored. Under the big lock. */
void kvm_gpamap_free(struct kvm_gpamap *m);

/** Take back every map of `vm` with a page in the `npages` frames from
 * `gfn`, for a slot over them that moves or goes. Under the big lock. */
void kvm_gpamap_zap(struct kvm_vm *vm, uint64_t gfn, uint64_t npages);

/** Take back every map of `vm`, at the VM's end. */
void kvm_gpamap_vm_end(struct kvm_vm *vm);

#endif /* _KVM_H_ */
