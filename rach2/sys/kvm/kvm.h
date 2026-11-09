/*
 * kvm.h: the kernel's side of /dev/kvm, the VM and vCPU records, the
 * operations a processor's backend (SVM or VMX) gives, the slot lookup, and
 * the calls each file of rach2/sys/kvm/ offers the others. Readme.md has
 * the design and the lock order.
 */
#ifndef _KVM_H_
#define _KVM_H_

#include "../lib/kint.h"
#include "kvm_abi.h"
#include "../io/anon.h"
#include "../proc/wait_queue.h"

struct fd_entry;
struct kvm_vm;
struct kvm_vcpu;

/* The most vCPUs a VM has, and the bound of their ids, which
 * KVM_CAP_MAX_VCPUS and KVM_CAP_MAX_VCPU_ID report: one xAPIC id each. */
#define KVM_MAX_VCPUS    64
#define KVM_MAX_VCPU_IDS 256

/* The memory slots a VM has, KVM_CAP_NR_MEMSLOTS. */
#define KVM_MEM_SLOTS 256

/* The CPUID entries KVM_SET_CPUID2 takes and KVM_GET_SUPPORTED_CPUID
 * gives at most, Linux's KVM_MAX_CPUID_ENTRIES. */
#define KVM_MAX_CPUID_ENTRIES 256

/* The MSRs one KVM_GET_MSRS or KVM_SET_MSRS moves at most, Linux's
 * KVM_MAX_MSR_ENTRIES. */
#define KVM_MAX_MSR_ENTRIES 256

/* The bytes of a vCPU's mapping: the run page and the page of port I/O
 * data (KVM_GET_VCPU_MMAP_SIZE). */
#define KVM_RUN_PAGES 2u

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
 * port I/O and MMIO go to the caller unless the in-kernel bus takes them
 * (iobus.c), HLT goes to the caller unless the in-kernel local APIC waits
 * for an interrupt (vlapic.c), and the others go to the caller as their
 * KVM_EXIT_* reasons.
 */
enum kvm_exit_kind {
    KVM_X_AGAIN = 0,   /* handled in the kernel; enter again */
    KVM_X_IO,          /* port I/O */
    KVM_X_MMIO,        /* an access to guest-physical memory with no slot */
    KVM_X_HLT,         /* the guest halted with interrupts enabled */
    KVM_X_SHUTDOWN,    /* a triple fault */
    KVM_X_IRQ_WINDOW,  /* the window the caller asked for is open */
    KVM_X_INTERNAL,    /* an exit the backend cannot handle */
    KVM_X_FAIL_ENTRY,  /* the processor refused the entry */
    KVM_X_DEBUG,       /* a debug exception under KVM_SET_GUEST_DEBUG */
    KVM_X_SYSTEM_EVENT /* a reset or shutdown the guest asked for */
};

/*
 * One exit, as the backend reports it. For KVM_X_IO, `io` and, for an OUT,
 * `count` * `size` bytes in `data`, which the loop copies to the run's
 * port I/O page; for KVM_X_MMIO, `mmio`; for KVM_X_INTERNAL, `internal`
 * as Linux's kvm_run.internal; for KVM_X_FAIL_ENTRY, the processor's reason
 * in `hw_reason`; for KVM_X_DEBUG, `debug`; for KVM_X_SYSTEM_EVENT,
 * `system_event`. A port or MMIO read the caller answers comes back to the
 * backend through `complete` before the next entry.
 */
struct kvm_exit {
    enum kvm_exit_kind kind;
    struct kvm_run_io io;
    struct kvm_run_mmio mmio;
    struct kvm_run_internal internal;
    struct kvm_run_debug debug;
    struct kvm_run_system_event system_event;
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
 * ignore_guest_pat)` gives the bits of the memory type KVM_MT_* in a 4 KiB
 * leaf, with the guest's PAT ignored or in charge.
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
 * an entry it cannot make. `exit` then turns that exit into `v->exit`,
 * under the big lock with interrupts on, and may sleep: 0, or -errno to end
 * KVM_RUN with that error. `complete` gives the backend the answer to the
 * port or MMIO read of `v->exit` before the next entry. `save` writes the
 * parts `parts` of the backend's state into the vCPU's record. `inject`
 * makes the event the record's `events` holds pending for the next entry,
 * so an interrupt, an NMI or an exception reaches the guest at its next
 * instruction boundary. `check_extension` answers the capabilities whose
 * truth depends on the processor, such as KVM_CAP_X86_ROBUST_SINGLESTEP:
 * the value KVM_CHECK_EXTENSION gives, 0 for none. `tdp` is the format of
 * its guest-physical entries.
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
    int irqchip;          /* KVM_IRQCHIP_*: which interrupt controller is in the kernel */
    uint32_t boot_cpu_id; /* KVM_SET_BOOT_CPU_ID */
    void *slots;          /* slots.c's */
    void *irq;            /* irq.c's */
    void *iobus;          /* iobus.c's */
    void *backend;        /* the backend's */
};

#define KVM_IRQCHIP_NONE  0
#define KVM_IRQCHIP_SPLIT 1

/* How the vCPU stands, for a kick from another processor. */
#define KVM_MODE_OUTSIDE  0 /* in the kernel, or in the caller */
#define KVM_MODE_IN_GUEST 1 /* between the check of its requests and the exit */

/* The requests another thread leaves a vCPU, in `requests`, which the run
 * loop answers before each entry. */
#define KVM_REQ_EXIT         0x1u /* go back to the caller */
#define KVM_REQ_EVENT        0x2u /* an event to inject */
#define KVM_REQ_TRIPLE_FAULT 0x4u /* end in KVM_EXIT_SHUTDOWN */

/* The most MSRs a vCPU keeps, those of the index list, the MTRRs and the
 * machine-check banks. */
#define KVM_VCPU_MSRS 192

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
    struct kvm_run *run;        /* the run page, from `run_phys` */
    uintptr_t run_phys;         /* the physical address of the KVM_RUN_PAGES */
    uint8_t *pio;               /* the port I/O page, the second */
    volatile uint32_t mode;     /* KVM_MODE_* */
    volatile uint32_t requests; /* KVM_REQ_* */
    volatile int32_t cpu;       /* the processor it last entered on, -1 before */
    int busy;                   /* an ioctl of the vCPU is under way, KVM_RUN among them */
    int has_run;                /* KVM_RUN has entered the guest */
    wait_queue_t wq;            /* the ioctls that wait for `busy` to clear */
    int pending;                /* `exit` is a read the caller answers in the run page */
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
    uint32_t msr_count;
    uint32_t msr_index[KVM_VCPU_MSRS];
    uint64_t msr_value[KVM_VCPU_MSRS];
    int64_t tsc_offset; /* the guest's TSC less the host's */
    uint64_t mcg_cap, mcg_status, mcg_ctl;
    uint64_t mce_banks[KVM_MAX_MCE_BANKS * 4]; /* CTL, STATUS, ADDR, MISC of each */

    /* Set by the backend after each exit, for the run page. */
    int if_flag;     /* RFLAGS.IF */
    int irq_allowed; /* an interrupt can be injected at the next entry */

    struct kvm_exit exit;
    void *lapic;   /* vlapic.c's */
    void *backend; /* the backend's */
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

/** Whether the caller runs in the address space that made `vm`; Linux
 * answers EIO to an ioctl of a VM or its vCPUs from any other. */
int kvm_vm_caller_ok(struct kvm_vm *vm);

/** Take a reference to `vm`. */
void kvm_vm_get(struct kvm_vm *vm);

/** Drop a reference to `vm`, freeing it, its vCPUs, its slots and the
 * backend's share with the last one. Under the big lock. */
void kvm_vm_put(struct kvm_vm *vm);

/** The value KVM_CHECK_EXTENSION gives for `cap`, of /dev/kvm when `vm` is
 * NULL and of the VM `vm` otherwise; 0 for a capability not offered. */
int32_t kvm_check_extension(struct kvm_vm *vm, long cap);

/** The host's kvmclock in nanoseconds, the monotonic clock. */
uint64_t kvm_host_ns(void);

/* vcpu.c */

/** KVM_CREATE_VCPU: a vCPU of `vm` with the id `id` and a descriptor of
 * the caller's on it, "anon_inode:kvm-vcpu:ID", read-write and
 * close-on-exec. The descriptor; -EINVAL for an id past KVM_MAX_VCPU_IDS
 * or a VM with KVM_MAX_VCPUS vCPUs, -EEXIST for an id the VM has, -ENOMEM,
 * the backend's error, or `fd_alloc`'s. */
int32_t kvm_vcpu_create(struct kvm_vm *vm, uint32_t id);

/** Free `v`, its pages and the backend's share; when the VM is freed. */
void kvm_vcpu_free(struct kvm_vcpu *v);

/** Ask `v` to leave guest mode and see its requests: an interrupt to the
 * processor it runs on when it is in the guest there, and nothing more
 * when it is not, since the loop reads `requests` before each entry. */
void kvm_vcpu_kick(struct kvm_vcpu *v, uint32_t request);

/** The vCPU ioctls, file_ops.ioctl of a vCPU's descriptor. */
int32_t kvm_vcpu_ioctl(struct fd_entry *e, uint32_t request, uintptr_t arg);

/* cpuid.c */

/** KVM_GET_SUPPORTED_CPUID's entries: up to `max` into `out`, from the
 * host's CPUID less what KVM does not offer a guest. The count, or -E2BIG
 * when more than `max` are needed. */
int32_t kvm_cpuid_supported(struct kvm_cpuid_entry2 *out, uint32_t max);

/** The entry of `v`'s CPUID for `function` and `index`, or NULL. */
const struct kvm_cpuid_entry2 *kvm_cpuid_find(const struct kvm_vcpu *v, uint32_t function,
                                              uint32_t index);

/** The XCR0 bits the host offers a guest: those the kernel saves. */
uint64_t kvm_xcr0_supported(void);

/** The XCR0 bits `v`'s CPUID allows: leaf 0xD's within the host's, and
 * x87 always. */
uint64_t kvm_xcr0_guest(const struct kvm_vcpu *v);

/* msr.c */

/** The MSRs of KVM_GET_MSR_INDEX_LIST, `n` of them, or of the feature
 * list for `feature`. */
const uint32_t *kvm_msr_list(int feature, uint32_t *n);

/** The value of the feature MSR `index` into `*value`: 0, or -1 for an
 * MSR not on the feature list. */
int kvm_msr_feature(uint32_t index, uint64_t *value);

/** Give `v` the MSRs of a vCPU at power-on. */
void kvm_msr_reset(struct kvm_vcpu *v);

/** Read the MSR `index` of `v` as the caller of KVM_GET_MSRS: 0, or -1
 * for an MSR it does not keep. */
int kvm_msr_get(struct kvm_vcpu *v, uint32_t index, uint64_t *value);

/** Write the MSR `index` of `v` as the caller of KVM_SET_MSRS, with the
 * checks Linux makes of a write from the host: 0, or -1 for an MSR it does
 * not keep or a value it refuses. Marks KVM_PART_MSRS dirty. */
int kvm_msr_set(struct kvm_vcpu *v, uint32_t index, uint64_t value);

/* slots.c: memory slots, replaced by 81-1b. */

/** The slot table of a new VM into `vm->slots`: 0 or -ENOMEM. */
int kvm_slots_init(struct kvm_vm *vm);

/** Free the slot table, unpinning what it pinned. */
void kvm_slots_free(struct kvm_vm *vm);

/** KVM_SET_USER_MEMORY_REGION. */
int32_t kvm_slots_set(struct kvm_vm *vm, const struct kvm_userspace_memory_region *r);

/*
 * The slot that holds the guest-physical frame `gfn` of `vm`, for the
 * guest-physical tables, VFIO's mappings and the transport's buffers: 1
 * with the slot's first frame, its frames, its flags and the caller's
 * address of `gfn` in `*hva`, or 0 for a frame no slot holds. Under the
 * big lock; the answer holds until the lock is let go.
 */
struct kvm_slot_view {
    uint64_t base_gfn;
    uint64_t npages;
    uint32_t flags; /* KVM_MEM_* */
    uint32_t id;
    uint64_t hva; /* the caller's address of `gfn` */
};
int kvm_slot_find(struct kvm_vm *vm, uint64_t gfn, struct kvm_slot_view *out);

/** The capabilities of slots.c: USER_MEMORY, DESTROY_MEMORY_REGION_WORKS,
 * JOIN_MEMORY_REGIONS_WORKS, READONLY_MEM, SYNC_MMU, NR_MEMSLOTS. */
int32_t kvm_slots_check_extension(long cap);

/* tdp.c: the guest-physical tables, replaced by 81-1b. */

/** Make the tables of a new VM: 0 or -ENOMEM. */
int kvm_tdp_init(struct kvm_vm *vm);

/** Free them. */
void kvm_tdp_free(struct kvm_vm *vm);

/** Map the guest-physical frame `gfn` after a fault of the backend's,
 * with `write` for a write: 0, 1 for a frame no slot holds, which is MMIO,
 * or -errno. */
int kvm_tdp_fault(struct kvm_vm *vm, uint64_t gfn, int write);

/* dirty.c: dirty logging, replaced by 81-1b. */

/** KVM_GET_DIRTY_LOG and KVM_CLEAR_DIRTY_LOG, `request` naming which. */
int32_t kvm_dirty_ioctl(struct kvm_vm *vm, uint32_t request, uintptr_t arg);

/* irq.c: interrupt routing, irqfd and MSIs, replaced by 81-4. */

/** The VM ioctls of irq.c: KVM_CREATE_IRQCHIP, KVM_IRQ_LINE,
 * KVM_IRQ_LINE_STATUS, KVM_SET_GSI_ROUTING, KVM_IRQFD, KVM_SIGNAL_MSI. */
int32_t kvm_irq_ioctl(struct kvm_vm *vm, uint32_t request, uintptr_t arg);

/** KVM_ENABLE_CAP of KVM_CAP_SPLIT_IRQCHIP on `vm`. */
int32_t kvm_irq_enable_split(struct kvm_vm *vm, const struct kvm_enable_cap *cap);

/** The capabilities of irq.c: IRQ_ROUTING, IRQFD, IRQFD_RESAMPLE,
 * SIGNAL_MSI, SPLIT_IRQCHIP, IRQ_INJECT_STATUS. */
int32_t kvm_irq_check_extension(long cap);

/** Free irq.c's share of `vm`. */
void kvm_irq_free(struct kvm_vm *vm);

/* iobus.c: the in-kernel bus and ioeventfd, replaced by 81-4. */

/** KVM_IOEVENTFD. */
int32_t kvm_iobus_ioctl(struct kvm_vm *vm, uint32_t request, uintptr_t arg);

/** Offer the port or MMIO write of `v->exit` to the in-kernel bus: 1 when
 * an ioeventfd took it, 0 when it goes to the caller. */
int kvm_iobus_write(struct kvm_vcpu *v);

/** The capabilities of iobus.c: IOEVENTFD, IOEVENTFD_ANY_LENGTH,
 * IOEVENTFD_NO_LENGTH. */
int32_t kvm_iobus_check_extension(long cap);

/** Free iobus.c's share of `vm`. */
void kvm_iobus_free(struct kvm_vm *vm);

/* vlapic.c: the local APIC in the kernel, replaced by 81-4. */

/** KVM_GET_LAPIC and KVM_SET_LAPIC of `v`. */
int32_t kvm_lapic_ioctl(struct kvm_vcpu *v, uint32_t request, uintptr_t arg);

/** A HLT of `v`: 1 when it goes to the caller as KVM_EXIT_HLT, which it
 * does with no local APIC in the kernel; 0 when an interrupt ended the wait
 * and the vCPU enters again; -EINTR for a signal. May sleep. */
int kvm_lapic_halt(struct kvm_vcpu *v);

/** Free vlapic.c's share of `v`. */
void kvm_lapic_free(struct kvm_vcpu *v);

/* decode.c: the instruction decoder, replaced by 81-5. */

/*
 * One decoded access to guest memory: its length in bytes, the operand's
 * size, whether it writes, and for a write the value. `bytes` and `len`
 * are the instruction's.
 */
struct kvm_insn {
    uint8_t bytes[15];
    uint32_t len;  /* the instruction's length */
    uint32_t size; /* the access's bytes */
    int write;
    uint64_t value; /* a write's value */
};

/** Decode the `n` bytes at `bytes`, an instruction run in `mode` (16, 32
 * or 64) that accessed guest memory, into `out`: 0, or -1 for an
 * instruction the decoder does not know, which goes to the caller as
 * KVM_INTERNAL_ERROR_EMULATION with the bytes. */
int kvm_decode(const uint8_t *bytes, uint32_t n, int mode, struct kvm_insn *out);

#endif /* _KVM_H_ */
